/*
** $Id: lstate.h,v 2.133.1.1 2017/04/19 17:39:34 roberto Exp $
** 全局状态机 Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include "lua.h"

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"

/*
**     关于垃圾收集对象的一些注意事项: Lua中的所有对象必须以某种方式保持可访问状态
** 直到被释放, 因此所有对象始终属于这些列表中的一个(且仅一个), 使用"CommonHeader"
** 的字段"next"作为链接:
** 'allgc': 几乎所有的可回收对象都在该链表上
** 'finobj': 有 __gc 元方法的 Table 和 userdata 会从 allgc 链表上转移到该链表
** 'tobefnz': 需要清除的 Table 和 userdata, 在该链表上, 调用 __gc 元方法清除
** 'fixedgc': 所有不被回收的对象(目前只有短字符串, 如保留字)
**
**     此外, 还有另一组控制灰色对象的链表. 这些列表由字段'gclist'链接.(所有可以变为
** 灰色的对象都有这样的字段. 所有对象中的字段都不相同, 但它始终具有此名称.)任何灰色
** 对象必须属于这些列表中的一个, 并且这些列表中的所有对象必须为灰色:
** 'gray': 常规灰色对象, 等待被标记.
** 'grayagain': 必须在原子阶段(atomic)重新访问的对象. 包括:
**  - 黑色对象进入向前屏障;
**  - 标记阶段的各种弱表;
**  - 所有线程.
** 'weak': 需要清除的弱值的表.
** 'ephemeron': 带有white->white结点的 ephemeron 表.
** 'allweark': 具有弱键和弱值的表将被清除.
*/

struct lua_longjmp;  /* defined in ldo.c */


/* 原子类型(相对于信号)更好地确保'lua_sethook'是线程安全的 */
#if !defined(l_signalT)
#include <signal.h>
#define l_signalT	sig_atomic_t
#endif


/* 额外的堆栈空间来处理TM调用和一些其他附加功能 */
#define EXTRA_STACK   5

/* 最初的数据栈的大小 */
#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)

/* 各种垃圾的种类 */
#define KGC_NORMAL	0
#define KGC_EMERGENCY	1	/* gc被分配失败所迫 */

/*
**    stringtable初始大小由宏MINSTRTABSIZE控制, 默认是64, 用户可以在luaconf.h重新定义
** MINSTRTABSIZE宏来改变默认大小. 在为stringtable初次分配空间的时候, 调用的也是luaS_resize
** 方法, 将stringtable空间由0调整到MINSTRTABSIZE的大小.
**    stringtable在字符串内部化的过程中扩大的策略和STL中的vector比较类似: 当空间不足时, 
** 大小扩大为当前空间的两倍大小.
*/
typedef struct stringtable 
{
  /* 字符串开散列算法哈希表, hash是一维数组指针, 其中数组元素
                  ** 类型为TString*(指向TString类型对象指针), 它并不是一个二
                  ** 维数组(数组元素类型为TString)指针 */
  TString **hash;	/* 二重数组, 用于保存字符串, [不同hash][相同hash] */
  int nuse;   /* hash表中元素的个数 *//* 字符串表当前字符串数量 */
  int size;   /* hash表的大小 */ /* 字符串表最大字符串数量 */
} stringtable;

/*
** 函数调用的相关信息
** 
当线程生成时，“func”会被调整为假设top函数在其堆栈中只有生成的值；
在这种情况下，实际的“func”值会保存在字段“extra”中。
当一个函数调用另一个带有连续符的函数时，“extra”会保留函数索引，以便在出现错误时，可以使用正确的top调用连续函数。
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
  StkId	top;
  struct CallInfo *previous, *next;  /* 调用链, 双向链表 */
  union {
    struct {  /* lua函数 */
      StkId base;  /* 保存着当前函数的数据栈栈底指针 指向函数名在栈中的位置 */
      const Instruction *savedpc; /* 保存着指向当前指令的指针 */
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

/* 判断是否是Lua函数 */
#define isLua(ci)	((ci)->callstatus & CIST_LUA)

/* assume that CIST_OAH has offset 0 and that 'v' is strictly 0/1 */
#define setoah(st,v)	((st) = ((st) & ~CIST_OAH) | (v))
#define getoah(st)	((st) & CIST_OAH)


/*
** 'global state', shared by all threads of this state
** 全局状态 由所有线程共享
** lua全局状态机, 无法用公开的API获取到它的指针、句柄或引用.
*/
/*
1. global_State.totalbytes + global_State.GCdebt 就是整个LuaVM使用的全部内存总量
2. luaE_setdebt函数只会修改global_State.GCdebt, 但是不会改变整个LuaVM的使用内存总量
3. 步进倍率是靠变量global_State.gcstepmul控制的
4. 间歇率是靠变量global_State.gcpause来控制的
5. 根据注释可知GCestimate代表在使用的非垃圾内存
*/
typedef struct global_State 
{
  lua_Alloc frealloc;  /* function to reallocate memory */
  void *ud;         /* auxiliary data to 'frealloc' */
  l_mem totalbytes;  /* 实际内存分配器所分配的内存与GCdebt的差值 */
  l_mem GCdebt;  /* 没有被回收器补偿的分配字节, 预估内存借债, 新申请的内存超过此值, 则触发GC */
  lu_mem GCmemtrav;  /* memory traversed by the GC */
  lu_mem GCestimate;  /* 使用中的非垃圾内存的估值(与当前内存实际分配数量大约相等) */
  stringtable strt;  /* hash table for strings 全局字符串表 */
  TValue l_registry;  /* 是一个表, 保存全局信息, 详见 init_registry */
  unsigned int seed;  /* randomized seed for hashes */
  lu_byte currentwhite;
  lu_byte gcstate;  /* state of garbage collector */
  lu_byte gckind;  /* kind of GC running */
  lu_byte gcrunning;  /* true if GC is running */
  GCObject *allgc;  /* 所有可回收对象的链表, 即清除阶段, 被GC依次遍历链表上的对象并根据标志进行清除 */
  GCObject **sweepgc;  /* current position of sweep in list */
  GCObject *finobj;  /* list of collectable objects with finalizers */
  GCObject *gray;  /* list of gray objects */
  GCObject *grayagain;  /* list of objects to be traversed atomically */
  GCObject *weak;  /* list of tables with weak values */
  GCObject *ephemeron;  /* list of ephemeron tables (weak keys) */
  GCObject *allweak;  /* list of all-weak tables */
  GCObject *tobefnz;  /* userdata GC列表 */
  GCObject *fixedgc;  /* list of objects not to be collected */
  struct lua_State *twups;  /* list of threads with open upvalues */
  unsigned int gcfinnum;  /* number of finalizers to call in each GC step */
  int gcpause;  /* size of pause between successive GCs 连续GC之间的暂停大小 */
  int gcstepmul;  /* GC 'granularity' */
  lua_CFunction panic;  /* to be called in unprotected errors 全局错误处理 */
  struct lua_State *mainthread;
  const lua_Number *version;  /* pointer to version number 版本号 */
  TString *memerrmsg;  /* memory-error message */
  TString *tmname[TM_N];  /* array with tag-method names */
  struct Table *mt[LUA_NUMTAGS];  /* metatables for basic types */
  TString *strcache[STRCACHE_N][STRCACHE_M];  /* cache for strings in API */
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
	global_State *l_G;

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
	CallInfo *ci;
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
	UpVal *openupval;  /* list of open upvalues in this stack */
	struct lua_State *twups;  /* list of threads with open upvalues */
  
	/* 类似PC 寄存器，指向最后一次执行的指令的指令 */
	const Instruction *oldpc;  /* last pc traced */
  
	/* GC 垃圾回收 */
	GCObject *gclist;

    /* Hook Debug 服务于DEBUG模块 */
    volatile lua_Hook hook; /* 用户注册的hook回调函数 */
    int basehookcount;    /* 用户设置的执行指令数(LUA_MASKCOUNT下有效) */
    int hookcount;        /* 运行时, 跑了多少指令(LUA_MASKCOUNT下有效) */
    l_signalT hookmask;   /* 支持哪些hook能力 */
    lu_byte allowhook;    /* 是否允许hook */

	  /* 异常错误处理: 因为thread 是一个死循环解释执行指令, 在这个过程中, 出现的异常与错误需要处理 */
	  struct lua_longjmp *errorJmp;  /* 保存当前的longjmp返回点 每次运行一段受保护的lua代码, 都会生成一个新的错误返回点 */
    ptrdiff_t errfunc;  /* current error handling function (stack index) */  
};

/* 获取lua_state中的全局数据global_state */
#define G(L)	(L->l_G)


/*
** Union of all collectable objects (only for conversions)
** 所有可收集对象的联合（仅用于转换）
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
/* 宏将GCObject转换为特定值 */
// GCObject -> String
#define gco2ts(o)  \
	check_exp(novariant((o)->tt) == LUA_TSTRING, &((cast_u(o))->ts))
// GCObject -> UserData
#define gco2u(o)  check_exp((o)->tt == LUA_TUSERDATA, &((cast_u(o))->u))
// GCObject -> Lua闭包
#define gco2lcl(o)  check_exp((o)->tt == LUA_TLCL, &((cast_u(o))->cl.l))
// GCObject -> C闭包
#define gco2ccl(o)  check_exp((o)->tt == LUA_TCCL, &((cast_u(o))->cl.c))
// GCObject -> Function
#define gco2cl(o)  \
	check_exp(novariant((o)->tt) == LUA_TFUNCTION, &((cast_u(o))->cl))
// GCObject -> Table
#define gco2t(o)  check_exp((o)->tt == LUA_TTABLE, &((cast_u(o))->h))
// GCObject -> Proto
#define gco2p(o)  check_exp((o)->tt == LUA_TPROTO, &((cast_u(o))->p))
// GCObject -> Thread
#define gco2th(o)  check_exp((o)->tt == LUA_TTHREAD, &((cast_u(o))->th))


/* macro to convert a Lua object into a GCObject */
// 用于将Lua对象转换为GCObject的宏
#define obj2gco(v) \
	check_exp(novariant((v)->tt) < LUA_TDEADKEY, (&(cast_u(v)->gc)))


/* 实际分配的总字节数actual number of total bytes allocated */
#define gettotalbytes(g)	cast(lu_mem, (g)->totalbytes + (g)->GCdebt)

LUAI_FUNC void luaE_setdebt (global_State *g, l_mem debt);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);
LUAI_FUNC CallInfo *luaE_extendCI (lua_State *L);
LUAI_FUNC void luaE_freeCI (lua_State *L);
LUAI_FUNC void luaE_shrinkCI (lua_State *L);


#endif

