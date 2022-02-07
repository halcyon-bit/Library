/*
** $Id: lgc.c,v 2.215.1.2 2017/08/31 16:15:27 roberto Exp $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#define lgc_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


/*
** Lua分为自动、手动两种GC方式.
** 手动式: luaC_step()
** 自动式: luaC_condGC(), luaC_checkGC()
**
**     手动方式就是通过调用collectgarbage([opt [, arg]])来启动GC. 而自动方式则需要满
** 足GC的条件. 大部分会引起内存增长的API中, 都调用了luaC_checkGC. 从而实现GC可以随内
** 存使用增加而自动进行. 而这些触发条件是通过g->GCdebt、g->totalbytes等参数计算得来.
** luaGC的触发条件: 当lua使用的内存达到阀值, 便会触发GC.
*/

/*
**     Upvalue对象在垃圾回收中的处理是比较特殊的.
**     对于open upvalue, 其v指向的是一个stack上的TValue, 所以open upvalue与thread的
** 关系非常紧密. 引用到open upvalue的只可能是其从属的thread, 以及lua closure. 如果没
** 有lua closure引用这个open upvalue, 就算它一定被thread引用着, 也已经没有实际的意义
** 了, 应该被回收掉. 也就是说thread对open upvalue的引用完全是一个弱引用. 所以Lua没有
** 将open upvalue当作一个独立的可回收对象, 而是将其清理工作交给从属的thread对象来完成.
** 在mark过程中, open upvalue对象只使用white和gray两个状态, 来代表是否被引用到. 通过
** 上面的引用关系可以看到, 有可能引用open upvalue的对象只可能被lua closure引用到. 所
** 以一个gray的open upvalue就代表当前有lua closure正在引用它, 而这个lua closure不一
** 定在这个thread的stack上面. 在清扫阶段, thread对象会遍历所有从属于自己的open upvalue.
** 如果不是gray, 就说明当前没有lua closure引用这个open upvalue了, 可以被销毁.
**     当退出upvalue的语法域或者thread被销毁, open upvalue会被close. 所有close upvalue
** 与thread已经没有弱引用关系, 会被转化为一个普通的可回收对象, 和其他对象一样进行独立
** 的垃圾回收.
*/

/*
**     当处于 atomic 内部时, GC的状态为GCSinsideatomic. 运行常规代码时, GC的状态永远
** 不应为此状态.
*/
#define GCSinsideatomic		(GCSpause + 1)

/*
** cost of sweeping one element (the size of a small object divided
** by some adjust for the sweep speed)
*/
#define GCSWEEPCOST	((sizeof(TString) + 4) / 4)

/* maximum number of elements to sweep in each single step */
#define GCSWEEPMAX	(cast_int((GCSTEPSIZE / GCSWEEPCOST) / 4))

/* cost of calling one finalizer */
#define GCFINALIZECOST	GCSWEEPCOST


/*
** macro to adjust 'stepmul': 'stepmul' is actually used like
** 'stepmul / STEPMULADJ' (value chosen by tests)
*/
#define STEPMULADJ		200


/*
** macro to adjust 'pause': 'pause' is actually used like
** 'pause / PAUSEADJ' (value chosen by tests)
*/
#define PAUSEADJ		100


/*
** 'makewhite' erases all color bits then sets only the current white
** bit
*/
#define maskcolors	(~(bitmask(BLACKBIT) | WHITEBITS))
#define makewhite(g,x)	\
 (x->marked = cast_byte((x->marked & maskcolors) | luaC_white(g)))

#define white2gray(x)	resetbits(x->marked, WHITEBITS)
#define black2gray(x)	resetbit(x->marked, BLACKBIT)


#define valiswhite(x)   (iscollectable(x) && iswhite(gcvalue(x)))

#define checkdeadkey(n)	lua_assert(!ttisdeadkey(gkey(n)) || ttisnil(gval(n)))


#define checkconsistency(obj)  \
  lua_longassert(!iscollectable(obj) || righttt(obj))


#define markvalue(g,o) { checkconsistency(o); \
  if (valiswhite(o)) reallymarkobject(g,gcvalue(o)); }

/* 如果 t 为白色, 则进行标记 */
#define markobject(g,t)	{ if (iswhite(t)) reallymarkobject(g, obj2gco(t)); }

/*
** 标记一个可能为NULL的对象
*/
#define markobjectN(g,t)	{ if (t) markobject(g,t); }

static void reallymarkobject(global_State* g, GCObject* o);


/*
** {======================================================
** Generic functions
** =======================================================
*/


/*
** 哈希数组中的最后一个元素
*/
#define gnodelast(h)	gnode(h, cast(size_t, sizenode(h)))


/*
** 将可回收对象 "o" 链接到 "p" 指向的链表中
*/
#define linkgclist(o,p)	((o)->gclist = (p), (p) = obj2gco(o))


/*
**     如果结点的键未标记, 则将其标记结点为无效(dead). 这样可以回收该结点, 但将其保
** 留在表中. 当Lua查找key(它可能是链的一部分)和遍历弱表(遍历期间可能从表中删除key)时,
** 需要一个死结点. 其他地方从不访问无效键, 因为其值为的 nil 足以表明该结点在逻辑上为
** 空.
*/
static void removeentry(Node* n)
{
    lua_assert(ttisnil(gval(n)));  /* Node 的值为 nil */
    if (valiswhite(gkey(n)))  /* Node 的键是否为白色 */
    {
        /* 未使用并且未标记的键, 标记其状态为无效(dead) */
        setdeadvalue(wgkey(n));
    }
}


/*
**     判断是否可以从弱表中清除键或值. 无法回收的对象永远不会从弱表中删除. 字符串表
** 现为"values", 不是引用, 因此也永远不会删除. 其他对象: 如果要被回收, 则不能保留; 对于要回收
** 的对象, 保留其键, 不保留值(值会设置为nil).
*/
static int iscleared(global_State* g, const TValue* o)
{
    if (!iscollectable(o))
        return 0;
    else if (ttisstring(o))
    {
        markobject(g, tsvalue(o));  /* 字符串不会被清除(非弱引用) */
        return 0;
    }
    else
        return iswhite(gcvalue(o));
}

/*
**     由于整个GC过程是分步执行的, 所以程序可以正常执行, 就可能会破坏当前对象之间的
** 引用关系. black对象表示已经被标记的对象, 所以应该不可能引用到一个white对象. 当程
** 序的改变使得一个black对象引用到一个white对象时, 就会造成错误. 解决这个问题的办法
** 就是设置 barrier. barrier 在程序正常运行过程中, 监控所有的引用改变. 如果一个black
** 对象需要引用一个white对象, 存在两种处理办法:
**     1. 将white对象设置成gray, 并添加到gray列表中等待扫描. 这样等于帮助整个GC的标
** 识过程向前推进了一步.
**     2. 将black对象该回成gray, 并添加到gray列表中等待扫描. 这样等于使整个GC的标识
** 过程后退了一步.
**     这种垃圾回收方式称为"Incremental Garbage Collection"(简称为"IGC"). 使用"IGC"
** 并不是没有代价的. IGC所检测出来的垃圾对象集合比实际的集合要小, 也就是说, 有些在GC
** 过程中变成垃圾的对象, 有可能在本轮GC中检测不到. 不过, 这些残余的垃圾对象一定会在
** 下一轮GC被检测出来, 不会造成泄露.
**
**     barrier 分为"向前"和"向后"两种
**
**     由于对table操作频率比较高, 因此触发到barrier条件时使用向后barrier. 把表添加到
** grayagain链表上, 等到后面重新遍历一次. 其他barrier操作涉及的情况比较简单, 操作频
** 率理论不高, 因为是向前barrier.
*/

/*
**     "向前"barrier, 若keepinvariant()返回非零, 则表示标记阶段尚未结束, 直接将对象
** v挂在灰色链表上. 如果标记阶段已经完成, 进入清除阶段, 则直接将对象o置为白色, 等待
** 下一轮GC.(GCSatomic 阶段之后白色对象必然是新一轮的白色值).
**     三种情况会调用 luaC_barrier_: 为C闭包设置upvalue, 为full userdata设置用户
** 数据, 设置元表(metatable).
**
** o 为黑色, v 为可回收对象, 并且 v 为白色
** 1. 若当前GC是需要保持不变式状态(标记或原子阶段), 则标记v对象;
** 2. 否则为清扫阶段, 则把o标记为白色, 因而说此函数是向前barrier, 直接把o跳过清扫时白
** 色切换
** 
** 调用地方:
** 1. lua_copy 若被复制到的位置是C闭包的上值
** 2. lua_setuservalue
** 3. lua_setupvalue 若是为C闭包设置上值
** 4. addk 向函数原型中添加常量时
** 5. lua_setmetatable
** 6. registerlocalvar 向函数原型注册局部变量
** 7. newupvalue 解析时函数原型添加新的上值名
** 8. addprototype 解析时函数原型添加新的内嵌函数原型
*/
void luaC_barrier_(lua_State* L, GCObject* o, GCObject* v)
{
    global_State* g = G(L);
    lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
    if (keepinvariant(g))  /* must keep invariant? */
        reallymarkobject(g, v);  /* restore invariant */
    else
    {   /* sweep phase */
        lua_assert(issweepphase(g));
        makewhite(g, o);  /* mark main obj. as white to avoid other barriers */
    }
}


/*
**     "向后"barrier, 目前该函数只用于监控table的key和value对象引用的变化. table
** 的变化是比较频繁的, 并且同一个引用可能被反复设置成不同的对象. 对table使用"向前"
** barrier, 逐个扫描每次引用变化的对象, 会造成很多不必要的消耗. 而使用"向后"的
** barrier 就等于将table分成了"未变"和"已变"两种状态. 只要table改变了一次, 就将其
** 变成gray, 等待重新扫描. 被变成gray的table在被重新扫描之前, 无论引用再发生多少次
** 变化也都无关紧要了.
**
**     引用关系变化最频繁的是thread对象. thread通过stack引用其他对象, 而stack作为运
** 行期栈, 在一直不停地被修改. 如果要监控这些引用变化, 肯定会造成执行效率严重下降.
** 所以lua并没有在所有的stack引用变化处加入barrier, 而是直接假设stack就是变化的. 所
** 以thread对象就算被扫描完成, 也不会被设置成black, 而是再次设置成gray, 等待再次扫描.
**
**     luaC_barrierback_函数比较粗爆, 直接将对象t变为灰色, 链接到grayagain链表. 这里
** 会出现两种情况:
** 1. 在 GCSatomic 之前被调用, 直接将其链接gray列表中, GCSatomic中会自行标记处理.
** 2. 在 GCSatomic 之后被调用, 产生barrier的对象t必为新一轮的白色, 而在sweeplist阶段
** 也会将对象t置为新一轮的白色, 同样不会影响对象t的生命周期.
**
** 调用地方:
** 1. lua_rawset
** 2. lua_rawseti
** 3. lua_rawsetp
** 4. luaH_newkey
** 5. luaV_finishset
** 6. 执行OP_SETLIST
** 7. luaV_fastset
*/
void luaC_barrierback_(lua_State* L, Table* t)
{
    global_State* g = G(L);
    lua_assert(isblack(t) && !isdead(g, t));
    black2gray(t);  /* make table gray (again) */
    linkgclist(t, g->grayagain);
}


/*
** uv 的值是可回收的且是闭合的
** 若是标记或原子阶段, 则标记uv的值
** 调用地方:
** 1. lua_load
** 2. lua_setupvalue: 若是为Lua闭包设置值
** 3. lua_upvaluejoin
** 4. luaF_close
** 5. OP_SETUPVAL
*/
void luaC_upvalbarrier_(lua_State* L, UpVal* uv)
{
    global_State* g = G(L);
    GCObject* o = gcvalue(uv->v);
    lua_assert(!upisopen(uv));  /* ensured by macro luaC_upvalbarrier */
    if (keepinvariant(g))
        markobject(g, o);
}

/*
**     将对象o从 allgc 链表中移除, 链接到fixedge链表上, 同时将其标记为灰色(永久), 表
** 明该对象不会被 gc 机制回收
*/
void luaC_fix(lua_State* L, GCObject* o)
{
    global_State* g = G(L);
    lua_assert(g->allgc == o);  /* 对象必须是 allgc 链表中的第一个 */
    white2gray(o);  /* 其标记永远为灰色 */
    g->allgc = o->next;  /* 将其从allgc链表中移除 */
    o->next = g->fixedgc;  /* 链接到 fixedgc 链表上 */
    g->fixedgc = o;
}


/*
** 创建一个新的可回收对象(给定类型和大小), 并将其链接到"allgc"链表
*/
GCObject* luaC_newobj(lua_State* L, int tt, size_t sz)
{
    global_State* g = G(L);
    /* 分配内存, 并更新GCdebt */
    GCObject* o = cast(GCObject*, luaM_newobject(L, novariant(tt), sz));
    o->marked = luaC_white(g);  /* 将对象标记为白色 */
    o->tt = tt;  /* 设置类型 */
    o->next = g->allgc;  /* 将其挂在 allgc 链表上 */
    g->allgc = o;
    return o;
}

/* }====================================================== */



/*
** {======================================================
** Mark functions
** =======================================================
*/


/*
**     标记一个对象. userdata, strings 都被直接标记为黑色. 因其不会引用其他
** 对象. 其他对象标记为灰色, 并连接到灰色链表中以便稍后访问.
**     UpVal 对象不再作为 GC 对象被虚拟机的 标记-清除GC 所管理. 而是单独使用
** 引用计数的方法管理.
*/
static void reallymarkobject(global_State* g, GCObject* o)
{
reentry:
    white2gray(o);  /* 白色 -> 灰色 */
    switch (o->tt)
    {
    case LUA_TSHRSTR:  /* 短字符串, 不会引用其他对象, 直接标记为黑色 */
    {
        gray2black(o);  /* 灰色 -> 黑色 */
        g->GCmemtrav += sizelstring(gco2ts(o)->shrlen);  /* 统计实际使用的内存大小 */
        break;
    }
    case LUA_TLNGSTR:  /* 长字符串, 同短字符串 */
    {
        gray2black(o);
        g->GCmemtrav += sizelstring(gco2ts(o)->u.lnglen);
        break;
    }
    case LUA_TUSERDATA:  /* 自定义用户数据 */
    {
        TValue uvalue;
        markobjectN(g, gco2u(o)->metatable);  /* 标记其元表 */
        gray2black(o);
        g->GCmemtrav += sizeudata(gco2u(o));
        getuservalue(g->mainthread, gco2u(o), &uvalue);
        if (valiswhite(&uvalue))
        {   /* 标记Udata中的user_ */
            o = gcvalue(&uvalue);
            goto reentry;
        }
        break;
    }
    /* 以下的类型都将其链接到灰色链表上 */
    case LUA_TLCL:
    {
        linkgclist(gco2lcl(o), g->gray);
        break;
    }
    case LUA_TCCL:
    {
        linkgclist(gco2ccl(o), g->gray);
        break;
    }
    case LUA_TTABLE:
    {
        linkgclist(gco2t(o), g->gray);
        break;
    }
    case LUA_TTHREAD:
    {
        linkgclist(gco2th(o), g->gray);
        break;
    }
    case LUA_TPROTO:
    {
        linkgclist(gco2p(o), g->gray);
        break;
    }
    default:
        lua_assert(0);
        break;
    }
}


/*
** 标记基本类型元表
*/
static void markmt(global_State* g)
{
    int i;
    for (i = 0; i < LUA_NUMTAGS; i++)
    {
        markobjectN(g, g->mt[i]);
    }
}


/*
** 标记 tobefnz 链表中的所有对象, 该链表上的对象需要单独回收
*/
static void markbeingfnz(global_State* g)
{
    GCObject* o;
    for (o = g->tobefnz; o != NULL; o = o->next)
    {
        markobject(g, o);
    }
}


/*
** 标记没有被标记的线程中的所有 upvalue(已被标记的线程中的 upvalue 在遍历线程时就已
** 经被标记了, 所以不需要重新标记), 将不再具有 upvalue 或者没有被标记的线程从 twups
** 链表中移除.
*/
static void remarkupvals(global_State* g)
{
    lua_State* thread;
    lua_State** p = &g->twups;
    /* 遍历具有 upvalue 的线程链表 */
    while ((thread = *p) != NULL)
    {
        lua_assert(!isblack(thread));  /* 线程永远不可能为黑色 */
        if (isgray(thread) && thread->openupval != NULL)
        {
            p = &thread->twups;  /* 保留已经标记并且有 upvalue 的线程 */
        }
        else
        {   /* 线程没有标记, 或者没有 upvalue */
            UpVal* uv;
            *p = thread->twups;  /* 将该线程从 twups 链表移除 */
            thread->twups = thread;  /* 标记该线程已经从 twups 链表中移除 */
            for (uv = thread->openupval; uv != NULL; uv = uv->u.open.next)
            {
                if (uv->u.open.touched)
                {
                    markvalue(g, uv->v);  /* 重新标记 upvalue */
                    uv->u.open.touched = 0;
                }
            }
        }
    }
}


/*
** 重置灰色列表, 从根节点进行标记, 开始GC, 当 g->gcstate 为 GCSpause 时调用
** 1. gray grayagain 链表都置为空
** 2. weak allweak ephemeron 都置为空
** 3. 标记主线程 标记注册表
** 4. 标记基本类型的元表
** 5. 标记上个循环留下来的将要被终结的对象
*/
static void restartcollection(global_State* g)
{
    /* 将用于辅助标记的各类型对象链表进行初始化清空 */
    /* gray是灰色链表, grayagain是需要原子阶段必须被重新访问的灰色节点链表 */
    g->gray = g->grayagain = NULL;
    g->weak = g->allweak = g->ephemeron = NULL;  /* 都是与弱表相关的链表 */
    markobject(g, g->mainthread);  /* 标记主线程(协程) */
    markvalue(g, &g->l_registry);  /* 标记注册表 */
    markmt(g);  /* 标记全局元表 */
    /* 标记上个GC循环留下来的(tobefnz链表)将要被终结的对象 */
    markbeingfnz(g);
}

/* }====================================================== */


/*
** {======================================================
** Traverse functions
** =======================================================
*/

/* Lua只会回收弱引用table中的对象, 数字和布尔值以及字符串是不可回收的. */
/*
** 遍历一个具有weak value的表(设置元表的 __mode="v")
**     遍历具有弱值的表并将其链接到正确的链表上. 在 GCSpropagate 阶段, 将其链接到
** grayagain 链表中, 以便在 GCSatomic 阶段进行重新访问. 在 GCSatomic 阶段, 如果表有
** 数组部分或哈希部分有值为白色, 则将其链接到 weak 链表中以进行清除.
**     仅标记哈希部分, 值不为空的key对象.
*/
static void traverseweakvalue(global_State* g, Table* h)
{
    Node* n, * limit = gnodelast(h);
    /* 如果有数组部分, 则假定它可能具有白色值(不值得遍历) */
    int hasclears = (h->sizearray > 0);
    for (n = gnode(h, 0); n < limit; n++)
    {   /* 遍历 hash 部分 */
        checkdeadkey(n);
        if (ttisnil(gval(n)))  /* 若 value 为 nil, 则设置结点为 dead */
        {
            /* 若 value 为 nil, 同时 key 为白色, 则设置结点为 dead */
            removeentry(n);
        }
        else
        {
            lua_assert(!ttisnil(gkey(n)));
            markvalue(g, gkey(n));  /* 标记 key */
            if (!hasclears && iscleared(g, gval(n)))  /* 判断 value 是否为白色 */
            {
                hasclears = 1;  /* 表需要清理 */
            }
        }
    }
    if (g->gcstate == GCSpropagate)
    {
        /* 链接到 grayagain 链表, 必须在 GCSatomic 阶段重新访问 */
        linkgclist(h, g->grayagain);
    }
    else if (hasclears)
    {
        /* 链接到 weak 链表, 表需要清除 */
        linkgclist(h, g->weak);
    }
}


/*
** 遍历一个具有weak key的表(设置元表的__mode="k")
**     遍历一个具有 weak key 的表并将其链接到适当的链表上. 如果在此遍历期间标记了任何
** 对象(这意味着必须继续收敛), 则返回true. 在 GCSpropagate 阶段, 将表保留在
** grayagain 列表中, 以便在 GCSatomic 阶段再次访问. 在 GCSatomic 阶段, 如果表有任何
** 白色->白色结点, 则必须在短暂收敛期间重新访问它(因为该键可能变为黑色). 否则, 如果有
** 任何白键, 则必须清除表(在 GCSatomic 阶段).
**     仅标记结点值对象
*/
static int traverseephemeron(global_State* g, Table* h)
{
    int marked = 0;  /* 如果在此阶段有 value 被标记, 则为 true */
    int hasclears = 0;  /* 如果表有白色键, 则为 true */
    int hasww = 0;  /* 如果表有白色键-白色值, 则为 true */
    Node* n, * limit = gnodelast(h);
    unsigned int i;
    /* 遍历数组部分 */
    for (i = 0; i < h->sizearray; i++)
    {
        if (valiswhite(&h->array[i]))
        {   /* 值为白色, 标记 */
            marked = 1;
            reallymarkobject(g, gcvalue(&h->array[i]));
        }
    }
    /* 遍历 hash 部分 */
    for (n = gnode(h, 0); n < limit; n++)
    {
        checkdeadkey(n);
        if (ttisnil(gval(n)))
        {
            /* 如果值为nil, 并且键为白色, 则设置结点状态为dead. */
            removeentry(n);
        }
        else if (iscleared(g, gkey(n)))
        {   /* key 没有被标记并且不是字符串 */
            hasclears = 1;  /* key 为弱引用, 并且无其他地方引用该值, 需要清除 */
            if (valiswhite(gval(n)))  /* value 也没有被标记? */
            {
                hasww = 1;  /* 可能是无效的键值 white-white entry */
            }
        }
        else if (valiswhite(gval(n)))
        {   /* value 没有被标记 */
            marked = 1;
            reallymarkobject(g, gcvalue(gval(n)));  /* 标记 value */
        }
    }
    /* 将表连接到正确的链表上 */
    if (g->gcstate == GCSpropagate) /* GCSpropagate 阶段 */
    {
        /* 链接到 grayagain 链表, 必须在 atomic 阶段重新遍历 */
        linkgclist(h, g->grayagain);
    }
    else if (hasww)  /* 表中含有白色-白色结点, GCSatomic 阶段 */
    {
        /* 链接到 ephemeron 链表, 必须重新访问一遍, 有可能变成黑色 */
        linkgclist(h, g->ephemeron);
    }
    else if (hasclears)  /* 表中仅含有白色键, GCSatomic 阶段 */
    {
        /* 链接到 allweak 链表, 可能需要清除 */
        linkgclist(h, g->allweak);
    }
    return marked;
}

/*
** 遍历强引用表
*/
static void traversestrongtable(global_State* g, Table* h)
{
    Node* n, * limit = gnodelast(h);
    unsigned int i;
    /* 遍历数组部分 */
    for (i = 0; i < h->sizearray; i++)
    {
        markvalue(g, &h->array[i]);
    }
    /* 遍历 hash 部分 */
    for (n = gnode(h, 0); n < limit; n++)
    {
        checkdeadkey(n);
        if (ttisnil(gval(n)))
        {
            /* 如果值为nil, 并且键为白色, 则设置结点状态为dead. */
            removeentry(n);
        }
        else
        {
            lua_assert(!ttisnil(gkey(n)));
            markvalue(g, gkey(n));  /* 标记 key */
            markvalue(g, gval(n));  /* 标记 value */
        }
    }
}

/*
** 遍历表中的所有键, 值. 若表为弱表, 则将其标记为灰色
*/
static lu_mem traversetable(global_State* g, Table* h)
{
    const char* weakkey, * weakvalue;  /* 弱键, 弱值 */
    const TValue* mode = gfasttm(g, h->metatable, TM_MODE);
    markobjectN(g, h->metatable);  /* 标记其元表 */
    if (mode && ttisstring(mode) &&  /* 是否存在弱键或弱表(弱引用)? */
        ((weakkey = strchr(svalue(mode), 'k')),
            (weakvalue = strchr(svalue(mode), 'v')),
            (weakkey || weakvalue)))
    {   /* 是否为弱表? */
        black2gray(h);  /* 弱表, 则黑色->灰色 */
        if (!weakkey)  /* 弱值(value) v? */
        {
            traverseweakvalue(g, h);  /* 遍历弱值表 */
        }
        else if (!weakvalue)  /* 弱键(key) k? */
        {
            traverseephemeron(g, h);  /* 遍历弱键表 */
        }
        else  /* 键值都为弱引用 */
        {
            linkgclist(h, g->allweak);  /* 则不进行遍历, 直接链接到 allweak 上 */
        }
    }
    else  /* 非弱表 */
    {
        traversestrongtable(g, h);
    }
    return sizeof(Table) + sizeof(TValue) * h->sizearray
        + sizeof(Node) * cast(size_t, allocsizenode(h));
}

/*
** 函数原型的遍历:
** 1. 如果有上次创建的闭包缓存, 则置缓存为NULL
** 2. 标记源文件信息
** 3. 标记常量
** 4. 标记上值名字
** 5. 标记内嵌的函数原型
** 6. 标记局部变量名
** (虽然正在构建原型, 但其数组可以大于所需的数组, 多余的填充为NULL, 因此使用
** 'markobjectN')
*/
static int traverseproto(global_State* g, Proto* f)
{
    int i;
    if (f->cache && iswhite(f->cache))
    {
        f->cache = NULL;
    }
    markobjectN(g, f->source);
    for (i = 0; i < f->sizek; i++)
    {
        /* 标记常量表 */
        markvalue(g, &f->k[i]);
    }
    for (i = 0; i < f->sizeupvalues; i++)
    {
        /* 标记 upvalue 的名字 */
        markobjectN(g, f->upvalues[i].name);
    }
    for (i = 0; i < f->sizep; i++)
    {
        /* 标记嵌套的原型 */
        markobjectN(g, f->p[i]);
    }
    for (i = 0; i < f->sizelocvars; i++)
    {
        /* 标记局部变量名称 */
        markobjectN(g, f->locvars[i].varname);
    }
    return sizeof(Proto) + sizeof(Instruction) * f->sizecode +
        sizeof(Proto*) * f->sizep + sizeof(TValue) * f->sizek +
        sizeof(int) * f->sizelineinfo + sizeof(LocVar) * f->sizelocvars +
        sizeof(Upvaldesc) * f->sizeupvalues;
}

/*
** 遍历 C 闭包
*/
static lu_mem traverseCclosure(global_State* g, CClosure* cl)
{
    int i;
    for (i = 0; i < cl->nupvalues; i++)  /* 标记 upvalues */
    {
        markvalue(g, &cl->upvalue[i]);
    }
    return sizeCclosure(cl->nupvalues);
}

/*
** Lua闭包的遍历:
** 1. 标记原型
** 2. 在 GCSinsideatomic 阶段标记所有 upvalue, 在非 GCSinsideatomic 阶段仅标记闭合
** 的 upvalue. (开放的 upvalue 指向的值在线程中, 因此那些值在非 GCSinsideatomic 阶段
** 线程被遍历时会被标记. 且那个阶段上值在线程中不能被改变, 不也会被再次遍历)
**
** open upvalues 指向线程中的值, 因此遍历该线程时应标记这些值, 但 GCSatomic 阶段除外
** (因为该值不能由该线程更改, 并且该线程不能再次遍历).
*/
static lu_mem traverseLclosure(global_State* g, LClosure* cl)
{
    int i;
    markobjectN(g, cl->p);  /* 标记其函数原型 Proto */
    for (i = 0; i < cl->nupvalues; i++)
    {   /* 标记 upvalues */
        UpVal* uv = cl->upvals[i];
        if (uv != NULL)
        {
            if (upisopen(uv) && g->gcstate != GCSinsideatomic)
            {
                uv->u.open.touched = 1;  /* 可能在'remarkupvals'中进行标记 */
            }
            else
            {
                markvalue(g, uv->v);
            }
        }
    }
    return sizeLclosure(cl->nupvalues);
}

/*
** 线程的遍历:
** 1. 标记栈
** 2. 若当前为 GCSinsideatomic 阶段, 则置栈顶到栈上限元素设为nil; 若不在开放上值线程
** 链表上且有打开的上值, 则将此对象链接到开放上值线程链接中
** 3. 若当前不是 GCSinsideatomic 阶段且GC类型不是 KGC_EMERGENCY, 则收缩线程的栈
*/
static lu_mem traversethread(global_State* g, lua_State* th)
{
    StkId o = th->stack;
    if (o == NULL)
    {
        return 1;  /* 堆栈尚未完全构建 */
    }
    lua_assert(g->gcstate == GCSinsideatomic ||
        th->openupval == NULL || isintwups(th));
    for (; o < th->top; o++)  /* 标记在堆栈中活动元素 */
    {
        markvalue(g, o);
    }
    if (g->gcstate == GCSinsideatomic)
    {   /* 最后的遍历? */
        StkId lim = th->stack + th->stacksize;  /* 堆栈的实际末尾 */
        for (; o < lim; o++)  /* 清除未标记的堆栈片段 */
        {
            setnilvalue(o);
        }
        /* 'remarkupvals'函数中可能已经将线程从'twups'列表中移除 */
        if (!isintwups(th) && th->openupval != NULL)
        {
            th->twups = g->twups;  /* 将其链接回 twups 链表 */
            g->twups = th;
        }
    }
    else if (g->gckind != KGC_EMERGENCY)
    {
        /* 在非 KGC_EMERGENCY 情况下更改堆栈大小 */
        luaD_shrinkstack(th);
    }
    return (sizeof(lua_State) + sizeof(TValue) * th->stacksize +
        sizeof(CallInfo) * th->nci);
}


/*
**     只从灰色链表中取一个灰色对象, 将其标记为黑色, 并对其引用的所有对象进行标记. (线
** 程除外, 它总是灰色的)
*/
static void propagatemark(global_State* g)
{
    lu_mem size;
    GCObject* o = g->gray;
    lua_assert(isgray(o));
    gray2black(o);  /* 灰色 -> 黑色 */
    switch (o->tt)
    {
    case LUA_TTABLE:  /* 表 */
    {
        Table* h = gco2t(o);
        g->gray = h->gclist;  /* 将其从灰色链表中移除 */
        size = traversetable(g, h);  /* 遍历表中元素 */
        break;
    }
    case LUA_TLCL:  /* lua 函数 */
    {
        LClosure* cl = gco2lcl(o);
        g->gray = cl->gclist;  /* 将其从灰色链表中移除 */
        size = traverseLclosure(g, cl);
        break;
    }
    case LUA_TCCL:  /* C 函数 */
    {
        CClosure* cl = gco2ccl(o);
        g->gray = cl->gclist;  /* 将其从灰色链表中移除 */
        size = traverseCclosure(g, cl);
        break;
    }
    case LUA_TTHREAD:  /* 线程 */
    {
        lua_State* th = gco2th(o);
        g->gray = th->gclist;  /* 将其从灰色链表中移除 */
        linkgclist(th, g->grayagain);  /* 链接到'grayagain'链表 */
        black2gray(o);  /* 重新标记为灰色 */
        size = traversethread(g, th);
        break;
    }
    case LUA_TPROTO:  /* 函数原型 */
    {
        Proto* p = gco2p(o);
        g->gray = p->gclist;  /* 将其从灰色链表中移除 */
        size = traverseproto(g, p);
        break;
    }
    default:
        lua_assert(0);
        return;
    }
    g->GCmemtrav += size;
}

/*
** 判断灰色链表是否为空, 不为空则遍历
*/
static void propagateall(global_State* g)
{
    while (g->gray) propagatemark(g);
}

/*
** 循环遍历 ephemeron 链表上弱key表
*/
static void convergeephemerons(global_State* g)
{
    int changed;
    do
    {
        GCObject* w;
        GCObject* next = g->ephemeron;  /* 获取 ephemeron 链表 */
        g->ephemeron = NULL;  /* 遍历时可能会修改此链表(traverseephemeron) */
        changed = 0;
        while ((w = next) != NULL)
        {
            next = gco2t(w)->gclist;
            if (traverseephemeron(g, gco2t(w)))
            {   /* 遍历时重新标记了一些值? */
                propagateall(g);  /* gray 链表被更改, 需要再次遍历 */
                changed = 1;  /* 将不得不重新访问 ephemeron 链表 */
            }
        }
    } while (changed);
}

/* }====================================================== */


/*
** {======================================================
** Sweep Functions
** =======================================================
*/


/*
** 当'l' != 'f'时, 清除列表'l'中所有弱表中未标记键(key)的结点
** 设置结点值为nil, 并且设置结点状态为dead
** 第三个参数 f 为 NULL(从引用看)
*/
static void clearkeys(global_State* g, GCObject* l, GCObject* f)
{
    for (; l != f; l = gco2t(l)->gclist)
    {
        Table* h = gco2t(l);
        Node* n, * limit = gnodelast(h);
        for (n = gnode(h, 0); n < limit; n++)
        {
            if (!ttisnil(gval(n)) && (iscleared(g, gkey(n))))
            {
                setnilvalue(gval(n));  /* 没有被引用的结点, 设置为 nil */
            }
            if (ttisnil(gval(n)))  /* 若结点值为 nil? */
            {
                removeentry(n);  /* 设置结点为 dead */
            }
        }
    }
}


/*
** 当'l' != 'f'时, 清除列表'l'中所有弱表中未标记值(value)的结点
** 设置结点值为nil, 并且设置结点状态为dead
** 第三个参数 f 为 NULL(从调用上看)
*/
static void clearvalues(global_State* g, GCObject* l, GCObject* f)
{
    for (; l != f; l = gco2t(l)->gclist)
    {
        Table* h = gco2t(l);
        Node* n, * limit = gnodelast(h);
        unsigned int i;
        /* 遍历数组部分 */
        for (i = 0; i < h->sizearray; i++)
        {
            TValue* o = &h->array[i];
            if (iscleared(g, o))  /* 值没有被引用 */
            {
                setnilvalue(o);  /* 设置值为 nil */
            }
        }
        /* 遍历 hash 部分 */
        for (n = gnode(h, 0); n < limit; n++)
        {
            if (!ttisnil(gval(n)) && iscleared(g, gval(n)))
            {
                setnilvalue(gval(n));  /* 设置结点的值为 nil */
                removeentry(n);  /* 若 key 也并未标记, 设置结点为 dead */
            }
        }
    }
}

/*
** 减少 UpVal 的引用
** 若UpVal的引用为0并且处于关闭状态, 则释放内存
** 关闭状态表明只有自己在用
*/
void luaC_upvdeccount(lua_State* L, UpVal* uv)
{
    lua_assert(uv->refcount > 0);
    uv->refcount--;
    if (uv->refcount == 0 && !upisopen(uv))
    {
        luaM_free(L, uv);
    }
}

/*
** 释放 lua 闭包
*/
static void freeLclosure(lua_State* L, LClosure* cl)
{
    int i;
    for (i = 0; i < cl->nupvalues; i++)
    {
        /* 修改upvalue的引用 */
        UpVal* uv = cl->upvals[i];
        if (uv)
        {
            luaC_upvdeccount(L, uv);
        }
    }
    luaM_freemem(L, cl, sizeLclosure(cl->nupvalues));
}

/*
** 释放对象内存
*/
static void freeobj(lua_State* L, GCObject* o)
{
    switch (o->tt)
    {
    case LUA_TPROTO:  /* 函数原型 */
        luaF_freeproto(L, gco2p(o));
        break;
    case LUA_TLCL:  /* lua 闭包 */
    {
        freeLclosure(L, gco2lcl(o));
        break;
    }
    case LUA_TCCL:  /* C 闭包 */
    {
        luaM_freemem(L, o, sizeCclosure(gco2ccl(o)->nupvalues));
        break;
    }
    case LUA_TTABLE:  /* 表 */
        luaH_free(L, gco2t(o));
        break;
    case LUA_TTHREAD:  /* 线程 */
        luaE_freethread(L, gco2th(o));
        break;
    case LUA_TUSERDATA:  /* 用户数据 */
        luaM_freemem(L, o, sizeudata(gco2u(o)));
        break;
    case LUA_TSHRSTR: /* 短字符串 */
        luaS_remove(L, gco2ts(o));  /* 从字符串 hash 表中移除 */
        luaM_freemem(L, o, sizelstring(gco2ts(o)->shrlen));
        break;
    case LUA_TLNGSTR:  /* 长字符串 */
    {
        luaM_freemem(L, o, sizelstring(gco2ts(o)->u.lnglen));
        break;
    }
    default:
        lua_assert(0);
    }
}


#define sweepwholelist(L,p)	sweeplist(L,p,MAX_LUMEM)
static GCObject** sweeplist(lua_State* L, GCObject** p, lu_mem count);


/*
**     从 p 链表上释放最多"count"个无效对象. 其中, 无效对象是标有旧(非当
** 前)白色的对象; 将所有非无效对象变回(当前)白色, 为下一个gc周期做准备.
** 返回继续遍历的位置; 如果链表访问完成, 则返回NULL.
*/
static GCObject** sweeplist(lua_State* L, GCObject** p, lu_mem count)
{
    global_State* g = G(L);
    int ow = otherwhite(g);  /* 旧白, 需要回收的对象就是这个颜色 */
    int white = luaC_white(g);  /* 新白, 标记阶段完成后新创建的对象为这个颜色 */
    while (*p != NULL && count-- > 0)
    {
        GCObject* curr = *p;
        int marked = curr->marked;
        if (isdeadm(ow, marked))
        {   /* 当前结点为无效结点 */
            *p = curr->next;  /* 从链表中移除 */
            freeobj(L, curr);  /* 释放该结点占用的内存 */
        }
        else
        {   /* 改变有效对象的标记为当前白 */
            curr->marked = cast_byte((marked & maskcolors) | white);
            p = &curr->next;  /* 下一个 */
        }
    }
    return (*p == NULL) ? NULL : p;
}


/*
** 释放一个链表直到没有无效对象或到达链表尾部
*/
static GCObject** sweeptolive(lua_State* L, GCObject** p)
{
    GCObject** old = p;
    do
    {
        p = sweeplist(L, p, 1);
    } while (p == old);
    return p;
}

/* }====================================================== */


/*
** {======================================================
** Finalization
** =======================================================
*/

/*
** 如果可能，收缩字符串表
*/
static void checkSizes(lua_State* L, global_State* g)
{
    if (g->gckind != KGC_EMERGENCY)
    {
        l_mem olddebt = g->GCdebt;
        if (g->strt.nuse < g->strt.size / 4)  /* 字符串表过大? */
        {
            luaS_resize(L, g->strt.size / 2);  /* 缩小为原先的一半 */
        }
        g->GCestimate += g->GCdebt - olddebt;  /* 更新估算 */
    }
}


static GCObject* udata2finalize(global_State* g)
{
    GCObject* o = g->tobefnz;  /* 获取第一个元素 */
    lua_assert(tofinalize(o));
    g->tobefnz = o->next;  /* 将其从'tobefnz'链表中移除 */
    o->next = g->allgc;  /* 链接到'allgc'链表上 */
    g->allgc = o;
    resetbit(o->marked, FINALIZEDBIT);  /* object is "normal" again */
    if (issweepphase(g))
    {
        makewhite(g, o);  /* 标记 */
    }
    return o;
}


static void dothecall(lua_State* L, void* ud)
{
    UNUSED(ud);
    luaD_callnoyield(L, L->top - 2, 0);
}


static void GCTM(lua_State* L, int propagateerrors)
{
    global_State* g = G(L);
    const TValue* tm;
    TValue v;
    setgcovalue(L, &v, udata2finalize(g));
    tm = luaT_gettmbyobj(L, &v, TM_GC);  /* 获取 __gc 元方法 */
    if (tm != NULL && ttisfunction(tm))
    {   /* 自带终结器, 调用? */
        int status;
        lu_byte oldah = L->allowhook;
        int running = g->gcrunning;
        L->allowhook = 0;  /* stop debug hooks during GC metamethod */
        g->gcrunning = 0;  /* avoid GC steps */
        setobj2s(L, L->top, tm);  /* push finalizer... */
        setobj2s(L, L->top + 1, &v);  /* ... and its argument */
        L->top += 2;  /* and (next line) call the finalizer */
        L->ci->callstatus |= CIST_FIN;  /* will run a finalizer */
        status = luaD_pcall(L, dothecall, NULL, savestack(L, L->top - 2), 0);
        L->ci->callstatus &= ~CIST_FIN;  /* not running a finalizer anymore */
        L->allowhook = oldah;  /* restore hooks */
        g->gcrunning = running;  /* restore state */
        if (status != LUA_OK && propagateerrors)
        {  /* error while running __gc? */
            if (status == LUA_ERRRUN)
            {   /* is there an error object? */
                const char* msg = (ttisstring(L->top - 1))
                    ? svalue(L->top - 1)
                    : "no message";
                luaO_pushfstring(L, "error in __gc metamethod (%s)", msg);
                status = LUA_ERRGCMM;  /* error in __gc metamethod */
            }
            luaD_throw(L, status);  /* re-throw error */
        }
    }
}


/*
** call a few (up to 'g->gcfinnum') finalizers
** 处理 tobefnz 链表上的元素, 调用相应的 __gc 元方法(分步)
*/
static int runafewfinalizers(lua_State* L)
{
    global_State* g = G(L);
    unsigned int i;
    lua_assert(!g->tobefnz || g->gcfinnum > 0);
    for (i = 0; g->tobefnz && i < g->gcfinnum; i++)
    {
        GCTM(L, 1);  /* call one finalizer */
    }
    g->gcfinnum = (!g->tobefnz) ? 0  /* nothing more to finalize? */
        : g->gcfinnum * 2;  /* else call a few more next time */
    return i;
}


/*
** 清除 tobenfnz 链表上的所有元素
*/
static void callallpendingfinalizers(lua_State* L)
{
    global_State* g = G(L);
    while (g->tobefnz)
    {
        GCTM(L, 0);
    }
}


/*
** 在链表"p"中找到最后一个结点的"下一个"结点(用于在其末尾添加元素)
*/
static GCObject** findlast(GCObject** p)
{
    while (*p != NULL)
    {
        p = &(*p)->next;
    }
    return p;
}


/*
**     将需要释放的对象(标记为白色的)(或者'all'对象), 从链表"finobj"移动到
** 链表"tobefnz"(待释放).
*/
static void separatetobefnz(global_State* g, int all)
{
    GCObject* curr;
    GCObject** p = &g->finobj;
    GCObject** lastnext = findlast(&g->tobefnz);
    while ((curr = *p) != NULL)
    {   /* 遍历 finobj 链表上的所有对象 */
        lua_assert(tofinalize(curr));
        if (!(iswhite(curr) || all))  /* 不需要释放? */
        {
            p = &curr->next;
        }
        else
        {
            /* 将对象从 finobj 链表上移除, 链接到 'tobefnz' 链表 */
            *p = curr->next;
            curr->next = *lastnext;
            *lastnext = curr;
            lastnext = &curr->next;
        }
    }
}


/*
**     如果对象'o'具有终结器, 则将其从'allgc'列表中删除(必须搜索该列表以找到它), 并
** 将其链接到'finobj'列表中.
*/
void luaC_checkfinalizer(lua_State* L, GCObject* o, Table* mt)
{
    global_State* g = G(L);
    if (tofinalize(o) ||                 /* obj. is already marked... */
        gfasttm(g, mt, TM_GC) == NULL)   /* or has no finalizer? */
        return;  /* nothing to be done */
    else
    {   /* move 'o' to 'finobj' list */
        GCObject** p;
        if (issweepphase(g))
        {
            makewhite(g, o);  /* "sweep" object 'o' */
            if (g->sweepgc == &o->next)  /* should not remove 'sweepgc' object */
                g->sweepgc = sweeptolive(L, g->sweepgc);  /* change 'sweepgc' */
        }
        /* search for pointer pointing to 'o' */
        for (p = &g->allgc; *p != o; p = &(*p)->next)
        { /* empty */
        }
        *p = o->next;  /* remove 'o' from 'allgc' list */
        o->next = g->finobj;  /* link it in 'finobj' list */
        g->finobj = o;
        l_setbit(o->marked, FINALIZEDBIT);  /* mark it as such */
    }
}

/* }====================================================== */



/*
** {======================================================
** GC control
** =======================================================
*/


/*
**     GCestimate 可以理解为 lua 的实际占用内存(当GC循环执行到 GCScallfin 状态以前,
** g->GCestimate 与 gettotalbytes(g) 必然相等, 即可以将 GCestimate 理解为当前 lua
** 的实际占用内存), 而 MAX_LMEM/estimate 即为本机最大内存量与当前 lua 实际使用量的比
** 值. 而 threshold 即为内存的阀值. 该阀值大部分时间是通过 estimate*gcpause 得到的.
** gcpause 默认值为100. 当然 gcpause 这个值也是可以通过手动GC函数 collectgarbage(“setpause”)
** 来设定的, 当 gcpause 为200时, 意味着 threshold = 2GCestimate, 则 debt=-GCestimate(gettotalbytes约等于GCestimate)，
** 所以 GCdebt 将在内存分配器分配新内存时由 -GCestimate 缓慢增长到大于零之后再开始新
** 的一轮GC, 所以 pause 被称为"间歇率", 即将 pause 设定为200时就会让收集器等到总内存
** 使用量达到之前的两倍时才开始新的GC循环.
**
** gcpause 控制每次完整GC间的间隔(即完整GC频率)
**     threshold = GCestimate / 100 * gcpause(默认200) 
**     debt = totalbytes - threshold ~~= totalbytes * (1 - gcpause / 100)
**     如果 gcpause 为200, 则当内存分配超过当前内存一倍时才触发下次完整GC
**     如果gcpause <= 100, 则基本每完整GC跑完后就会开始下一轮完整GC
**     说明 gcpause 值越小完整GC频率越高
*/
static void setpause(global_State* g)
{
    l_mem threshold, debt;
    l_mem estimate = g->GCestimate / PAUSEADJ;  /* adjust 'estimate' */
    lua_assert(estimate > 0);
    threshold = (g->gcpause < MAX_LMEM / estimate)  /* overflow? */
        ? estimate * g->gcpause  /* no overflow */
        : MAX_LMEM;  /* overflow; truncate to maximum */
    debt = gettotalbytes(g) - threshold;
    luaE_setdebt(g, debt);
}


/*
** 进入第一个清除阶段(GCSswpallgc).
** 调用'sweeplist'会尝试使指针指向列表内的一个对象(而不是header)，因此实际清除不需要
** 跳过在“now”和实际清除开始之间创建的对象.
*/
static void entersweep(lua_State* L)
{
    global_State* g = G(L);
    g->gcstate = GCSswpallgc;
    lua_assert(g->sweepgc == NULL);
    g->sweepgc = sweeplist(L, &g->allgc, 1);
}

/* 释放所有对象 */
void luaC_freeallobjects(lua_State* L)
{
    global_State* g = G(L);
    separatetobefnz(g, 1);  /* 处理所有自带终结器的对象 */
    lua_assert(g->finobj == NULL);
    callallpendingfinalizers(L);  /* 释放所有自带终结器的对象 */
    lua_assert(g->tobefnz == NULL);
    g->currentwhite = WHITEBITS; /* 改变当前白色, 所有的对象都是死的 */
    g->gckind = KGC_NORMAL;
    sweepwholelist(L, &g->finobj);  /* 释放 finobj 链表上的对象 */
    sweepwholelist(L, &g->allgc);  /* 释放 allgc 链表上的对象 */
    sweepwholelist(L, &g->fixedgc);  /* 释放 fixedgc 链表上的对象 */
    lua_assert(g->strt.nuse == 0);  /* 所有段字符串也在 allgc 链表上 */
}

/*
**     对于 grayagain 链表, 在进行 GCSpropagate 阶段时, 为了防止每次进入 singlestep
** 时 gray 上都有对象, 产生活锁, 对于可能反复变灰, 或者二次变灰的对象都会在 grayagain
** 链表上留待 GCSatomic 阶段一次性标记.
**
**     atomic 函数刚开始就先保存 grayagain, 因此为 atomic 函数中间不会让出, 为原子操
** 作, 不会受 barrier 函数的影响, 而除此之外在 GCSatomic 阶段惟一能进入 grayagain 的
** 就只有 LUA_TTHREAD 和 weak table(这两个永远不会为黑色), 重复去遍历 grayagain 没有
** 意义.
**
** atomic 函数的大致流程:
**     atomic 开始重新遍历根对象(当前 thread, 注册表 l_registry, 全局 metatable, 线
** 程相关的上值), 原因如下:
** 1. 在 lstate.c 中函数 init_registry 会改变 l_registry 的值, 并且不会有 barrier
** 的调用;
** 2. 根据 lapi.c 中的 lua_setmetatable 函数可知, 在修改全局 metatable 表时, 同样不
** 会有 barrier;
** 3. 线程相关的上值经常变化, 因为必须在 atomic 阶段重新遍历标记.
**
**     atomic 接着去遍历之前的 grayagain (grayagain 上会有弱表和线程的存在), 并清理
** 弱表的空间, 即将弱结点的状态改为 dead.
**
**     atomic 接着去调用 separatetobefnz 函数将带 __gc 函数的需要回收的(白色)对象放
** 到 tobefnz链表中, 留待以后清理. 这里需要修改一下之前的说明中有写到所有对象都在 allgc
** 链表的说法.
**     当对一个对像设置 __gc 函数时, 例如: setmetatable(tbl, {__gc == function() end}),
** LuaVM 会调用 luaC_checkfinalizer 函数将对将 tbl 从 allgc 链表上移除, 并将其添加到
** finobj 链表上.(__gc元表设置越早效率越高, 因为这一操作需要遍历才实现)
**     atomic 重新使 tobefnz 上的所有对象全部可达(标记), 不然 tobefnz 上的对象就有可
** 能被 singlestep 的后续步骤清除, 这样当调用__gc(tbl)时, 就无法传入正确的tbl.
**
**     atomic 最后重新清理了一下弱表(tobefnz 上的对象是有可能引用弱表的, 如果这些弱表
** 在此时还是白色的, 同样需要阻止其被回收, 但是其中的一些key需要重新考虑清除一下), 将当
** 前白色值切换到新一轮的白色值 (在此之后, 所有新建的对象虽然也为白色, 但是在GC循环走完
** 之前并不会被回收). 准备进入 GCSswpallgc 状态.
*/
/*
** 1. 置当前GC状态为 GCSinsideatomic
** 2. 标记当前运行的线程
** 3. 标记注册表
** 4. 标记基本类型的元表
** 5. 标记上值:
**       若线程不为灰色或没有上值, 则从有开放上值的线程链表(twups)中移除, 并标记所有
**    触碰过的上值
** 6. 再次遍历 gray 上所有元素
** 7. 遍历 grayagain 链表上所有元素
** 8. 循环遍历 ephemeron 链表上弱key表(因为上面的步骤可能导致 key 变为黑色), 直到没有
** 需要标记的结点, 最后 ephemeron 链表上的元素部分仍然是之前链表上的元素
** ---------------- 至此所有可被访问的强对象都被标记了 ----------------
** 9. 清理weak链表上弱表中可能需要被清理的值
** 10. 清理allweak链表上弱表中的可能需要被清理的值
** 11. 把finobj链表上没有被标记的对象移动到tobefnz链表上
** 12. 标记tobefnz链表上的元素
** 13. 再次遍历gray上所有元素
** 14. 执行第8步
** ---------------- 至此所有复活的对象都被标记了 ----------------
** 15. 清理ephemeron链表上弱表中的需要被清理的key
** 16. 清理allweak链表上弱表中的需要被清理的key
** 17. 清理weak链表上11步之后新增弱表中的可能需要被清理的value
** 18. 清理allweak链表上11步之后新增弱表中的可能需要被清理的value
** 19. 清理字符串缓存
** 20. 切换白色    
** 分析: 执行9,10步是因为即使弱key表上结点的value可能在12到14步被标记, 但实际也是将要
** 被终结的对象, 因此要把这个弱值置为nil.
*/
static l_mem atomic(lua_State* L)
{
    global_State* g = G(L);
    l_mem work;
    GCObject* origweak, * origall;
    GCObject* grayagain = g->grayagain;  /* 保存刚开始的 grayagain 链表 */
    lua_assert(g->ephemeron == NULL && g->weak == NULL);
    lua_assert(!iswhite(g->mainthread));
    g->gcstate = GCSinsideatomic;  /* 置GC状态为 GCSinsideatomic */
    g->GCmemtrav = 0;  /* 开始计算内存使用量 */
    markobject(g, L);  /* 标记当前运行线程 */
    /* API可能会更改注册表和全局元表, 需要重新标记 */
    markvalue(g, &g->l_registry);  /* 标记注册表 */
    markmt(g);  /* 标记基本类型的元表 */
    /* remark occasional upvalues of (maybe) dead threads */
    remarkupvals(g);  /* 标记上值 */
    propagateall(g);  /* 再次遍历 gray 上所有元素 */
    work = g->GCmemtrav;  /* 停止计算内存使用量(不需要计算'grayagain'链表上的数据)
                             已经计算过了 */
                             /* 遍历 grayagin 链表 */
    g->gray = grayagain;
    propagateall(g);
    g->GCmemtrav = 0;  /* 开始计算内存使用量 */
    convergeephemerons(g);  /* 遍历并标记 ephemeron 链表上的对象, 与弱键相关的表 */
    /* 至此所有可被访问的强对象都被标记了. */
    /* 在检测 tobefnz 链表前, 清理 弱表的 value */
    clearvalues(g, g->weak, NULL);  /* 清理weak链表上弱表中需要被清理的值 */
    clearvalues(g, g->allweak, NULL);  /* 清理allweak链表上弱表中的需要被清理的值 */
    origweak = g->weak; origall = g->allweak;  /* origweak 和 origall 应该都是 NULL */
    work += g->GCmemtrav;  /* 停止计算 (对象已访问完成) */

    separatetobefnz(g, 0);  /* 处理 finobj 链表上需要释放的对象, 移动到 tobefnz 链
                               表, 至此 finobj 上不再有(白色对象) */
    g->gcfinnum = 1;  /* 可能有对象需要回收, 同时表示每次回收的数量 */
    markbeingfnz(g);  /* 标记 tobefnz 链表上的对象 */
    propagateall(g);  /* markbeingfnz 可能会修改 gray 链表,  需要重新标记 gray 链
                         表, 有些对象可能"复活" */
    g->GCmemtrav = 0;  /* 重新开始计算 */
    convergeephemerons(g);  /* 遍历并标记 ephemeron 链表上的对象, 与弱键相关的表 */
    /* 至此所有复活的对象都被标记了. */
    /* 清除弱表中所有无效的结点 */
    clearkeys(g, g->ephemeron, NULL);  /* 清理ephemeron链表上弱表中的需要被清理的key */
    clearkeys(g, g->allweak, NULL);  /* 清理allweak链表上弱表中的需要被清理的key */

    /* 清理之后新增的弱表中需要清理的 value */
    clearvalues(g, g->weak, origweak);
    clearvalues(g, g->allweak, origall);
    luaS_clearcache(g);  /* 清理字符串缓存 */
    g->currentwhite = cast_byte(otherwhite(g));  /* 更新当前白色标记 */
    work += g->GCmemtrav;  /* 完成计算 */
    return work;  /* 在该阶段的内存使用的估值 */
}

/*
**     如果 sweepgc 链表不为空, 以增量模式(分阶段, 多次调用 sweepstep 函数释放链表
** 上的对象. 如果 sweepgc 链表为空, 则进入下一阶段(nextstate), 并且 sweepgc 指向下
** 一阶段需要释放的链表.
*/
static lu_mem sweepstep(lua_State* L, global_State* g,
    int nextstate, GCObject** nextlist)
{
    if (g->sweepgc)
    {
        l_mem olddebt = g->GCdebt;
        g->sweepgc = sweeplist(L, g->sweepgc, GCSWEEPMAX);
        g->GCestimate += g->GCdebt - olddebt;  /* 更新内存使用量 */
        if (g->sweepgc)  /* 尚未完全释放, 调整下次释放的速度 */
        {
            return (GCSWEEPMAX * GCSWEEPCOST);
        }
    }
    /* 当前链表已全部释放, 进入下一阶段, 更新下次需要释放的链表 */
    g->gcstate = nextstate;
    g->sweepgc = nextlist;
    return 0;
}

/*
**     luaVM 刚创建时 g->gcstate 默认值是 GCSpause.
**
**     如果整个GC过程一次执行完成后, 在对象很多的情况下, 会执行很长时间, 导致程序本身
** 的响应速度变慢. 所以Lua采用分步的方法执行, 同时对每次的访问量有个限制.
**
**     增量GC的标记过程可能会被打断(这一点可以从 case GCSpropagate 看出), 当一个对象
** 被标记为黑色后, Lua可能会重新对这个黑色对象进行操作, 这时就需要屏障(barrier)来感知
** 这一行为. Lua的GC有两种GC屏障,向前屏障(luaC_barrier)和向后屏障(luaC_barrierback).
**
**     除 GCSatomic 阶段是必须一次执行的外, 标记和清除阶段都是增量进行的. 如果在执行
** 完 GCSatomic 阶段后, 必须要保证新建的对象(新建对象默认都是白色的)不会被误清除. 所
** 执行完 GCStatomic 阶段后, 白色值会从白1切换到白2.
*/
static lu_mem singlestep(lua_State* L)
{
    /*
    **     注意: 该函数中存在对象所在链表的转移(如: gray->grayagain), 但对象总是在
    ** allgc 链表上. 因为 allgc 中使用的是 next 指针, 而发生转移使用的是 gclist 链
    ** 表. 每一个可gc的对象中都有一个 gclist 成员. 除了 TString 和 Udata.
    */
    global_State* g = G(L);
    switch (g->gcstate)
    {
    case GCSpause:
    {
        /* 重启垃圾回收 */
        /* 从根对象开始标记, 将白色->灰色, 并加入到灰色链表中 */
        g->GCmemtrav = g->strt.size * sizeof(GCObject*);  /* 计算实际内存使用量 */
        restartcollection(g);
        g->gcstate = GCSpropagate;
        return g->GCmemtrav;
    }
    case GCSpropagate:
    {
        /* 遍历标记: 访问灰色对象 */
        /*
        **     从灰色链表中取出一个对象(引用的值还没有被标记的对象)将其标记为黑色, 从
        ** gray 链表上移除, 并遍历和这个对象相关的其他对象; 直到 gray 链表为空, 就会
        ** 进入 GCStatomic 阶段.
        **
        **     判断灰色链表是否为空, 并非是循环实现的. 而是若不为灰, 状态不会发生改变.
        ** 每次进入 singlestep 函数后, 由于状态没有改变, 则反复执行对应的处理函数,
        ** 直至状态改变.
        **
        **     propagatemark 每次只会从 gray 链表中取一个灰色结点, 置为黑, 并从 gary
        ** 链表中移除, 遍历与此结点相关的其他结点, 并将有关结点加入到对应的链表中, 至
        ** 此就完成了一次 GCSpropagate 状态处理.
        **     这个过程只是处理了一个原先灰色链表中的灰色节点, 因为标记与对应节点有关
        ** 的节点实际上是通过遍历完成的, 这个过程的开销会很大, 所以 lua 只希望每次进
        ** 行 GCSpropagate 时, 处理一个这样的节点.
        **     这样的好处就是将开销大的步骤通过多次调用, 减少每次阻塞的时间. 而同时带
        ** 来了一个新的问题, 如果 lua 创建分配对象的速度远大于 GCSpropagate 处理的速
        ** 度, 那么 lua 的GC过程将会阻塞在 GCSpropagate 这个阶段.
        */
        g->GCmemtrav = 0;
        lua_assert(g->gray);
        propagatemark(g);
        if (g->gray == NULL)  /* 灰色链表全部标记完成? */
        {
            g->gcstate = GCSatomic;  /* 进入下一阶段 */
        }
        return g->GCmemtrav;  /* 在此步骤中遍历的内存(使用量) */
    }
    case GCSatomic:
    {

        /* 最后对灰色链表进行一次清除且保证是原子操作 */
        lu_mem work;
        /*
        **     先检测g->gray, 是因为luaC_barrier函数(用于处理新建对象的一种机制)
        ** 的存在, 它调用 reallymarkobject 时有可能会操作变量 global_State.gray.
        */
        propagateall(g);  /* 确保 gray 链表为空, 不为空则遍历 gray 上所有元素 */
        work = atomic(L);  /* atomic 完成需要原子操作 */
        /*
        **     在进入 GCSswpallgc 前会将 sweepgc 指向 allgc 上几乎所有对象, 之所以
        ** 用'几乎'这个词, 是因为它是通过函数 sweeplist 来做转换的, 这里不直接使用
        ** g->sweepgc = &g->allgc, 是希望 g->sweepgc 尽可能为 &(g->allgc->next)
        ** 的值. 原因是, 在进入 GCSswpallgc 状态后, 整个 GC 又进入了增量模式, 此时可
        ** 能会有很多新创建的对象(而这些对象是下一轮 GC 的白色值, 因此必然不会被回
        ** 收)挂在 allgc 上, g->sweepgc 指向 &(g->allgc->next) 可以尽可能的避免这
        ** 些额外干扰.
        */
        /* 进入清理阶段 用sweepgc记录下次该清理哪个链表 */
        entersweep(L);  /* 进入第一个清除阶段, */
        g->GCestimate = gettotalbytes(g);  /* 初步估算 */;
        return work;
    }
    case GCSswpallgc:
    {
        /*
        **     释放 allgc 链表上需要回收的对象, 并更新有效对象的标签. 当 allgc 链
        ** 表释放完成后, 进入 GCSswpfinobj 阶段, 准备释放 finobj 链表
        */
        return sweepstep(L, g, GCSswpfinobj, &g->finobj);
    }
    case GCSswpfinobj:
    {
        /*
        **     释放 finobj 链表上需要回收的对象, 并更新有效对象的标签(finobj 链表
        ** 上应该没有需要释放的对象). 当 finobj 链表释放完成后, 进入 GCSswptobefnz
        ** 阶段, 准备释放 tobefnz 链表. 原子阶段已经把白色对象转移到tobefnz上, 这里
        ** 仅仅是切换白色
        */
        return sweepstep(L, g, GCSswptobefnz, &g->tobefnz);
    }
    case GCSswptobefnz:
    {
        /*
        **     释放 tobefnz 链表上需要回收的对象, 切换白色(tobefnz 链表上应该没有需
        ** 要释放的对象). 当 tobefnz 链表释放完成后, 进入 GCSswpend 阶段.
        */
        return sweepstep(L, g, GCSswpend, NULL);
    }
    case GCSswpend:
    {
        makewhite(g, g->mainthread);  /* 重新标记 mainthread */
        checkSizes(L, g);  /* 调整全局字符串表的大小 */
        g->gcstate = GCScallfin;
        return 0;
    }
    case GCScallfin:
    {
        /*
        ** 逐个取出 tobefnz 链表上的对象, 然后调用其__gc函数, 并将其放入 allgc 链表中,
        ** 准备在下个 GC 循环回正式回收此对象. 这也意味着对象可以在GC函数中被复活, 同时
        ** 这个被复活的对象会失去其原有的__gc函数.
        */
        if (g->tobefnz && g->gckind != KGC_EMERGENCY)
        {
            int n = runafewfinalizers(L);
            return (n * GCFINALIZECOST);
        }
        else
        {
            /* 紧急模式或 tobefnz 释放完成 */
            g->gcstate = GCSpause;  /* GC 完成(新一轮GC) */
            return 0;
        }
    }
    default:
        lua_assert(0);
        return 0;
    }
}


/*
** advances the garbage collector until it reaches a state allowed
** by 'statemask'
*/
void luaC_runtilstate(lua_State* L, int statesmask)
{
    global_State* g = G(L);
    while (!testbit(statesmask, g->gcstate))
        singlestep(L);
}


/*
** 获得GC债务(需要释放的内存量)并将其从Kb转换为"工作单位"(避免零债务和溢出)
** 核心功能为 debt=debt*stepmul
**
** gcstepmul 控制本次单步 gc 的力度
**     debt = (debt / stepmuladj + 1) * gcstepmul 
**     如果 gcstepmul 为200, 则基本是上次借多少债本次 gcstep 就要遍历多少内存
**     如果 gcstepmul 小于200, 则要遍历的内存比借债少 (gcstep的时间较短, 同时一次完
** 整GC每步之间的间隔较长(频率增加))
**     如果 gcstepmul 大于200, 则要遍历的内存比借债多 (gcstep的时间较长, 同时一次完
** 整GC每步之间的间隔较短(频率减小))
*/
static l_mem getdebt(global_State* g)
{
    l_mem debt = g->GCdebt;
    int stepmul = g->gcstepmul;
    if (debt <= 0)
    {
        return 0;  /* minimal debt */
    }
    else
    {
        debt = (debt / STEPMULADJ) + 1;
        debt = (debt < MAX_LMEM / stepmul) ? debt * stepmul : MAX_LMEM;
        return debt;
    }
}

/*
**     当 GCdebt > 0 时, luaC_step 会通过控制 GCdebt, 循环调用 singlestep()来对内
** 存进行回收.
**     通过 gcpause、gcstepmul 可以对 debt 的值进行缩放, debt 的值越大, 则需要GC偿
** 还的债务越大, GC的过程会越活跃; 反之GC的债务越小, GC会越慢. 即: debt越大则GC越快,
** 反之则越慢.
*/
void luaC_step(lua_State* L)
{
    global_State* g = G(L);
    l_mem debt = getdebt(g);  /* GC deficit (be paid now) */
    /*
    **     debt并非是GCdebt, 而是被乘以倍率的GCdebt. 这个倍率即为gcstepmul. 默认为
    ** 200, 就是GCdebt的一个倍率, 上述getdebt函数的核心功能为debt=debt*stepmul. 即
    ** 通过stepmul将GCdebt放大或缩小一个倍率.
    */
    if (!g->gcrunning)
    {   /* gc没有运行? */
        luaE_setdebt(g, -GCSTEPSIZE * 10);  /* 避免被频繁调用 */
        return;
    }
    do
    {
        /*
        **     将 GCdebt 放大后的 debt 将会导致该循环的次数增加, 从而延长"一步"的工作
        ** 量, 所以 stepmul 被称为"步进倍率". 如果将 stepmul 设定的很大, 则将会将
        ** GCdebt放大很多倍, 那么GC将会退化成之前的GC版本stop-the-world, 因为它试图
        ** 在尽可能多的回收内存, 导致阻塞. 在这个循环中, 将会调用singlestep, 进行GC的
        ** 分步过程. 当进行完一个完整的GC过程或 GCdebt 小于一个基准量(-GCSTEPSIZE)时,
        ** 将会退出这个循环.
        */
        lu_mem work = singlestep(L);  /* perform one single step */
        debt -= work;
    } while (debt > -GCSTEPSIZE && g->gcstate != GCSpause);

    if (g->gcstate == GCSpause)
    {
        /*
        **     如果是完成一个GC循环, 则需要设定下一次GC循环的等待时间, 依然是通过
        ** GCdebt来设定
        */
        setpause(g);
    }
    else
    {
        /* 如果一个GC循环未结束, 则需要重新设置GCdebt, 等待下一次的触发. */
        debt = (debt / g->gcstepmul) * STEPMULADJ;  /* convert 'work units' to Kb */
        luaE_setdebt(g, debt);
        runafewfinalizers(L);
    }
}


/*
** 执行一次完整的GC. 如果是"isemergency", 则设置 gckind, 以避免某些可能以某些意外方
** 式更改解释器状态的操作. 在进行 gc 之前, 先检测"keepinvariant"; 如果为真, 则可能
** 有一些标记为黑色的对象, 因此必须将所有对象从黑色重新标记为白色(由于白色没有更改，因
** 此将不会收集任何内容).
** Performs a full GC cycle; if 'isemergency', set a flag to avoid
** some operations which could change the interpreter state in some
** unexpected ways (running finalizers and shrinking some structures).
** Before running the collection, check 'keepinvariant'; if it is true,
** there may be some objects marked as black, so the collector has
** to sweep all objects to turn them back to white (as white has not
** changed, nothing will be collected).
*/
void luaC_fullgc(lua_State* L, int isemergency)
{
    global_State* g = G(L);
    lua_assert(g->gckind == KGC_NORMAL);
    if (isemergency)
    {
        g->gckind = KGC_EMERGENCY;  /* set flag */
    }
    if (keepinvariant(g))
    {   /* black objects? */
        entersweep(L); /* sweep everything to turn them back to white */
    }
    /* finish any pending sweep phase to start a new cycle */
    luaC_runtilstate(L, bitmask(GCSpause));
    luaC_runtilstate(L, ~bitmask(GCSpause));  /* start new collection */
    luaC_runtilstate(L, bitmask(GCScallfin));  /* run up to finalizers */
    /* estimate must be correct after a full GC cycle */
    lua_assert(g->GCestimate == gettotalbytes(g));
    luaC_runtilstate(L, bitmask(GCSpause));  /* finish collection */
    g->gckind = KGC_NORMAL;
    setpause(g);
}

/* }====================================================== */


