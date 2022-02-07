/*
** $Id: lcode.h,v 1.64.1.1 2017/04/19 17:20:42 roberto Exp $
** Code generator for Lua
** See Copyright Notice in lua.h
*/

#ifndef lcode_h
#define lcode_h

#include "llex.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"

/*
** Proto结构体用于保存函数运行时的指令. lua在解析函数的过程中, 会将一条条语句逐个'编译'
** 成指令, 这些指令存放在Proto->code成员中.
** 在解析整个函数结束后, 会生成一个常量表, 用于保存函数体内的常量字符串和数字, 指令存放在
** code里面, 函数体内的子函数放在p数组里
*/

/*
** 代码生成过程中根据表达式类型做相应的代码生成操作，
** lua中每个函数最大有250个寄存器，表达式的计算就是选
** 择这些寄存器存放并生成数据，而寄存器的下标是在代码生
** 成阶段选择好的，寄存器的释放是根据变量和表达式的生命
** 周期结束的时候释放。代码生成过程会将变量的生命周期的起
** 始pc和结束指令pc分别存放在Proto中的LocVar的startpc和
** endpc里面，供调试使用。
*/
/*
** Marks the end of a patch list. It is an invalid value both as an absolute
** address, and as a list link (would link an element to itself).
*/
#define NO_JUMP (-1)


/*
** grep "ORDER OPR" if you change these enums  (ORDER OP)
*/
/* 双目操作符 */
typedef enum BinOpr {
	OPR_ADD, OPR_SUB, OPR_MUL, OPR_MOD, OPR_POW,
	OPR_DIV,
	OPR_IDIV,
	OPR_BAND, OPR_BOR, OPR_BXOR,
	OPR_SHL, OPR_SHR,
	OPR_CONCAT,
	OPR_EQ, OPR_LT, OPR_LE,
	OPR_NE, OPR_GT, OPR_GE,
	OPR_AND, OPR_OR,
	OPR_NOBINOPR
} BinOpr;

/* 单目操作符 */
typedef enum UnOpr { OPR_MINUS, OPR_BNOT, OPR_NOT, OPR_LEN, OPR_NOUNOPR } UnOpr;


/* get (pointer to) instruction of given 'expdesc' */
#define getinstruction(fs,e)	((fs)->f->code[(e)->u.info])

#define luaK_codeAsBx(fs,o,A,sBx)	luaK_codeABx(fs,o,A,(sBx)+MAXARG_sBx)

#define luaK_setmultret(fs,e)	luaK_setreturns(fs, e, LUA_MULTRET)

#define luaK_jumpto(fs,t)	luaK_patchlist(fs, luaK_jump(fs), t)

LUAI_FUNC int luaK_codeABx(FuncState *fs, OpCode o, int A, unsigned int Bx);
LUAI_FUNC int luaK_codeABC(FuncState *fs, OpCode o, int A, int B, int C);
LUAI_FUNC int luaK_codek(FuncState *fs, int reg, int k);
LUAI_FUNC void luaK_fixline(FuncState *fs, int line);
LUAI_FUNC void luaK_nil(FuncState *fs, int from, int n);
LUAI_FUNC void luaK_reserveregs(FuncState *fs, int n);
LUAI_FUNC void luaK_checkstack(FuncState *fs, int n);
LUAI_FUNC int luaK_stringK(FuncState *fs, TString *s);
LUAI_FUNC int luaK_intK(FuncState *fs, lua_Integer n);
LUAI_FUNC void luaK_dischargevars(FuncState *fs, expdesc *e);
LUAI_FUNC int luaK_exp2anyreg(FuncState *fs, expdesc *e);
LUAI_FUNC void luaK_exp2anyregup(FuncState *fs, expdesc *e);
LUAI_FUNC void luaK_exp2nextreg(FuncState *fs, expdesc *e);
LUAI_FUNC void luaK_exp2val(FuncState *fs, expdesc *e);
LUAI_FUNC int luaK_exp2RK(FuncState *fs, expdesc *e);
LUAI_FUNC void luaK_self(FuncState *fs, expdesc *e, expdesc *key);
LUAI_FUNC void luaK_indexed(FuncState *fs, expdesc *t, expdesc *k);
LUAI_FUNC void luaK_goiftrue(FuncState *fs, expdesc *e);
LUAI_FUNC void luaK_goiffalse(FuncState *fs, expdesc *e);
LUAI_FUNC void luaK_storevar(FuncState *fs, expdesc *var, expdesc *e);
LUAI_FUNC void luaK_setreturns(FuncState *fs, expdesc *e, int nresults);
LUAI_FUNC void luaK_setoneret(FuncState *fs, expdesc *e);
LUAI_FUNC int luaK_jump(FuncState *fs);
LUAI_FUNC void luaK_ret(FuncState *fs, int first, int nret);
LUAI_FUNC void luaK_patchlist(FuncState *fs, int list, int target);
LUAI_FUNC void luaK_patchtohere(FuncState *fs, int list);
LUAI_FUNC void luaK_patchclose(FuncState *fs, int list, int level);
LUAI_FUNC void luaK_concat(FuncState *fs, int *l1, int l2);
LUAI_FUNC int luaK_getlabel(FuncState *fs);
LUAI_FUNC void luaK_prefix(FuncState *fs, UnOpr op, expdesc *v, int line);
LUAI_FUNC void luaK_infix(FuncState *fs, BinOpr op, expdesc *v);
LUAI_FUNC void luaK_posfix(FuncState *fs, BinOpr op, expdesc *v1,
	expdesc *v2, int line);
LUAI_FUNC void luaK_setlist(FuncState *fs, int base, int nelems, int tostore);


#endif
