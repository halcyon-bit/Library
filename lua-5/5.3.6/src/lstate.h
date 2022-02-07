/*
** $Id: lstate.h,v 2.133.1.1 2017/04/19 17:39:34 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include "lua.h"

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"


/*

** Some notes about garbage-collected objects: All objects in Lua must
** be kept somehow accessible until being freed, so all objects always
** belong to one (and only one) of these lists, using field 'next' of
** the 'CommonHeader' for the link:
**
** 'allgc': all objects not marked for finalization;
** 'finobj': all objects marked for finalization;
** 'tobefnz': all objects ready to be finalized;
** 'fixedgc': all objects that are not to be collected (currently
** only small strings, such as reserved words).
**
** Moreover, there is another set of lists that control gray objects.
** These lists are linked by fields 'gclist'. (All objects that
** can become gray have such a field. The field is not the same
** in all objects, but it always has this name.)  Any gray object
** must belong to one of these lists, and all objects in these lists
** must be gray:
**
** 'gray': regular gray objects, still waiting to be visited.
** 'grayagain': objects that must be revisited at the atomic phase.
**   That includes
**   - black objects got in a write barrier;
**   - all kinds of weak tables during propagation phase;
**   - all threads.
** 'weak': tables with weak values to be cleared;
** 'ephemeron': ephemeron tables with white->white entries;
** 'allweak': tables with weak keys and/or weak values to be cleared.
** The last three lists are used only during the atomic phase.

*/


struct lua_longjmp;  /* defined in ldo.c */


/*
** Atomic type (relative to signals) to better ensure that 'lua_sethook'
** is thread safe
*/
#if !defined(l_signalT)
#include <signal.h>
#define l_signalT	sig_atomic_t
#endif


/* extra stack space to handle TM calls and some other extras */
#define EXTRA_STACK   5


#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)


/* kinds of Garbage Collection */
#define KGC_NORMAL	0
#define KGC_EMERGENCY	1	/* gc was forced by an allocation failure */


typedef struct stringtable {
    TString** hash;
    int nuse;  /* number of elements */
    int size;
} stringtable;


/*
** Information about a call.
** When a thread yields, 'func' is adjusted to pretend that the
** top function has only the yielded values in its stack; in that
** case, the actual 'func' value is saved in field 'extra'.
** When a function calls another with a continuation, 'extra' keeps
** the function index so that, in case of errors, the continuation
** function can be called with the correct top.
*/
/* 正在调用的函数一定存在于数据栈上 */
typedef struct CallInfo
{
    /* 指向正在执行的函数在数据栈上的位置 */
    /* 需要记录这个信息, 是因为如果当前是一个lua函数, 且传入的参数个数不定的时候,
       需要用这个位置和当前数据栈顶的位置相减, 获取不定参数的个数(func-top) */
    StkId func;
    /* 函数所能使用的栈的最大位置, 任何一次Lua的C函数调用, 都只保证有LUA_MINSTACK的空闲数据栈空间使用 */
    StkId top;
    struct CallInfo* previous, * next;  /* 调用链, 双向链表 */
    union {
        struct {  /* lua函数 */
            StkId base;  /* 保存着当前函数的数据栈栈底指针 指向函数名在栈中的位置 */
            const Instruction* savedpc; /* 保存着指向当前指令的指针 */
        } l;
        struct {  /* C函数 */
            lua_KFunction k;  /* continuation in case of yields */
            ptrdiff_t old_errfunc;
            lua_KContext ctx;  /* context info. in case of yields */
        } c;
    } u;
    ptrdiff_t extra;
    short nresults;  /* expected number of results from this function */
    /* 正在调用的函数的运行状态, 有一位标志用来区分是C函数还是Lua函数 */
    unsigned short callstatus;
} CallInfo;


/*
** Bits in CallInfo status
*/
#define CIST_OAH	(1<<0)	/* original value of 'allowhook' */
#define CIST_LUA	(1<<1)	/* call is running a Lua function */
#define CIST_HOOKED	(1<<2)	/* call is running a debug hook */
#define CIST_FRESH	(1<<3)	/* call is running on a fresh invocation
                                   of luaV_execute */
#define CIST_YPCALL	(1<<4)	/* call is a yieldable protected call */
#define CIST_TAIL	(1<<5)	/* call was tail called */
#define CIST_HOOKYIELD	(1<<6)	/* last hook called yielded */
#define CIST_LEQ	(1<<7)  /* using __lt for __le */
#define CIST_FIN	(1<<8)  /* call is running a finalizer */

#define isLua(ci)	((ci)->callstatus & CIST_LUA)

                                   /* assume that CIST_OAH has offset 0 and that 'v' is strictly 0/1 */
#define setoah(st,v)	((st) = ((st) & ~CIST_OAH) | (v))
#define getoah(st)	((st) & CIST_OAH)


/*
** 'global state', shared by all threads of this state
*/
typedef struct global_State
{
    lua_Alloc frealloc;  /* function to reallocate memory */
    void* ud;         /* auxiliary data to 'frealloc' */
    l_mem totalbytes;  /* 为实际内存分配器所分配的内存与GCdebt的差值 */
    l_mem GCdebt;  /* 需要回收的内存数量, 查看luaM_realloc */
    /*
    **     GCdebt 就是不断的统计释放与分配的内存. 当新增分配内存时, GCdebt值将会增加,
    ** 即GC需要释放的内存增加; 当释放内存时, GCdebt 将会减少, 即GC需要释放的内存减少.
    ** GCdebt 大于零则意味着有需要GC释放还未释放的内存.
    */
    lu_mem GCmemtrav;  /* 用于标记阶段计算内存的使用量 */
    lu_mem GCestimate;  /* 内存实际使用量的估计值(与当前内存实际分配数量大约相等) */
    stringtable strt;  /* hash table for strings */
    TValue l_registry;
    unsigned int seed;  /* randomized seed for hashes */
    lu_byte currentwhite;
    lu_byte gcstate;  /* state of garbage collector */
    lu_byte gckind;  /* kind of GC running */
    lu_byte gcrunning;  /* true if GC is running */
    GCObject* allgc;  /* 所有可收集对象的列表(没有自带终结器_gc) */
    GCObject** sweepgc;  /* current position of sweep in list */
    GCObject* finobj;  /* 所有被标记为自带终结器(_gc)的对象链表 */
    /* 新增元素的地方:
    ** 第一次设置 table 的元表且元表中含有__gc方法, 此 table 对象会从 allgc 链表上移除
    ** 第一次设置 userdata 的元表且元表中含有__gc方法, 此 userdata 对象会从 allgc 链表上移除
    */
    GCObject* gray;  /* 常规灰色等待被访问对象链表 */
    GCObject* grayagain;  /* 在原子阶段必须被重新访问的灰色对象链表 */
    /*
    ** 包括: 在白色 barrier 中的黑色对象; 在标记阶段的所有类型弱表; 所有线程
    ** 新增元素的地方:
    ** 1. 标记阶段遍历弱值表时
    ** 2. 标记阶段遍历弱key表时
    ** 3. 遍历线程时
    ** 4. 将黑色结点变灰色向后barrier时
    */
    /* 以下3个链表仅仅在原子阶段有效 */
    GCObject* weak;  /* 弱值表对象链表 */
    /*
    ** 新增元素的地方:
    ** 非标记阶段遍历弱值表含有可能需要清理的值时
    */
    GCObject* ephemeron;  /* 含有弱key表的链表, 含有白色->白色结点 */
    /*
    ** 新增元素的地方:
    ** 1. 非标记阶段遍历弱key表时有结点的key和value都为白色
    */
    GCObject* allweak;  /* 有弱键和/或弱值将被清理的表 */
    /*
    ** 新增元素的地方:
    ** 1. 非标记阶段遍历弱 key 表时含有可能需要清理的 key 且其 value 标记过
    ** 2. 遍历表时表是弱key且弱值型
    */
    GCObject* tobefnz;  /* 将要被释放的对象链表(userdata) */
    GCObject* fixedgc;  /* 不会被回收的对象链表 */
    struct lua_State* twups;  /* list of threads with open upvalues */
    unsigned int gcfinnum;  /* number of finalizers to call in each GC step */
    int gcpause;  /* size of pause between successive GCs */
    int gcstepmul;  /* GC 'granularity' */
    lua_CFunction panic;  /* to be called in unprotected errors */
    struct lua_State* mainthread;
    const lua_Number* version;  /* pointer to version number */
    TString* memerrmsg;  /* memory-error message */
    TString* tmname[TM_N];  /* array with tag-method names */
    struct Table* mt[LUA_NUMTAGS];  /* metatables for basic types */
    TString* strcache[STRCACHE_N][STRCACHE_M];  /* cache for strings in API */
} global_State;


/*
** 'per thread' state
*/
/*
** lua_State中两个重要的数据结构: 数据栈和调用链
** 数据栈分为两类: 值类型和引用类型. 值类型可以被任意复制, 而引用类型共享一份数据, 由GC负责维护生命期
** 调用链: 以双向链表的形式存储在线程对象中
*/
struct lua_State
{
    /* gc头 */
    CommonHeader;
    /* 每一个THREAD实际就是一个死循环解释执行指令的容器, 本质上是一个状态机, 该字段表示中间步骤的状态 */
    lu_byte status;
    /* Lua全局对象, 只存在一份, 同一Lua虚拟机中的所有执行线程, 共享了一块全局数据l_G */
    global_State* l_G;

    /* 数据栈 */
    /* 栈的大小 (本质为TValue数组的大小) */
    int stacksize;
    /* 栈顶指针(类似于esp, 会变化的) */
    StkId top;
    /* 栈底指针(类似于ebp, 基本不变) */
    StkId stack;
    /* 栈的最大位置(真正意义上的栈顶) */
    StkId stack_last;  /* 堆栈中的最后一个空闲位置 */

    /* 函数调用链 */
    /* 指向当前调用函数(会变化) */
    CallInfo* ci;
    /* 记录调用链的栈底 (最外层的Callinfo) base_ci一定是从C函数发起的调用 */
    CallInfo base_ci;  /* CallInfo for first level (C calling Lua) */
    /* 记录 non-yieldable 的调用个数 */
    unsigned short nny;  /* number of non-yieldable calls in stack */
    /* 记录调用栈中C函数的个数 */
    unsigned short nCcalls;  /* number of nested C calls */
    /* 调用链中函数个数 */
    unsigned short nci;  /* number of items in 'ci' list */

    /* 闭包变量信息 */
    /* C Closure和Lua Closure都会有闭包变量
    ** C Closure的闭包直接就是一个TValue数组保存在CClosure里,
    ** Lua Closure的闭包变量, 分为open和close两种状态,
    ** 如果是close状态, 则拷贝到LClosure自己的UpVal数组里,
    ** 但如果是open状态, 则直接指向了作用域上的变量地址.
    ** 调用栈展开过程中, 从调用栈的栈顶的所有open状态的UpVal, 也构成栈结构(链表串起来的),
    ** 一个lua-State代表一个协程, 一个协程能闭包其它协程的变量,
    ** twups就是代表其它的协程(闭包了当前的lua-state的变量)*/
    UpVal* openupval;  /* list of open upvalues in this stack */
    struct lua_State* twups;  /* list of threads with open upvalues */

    /* 类似PC 寄存器，指向最后一次执行的指令的指令 */
    const Instruction* oldpc;  /* last pc traced */

    /* GC 垃圾回收 */
    GCObject* gclist;

    /* Hook Debug 服务于DEBUG模块 */
    volatile lua_Hook hook; /* 用户注册的hook回调函数 */
    int basehookcount;    /* 用户设置的执行指令数(LUA_MASKCOUNT下有效) */
    int hookcount;        /* 运行时, 跑了多少指令(LUA_MASKCOUNT下有效) */
    l_signalT hookmask;   /* 支持哪些hook能力 */
    lu_byte allowhook;    /* 是否允许hook */

    /* 异常错误处理: 因为thread 是一个死循环解释执行指令, 在这个过程中, 出现的异常与错误需要处理 */
    struct lua_longjmp* errorJmp;  /* 保存当前的longjmp返回点 每次运行一段受保护的lua代码, 都会生成一个新的错误返回点 */
    ptrdiff_t errfunc;  /* current error handling function (stack index) */
};


#define G(L)	(L->l_G)


/*
** Union of all collectable objects (only for conversions)
*/
union GCUnion {
    GCObject gc;  /* common header */
    struct TString ts;
    struct Udata u;
    union Closure cl;
    struct Table h;
    struct Proto p;
    struct lua_State th;  /* thread */
};


#define cast_u(o)	cast(union GCUnion *, (o))

/* macros to convert a GCObject into a specific value */
#define gco2ts(o)  \
	check_exp(novariant((o)->tt) == LUA_TSTRING, &((cast_u(o))->ts))
#define gco2u(o)  check_exp((o)->tt == LUA_TUSERDATA, &((cast_u(o))->u))
#define gco2lcl(o)  check_exp((o)->tt == LUA_TLCL, &((cast_u(o))->cl.l))
#define gco2ccl(o)  check_exp((o)->tt == LUA_TCCL, &((cast_u(o))->cl.c))
#define gco2cl(o)  \
	check_exp(novariant((o)->tt) == LUA_TFUNCTION, &((cast_u(o))->cl))
#define gco2t(o)  check_exp((o)->tt == LUA_TTABLE, &((cast_u(o))->h))
#define gco2p(o)  check_exp((o)->tt == LUA_TPROTO, &((cast_u(o))->p))
#define gco2th(o)  check_exp((o)->tt == LUA_TTHREAD, &((cast_u(o))->th))


/* macro to convert a Lua object into a GCObject */
#define obj2gco(v) \
	check_exp(novariant((v)->tt) < LUA_TDEADKEY, (&(cast_u(v)->gc)))


/* actual number of total bytes allocated */
#define gettotalbytes(g)	cast(lu_mem, (g)->totalbytes + (g)->GCdebt)

LUAI_FUNC void luaE_setdebt(global_State* g, l_mem debt);
LUAI_FUNC void luaE_freethread(lua_State* L, lua_State* L1);
LUAI_FUNC CallInfo* luaE_extendCI(lua_State* L);
LUAI_FUNC void luaE_freeCI(lua_State* L);
LUAI_FUNC void luaE_shrinkCI(lua_State* L);


#endif

