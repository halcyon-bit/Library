/*
** $Id: lstring.c,v 2.56.1.1 2017/04/19 17:20:42 roberto Exp $
** String table (keeps all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#define lstring_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"


#define MEMERRMSG       "not enough memory"

/*
** 短字符串保存在global_State.strt表中, 同时也在gc链表上, 并且保证唯一
** 长字符串保存在global_state.allgc链表(gc链表), 不保证唯一
**
** 短字符串的存储方式采用拉链法和开放定址法结合(二维数组)
** 相同hash值的字符串在同一个链表上
** 不同hash值的字符串在不同的链表上
*/

/*
** 引入一个随机种子, 这样做的目的防止Hash Dos, 攻击者构造出非常多相同Hash值的不同字符
** 串, 从而降低Lua从外部压入字符串进入全局的字符串Hash表的效率.
*/

/*
** 短字符串内部化(散列过程描述)
**    首先求得传入短字符串的哈希值, 然后将该哈希值与stringtable大小取模, 从而得到该字符
** 串在stringtable中存放位置(相同哈希值的字符串链表); 接着从该字符串链表的第一个位置开始,
** 将链表中每个字符串与传入字符串比较字符串内容, 如果相等说明传入字符串已经在表中使用; 如
** 果不相等说明不是同一个字符串, 继续往后查找. 如果字符串链表中都没有查找到, 那么需要创建
** 一个新的字符串. 创建过程中, 碰到哈希值相同的字符串, 简单地串在同一个哈希位的链表上即可.
** 描述开散列的哈希过程: 传入字符串被放入字符串表的时候, 先检查一下表中有没有相同的字符串,
** 如果有则复用已有的字符串, 如果没有则创建一个新的字符串.
**
**    由于Lua的垃圾回收过程是分步完成的, 而向stringtable添加新字符串在垃圾回收的任何步骤
** 之间都可能发生, 所以这个过程中需要检查表中的字符串是否已经死掉(标记为可垃圾回收);有可能
** 在标记完字符串死掉后, 在下个步骤中又产生了相同的字符串导致这个字符串复活.
*/

/*
** Lua最多使用字符串中的~(2^LUAI_HASHLIMIT)个字节来计算其哈希值
*/
#if !defined(LUAI_HASHLIMIT)
#define LUAI_HASHLIMIT		5
#endif


/*
** 判断两个长字符串是否相等
*/
int luaS_eqlngstr(TString* a, TString* b)
{
    size_t len = a->u.lnglen;
    lua_assert(a->tt == LUA_TLNGSTR && b->tt == LUA_TLNGSTR);
    return (a == b) ||  /* 引用相同的字符串 */
        ((len == b->u.lnglen) &&  /* 长度相同并且 */
            (memcmp(getstr(a), getstr(b), len) == 0));  /* 字符串的内容相同 */
}

/*
**    对于比较长的字符串(32字节以上), 为了加快哈希过程, 计算字符串哈希值是跳跃进行的.
** 跳跃的步长(step)是由LUAI_HASHLIMIT宏控制的.
**
** 随机种子
**    Hash DoS攻击: 攻击者构造出上千万个拥有相同哈希值的不同字符串, 用来数十倍地降低
** Lua从外部压入字符串到内部字符串表的效率. 当Lua用于大量依赖字符串处理的服务(例如HTTP)
** 的处理时, 输入的字符串将不可控制, 很容易被人恶意利用.
**
**    为了防止Hash DoS攻击的发生, Lua一方面将长字符串独立出来, 大文本的输入字符串将不
** 再通过哈希内部化进入全局字符串表中; 另一方面使用一个随机种子用于字符串哈希值的计算,
** 使得攻击者无法轻易构造出拥有相同哈希值的不同字符串.
**
**    随机种子是在创建虚拟机的global_State(全局状态机)时构造并存储在global_State中的.
** 随机种子也是使用luaS_hash函数生成, 它利用内存地址随机性以及一个用户可配置的一个随机
** 量(luai_makeseed宏)同时来决定.
**
**    用户可以在luaconf.h中配置luai_makeseed来定义自己的随机方法, Lua默认是利用time函
** 数获取系统当前时间来构造随机种子. luai_makeseed的默认行为有可能给调试带来一些困扰:
** 由于字符串hash值的不同, 程序每次运行过程中的内部布局将有一些细微变化, 不过字符串池使
** 用的是开散列算法, 这个影响将非常小. 如果用户希望让嵌入Lua的程序每次运行都严格一致, 那
** 么可以自定义luai_makeseed函数来实现.
*/
unsigned int luaS_hash(const char* str, size_t l, unsigned int seed)
{
    unsigned int h = seed ^ cast(unsigned int, l);
    size_t step = (l >> LUAI_HASHLIMIT) + 1;
    for (; l >= step; l -= step)
    {
        h ^= ((h << 5) + (h >> 2) + cast_byte(str[l - 1]));
    }
    return h;
}

/* 计算长字符串的hash值并保存 */
unsigned int luaS_hashlongstr(TString* ts)
{
    /*
    ** 创建长字符串时并不计算其hash值, 当需要用时在计算
    ** 采用的是惰性求哈希值, 长字符串的hash值只计算一次, 并保存在TString的hash里
    ** extra为1表示该字符串已经计算过hash值
    */
    lua_assert(ts->tt == LUA_TLNGSTR);
    if (ts->extra == 0)
    {
        ts->hash = luaS_hash(getstr(ts), ts->u.lnglen, ts->hash);
        ts->extra = 1;
    }
    return ts->hash;
}


/*
** stringtable的扩大及字符串的重新哈希
**     当stringtable中的字符串数量(stringtable.nuse)超过预定容量(stringtable.size)
** 时, 说明stringtable太拥挤, 许多字符串可能都哈希到同一个维度中去, 这将会降低
** stringtable的遍历效率. 这个时候需要调用luaS_resize方法将stringtable的哈希链表数
** 组扩大, 重新排列所有字符串的位置.
**     改变全局字符串表(global_State.strt)的大小
** 调用: lstring.c中创建新的短字符串时, 如果没有空间时, 会进行适当的扩充(原先的2倍)
** 调用: lgc.c的checkSizes函数中, 这里会进行检查, 如果此时散列表的大小太大, 比如是
** 实际存放的字符串数量的4倍, 那么会将散列表数组减少为原来的一半.
*/
void luaS_resize(lua_State* L, int newsize)
{
    int i;
    stringtable* tb = &G(L)->strt;  /* 取得全局stringtable */
    if (newsize > tb->size)
    {   /* 增长hash表, 如果stringtable的新容量大于旧容量, 重新分配 */
        luaM_reallocvector(L, tb->hash, tb->size, newsize, TString*);
        for (i = tb->size; i < newsize; i++)
        {
            /* 初始化扩展的部分 */
            tb->hash[i] = NULL;
        }
    }
    /* 重新计算原有字符串在hash表中的位置, 因字符串的位置与表的大小有关, 根据新容量进行重新哈希 */
    for (i = 0; i < tb->size; i++)
    {
        TString* p = tb->hash[i];
        tb->hash[i] = NULL;
        /* 将每个哈希链表中的元素哈希到新的位置(头插法) */
        while (p)
        {
            TString* hnext = p->u.hnext;
            unsigned int h = lmod(p->hash, newsize);
            p->u.hnext = tb->hash[h];
            tb->hash[h] = p;
            p = hnext;
        }
    }
    /* 如果stringtable的新容量小于旧容量, 那么要减小表的长度 */
    if (newsize < tb->size)
    {
        /* 缩小hash表, assert()多余的部分必须为NULL */
        lua_assert(tb->hash[newsize] == NULL && tb->hash[tb->size - 1] == NULL);
        luaM_reallocvector(L, tb->hash, tb->size, newsize, TString*);
    }
    tb->size = newsize;
}


/*
** 清空字符串的临时缓存. (表中无用字符串不能为空, 所以用一个不可回收的字符串填充
*/
void luaS_clearcache(global_State* g)
{
    int i, j;
    for (i = 0; i < STRCACHE_N; i++)
    {
        for (j = 0; j < STRCACHE_M; j++)
        {
            if (iswhite(g->strcache[i][j]))  /* 判断其标记是否为白色(可回收状态)? */
            {
                g->strcache[i][j] = g->memerrmsg;  /* 用不可回收的字符串替换 */
            }
        }
    }
}


/*
** 初始化全局字符串表(hash表)和字符串临时缓存
*/
void luaS_init(lua_State* L)
{
    global_State* g = G(L);
    int i, j;
    luaS_resize(L, MINSTRTABSIZE);  /* 设置全局hash表的初始大小 */
    /* 创建内存错误消息 */
    g->memerrmsg = luaS_newliteral(L, MEMERRMSG);
    luaC_fix(L, obj2gco(g->memerrmsg));  /* 保证内存错误字符串不被回收 */
    for (i = 0; i < STRCACHE_N; i++)  /* 初始化字符串临时缓存表 */
    {
        for (j = 0; j < STRCACHE_M; j++)
        {
            g->strcache[i][j] = g->memerrmsg;
        }
    }
}

/*
** 创建字符串
** 字符串在内存中的表现是: TString(头) + 字符串 + '\0', '\0'是为了C字符串准备的
*/
static TString* createstrobj(lua_State* L, size_t l, int tag, unsigned int h)
{
    TString* ts;
    GCObject* o;
    size_t totalsize;  /* 字符串的真正长度: sizeof(UTString) + l + 1, 1是为'\0'准备的 */
    totalsize = sizelstring(l);
    o = luaC_newobj(L, tag, totalsize);  /* 创建GCObject对象, 并挂在allgc链表上 */
    ts = gco2ts(o);  /* 将gc对象转化为TString对象 */
    ts->hash = h;  /* 字符串的哈希值 */
    ts->extra = 0;  /* 0表示不是系统保留字符串(非保留字) */
    getstr(ts)[l] = '\0';  /* 字符串的结束标记(适应C) */
    return ts;
}

/* 创建长字符串, 并未将字符串内容拷贝到TString中, 由调用者操作 */
TString* luaS_createlngstrobj(lua_State* L, size_t l)
{
    TString* ts = createstrobj(L, l, LUA_TLNGSTR, G(L)->seed);
    ts->u.lnglen = l;  /* 长字符串的长度 */
    return ts;
}


void luaS_remove(lua_State* L, TString* ts)
{
    stringtable* tb = &G(L)->strt;
    TString** p = &tb->hash[lmod(ts->hash, tb->size)];
    while (*p != ts)  /* 找到需要移除的字符串 */
    {
        p = &(*p)->u.hnext;
    }
    *p = (*p)->u.hnext;  /* 移除 */
    tb->nuse--;
}


/*
** 检查短字符串是否存在并重新使用它或创建一个新字符串
*/
static TString* internshrstr(lua_State* L, const char* str, size_t l)
{
    TString* ts;
    global_State* g = G(L);
    /* 计算该字符串的hash值 */
    unsigned int h = luaS_hash(str, l, g->seed);
    /* 根据hash值获取hash链表 */
    TString** list = &g->strt.hash[lmod(h, g->strt.size)];
    lua_assert(str != NULL);
    /* 判断该短字符串是否在hash表中 */
    for (ts = *list; ts != NULL; ts = ts->u.hnext)
    {
        if (l == ts->shrlen &&
            (memcmp(str, getstr(ts), l * sizeof(char)) == 0))
        {
            /*
            ** 短字符串存在于全局链表, 判断这个字符串是否在当前GC阶段被判定为
            ** 需要回收, 如果是, 则调用changewhite函数修改它的状态, 将其改为
            ** 不需要进行回收, 从而达到复用字符串的目的.
            */
            if (isdead(g, ts))  /* 被标记回收, 但还没有进行回收操作? */
            {
                changewhite(ts);  /* 重新利用 */
            }
            return ts;
        }
    }
    /* 没找到, 则创建 */
    if (g->strt.nuse >= g->strt.size && g->strt.size <= MAX_INT / 2)
    {
        /* hash表中空间不足, 扩展为原先的两倍 */
        luaS_resize(L, g->strt.size * 2);
        list = &g->strt.hash[lmod(h, g->strt.size)];  /* 重新获取链表 */
    }
    ts = createstrobj(L, l, LUA_TSHRSTR, h);
    memcpy(getstr(ts), str, l * sizeof(char));  /* 将字符串拷贝到TString结构体后面 */
    ts->shrlen = cast_byte(l);  /* 长度 */
    ts->u.hnext = *list;  /* 链接到hash链表上 */
    *list = ts;
    g->strt.nuse++;
    return ts;
}


/*
** 创建新字符串(根据长度, 创建对应的字符串)
*/
TString* luaS_newlstr(lua_State* L, const char* str, size_t l)
{
    if (l <= LUAI_MAXSHORTLEN)  /* 短字符串(默认是40) */
    {
        return internshrstr(L, str, l);
    }
    else
    {
        TString* ts;  /* 长字符串 */
        if (l >= (MAX_SIZE - sizeof(TString)) / sizeof(char))
        {
            luaM_toobig(L);  /* 字符串过长 */
        }
        ts = luaS_createlngstrobj(L, l);
        /* 将字符串字符拷贝到TString中 */
        memcpy(getstr(ts), str, l * sizeof(char));
        return ts;
    }
}

/*
** 创建或重用以零结尾的字符串, 首先检查字符串临时缓存(使用字符串地址作为键)
** 缓存只能包含零终止字符串, 因此使用'strcmp'是安全的
*/
TString* luaS_new(lua_State* L, const char* str)
{
    unsigned int i = point2uint(str) % STRCACHE_N;
    int j;
    TString** p = G(L)->strcache[i];
    for (j = 0; j < STRCACHE_M; j++)
    {
        /* 判断字符串缓存表中是否有该字符串 */
        if (strcmp(str, getstr(p[j])) == 0)
        {
            return p[j];  /* 存在, 返回 */
        }
    }
    /* 不存在, 将最后一个丢弃, 前面的后移 */
    for (j = STRCACHE_M - 1; j > 0; j--)
    {
        p[j] = p[j - 1];
    }
    /* 将新创建的字符串放在开头 */
    p[0] = luaS_newlstr(L, str, strlen(str));
    return p[0];
}

/* 创建用户自定义数据类型, 即创建一块内存空间, 用于保存用户自定义的数据 */
Udata* luaS_newudata(lua_State* L, size_t s)
{
    Udata* u;
    GCObject* o;
    if (s > MAX_SIZE - sizeof(Udata))
    {
        luaM_toobig(L);
    }
    o = luaC_newobj(L, LUA_TUSERDATA, sizeludata(s));
    u = gco2u(o);
    u->len = s;
    u->metatable = NULL;
    setuservalue(L, u, luaO_nilobject);
    return u;
}

