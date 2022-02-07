/*
** $Id: lopcodes.c,v 1.55.1.1 2017/04/19 17:20:42 roberto Exp $
** Opcodes for Lua virtual machine
** See Copyright Notice in lua.h
*/

#define lopcodes_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>

#include "lopcodes.h"


/* ORDER OP */

LUAI_DDEF const char* const luaP_opnames[NUM_OPCODES + 1] = {
  "MOVE",
  "LOADK",
  "LOADKX",
  "LOADBOOL",
  "LOADNIL",
  "GETUPVAL",
  "GETTABUP",
  "GETTABLE",
  "SETTABUP",
  "SETUPVAL",
  "SETTABLE",
  "NEWTABLE",
  "SELF",
  "ADD",
  "SUB",
  "MUL",
  "MOD",
  "POW",
  "DIV",
  "IDIV",
  "BAND",
  "BOR",
  "BXOR",
  "SHL",
  "SHR",
  "UNM",
  "BNOT",
  "NOT",
  "LEN",
  "CONCAT",
  "JMP",
  "EQ",
  "LT",
  "LE",
  "TEST",
  "TESTSET",
  "CALL",
  "TAILCALL",
  "RETURN",
  "FORLOOP",
  "FORPREP",
  "TFORCALL",
  "TFORLOOP",
  "SETLIST",
  "CLOSURE",
  "VARARG",
  "EXTRAARG",
  NULL
};


#define opmode(t,a,b,c,m) (((t)<<7) | ((a)<<6) | ((b)<<4) | ((c)<<2) | (m))
/*
** T: (第7bit)表示这是不是一条逻辑测试相关的指令, 这种指令可能会涉及一次条件跳转
      , 将PC指针自增1. (之所以需要这个标记, 是因为Lua中所有涉及条件分支的地方,
      实际上都在分支指令后紧随着一条JMP指令. Lua没有为布尔运算单独设计opcode,
      它让所有的布尔运算都以分支执行流的形式出现. Lua的And与Or关键字支持短路求值,
      所以在VM中以分支跳转的形式实现). 分支指令和之后的JMP跳转指令是一体的,
      是因为32bit的Instruction无法全部描述才分拆为两条指令. 这个指令可以用来检测
      是不是分支指令. 当遇到JMP指令时, 可以回溯到前面的一条指令来分辨是否是一次条件
      跳转. 这对生成Lua的bytecode模块有帮助.
** A: (第6bit)表示这个指令会不会赋值给R(A), 这个标记在debug模块被用于跟踪最后
      改变register内容的指令位置, 帮助生成debug info.
** B: (第4-5bit) B参数的格式.
** C: (第2-3bit) C参数的格式.
** mode: (第0-1bit)OpCode的格式
** 这些分类信息, 用于luac反编译字节码时的输出, 对于Lua的运行时没有实际意义.
*/
LUAI_DDEF const lu_byte luaP_opmodes[NUM_OPCODES] = {
    /*       T  A    B       C     mode		   opcode	*/
      opmode(0, 1, OpArgR, OpArgN, iABC)		/* OP_MOVE */
     ,opmode(0, 1, OpArgK, OpArgN, iABx)		/* OP_LOADK */
     ,opmode(0, 1, OpArgN, OpArgN, iABx)		/* OP_LOADKX */
     ,opmode(0, 1, OpArgU, OpArgU, iABC)		/* OP_LOADBOOL */
     ,opmode(0, 1, OpArgU, OpArgN, iABC)		/* OP_LOADNIL */
     ,opmode(0, 1, OpArgU, OpArgN, iABC)		/* OP_GETUPVAL */
     ,opmode(0, 1, OpArgU, OpArgK, iABC)		/* OP_GETTABUP */
     ,opmode(0, 1, OpArgR, OpArgK, iABC)		/* OP_GETTABLE */
     ,opmode(0, 0, OpArgK, OpArgK, iABC)		/* OP_SETTABUP */
     ,opmode(0, 0, OpArgU, OpArgN, iABC)		/* OP_SETUPVAL */
     ,opmode(0, 0, OpArgK, OpArgK, iABC)		/* OP_SETTABLE */
     ,opmode(0, 1, OpArgU, OpArgU, iABC)		/* OP_NEWTABLE */
     ,opmode(0, 1, OpArgR, OpArgK, iABC)		/* OP_SELF */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_ADD */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_SUB */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_MUL */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_MOD */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_POW */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_DIV */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_IDIV */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_BAND */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_BOR */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_BXOR */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_SHL */
     ,opmode(0, 1, OpArgK, OpArgK, iABC)		/* OP_SHR */
     ,opmode(0, 1, OpArgR, OpArgN, iABC)		/* OP_UNM */
     ,opmode(0, 1, OpArgR, OpArgN, iABC)		/* OP_BNOT */
     ,opmode(0, 1, OpArgR, OpArgN, iABC)		/* OP_NOT */
     ,opmode(0, 1, OpArgR, OpArgN, iABC)		/* OP_LEN */
     ,opmode(0, 1, OpArgR, OpArgR, iABC)		/* OP_CONCAT */
     ,opmode(0, 0, OpArgR, OpArgN, iAsBx)		/* OP_JMP */
     ,opmode(1, 0, OpArgK, OpArgK, iABC)		/* OP_EQ */
     ,opmode(1, 0, OpArgK, OpArgK, iABC)		/* OP_LT */
     ,opmode(1, 0, OpArgK, OpArgK, iABC)		/* OP_LE */
     ,opmode(1, 0, OpArgN, OpArgU, iABC)		/* OP_TEST */
     ,opmode(1, 1, OpArgR, OpArgU, iABC)		/* OP_TESTSET */
     ,opmode(0, 1, OpArgU, OpArgU, iABC)		/* OP_CALL */
     ,opmode(0, 1, OpArgU, OpArgU, iABC)		/* OP_TAILCALL */
     ,opmode(0, 0, OpArgU, OpArgN, iABC)		/* OP_RETURN */
     ,opmode(0, 1, OpArgR, OpArgN, iAsBx)		/* OP_FORLOOP */
     ,opmode(0, 1, OpArgR, OpArgN, iAsBx)		/* OP_FORPREP */
     ,opmode(0, 0, OpArgN, OpArgU, iABC)		/* OP_TFORCALL */
     ,opmode(0, 1, OpArgR, OpArgN, iAsBx)		/* OP_TFORLOOP */
     ,opmode(0, 0, OpArgU, OpArgU, iABC)		/* OP_SETLIST */
     ,opmode(0, 1, OpArgU, OpArgN, iABx)		/* OP_CLOSURE */
     ,opmode(0, 1, OpArgU, OpArgN, iABC)		/* OP_VARARG */
     ,opmode(0, 0, OpArgU, OpArgU, iAx)		/* OP_EXTRAARG */
};

