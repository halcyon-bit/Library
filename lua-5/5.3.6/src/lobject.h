/*
** $Id: lobject.h,v 2.117.1.1 2017/04/19 17:39:34 roberto Exp $
** Type definitions for Lua objects
** See Copyright Notice in lua.h
*/


#ifndef lobject_h
#define lobject_h


#include <stdarg.h>


#include "llimits.h"
#include "lua.h"


/*
** 非值的额外标签 其他类型
*/
#define LUA_TPROTO	LUA_NUMTAGS		/* 函数原型 */
#define LUA_TDEADKEY	(LUA_NUMTAGS+1)		/* removed keys in tables 删除表中的键 */

/*
** number of all possible tags (including LUA_TNONE but excluding DEADKEY)
*/
#define LUA_TOTALTAGS	(LUA_TPROTO + 2)


/*
** TValuefields 中 tt_ 标记使用以下位：
** 0-3位: 基本类型, 在lua.h中
** 4-5位: 扩展类型(基本类型的变种)
** 第6位: 可回收标记
** CommonHeader 中 tt 与 TValuefields 中 tt_ 的 0-5bits 相同
*/


/*
** LUA_TFUNCTION variants:
** 0 - Lua function
** 1 - light C function
** 2 - regular C function (closure)
*/

/* LUA_TFUNCTION(函数) 子类型 */
#define LUA_TLCL	(LUA_TFUNCTION | (0 << 4))  /* Lua闭包 */
#define LUA_TLCF	(LUA_TFUNCTION | (1 << 4))  /* 轻量C函数 */
#define LUA_TCCL	(LUA_TFUNCTION | (2 << 4))  /* C闭包 */

/* LUA_TSTRING(字符串) 子类型 */
#define LUA_TSHRSTR	(LUA_TSTRING | (0 << 4))  /* 短字符串 */
#define LUA_TLNGSTR	(LUA_TSTRING | (1 << 4))  /* 长字符串 */

/* LUA_TNUMBER(实数) 子类型 */
#define LUA_TNUMFLT	(LUA_TNUMBER | (0 << 4))  /* 浮点数(float) */
#define LUA_TNUMINT	(LUA_TNUMBER | (1 << 4))  /* 整形(int) */


/* 可回收类型的标记 */
#define BIT_ISCOLLECTABLE	(1 << 6)

/* 设置可回收的标记 */
#define ctb(t)			((t) | BIT_ISCOLLECTABLE)


/*
** 所有可收集对象的通用类型
*/
typedef struct GCObject GCObject;


/*
**     所有可收集对象的公共标题(以宏的形式, 包含在其他对象中). 所有可GC类型的
** struct, 在定义的首部都有CommonHeader宏, 是为了GCObject与这些类型直接的转
** 换, 详见联合GCUnion.
**     next 链表(链接下一个GCObject, 将所有GC对象用链表链接在一起), tt为类型
** 标记字段, marked在GC模块中使用, 表示GC过程中对象的活动状态, marked是gc处理
** 时存放的颜色值.
*/
#define CommonHeader	GCObject *next; lu_byte tt; lu_byte marked


/*
** Common type has only the common header
*/
struct GCObject
{
    CommonHeader;
};


/*
** 标记值. 这是Lua值的基本表示，
** 实际值加上其类型的标签。
*/

/*
** Union of all Lua values
** 基础数据类型
*/

/*
** Lua数据类型分为需要被GC管理回收的对象和按值存储的对象.
** 需要被GC管理回收的对象为: TString, Udata, Cloure, Table, Proto,
** lua_State. 这些数据类型有一个共同的部分CommonHeader.
** Value联合体的第一个字段为GCObject, 它是所有可以GC类型的公共定义。
** 包括FUNCTION, STRING, USERDATA, TABLE, THREAD.
*/
typedef union Value
{
    GCObject* gc;    /* collectable objects 可以gc的数据类型 */
    /*
    ** 轻量级userdata, 表示C指针的值, 对Lua虚拟机来说, 这种数据类型不需要GC(垃
    ** 圾回收), 其指向的内存由用户分配和释放
    */
    void* p;         /* light userdata 轻量用户数据, 指向c对象指针 */
    int b;           /* booleans 布尔类型 */
    lua_CFunction f; /* light C functions 函数指针 */
    lua_Integer i;   /* integer numbers 整形值 */
    lua_Number n;    /* float numbers 浮点值 */
} Value;

/* value_: 值	tt_: 值类型, 其中0-3bit表示基本类型;4-5bit表示子类型;第6bit表示是否可GC */
#define TValuefields	Value value_; int tt_

// 数据类型结构
typedef struct lua_TValue
{
    TValuefields;
} TValue;


/* 定义零值的宏  (值, 标记)(TValue) */
#define NILCONSTANT	{NULL}, LUA_TNIL

/* 获取TValue的值(value_) */
#define val_(o)		((o)->value_)

/* 获取TValue的标记(tt_) */
#define rttype(o)	((o)->tt_)

/* 获取TValue标记(tt_)的0-3bit, 即基本类型 */
#define novariant(x)	((x) & 0x0F)

/* 获取TValue标记(tt_)的后6位, 即子类型(bits 0-3 for tags + variant bits 4-5) */
#define ttype(o)	(rttype(o) & 0x3F)

/* 获取TValue标记(tt_)的后4位, 即基本类型 */
#define ttnov(o)	(novariant(rttype(o)))

/* 判断TValue的标记(tt_)是否为t */
#define checktag(o,t)		(rttype(o) == (t))
/* 判断TValue的标记(tt_)的后4位是否是t(基本类型)(0-3bit) */
#define checktype(o,t)		(ttnov(o) == (t))
/* 判断TValue是否为 实数(LUA_TNUMBER)(基本类型) */
#define ttisnumber(o)		checktype((o), LUA_TNUMBER)
/* 判断TValue是否为 浮点(LUA_TNUMFLT)(子类型) */
#define ttisfloat(o)		checktag((o), LUA_TNUMFLT)
/* 判断TValue是否为 整形(LUA_TNUMINT)(子类型) */
#define ttisinteger(o)		checktag((o), LUA_TNUMINT)
/* 判断TValue是否为 nil(LUA_TNIL)(基本类型) */
#define ttisnil(o)		checktag((o), LUA_TNIL)
/* 判断TValue是否为 布尔(LUA_TBOOLEAN)(基本类型) */
#define ttisboolean(o)		checktag((o), LUA_TBOOLEAN)
/* 判断TValue是否为 lightuserdata(LUA_TLIGHTUSERDATA)(基本类型) */
#define ttislightuserdata(o)	checktag((o), LUA_TLIGHTUSERDATA)
/* 判断TValue是否为 字符串(LUA_TSTRING)(基本类型) */
#define ttisstring(o)		checktype((o), LUA_TSTRING)
/* 判断TValue是否为 短字符串(LUA_TSHRSTR)(子类型)(可回收类型) */
#define ttisshrstring(o)	checktag((o), ctb(LUA_TSHRSTR))
/* 判断TValue是否为 长字符串(LUA_TLNGSTR)(子类型)(可回收类型) */
#define ttislngstring(o)	checktag((o), ctb(LUA_TLNGSTR))
/* 判断TValue是否为 表(LUA_TTABLE)(基本类型)(可回收类型) */
#define ttistable(o)		checktag((o), ctb(LUA_TTABLE))
/* 判断TValue是否为 函数(LUA_TFUNCTION)(基本类型) */
#define ttisfunction(o)		checktype(o, LUA_TFUNCTION)
/* 判断TValue是否为 闭包(后五位) ?? 函数 */
#define ttisclosure(o)		((rttype(o) & 0x1F) == LUA_TFUNCTION)
/* 判断TValue是否为 c闭包(LUA_TCCL)(子类型)(可回收类型) */
#define ttisCclosure(o)		checktag((o), ctb(LUA_TCCL))
/* 判断TValue是否为 lua闭包(LUA_TLCL)(子类型)(可回收类型) */
#define ttisLclosure(o)		checktag((o), ctb(LUA_TLCL))
/* 判断TValue是否为 light c函数(LUA_TLCF)(子类型) */
#define ttislcf(o)		checktag((o), LUA_TLCF)
/* 判断TValue是否为 LUA_TUSERDATA */
#define ttisfulluserdata(o)	checktag((o), ctb(LUA_TUSERDATA))
/* 判断TValue是否为 LUA_TTHREAD */
#define ttisthread(o)		checktag((o), ctb(LUA_TTHREAD))
/* 判断TValue是否为 LUA_TDEADKEY */
#define ttisdeadkey(o)		checktag((o), LUA_TDEADKEY)

/*
** 这组宏 实现了带类型校验的值获取操作.
** 对于需要GC的类型 利用下面一组宏(联合GCUnion下面(lstate.h)),
** 将GCObject转换后对应的TString TUSERDATA TFUNCTION TTABLE TPROTO TTHREAD数据类型.
** 所有的GCObject都有共同的头部, 也就是CommonHeader,
** 所以可以把GCObject转为GCUnion之后, 再转为具体类型的指针,
** 安全性由check_exp保证
*/
/* 获取TValue 整形 返回lua_Integer */
#define ivalue(o)	check_exp(ttisinteger(o), val_(o).i)
/* 获取TValue 浮点 返回lua_Number */
#define fltvalue(o)	check_exp(ttisfloat(o), val_(o).n)
/* 获取TValue 的实数(内部根据子类型进行转换) 返回lua_Number */
#define nvalue(o)	check_exp(ttisnumber(o), \
	(ttisinteger(o) ? cast_num(ivalue(o)) : fltvalue(o)))
/* 获取TValue GC数据 返回GCObject */
#define gcvalue(o)	check_exp(iscollectable(o), val_(o).gc)
/* 获取TValue 轻量自定义用户类型 返回void* */
#define pvalue(o)	check_exp(ttislightuserdata(o), val_(o).p)
/* 获取TValue 字符串 */
#define tsvalue(o)	check_exp(ttisstring(o), gco2ts(val_(o).gc))
/* 获取TValue UserData */
#define uvalue(o)	check_exp(ttisfulluserdata(o), gco2u(val_(o).gc))
/* 获取TValue 函数 */
#define clvalue(o)	check_exp(ttisclosure(o), gco2cl(val_(o).gc))
/* 获取TValue Lua函数(闭包) */
#define clLvalue(o)	check_exp(ttisLclosure(o), gco2lcl(val_(o).gc))
/* 获取TValue C函数(闭包) */
#define clCvalue(o)	check_exp(ttisCclosure(o), gco2ccl(val_(o).gc))
/* 获取TValue 轻量C函数 */
#define fvalue(o)	check_exp(ttislcf(o), val_(o).f)
/* 获取TValue 表 */
#define hvalue(o)	check_exp(ttistable(o), gco2t(val_(o).gc))
/* 获取TValue 布尔类型 */
#define bvalue(o)	check_exp(ttisboolean(o), val_(o).b)
/* 获取TValue 线程 */
#define thvalue(o)	check_exp(ttisthread(o), gco2th(val_(o).gc))
/* 死值可能会获得'gc'字段，但无法访问其内容 */
#define deadvalue(o)	check_exp(ttisdeadkey(o), cast(void *, val_(o).gc))

/* 判断TValue 是否为flase (nil 或 FALSE) */
#define l_isfalse(o)	(ttisnil(o) || (ttisboolean(o) && bvalue(o) == 0))

/* 判断是否是可gc类型 */
#define iscollectable(o)	(rttype(o) & BIT_ISCOLLECTABLE)


/* 针对GCObject, Lua提供一个判断TValue里的tag type 和GCObject里的tag type是否一致的判断 */
#define righttt(obj)		(ttype(obj) == gcvalue(obj)->tt)


#define checkliveness(L,obj) \
	lua_longassert(!iscollectable(obj) || \
		(righttt(obj) && (L == NULL || !isdead(G(L),gcvalue(obj)))))


/* 设置TValue 的类型 */
#define settt_(o,t)	((o)->tt_=(t))

/* 设置TValue的值 float */
#define setfltvalue(obj,x) \
  { TValue *io=(obj); val_(io).n=(x); settt_(io, LUA_TNUMFLT); }

/* 更改TValue的值 float */
#define chgfltvalue(obj,x) \
  { TValue *io=(obj); lua_assert(ttisfloat(io)); val_(io).n=(x); }

/* 设置TValue的值 int */
#define setivalue(obj,x) \
  { TValue *io=(obj); val_(io).i=(x); settt_(io, LUA_TNUMINT); }

/* 更改TValue的值 int */
#define chgivalue(obj,x) \
  { TValue *io=(obj); lua_assert(ttisinteger(io)); val_(io).i=(x); }

/* 设置TValue的值 nil */
#define setnilvalue(obj) settt_(obj, LUA_TNIL)

/* 设置TValue的值 轻量c函数 */
#define setfvalue(obj,x) \
  { TValue *io=(obj); val_(io).f=(x); settt_(io, LUA_TLCF); }

/* 设置TValue的值 轻量用户数据 */
#define setpvalue(obj,x) \
  { TValue *io=(obj); val_(io).p=(x); settt_(io, LUA_TLIGHTUSERDATA); }

/* 设置TValue的值 bool */
#define setbvalue(obj,x) \
  { TValue *io=(obj); val_(io).b=(x); settt_(io, LUA_TBOOLEAN); }

/* 设置TValue的值 gc(可回收) */
#define setgcovalue(L,obj,x) \
  { TValue *io = (obj); GCObject *i_g=(x); \
    val_(io).gc = i_g; settt_(io, ctb(i_g->tt)); }

/* 设置TValue的值 gc(string -> gc) */
#define setsvalue(L,obj,x) \
  { TValue *io = (obj); TString *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(x_->tt)); \
    checkliveness(L,io); }

/* 设置TValue的值 gc(udata -> gc) */
#define setuvalue(L,obj,x) \
  { TValue *io = (obj); Udata *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TUSERDATA)); \
    checkliveness(L,io); }

/* 设置TValue的值 gc(thread -> gc) */
#define setthvalue(L,obj,x) \
  { TValue *io = (obj); lua_State *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TTHREAD)); \
    checkliveness(L,io); }

/* 设置TValue的值 gc(Lua闭包 -> gc) */
#define setclLvalue(L,obj,x) \
  { TValue *io = (obj); LClosure *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TLCL)); \
    checkliveness(L,io); }

/* 设置TValue的值 gc(C闭包 -> gc) */
#define setclCvalue(L,obj,x) \
  { TValue *io = (obj); CClosure *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TCCL)); \
    checkliveness(L,io); }

/* 设置TValue的值 gc(table -> gc) */
#define sethvalue(L,obj,x) \
  { TValue *io = (obj); Table *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TTABLE)); \
    checkliveness(L,io); }

/* 设置TValue 为删除的键(死值) */
#define setdeadvalue(obj)	settt_(obj, LUA_TDEADKEY)


#define setobj(L,obj1,obj2) \
	{ TValue *io1=(obj1); *io1 = *(obj2); \
	  (void)L; checkliveness(L,io1); }


/*
** different types of assignments, according to destination
*/

/* from stack to (same) stack */
#define setobjs2s	setobj
/* to stack (not from same stack) */
#define setobj2s	setobj
#define setsvalue2s	setsvalue
#define sethvalue2s	sethvalue
#define setptvalue2s	setptvalue
/* from table to same table */
#define setobjt2t	setobj
/* to new object */
#define setobj2n	setobj
#define setsvalue2n	setsvalue

/* to table (define it as an expression to be used in macros) */
#define setobj2t(L,o1,o2)  ((void)L, *(o1)=*(o2), checkliveness(L,(o1)))

/*
** {======================================================
** 类型和原型
** =======================================================
*/

typedef TValue* StkId;  /* 堆栈元素的索引 */

/*
** 字符串类型
** 字符串的头; 字符串的字符在此结构的结尾
** (根据'UTString'对齐)
*/
typedef struct TString
{
    CommonHeader;
    /*
    ** 标记是否是虚拟机保留的字符串, 如果是短字符串, 1就是lua中保留的字符串(关键字),
    ** 不可GC, 用于词法分析器中对保留字的快速判断; 长字符串1, 表示已经计算过 hash, 用
    ** 于惰性求哈希值的策略(第一次用到才进行哈希计算).
    */
    lu_byte extra;
    lu_byte shrlen;  /* 短字符串的长度 */
    unsigned int hash;  /* 记录字符串的hash值, 可以用来加快字符串的匹配和查找. */
    union {
        size_t lnglen;  /* 记录长字符串的长度 */
        /* hash table 中相同 hash 值的字符串将串成一个列表, hnext 域指向下一个列表节点的指针 */
        struct TString* hnext;
    } u;
} TString;


/*
**     确保此类型后的地址始终完全对齐, L_Umaxalign 是一个宏, 用来保证 UTString 结
** 构里的 TString 按照这个长度来对齐, 按照union的定义, union的大小, 必须是单个结构
** 大小的整数倍. 例如:
** union un
** {
**  	int a[7];	// 元长度4
**  	double b;	// 元长度8
**  	char c[10];	// 元长度1
**  	int d[3];	// 元长度4
** };
** sizeof(un) = 32
** 联合体大小要满足最大元长度的整数倍
*/
/*
**     dummy 的目的是为了让 TString 按照 L_Umaxalign 类型来对齐, 在C语言中, struct/
** union 这样的复合数据类型是按照这个类型中最大对齐量的数据来对齐的, 所以这里就是按照
** double类型的对齐量来对齐的, 一般而言是8字节. 之所以要进行对齐操作, 是为了在CPU读取
** 数据时性能更高.
*/
typedef union UTString
{
    L_Umaxalign dummy;  /* 确保字符串的最大对齐 用于最大字节对齐 */
    TString tsv;  /* 字符串头部信息 */
} UTString;


/*
**     lua 源码中定义了一个叫"L_Umaxalign"的数据类型:
** typedef union { double u; void *s; lua_Integer i; long l; } L_Umaxalign;
** 其实就是该平台上最长的数据类型. 然后和 TString 和 Udata 结构定义成一个联合体, 之
** 所以这样处理是因为 TString 和 Udata 其实是一块连续内存区域的头部,  后面紧跟实体数
** 据. 为了保证访问实体数据时候字节对齐,  必须让实体数的起始地址是平台最大数据长度的
** 整数倍. 好比malloc返回的地址在32位平台上是8的整数倍, 在64位平台上是16的整数倍一样.
*/

/*
** 从'TString *ts'获取实际字符串(字节数组). 输入为TString*
** (访问'extra'确保值实际上是'TString'.)
*/
#define getstr(ts)  \
  check_exp(sizeof((ts)->extra), cast(char *, (ts)) + sizeof(UTString))


/* 从TValue *o获取实际字符串, 输入为TValue* */
#define svalue(o)       getstr(tsvalue(o))

/* 从 TString *s 获取字符串长度, 输入为 TString* */
#define tsslen(s)	((s)->tt == LUA_TSHRSTR ? (s)->shrlen : (s)->u.lnglen)

/* 从 TValue *o 获取字符串长度, 输入为 TValue* */
#define vslen(o)	tsslen(tsvalue(o))


/*
** 用户数据类型; 实际数据在此结构的结尾
** (根据'UUdata'对齐; 见下)
*/
typedef struct Udata
{
    CommonHeader;
    lu_byte ttuv_;  /* 用户自定义值的标记(即user_的类型) */
    struct Table* metatable;  /* 元方法 */
    size_t len;  /* 数据的长度 */
    union Value user_;  /* 用户数据 */
} Udata;


/*
** 确保此类型后的地址始终完全对齐, 参照UTString
*/
typedef union UUdata
{
    L_Umaxalign dummy;  /* ensures maximum alignment for 'local' udata */
    Udata uv;
} UUdata;


/*
** 获取'Udata'中数据块的内容. 输入为UData*
** (访问'ttuv_'确保该值实际上是'Udata'.)
*/
#define getudatamem(u)  \
  check_exp(sizeof((u)->ttuv_), (cast(char*, (u)) + sizeof(UUdata)))

/* 设置 Udata 的 user_ */
#define setuservalue(L,u,o) \
	{ const TValue *io=(o); Udata *iu = (u); \
	  iu->user_ = io->value_; iu->ttuv_ = rttype(io); \
	  checkliveness(L,io); }

/* 获取 Udata 的 user_ */
#define getuservalue(L,u,o) \
	{ TValue *io=(o); const Udata *iu = (u); \
	  io->value_ = iu->user_; settt_(io, iu->ttuv_); \
	  checkliveness(L,io); }


/*
** 函数原型中upvalue的描述
*/
typedef struct Upvaldesc
{
    TString* name;  /* upvalue的名字 (for debug information) */
    lu_byte instack;  /* 是否在栈中(寄存器) */
    lu_byte idx;  /* upvalue的索引(在堆栈或外部函数列表中) */
} Upvaldesc;


/*
** 函数原型的局部变量的描述
** (used for debug information)
*/
typedef struct LocVar
{
    TString* varname;  /* 局部变量的名字 */
    int startpc;  /* 当变量有效时的起始位置 */
    int endpc;    /* 当变量无效时的终止位置 */
} LocVar;


/*
** 函数原型
*/
typedef struct Proto
{
    CommonHeader;
    lu_byte numparams;  /* 参数的数量(确定的) */
    lu_byte is_vararg;  /* 不定参数的标志, 1 表示不定参数 */
    lu_byte maxstacksize;  /* 此功能所需的寄存器数量 */
    int sizeupvalues;  /* upvalues 数组的大小 */
    int sizek;  /* k 数组的大小 */
    int sizecode;  /* code 数组的大小 */
    int sizelineinfo;
    int sizep;  /* p 数组的大小 */
    int sizelocvars;  /* locvars 数组的内存大小 */
    int linedefined;  /* debug information  */
    int lastlinedefined;  /* debug information  */
    TValue* k;  /* 函数使用的常量(常量表) */
    Instruction* code;  /* opcodes 指令表 */
    struct Proto** p;  /* 函数内定义的函数 */
    int* lineinfo;  /* map from opcodes to source lines (debug information) */
    LocVar* locvars;  /* 局部变量信息 (debug information) */
    Upvaldesc* upvalues;  /* upvalue 信息 */
    struct LClosure* cache;  /* 这个原型的最后创建的闭包 */
    TString* source;  /* used for debug information */
    GCObject* gclist;
} Proto;



/*
** Lua Upvalues
*/
typedef struct UpVal UpVal;


/*
** Closures
*/

#define ClosureHeader \
	CommonHeader; lu_byte nupvalues; GCObject *gclist

/* C 闭包 */
typedef struct CClosure
{
    ClosureHeader;
    lua_CFunction f;    /* C 函数指针 */
    TValue upvalue[1];  /* upvalues列表 C闭包中的upvalue是关闭状态. 不需要引用外层函数中的局部变量 */
} CClosure;

/* Lua 闭包 */
typedef struct LClosure
{
    ClosureHeader;
    struct Proto* p;  /* 函数原型 */
    UpVal* upvals[1];  /* upvalues列表 */
} LClosure;

/* 闭包(Lua 和 C) */
typedef union Closure
{
    CClosure c;
    LClosure l;
} Closure;

/* 判断是否为lua闭包 */
#define isLfunction(o)	ttisLclosure(o)

/* 获取lua闭包中的函数原型 */
#define getproto(o)	(clLvalue(o)->p)


/*
** Tables
*/

/*
** TKey 结构是一个链表结构, 用来存放hash相同的所有 key
**     union 结构的说明, 当设置nk后, 使用 tvk 就等同于 nk 的 TValueFields
** 一般情况下, 如果看到一个数据类型是 union, 就可以知道这个数据想以一种较为省
** 内存的方式来表示多种用途, 而这些用途之间是“互斥”的, 也就是说, 在某个时刻该
** 数据类型只会是其中的一个含义. 这种编程技巧在 lua 中非常常见.
*/
typedef union TKey
{
    struct
    {
        TValuefields;   /* key值 */
        int next;   /* 用于标记链表的下一个节点(相对位置) */
        /* next 指向 key 冲突的下一个结点 */
    } nk;
    TValue tvk; /* key值 */
} TKey;

/* 设置key, 不考虑TKey中的next */
#define setnodekey(L,key,obj) \
	{ TKey *k_=(key); const TValue *io_=(obj); \
	  k_->nk.value_ = io_->value_; k_->nk.tt_ = io_->tt_; \
	  (void)L; checkliveness(L,io_); }

/* (key，value)对结构, 通过key计算获得的相同hash的Node保存在一个链表中; */
typedef struct Node
{
    TValue i_val; // 值value
    TKey i_key;   // 键key
} Node;

/*
** flags: 这是一个 byte 类型的数据, 用于表示这个表中提供了哪些元方法. 最开始这个flags
**        是空的, 也就是0, 当查找一次之后, 如果该表中存在某个元方法, 那么将该元方法对
**        应的 flag bit 置为l, 这样下一次查找时只需要比较这个 bit 就行了. 每个元方法
**        对应的bit定义在ltm.h文件中.
**        用于元表元方法的一些优化手段, 一共有8位用于标记是否没有某个元方法, 初始值都
**        是有的.
** lsizenode: 该表中以2为底的散列表大小的对数值. 同时由此可知, 散列表部分的大小一定是
**        2的幕, 即如果散列桶数组要扩展的话, 也是以每次在原大小基础上乘以2的形式扩展.
** sizearray: 数组部分的大小.
** *array: 指向数组部分的指针.
** *node: 指向该表的散列桶数组起始位置的指针.
** *lastfree: 指向该表散列桶数组的最后位置的指针(初始时).
** *metatable: 存放该表的元表.
** *gclist: GC相关的链表.
**
**     这里需要注意一个细节, 那就是lsizenode使用的是byte类型, 而sizearray使用的是int
** 类型. 由于在散列桶部分, 每个散列值相同的数据都会以链表的形式串起来, 所以即使数量用完
** 了, 也不要紧. 因此这里使用byte类型, 而且是原数据以2为底的对数值, 因为要根据这个值还
** 原回原来的真实数据, 也只是需要移位操作罢了, 速度很快.
*/
/* table是一个数组和hash表的结合体 */
typedef struct Table
{
    CommonHeader;
    lu_byte flags;  /* 元方法的标记, 用于查询table是否包含某个类别的元方法 */
    /* 哈希表的大小一定为 2 的整数次幂, 所以 lsizenode 表示的是幂次, 而不是实际大小 */
    lu_byte lsizenode;  /* 哈希部分(Node)的数量, 实际大小为(2^lsizenode), 哈希表的长度只能是2的幂次 */
    unsigned int sizearray;  /* 数组部分的大小 */
    TValue* array;  /* 数组部分 */
    Node* node;  /* 哈希部分 */
    Node* lastfree;  /* 表示table的hash表空闲节点的游标, 最后一个空闲位置 */
    struct Table* metatable;  /* 元表 */
    GCObject* gclist;  /* gc 链表 */
} Table;
/*
**     每个 table 结构, 最多会由三块连续内存构成. 一个Table结构, 一个存放了连
** 续整数索引的数组, 和一个大小为 2 的整数次幂的哈希表. 哈希表的最小尺寸为 2 的
** 0次幂, 也就是1. 为了减少空表的维护成本, Lua 做了优化, 它定义了一个不可改写的
** 空哈希表: dummynode. 让空表被初始化时, node 域指向这个 dummy 节点. 它虽然是
** 一个全局变量, 但因为对其访问是只读的, 所以不会引起线程安全问题.
*/

/*
** 用于散列的“模块”操作（大小始终为2的幂）
*/
#define lmod(s,size) \
	(check_exp((size&(size-1))==0, (cast(int, (s) & ((size)-1)))))

// 计算 Table 中 node 的大小
#define twoto(x)	(1<<(x))
#define sizenode(t)	(twoto((t)->lsizenode))


/*
** 固定的零值, TValue nil
*/
#define luaO_nilobject		(&luaO_nilobject_)

/* TValue nil */
LUAI_DDEC const TValue luaO_nilobject_;

/* 'luaO_utf8esc'函数中缓冲区的大小 */
#define UTF8BUFFSZ	8

LUAI_FUNC int luaO_int2fb(unsigned int x);
LUAI_FUNC int luaO_fb2int(int x);
LUAI_FUNC int luaO_utf8esc(char* buff, unsigned long x);
LUAI_FUNC int luaO_ceillog2(unsigned int x);
LUAI_FUNC void luaO_arith(lua_State* L, int op, const TValue* p1,
    const TValue* p2, TValue* res);
LUAI_FUNC size_t luaO_str2num(const char* s, TValue* o);
LUAI_FUNC int luaO_hexavalue(int c);
LUAI_FUNC void luaO_tostring(lua_State* L, StkId obj);
LUAI_FUNC const char* luaO_pushvfstring(lua_State* L, const char* fmt,
    va_list argp);
LUAI_FUNC const char* luaO_pushfstring(lua_State* L, const char* fmt, ...);
LUAI_FUNC void luaO_chunkid(char* out, const char* source, size_t len);

#endif