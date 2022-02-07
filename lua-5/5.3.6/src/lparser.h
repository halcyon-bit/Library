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
**     对一门语言进行解析一般是两遍遍历的过程, 第一遍解析源代码并生成AST(抽象语法树),
** 第二遍再将AST翻译为对应的字节码. AST仅是分析过程中的中间产物, 在实际输出中是不需要的
**
**     lua使用一遍扫描代码文件的方式生成字节码, 即在第一遍扫描代码的时候同时生成对应的
** 字节码, 这么做主要是加快解释执行的速度.
**
**     lua使用的是递归下降法进行解析, 这个分析方式针对文法中的每一个非终结符, 建立一个
** 子程序模拟语法树向下推导, 在推导过程中遇到终结符则检查是否匹配, 遇到非终结符则调用对
** 应的相关子程序进行处理.
**
**     lua以EBNF语法为例, 小写字母表示非终结符, 大写字母表示终结符.
**
** chunk -> { stat [';'] }
** stat -> ifstat | whilestat | dostat | forstat | repeatstat | funcstat | localstat | retstat | breakstat | exprstat
** ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END
** whilestat -> WHILE cond DO block END
** dostat -> DO block END
** orstat -> FOR {fornum | forlist} END
** repeatstat -> REPEAT block UNTIL cond
** funcstat -> FUNCTION funcname body
** localstat -> LOCAL function Name funcbody | LOCAL NAME {',' NAME} ['=' explist1]
** retstat -> RETURN [explist1]
** breakstat -> BREAK
** exprstat -> primaryexp
** block -> chunk
** cond -> exp
** fornum -> NAME = exp1,exp1[,exp1] forbody
** forlist -> NAME {,NAME} IN explist1 forbody
** forbody -> DO block
** funcname -> NAME {field} [':' NAME]
** body -> '(' parlist ')' chunk END
** primaryexp -> prefixexp {'.' NAME | '[' exp ']' | NAME funcargs | funcargs }
** prefixexp -> NAME | '(' exp ')'
** funcargs -> '(' explistl ')' | constructor | STRING
** exp -> subexpr
** subexpr -> (simpleexp | unop subexpr) {binop subexpr}
** simpleexp -> NUMBER | STRING | NIL | true | false | ... | constructor | FUNCTION body | primaryexp
** explistl -> expr {',' expr}
** constructor -> '{' [ fieldlist ] '}'
** fieldlist -> field { fieldsep field } [ fieldsep ]
** field -> '[' exp ']' '=' exp | name '=' exp | exp
** fieldsep -> ',' | ';'
**
** [] 内包含的为可选项
** {} 内包含的可可重复0至无数次的项
** | 表示在其左右两边任选一项, 相当于"OR"的意思
** ::= 是"被定义为"的意思
**
**     chunk 表示一个执行单元, 可以简单理解为一串语句段, 每个语句段可以使用';'分隔. 即
** 可以是一个lua文件, 也可以是一个函数体. 这是body表达式的EBNF中也出现chunk的原因
**
**     Lua把 chunk 当作拥有不定参数的匿名函数来处理. 正因为这样, chunk 内可以定义局部
** 变量, 接收参数, 并且返回值
*/

/*
** Expression and variable descriptor.
** Code generation for variables and expressions can be delayed to allow
** optimizations; An 'expdesc' structure describes a potentially-delayed
** variable/expression. It has a description of its "main" value plus a
** list of conditional jumps that can also produce its value (generated
** by short-circuit operators 'and'/'or').
*/

/* 变量或表达式的种类 */
typedef enum
{
    VVOID,  /* when 'expdesc' describes the last expression a list,
                this kind means an empty list (so, no expression) */
                VNIL,  /* constant nil */
                VTRUE,  /* constant true */
                VFALSE,  /* constant false */
                VK,  /* 常量(在Proto.k列表中); info = 常量表(k)中的位置(id) */
                VKFLT,  /* floating constant; nval = numerical float value */
                VKINT,  /* integer constant; nval = numerical integer value */
                VNONRELOC,  /* expression has its value in a fixed register;
                                info = result register */
                                VLOCAL,  /* 局部变量local variable; info = local register */
                                VUPVAL,  /* upvalue variable; info = index of upvalue in 'upvalues' */
                                VINDEXED,  /* indexed variable;
                                                ind.vt = whether 't' is register or upvalue;
                                                ind.t = table register or upvalue;
                                                ind.idx = key's R/K index */
                                                VJMP,  /* expression is a test/comparison;
                                                            info = pc of corresponding jump instruction */
                                                            VRELOCABLE,  /* expression can put result in any register;
                                                                            info = instruction pc */
                                                                            VCALL,  /* expression is a function call; info = instruction pc */
                                                                            VVARARG  /* vararg expression; info = instruction pc */
} expkind;

/* 是否为变量 */
#define vkisvar(k)	    (VLOCAL <= (k) && (k) <= VINDEXED)
#define vkisinreg(k)	((k) == VNONRELOC || (k) == VLOCAL)

/* 表达式的解析 */
typedef struct expdesc
{
    expkind k;  /* 变量或表达式的种类 */
    union
    {
        lua_Integer ival;    /* for VKINT */
        lua_Number nval;  /* for VKFLT */
        int info;  /* for generic use */
        struct {  /* for indexed variables (VINDEXED) */
            short idx;  /* index (R/K) */
            lu_byte t;  /* table (register or upvalue) */
            lu_byte vt;  /* whether 't' is register (VLOCAL) or upvalue (VUPVAL) */
        } ind;
    } u;
    int t;  /* patch list of 'exit when true' */
    int f;  /* patch list of 'exit when false' */
} expdesc;


/* description of active local variable */
typedef struct Vardesc
{
    short idx;  /* variable index in stack */
} Vardesc;


/* description of pending goto statements and label statements */
typedef struct Labeldesc
{
    TString* name;  /* label identifier */
    int pc;  /* position in code */
    int line;  /* line where it appeared */
    lu_byte nactvar;  /* local level where it appears in current block */
} Labeldesc;


/* list of labels or gotos */
typedef struct Labellist
{
    Labeldesc* arr;  /* array */
    int n;  /* number of entries in use */
    int size;  /* array size */
} Labellist;


/* dynamic structures used by the parser */
typedef struct Dyndata
{
    struct {  /* list of active local variables */
        Vardesc* arr;
        int n;
        int size;
    } actvar;
    Labellist gt;  /* list of pending gotos */
    Labellist label;   /* list of active labels */
} Dyndata;


/* control of blocks */
struct BlockCnt;  /* defined in lparser.c */


/* state needed to generate code for a given function */
typedef struct FuncState {
    Proto* f;  /* current function header */
    struct FuncState* prev;  /* enclosing function */
    struct LexState* ls;  /* lexical state */
    struct BlockCnt* bl;  /* chain of current blocks */
    int pc;  /* next position to code (equivalent to 'ncode') */
    int lasttarget;   /* 'label' of last 'jump label' */
    int jpc;  /* list of pending jumps to 'pc' */
    int nk;  /* number of elements in 'k' */
    int np;  /* number of elements in 'p' */
    int firstlocal;  /* index of first local var (in Dyndata array) */
    short nlocvars;  /* number of elements in 'f->locvars' */
    lu_byte nactvar;  /* number of active local variables */
    lu_byte nups;  /* number of upvalues */
    lu_byte freereg;  /* 当前函数栈的下一个可用位置, statement 函数最后, 都会根据
    函数栈存放的变量数量(包括函数的局部变量、函数的参数等)进行调整 first free register */
} FuncState;


LUAI_FUNC LClosure* luaY_parser(lua_State* L, ZIO* z, Mbuffer* buff,
    Dyndata* dyd, const char* name, int firstchar);


#endif
