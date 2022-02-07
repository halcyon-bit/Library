/*
** $Id: lobject.c,v 2.113.1.1 2017/04/19 17:29:57 roberto Exp $
** Some generic functions over Lua objects
** See Copyright Notice in lua.h
*/

#define lobject_c
#define LUA_CORE

#include "lprefix.h"


#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "lvm.h"

/* 零值 nil */
LUAI_DDEF const TValue luaO_nilobject_ = { NILCONSTANT };

/*
** 将整数转换为“浮点字节”
**  eeeeexxx 为二进制, 指数形式: 前五位为指数, 2为底数, 数值部分为后3位加8
** 将整数转换为“浮点字节”, 表示为(eeeeexxx), 其中如果 eeeee!=0 则实数值
** 为(1xxx)* 2^(eeeee-1), 否则为(xxx)
** 参数大于8, 才进行转换
*/
int luaO_int2fb(unsigned int x)
{
    int e = 0;  /* 指数 */
    if (x < 8)
    {
        return x;
    }
    while (x >= (8 << 4))
    {   /* coarse steps */
        x = (x + 0xf) >> 4;  /* x = ceil(x / 16) */
        e += 4;
    }
    while (x >= (8 << 1))
    {   /* fine steps */
        x = (x + 1) >> 1;  /* x = ceil(x / 2) */
        e++;
    }
    return ((e + 1) << 3) | (cast_int(x) - 8);
}

/* 反计算, “浮点字节”转换为整数, 对应于上面的函数luaO_int2fb */
int luaO_fb2int(int x)
{
    return (x < 8) ? x : ((x & 7) + 8) << ((x >> 3) - 1);
}

/*
** 计算 大于或者等于 log2(x) 的最小整数 ceil(log2(x))
*/
int luaO_ceillog2(unsigned int x)
{
    static const lu_byte log_2[256] =
    {   /* log_2[i] = ceil(log2(i - 1)) */
        0,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8
    };
    int l = 0;
    x--;
    while (x >= 256)
    {
        l += 8;
        x >>= 8;
    }
    return l + log_2[x];
}

/* 基本算术运算(整数) */
static lua_Integer intarith(lua_State* L, int op, lua_Integer v1, lua_Integer v2)
{
    switch (op)
    {
    case LUA_OPADD:
        return intop(+, v1, v2); /* v1 + v2 */
    case LUA_OPSUB:
        return intop(-, v1, v2); /* v1 - v2 */
    case LUA_OPMUL:
        return intop(*, v1, v2); /* v1 * v2 */
    case LUA_OPMOD:
        return luaV_mod(L, v1, v2); /* v1 % v2 */
    case LUA_OPIDIV:
        return luaV_div(L, v1, v2); /* v1 / v2 */
    case LUA_OPBAND:
        return intop(&, v1, v2); /* v1 & v2 */
    case LUA_OPBOR:
        return intop(| , v1, v2); /* v1 | v2 */
    case LUA_OPBXOR:
        return intop(^, v1, v2); /* v1 ^ v2 */  /* 异或 */
    case LUA_OPSHL:
        return luaV_shiftl(v1, v2); /* v1 << v2 */  /* 逻辑左移指令 */
    case LUA_OPSHR:
        return luaV_shiftl(v1, -v2); /* v1  v2 */  /* 逻辑右移指令 */
    case LUA_OPUNM:
        return intop(-, 0, v1); /* 0 - v1 */
    case LUA_OPBNOT:
        return intop(^, ~l_castS2U(0), v1); /* ~(0) ^ v1 */
    default:
        lua_assert(0);
        return 0;
    }
}

/* 基本算术运算(实数) */
static lua_Number numarith(lua_State* L, int op, lua_Number v1, lua_Number v2)
{
    switch (op)
    {
    case LUA_OPADD:
        return luai_numadd(L, v1, v2);  /* v1 + v2 */
    case LUA_OPSUB:
        return luai_numsub(L, v1, v2);  /* v1 - v2 */
    case LUA_OPMUL:
        return luai_nummul(L, v1, v2);  /* v1 * v2 */
    case LUA_OPDIV:
        return luai_numdiv(L, v1, v2);  /* v1 / v2 */
    case LUA_OPPOW:
        return luai_numpow(L, v1, v2);  /* pow(v1, v2) */ /* 幂运算 */
    case LUA_OPIDIV:
        return luai_numidiv(L, v1, v2); /* v1 // v2 */ /* 地板除法 */
    case LUA_OPUNM:
        return luai_numunm(L, v1);      /* -v1 */
    case LUA_OPMOD:
    {   /* 取余 */
        lua_Number m;
        luai_nummod(L, v1, v2, m);
        return m;
    }
    default:
        lua_assert(0);
        return 0;
    }
}

/* 基本算术运算(TValue) */
void luaO_arith(lua_State* L, int op, const TValue* p1, const TValue* p2,
    TValue* res)
{
    switch (op)
    {
    case LUA_OPBAND: case LUA_OPBOR: case LUA_OPBXOR:
    case LUA_OPSHL: case LUA_OPSHR:
    case LUA_OPBNOT:
    {   /* 这些运算符仅适用于整数 */
        lua_Integer i1; lua_Integer i2;
        if (tointeger(p1, &i1) && tointeger(p2, &i2))
        {
            setivalue(res, intarith(L, op, i1, i2));
            return;
        }
        else
            break;  /* go to the end */
    }
    case LUA_OPDIV: case LUA_OPPOW:
    {   /* 这些运算符仅适用于浮点 */
        lua_Number n1; lua_Number n2;
        if (tonumber(p1, &n1) && tonumber(p2, &n2))
        {
            setfltvalue(res, numarith(L, op, n1, n2));
            return;
        }
        else
            break;  /* go to the end */
    }
    default:
    {   /* 其他运算符 */
        lua_Number n1; lua_Number n2;
        if (ttisinteger(p1) && ttisinteger(p2))
        {
            setivalue(res, intarith(L, op, ivalue(p1), ivalue(p2)));
            return;
        }
        else if (tonumber(p1, &n1) && tonumber(p2, &n2))
        {
            setfltvalue(res, numarith(L, op, n1, n2));
            return;
        }
        else
            break;  /* go to the end */
    }
    }
    /* 不能进行基本操作; 尝试元方法 */
    lua_assert(L != NULL);  /* should not fail when folding (compile time) */
    luaT_trybinTM(L, p1, p2, res, cast(TMS, (op - LUA_OPADD) + TM_ADD));
}

/* 16进制转换为10进制 */
int luaO_hexavalue(int c)
{
    /* 如: 8的ASCII为56, c=56, 返回8 */
    if (lisdigit(c)) /* 检查c是否为十进制数字字符 即数字ASCII */
    {
        return c - '0'; /* 返回数字实际值 */
    }
    else
    {
        return (ltolower(c) - 'a') + 10; /* 十六进制中的A, B, C, D, E, F */
    }
}

/* 判断正负 正为0, 负为1 */
static int isneg(const char** s)
{
    if (**s == '-')
    {
        (*s)++;
        return 1;
    }
    else if (**s == '+')
    {
        (*s)++;
    }
    return 0;
}


/*
** {==================================================================
** Lua对'lua_strx2number'的实现
** ===================================================================
*/

#if !defined(lua_strx2number)

/* 要读取的最大有效位数（以避免单个浮点数溢出） */
#define MAXSIGDIG	30

/*
** 按照'strtod'的C99规范将十六进制数字字符串转换为数字
*/
static lua_Number lua_strx2number(const char* s, char** endptr)
{
    int dot = lua_getlocaledecpoint();  /* 小数点 */
    lua_Number r = 0.0;  /* 结果(累加器) */
    int sigdig = 0;  /* 有效位数 */
    int nosigdig = 0;  /* 非有效数字的数量 */
    int e = 0;  /* 指数修正 */
    int neg;  /* 如果数字为负数，则为1 */
    int hasdot = 0;  /* 看到'.'后是ture */
    *endptr = cast(char*, s);  /* nothing is valid yet */
    while (lisspace(cast_uchar(*s)))
    {
        s++;  /* 跳过初始空格 */
    }
    neg = isneg(&s);  /* 检测正负 */
    if (!(*s == '0' && (*(s + 1) == 'x' || *(s + 1) == 'X')))  /* 判断前缀是 0x? */
        return 0.0;  /* 无效的格式 (no '0x') */
    for (s += 2; ; s++)
    {   /* 跳过'0x'并读取数字 */
        if (*s == dot)
        {
            if (hasdot)
            {
                break;  /* 第二个小数点? 停止循环 */
            }
            else
            {
                hasdot = 1;
            }
        }
        else if (lisxdigit(cast_uchar(*s))) /* 检查参数*s是否为16进制数字: 0123456789abcdefABCDEF */
        {
            if (sigdig == 0 && *s == '0')  /* 非重要数据 (0)?non-significant digit (zero)? */
            {
                nosigdig++;
            }
            else if (++sigdig <= MAXSIGDIG)  /* 可以读取而不会溢出? */
            {
                r = (r * cast_num(16.0)) + luaO_hexavalue(*s);  /* 转化为10进制 */
            }
            else
            {
                e++; /* 数字太多; 忽略，但仍然计算指数 */
            }
            if (hasdot)
            {
                e--;  /* 十进制数字? 正确的指数 */
            }
        }
        else
        {
            break;  /* 既不是点也不是数字 */
        }
    }
    if (nosigdig + sigdig == 0)  /* 没有数字? */
    {
        return 0.0;  /* 无效的格式 */
    }
    *endptr = cast(char*, s);  /* 有效到此为止 */
    e *= 4;  /* each digit multiplies/divides value by 2^4 */
    if (*s == 'p' || *s == 'P')
    {  /* 指数部分? */
        int exp1 = 0;  /* 指数值 */
        int neg1;  /* 指数信号 */
        s++;  /* 跳过 'p' */
        neg1 = isneg(&s);  /* 正负信息 */
        if (!lisdigit(cast_uchar(*s)))
        {
            return 0.0;  /* 无效; 必须至少有一个数字 */
        }
        while (lisdigit(cast_uchar(*s)))  /* 读指数 */
        {
            exp1 = exp1 * 10 + *(s++) - '0';
        }
        if (neg1)
        {
            exp1 = -exp1;
        }
        e += exp1;
        *endptr = cast(char*, s);  /* 有效到此为止 */
    }
    if (neg)
    {
        r = -r;
    }
    return l_mathop(ldexp)(r, e);
}

#endif
/* }====================================================== */


/* 最大数字长度 */
#if !defined (L_MAXLENNUM)
#define L_MAXLENNUM	200
#endif

/* 字符串转化为数字 mode = 'x' 16进制的字符串 */
static const char* l_str2dloc(const char* s, lua_Number* result, int mode)
{
    char* endptr; /* 结束位置 */
    *result = (mode == 'x') ? lua_strx2number(s, &endptr)  /* 尝试转换 */
        : lua_str2number(s, &endptr);
    if (endptr == s)
    {
        return NULL;  /* 没有有效的数字? */
    }
    while (lisspace(cast_uchar(*endptr)))
    {
        endptr++;  /* 跳过末尾空格 */
    }
    return (*endptr == '\0') ? endptr : NULL;  /* 如果尾部没有字符，则确定 */
}

/*
** 将字符串's'转换为 lua 数字(存入'result'). 失败时返回NULL或成功时返回'\0'结尾的地址.
** 'pmode'指向(和'mode'包含)字符串中的特殊内容:
** -'x'/'X'表示十六进制数字
** -'n'/'N'表示'inf'或'nan'(应该是被拒绝)
** -'.'只是优化搜索常见情况(没什么特别的)
** 这个函数接受当前的语言环境或点作为基数标记.
** 如果转换失败，则可能意味着number有一个点, 但locale接受其他内容.
** 在这种情况下, 代码将's'复制到缓冲区(因为's'是只读的),
** 将点更改为当前的语言环境基数, 并尝试再次转换.
*/
static const char* l_str2d(const char* s, lua_Number* result)
{
    const char* endptr;
    const char* pmode = strpbrk(s, ".xXnN");
    int mode = pmode ? ltolower(cast_uchar(*pmode)) : 0;
    if (mode == 'n')  /* 拒绝 'inf' and 'nan' */
        return NULL;
    endptr = l_str2dloc(s, result, mode);  /* 第一次尝试转换 */
    if (endptr == NULL)
    {   /* 失败? 可能是一个不同的区域 */
        char buff[L_MAXLENNUM + 1];
        const char* pdot = strchr(s, '.');
        if (strlen(s) > L_MAXLENNUM || pdot == NULL)
        {
            return NULL;  /* 字符串太长或没有点; 失败 */
        }
        strcpy(buff, s);  /* 将字符串复制到缓冲区 */
        buff[pdot - s] = lua_getlocaledecpoint();  /* 正确的小数点 */
        endptr = l_str2dloc(buff, result, mode);  /* 再次尝试转换 */
        if (endptr != NULL)
        {
            endptr = s + (endptr - buff);  /* make relative to 's' */
        }
    }
    return endptr;
}


#define MAXBY10		cast(lua_Unsigned, LUA_MAXINTEGER / 10)
#define MAXLASTD	cast_int(LUA_MAXINTEGER % 10)

/* 将字符串转化为整形 */
static const char* l_str2int(const char* s, lua_Integer* result)
{
    lua_Unsigned a = 0;
    int empty = 1;
    int neg;
    while (lisspace(cast_uchar(*s)))
    {
        s++;  /* 跳过初始空格 */
    }
    neg = isneg(&s);    /* 判断正负 */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {   /* 16进制? */
        s += 2;  /* 跳过 '0x' */
        for (; lisxdigit(cast_uchar(*s)); s++)
        {
            a = a * 16 + luaO_hexavalue(*s);
            empty = 0;
        }
    }
    else
    {   /* 十进制 */
        for (; lisdigit(cast_uchar(*s)); s++)
        {
            int d = *s - '0';
            if (a >= MAXBY10 && (a > MAXBY10 || d > MAXLASTD + neg))  /* 溢出? */
                return NULL;  /* 转化失败 */
            a = a * 10 + d;
            empty = 0;
        }
    }
    while (lisspace(cast_uchar(*s)))
    {
        s++;  /* 跳过尾部空格 */
    }
    if (empty || *s != '\0')
    {
        return NULL;  /* 字符串数字有问题 */
    }
    else
    {
        *result = l_castU2S((neg) ? 0u - a : a);    /* 正负 */
        return s;
    }
}

/* 将字符串转化为数字 */
size_t luaO_str2num(const char* s, TValue* o)
{
    lua_Integer i; lua_Number n;
    const char* e;
    if ((e = l_str2int(s, &i)) != NULL)
    {   /* 尝试转化为整形 */
        setivalue(o, i);
    }
    else if ((e = l_str2d(s, &n)) != NULL)
    {   /* 尝试转化为浮点数 */
        setfltvalue(o, n);
    }
    else
    {
        return 0;  /* 转化失败 */
    }
    return (e - s) + 1;  /* 成功, 返回字符串的长度 */
}

/* 将x转化为utf8模式 */
int luaO_utf8esc(char* buff, unsigned long x)
{
    int n = 1;  /* 放入缓冲区的字节数(向后) */
    lua_assert(x <= 0x10FFFF);
    if (x < 0x80)  /* ascii? */
    {
        buff[UTF8BUFFSZ - 1] = cast(char, x);
    }
    else
    {  /* need continuation bytes */
        unsigned int mfb = 0x3f;  /* maximum that fits in first byte */
        do
        {  /* add continuation bytes */
            buff[UTF8BUFFSZ - (n++)] = cast(char, 0x80 | (x & 0x3f));
            x >>= 6;  /* remove added bits */
            mfb >>= 1;  /* now there is one less bit available in first byte */
        } while (x > mfb);  /* still needs continuation byte? */
        buff[UTF8BUFFSZ - n] = cast(char, (~mfb << 1) | x);  /* add first byte */
    }
    return n;
}


/* 数字转换为字符串的最大长度 */
#define MAXNUMBER2STR	50

/*
** 将数字对象转换为字符串
*/
void luaO_tostring(lua_State* L, StkId obj)
{
    char buff[MAXNUMBER2STR];
    size_t len;
    lua_assert(ttisnumber(obj));
    if (ttisinteger(obj))
    {
        len = lua_integer2str(buff, sizeof(buff), ivalue(obj));
    }
    else
    {
        len = lua_number2str(buff, sizeof(buff), fltvalue(obj));
#if !defined(LUA_COMPAT_FLOATSTRING)
        if (buff[strspn(buff, "-0123456789")] == '\0')
        {   /* 浮点数可能也是整数? */
            buff[len++] = lua_getlocaledecpoint();
            buff[len++] = '0';  /* 在字符串末尾添加 '.0' */
        }
#endif
    }
    setsvalue2s(L, obj, luaS_newlstr(L, buff, len));
}

/*
** 创建字符串, 并将字符串压入栈顶, 同时增加栈顶
*/
static void pushstr(lua_State* L, const char* str, size_t l)
{
    setsvalue2s(L, L->top, luaS_newlstr(L, str, l));
    luaD_inctop(L);
}

/*
** 格式化字符串
** 此函数仅处理'%d', '%c', '%f', '%p'和'%s'常规格式, 以及
** 特定于Lua的'%I'和'%U'
*/
const char* luaO_pushvfstring(lua_State* L, const char* fmt, va_list argp)
{
    int n = 0;
    for (;;)
    {
        const char* e = strchr(fmt, '%');   /* 匹配'%', e="%..." */
        if (e == NULL)
        {
            break;  /* '%'解析完成 */
        }
        /* 小疑问: "%d%s", 是否是'%d'和'%s'中间的空字符也入栈了, 测试后, 入栈了 */
        pushstr(L, fmt, e - fmt);   /* 将'%'前面的所有字符压入栈中(栈顶) */
        switch (*(e + 1))
        {
        case 's':
        {   /* '\0'结束的字符串 */
            const char* s = va_arg(argp, char*);   /* 获取参数 */
            if (s == NULL)
            {
                s = "(null)";
            }
            pushstr(L, s, strlen(s));   /* 将参数压入栈中 */
            break;
        }
        case 'c':
        {   /* 单个字符 */
            char buff = cast(char, va_arg(argp, int));
            if (lisprint(cast_uchar(buff)))     /* 判断是否为可打印的字符 */
            {
                pushstr(L, &buff, 1);
            }
            else  /* 不可打印字符; 打印其编码 */
            {
                luaO_pushfstring(L, "<\\%d>", cast_uchar(buff));
            }
            break;
        }
        case 'd':
        {   /* 整数 */
            setivalue(L->top, va_arg(argp, int)); /* 将整形参数压入栈中, 未增长栈指针 */
            goto top2str;
        }
        case 'I':
        {   /* 'lua_Integer' */
            setivalue(L->top, cast(lua_Integer, va_arg(argp, l_uacInt)));   /* 同'd' */
            goto top2str;
        }
        case 'f':
        {  /* 'lua_Number' */
            setfltvalue(L->top, cast_num(va_arg(argp, l_uacNumber)));
        top2str:  /* 将top元素转换为字符串(即刚入栈的元素转化为字符串, 还保存在原先位置) */
            luaD_inctop(L);
            luaO_tostring(L, L->top - 1);   /* 转换 */
            break;
        }
        case 'p':
        {   /* 指针 */
            char buff[4 * sizeof(void*) + 8]; /* 应该有足够的空间容纳'%p' */
            void* p = va_arg(argp, void*);
            int l = lua_pointer2str(buff, sizeof(buff), p); /* 转换(l_sprintf) */
            pushstr(L, buff, l);    /* 入栈 */
            break;
        }
        case 'U':
        {   /* 一个'int'作为UTF-8序列 */
            char buff[UTF8BUFFSZ];
            int l = luaO_utf8esc(buff, cast(long, va_arg(argp, long))); /* 转换 */
            pushstr(L, buff + UTF8BUFFSZ - l, l); /* 入栈 */
            break;
        }
        case '%':
        {   /* % */
            pushstr(L, "%", 1);
            break;
        }
        default:
        {
            luaG_runerror(L, "invalid option '%%%c' to 'lua_pushfstring'",
                *(e + 1));    /* 错误 */
        }
        }
        n += 2; /* 表示压入栈中参数的个数, '%'前的字符, 和'%.'对应的参数 */
        fmt = e + 2;  /* 移动fmt */
    }
    luaD_checkstack(L, 1);
    pushstr(L, fmt, strlen(fmt));   /* 将最后剩余的字符串压入栈中 */
    if (n > 0)
    {
        luaV_concat(L, n + 1);      /* 将栈中的字符串进行拼接 */
    }
    return svalue(L->top - 1);      /* 返回拼接结果, 并没有减少栈顶高度 */
}

/*
** 格式化字符串, 不定参数, 类似于 printf
*/
const char* luaO_pushfstring(lua_State* L, const char* fmt, ...)
{
    const char* msg;
    va_list argp;
    va_start(argp, fmt);
    msg = luaO_pushvfstring(L, fmt, argp);
    va_end(argp);
    return msg;
}


/* 计算没有以'\0'结尾的字符串的长度 */
#define LL(x)	(sizeof(x)/sizeof(char) - 1)

#define RETS	"..."
#define PRE	"[string \""
#define POS	"\"]"

#define addstr(a,b,l)	( memcpy(a,b,(l) * sizeof(char)), a += (l) )

/*
** 根据 source 获取信息(lua 代码的来源), 同时防止缓冲区溢出
** 可以写一段有错误的 lua 代码, 输出的错误信息中最开头的部分就是
*/
void luaO_chunkid(char* out, const char* source, size_t bufflen)
{
    size_t l = strlen(source);
    /* 根据 source 进行分类 */
    if (*source == '=')     /* =stdin 命令行界面的输入 */
    {
        if (l <= bufflen)  /* 判断缓冲区的大小是否足够? 防止溢出 */
        {
            memcpy(out, source + 1, l * sizeof(char));
        }
        else
        {  /* out 大小不足时, 截断超出的字符 */
            addstr(out, source + 1, bufflen - 1);
            *out = '\0';
        }
    }
    else if (*source == '@')
    {   /* 文件输入 @file name */
        if (l <= bufflen)
        {
            memcpy(out, source + 1, l * sizeof(char));
        }
        else
        {   /* 在文件名前加 '...' 例如 ...bc.lua */
            addstr(out, RETS, LL(RETS));
            bufflen -= LL(RETS);
            memcpy(out, source + 1 + l - bufflen, bufflen * sizeof(char));
        }
    }
    else
    {   /* 输入的信息是 lua 代码, 非控制台输入 */
        /* 输出格式为: [string "source"] */
        const char* nl = strchr(source, '\n');  /* 找到第一行的信息 */
        addstr(out, PRE, LL(PRE));  /* 添加前缀 [string " */
        bufflen -= LL(PRE RETS POS) + 1;  /* 计算剩余长度(除去末尾的'\0') */
        if (l < bufflen && nl == NULL)
        {   /* lua 代码很短 */
            addstr(out, source, l);  /* 全部显示 */
        }
        else
        {
            if (nl != NULL)
            {
                l = nl - source;  /* 第一行的字符数量 */
            }
            if (l > bufflen)
            {
                l = bufflen;
            }
            addstr(out, source, l);
            addstr(out, RETS, LL(RETS));
        }
        memcpy(out, POS, (LL(POS) + 1) * sizeof(char));
    }
}

