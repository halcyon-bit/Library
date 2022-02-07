/*
** $Id: llex.c,v 2.96.1.1 2017/04/19 17:20:42 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#define llex_c
#define LUA_CORE

#include "lprefix.h"


#include <locale.h>
#include <string.h>

#include "lua.h"

#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lobject.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"


/* 从输入流中读取一个字符值，保存为 ls (词法分析器)的当前字符值 */
#define next(ls) (ls->current = zgetc(ls->z))


/* 判断当前是否是回车(即开始新的一行) */
#define currIsNewline(ls)	(ls->current == '\n' || ls->current == '\r')


/* ORDER RESERVED token 的字符串形式 */
static const char* const luaX_tokens[] =
{
    "and", "break", "do", "else", "elseif",
    "end", "false", "for", "function", "goto", "if",
    "in", "local", "nil", "not", "or", "repeat",
    "return", "then", "true", "until", "while",
    "//", "..", "...", "==", ">=", "<=", "~=",
    "<<", ">>", "::", "<eof>",
    "<number>", "<integer>", "<name>", "<string>"
};

/* 把当前字符保存到变长缓冲区(ls->buff), 然后读取下一个字符 */
#define save_and_next(ls) (save(ls, ls->current), next(ls))

/* 报告词法错误，然后抛出 LUA_ERRSYNTAX 异常 */
static l_noret lexerror(LexState* ls, const char* msg, int token);

/* 把字符(c)保存到 ls->buff 变长缓冲区的结尾(确保不会超出缓冲区范围) */
static void save(LexState* ls, int c)
{
    Mbuffer* b = ls->buff;
    if (luaZ_bufflen(b) + 1 > luaZ_sizebuffer(b))
    {
        /* 当前buff已经满了, 扩展为原先的2倍, 但不能超过 MAX_SIZE */
        size_t newsize;
        if (luaZ_sizebuffer(b) >= MAX_SIZE / 2)
        {
            lexerror(ls, "lexical element too long", 0);
        }
        newsize = luaZ_sizebuffer(b) * 2;
        luaZ_resizebuffer(ls->L, b, newsize);
    }
    b->buffer[luaZ_bufflen(b)++] = cast(char, c);  /* 保存字符c */
}

/* 初始化词法分析器, 把所有保留字加入系统中, 确保所有保留字字符串不被回收 */
void luaX_init(lua_State* L)
{
    int i;
    TString* e = luaS_newliteral(L, LUA_ENV);  /* 创建全局环境变量字符串 */
    luaC_fix(L, obj2gco(e));  /* 该字符串永远不会被回收 */
    for (i = 0; i < NUM_RESERVED; i++)
    {
        /* 创建保留字字符串 */
        TString* ts = luaS_new(L, luaX_tokens[i]);
        luaC_fix(L, obj2gco(ts));  /* 同样保证保留字符串永远不被回收 */
        ts->extra = cast_byte(i + 1);  /* 标记该字符串为保留字符串 */
    }
}

/* 把token(单字符、控制字符、保留字符号)转为字符串 */
const char* luaX_token2str(LexState* ls, int token)
{
    if (token < FIRST_RESERVED)
    {   /* 非保留字, 即ASCII值 */
        lua_assert(token == cast_uchar(token));
        return luaO_pushfstring(ls->L, "'%c'", token);
    }
    else
    {   /* 保留字 */
        const char* s = luaX_tokens[token - FIRST_RESERVED];
        if (token < TK_EOS)  /* 固定格式(符号和保留字) */
            return luaO_pushfstring(ls->L, "'%s'", s);
        else  /* names, strings, and numerals */
            return s;
    }
}

/*
** 将token转化为对应的字符串
** 如果token是name、int、string、float, 把ls->buff转成'\0'结尾的C字符串
** 如果token是特殊token(保留字、运算符), 获取其字符串形式.
*/
static const char* txtToken(LexState* ls, int token)
{
    switch (token)
    {
    case TK_NAME: case TK_STRING:
    case TK_FLT: case TK_INT:
        save(ls, '\0');
        return luaO_pushfstring(ls->L, "'%s'", luaZ_buffer(ls->buff));
    default:
        return luaX_token2str(ls, token);
    }
}

/* 报告词法错误, 然后抛出LUA_ERRSYNTAX异常 */
static l_noret lexerror(LexState* ls, const char* msg, int token)
{
    /* 将'文件名:行号'添加到msg中 */
    msg = luaG_addinfo(ls->L, msg, ls->source, ls->linenumber);
    if (token)
    {
        /* 创建错误信息, 并将其放在栈顶 */
        luaO_pushfstring(ls->L, "%s near %s", msg, txtToken(ls, token));
    }
    luaD_throw(ls->L, LUA_ERRSYNTAX);  /* 抛出异常 */
}

/* 调用lexerror, 抛出错误 */
l_noret luaX_syntaxerror(LexState* ls, const char* msg)
{
    lexerror(ls, msg, ls->t.token);
}



/* 在符号表(ls->t)中创建字符串(确保不会重复创建), 返回TString指针 */
TString* luaX_newstring(LexState* ls, const char* str, size_t l)
{
    lua_State* L = ls->L;
    TValue* o;  /* entry for 'str' */
    TString* ts = luaS_newlstr(L, str, l);  /* 创建字符串 */
    setsvalue2s(L, L->top++, ts);  /* 暂时将其放入栈顶, 函数末尾会移除 */
    o = luaH_set(L, ls->h, L->top - 1);  /* 从表ls->h中查询key=string的键值是否存在 */
    if (ttisnil(o))
    {
        /* 字符串不存在, 并创建了新的key(luaH_set中完成创建新key) */
        setbvalue(o, 1);  /* t[string] = true 设置value */
        luaC_checkGC(L);
    }
    else
    {   /* 字符串已经存在 */
        ts = tsvalue(keyfromval(o));  /* 重新使用先前存储的值 */
    }
    L->top--;  /* 将临时字符串从栈顶移除 */
    return ts;
}


/*
** 增加行号并跳过换行符
** (\n, \r, \n\r或\r\n中的任何一个)
*/
static void inclinenumber(LexState* ls)
{
    int old = ls->current;
    lua_assert(currIsNewline(ls));
    next(ls);  /* 跳过 '\n' or '\r' */
    if (currIsNewline(ls) && ls->current != old)
        next(ls);  /* 跳过 '\n\r' or '\r\n' */
    if (++ls->linenumber >= MAX_INT)
        lexerror(ls, "chunk has too many lines", 0);
}

/* 初始化词法解析器的输入流ls，然后读取第一个字符值 */
void luaX_setinput(lua_State* L, LexState* ls, ZIO* z, TString* source,
    int firstchar)
{
    ls->t.token = 0;  /* 当前token */
    ls->L = L;
    ls->current = firstchar;  /* 第一个字符 */
    ls->lookahead.token = TK_EOS;  /* 下一个token(预读) */
    ls->z = z;
    ls->fs = NULL;
    ls->linenumber = 1;
    ls->lastline = 1;
    ls->source = source;
    ls->envn = luaS_newliteral(L, LUA_ENV);  /* 获取全局环境字符串, 无则创建 */
    luaZ_resizebuffer(ls->L, ls->buff, LUA_MINBUFFER);  /* 初始化字符缓冲区 */
}



/*
** =======================================================
** LEXICAL ANALYZER
** =======================================================
*/

/*
** 判断当前字符是否为指定字符c,
** 如果是, 读取下一个字符, 并返回1; 否则返回0
*/
static int check_next1(LexState* ls, int c)
{
    if (ls->current == c)
    {
        next(ls);
        return 1;
    }
    else
        return 0;
}


/*
** 检测当前字符是否在字符集set(2个)中的任何一个
** 如果是, 则保存该字符(save_and_next), 并返回1;
** 否则, 返回0
*/
static int check_next2(LexState* ls, const char* set)
{
    lua_assert(set[2] == '\0');
    if (ls->current == set[0] || ls->current == set[1])
    {
        save_and_next(ls);
        return 1;
    }
    else
        return 0;
}


/* LUA_NUMBER */
/*
** 循环(逐字符)读取LUA_NUMBER字面值字符串, 并将字符串转化为相应的数字
** 返回TK_INT or TK_FLT
*/
static int read_numeral(LexState* ls, SemInfo* seminfo)
{
    TValue obj;
    const char* expo = "Ee";
    int first = ls->current;
    lua_assert(lisdigit(ls->current));
    save_and_next(ls);
    if (first == '0' && check_next2(ls, "xX"))
        expo = "Pp";  /* 16进制 */
    for (;;)
    {
        if (check_next2(ls, expo))  /* 判断数值是否为指数形式, 如2e+1, 即2*10^1 */
        {
            check_next2(ls, "-+");  /* 指数的正负号 */
        }
        if (lisxdigit(ls->current))  /* 数字 */
        {
            save_and_next(ls);
        }
        else if (ls->current == '.')  /* 小数点 */
        {
            save_and_next(ls);
        }
        else
            break;
    }
    save(ls, '\0');
    if (luaO_str2num(luaZ_buffer(ls->buff), &obj) == 0)  /* 将字符串(数字)转换为数字 */
    {
        /* 数字格式错误 */
        lexerror(ls, "malformed number", TK_FLT);
    }
    if (ttisinteger(&obj))
    {
        seminfo->i = ivalue(&obj);  /* TK_INT 整形 */
        return TK_INT;
    }
    else
    {
        lua_assert(ttisfloat(&obj));  /* TK_FLT 浮点 */
        seminfo->r = fltvalue(&obj);
        return TK_FLT;
    }
}


/*
** 跳过序列'[=*['或']=*]'; 如果序列正常, 则返回'='的数量;
** 否则, 返回一个负数(如果在初始括号后没有'=', 则返回-1)
** 序列为: [[, 中间包含n个'='
*/
static int skip_sep(LexState* ls)
{
    int count = 0;  /* 记录'='的数量 */
    int s = ls->current;
    lua_assert(s == '[' || s == ']');
    save_and_next(ls);
    while (ls->current == '=')
    {
        save_and_next(ls);
        count++;
    }
    return (ls->current == s) ? count : (-count) - 1;
}

/*
** 读取带分隔符的长字符串(如[[abc]], [==[abc]==])
** seminfo为NULL, 表示该字符串为注释
** sep为[[中间'='的数量, 为了匹配]]中的'='数量
*/
static void read_long_string(LexState* ls, SemInfo* seminfo, int sep)
{
    int line = ls->linenumber;  /* 初始行(用于显示错误信息) */
    save_and_next(ls);  /* 跳过第二个'[' */
    if (currIsNewline(ls))  /* 判断是否是新的一行? */
    {
        inclinenumber(ls);  /* 增加行号 */
    }
    for (;;)
    {
        switch (ls->current)
        {
        case EOZ:
        {   /* error */
            const char* what = (seminfo ? "string" : "comment");
            const char* msg = luaO_pushfstring(ls->L,
                "unfinished long %s (starting at line %d)", what, line);
            lexerror(ls, msg, TK_EOS);
            break;  /* 异常 */
        }
        case ']':
        {
            /* 长字符串结束? */
            if (skip_sep(ls) == sep)  /* 判断'='数量是否与字符串开始标志中的数量相同 */
            {
                /* 相同, 则表示长字符串结束 */
                save_and_next(ls);  /* 跳过第二个']' */
                goto endloop;
            }
            break;
        }
        case '\n':
        case '\r':
        {
            /* 新的一行 */
            save(ls, '\n');
            inclinenumber(ls);
            if (!seminfo)
            {
                /* 如果长字符串为注释 */
                luaZ_resetbuffer(ls->buff);  /* 避免浪费空间 */
            }
            break;
        }
        default: {
            if (seminfo)
            {
                save_and_next(ls);
            }
            else
            {
                next(ls);
            }
        }
        }
    }
endloop:
    if (seminfo)
    {
        /* 该长字符串非注释, 保存 */
        /* 要去除前后的序列(如'[==[') */
        seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + (2 + sep),
            luaZ_bufflen(ls->buff) - 2 * (2 + sep));
    }
}

/* 若c为0, 则产生异常 */
static void esccheck(LexState* ls, int c, const char* msg)
{
    if (!c)
    {
        if (ls->current != EOZ)
        {
            save_and_next(ls);  /* add current to buffer for error message */
        }
        lexerror(ls, msg, TK_STRING);
    }
}

/* 十六进制 */
static int gethexa(LexState* ls)
{
    save_and_next(ls);
    esccheck(ls, lisxdigit(ls->current), "hexadecimal digit expected");
    return luaO_hexavalue(ls->current);
}

/* 16进制数据 */
static int readhexaesc(LexState* ls)
{
    int r = gethexa(ls);
    r = (r << 4) + gethexa(ls);
    luaZ_buffremove(ls->buff, 2);  /* 将上面保存的两个字符从缓冲区中移除 */
    return r;
}

/*
** 用UTF-8编码的Unicode字符, 可以用转义符\u{XXX}来表示(这里必须
** 有一对花括号), 此处的XXX是用16进制表示的字符编号
*/
static unsigned long readutf8esc(LexState* ls)
{
    unsigned long r;
    int i = 4;  /* chars to be removed: '\', 'u', '{', and first digit */
    save_and_next(ls);  /* skip 'u' */
    esccheck(ls, ls->current == '{', "missing '{'");
    r = gethexa(ls);  /* must have at least one digit */
    while ((save_and_next(ls), lisxdigit(ls->current)))
    {
        i++;
        r = (r << 4) + luaO_hexavalue(ls->current);
        esccheck(ls, r <= 0x10FFFF, "UTF-8 value too large");
    }
    esccheck(ls, ls->current == '}', "missing '}'");
    next(ls);  /* skip '}' */
    luaZ_buffremove(ls->buff, i);  /* remove saved chars from buffer */
    return r;
}

/* utf8数据 */
static void utf8esc(LexState* ls)
{
    char buff[UTF8BUFFSZ];
    int n = luaO_utf8esc(buff, readutf8esc(ls));
    for (; n > 0; n--)  /* add 'buff' to string */
        save(ls, buff[UTF8BUFFSZ - n]);
}

/* 十进制 3位 */
static int readdecesc(LexState* ls)
{
    int i;
    int r = 0;  /* result accumulator */
    for (i = 0; i < 3 && lisdigit(ls->current); i++)
    {   /* read up to 3 digits */
        r = 10 * r + ls->current - '0';
        save_and_next(ls);
    }
    esccheck(ls, r <= UCHAR_MAX, "decimal escape too large");
    luaZ_buffremove(ls->buff, i);  /* remove read digits from buffer */
    return r;
}

/*
** 读取字符串(如"abc", 'abc')
** del: '"' 或 '\''
*/
static void read_string(LexState* ls, int del, SemInfo* seminfo)
{
    save_and_next(ls);  /* 保存'"' 或 '\'' (for error messages) */
    while (ls->current != del)
    {
        switch (ls->current)
        {
        case EOZ:
            lexerror(ls, "unfinished string", TK_EOS);
            break;  /* 异常 */
        case '\n':
        case '\r':
            lexerror(ls, "unfinished string", TK_STRING);
            break;  /* 异常, 短字符串只有一行 */
        case '\\':
        {   /* 转义字符 */
            int c;  /* 要保存的最终字符 */
            save_and_next(ls);  /* 保存 '\\' (for error messages) */
            switch (ls->current)
            {
            case 'a':
                c = '\a'; goto read_save;  /* 响铃 */
            case 'b':
                c = '\b'; goto read_save;  /* 退格 */
            case 'f':
                c = '\f'; goto read_save;  /* 换页 */
            case 'n':
                c = '\n'; goto read_save;  /* 换行 */
            case 'r':
                c = '\r'; goto read_save;  /* 回车 */
            case 't':
                c = '\t'; goto read_save;  /* 横项制表 */
            case 'v':
                c = '\v'; goto read_save;  /* 纵向制表 */
            case 'x':
                c = readhexaesc(ls); goto read_save;  /* 16进制 */
            case 'u':
                utf8esc(ls);  goto no_save;  /* utf8 */
            case '\n': case '\r':
                inclinenumber(ls); c = '\n'; goto only_save;
            case '\\': case '\"': case '\'':
                c = ls->current; goto read_save;
            case EOZ:
                goto no_save;  /* 将在下一个循环中引发错误 */
            case 'z':
            {   /* zap following span of spaces */
                luaZ_buffremove(ls->buff, 1);  /* remove '\\' */
                next(ls);  /* skip the 'z' */
                while (lisspace(ls->current))
                {
                    if (currIsNewline(ls))
                        inclinenumber(ls);
                    else
                        next(ls);
                }
                goto no_save;
            }
            default:
            {
                esccheck(ls, lisdigit(ls->current), "invalid escape sequence");
                c = readdecesc(ls);  /* digital escape '\ddd' */
                goto only_save;
            }
            }
        read_save:
            next(ls);
            /* go through */
        only_save:
            luaZ_buffremove(ls->buff, 1);  /* remove '\\' */
            save(ls, c);
            /* go through */
        no_save:
            break;
        }
        default:
            save_and_next(ls);
        }
    }
    save_and_next(ls);  /* 跳过分隔符 */
    /* 保存字符串 */
    seminfo->ts = luaX_newstring(ls, luaZ_buffer(ls->buff) + 1,
        luaZ_bufflen(ls->buff) - 2);
}

/* 解析脚本, 转化为对应的token */
static int llex(LexState* ls, SemInfo* seminfo)
{
    luaZ_resetbuffer(ls->buff);  /* 初始化buff(用于保存字符串) */
    for (;;)
    {
        switch (ls->current)
        {
        case '\n': case '\r':
        {   /* 行尾, 增加行数 */
            inclinenumber(ls);
            break;
        }
        case ' ': case '\f': case '\t': case '\v':
        {   /* 空格, 跳过 */
            next(ls);
            break;
        }
        case '-':
        {   /* '-'(符号) or '--'(注释) */
            next(ls);
            if (ls->current != '-')
                return '-';
            /* '--'表示这是一个注释, 注释都不保存的 */
            next(ls);
            if (ls->current == '[')
            {   /* 这是一个长注释 --[[ */
                int sep = skip_sep(ls);
                luaZ_resetbuffer(ls->buff);  /* 清空字符串缓冲区 */
                if (sep >= 0)
                {
                    read_long_string(ls, NULL, sep);  /* 读取长注释, 但是不保存 */
                    luaZ_resetbuffer(ls->buff);  /* 不保存注释, 所以清空字符串缓冲区 */
                    break;
                }
            }
            /* 短注释 -- */
            while (!currIsNewline(ls) && ls->current != EOZ)
                next(ls);  /* 跳到行尾(或文件结尾) */
            break;
        }
        case '[':
        {   /* 长字符串 or '[' */
            int sep = skip_sep(ls);
            if (sep >= 0)
            {
                /* 长字符串 */
                read_long_string(ls, seminfo, sep);
                return TK_STRING;
            }
            else if (sep != -1)  /* '[=...' 缺少第二个括号, 错误 */
                lexerror(ls, "invalid long string delimiter", TK_STRING);
            return '[';
        }
        case '=':
        {
            next(ls);
            if (check_next1(ls, '='))
                return TK_EQ;  /* '==' */
            else
                return '=';  /* '=' */
        }
        case '<':
        {
            next(ls);
            if (check_next1(ls, '='))
                return TK_LE;  /* '<=' */
            else if (check_next1(ls, '<'))
                return TK_SHL;  /* '<<' */
            else
                return '<';  /* '<' */
        }
        case '>':
        {
            next(ls);
            if (check_next1(ls, '='))
                return TK_GE;  /* '>=' */
            else if (check_next1(ls, '>'))
                return TK_SHR;  /* '>>' */
            else
                return '>';  /* '>' */
        }
        case '/':
        {
            next(ls);
            if (check_next1(ls, '/'))
                return TK_IDIV;  /* '//' */
            else
                return '/';  /* '/' */
        }
        case '~':
        {
            next(ls);
            if (check_next1(ls, '='))
                return TK_NE;  /* '~=' */
            else
                return '~';  /* '~' */
        }
        case ':':
        {
            next(ls);
            if (check_next1(ls, ':'))
                return TK_DBCOLON;  /* '::' */
            else
                return ':';  /* ':' */
        }
        case '"': case '\'':
        {   /* 短字符串 */
            read_string(ls, ls->current, seminfo);
            return TK_STRING;
        }
        case '.':
        {   /* '.', '..', '...', or number */
            save_and_next(ls);
            if (check_next1(ls, '.'))
            {
                if (check_next1(ls, '.'))
                    return TK_DOTS;   /* '...' */
                else
                    return TK_CONCAT;   /* '..' *//* 字符串连接符 */
            }
            else if (!lisdigit(ls->current))
                return '.';
            else
                return read_numeral(ls, seminfo);  /* 数字 */
        }
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        {
            return read_numeral(ls, seminfo);
        }
        case EOZ:
        {
            return TK_EOS;
        }
        default:
        {
            if (lislalpha(ls->current))
            {  /* 标识符或保留字? */
                TString* ts;
                do
                {
                    save_and_next(ls);
                } while (lislalnum(ls->current));
                ts = luaX_newstring(ls, luaZ_buffer(ls->buff),
                    luaZ_bufflen(ls->buff));
                seminfo->ts = ts;
                if (isreserved(ts))  /* 保留字? */
                    return ts->extra - 1 + FIRST_RESERVED;
                else
                {
                    return TK_NAME;
                }
            }
            else
            {   /* 单个字符 tokens(+ - / ...) */
                int c = ls->current;
                next(ls);
                return c;
            }
        }
        }
    }
}

/*
** 读取下一个token
** 当lookahead中有token时, 直接获取lookahead的token, 否则llex()
*/
void luaX_next(LexState* ls)
{
    ls->lastline = ls->linenumber;
    if (ls->lookahead.token != TK_EOS)
    {   /* 当存在预读取的token时, 直接获取该token */
        ls->t = ls->lookahead;
        ls->lookahead.token = TK_EOS;  /* 将预读取的token消除 */
    }
    else
        ls->t.token = llex(ls, &ls->t.seminfo);  /* 解析下一个token */
}

/* 预读一个token, 是当前token的下一个, 保存在ls的lookahead中 */
int luaX_lookahead(LexState* ls)
{
    lua_assert(ls->lookahead.token == TK_EOS);
    ls->lookahead.token = llex(ls, &ls->lookahead.seminfo);
    return ls->lookahead.token;
}

