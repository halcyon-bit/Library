/*
** $Id: llex.h,v 1.79.1.1 2017/04/19 17:20:42 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#ifndef llex_h
#define llex_h

#include "lobject.h"
#include "lzio.h"

/* RESERVED枚举的起始值, 避开ASCII的取值范围 */
#define FIRST_RESERVED	257


#if !defined(LUA_ENV)
#define LUA_ENV		"_ENV"  /* 全局环境变量名称 */
#endif


/*
** 警告: 如果更改此枚举的顺序, 注意其顺序
** grep "ORDER RESERVED"
*/
enum RESERVED
{
    /* TK_AND到TK_WHILE是表示保留字的终结符(终结符是指语法中不需要继续推导的符号) */
    TK_AND = FIRST_RESERVED, TK_BREAK,
    TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
    TK_GOTO, TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
    TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
    /* TK_IDIV到TK_STRING是非保留字的其它终结符(运算符、字面值和文件结束) */
    TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
    TK_SHL, TK_SHR,
    TK_DBCOLON, TK_EOS,
    TK_FLT, TK_INT, TK_NAME, TK_STRING
    /* 浮点, 整形,   变量名,  字符串 */
};

/* 保留字(RESERVED)的数量 */
#define NUM_RESERVED	(cast(int, TK_WHILE-FIRST_RESERVED+1))

/* 单个语义信息, 对应TK_FLT, INT, NAME, STRING的值 */
typedef union
{
    lua_Number r;  /* 浮点, 对应TK_FLT */
    lua_Integer i;  /* 整形, 对应TK_INT */
    TString* ts;  /* 字符串, 对应TK_STRING or TK_NAME */
} SemInfo;  /* 语义信息 */

/*
** 单个符号信息(token)
** token的分类: 数字和字符串; 特殊字符(运算符和括号); 关键字(保留字)
*/
typedef struct Token
{
    int token;  /* 类型, 既可以是一个字符(ASCII), 也可以是一个enum RESERVED */
    /*
    **     如果类型是一个TK_FLT, seminfo.r就用来表示这个数字; 如果是TK_NAME或者
    ** TK_STRING, seminfo.ts就表示对应的字符串.
    */
    SemInfo seminfo;
} Token;


/* 所有函数共享, 词法分析器的状态加上解析器的状态 */
typedef struct LexState
{
    int current;  /* 当前字符 (charint) */
    int linenumber;  /* 行号(累加) */
    int lastline;  /* 最后一个token('consumed')的行号 */
    Token t;  /* 当前token */
    Token lookahead;  /* 下一个token(预读, t的下一个) */
    struct FuncState* fs;  /*     当前的函数状态指针, 每一个函数对应一个FuncState,
                           ** 形成一个链表 */
    struct lua_State* L;  /* lua状态机 */
    ZIO* z;  /* 输入流 */
    Mbuffer* buff;  /* tokens的缓冲区(保存字符) */
    Table* h;  /* 避免收集/重用字符串, 该表中保存着所有的遇到的字符串, 其保证字符串
               ** 唯一, 好像不仅仅是收集字符串, 数字也会收集, 详见code中的addk */
    struct Dyndata* dyd;  /* 用于保存数据信息, 如变量的位置, goto, label标签等等 */
    TString* source;  /* 当前源文件名称 */
    TString* envn;  /* 环境变量名称, _ENV */
} LexState;


LUAI_FUNC void luaX_init(lua_State* L);
LUAI_FUNC void luaX_setinput(lua_State* L, LexState* ls, ZIO* z,
    TString* source, int firstchar);
LUAI_FUNC TString* luaX_newstring(LexState* ls, const char* str, size_t l);
LUAI_FUNC void luaX_next(LexState* ls);
LUAI_FUNC int luaX_lookahead(LexState* ls);
LUAI_FUNC l_noret luaX_syntaxerror(LexState* ls, const char* s);
LUAI_FUNC const char* luaX_token2str(LexState* ls, int token);

#endif
