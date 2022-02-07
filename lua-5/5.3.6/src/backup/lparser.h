/*
** $Id: lparser.h,v 1.76.1.1 2017/04/19 17:20:42 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/

#ifndef lparser_h
#define lparser_h

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"


/*
** 表达式和变量的描述.
** Code generation for variables and expressions can be delayed to allow
** optimizations; An 'expdesc' structure describes a potentially-delayed
** variable/expression. It has a description of its "main" value plus a
** list of conditional jumps that can also produce its value (generated
** by short-circuit operators 'and'/'or').
** 表达式和变量描述符。
**可以延迟变量和表达式的代码生成以允许优化; 'expdesc'结构描述了潜在延迟的变量/表达式。 它描述了它的“主”值加上一个条件跳转列表，它也可以产生它的值（由短路运算符'和'/'或'生成）。
*/

/* 变量或表达式的种类 */
typedef enum {
    VVOID,      /* 当'expdesc'描述列表的最后一个表达式时, 这种类型意味着一个空列表(所以, 没有表达式) */
    VNIL,       /* nil 常量nil */
    VTRUE,      /* true 常量true */
    VFALSE,     /* false 常量false */
    VK,         /* 常量(在Proto.k列表中); expdesc.info = 常量在Proto.k列表中的位置(index) */
    VKFLT,      /* 浮点常量; expdesc.nval = 浮点值 */
    VKINT,      /* 整形常量; expdesc.ival = 整形值 */
	VNONRELOC,  /* expression has its value in a fixed register;
                 info = result register */
  	VLOCAL,  /* local variable; info = local register */
  	VUPVAL,  /* upvalue variable; info = index of upvalue in 'upvalues' */
  	VINDEXED,  /* indexed variable;
                ind.vt = whether 't' is register or upvalue;
                ind.t = table register or upvalue;
                ind.idx = key's R/K index */
  	VJMP,  /* expression is a test/comparison;
            info = pc of corresponding jump instruction */
  	VRELOCABLE,  /* expression can put result in any register;
                  info = instruction pc */
  	VCALL,  /* 表达式为函数调用; info = instruction pc */
  	VVARARG  /* vararg expression; info = instruction pc */
} expkind;


#define vkisvar(k)	(VLOCAL <= (k) && (k) <= VINDEXED)
#define vkisinreg(k)	((k) == VNONRELOC || (k) == VLOCAL)

/* 表达式的相关描述信息 */
typedef struct expdesc
{
    expkind k;    /* 表达式类型 */
    union {
        lua_Integer ival;    /* 保存整形数据, 当k为VKINT */
        lua_Number nval;  /* 保存浮点数据, 当k为VKFLT */
        int info;  /* 通用for generic use */
        struct {  /* for indexed variables (VINDEXED) */
            short idx;  /* index (R/K) */
            lu_byte t;  /* table (register or upvalue) */
            lu_byte vt;  /* whether 't' is register (VLOCAL) or upvalue (VUPVAL) */
        } ind;
    } u;	/* 对应类型的数据 */
    int t;  /* 当表达式为true时, 跳转的位置偏移patch list of 'exit when true' */
    int f;  /* 当表达式为false时, 跳转的位置偏移patch list of 'exit when false' */
} expdesc;


/* 局部变量的描述 */
typedef struct Vardesc 
{
  	short idx;  /* 在栈中变量的索引 */
} Vardesc;


/* goto语句和label语句的描述 */
typedef struct Labeldesc 
{
	TString *name;  /* label的名称, 用来相互查找 */
	int pc;  /* 如果是label, pc表示这个label对应的当前函数指令集合的位置, 也就是待跳转的指令位置; 如果是goto, 则代表为这个goto生成的OP_JMP指令的位置 */
	int line;  /* 出现的位置 */
	lu_byte nactvar;  /* nactvar代表解析此goto或者label时, 函数有多少个有效的局部变量, 用来在跳转时决定需要关闭哪些upvalue */
} Labeldesc;


/* labels或gotos列表 */
typedef struct Labellist {
	Labeldesc *arr;  /* labels和gotos的数组 */
	int n;  /* 正在使用的数量 */
	int size;  /* arr数组的大小 */
} Labellist;


/* dynamic structures used by the parser */
typedef struct Dyndata {
	struct {  /* 活动的局部变量列表 */
		Vardesc *arr;	/* */
		int n;		/* arr 数组中变量个数 */
		int size;	/* arr 数组大小 */
	} actvar;
	Labellist gt;  /* goto列表 */
	Labellist label;   /* label列表 */
} Dyndata;


/* control of blocks */
struct BlockCnt;  /* defined in lparser.c */


/* 为给定功能生成代码所需的状态 */
typedef struct FuncState {
	Proto *f;  /* 当前函数的Proto, Proto中保存着函数的指令、变量信息、upvalue信息等其他信息 */
	struct FuncState *prev;  /* 上一个FuncState, Lua中每一个函数对应生成一个FuncState, 所有的FuncState形成链表 */
	struct LexState *ls;  /* 词法解析器 */
	struct BlockCnt *bl;  /* 当前块链, 语法块, 例如IF中要执行的就是一个语法块, 也会形成链表 */
	int pc;  /* 下一个代码的位置(相当于'ncode') */
	int lasttarget;   /* 最后一个跳转的标签 */
	int jpc;  /* 等待跳转到'pc'的列表, 待回填的跳转列表 */
	int nk;  /* 'k'中的元素数量(Proto.k)(常数表) */
	int np;  /* 'p'中的元素数量(Proto.p)(内嵌函数表) */
	int firstlocal;  /* 当前函数的第一个局部变量在全局数据表中的位置(in Dyndata array) */
	short nlocvars;  /* 'f->locvars'中的元素数量，从 0 开始 */
	lu_byte nactvar;  /* 当前函数中局部变量的个数(局部变量保存在Dyndata array中) */
	lu_byte nups;  /* upvalues的数量 */
	lu_byte freereg;  /* 第一个可用的寄存器 */
} FuncState;


LUAI_FUNC LClosure *luaY_parser(lua_State *L, ZIO *z, Mbuffer *buff,
                                Dyndata *dyd, const char *name, int firstchar);


#endif
