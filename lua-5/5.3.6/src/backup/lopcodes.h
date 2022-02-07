/*
** $Id: lopcodes.h,v 1.149.1.1 2017/04/19 17:20:42 roberto Exp $
** Opcodes for Lua virtual machine
** See Copyright Notice in lua.h
*/

#ifndef lopcodes_h
#define lopcodes_h

#include "llimits.h"

/*
** Lua中的每条指令都是由一个32位无符号整数表示, 指令种类和操作对象都被编写进这个数字
** 指令类型为Instruction
** Lua的寄存器指向当前栈空间, 最多支持256个寄存器
*/

/*===========================================================================
  We assume that instructions are unsigned numbers.
  All instructions have an opcode in the first 6 bits.
  Instructions can have the following fields:
	'A' : 8 bits
	'B' : 9 bits
	'C' : 9 bits
	'Ax' : 26 bits ('A', 'B', and 'C' together)
	'Bx' : 18 bits ('B' and 'C' together)
	'sBx' : signed Bx

  A signed argument is represented in excess K; that is, the number
  value is the unsigned value minus K. K is exactly the maximum value
  for that argument (so that -max is represented by 0, and +max is
  represented by 2*max), which is half the maximum for the corresponding
  unsigned argument.
===========================================================================*/
/*
** 指令格式:
**
**    低位                             高位
** | 6 bits  |  8 bits  |  9 bits  |  9 bits |
** | opcode  |     A    |     C    |    B    |
** | opcode  |     A    |         Bx         |
** | opcode  |     A    |         sBx        |
** | opcode  |               Ax              |
** 
** A一般为目的地址, Bx和sBx的区别: sBx可以表示负数, 以2^17(0)为分界,
** 2^17+1为1, 2^17-1为-1. 跳转指令就需要用到sbx, 实现向前跳转(负数)
** 因为opcode为6位, 最多支持2^6-1=63条指令
** 参数A, B, C一般用来存放指令操作数据的地址(索引), 而地址(索引)有以下三种:
** 寄存器 idx     idx从0开始
** 常量表 idx     idx从1开始
** upvalue idx    idx从0开始
*/

/* 根据参数个数和种类的不同, 指令种类分为4种 */
enum OpMode {iABC, iABx, iAsBx, iAx};  /* 基本指令格式 */


/*
** A 一般为目的地址, Bx和sBx的区别: sBx可以表示负数 0 = 2^17 为分界
**   低位                             高位
**  6 bits  |  8 bits  |  9 bits  |  9 bits |
**  opcode  |     A    |     C    |    B    |
**  opcode  |     A    |         Bx         |
**  opcode  |     A    |         sBx        |
**  opcode  |               Ax              |
*/
/* A, B, C, opcode的大小和位置 */
/*
** 操作码参数的大小和位置
*/
/* 大小 */
#define SIZE_C		9                             /* C  */
#define SIZE_B		9                             /* B  */
#define SIZE_Bx		(SIZE_C + SIZE_B)             /* Bx */
#define SIZE_A		8                             /* A  */
#define SIZE_Ax		(SIZE_C + SIZE_B + SIZE_A)    /* Ax */

#define SIZE_OP		6                             /* 指令 */

/* 位置 */
#define POS_OP		0
#define POS_A		(POS_OP + SIZE_OP)
#define POS_C		(POS_A + SIZE_A)
#define POS_B		(POS_C + SIZE_C)
#define POS_Bx		POS_C
#define POS_Ax		POS_A


/*
** 操作码参数的限制
** 使用(signed) int来操纵大多数参数,
** 所以它们必须符合LUAI_BITSINT-1位(-1表示符号)
** 32位系统中LUAI_BITSINT = 32
*/
#if SIZE_Bx < LUAI_BITSINT-1
#define MAXARG_Bx        ((1<<SIZE_Bx)-1)
#define MAXARG_sBx        (MAXARG_Bx>>1)         /* 'sBx' is signed */
#else
#define MAXARG_Bx        MAX_INT
#define MAXARG_sBx        MAX_INT
#endif

#if SIZE_Ax < LUAI_BITSINT-1
#define MAXARG_Ax	((1<<SIZE_Ax)-1)
#else
#define MAXARG_Ax	MAX_INT
#endif


/*
** 所能表示的最大操作数(即寄存器的最大编号255)
*/
#define MAXARG_A        ((1<<SIZE_A)-1)   
#define MAXARG_B        ((1<<SIZE_B)-1)
#define MAXARG_C        ((1<<SIZE_C)-1)


/* (32位)从p的位置起向高位移动创建n个1, 其他位为0 */
#define MASK1(n,p)	((~((~(Instruction)0)<<(n)))<<(p))

/* (32位)从p的位置起向高位移动创建n个0, 其他位为1 */
#define MASK0(n,p)	(~MASK1(n,p))

/*
** the following macros help to manipulate instructions
*/

/* 获取指令码 (i & 00000000000000000000000000111111) */
#define GET_OPCODE(i)	(cast(OpCode, ((i)>>POS_OP) & MASK1(SIZE_OP,0)))
/* 设置指令码 (现将i的指令位都置0, 将o的非指令位置0, i | o) */
#define SET_OPCODE(i,o)	((i) = (((i)&MASK0(SIZE_OP,POS_OP)) | \
		((cast(Instruction, o)<<POS_OP)&MASK1(SIZE_OP,POS_OP))))


/* 获取操作数(a,b,...)值 */
#define getarg(i,pos,size)	    (cast(int, ((i)>>pos) & MASK1(size,0)))
/* 设置操作数(a,b,...)值 */
#define setarg(i,v,pos,size)	((i) = (((i)&MASK0(size,pos)) | \
                                ((cast(Instruction, v)<<pos)&MASK1(size,pos))))
/* 操作数A */
#define GETARG_A(i)	        getarg(i, POS_A, SIZE_A)
#define SETARG_A(i,v)       setarg(i, v, POS_A, SIZE_A)

/* 操作数B */
#define GETARG_B(i)	        getarg(i, POS_B, SIZE_B)
#define SETARG_B(i,v)	    setarg(i, v, POS_B, SIZE_B)

/* 操作数C */
#define GETARG_C(i)	        getarg(i, POS_C, SIZE_C)
#define SETARG_C(i,v)	    setarg(i, v, POS_C, SIZE_C)

/* 操作数Bx */
#define GETARG_Bx(i)	    getarg(i, POS_Bx, SIZE_Bx)
#define SETARG_Bx(i,v)	    setarg(i, v, POS_Bx, SIZE_Bx)

/* 操作数Ax */
#define GETARG_Ax(i)	    getarg(i, POS_Ax, SIZE_Ax)
#define SETARG_Ax(i,v)	    setarg(i, v, POS_Ax, SIZE_Ax)

/* 操作数sBx */
#define GETARG_sBx(i)	    (GETARG_Bx(i)-MAXARG_sBx)   /* MAXARG_sBx为131071(在lua中表示0) */
#define SETARG_sBx(i,b)	    SETARG_Bx((i),cast(unsigned int, (b)+MAXARG_sBx))


/* 创建iABC指令 */
#define CREATE_ABC(o,a,b,c)	((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_A) \
			| (cast(Instruction, b)<<POS_B) \
			| (cast(Instruction, c)<<POS_C))

/* 创建iABx指令 */
#define CREATE_ABx(o,a,bc)	((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_A) \
			| (cast(Instruction, bc)<<POS_Bx))

/* 创建iAx指令 */
#define CREATE_Ax(o,a)		((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_Ax))


/*
** 运行RK指数的宏
*/
/*
** 使用最高位来区分寄存器或者常量编号
** 最高位为1表示常量编号, 0表示寄存器
*/
/*
** 当函数引用的常量较少时, Lua允许让具体的指令直接访问常量表, 而不需要将常量读取到寄存器(B or C)
** 为了区分使用一位来表示是寄存器, 还是常量编号
** (左移)最高位为1表示常量编号, 0表示寄存器
*/
#define BITRK		(1 << (SIZE_B - 1))   /* B 和 C的长度是一样的 */

/* 
** 判断操作数是否为常量编号
*/
#define ISK(x)		((x) & BITRK)

/* 
** 获取常量编号中的实际常量编号
** 即去除判断操作数是常量编号还是寄存器的标记
*/
#define INDEXK(r)	((int)(r) & ~BITRK)

#if !defined(MAXINDEXRK)  /* (for debugging only) */
#define MAXINDEXRK	(BITRK - 1)   // 所能表示的最大的寄存器编号(猜测)
#endif

/* 
** 将常量索引编码为RK值 
** 即将常量编号转化为寄存器模式
*/
#define RKASK(x)	((x) | BITRK)/* 将索引x标记为常量索引 */


/*
** 无效寄存器，适合8位
*/
#define NO_REG		MAXARG_A


/*
** R(x) - 寄存器索引(一定要操作lua栈)
** Kst(x) - 常量编号模式(直接读取常量表)
** RK(x) == if ISK(x) then Kst(INDEXK(x)) else R(x)
** 表示可能是一个寄存器索引或者一个常量索引, RK只能用参数B与参数C(SIZE_B=SIZE_C=9),
** 其中参数的最高位区分是寄存器索引还是常量索引
*/


/*
** grep "ORDER OP" if you change these enums
*/
//https://blog.csdn.net/yuanlin2008/article/category/1307277
typedef enum {
/*----------------------------------------------------------------------
name		          args	  description
------------------------------------------------------------------------*/
/* 
** OP_MOVE用来将寄存器B中的值拷贝到寄存器A中. 由于Lua是register based vm, 
** 大部分的指令都是直接对寄存器进行操作, 而不需要对数据进行压栈和弹栈, 所以需要OP_MOVE指令的地方并不多.
** 最直接的使用之处就是将一个local变量复制给另一个local变量时
*/
OP_MOVE,      /*	A B     R(A) := R(B)    */
/* 
** LOADK将Bx表示的常量表中的常量值装载到寄存器A中. 
** 很多其他指令, 比如数学操作指令, 其本身可以直接从常量表中索引操作数,
** 所以可以不依赖于LOADK指令.
*/
OP_LOADK,     /*	A Bx     R(A) := Kst(Bx)   */
/* 
** 当需要生成LOADK指令时, 如果需要索引的常量id超出了Bx所能表示的有效范围, 
** 那么就生成一个LOADKX指令, 取代LOADK指令, 并且接下来立即生成一个EXTRAARG指令,
** 并用其Ax来存放这个id. 使得一个函数可以处理超过262143个常量.
*/
OP_LOADKX,    /*	A     R(A) := Kst(extra arg)    */
/* 
** LOADBOOL将B所表示的boolean值装载到寄存器A中. 
** B使用0和1分别代表false和true.
** C也表示一个boolean值, 如果C为1, 就跳过下一个指令.
*/
OP_LOADBOOL,  /*	A B C     R(A) := (Bool)B; if (C) pc++			*/
/* 
** 使A到B所表示的范围内的寄存器赋值为nil
*/
OP_LOADNIL,   /*	A B     R(A), R(A+1), ..., R(A+B) := nil		*/

/*
** 在编译期, 如果要访问变量a时, 会依照以下的顺序决定变量a的类型
** a是当前函数的local变量
** a是外层函数的local变量，那么a是当前函数的upvalue
** a是全局变量(把全局表放到最外层函数的名字为"_ENV"的upvalue中)
*/
/*
** GETUPVAL将B为索引的upvalue的值装载到A寄存器中
*/
OP_GETUPVAL,  /*	A B	    R(A) := UpValue[B]				*/
/*
** 将B为索引的upvalue当作一个table, 并将C做为索引的寄存器或者常量当作key获取的值放入寄存器A
*/
OP_GETTABUP,  /*	A B C	    R(A) := UpValue[B][RK(C)]			*/
/*
** 使用C表示的key, 将寄存器B中的表项值获取到寄存器A中
*/
OP_GETTABLE,  /*	A B C	    R(A) := R(B)[RK(C)]				*/
/* 
** 将A为索引的upvalue当作一个table, 将C寄存器或者常量的值以B寄存器或常量为key, 存入table
*/
OP_SETTABUP,  /*	A B C	    UpValue[A][RK(B)] := RK(C)			*/
/* 
** 将A寄存器的值保存到B为索引的upvalue中
*/
OP_SETUPVAL,  /*	A B	    UpValue[B] := R(A)				*/
/*
** 设置寄存器A的表的B项为C代表的值
*/
OP_SETTABLE,  /*	A B C	    R(A)[RK(B)] := RK(C)				*/
/* 
** 在寄存器A处创建一个table对象.
** B和C分别用来存储这个table数组部分和hash部分的初始大小.
** 初始大小是在编译期计算出来并生成到这个指令中的, 
** 目的是使接下来对table的初始化填充不会造成rehash而影响效率. 
** B和C使用"floating point byte"的方法来表示成(eeeeexxx)的二进制形式, 其实际值为(1xxx) * 2^(eeeee-1)
*/
OP_NEWTABLE,  /*	A B C	    R(A) := {} (size = B,C)				*/

/*
** 做好调用成员函数之前的准备, 其中待调用模块赋值到R(A+l)中, 而待调用的成员函数存放在R(A)中, 
** 待调用的模块存放在R(B)中, 待调用的函数名存放在RK(C)中
** 专门为":"运算符准备的指令.
** 专门为":"运算符准备的指令. 从寄存器B表示的table中, 获取出C作为key的closure, 存入寄存器A中, 
** 然后将table本身存入到寄存器A＋1中, 为接下来调用这个closure做准备
*/
OP_SELF,      /*	A B C	    R(A+1) := R(B); R(A) := R(B)[RK(C)]		*/

OP_ADD,       /*	A B C	    R(A) := RK(B) + RK(C)				*/
OP_SUB,       /*	A B C	    R(A) := RK(B) - RK(C)				*/
OP_MUL,       /*	A B C	    R(A) := RK(B) * RK(C)				*/
OP_MOD,       /*	A B C	    R(A) := RK(B) % RK(C)				*/
OP_POW,       /*	A B C	    R(A) := RK(B) ^ RK(C)				*/
OP_DIV,       /*	A B C	    R(A) := RK(B) / RK(C)				*/
OP_IDIV,      /*	A B C	    R(A) := RK(B) // RK(C)				*/
OP_BAND,      /*	A B C	    R(A) := RK(B) & RK(C)				*/
OP_BOR,       /*	A B C	    R(A) := RK(B) | RK(C)				*/
OP_BXOR,      /*	A B C	    R(A) := RK(B) ~ RK(C)				*/
OP_SHL,       /*	A B C	    R(A) := RK(B) << RK(C)				*/
OP_SHR,       /*	A B C	    R(A) := RK(B) >> RK(C)				*/
OP_UNM,       /*	A B	    R(A) := -R(B)					*/
OP_BNOT,      /*	A B	    R(A) := ~R(B)					*/
OP_NOT,       /*	A B	    R(A) := not R(B)				*/

/*
** 直接对应'#'操作符, 返回B对象的长度, 并保存到A中
*/
OP_LEN,       /*	A B	    R(A) := length of R(B)				*/

/*
** 将B和C指定范围内的字符串按顺序传接到一起, 将结果存入到A
对应'..'操作符, 将从R(B)到R(C)范围内的字符串按顺序连接到一起, 将结果存入到A
*/
OP_CONCAT,    /*	A B C	    R(A) := R(B).. ... ..R(C)			*/

/* 
** JMP执行一个跳转, sBx表示跳转的偏移位置, 被加到当前指向下一指令的指令指针上. 
** 如果sBx为0, 表示没有任何跳转; 1表示跳过下一个指令; -1表示重新执行当前指令. 
** 如果A>0, 表示需要关闭所有从寄存器A+1开始的所有local变量. 实际执行的关闭操作只对upvalue有效.
*/
OP_JMP,       /*	A sBx     pc+=sBx; if (A) close all upvalues >= R(A - 1)	*/
/*
** 关系指令对RK(B)和RK(C)进行比较, 然后将比较结果与A指定的boolean值进行比较, 来决定最终的boolean值.
** A在这里为每个关系指令提供了两种比较目标, 满足和不满足
** 如果RK(B)和RK(C)进行比较, 比较结果不等于A指定的boolean值, 那么递增pc指令
*/
OP_EQ,        /*	A B C     if ((RK(B) == RK(C)) ~= A) then pc++		*/
OP_LT,        /*	A B C	    if ((RK(B) <  RK(C)) ~= A) then pc++		*/
OP_LE,        /*	A B C	    if ((RK(B) <= RK(C)) ~= A) then pc++		*/

/*
** 逻辑指令用于实现and和or逻辑运算符, 或者在条件语句中判断一个寄存器. 
** TESTSET将寄存器B转化成一个boolean值, 然后与C进行比较. 如果不相等, 跳过后面的JMP指令.
** 否则将寄存器B的值赋给寄存器A, 然后继续执行.
** TEST是TESTSET的简化版, 不需要赋值操作.
*/
OP_TEST,      /*	A C	    if not (R(A) <=> C) then pc++			*/
OP_TESTSET,   /*	A B C	    if (R(B) <=> C) then R(A) := R(B) else pc++	*/

/*
** CALL执行一个函数调用. 寄存器A中存放函数对象, 所有参数按顺序放置在A后面的寄存器中.
** B－1表示参数个数. 如果参数列表的最后一个表达式是变长的, 则B会设置为0, 表示使用A＋1到当前栈顶作为参数.
** 函数调用的返回值会按顺序存放在从寄存器A开始的C-1个寄存器中. 如果C为0, 表示返回值的个数由函数决定.
*/
OP_CALL,      /*	A B C	    R(A), ... ,R(A+C-2) := R(A)(R(A+1), ... ,R(A+B-1)) */
/*
** 如果一个return statement只有一个函数调用表达式, 这个函数调用指令CALL会被改为TAILCALL指令.
** TAILCALL不会为要调用的函数增加调用堆栈的深度, 而是直接使用当前调用信息.
** ABC操作数与CALL的意思一样, 不过C永远都是0. TAILCALL在执行过程中, 只对lua closure进行tail call处理,
** 对于c closure, 其实与CALL没什么区别.
*/
OP_TAILCALL,  /*	A B C	    return R(A)(R(A+1), ... ,R(A+B-1))		*/
/*
** RETURE将返回结果存放到寄存器A到寄存器A＋B-2中.
** 如果返回的为变长表达式, 则B会被设置为0, 表示将寄存器A到当前栈顶的所有值返回.
*/
OP_RETURN,    /*	A B	    return R(A), ... ,R(A+B-2)	(see note)	*/

/* 循环 */
OP_FORLOOP,   /*	A sBx	    R(A)+=R(A+2);
			if R(A) <?= R(A+1) then { pc+=sBx; R(A+3)=R(A) }*/
OP_FORPREP,   /*	A sBx	    R(A)-=R(A+2); pc+=sBx				*/

OP_TFORCALL,  /*	A C	    R(A+3), ... ,R(A+2+C) := R(A)(R(A+1), R(A+2));	*/
OP_TFORLOOP,  /*	A sBx	    if R(A+1) ~= nil then { R(A)=R(A+1); pc += sBx }*/

/*
** SETLIST用来配合NEWTABLE, 初始化表的数组部分使用的. 
** A为保存待设置表的寄存器, SETLIST要将A下面紧接着的寄存器列表(1--B)中的值逐个设置给表的数组部分.
** 当设置的数据过多时, 则进行分批设置. 每批的数量由lopcodes.h中的LFIELDS_PER_FLUSH定义, 数量为50 
*/
OP_SETLIST,   /*	A B C	     R(A)[(C-1)*FPF+i] := R(A+i), 1 <= i <= B	*/

/*
** 为指定的函数prototype创建一个closure, 并将这个closure保存到寄存器A中. Bx用来指定函数prototype的id
*/
OP_CLOSURE,   /*	A Bx	    R(A) := closure(KPROTO[Bx])			*/
/*
** 不定参数, 对应'...'运算符. 拷贝B-1个参数到从A开始的寄存器中, 
** 如果不足, 使用nil补充. 如果B为0, 表示拷贝实际的参数数量.
*/
OP_VARARG,    /*	A B	    R(A), R(A+1), ..., R(A+B-2) = vararg		*/

OP_EXTRAARG   /*	Ax	extra (larger) argument for previous opcode	*/
} OpCode;


#define NUM_OPCODES	(cast(int, OP_EXTRAARG) + 1)

/*
** lua中所有涉及条件分支的地方, 实际上都在分支指令后紧随着一条JMP指令.
** lua没有为布尔运算单独设计操作码, 它让所有的布尔运算都以分支执行流的形式出现.
** 
*/

/*===========================================================================
  Notes:
  (*) In OP_CALL, if (B == 0) then B = top. If (C == 0), then 'top' is
  set to last_result+1, so next open instruction (OP_CALL, OP_RETURN,
  OP_SETLIST) may use 'top'.

  (*) In OP_VARARG, if (B == 0) then use actual number of varargs and
  set top (like in OP_CALL with C == 0).

  (*) In OP_RETURN, if (B == 0) then return up to 'top'.

  (*) In OP_SETLIST, if (B == 0) then B = 'top'; if (C == 0) then next
  'instruction' is EXTRAARG(real C).

  (*) In OP_LOADKX, the next 'instruction' is always EXTRAARG.

  (*) For comparisons, A specifies what condition the test should accept
  (true or false).

  (*) All 'skips' (pc++) assume that next instruction is a jump.

===========================================================================*/

/* 此部分用于luac反编译字节码时的输出, 对于lua运行期没有实际意义 */

/*
** 指令属性的掩码。 格式为：
** bits 0-1: 操作模式: OpMode {iABC, iABx, iAsBx, iAx}
** bits 2-3: C arg mode
** bits 4-5: B arg mode
** bit 6: 当前指令会修改寄存器A
** bit 7: 当前指令是否涉及一次条件跳转(下一条指令必须是跳转) 1表示涉及跳转
*/

enum OpArgMask {
    OpArgN,  /* 参数未被使用(只是没有作为R()或者RK()宏的参数使用) */
    OpArgU,  /* 已使用参数 */
    OpArgR,  /* 表示该参数是寄存器或跳转偏移 */
    OpArgK   /* 表示该参数是常盘还是寄存器, K表示常盘 */
};

LUAI_DDEC const lu_byte luaP_opmodes[NUM_OPCODES];

/* 获取操作模式 */
#define getOpMode(m)	(cast(enum OpMode, luaP_opmodes[m] & 3))
#define getBMode(m)	(cast(enum OpArgMask, (luaP_opmodes[m] >> 4) & 3))
#define getCMode(m)	(cast(enum OpArgMask, (luaP_opmodes[m] >> 2) & 3))
#define testAMode(m)	(luaP_opmodes[m] & (1 << 6))
#define testTMode(m)	(luaP_opmodes[m] & (1 << 7))


LUAI_DDEC const char *const luaP_opnames[NUM_OPCODES+1];  /* opcode names */


/* number of list items to accumulate before a SETLIST instruction */
#define LFIELDS_PER_FLUSH	50


#endif
