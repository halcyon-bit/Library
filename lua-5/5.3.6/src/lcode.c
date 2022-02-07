/*
** $Id: lcode.c,v 2.112.1.1 2017/04/19 17:20:42 roberto Exp $
** Code generator for Lua
** See Copyright Notice in lua.h
*/

#define lcode_c
#define LUA_CORE

#include "lprefix.h"


#include <math.h>
#include <stdlib.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"


/* Lua函数中的最大寄存器数(必须适合8位) */
#define MAXREGS		255


#define hasjumps(e)	((e)->t != (e)->f)


/*
** 如果表达式(e)是数字常数, 则用其值填充'v'并返回1. 否则, 返回0.
*/
static int tonumeral(const expdesc *e, TValue *v) {
	if (hasjumps(e))  /* 跳转 */
		return 0;  /* 不是数字 */
	switch (e->k) {
	case VKINT:
		if (v) setivalue(v, e->u.ival);
		return 1;
	case VKFLT:
		if (v) setfltvalue(v, e->u.nval);
		return 1;
	default: return 0;
	}
}


/*
** 创建一个 OP_LOADNIL 指令, 但是会尝试优化: 如果上一条指令也是
** OP_LOADNIL, 并且范围兼容, 则调整上条指令的范围, 而不是创建一
** 个新的OP_LOADNIL指令. (例如: 'local a; local b' 只会生成一个
** OP_LOADNIL指令.)
*/
void luaK_nil(FuncState *fs, int from, int n) {
	Instruction *previous;
	int l = from + n - 1;  /* 最后一个需要设置为nil的寄存器 */
	if (fs->pc > fs->lasttarget) {  /* no jumps to current position? */
		previous = &fs->f->code[fs->pc - 1];  /* 获取上一条指令 */
		if (GET_OPCODE(*previous) == OP_LOADNIL) {  /* 上条指令也是 LOADNIL */
			int pfrom = GETARG_A(*previous);  /* 获取上条指令的范围(起始) */
			int pl = pfrom + GETARG_B(*previous);  /* 获取上条指令的范围(结束) */
			if ((pfrom <= from && from <= pl + 1) ||
				(from <= pfrom && pfrom <= l + 1)) {  /* 如果新的范围与上条指令的范围相连 */
				if (pfrom < from) from = pfrom;  /* from = min(from, pfrom) */
				if (pl > l) l = pl;  /* l = max(l, pl) */
				SETARG_A(*previous, from);  /* 修改上条指令的范围 */
				SETARG_B(*previous, l - from);
				return;
			}
		}  /* else go through */
	}
	luaK_codeABC(fs, OP_LOADNIL, from, n - 1, 0);  /* 否则没有优化 */
}


/*
**     根据跳转指令得到sBx参数值, 但是由于sBx是相对位置, 所以还需要转换成绝对位置.
** 用于遍历跳转列表.
*/
static int getjump(FuncState *fs, int pc) {
	int offset = GETARG_sBx(fs->f->code[pc]);  /* 获取指令的sBx参数值 */
	if (offset == NO_JUMP)  /* 指向自身表示列表的结尾 */
		return NO_JUMP;  /* 列表的结尾 */
	else
		return (pc + 1) + offset;  /* 转换成绝对位置 */
}


/*
** 计算两个指令之间的偏移量作为跳转指令的sBx参数值设置进去
*/
static void fixjump(FuncState *fs, int pc, int dest) {
	Instruction *jmp = &fs->f->code[pc];
	int offset = dest - (pc + 1);  /* 计算偏移量 */
	lua_assert(dest != NO_JUMP);
	if (abs(offset) > MAXARG_sBx)
		luaX_syntaxerror(fs->ls, "control structure too long");
	/* 将偏移量写入pc位置处的指令, 用于指出下一个跳转指令的位置 */
	SETARG_sBx(*jmp, offset);
}


/*
** 跳转链表, 即最终跳转位置一样的指令形成的链表(暂时还不知道要跳转的目的地址)
** 将l2跳转指令链接到l1跳转链表后面
** l1是空悬链表的第一个指令位置, l2是待加入该链表的指令位置.
*/
void luaK_concat(FuncState *fs, int *l1, int l2) {
	/* 如果l2是NO_JUMP, 则直接返回, 因为这个位置存储的指令不是一个跳转指令 */
	if (l2 == NO_JUMP) return;  /* l2跳转链表为空 */
	else if (*l1 == NO_JUMP)  /* no original list? */
	  /*
		  ** 如果l1是NO_JUMP, 说明这个跳转链表为空, 当前没有空悬的跳转指令在该链表中,
		  ** 直接赋值为l2
		  */
		*l1 = l2;  /* 'l1' points to 'l2' */
	else {
		/*
			** l1现在是一个非空的跳转链表, 首先遍历这个链表到最后一个元素, 其判定标准是跳
			** 转位置为NO_JUMP时表示是跳转链表的最后一个元素, 然后调用fixjump函数将最后一
			** 个元素的跳转位置设置为l2, 这样l2就添加到了该跳转链表中.
			**
			** 利用了跳转指令中的跳转地址这一个参数来存储链表中下一个元素的值.
			*/
		int list = *l1;
		int next;
		while ((next = getjump(fs, list)) != NO_JUMP)  /* find last element */
			list = next;  /* 找到l1链表的最后一个元素 */
		fixjump(fs, list, l2);  /* 将l2链接到后面 */
	}
}


/*
** 生成一个新的跳转指令
** 创建一个跳转指令并返回其位置, 以便稍后修复其目标(使用'fixjump').
** 如果跳转到这个位置(保存在'jpc'中), 将它们全部链接在一起,
** 以便'patchlistaux'将它们直接固定到最终目的地.
*/
int luaK_jump(FuncState *fs) {
	/* 将待关闭的upvalue起始id A 初始化0, 跳转偏移量sbx初始化为NO_JUMP */
	int jpc = fs->jpc;  /* save list of jumps to here */
	int j;
	fs->jpc = NO_JUMP;  /* no more jumps to here */
	j = luaK_codeAsBx(fs, OP_JMP, 0, NO_JUMP);
	luaK_concat(fs, &j, jpc);  /* keep them on hold */
	return j;  /* 返回OP_JMP指令的位置 便于回填 */
}


/*
** Code a 'return' instruction
*/
void luaK_ret(FuncState *fs, int first, int nret) {
	luaK_codeABC(fs, OP_RETURN, first, nret + 1, 0);
}


/*
** Code a "conditional jump", that is, a test or comparison opcode
** followed by a jump. Return jump position.
*/
static int condjump(FuncState *fs, OpCode op, int A, int B, int C) {
	luaK_codeABC(fs, op, A, B, C);
	return luaK_jump(fs);
}


/*
** returns current 'pc' and marks it as a jump target (to avoid wrong
** optimizations with consecutive instructions not in the same basic block).
*/
int luaK_getlabel(FuncState *fs) {
	fs->lasttarget = fs->pc;
	return fs->pc;
}


/*
**返回指令“控制”给定跳转（即其条件）的位置，或者返回跳转本身（如果它是无条件的）的位置。
** Returns the position of the instruction "controlling" a given
** jump (that is, its condition), or the jump itself if it is
** unconditional.
*/
static Instruction *getjumpcontrol(FuncState *fs, int pc) {
	Instruction *pi = &fs->f->code[pc];
	if (pc >= 1 && testTMode(GET_OPCODE(*(pi - 1))))
		return pi - 1;
	else
		return pi;
}


/*
** Patch destination register for a TESTSET instruction.
** If instruction in position 'node' is not a TESTSET, return 0 ("fails").
** Otherwise, if 'reg' is not 'NO_REG', set it as the destination
** register. Otherwise, change instruction to a simple 'TEST' (produces
** no register value)
*/
static int patchtestreg(FuncState *fs, int node, int reg) {
	Instruction *i = getjumpcontrol(fs, node);
	if (GET_OPCODE(*i) != OP_TESTSET)
		return 0;  /* cannot patch other instructions */
	if (reg != NO_REG && reg != GETARG_B(*i))
		SETARG_A(*i, reg);
	else {
		/* no register to put value or register already has the value;
		   change instruction to simple test */
		*i = CREATE_ABC(OP_TEST, GETARG_B(*i), 0, GETARG_C(*i));
	}
	return 1;
}


/*
** Traverse a list of tests ensuring no one produces a value
*/
static void removevalues(FuncState *fs, int list) {
	for (; list != NO_JUMP; list = getjump(fs, list))
		patchtestreg(fs, list, NO_REG);
}


/*
** Traverse a list of tests, patching their destination address and
** registers: tests producing values jump to 'vtarget' (and put their
** values in 'reg'), other tests jump to 'dtarget'.
*/
static void patchlistaux(FuncState *fs, int list, int vtarget, int reg,
	int dtarget) {
	while (list != NO_JUMP) {
		int next = getjump(fs, list);
		if (patchtestreg(fs, list, reg))
			fixjump(fs, list, vtarget);
		else
			fixjump(fs, list, dtarget);  /* jump to default target */
		list = next;
	}
}


/*
** Ensure all pending jumps to current position are fixed (jumping
** to current position with no values) and reset list of pending
** jumps
*/
static void dischargejpc(FuncState *fs) {
	patchlistaux(fs, fs->jpc, fs->pc, NO_REG, fs->pc);
	fs->jpc = NO_JUMP;
}


/*
** 将一个链表准备回填到当前位置
** Add elements in 'list' to list of pending jumps to "here"
** (current position)
*/
void luaK_patchtohere(FuncState *fs, int list) {
	luaK_getlabel(fs);  /* mark "here" as a jump target */
	luaK_concat(fs, &fs->jpc, list);
}


/*
** 回填一个链表的跳转位置
** Path all jumps in 'list' to jump to 'target'.
** (The assert means that we cannot fix a jump to a forward address
** because we only know addresses once code is generated.)
*/
void luaK_patchlist(FuncState *fs, int list, int target) {
	if (target == fs->pc)  /* 'target' is current position? */
		luaK_patchtohere(fs, list);  /* add list to pending jumps */
	else {
		lua_assert(target < fs->pc);
		patchlistaux(fs, list, target, NO_REG, target);
	}
}


/*
** 回填一个链表中的A, 也就是需要关闭的upvalue id
** Path all jumps in 'list' to close upvalues up to given 'level'
** (The assertion checks that jumps either were closing nothing
** or were closing higher levels, from inner blocks.)
*/
void luaK_patchclose(FuncState *fs, int list, int level) {
	level++;  /* argument is +1 to reserve 0 as non-op */
	for (; list != NO_JUMP; list = getjump(fs, list)) {
		lua_assert(GET_OPCODE(fs->f->code[list]) == OP_JMP &&
			(GETARG_A(fs->f->code[list]) == 0 ||
				GETARG_A(fs->f->code[list]) >= level));
		SETARG_A(fs->f->code[list], level);
	}
}


/*
** Emit instruction 'i', checking for array sizes and saving also its
** line information. Return 'i' position.
*/
static int luaK_code(FuncState *fs, Instruction i) {
	Proto *f = fs->f;
	dischargejpc(fs);  /* 'pc' will change */
	/* put new instruction in code array */
	luaM_growvector(fs->ls->L, f->code, fs->pc, f->sizecode, Instruction,
		MAX_INT, "opcodes");
	f->code[fs->pc] = i;
	/* save corresponding line information */
	luaM_growvector(fs->ls->L, f->lineinfo, fs->pc, f->sizelineinfo, int,
		MAX_INT, "opcodes");
	f->lineinfo[fs->pc] = fs->ls->lastline;
	return fs->pc++;
}


/*
** Format and emit an 'iABC' instruction. (Assertions check consistency
** of parameters versus opcode.)
*/
int luaK_codeABC(FuncState *fs, OpCode o, int a, int b, int c) {
	lua_assert(getOpMode(o) == iABC);
	lua_assert(getBMode(o) != OpArgN || b == 0);
	lua_assert(getCMode(o) != OpArgN || c == 0);
	lua_assert(a <= MAXARG_A && b <= MAXARG_B && c <= MAXARG_C);
	return luaK_code(fs, CREATE_ABC(o, a, b, c));
}


/*
** Format and emit an 'iABx' instruction.
*/
int luaK_codeABx(FuncState *fs, OpCode o, int a, unsigned int bc) {
	lua_assert(getOpMode(o) == iABx || getOpMode(o) == iAsBx);
	lua_assert(getCMode(o) == OpArgN);
	lua_assert(a <= MAXARG_A && bc <= MAXARG_Bx);
	return luaK_code(fs, CREATE_ABx(o, a, bc));
}


/*
** Emit an "extra argument" instruction (format 'iAx')
*/
static int codeextraarg(FuncState *fs, int a) {
	lua_assert(a <= MAXARG_Ax);
	return luaK_code(fs, CREATE_Ax(OP_EXTRAARG, a));
}


/*
** Emit a "load constant" instruction, using either 'OP_LOADK'
** (if constant index 'k' fits in 18 bits) or an 'OP_LOADKX'
** instruction with "extra argument".
*/
int luaK_codek(FuncState *fs, int reg, int k) {
	if (k <= MAXARG_Bx)
		return luaK_codeABx(fs, OP_LOADK, reg, k);
	else {
		int p = luaK_codeABx(fs, OP_LOADKX, reg, 0);
		codeextraarg(fs, k);
		return p;
	}
}


/*
** Check register-stack level, keeping track of its maximum size
** in field 'maxstacksize'
*/
void luaK_checkstack(FuncState *fs, int n) {
	int newstack = fs->freereg + n;
	if (newstack > fs->f->maxstacksize) {
		if (newstack >= MAXREGS)
			luaX_syntaxerror(fs->ls,
				"function or expression needs too many registers");
		fs->f->maxstacksize = cast_byte(newstack);
	}
}


/*
** Reserve 'n' registers in register stack
*/
void luaK_reserveregs(FuncState *fs, int n) {
	luaK_checkstack(fs, n);
	fs->freereg += n;
}


/*
** 如果寄存器(reg)既不是常数索引也不是局部变量, 释放寄存器(reg)
*/
static void freereg(FuncState *fs, int reg) {
	if (!ISK(reg) && reg >= fs->nactvar) {
		fs->freereg--;
		lua_assert(reg == fs->freereg);
	}
}


/*
** 释放表达式"e"使用的寄存器(如果可能的话)
*/
static void freeexp(FuncState *fs, expdesc *e) {
	if (e->k == VNONRELOC)
		freereg(fs, e->u.info);
}


/*
** Free registers used by expressions 'e1' and 'e2' (if any) in proper
** order.
*/
static void freeexps(FuncState *fs, expdesc *e1, expdesc *e2) {
	int r1 = (e1->k == VNONRELOC) ? e1->u.info : -1;
	int r2 = (e2->k == VNONRELOC) ? e2->u.info : -1;
	if (r1 > r2) {
		freereg(fs, r1);
		freereg(fs, r2);
	}
	else {
		freereg(fs, r2);
		freereg(fs, r1);
	}
}


/*
**     将常量"v"添加到常量列表(Proto.k)中. 在 LexState 的 信息表(h)中 查找是否已经
** 存在常量"v", 若有则缓存了该常量的位置, 并尝试重用该常量. 因为某些值不应该用作键(nil
** 整数键会转化为void*作为键), 所以调用者必须提供有效的"键"来索引高速缓存.
** 可尽量避免将重复的值添加到常量表中.(可能会重复)
** 例子:
** local a = "123123"
** function func()
**     local b = "456456"
**     b = "123123"
** end
** a = "123123"
** 在最外层的函数的常量表中会有2个"123123"字符串
*/
static int addk(FuncState *fs, TValue *key, TValue *v) {
	lua_State *L = fs->ls->L;
	Proto *f = fs->f;
	TValue *idx = luaH_set(L, fs->ls->h, key);  /* index scanner table */
	int k, oldsize;
	if (ttisinteger(idx)) {  /* is there an index there? */
		k = cast_int(ivalue(idx));
		/* 判断是否是正确的id? (warning: 必须区分浮点数和整数!) */
		if (k < fs->nk && ttype(&f->k[k]) == ttype(v) &&
			luaV_rawequalobj(&f->k[k], v))
			return k;  /* 重用该常量 */
	}
	/* 没有找到该常量; 创建并加入到常量列表 */
	oldsize = f->sizek;
	k = fs->nk;
	/* 数值不需要 GC barrier; 该表没有 metatable, so it does not need to invalidate cache */
	setivalue(idx, k);
	luaM_growvector(L, f->k, k, f->sizek, TValue, MAXARG_Ax, "constants");
	while (oldsize < f->sizek) setnilvalue(&f->k[oldsize++]);
	setobj(L, &f->k[k], v);
	fs->nk++;
	luaC_barrier(L, f, v);
	return k;  /* 返回常量表中的id */
}


/*
** 向常量表(k)中添加字符串常量(s), 返回其在常量表中的id(编号).
*/
int luaK_stringK(FuncState *fs, TString *s) {
	TValue o;
	setsvalue(fs->ls->L, &o, s);
	return addk(fs, &o, &o);  /* 使用字符串作为key */
}


/*
** 将一个整数添加到常量列表中并返回其索引.
** 整数使用userdata类型作为键, 以避免与具有相同值的浮点数冲突; 转换为"void *"仅用于
** 哈希, 因此没有"精度"问题.
*/
int luaK_intK(FuncState *fs, lua_Integer n) {
	TValue k, o;
	setpvalue(&k, cast(void*, cast(size_t, n)));
	setivalue(&o, n);
	return addk(fs, &k, &o);
}

/*
** 将一个浮点数添加到常量列表中并返回其索引.
*/
static int luaK_numberK(FuncState *fs, lua_Number r) {
	TValue o;
	setfltvalue(&o, r);
	return addk(fs, &o, &o);  /* 使用浮点值作为key */
}


/*
** 将一个布尔值添加到常量列表中并返回其索引.
*/
static int boolK(FuncState *fs, int b) {
	TValue o;
	setbvalue(&o, b);
	return addk(fs, &o, &o);  /* 使用布尔值作为key */
}


/*
** 将 nil 添加到常量列表中并返回其索引.
*/
static int nilK(FuncState *fs) {
	TValue k, v;
	setnilvalue(&v);
	/* 不能使用nil作为键; 而是使用表本身来表示nil */
	sethvalue(fs->ls->L, &k, fs->ls->h);
	return addk(fs, &k, &v);
}


/*
** Fix an expression to return the number of results 'nresults'.
** Either 'e' is a multi-ret expression (function call or vararg)
** or 'nresults' is LUA_MULTRET (as any expression can satisfy that).
*/
void luaK_setreturns(FuncState *fs, expdesc *e, int nresults) {
	if (e->k == VCALL) {  /* expression is an open function call? */
		SETARG_C(getinstruction(fs, e), nresults + 1);
	}
	else if (e->k == VVARARG) {
		Instruction *pc = &getinstruction(fs, e);
		SETARG_B(*pc, nresults + 1);
		SETARG_A(*pc, fs->freereg);
		luaK_reserveregs(fs, 1);
	}
	else lua_assert(nresults == LUA_MULTRET);
}


/*
** Fix an expression to return one result.
** If expression is not a multi-ret expression (function call or
** vararg), it already returns one result, so nothing needs to be done.
** Function calls become VNONRELOC expressions (as its result comes
** fixed in the base register of the call), while vararg expressions
** become VRELOCABLE (as OP_VARARG puts its results where it wants).
** (Calls are created returning one result, so that does not need
** to be fixed.)
*/
void luaK_setoneret(FuncState *fs, expdesc *e) {
	if (e->k == VCALL) {  /* expression is an open function call? */
	  /* already returns 1 value */
		lua_assert(GETARG_C(getinstruction(fs, e)) == 2);
		e->k = VNONRELOC;  /* result has fixed position */
		e->u.info = GETARG_A(getinstruction(fs, e));
	}
	else if (e->k == VVARARG) {
		SETARG_B(getinstruction(fs, e), 2);
		e->k = VRELOCABLE;  /* can relocate its simple result */
	}
}


/*
** Ensure that expression 'e' is not a variable.
*/
/*
** 为什么需要重定向, 因为先生成了指令, 并不知道指令的具体参数.(自己的理解)
** 而 VLOCAL 并没有生成指令.
*/
void luaK_dischargevars(FuncState *fs, expdesc *e) {
	switch (e->k) {
	case VLOCAL: {  /* already in a register */
	  /*
	  **     变量是VLOCAL, 说明前面已经看见过这个变量, 那么它既不需要重定
	  ** 向, 也不需要额外的语句把这个值加载进来. 所有局部变量都会在函数栈中
	  ** 有一个对应的位置
	  */
		e->k = VNONRELOC;  /* becomes a non-relocatable value */
		break;
	}
	case VUPVAL: {  /* move value to some (pending) register */
		e->u.info = luaK_codeABC(fs, OP_GETUPVAL, 0, e->u.info, 0);
		e->k = VRELOCABLE;
		break;
	}
	case VINDEXED: {
		/*
		**     需要重定向, 先生成 OP_GETTABLE 或 OP_GETTABUP 指令, 获取t[idx]
		** 的值, 指令的A参数目前为止0, 因为这个参数保存的是获取这个变量的值之
		** 后需要存放到的寄存器地址, 此时并不知道.
		**     之所以需要重定向, 是因为当生成该指令时, 并不知道当前可用的寄存器
		** 地址是什么, 即并不知道这个变量的值最后将加载到栈的哪个位置. 也就是
		** 该指令中的参数A在生成该指令时还是未知的, 需要到后面的discharge2reg
		** 函数中才知道, 因为寄存器地址作为这个函数的参数传入了.
		*/
		OpCode op;
		freereg(fs, e->u.ind.idx);
		if (e->u.ind.vt == VLOCAL) {  /* is 't' in a register? */
			freereg(fs, e->u.ind.t);
			op = OP_GETTABLE;
		}
		else {
			lua_assert(e->u.ind.vt == VUPVAL);
			op = OP_GETTABUP;  /* 't' is in an upvalue */
		}
		e->u.info = luaK_codeABC(fs, op, 0, e->u.ind.t, e->u.ind.idx);
		e->k = VRELOCABLE;
		break;
	}
	case VVARARG: case VCALL: {
		luaK_setoneret(fs, e);
		break;
	}
	default: break;  /* there is one value available (somewhere) */
	}
}


/*
** Ensures expression value is in register 'reg' (and therefore
** 'e' will become a non-relocatable expression).
*/
static void discharge2reg(FuncState *fs, expdesc *e, int reg) {
	/* 解析表达式(e), 判断是否需要重定向 */
	luaK_dischargevars(fs, e);
	switch (e->k) {
	case VNIL: {
		luaK_nil(fs, reg, 1);
		break;
	}
	case VFALSE: case VTRUE: {
		luaK_codeABC(fs, OP_LOADBOOL, reg, e->k == VTRUE, 0);
		break;
	}
	case VK: {
		luaK_codek(fs, reg, e->u.info);
		break;
	}
	case VKFLT: {  /* 生成 LOADK 指令, 将整数加载到reg对应的寄存器中 */
		luaK_codek(fs, reg, luaK_numberK(fs, e->u.nval));
		break;
	}
	case VKINT: {  /* 生成 LOADK 指令, 将整数加载到reg对应的寄存器中 */
		luaK_codek(fs, reg, luaK_intK(fs, e->u.ival));
		break;
	}
	case VRELOCABLE: {  /* 重定向, 根据reg参数来写入这个指令的参数A */
		Instruction *pc = &getinstruction(fs, e);
		SETARG_A(*pc, reg);  /* instruction will put result in 'reg' */
		break;
	}
	case VNONRELOC: {  /* 表达式类型是VNONRELOC, 也就是不需要重定位, 直接生成MOV指令 */
		if (reg != e->u.info)
			luaK_codeABC(fs, OP_MOVE, reg, e->u.info, 0);
		break;
	}
	default: {
		lua_assert(e->k == VJMP);
		return;  /* nothing to do... */
	}
	}
	e->u.info = reg;
	e->k = VNONRELOC;
}


/*
** Ensures expression value is in any register.
*/
static void discharge2anyreg(FuncState *fs, expdesc *e) {
	if (e->k != VNONRELOC) {  /* no fixed register yet? */
		luaK_reserveregs(fs, 1);  /* get a register */
		discharge2reg(fs, e, fs->freereg - 1);  /* put value there */
	}
}


static int code_loadbool(FuncState *fs, int A, int b, int jump) {
	luaK_getlabel(fs);  /* those instructions may be jump targets */
	return luaK_codeABC(fs, OP_LOADBOOL, A, b, jump);
}


/*
** check whether list has any jump that do not produce a value
** or produce an inverted value
*/
static int need_value(FuncState *fs, int list) {
	for (; list != NO_JUMP; list = getjump(fs, list)) {
		Instruction i = *getjumpcontrol(fs, list);
		if (GET_OPCODE(i) != OP_TESTSET) return 1;
	}
	return 0;  /* not found */
}


/*
** Ensures final expression result (including results from its jump
** lists) is in register 'reg'.
** If expression has jumps, need to patch these jumps either to
** its final position or to "load" instructions (for those tests
** that do not produce values).
*/
static void exp2reg(FuncState *fs, expdesc *e, int reg) {
	discharge2reg(fs, e, reg);
	if (e->k == VJMP)  /* expression itself is a test? */
		luaK_concat(fs, &e->t, e->u.info);  /* put this jump in 't' list */
	if (hasjumps(e)) {
		int final;  /* position after whole expression */
		int p_f = NO_JUMP;  /* position of an eventual LOAD false */
		int p_t = NO_JUMP;  /* position of an eventual LOAD true */
		if (need_value(fs, e->t) || need_value(fs, e->f)) {
			int fj = (e->k == VJMP) ? NO_JUMP : luaK_jump(fs);
			p_f = code_loadbool(fs, reg, 0, 1);
			p_t = code_loadbool(fs, reg, 1, 0);
			luaK_patchtohere(fs, fj);
		}
		final = luaK_getlabel(fs);
		patchlistaux(fs, e->f, final, reg, p_f);
		patchlistaux(fs, e->t, final, reg, p_t);
	}
	e->f = e->t = NO_JUMP;
	e->u.info = reg;
	e->k = VNONRELOC;
}


/*
** 确保最终表达式结果(包括其跳转列表的结果)在下一个可用寄存器中.
** 1. 调用 luaK_dischargevars 函数, 根据变量所在的不同作用域(local, global, upvalue)
** 来决定这个变量是否需要重定向.
** 2. 调用 luaK_reserveregs 函数, 分配可用的函数寄存器空间, 得到这个空间对应的寄存器
** 索引, 有了空间, 才能存储变量
** 3. 调用 exp2reg 函数, 真正完成把表达式的数据存放入寄存器空间的工作. 在这个函数中,
** 最终又会调用 discharge2reg 函数, 这个函数根据不同的表达式类型(NIL, 布尔表达式, 数
** 字等)来生成存取表达式的值到寄存器的字节码.
*/
void luaK_exp2nextreg(FuncState *fs, expdesc *e) {
	luaK_dischargevars(fs, e);
	freeexp(fs, e);
	luaK_reserveregs(fs, 1);
	exp2reg(fs, e, fs->freereg - 1);
}


/*
** Ensures final expression result (including results from its jump
** lists) is in some (any) register and return that register.
** 确保最终表达式结果(包括其跳转列表的结果)在某些(任意)寄存器中, 并返回该寄存器
*/
int luaK_exp2anyreg(FuncState *fs, expdesc *e) {
	/* 解析表达式是否需要重定位 */
	luaK_dischargevars(fs, e);
	if (e->k == VNONRELOC) {  /* 表达式已经有一个寄存器? */
		if (!hasjumps(e))  /* 无跳转? */
			return e->u.info;  /* 结果已经在寄存器中 */
		if (e->u.info >= fs->nactvar) {  /* reg. is not a local? */
			exp2reg(fs, e, e->u.info);  /* put final result in it */
			return e->u.info;
		}
	}
	luaK_exp2nextreg(fs, e);  /* 否则, 请使用下一个可用的寄存器 */
	return e->u.info;
}


/*
** Ensures final expression result is either in a register or in an
** upvalue.
*/
void luaK_exp2anyregup(FuncState *fs, expdesc *e) {
	if (e->k != VUPVAL || hasjumps(e))
		luaK_exp2anyreg(fs, e);
}


/*
** Ensures final expression result is either in a register or it is
** a constant.
*/
void luaK_exp2val(FuncState *fs, expdesc *e) {
	if (hasjumps(e))
		luaK_exp2anyreg(fs, e);
	else
		luaK_dischargevars(fs, e);
}


/*
** Ensures final expression result is in a valid R/K index
** (that is, it is either in a register or in 'k' with an index
** in the range of R/K indices).
** Returns R/K index.
*/
int luaK_exp2RK(FuncState *fs, expdesc *e) {
	luaK_exp2val(fs, e);
	switch (e->k) {  /* move constants to 'k' */
	case VTRUE: e->u.info = boolK(fs, 1); goto vk;
	case VFALSE: e->u.info = boolK(fs, 0); goto vk;
	case VNIL: e->u.info = nilK(fs); goto vk;
	case VKINT: e->u.info = luaK_intK(fs, e->u.ival); goto vk;
	case VKFLT: e->u.info = luaK_numberK(fs, e->u.nval); goto vk;
	case VK:
	vk:
		e->k = VK;
		if (e->u.info <= MAXINDEXRK)  /* constant fits in 'argC'? */
			return RKASK(e->u.info);
		else break;
	default: break;
	}
	/* not a constant in the right range: put it in a register */
	return luaK_exp2anyreg(fs, e);
}


/*
** Generate code to store result of expression 'ex' into variable 'var'.
*/
void luaK_storevar(FuncState *fs, expdesc *var, expdesc *ex) {
	switch (var->k) {
	case VLOCAL: {
		freeexp(fs, ex);
		exp2reg(fs, ex, var->u.info);  /* compute 'ex' into proper place */
		return;
	}
	case VUPVAL: {
		int e = luaK_exp2anyreg(fs, ex);
		luaK_codeABC(fs, OP_SETUPVAL, e, var->u.info, 0);
		break;
	}
	case VINDEXED: {
		OpCode op = (var->u.ind.vt == VLOCAL) ? OP_SETTABLE : OP_SETTABUP;
		int e = luaK_exp2RK(fs, ex);
		luaK_codeABC(fs, op, var->u.ind.t, var->u.ind.idx, e);
		break;
	}
	default: lua_assert(0);  /* invalid var kind to store */
	}
	freeexp(fs, ex);
}


/*
** Emit SELF instruction (convert expression 'e' into 'e:key(e,').
*/
void luaK_self(FuncState *fs, expdesc *e, expdesc *key) {
	int ereg;
	luaK_exp2anyreg(fs, e);
	ereg = e->u.info;  /* register where 'e' was placed */
	freeexp(fs, e);
	e->u.info = fs->freereg;  /* base register for op_self */
	e->k = VNONRELOC;  /* self expression has a fixed register */
	luaK_reserveregs(fs, 2);  /* function and 'self' produced by op_self */
	luaK_codeABC(fs, OP_SELF, e->u.info, ereg, luaK_exp2RK(fs, key));
	freeexp(fs, key);
}


/*
** Negate condition 'e' (where 'e' is a comparison).
*/
static void negatecondition(FuncState *fs, expdesc *e) {
	Instruction *pc = getjumpcontrol(fs, e->u.info);
	lua_assert(testTMode(GET_OPCODE(*pc)) && GET_OPCODE(*pc) != OP_TESTSET &&
		GET_OPCODE(*pc) != OP_TEST);
	SETARG_A(*pc, !(GETARG_A(*pc)));
}


/*
** Emit instruction to jump if 'e' is 'cond' (that is, if 'cond'
** is true, code will jump if 'e' is true.) Return jump position.
** Optimize when 'e' is 'not' something, inverting the condition
** and removing the 'not'.
*/
static int jumponcond(FuncState *fs, expdesc *e, int cond) {
	if (e->k == VRELOCABLE) {
		Instruction ie = getinstruction(fs, e);
		if (GET_OPCODE(ie) == OP_NOT) {
			fs->pc--;  /* remove previous OP_NOT */
			return condjump(fs, OP_TEST, GETARG_B(ie), 0, !cond);
		}
		/* else go through */
	}
	discharge2anyreg(fs, e);
	freeexp(fs, e);
	return condjump(fs, OP_TESTSET, NO_REG, e->u.info, cond);
}


/*
** Emit code to go through if 'e' is true, jump otherwise.
*/
void luaK_goiftrue(FuncState *fs, expdesc *e) {
	int pc;  /* pc of new jump */
	luaK_dischargevars(fs, e);
	switch (e->k) {
	case VJMP: {  /* condition? */
		negatecondition(fs, e);  /* jump when it is false */
		pc = e->u.info;  /* save jump position */
		break;
	}
	case VK: case VKFLT: case VKINT: case VTRUE: {
		pc = NO_JUMP;  /* always true; do nothing */
		break;
	}
	default: {
		pc = jumponcond(fs, e, 0);  /* jump when false */
		break;
	}
	}
	luaK_concat(fs, &e->f, pc);  /* insert new jump in false list */
	luaK_patchtohere(fs, e->t);  /* true list jumps to here (to go through) */
	e->t = NO_JUMP;
}


/*
** Emit code to go through if 'e' is false, jump otherwise.
*/
void luaK_goiffalse(FuncState *fs, expdesc *e) {
	int pc;  /* pc of new jump */
	luaK_dischargevars(fs, e);
	switch (e->k) {
	case VJMP: {
		pc = e->u.info;  /* already jump if true */
		break;
	}
	case VNIL: case VFALSE: {
		pc = NO_JUMP;  /* always false; do nothing */
		break;
	}
	default: {
		pc = jumponcond(fs, e, 1);  /* jump if true */
		break;
	}
	}
	luaK_concat(fs, &e->t, pc);  /* insert new jump in 't' list */
	luaK_patchtohere(fs, e->f);  /* false list jumps to here (to go through) */
	e->f = NO_JUMP;
}


/*
** Code 'not e', doing constant folding.
*/
static void codenot(FuncState *fs, expdesc *e) {
	luaK_dischargevars(fs, e);
	switch (e->k) {
	case VNIL: case VFALSE: {
		e->k = VTRUE;  /* true == not nil == not false */
		break;
	}
	case VK: case VKFLT: case VKINT: case VTRUE: {
		e->k = VFALSE;  /* false == not "x" == not 0.5 == not 1 == not true */
		break;
	}
	case VJMP: {
		negatecondition(fs, e);
		break;
	}
	case VRELOCABLE:
	case VNONRELOC: {
		discharge2anyreg(fs, e);
		freeexp(fs, e);
		e->u.info = luaK_codeABC(fs, OP_NOT, 0, e->u.info, 0);
		e->k = VRELOCABLE;
		break;
	}
	default: lua_assert(0);  /* cannot happen */
	}
	/* interchange true and false lists */
	{ int temp = e->f; e->f = e->t; e->t = temp; }
	removevalues(fs, e->f);  /* values are useless when negated */
	removevalues(fs, e->t);
}


/*
** 创建表达式"t[k]". "t"必须已将其最终结果存储在寄存器或upvalue中.
** 创建 VINDEXED 的表达式, t为表的id, idx为键(key)的编号(寄存器或常量编号)
** vt 为表的类型(局部 or upvalue)
*/
void luaK_indexed(FuncState *fs, expdesc *t, expdesc *k) {
	lua_assert(!hasjumps(t) && (vkisinreg(t->k) || t->k == VUPVAL));
	t->u.ind.t = t->u.info;  /* register or upvalue index */
	t->u.ind.idx = luaK_exp2RK(fs, k);  /* R/K index for key */
	t->u.ind.vt = (t->k == VUPVAL) ? VUPVAL : VLOCAL;
	t->k = VINDEXED;
}


/*
**     在进行常量展开(constfolding)操作时, 判断两个操作数的有效性; 按位运算需要转换为
** 整数的操作数, 除法运算的除数不能为0. 如果常量展开会引发错误, 则返回false.
*/
static int validop(int op, TValue *v1, TValue *v2) {
	switch (op) {
	case LUA_OPBAND: case LUA_OPBOR: case LUA_OPBXOR:
	case LUA_OPSHL: case LUA_OPSHR: case LUA_OPBNOT: {  /* 转换为整形 */
		lua_Integer i;
		return (tointeger(v1, &i) && tointeger(v2, &i));
	}
	case LUA_OPDIV: case LUA_OPIDIV: case LUA_OPMOD:  /* 除数不能为零 */
		return (nvalue(v2) != 0);
	default: return 1;  /* 其他情况都合法 */
	}
}


/*
** 对于二元计算时, lua做了一些优化
**     在判断操作符两个对象都是常量数字时, 会进行常量展开("constant-fold")操作, 即在
** 计算时将它们的结果计算好, 再赋值给相应的变量, 例如: local a = 4 + 7
**     但是并不是任何时候只要操作对象是常量, 都可以进行常量展开操作, 还要考虑到操作的
** 优先级. 例如: local a = b + 4 + 7
**     如果成功, 返回1. 最终结果保存在表达式(e1)中.
*/
static int constfolding(FuncState *fs, int op, expdesc *e1,
	const expdesc *e2) {
	TValue v1, v2, res;
	if (!tonumeral(e1, &v1) || !tonumeral(e2, &v2) || !validop(op, &v1, &v2))
		return 0;  /* 非数字操作数或不安全操作 */
	luaO_arith(fs->ls->L, op, &v1, &v2, &res);  /* 进行相应的二元运算 */
	if (ttisinteger(&res)) {
		e1->k = VKINT;
		e1->u.ival = ivalue(&res);
	}
	else {  /* folds neither NaN nor 0.0 (to avoid problems with -0.0) */
		lua_Number n = fltvalue(&res);
		if (luai_numisnan(n) || n == 0)
			return 0;
		e1->k = VKFLT;
		e1->u.nval = n;
	}
	return 1;
}


/*
** Emit code for unary expressions that "produce values"
** (everything but 'not').
** Expression to produce final result will be encoded in 'e'.
** **为“产生值”（所有但不是）的一元表达式发出代码。 产生最终结果的表达式将被编码为“ e”。
*/
static void codeunexpval(FuncState *fs, OpCode op, expdesc *e, int line) {
	int r = luaK_exp2anyreg(fs, e);  /* 表达式(e)生成对应的操作码, 返回对应的寄存器 */
	freeexp(fs, e);
	e->u.info = luaK_codeABC(fs, op, 0, r, 0);  /* 生成操作码 */
	e->k = VRELOCABLE;  /* 所有这些操作都需要重定位 */
	luaK_fixline(fs, line);
}


/*
** Emit code for binary expressions that "produce values"
** (everything but logical operators 'and'/'or' and comparison
** operators).
** Expression to produce final result will be encoded in 'e1'.
** Because 'luaK_exp2RK' can free registers, its calls must be
** in "stack order" (that is, first on 'e2', which may have more
** recent registers to be released).
*/
static void codebinexpval(FuncState *fs, OpCode op,
	expdesc *e1, expdesc *e2, int line) {
	int rk2 = luaK_exp2RK(fs, e2);  /* both operands are "RK" */
	int rk1 = luaK_exp2RK(fs, e1);
	freeexps(fs, e1, e2);
	e1->u.info = luaK_codeABC(fs, op, 0, rk1, rk2);  /* generate opcode */
	e1->k = VRELOCABLE;  /* all those operations are relocatable */
	luaK_fixline(fs, line);
}


/*
** Emit code for comparisons.
** 'e1' was already put in R/K form by 'luaK_infix'.
*/
static void codecomp(FuncState *fs, BinOpr opr, expdesc *e1, expdesc *e2) {
	int rk1 = (e1->k == VK) ? RKASK(e1->u.info)
		: check_exp(e1->k == VNONRELOC, e1->u.info);
	int rk2 = luaK_exp2RK(fs, e2);
	freeexps(fs, e1, e2);
	switch (opr) {
	case OPR_NE: {  /* '(a ~= b)' ==> 'not (a == b)' */
		e1->u.info = condjump(fs, OP_EQ, 0, rk1, rk2);
		break;
	}
	case OPR_GT: case OPR_GE: {
		/* '(a > b)' ==> '(b < a)';  '(a >= b)' ==> '(b <= a)' */
		OpCode op = cast(OpCode, (opr - OPR_NE) + OP_EQ);
		e1->u.info = condjump(fs, op, 1, rk2, rk1);  /* invert operands */
		break;
	}
	default: {  /* '==', '<', '<=' use their own opcodes */
		OpCode op = cast(OpCode, (opr - OPR_EQ) + OP_EQ);
		e1->u.info = condjump(fs, op, 1, rk1, rk2);
		break;
	}
	}
	e1->k = VJMP;
}


/*
** 将前缀运算"op"应用于表达式"e"
*/
void luaK_prefix(FuncState *fs, UnOpr op, expdesc *e, int line) {
	static const expdesc ef = { VKINT, {0}, NO_JUMP, NO_JUMP };
	switch (op) {
	case OPR_MINUS: case OPR_BNOT:  /* 使用'ef'作为伪第二操作数 */
		if (constfolding(fs, op + LUA_OPUNM, e, &ef))  /* 进行常量展开操作 */
			break;
		/* FALLTHROUGH */
	case OPR_LEN:
		/* 生成对应的指令码 */
		codeunexpval(fs, cast(OpCode, op + OP_UNM), e, line);
		break;
	case OPR_NOT: codenot(fs, e); break;
	default: lua_assert(0);
	}
}


/*
** Process 1st operand 'v' of binary operation 'op' before reading
** 2nd operand.
*/
void luaK_infix(FuncState *fs, BinOpr op, expdesc *v) {
	switch (op) {
	case OPR_AND: {
		luaK_goiftrue(fs, v);  /* go ahead only if 'v' is true */
		break;
	}
	case OPR_OR: {
		luaK_goiffalse(fs, v);  /* go ahead only if 'v' is false */
		break;
	}
	case OPR_CONCAT: {
		luaK_exp2nextreg(fs, v);  /* operand must be on the 'stack' */
		break;
	}
	case OPR_ADD: case OPR_SUB:
	case OPR_MUL: case OPR_DIV: case OPR_IDIV:
	case OPR_MOD: case OPR_POW:
	case OPR_BAND: case OPR_BOR: case OPR_BXOR:
	case OPR_SHL: case OPR_SHR: {
		if (!tonumeral(v, NULL))
			luaK_exp2RK(fs, v);
		/* else keep numeral, which may be folded with 2nd operand */
		break;
	}
	default: {
		luaK_exp2RK(fs, v);
		break;
	}
	}
}


/*
** Finalize code for binary operation, after reading 2nd operand.
** For '(a .. b .. c)' (which is '(a .. (b .. c))', because
** concatenation is right associative), merge second CONCAT into first
** one.
*/
void luaK_posfix(FuncState *fs, BinOpr op,
	expdesc *e1, expdesc *e2, int line) {
	switch (op) {
	case OPR_AND: {
		lua_assert(e1->t == NO_JUMP);  /* list closed by 'luK_infix' */
		luaK_dischargevars(fs, e2);
		luaK_concat(fs, &e2->f, e1->f);
		*e1 = *e2;
		break;
	}
	case OPR_OR: {
		lua_assert(e1->f == NO_JUMP);  /* list closed by 'luK_infix' */
		luaK_dischargevars(fs, e2);
		luaK_concat(fs, &e2->t, e1->t);
		*e1 = *e2;
		break;
	}
	case OPR_CONCAT: {
		luaK_exp2val(fs, e2);
		if (e2->k == VRELOCABLE &&
			GET_OPCODE(getinstruction(fs, e2)) == OP_CONCAT) {
			lua_assert(e1->u.info == GETARG_B(getinstruction(fs, e2)) - 1);
			freeexp(fs, e1);
			SETARG_B(getinstruction(fs, e2), e1->u.info);
			e1->k = VRELOCABLE; e1->u.info = e2->u.info;
		}
		else {
			luaK_exp2nextreg(fs, e2);  /* operand must be on the 'stack' */
			codebinexpval(fs, OP_CONCAT, e1, e2, line);
		}
		break;
	}
	case OPR_ADD: case OPR_SUB: case OPR_MUL: case OPR_DIV:
	case OPR_IDIV: case OPR_MOD: case OPR_POW:
	case OPR_BAND: case OPR_BOR: case OPR_BXOR:
	case OPR_SHL: case OPR_SHR: {
		//      if (!constfolding(fs, op + LUA_OPADD, e1, e2))
		codebinexpval(fs, cast(OpCode, op + OP_ADD), e1, e2, line);
		break;
	}
	case OPR_EQ: case OPR_LT: case OPR_LE:
	case OPR_NE: case OPR_GT: case OPR_GE: {
		codecomp(fs, op, e1, e2);
		break;
	}
	default: lua_assert(0);
	}
}


/*
** Change line information associated with current position.
*/
void luaK_fixline(FuncState *fs, int line) {
	fs->f->lineinfo[fs->pc - 1] = line;
}


/*
** Emit a SETLIST instruction.
** 'base' is register that keeps table;
** 'nelems' is #table plus those to be stored now;
** 'tostore' is number of values (in registers 'base + 1',...) to add to
** table (or LUA_MULTRET to add up to stack top).
*/
void luaK_setlist(FuncState *fs, int base, int nelems, int tostore) {
	int c = (nelems - 1) / LFIELDS_PER_FLUSH + 1;
	int b = (tostore == LUA_MULTRET) ? 0 : tostore;
	lua_assert(tostore != 0 && tostore <= LFIELDS_PER_FLUSH);
	if (c <= MAXARG_C)
		luaK_codeABC(fs, OP_SETLIST, base, b, c);
	else if (c <= MAXARG_Ax) {
		luaK_codeABC(fs, OP_SETLIST, base, b, 0);
		codeextraarg(fs, c);
	}
	else
		luaX_syntaxerror(fs->ls, "constructor too long");
	fs->freereg = base + 1;  /* free registers with list values */
}

