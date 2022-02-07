/*
** $Id: ltable.c,v 2.118.1.4 2018/06/08 16:22:51 roberto Exp $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#define ltable_c
#define LUA_CORE

#include "lprefix.h"


/*
** Implementation of tables (aka arrays, objects, or hash tables).
** Tables keep its elements in two parts: an array part and a hash part.
** Non-negative integer keys are all candidates to be kept in the array
** part. The actual size of the array is the largest 'n' such that
** more than half the slots between 1 and n are in use.
** Hash uses a mix of chained scatter table with Brent's variation.
** A main invariant of these tables is that, if an element is not
** in its main position (i.e. the 'original' position that its hash gives
** to it), then the colliding element is in its own main position.
** Hence even when the load factor reaches 100%, performance remains good.
*/

#include <math.h>
#include <limits.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"

/*
**     Table 需要实现四种基本操作: 读, 写, 迭代和获取长度. lua 中并没有删除操
** 作, 而仅仅是把对应键位的值设置为 nil.
**     写操作被实现为查询已有键位, 若不存在则创建新建. 得到键位后, 写入操作就是
** 一次赋值. 所以, 在 table 模块中, 实际实现的基本操作为: 创建、查询、迭代和获
** 取长度.
*/

/*
** 查找算法伪代码:
** 如果输入的key是一个正整数, 并且它的值>0 && <=数组大小
**    尝试在数组部分查找
** 否则尝试在散列表部分查找
**    计算出该key的散列值, 根据此散列值访问Node数组得到散列桶所在位置
**    遍历该散列桶下的所有链表元素, 直到找到该Key为止
*/

/*
** 新增元素(散列表部分):
**
** 哈希表中数据的组织方式:
**     首先计算数据 key 所在为桶数组位置, 这个位置称为 mainposition. 相同
** mainposition 的数据以链表的形式组织.
**
** 对应的函数:
**     luaH_set, luaH_setint这2个函数, luaH_set 的实际行为并不在其函数内部对
** key 所对应的值(value)进行添加或者修改, 而是返回根据该 key 查找到的值(value)
** TValue 指针, 由外部的使用者来进行实际的替换操作. 而 luaH_setint 在内部对 key
** 所对应的 value 进行赋值.
**     当找不到对应的 key 时(即 key 在哈希表中并不存在), 2个函数最终都会调用
** luaH_newkey 函数分配一个新的 key. 设置值(value)同上.
*/

/*
**     数组部分的最大大小(MAXASIZE)为2^MAXABITS. MAXABITS是使MAXASIZE适合无符号
** int的最大整数.
*/
#define MAXABITS	cast_int(sizeof(int) * CHAR_BIT - 1)
#define MAXASIZE	(1u << MAXABITS)

/*
**     哈希部分的最大大小为2^MAXHBITS. MAXHBITS是最大的整数, 以使2^MAXHBITS适合带符
** 号的int. (请注意: 表中元素的最大数量, 2^MAXABITS + 2^MAXHBITS 仍适合无符号int.)
*/
#define MAXHBITS	(MAXABITS - 1)


#define hashpow2(t,n)		(gnode(t, lmod((n), sizenode(t))))

#define hashstr(t,str)		hashpow2(t, (str)->hash)
#define hashboolean(t,p)	hashpow2(t, p)
#define hashint(t,i)		hashpow2(t, i)


/*
** 对于某些类型, 最好避免使用2的幂的模数, 因为它们倾向于具有2个因数.
*/
#define hashmod(t,n)	(gnode(t, ((n) % ((sizenode(t)-1)|1))))

#define hashpointer(t,p)	hashmod(t, point2uint(p))

/* 空表 */
#define dummynode		(&dummynode_)

/* 空表, 全局只读 */
static const Node dummynode_ = {
    {NILCONSTANT},  /* 值 */
    {{NILCONSTANT, 0}}  /* 键 */
};


/*
** 计算浮点数的哈希值, 公式: n = frexp(n, &i); return (n * INT_MAX)+i
** frexp(x, i): 把一个浮点数分解为尾数和指数, i为指数, 返回尾数.
**
** 但有一些数字上的细微之处.
** 在二补数(二进制补码?)表示中, INT_MAX没有与浮点数完全相同的表示, 但INT_MIN有; 因为
** 'frexp'的绝对值小于1(除非'n'为inf/NaN), 所以乘积'frexp * -INT_MIN'的绝对值小于或
** 等于INT_MAX. 接下来, 使用'unsigned int'可以避免在添加'i'时溢出. 使用"~u"(而不是
** "-u")可以避免INT_MIN出现问题.
*/
#if !defined(l_hashfloat)
static int l_hashfloat(lua_Number n)
{
    int i;
    lua_Integer ni;
    n = l_mathop(frexp)(n, &i) * -cast_num(INT_MIN);
    if (!lua_numbertointeger(n, &ni))
    {   /* is 'n' inf/-inf/NaN? */
        lua_assert(luai_numisnan(n) || l_mathop(fabs)(n) == cast_num(HUGE_VAL));
        return 0;
    }
    else
    {   /* normal case */
        unsigned int u = cast(unsigned int, i) + cast(unsigned int, ni);
        return cast_int(u <= cast(unsigned int, INT_MAX) ? u : ~u);
    }
}
#endif

/*
** 返回表中元素的"mainposition"位置(即其哈希值的索引)
*/
static Node* mainposition(const Table* t, const TValue* key)
{
    switch (ttype(key))
    {
    case LUA_TNUMINT:
        return hashint(t, ivalue(key));
    case LUA_TNUMFLT:
        return hashmod(t, l_hashfloat(fltvalue(key)));
    case LUA_TSHRSTR:
        return hashstr(t, tsvalue(key));
    case LUA_TLNGSTR:
        return hashpow2(t, luaS_hashlongstr(tsvalue(key)));
    case LUA_TBOOLEAN:
        return hashboolean(t, bvalue(key));
    case LUA_TLIGHTUSERDATA:
        return hashpointer(t, pvalue(key));
    case LUA_TLCF:
        return hashpointer(t, fvalue(key));
    default:
        lua_assert(!ttisdeadkey(key));
        return hashpointer(t, gcvalue(key));
    }
}


/*
** 如果'key'是存在于表的数组部分中的适当键, 则返回'key'的索引, 否则返回0.
** 即判断 key 是否是int, 并且key > 0 && key <= MAXASIZE
*/
static unsigned int arrayindex(const TValue* key)
{
    if (ttisinteger(key))
    {
        lua_Integer k = ivalue(key);
        if (0 < k && (lua_Unsigned)k <= MAXASIZE)
        {
            return cast(unsigned int, k);  /* 'key'是适当的数组索引 */
        }
    }
    return 0;  /* 'key'不满足某些条件 */
}

/*
** 返回表遍历的"key"的索引. 首先是数组部分中的所有元素, 然后是哈希部分中的元素. 遍历的
** 开始用0表示.
*/
static unsigned int findindex(lua_State* L, Table* t, StkId key)
{
    unsigned int i;
    if (ttisnil(key))
    {
        return 0;  /* first iteration */
    }
    i = arrayindex(key);
    if (i != 0 && i <= t->sizearray)  /* 'key'是否在数组部分? */
    {
        return i;  /* that's the index */
    }
    else
    {
        int nx;
        Node* n = mainposition(t, key);
        for (;;)
        {   /* check whether 'key' is somewhere in the chain */
            /* key may be dead already, but it is ok to use it in 'next' */
            if (luaV_rawequalobj(gkey(n), key) ||
                (ttisdeadkey(gkey(n)) && iscollectable(key) &&
                    deadvalue(gkey(n)) == gcvalue(key)))
            {
                i = cast_int(n - gnode(t, 0));  /* key index in hash table */
                /* hash elements are numbered after array ones */
                return (i + 1) + t->sizearray;
            }
            nx = gnext(n);
            if (nx == 0)
            {
                luaG_runerror(L, "invalid key to 'next'");  /* key not found */
            }
            else
            {
                n += nx;
            }
        }
    }
}

/*
** 表的迭代, 传入上一个键, 返回下一个键值对
** 迭代时, 不可进行insert操作, 但可以修改某键对应的值
*/
int luaH_next(lua_State* L, Table* t, StkId key)
{
    unsigned int i = findindex(L, t, key);  /* find original element */
    for (; i < t->sizearray; i++)
    {   /* 数组部分 */
        if (!ttisnil(&t->array[i]))
        {   /* a non-nil value? */
            setivalue(key, i + 1);
            setobj2s(L, key + 1, &t->array[i]);
            return 1;
        }
    }
    for (i -= t->sizearray; cast_int(i) < sizenode(t); i++)
    {   /* 哈希部分 */
        if (!ttisnil(gval(gnode(t, i))))
        {   /* a non-nil value? */
            setobj2s(L, key, gkey(gnode(t, i)));
            setobj2s(L, key + 1, gval(gnode(t, i)));
            return 1;
        }
    }
    return 0;  /* no more elements */
}


/*
** {=============================================================
** Rehash
** ==============================================================
*/

/*
**     计算表't'的数组部分的最佳大小. "nums"是"计数数组", 其中"nums[i]"是表中
** 2^(i-1)+1和2^i之间的整数个数. "pna"以表中整数键的总数输入, 而留下将要到达数组部分
** 的键数; 返回最佳大小.
**     数组部分的利用率要达到 50%.
*/
static unsigned int computesizes(unsigned int nums[], unsigned int* pna)
{
    int i;
    unsigned int twotoi;  /* 2^i (最佳尺寸的候选) */
    unsigned int a = 0;  /* 小于2^i的元素个数 */
    unsigned int na = 0;  /* 要转到数组部分的元素数 */
    unsigned int optimal = 0;  /* 数组部分的最佳大小 */
    /* 循环, 而键可以占总大小的一半以上 */
    for (i = 0, twotoi = 1;
        twotoi > 0 && *pna > twotoi / 2;
        i++, twotoi *= 2)
    {
        if (nums[i] > 0)
        {
            a += nums[i];
            if (a > twotoi / 2)
            {   /* 存在超过一半的元素? */
                optimal = twotoi;  /* 最佳大小(到现在) */
                na = a;  /* 所有达到"最佳"的元素都将进入数组部分 */
            }
        }
    }
    lua_assert((optimal == 0 || optimal / 2 < na) && na <= optimal);
    *pna = na;
    return optimal;
}

/*
** 判断 key 是否符合整数的要求, 并更新相应位置的nums数组
*/
static int countint(const TValue* key, unsigned int* nums)
{
    unsigned int k = arrayindex(key);
    if (k != 0)
    {   /* 'key'是合适的数组索引吗? */
        nums[luaO_ceillog2(k)]++;  /* count as such */
        return 1;
    }
    else
    {
        return 0;
    }
}

/*
**     对表't'的数组部分中的键进行计数: 用要进入相应切片的键数填充'nums[i]', 并返回非
** 空键的总数.
*/
static unsigned int numusearray(const Table* t, unsigned int* nums)
{
    int lg;
    unsigned int ttlg;  /* 2^lg */
    unsigned int ause = 0;  /* "nums"的总和 */
    unsigned int i = 1;  /* 计数遍历所有数组键 */
    /* 遍历每个切片 */
    for (lg = 0, ttlg = 1; lg <= MAXABITS; lg++, ttlg *= 2)
    {
        unsigned int lc = 0;  /* 数量 */
        unsigned int lim = ttlg;
        if (lim > t->sizearray)
        {
            lim = t->sizearray;  /* 调整上限 */
            if (i > lim)
                break;  /* 全部都访问了 */
        }
        /* 计算在范围(2^(lg - 1), 2^lg] 的元素个数 */
        for (; i <= lim; i++)
        {
            if (!ttisnil(&t->array[i - 1]))
                lc++;
        }
        nums[lg] += lc;
        ause += lc;
    }
    return ause;
}

/*
** 统计哈希表中整数键的个数, 并更新nums数组, 返回哈希表中元素个数
*/
static int numusehash(const Table* t, unsigned int* nums, unsigned int* pna)
{
    int totaluse = 0;  /* 元素总数 */
    int ause = 0;  /* 元素添加到'nums'(可以去数组部分) */
    int i = sizenode(t);
    while (i--)
    {
        Node* n = &t->node[i];
        if (!ttisnil(gval(n)))
        {
            ause += countint(gkey(n), nums);
            totaluse++;
        }
    }
    *pna += ause;
    return totaluse;
}

/*
** 设置数组部分的大小
*/
static void setarrayvector(lua_State* L, Table* t, unsigned int size)
{
    unsigned int i;
    luaM_reallocvector(L, t->array, t->sizearray, size, TValue);
    for (i = t->sizearray; i < size; i++)
        setnilvalue(&t->array[i]);
    t->sizearray = size;
}

/*
** 设置哈希表部分的大小
*/
static void setnodevector(lua_State* L, Table* t, unsigned int size)
{
    if (size == 0)
    {   /* 0表示没有哈希表 */
        t->node = cast(Node*, dummynode);  /* 赋值空表 */
        t->lsizenode = 0;
        t->lastfree = NULL;  /* 表示它正在使用dummy节点 */
    }
    else
    {
        int i;
        int lsize = luaO_ceillog2(size);
        if (lsize > MAXHBITS)
            luaG_runerror(L, "table overflow");
        size = twoto(lsize);
        t->node = luaM_newvector(L, size, Node);
        for (i = 0; i < (int)size; i++)
        {
            Node* n = gnode(t, i);
            gnext(n) = 0;
            setnilvalue(wgkey(n));
            setnilvalue(gval(n));
        }
        t->lsizenode = cast_byte(lsize);
        t->lastfree = gnode(t, size);  /* 所有结点都是可用的 */
    }
}


typedef struct
{
    Table* t;
    unsigned int nhsize;
} AuxsetnodeT;


static void auxsetnode(lua_State* L, void* ud)
{
    AuxsetnodeT* asn = cast(AuxsetnodeT*, ud);
    setnodevector(L, asn->t, asn->nhsize);
}


void luaH_resize(lua_State* L, Table* t, unsigned int nasize,
    unsigned int nhsize)
{
    unsigned int i;
    int j;
    AuxsetnodeT asn;
    unsigned int oldasize = t->sizearray;
    int oldhsize = allocsizenode(t);
    Node* nold = t->node;  /* 保存旧的哈希表... */
    if (nasize > oldasize)  /* 数组部分的大小是否需要扩充? */
        setarrayvector(L, t, nasize);
    /* 创建具有新大小的新哈希表 */
    asn.t = t; asn.nhsize = nhsize;
    if (luaD_rawrunprotected(L, auxsetnode, &asn) != LUA_OK)
    {   /* mem. error? */
        setarrayvector(L, t, oldasize);  /* 恢复数组部分的原先大小 */
        luaD_throw(L, LUA_ERRMEM);  /* 抛出内存异常 */
    }
    if (nasize < oldasize)
    {   /* 数组部分需要大小缩小? 并将无法保存在数组中的信息保存到哈希表中 */
        t->sizearray = nasize;
        /* 将即将缩小的数组部分的其他元素(大于nasize)重新插入表中 */
        for (i = nasize; i < oldasize; i++)
        {
            if (!ttisnil(&t->array[i]))
                luaH_setint(L, t, i + 1, &t->array[i]);
        }
        /* 缩小数组大小 */
        luaM_reallocvector(L, t->array, oldasize, nasize, TValue);
    }
    /* 将原先哈希表中的数据重新插入表中 */
    for (j = oldhsize - 1; j >= 0; j--)
    {
        Node* old = nold + j;
        if (!ttisnil(gval(old)))
        {
            /* doesn't need barrier/invalidate cache, as entry was
               already present in the table */
            setobjt2t(L, luaH_set(L, t, gkey(old)), gval(old));
        }
    }
    if (oldhsize > 0)  /* 没有哈希表? */
        luaM_freearray(L, nold, cast(size_t, oldhsize)); /* 释放旧的哈希表 */
}

/* 仅调整数组部分的大小 */
void luaH_resizearray(lua_State* L, Table* t, unsigned int nasize)
{
    int nsize = allocsizenode(t);
    luaH_resize(L, t, nasize, nsize);
}

/*
**     rehash 的主要工作是统计当前 table 中到底有多少有效键值对, 以及决定数组部分需要
** 开辟多少空间. 其原则是最终数组部分的利用率需要超过 50%.
**     lua 使用一个 rehash 函数中定义在栈上的 nums 数组来做这个整数键的统计工作. 这个
** 数组按2的整数幂次来分开统计各个区段间的整数键个数. 统计过程的实现见 numusearray 和
** numusehash 函数.
**     最终, computesizes 函数计算出不低于 50% 利用率下, 数组该维持多少空间. 同时, 还
** 可以得到有多少有效键将被存储在哈希表中.
**     根据这些统计数据, rehash 函数调用 luaH_resize 这个api来重新调整数组部分和哈希部
** 分的大小, 并把不能放在数组里的键值对重新塞入哈希表.
**
** 主要操作:
**     (1) 分配一个位图 nums, 将其中所有的位置置0. 这个位图的意义在于: nums数组中第i
** 个元素存放的是key在2^(i-1)和2^i之间的元素数量.
**     (2) 遍历 lua 表中的数组部分, 计算其中的元素数量, 更新对应的nums数组中的元素数量
** (numusearray函数)
**     (3) 遍历 lua 表中哈希表部分, 因为其中也可能存放了正整数, 需要根据这里的正整数数
** 量更新对应的 nums 数组元素数量(numusehash 函数).
**     (4) 此时 nums 数量已经有当前这个 table 中所有正整数的分配统计, 逐个遍历 nums 数
** 组, 获得其范围区间内所包含的整数数量大于 50% 的最大索引, 作为重新散列之后的数组大小,
** 超过这个范围的正整数, 就分配到哈希表部分中(computesizes函数).
**     (5) 根据上面计算得到的调整后的数组和哈希表大小调整表(luaH_resize函数).
**
** nums[i] = number of keys 'k' where 2^(i - 1) < k <= 2^i
** i 位置存放的是 key(正整数) 在2^(i - 1)和2^i之间的元素数量
**
**     在重新散列的过程中, 除了增大 lua 表的大小以容纳新的数据之外, 还希望能借此机会对原
** 有的数组和哈希表进行调整, 让两部分都尽可能发挥其存储的最高容纳效率.
**     标椎是: 希望在调整后, 数组在每一个2次方位置容纳的元素数量都超过该范围的50%.
*/
static void rehash(lua_State* L, Table* t, const TValue* ek)
{
    unsigned int asize;  /* 数组部分的最佳大小 */
    unsigned int na;  /* 数组部分中的键数 */
    unsigned int nums[MAXABITS + 1];
    int i;
    int totaluse;
    for (i = 0; i <= MAXABITS; i++)
    {
        /* 初始化 */
        nums[i] = 0;
    }
    na = numusearray(t, nums);  /* 计算数组部分中的键 */
    totaluse = na;  /* 所有这些键都是整数键 */
    totaluse += numusehash(t, nums, &na);  /* 计算哈希部分的整数键 */
    /* 计算额外的键(参数) */
    na += countint(ek, nums);
    totaluse++;
    /* 计算数组部分的新大小 */
    asize = computesizes(nums, &na);
    /* 将表调整为新的大小 */
    luaH_resize(L, t, asize, totaluse - na);
}



/*
** }=============================================================
*/

/*
** 创建新Table, array部分和hash部分都为空
*/
Table* luaH_new(lua_State* L)
{
    /* 创建gc对象, 并挂在allgc链表上 */
    GCObject* o = luaC_newobj(L, LUA_TTABLE, sizeof(Table));
    Table* t = gco2t(o);  /* 将gc转化为table */
    t->metatable = NULL;  /* 设置元表 */
    t->flags = cast_byte(~0);  /* 元表的一些属性信息 */
    t->array = NULL;  /* 设置数组部分 */
    t->sizearray = 0;  /* 设置数组长度 */
    setnodevector(L, t, 0);  /* 设置哈希部分 */
    return t;
}

/*
** 释放Table, 不负责将Table从gc链表上删除
** 仅在gc阶段调用
*/
void luaH_free(lua_State* L, Table* t)
{
    if (!isdummy(t))  /* 判断是否有哈希表 */
    {
        luaM_freearray(L, t->node, cast(size_t, sizenode(t)));  /* 释放哈希表 */
    }
    luaM_freearray(L, t->array, t->sizearray);  /* 释放数组 */
    luaM_free(L, t);  /* 释放Table */
}

/*
** 查找哈希链表中的空闲节点, 从后往前找
**     lastfree 的初始值为哈希表的最后一个节点, 从后一次向前寻找 key 为 nil
** 的结点, 当 lastfree 移动到哈希表的头部时, 就需要进行 rehash 操作.
*/
static Node* getfreepos(Table* t)
{
    if (!isdummy(t))
    {
        while (t->lastfree > t->node)
        {
            t->lastfree--;
            if (ttisnil(gkey(t->lastfree)))
            {
                return t->lastfree;
            }
        }
    }
    return NULL;  /* 没有找到空闲结点 */
}


/*
**     lua 的哈希表以闭散列方式实现. 每个可能的键值, 在哈希表中都有一个主要位
** 置, 称作 mainposition. 创建一个新键时, 根据哈希检查其 mainposition, 若无人
** 使用, 则可以直接设置为新键. 若有其它键占据了这个位置, 则检查占据此位置的键的
** 主位置(即它的 mainposition)是不是这里. 若两者位置冲突(即两人的哈希值相同),
** 则在哈希表中找一空闲位置, 放置新键, 利用 Node 结构中的 next 域, 以一个单向链
** 表的形式把它们链起来; 否则, 新键占据这个位置, 而老键更换到新位置并根据它的主
** 键找到属于它的链的那条单向链表中上一个结点, 重新链入.
**
**     无论是哪种冲突情况, 都需要在哈希表中找到一个空闲可用的结点. 这里是在 getfreepos
** 函数中, 递减 lastfree 来实现的. lua 也不会在设置键位的值为 nil 时而回收空间,
** 而是在预先准备好的哈希空间使用完后惰性回收. 即在 lastfree 递减到哈希空间头时,
** 做一次 rehash 操作.
*/
TValue* luaH_newkey(lua_State* L, Table* t, const TValue* key)
{
    Node* mp;
    TValue aux;
    if (ttisnil(key))  /* key 不可以为 nil */
    {
        luaG_runerror(L, "table index is nil");
    }
    else if (ttisfloat(key))
    {   /* 如果 key 为浮点(浮点数包括整形), 尝试将其转换为整形 */
        lua_Integer k;
        if (luaV_tointeger(key, &k, 0))
        {   /* 第三个参数0表示 key 必须是一个整数, 像 1.0 */
            setivalue(&aux, k);
            key = &aux;  /* key 为整数 */
        }
        else if (luai_numisnan(fltvalue(key)))  /* 判断 key 是否为 NaN */
        {
            luaG_runerror(L, "table index is NaN");
        }
    }
    mp = mainposition(t, key);  /* 获取 key 在哈希表中的主位置(mainposition) */
    if (!ttisnil(gval(mp)) || isdummy(t))
    {   /* 如果 mainposition 不为空, 则表示已经有键占据了该位置 */
        Node* othern;
        Node* f = getfreepos(t);  /* 在哈希表中获取一个空闲节点 */
        if (f == NULL)
        {   /* 若没有空闲位置, 表明哈希表需要调整 */
            rehash(L, t, key);  /* 调整 table 的大小 */
            /* whatever called 'newkey' takes care of TM cache */
            return luaH_set(L, t, key);  /* 重新调用 luaH_set */
        }
        lua_assert(!isdummy(t));
        /* 计算占据主位置的旧键的主位置(mainposition) */
        othern = mainposition(t, gkey(mp));
        if (othern != mp)
        {   /* 旧键的主位置并非是新键主位置, 鸠占鹊巢, 旧键不应该保存在这里 */
            /* 找到旧键所属链表的上一个结点 */
            while (othern + gnext(othern) != mp)
            {
                othern += gnext(othern);
            }
            /* 将旧键放置在新的空闲节点中, 并修改链表中上一节点的 next 值, 即修复链表 */
            gnext(othern) = cast_int(f - othern);  /* 修复'next', 将 f 与 othern 链接起来 */
            *f = *mp;  /* 将旧键转移到新的空闲位置 f 处. (但mp后面的节点尚未更改) */
            /* 修复旧键的 next 域 */
            if (gnext(mp) != 0)
            {
                gnext(f) += cast_int(mp - f);  /* 修复 'next' */
                gnext(mp) = 0;  /* 清除 mp 的 next, 现在新键的主位置可用了 */
            }
            setnilvalue(gval(mp));  /* 设置 mp 对应的 value 为 nil */
        }
        else
        {   /* 旧键和新键的主位置相同, 即俩键的 hash 值相同 */
            /* 将新键放入新的空闲位置, 同时将新键链接到主位置所属的链表中(头插法) */
            if (gnext(mp) != 0)
            {
                /* 将 mp 后的节点链接到 f 后 */
                gnext(f) = cast_int((mp + gnext(mp)) - f);
            }
            else
            {
                lua_assert(gnext(f) == 0);
            }
            /* 将 f 链接到 mp 之后 */
            gnext(mp) = cast_int(f - mp);
            mp = f;
        }
    }
    /* 设置 mp 的 key */
    setnodekey(L, &mp->i_key, key);
    luaC_barrierback(L, t, key);
    lua_assert(ttisnil(gval(mp)));  /* mp 的 value 必须为 nil */
    return gval(mp);  /* 返回 mp 的 value */
}


/*
** 根据整形键(key)获取对应的值(value)
*/
const TValue* luaH_getint(Table* t, lua_Integer key)
{
    /* (1 <= key && key <= t->sizearray) 是否在数组部分 */
    if (l_castS2U(key) - 1 < t->sizearray)
    {
        return &t->array[key - 1];  /* 数组部分 */
    }
    else
    {
        /* 哈希表部分 */
        Node* n = hashint(t, key);  /* 计算key的哈希值, 并获取hash表中对应位置的node链表 */
        for (;;)
        {   /* 判断是否在链表中 */
            if (ttisinteger(gkey(n)) && ivalue(gkey(n)) == key)
            {
                /* 在hash表中找到key对应的值 */
                return gval(n);
            }
            else
            {
                /* 链表中下一个结点 */
                int nx = gnext(n);
                if (nx == 0)
                {
                    break;
                }
                n += nx;
            }
        }
        return luaO_nilobject;  /* 没找到则返回nil */
    }
}


/*
** 以短字符串作为键(key)获取对应的值(value)
*/
const TValue* luaH_getshortstr(Table* t, TString* key)
{
    Node* n = hashstr(t, key);  /* 通过短字符串的 hash 值, 获取在哈希表中的位置 */
    lua_assert(key->tt == LUA_TSHRSTR);
    for (;;)
    {   /* 遍历链表判断是否存在 */
        const TValue* k = gkey(n);
        if (ttisshrstring(k) && eqshrstr(tsvalue(k), key))
        {
            return gval(n);  /* 找到, 返回值 */
        }
        else
        {
            int nx = gnext(n);
            if (nx == 0)
            {
                return luaO_nilobject;  /* 没有找到 */
            }
            n += nx;
        }
    }
}


/*
** "通用"获取版本, 从哈希表中获取信息. (以下情况不是通用的: 对于可能在数组部分中的整数
** 无效, 对于具有整数值的浮点数无效.)
*/
static const TValue* getgeneric(Table* t, const TValue* key)
{
    Node* n = mainposition(t, key);
    for (;;)
    {   /* 判断链表中是否存在key */
        if (luaV_rawequalobj(gkey(n), key))
            return gval(n);  /* 存在key */
        else
        {
            int nx = gnext(n);
            if (nx == 0)
                return luaO_nilobject;  /* 没找到, 返回nil */
            n += nx;
        }
    }
}

/* 根据字符串key获取对应的value */
const TValue* luaH_getstr(Table* t, TString* key)
{
    if (key->tt == LUA_TSHRSTR)  /* 短字符串 */
    {
        return luaH_getshortstr(t, key);
    }
    else
    {   /* 长字符串, 通过getgeneric获取 */
        TValue ko;
        setsvalue(cast(lua_State*, NULL), &ko, key);
        return getgeneric(t, &ko);
    }
}

/*
** 根据key获取对应的value
*/
const TValue* luaH_get(Table* t, const TValue* key)
{
    switch (ttype(key))
    {
    case LUA_TSHRSTR:
        return luaH_getshortstr(t, tsvalue(key));
    case LUA_TNUMINT:
        return luaH_getint(t, ivalue(key));
    case LUA_TNIL:
        return luaO_nilobject;
    case LUA_TNUMFLT:
    {
        /*
        ** 先判断是否可以转化为整数, 若可以, 则调用luaH_getint,
        ** 否则, 交给getgeneric处理
        */
        lua_Integer k;
        if (luaV_tointeger(key, &k, 0))
            return luaH_getint(t, k);
        /* else... */
    }  /* FALLTHROUGH */
    default:
        return getgeneric(t, key);
    }
}

/*
** 注意: 使用此功能时, 可能需要检查GC屏障并使TM缓存无效.
**     函数流程, 先通过 key 找到对应的 value, 如果没有则创建 key, 最终返回 key
** 对应的 value, 在外部(调用者)设置 value 的值.
*/
TValue* luaH_set(lua_State* L, Table* t, const TValue* key)
{
    const TValue* p = luaH_get(t, key);  /* 通过 key 获取 value */
    if (p != luaO_nilobject)
    {
        return cast(TValue*, p);  /* 键 key 存在, 返回对应的 value */
    }
    else
    {
        /* 键 key 不存在, 则创建 key, 也返回对应的 value */
        /* 注: 此时 key 一定不在数组部分 */
        return luaH_newkey(L, t, key);
    }
}

/*
** 设置 key(整形) 对应的 value
*/
void luaH_setint(lua_State* L, Table* t, lua_Integer key, TValue* value)
{
    const TValue* p = luaH_getint(t, key);  /* 通过 key 获取 value */
    TValue* cell;
    if (p != luaO_nilobject)
    {
        cell = cast(TValue*, p);  /* key 存在 */
    }
    else
    {
        /* key 不存在, 创建新 key */
        TValue k;
        setivalue(&k, key);
        cell = luaH_newkey(L, t, &k);
    }
    setobj2t(L, cell, value);  /* 设置 key 对应的 value 的值 */
}

/* 使用二分法, 来快速在哈希表中定位一个非空的整数键的位置 */
static lua_Unsigned unbound_search(Table* t, lua_Unsigned j)
{
    lua_Unsigned i = j;  /* i is zero or a present index */
    j++;
    /* find 'i' and 'j' such that i is present and j is not */
    while (!ttisnil(luaH_getint(t, j)))
    {
        i = j;
        if (j > l_castS2U(LUA_MAXINTEGER) / 2)
        {   /* overflow? */
            /* 该表的创建的很不好: 求助于线性搜索 */
            i = 1;
            while (!ttisnil(luaH_getint(t, i)))
                i++;
            return i - 1;
        }
        j *= 2;
    }
    /* now do a binary search between them */
    /* 二分法搜索 */
    while (j - i > 1)
    {
        lua_Unsigned m = (i + j) / 2;
        if (ttisnil(luaH_getint(t, m)))
            j = m;
        else
            i = m;
    }
    return i;
}


/*
** 获取表的长度, 当数组部分填满时才会进一步检索哈希表
** 如果数组部分最后一个元素不为nil, 并且没有哈希部分, 则返回数组的长度
** 如果数组部分最后一个元素为nil, 则采用二分法查找, 找到第一个从非nil到nil的位置, 从而返回数组部分的大小.
** hash部分调用unbound_search
*/
lua_Unsigned luaH_getn(Table* t)
{
    unsigned int j = t->sizearray;
    if (j > 0 && ttisnil(&t->array[j - 1]))
    {
        /* there is a boundary in the array part: (binary) search for it */
        /* 数组部分有一个边界:(二分法)搜索它 */
        unsigned int i = 0;
        while (j - i > 1)
        {
            unsigned int m = (i + j) / 2;
            if (ttisnil(&t->array[m - 1]))
                j = m;
            else
                i = m;
        }
        return i;
    }
    /* 否则必须在哈希部分找到边界 */
    else if (isdummy(t))  /* 没有哈希表? */
        return j;  /* 返回数组部分的长度... */
    else
        return unbound_search(t, j);  /* 在哈希表中寻找 */
}

#if defined(LUA_DEBUG)

Node* luaH_mainposition(const Table* t, const TValue* key)
{
    return mainposition(t, key);
}

int luaH_isdummy(const Table* t) { return isdummy(t); }

#endif
