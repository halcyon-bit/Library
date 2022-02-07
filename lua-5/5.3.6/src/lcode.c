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


/* Lua�����е����Ĵ�����(�����ʺ�8λ) */
#define MAXREGS		255


#define hasjumps(e)	((e)->t != (e)->f)


/*
** ������ʽ(e)�����ֳ���, ������ֵ���'v'������1. ����, ����0.
*/
static int tonumeral(const expdesc *e, TValue *v) {
	if (hasjumps(e))  /* ��ת */
		return 0;  /* �������� */
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
** ����һ�� OP_LOADNIL ָ��, ���ǻ᳢���Ż�: �����һ��ָ��Ҳ��
** OP_LOADNIL, ���ҷ�Χ����, ���������ָ��ķ�Χ, �����Ǵ���һ
** ���µ�OP_LOADNILָ��. (����: 'local a; local b' ֻ������һ��
** OP_LOADNILָ��.)
*/
void luaK_nil(FuncState *fs, int from, int n) {
	Instruction *previous;
	int l = from + n - 1;  /* ���һ����Ҫ����Ϊnil�ļĴ��� */
	if (fs->pc > fs->lasttarget) {  /* no jumps to current position? */
		previous = &fs->f->code[fs->pc - 1];  /* ��ȡ��һ��ָ�� */
		if (GET_OPCODE(*previous) == OP_LOADNIL) {  /* ����ָ��Ҳ�� LOADNIL */
			int pfrom = GETARG_A(*previous);  /* ��ȡ����ָ��ķ�Χ(��ʼ) */
			int pl = pfrom + GETARG_B(*previous);  /* ��ȡ����ָ��ķ�Χ(����) */
			if ((pfrom <= from && from <= pl + 1) ||
				(from <= pfrom && pfrom <= l + 1)) {  /* ����µķ�Χ������ָ��ķ�Χ���� */
				if (pfrom < from) from = pfrom;  /* from = min(from, pfrom) */
				if (pl > l) l = pl;  /* l = max(l, pl) */
				SETARG_A(*previous, from);  /* �޸�����ָ��ķ�Χ */
				SETARG_B(*previous, l - from);
				return;
			}
		}  /* else go through */
	}
	luaK_codeABC(fs, OP_LOADNIL, from, n - 1, 0);  /* ����û���Ż� */
}


/*
**     ������תָ��õ�sBx����ֵ, ��������sBx�����λ��, ���Ի���Ҫת���ɾ���λ��.
** ���ڱ�����ת�б�.
*/
static int getjump(FuncState *fs, int pc) {
	int offset = GETARG_sBx(fs->f->code[pc]);  /* ��ȡָ���sBx����ֵ */
	if (offset == NO_JUMP)  /* ָ�������ʾ�б�Ľ�β */
		return NO_JUMP;  /* �б�Ľ�β */
	else
		return (pc + 1) + offset;  /* ת���ɾ���λ�� */
}


/*
** ��������ָ��֮���ƫ������Ϊ��תָ���sBx����ֵ���ý�ȥ
*/
static void fixjump(FuncState *fs, int pc, int dest) {
	Instruction *jmp = &fs->f->code[pc];
	int offset = dest - (pc + 1);  /* ����ƫ���� */
	lua_assert(dest != NO_JUMP);
	if (abs(offset) > MAXARG_sBx)
		luaX_syntaxerror(fs->ls, "control structure too long");
	/* ��ƫ����д��pcλ�ô���ָ��, ����ָ����һ����תָ���λ�� */
	SETARG_sBx(*jmp, offset);
}


/*
** ��ת����, ��������תλ��һ����ָ���γɵ�����(��ʱ����֪��Ҫ��ת��Ŀ�ĵ�ַ)
** ��l2��תָ�����ӵ�l1��ת�������
** l1�ǿ�������ĵ�һ��ָ��λ��, l2�Ǵ�����������ָ��λ��.
*/
void luaK_concat(FuncState *fs, int *l1, int l2) {
	/* ���l2��NO_JUMP, ��ֱ�ӷ���, ��Ϊ���λ�ô洢��ָ���һ����תָ�� */
	if (l2 == NO_JUMP) return;  /* l2��ת����Ϊ�� */
	else if (*l1 == NO_JUMP)  /* no original list? */
	  /*
		  ** ���l1��NO_JUMP, ˵�������ת����Ϊ��, ��ǰû�п�������תָ���ڸ�������,
		  ** ֱ�Ӹ�ֵΪl2
		  */
		*l1 = l2;  /* 'l1' points to 'l2' */
	else {
		/*
			** l1������һ���ǿյ���ת����, ���ȱ�������������һ��Ԫ��, ���ж���׼����
			** תλ��ΪNO_JUMPʱ��ʾ����ת��������һ��Ԫ��, Ȼ�����fixjump���������һ
			** ��Ԫ�ص���תλ������Ϊl2, ����l2����ӵ��˸���ת������.
			**
			** ��������תָ���е���ת��ַ��һ���������洢��������һ��Ԫ�ص�ֵ.
			*/
		int list = *l1;
		int next;
		while ((next = getjump(fs, list)) != NO_JUMP)  /* find last element */
			list = next;  /* �ҵ�l1��������һ��Ԫ�� */
		fixjump(fs, list, l2);  /* ��l2���ӵ����� */
	}
}


/*
** ����һ���µ���תָ��
** ����һ����תָ�������λ��, �Ա��Ժ��޸���Ŀ��(ʹ��'fixjump').
** �����ת�����λ��(������'jpc'��), ������ȫ��������һ��,
** �Ա�'patchlistaux'������ֱ�ӹ̶�������Ŀ�ĵ�.
*/
int luaK_jump(FuncState *fs) {
	/* �����رյ�upvalue��ʼid A ��ʼ��0, ��תƫ����sbx��ʼ��ΪNO_JUMP */
	int jpc = fs->jpc;  /* save list of jumps to here */
	int j;
	fs->jpc = NO_JUMP;  /* no more jumps to here */
	j = luaK_codeAsBx(fs, OP_JMP, 0, NO_JUMP);
	luaK_concat(fs, &j, jpc);  /* keep them on hold */
	return j;  /* ����OP_JMPָ���λ�� ���ڻ��� */
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
**����ָ����ơ�������ת��������������λ�ã����߷�����ת������������������ģ���λ�á�
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
** ��һ������׼�������ǰλ��
** Add elements in 'list' to list of pending jumps to "here"
** (current position)
*/
void luaK_patchtohere(FuncState *fs, int list) {
	luaK_getlabel(fs);  /* mark "here" as a jump target */
	luaK_concat(fs, &fs->jpc, list);
}


/*
** ����һ���������תλ��
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
** ����һ�������е�A, Ҳ������Ҫ�رյ�upvalue id
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
** ����Ĵ���(reg)�Ȳ��ǳ�������Ҳ���Ǿֲ�����, �ͷżĴ���(reg)
*/
static void freereg(FuncState *fs, int reg) {
	if (!ISK(reg) && reg >= fs->nactvar) {
		fs->freereg--;
		lua_assert(reg == fs->freereg);
	}
}


/*
** �ͷű��ʽ"e"ʹ�õļĴ���(������ܵĻ�)
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
**     ������"v"��ӵ������б�(Proto.k)��. �� LexState �� ��Ϣ��(h)�� �����Ƿ��Ѿ�
** ���ڳ���"v", �����򻺴��˸ó�����λ��, ���������øó���. ��ΪĳЩֵ��Ӧ��������(nil
** ��������ת��Ϊvoid*��Ϊ��), ���Ե����߱����ṩ��Ч��"��"���������ٻ���.
** �ɾ������⽫�ظ���ֵ��ӵ���������.(���ܻ��ظ�)
** ����:
** local a = "123123"
** function func()
**     local b = "456456"
**     b = "123123"
** end
** a = "123123"
** �������ĺ����ĳ������л���2��"123123"�ַ���
*/
static int addk(FuncState *fs, TValue *key, TValue *v) {
	lua_State *L = fs->ls->L;
	Proto *f = fs->f;
	TValue *idx = luaH_set(L, fs->ls->h, key);  /* index scanner table */
	int k, oldsize;
	if (ttisinteger(idx)) {  /* is there an index there? */
		k = cast_int(ivalue(idx));
		/* �ж��Ƿ�����ȷ��id? (warning: �������ָ�����������!) */
		if (k < fs->nk && ttype(&f->k[k]) == ttype(v) &&
			luaV_rawequalobj(&f->k[k], v))
			return k;  /* ���øó��� */
	}
	/* û���ҵ��ó���; ���������뵽�����б� */
	oldsize = f->sizek;
	k = fs->nk;
	/* ��ֵ����Ҫ GC barrier; �ñ�û�� metatable, so it does not need to invalidate cache */
	setivalue(idx, k);
	luaM_growvector(L, f->k, k, f->sizek, TValue, MAXARG_Ax, "constants");
	while (oldsize < f->sizek) setnilvalue(&f->k[oldsize++]);
	setobj(L, &f->k[k], v);
	fs->nk++;
	luaC_barrier(L, f, v);
	return k;  /* ���س������е�id */
}


/*
** ������(k)������ַ�������(s), �������ڳ������е�id(���).
*/
int luaK_stringK(FuncState *fs, TString *s) {
	TValue o;
	setsvalue(fs->ls->L, &o, s);
	return addk(fs, &o, &o);  /* ʹ���ַ�����Ϊkey */
}


/*
** ��һ��������ӵ������б��в�����������.
** ����ʹ��userdata������Ϊ��, �Ա����������ֵͬ�ĸ�������ͻ; ת��Ϊ"void *"������
** ��ϣ, ���û��"����"����.
*/
int luaK_intK(FuncState *fs, lua_Integer n) {
	TValue k, o;
	setpvalue(&k, cast(void*, cast(size_t, n)));
	setivalue(&o, n);
	return addk(fs, &k, &o);
}

/*
** ��һ����������ӵ������б��в�����������.
*/
static int luaK_numberK(FuncState *fs, lua_Number r) {
	TValue o;
	setfltvalue(&o, r);
	return addk(fs, &o, &o);  /* ʹ�ø���ֵ��Ϊkey */
}


/*
** ��һ������ֵ��ӵ������б��в�����������.
*/
static int boolK(FuncState *fs, int b) {
	TValue o;
	setbvalue(&o, b);
	return addk(fs, &o, &o);  /* ʹ�ò���ֵ��Ϊkey */
}


/*
** �� nil ��ӵ������б��в�����������.
*/
static int nilK(FuncState *fs) {
	TValue k, v;
	setnilvalue(&v);
	/* ����ʹ��nil��Ϊ��; ����ʹ�ñ�������ʾnil */
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
** Ϊʲô��Ҫ�ض���, ��Ϊ��������ָ��, ����֪��ָ��ľ������.(�Լ������)
** �� VLOCAL ��û������ָ��.
*/
void luaK_dischargevars(FuncState *fs, expdesc *e) {
	switch (e->k) {
	case VLOCAL: {  /* already in a register */
	  /*
	  **     ������VLOCAL, ˵��ǰ���Ѿ��������������, ��ô���Ȳ���Ҫ�ض�
	  ** ��, Ҳ����Ҫ������������ֵ���ؽ���. ���оֲ����������ں���ջ��
	  ** ��һ����Ӧ��λ��
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
		**     ��Ҫ�ض���, ������ OP_GETTABLE �� OP_GETTABUP ָ��, ��ȡt[idx]
		** ��ֵ, ָ���A����ĿǰΪֹ0, ��Ϊ�������������ǻ�ȡ���������ֵ֮
		** ����Ҫ��ŵ��ļĴ�����ַ, ��ʱ����֪��.
		**     ֮������Ҫ�ض���, ����Ϊ�����ɸ�ָ��ʱ, ����֪����ǰ���õļĴ���
		** ��ַ��ʲô, ������֪�����������ֵ��󽫼��ص�ջ���ĸ�λ��. Ҳ����
		** ��ָ���еĲ���A�����ɸ�ָ��ʱ����δ֪��, ��Ҫ�������discharge2reg
		** �����в�֪��, ��Ϊ�Ĵ�����ַ��Ϊ��������Ĳ���������.
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
	/* �������ʽ(e), �ж��Ƿ���Ҫ�ض��� */
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
	case VKFLT: {  /* ���� LOADK ָ��, ���������ص�reg��Ӧ�ļĴ����� */
		luaK_codek(fs, reg, luaK_numberK(fs, e->u.nval));
		break;
	}
	case VKINT: {  /* ���� LOADK ָ��, ���������ص�reg��Ӧ�ļĴ����� */
		luaK_codek(fs, reg, luaK_intK(fs, e->u.ival));
		break;
	}
	case VRELOCABLE: {  /* �ض���, ����reg������д�����ָ��Ĳ���A */
		Instruction *pc = &getinstruction(fs, e);
		SETARG_A(*pc, reg);  /* instruction will put result in 'reg' */
		break;
	}
	case VNONRELOC: {  /* ���ʽ������VNONRELOC, Ҳ���ǲ���Ҫ�ض�λ, ֱ������MOVָ�� */
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
** ȷ�����ձ��ʽ���(��������ת�б�Ľ��)����һ�����üĴ�����.
** 1. ���� luaK_dischargevars ����, ���ݱ������ڵĲ�ͬ������(local, global, upvalue)
** ��������������Ƿ���Ҫ�ض���.
** 2. ���� luaK_reserveregs ����, ������õĺ����Ĵ����ռ�, �õ�����ռ��Ӧ�ļĴ���
** ����, ���˿ռ�, ���ܴ洢����
** 3. ���� exp2reg ����, ������ɰѱ��ʽ�����ݴ����Ĵ����ռ�Ĺ���. �����������,
** �����ֻ���� discharge2reg ����, ����������ݲ�ͬ�ı��ʽ����(NIL, �������ʽ, ��
** �ֵ�)�����ɴ�ȡ���ʽ��ֵ���Ĵ������ֽ���.
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
** ȷ�����ձ��ʽ���(��������ת�б�Ľ��)��ĳЩ(����)�Ĵ�����, �����ظüĴ���
*/
int luaK_exp2anyreg(FuncState *fs, expdesc *e) {
	/* �������ʽ�Ƿ���Ҫ�ض�λ */
	luaK_dischargevars(fs, e);
	if (e->k == VNONRELOC) {  /* ���ʽ�Ѿ���һ���Ĵ���? */
		if (!hasjumps(e))  /* ����ת? */
			return e->u.info;  /* ����Ѿ��ڼĴ����� */
		if (e->u.info >= fs->nactvar) {  /* reg. is not a local? */
			exp2reg(fs, e, e->u.info);  /* put final result in it */
			return e->u.info;
		}
	}
	luaK_exp2nextreg(fs, e);  /* ����, ��ʹ����һ�����õļĴ��� */
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
** �������ʽ"t[k]". "t"�����ѽ������ս���洢�ڼĴ�����upvalue��.
** ���� VINDEXED �ı��ʽ, tΪ���id, idxΪ��(key)�ı��(�Ĵ����������)
** vt Ϊ�������(�ֲ� or upvalue)
*/
void luaK_indexed(FuncState *fs, expdesc *t, expdesc *k) {
	lua_assert(!hasjumps(t) && (vkisinreg(t->k) || t->k == VUPVAL));
	t->u.ind.t = t->u.info;  /* register or upvalue index */
	t->u.ind.idx = luaK_exp2RK(fs, k);  /* R/K index for key */
	t->u.ind.vt = (t->k == VUPVAL) ? VUPVAL : VLOCAL;
	t->k = VINDEXED;
}


/*
**     �ڽ��г���չ��(constfolding)����ʱ, �ж���������������Ч��; ��λ������Ҫת��Ϊ
** �����Ĳ�����, ��������ĳ�������Ϊ0. �������չ������������, �򷵻�false.
*/
static int validop(int op, TValue *v1, TValue *v2) {
	switch (op) {
	case LUA_OPBAND: case LUA_OPBOR: case LUA_OPBXOR:
	case LUA_OPSHL: case LUA_OPSHR: case LUA_OPBNOT: {  /* ת��Ϊ���� */
		lua_Integer i;
		return (tointeger(v1, &i) && tointeger(v2, &i));
	}
	case LUA_OPDIV: case LUA_OPIDIV: case LUA_OPMOD:  /* ��������Ϊ�� */
		return (nvalue(v2) != 0);
	default: return 1;  /* ����������Ϸ� */
	}
}


/*
** ���ڶ�Ԫ����ʱ, lua����һЩ�Ż�
**     ���жϲ��������������ǳ�������ʱ, ����г���չ��("constant-fold")����, ����
** ����ʱ�����ǵĽ�������, �ٸ�ֵ����Ӧ�ı���, ����: local a = 4 + 7
**     ���ǲ������κ�ʱ��ֻҪ���������ǳ���, �����Խ��г���չ������, ��Ҫ���ǵ�������
** ���ȼ�. ����: local a = b + 4 + 7
**     ����ɹ�, ����1. ���ս�������ڱ��ʽ(e1)��.
*/
static int constfolding(FuncState *fs, int op, expdesc *e1,
	const expdesc *e2) {
	TValue v1, v2, res;
	if (!tonumeral(e1, &v1) || !tonumeral(e2, &v2) || !validop(op, &v1, &v2))
		return 0;  /* �����ֲ������򲻰�ȫ���� */
	luaO_arith(fs->ls->L, op, &v1, &v2, &res);  /* ������Ӧ�Ķ�Ԫ���� */
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
** **Ϊ������ֵ�������е����ǣ���һԪ���ʽ�������롣 �������ս���ı��ʽ��������Ϊ�� e����
*/
static void codeunexpval(FuncState *fs, OpCode op, expdesc *e, int line) {
	int r = luaK_exp2anyreg(fs, e);  /* ���ʽ(e)���ɶ�Ӧ�Ĳ�����, ���ض�Ӧ�ļĴ��� */
	freeexp(fs, e);
	e->u.info = luaK_codeABC(fs, op, 0, r, 0);  /* ���ɲ����� */
	e->k = VRELOCABLE;  /* ������Щ��������Ҫ�ض�λ */
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
** ��ǰ׺����"op"Ӧ���ڱ��ʽ"e"
*/
void luaK_prefix(FuncState *fs, UnOpr op, expdesc *e, int line) {
	static const expdesc ef = { VKINT, {0}, NO_JUMP, NO_JUMP };
	switch (op) {
	case OPR_MINUS: case OPR_BNOT:  /* ʹ��'ef'��Ϊα�ڶ������� */
		if (constfolding(fs, op + LUA_OPUNM, e, &ef))  /* ���г���չ������ */
			break;
		/* FALLTHROUGH */
	case OPR_LEN:
		/* ���ɶ�Ӧ��ָ���� */
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

