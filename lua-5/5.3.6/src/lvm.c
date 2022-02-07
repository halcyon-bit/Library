/*
** $Id: lvm.c,v 2.268.1.1 2017/04/19 17:39:34 roberto Exp $
** Lua virtual machine
** See Copyright Notice in lua.h
*/

#define lvm_c
#define LUA_CORE

#include "lprefix.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"


/* 表循环次数限制 limit for table tag-method chains (to avoid loops) */
#define MAXTAGLOOP  2000



/*
** 'l_intfitsf'检查给定的整数是否可以转换为浮点数而不进行舍入.
** 用于比较. 如果所有整数都精确地适合浮点数, 则保留未定义.
*/
#if !defined(l_intfitsf)

/* 浮点尾数中的位数 */
#define NBM   (l_mathlim(MANT_DIG))

/*
** Check whether some integers may not fit in a float, that is, whether
** (maxinteger >> NBM) > 0 (that implies (1 << NBM) <= maxinteger).
** (The shifts are done in parts to avoid shifting by more than the size
** of an integer. In a worst case, NBM == 113 for long double and
** sizeof(integer) == 32.)
*/
#if ((((LUA_MAXINTEGER >> (NBM / 4)) >> (NBM / 4)) >> (NBM / 4)) \
	>> (NBM - (3 * (NBM / 4))))  >  0

#define l_intfitsf(i)  \
  (-((lua_Integer)1 << NBM) <= (i) && (i) <= ((lua_Integer)1 << NBM))

#endif

#endif



/* 尝试将一个值转化为float. 在宏tonumber中调用. */
int luaV_tonumber_(const TValue* obj, lua_Number* n)
{
    TValue v;
    if (ttisinteger(obj))   // obj是否为整数
    {
        *n = cast_num(ivalue(obj));
        return 1;
    }
    else if (cvt2num(obj) &&  /* 字符串可转换为数字? */
        luaO_str2num(svalue(obj), &v) == vslen(obj) + 1)
    {
        *n = nvalue(&v);  /* 将luaO_str2num的结果转化为float */
        return 1;
    }
    else
        return 0;  /* 转化失败 */
}


/*
** 尝试将一个值转化为整形, 根据'mode'进行舍入:
** mode == 0: 仅接受整形数值, 即obj为整数
** mode == 1: 采用向下取整规则(floor)
** mode == 2: 采用向上取整规则(ceil)
*/
int luaV_tointeger(const TValue* obj, lua_Integer* p, int mode)
{
    TValue v;
again:
    if (ttisfloat(obj))
    {
        lua_Number n = fltvalue(obj);
        lua_Number f = l_floor(n);
        if (n != f)
        {   /* obj不是整形数值? */
            if (mode == 0)
                return 0;       /* 如果模式为0, 则失败 */
            else if (mode > 1)  /* mode == 2? */
                f += 1;         /* 采用向上取整 (注: n != f) */
        }
        return lua_numbertointeger(f, p);
    }
    else if (ttisinteger(obj))
    {   /* obj为整形 */
        *p = ivalue(obj);
        return 1;
    }
    else if (cvt2num(obj) &&    /* obj为字符串, 先转化为float, 再次重新执行LuaV_tointeger函数 */
        luaO_str2num(svalue(obj), &v) == vslen(obj) + 1)
    {
        obj = &v;
        goto again;  /* 将结果从'luaO_str2num'转换为整数 */
    }
    return 0;  /* 转化失败 */
}


/*
** 尝试将'for'限制转换为整数, 保留循环的语义.
** (以下解释假定为非负步骤; 它比照适用于负步骤.)
** 如果限制可以转换为整数, 则向下舍入. 否则, 请检查限制是否可以转换为数字.
** 如果数字太大, 可以将限制设置为LUA_MAXINTEGER, 这意味着没有限制.
** 如果数字为负, 则循环不应该运行, 因为任何初始整数值都大于限制.
** 因此, 它设置了LUA_MININTEGER的限制. 当初始值为LUA_MININTEGER时, 'stopnow'会纠正极端情况,
** 在这种情况下, LUA_MININTEGER限制仍会运行一次循环.
*/
static int forlimit(const TValue* obj, lua_Integer* p, lua_Integer step,
    int* stopnow)
{
    *stopnow = 0;  /* 通常, 让循环运行 停止标志 */
    if (!luaV_tointeger(obj, p, (step < 0 ? 2 : 1)))
    {   /* 无法转换为整数 */
        lua_Number n;  /* 尝试转换为float */
        if (!tonumber(obj, &n)) /* 无法转换为float? */
        {
            return 0;  /* 不是实数 */
        }
        if (luai_numlt(0, n))
        {  /* if true, 循环超过了最大限制 */
            *p = LUA_MAXINTEGER;
            if (step < 0)
            {
                *stopnow = 1;
            }
        }
        else
        {   /* 比最小值还小 */
            *p = LUA_MININTEGER;
            if (step >= 0)
            {
                *stopnow = 1;
            }
        }
    }
    return 1;
}


/*
** 完成表的访问 'val = t[key]'
** 如果'slot'为NULL, 't'不是表格; 否则, 'slot'指向t[k](必须为nil)
*/
void luaV_finishget(lua_State* L, const TValue* t, TValue* key, StkId val,
    const TValue* slot)
{
    int loop;  /* 避免无限循环 */
    const TValue* tm;  /* 元方法 */
    for (loop = 0; loop < MAXTAGLOOP; loop++)
    {
        if (slot == NULL)
        {  /* 't' 不是table */
            lua_assert(!ttistable(t));
            tm = luaT_gettmbyobj(L, t, TM_INDEX);
            if (ttisnil(tm))
            {
                luaG_typeerror(L, t, "index");  /* no metamethod */
            }
            /* else will try the metamethod */
        }
        else
        {  /* 't' is a table */
            lua_assert(ttisnil(slot));
            tm = fasttm(L, hvalue(t)->metatable, TM_INDEX);  /* table's metamethod */
            if (tm == NULL)
            {  /* no metamethod? */
                setnilvalue(val);  /* result is nil */
                return;
            }
            /* else will try the metamethod */
        }
        if (ttisfunction(tm))
        {  /* is metamethod a function? */
            luaT_callTM(L, tm, t, key, val, 1);  /* call it */
            return;
        }
        t = tm;  /* else try to access 'tm[key]' */
        if (luaV_fastget(L, t, key, slot, luaH_get))
        {  /* fast track? */
            setobj2s(L, val, slot);  /* done */
            return;
        }
        /* else repeat (tail call 'luaV_finishget') */
    }
    luaG_runerror(L, "'__index' chain too long; possible loop");
}


/*
** 完成表的赋值 't[key] = val'
** 如果'slot'为NULL, 则't'不是表. 否则, 'slot'指向't[key]',
** 如果没有t[key]则指向'luaO_nilobject'.
** ('slot'的值必须为nil，否则'luaV_fastset'将完成这项工作.)
*/
void luaV_finishset(lua_State* L, const TValue* t, TValue* key,
    StkId val, const TValue* slot)
{
    int loop;  /* 避免无限循环 */
    for (loop = 0; loop < MAXTAGLOOP; loop++)
    {
        const TValue* tm;  /* '__newindex' metamethod */
        if (slot != NULL)
        {  /* is 't' a table? */
            Table* h = hvalue(t);  /* save 't' table */
            lua_assert(ttisnil(slot));  /* old value must be nil */
            tm = fasttm(L, h->metatable, TM_NEWINDEX);  /* get metamethod */
            if (tm == NULL)
            {  /* no metamethod? */
                if (slot == luaO_nilobject)  /* no previous entry? */
                {
                    slot = luaH_newkey(L, h, key);  /* create one */
                }
                /* no metamethod and (now) there is an entry with given key */
                setobj2t(L, cast(TValue*, slot), val);  /* set its new value */
                invalidateTMcache(h);
                luaC_barrierback(L, h, val);
                return;
            }
            /* else will try the metamethod */
        }
        else
        {  /* not a table; check metamethod */
            if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_NEWINDEX)))
                luaG_typeerror(L, t, "index");
        }
        /* try the metamethod */
        if (ttisfunction(tm))
        {
            luaT_callTM(L, tm, t, key, val, 0);
            return;
        }
        t = tm;  /* else repeat assignment over 'tm' */
        if (luaV_fastset(L, t, key, slot, luaH_get, val))
            return;  /* done */
        /* else loop */
    }
    luaG_runerror(L, "'__newindex' chain too long; possible loop");
}


/*
** Compare two strings 'ls' x 'rs', returning an integer smaller-equal-
** -larger than zero if 'ls' is smaller-equal-larger than 'rs'.
** The code is a little tricky because it allows '\0' in the strings
** and it uses 'strcoll' (to respect locales) for each segments
** of the strings.
*/
static int l_strcmp(const TString* ls, const TString* rs) {
    const char* l = getstr(ls);
    size_t ll = tsslen(ls);
    const char* r = getstr(rs);
    size_t lr = tsslen(rs);
    for (;;) {  /* for each segment */
        int temp = strcoll(l, r);
        if (temp != 0)  /* not equal? */
            return temp;  /* done */
        else {  /* strings are equal up to a '\0' */
            size_t len = strlen(l);  /* index of first '\0' in both strings */
            if (len == lr)  /* 'rs' is finished? */
                return (len == ll) ? 0 : 1;  /* check 'ls' */
            else if (len == ll)  /* 'ls' is finished? */
                return -1;  /* 'ls' is smaller than 'rs' ('rs' is not finished) */
              /* both strings longer than 'len'; go on comparing after the '\0' */
            len++;
            l += len; ll -= len; r += len; lr -= len;
        }
    }
}


/*
** Check whether integer 'i' is less than float 'f'. If 'i' has an
** exact representation as a float ('l_intfitsf'), compare numbers as
** floats. Otherwise, if 'f' is outside the range for integers, result
** is trivial. Otherwise, compare them as integers. (When 'i' has no
** float representation, either 'f' is "far away" from 'i' or 'f' has
** no precision left for a fractional part; either way, how 'f' is
** truncated is irrelevant.) When 'f' is NaN, comparisons must result
** in false.
*/
static int LTintfloat(lua_Integer i, lua_Number f) {
#if defined(l_intfitsf)
    if (!l_intfitsf(i)) {
        if (f >= -cast_num(LUA_MININTEGER))  /* -minint == maxint + 1 */
            return 1;  /* f >= maxint + 1 > i */
        else if (f > cast_num(LUA_MININTEGER))  /* minint < f <= maxint ? */
            return (i < cast(lua_Integer, f));  /* compare them as integers */
        else  /* f <= minint <= i (or 'f' is NaN)  -->  not(i < f) */
            return 0;
    }
#endif
    return luai_numlt(cast_num(i), f);  /* compare them as floats */
}


/*
** Check whether integer 'i' is less than or equal to float 'f'.
** See comments on previous function.
*/
static int LEintfloat(lua_Integer i, lua_Number f) {
#if defined(l_intfitsf)
    if (!l_intfitsf(i)) {
        if (f >= -cast_num(LUA_MININTEGER))  /* -minint == maxint + 1 */
            return 1;  /* f >= maxint + 1 > i */
        else if (f >= cast_num(LUA_MININTEGER))  /* minint <= f <= maxint ? */
            return (i <= cast(lua_Integer, f));  /* compare them as integers */
        else  /* f < minint <= i (or 'f' is NaN)  -->  not(i <= f) */
            return 0;
    }
#endif
    return luai_numle(cast_num(i), f);  /* compare them as floats */
}


/*
** Return 'l < r', for numbers.
*/
static int LTnum(const TValue* l, const TValue* r) {
    if (ttisinteger(l)) {
        lua_Integer li = ivalue(l);
        if (ttisinteger(r))
            return li < ivalue(r);  /* both are integers */
        else  /* 'l' is int and 'r' is float */
            return LTintfloat(li, fltvalue(r));  /* l < r ? */
    }
    else {
        lua_Number lf = fltvalue(l);  /* 'l' must be float */
        if (ttisfloat(r))
            return luai_numlt(lf, fltvalue(r));  /* both are float */
        else if (luai_numisnan(lf))  /* 'r' is int and 'l' is float */
            return 0;  /* NaN < i is always false */
        else  /* without NaN, (l < r)  <-->  not(r <= l) */
            return !LEintfloat(ivalue(r), lf);  /* not (r <= l) ? */
    }
}


/*
** Return 'l <= r', for numbers.
*/
static int LEnum(const TValue* l, const TValue* r) {
    if (ttisinteger(l)) {
        lua_Integer li = ivalue(l);
        if (ttisinteger(r))
            return li <= ivalue(r);  /* both are integers */
        else  /* 'l' is int and 'r' is float */
            return LEintfloat(li, fltvalue(r));  /* l <= r ? */
    }
    else {
        lua_Number lf = fltvalue(l);  /* 'l' must be float */
        if (ttisfloat(r))
            return luai_numle(lf, fltvalue(r));  /* both are float */
        else if (luai_numisnan(lf))  /* 'r' is int and 'l' is float */
            return 0;  /*  NaN <= i is always false */
        else  /* without NaN, (l <= r)  <-->  not(r < l) */
            return !LTintfloat(ivalue(r), lf);  /* not (r < l) ? */
    }
}


/*
** Main operation less than; return 'l < r'.
*/
int luaV_lessthan(lua_State* L, const TValue* l, const TValue* r) {
    int res;
    if (ttisnumber(l) && ttisnumber(r))  /* both operands are numbers? */
        return LTnum(l, r);
    else if (ttisstring(l) && ttisstring(r))  /* both are strings? */
        return l_strcmp(tsvalue(l), tsvalue(r)) < 0;
    else if ((res = luaT_callorderTM(L, l, r, TM_LT)) < 0)  /* no metamethod? */
        luaG_ordererror(L, l, r);  /* error */
    return res;
}


/*
** Main operation less than or equal to; return 'l <= r'. If it needs
** a metamethod and there is no '__le', try '__lt', based on
** l <= r iff !(r < l) (assuming a total order). If the metamethod
** yields during this substitution, the continuation has to know
** about it (to negate the result of r<l); bit CIST_LEQ in the call
** status keeps that information.
*/
int luaV_lessequal(lua_State* L, const TValue* l, const TValue* r) {
    int res;
    if (ttisnumber(l) && ttisnumber(r))  /* both operands are numbers? */
        return LEnum(l, r);
    else if (ttisstring(l) && ttisstring(r))  /* both are strings? */
        return l_strcmp(tsvalue(l), tsvalue(r)) <= 0;
    else if ((res = luaT_callorderTM(L, l, r, TM_LE)) >= 0)  /* try 'le' */
        return res;
    else {  /* try 'lt': */
        L->ci->callstatus |= CIST_LEQ;  /* mark it is doing 'lt' for 'le' */
        res = luaT_callorderTM(L, r, l, TM_LT);
        L->ci->callstatus ^= CIST_LEQ;  /* clear mark */
        if (res < 0)
            luaG_ordererror(L, l, r);
        return !res;  /* result is negated */
    }
}


/*
** Lua值相等的主要操作; 返回't1 == t2'.
** L == NULL表示原始相等（没有metamethods）
*/
int luaV_equalobj(lua_State* L, const TValue* t1, const TValue* t2)
{
    const TValue* tm;
    /* 如果类型(含子类型)不同 */
    if (ttype(t1) != ttype(t2))
    {   /* 如果大类型不同或大类型不是数字类型? */
        if (ttnov(t1) != ttnov(t2) || ttnov(t1) != LUA_TNUMBER)
            return 0;  /* 只有两个不同类型的数字型数据可以进行对比 */
        else
        {   /* 不同类型的两个数字型数据 */
            lua_Integer i1, i2;  /* 将其全部转化为整形, 在进行对比操作 */
            return (tointeger(t1, &i1) && tointeger(t2, &i2) && i1 == i2);
        }
    }
    /* 值具有相同的类型 */
    switch (ttype(t1))
    {
    case LUA_TNIL:
        return 1;
    case LUA_TNUMINT:
        return (ivalue(t1) == ivalue(t2));
    case LUA_TNUMFLT:
        return luai_numeq(fltvalue(t1), fltvalue(t2));
    case LUA_TBOOLEAN:
        return bvalue(t1) == bvalue(t2);  /* true 只能为1 !! */
    case LUA_TLIGHTUSERDATA:
        return pvalue(t1) == pvalue(t2);
    case LUA_TLCF:
        return fvalue(t1) == fvalue(t2);
        /* 根据子类型不同，用不同字符串比较策略进行比较 */
    case LUA_TSHRSTR:
        return eqshrstr(tsvalue(t1), tsvalue(t2));
    case LUA_TLNGSTR:
        return luaS_eqlngstr(tsvalue(t1), tsvalue(t2));
    case LUA_TUSERDATA:
    {
        if (uvalue(t1) == uvalue(t2))
            return 1;
        else if (L == NULL)
            return 0;
        tm = fasttm(L, uvalue(t1)->metatable, TM_EQ);
        if (tm == NULL)
            tm = fasttm(L, uvalue(t2)->metatable, TM_EQ);
        break;  /* 尝试TM */
    }
    case LUA_TTABLE:
    {
        if (hvalue(t1) == hvalue(t2))
            return 1;
        else if (L == NULL)
            return 0;
        tm = fasttm(L, hvalue(t1)->metatable, TM_EQ);
        if (tm == NULL)
            tm = fasttm(L, hvalue(t2)->metatable, TM_EQ);
        break;  /* 尝试TM */
    }
    default:
        return gcvalue(t1) == gcvalue(t2);
    }
    if (tm == NULL)  /* 没有TM? */
        return 0;    /* 两个对象不相同 */
    luaT_callTM(L, tm, t1, t2, L->top, 1);  /* 调用TM */
    return !l_isfalse(L->top);
}


/* 'luaV_concat'使用该宏来确保'o'是一个字符串 */
#define tostring(L,o)  \
  (ttisstring(o) || (cvt2str(o) && (luaO_tostring(L, o), 1)))

/* 判断'o'是否是一个空字符串 */
#define isemptystr(o) (ttisshrstring(o) && tsvalue(o)->shrlen == 0)

/* 将栈中的字符串从top - n 到 top - 1 拷贝到buffer中 */
static void copy2buff(StkId top, int n, char* buff)
{
    size_t tl = 0;  /* 已经复制的长度 */
    do {
        size_t l = vslen(top - n);  /* 被复制的字符串长度 */
        memcpy(buff + tl, svalue(top - n), l * sizeof(char));
        tl += l;
    } while (--n > 0);
}


/*
** 拼接的主要操作: 在堆栈中总数为'total'的值
** 从"L->top-total"到"L->top-1"
*/
void luaV_concat(lua_State* L, int total)
{
    lua_assert(total >= 2);     /* total大于等于2 */
    do
    {
        StkId top = L->top;
        int n = 2;  /* 此阶段处理的元素数(至少2个) */
        if (!(ttisstring(top - 2) || cvt2str(top - 2)) || !tostring(L, top - 1))
            luaT_trybinTM(L, top - 2, top - 1, top - 2, TM_CONCAT);   /* 若不是字符串、数字或者无法转换为字符串, 尝试TM方法 */
        else if (isemptystr(top - 1))  /* 第二个操作数是否为空? */
            cast_void(tostring(L, top - 2));  /* 结果是第一个操作数 */
        else if (isemptystr(top - 2))
        {   /* 第一个操作数是一个空字符串? */
            setobjs2s(L, top - 2, top - 1);  /* 结果是第二个操作数. */
        }
        else
        {
            /* 至少两个非空字符串值; 得到尽可能多的 */
            size_t tl = vslen(top - 1);     /* 字符串长度 */
            TString* ts;
            /* 所有字符串的总长度(tl)和字符串个数(n) */
            for (n = 1; n < total && tostring(L, top - n - 1); n++)
            {
                size_t l = vslen(top - n - 1);
                if (l >= (MAX_SIZE / sizeof(char)) - tl)
                    luaG_runerror(L, "string length overflow");
                tl += l;
            }
            if (tl <= LUAI_MAXSHORTLEN)
            {   /* 是一个短字符串? */
                char buff[LUAI_MAXSHORTLEN];
                copy2buff(top, n, buff);  /* 将所有字符串拼接到buff */
                ts = luaS_newlstr(L, buff, tl); /* 创建LUA_STRING */
            }
            else
            {   /* 长字符串; 直接将字符串复制到最终结果 */
                ts = luaS_createlngstrobj(L, tl);
                copy2buff(top, n, getstr(ts));
            }
            setsvalue2s(L, top - n, ts);  /* 将结果放在栈中(位置: top-n) */
        }
        total -= n - 1;   /* 使用'n'个字符串创建了1个新字符串 */
        L->top -= n - 1;  /* 弹出'n'字符串并压入一个字符串 */
    } while (total > 1); /* 重复直到只剩下1个结果 */
}


/*
** 获取长度, 主要操作为 'ra = #rb'.
*/
void luaV_objlen(lua_State* L, StkId ra, const TValue* rb) {
    const TValue* tm;
    switch (ttype(rb)) {
    case LUA_TTABLE: {
        Table* h = hvalue(rb);
        tm = fasttm(L, h->metatable, TM_LEN);
        if (tm) break; /* 元方法? 跳出switch执行 */
        setivalue(ra, luaH_getn(h));  /* 调用原本的 */
        return;
    }
    case LUA_TSHRSTR: {
        setivalue(ra, tsvalue(rb)->shrlen);
        return;
    }
    case LUA_TLNGSTR: {
        setivalue(ra, tsvalue(rb)->u.lnglen);
        return;
    }
    default: {  /* 其他类型, 尝试元方法 */
        tm = luaT_gettmbyobj(L, rb, TM_LEN);
        if (ttisnil(tm))  /* 没有元方法? */
            luaG_typeerror(L, rb, "get length of");
        break;
    }
    }
    luaT_callTM(L, tm, rb, rb, ra, 1);
}


/*
** Integer division; return 'm // n', that is, floor(m/n).
** C division truncates its result (rounds towards zero).
** 'floor(q) == trunc(q)' when 'q >= 0' or when 'q' is integer,
** otherwise 'floor(q) == trunc(q) - 1'.
*/
lua_Integer luaV_div(lua_State* L, lua_Integer m, lua_Integer n) {
    if (l_castS2U(n) + 1u <= 1u) {  /* special cases: -1 or 0 */
        if (n == 0)
            luaG_runerror(L, "attempt to divide by zero");
        return intop(-, 0, m);   /* n==-1; avoid overflow with 0x80000...//-1 */
    }
    else {
        lua_Integer q = m / n;  /* perform C division */
        if ((m ^ n) < 0 && m % n != 0)  /* 'm/n' would be negative non-integer? */
            q -= 1;  /* correct result for different rounding */
        return q;
    }
}


/*
** Integer modulus; return 'm % n'. (Assume that C '%' with
** negative operands follows C99 behavior. See previous comment
** about luaV_div.)
*/
lua_Integer luaV_mod(lua_State* L, lua_Integer m, lua_Integer n) {
    if (l_castS2U(n) + 1u <= 1u) {  /* special cases: -1 or 0 */
        if (n == 0)
            luaG_runerror(L, "attempt to perform 'n%%0'");
        return 0;   /* m % -1 == 0; avoid overflow with 0x80000...%-1 */
    }
    else {
        lua_Integer r = m % n;
        if (r != 0 && (m ^ n) < 0)  /* 'm/n' would be non-integer negative? */
            r += n;  /* correct result for different rounding */
        return r;
    }
}


/* number of bits in an integer */
#define NBITS	cast_int(sizeof(lua_Integer) * CHAR_BIT)

/*
** Shift left operation. (Shift right just negates 'y'.)
*/
lua_Integer luaV_shiftl(lua_Integer x, lua_Integer y) {
    if (y < 0) {  /* shift right? */
        if (y <= -NBITS) return 0;
        else return intop(>> , x, -y);
    }
    else {  /* shift left */
        if (y >= NBITS) return 0;
        else return intop(<< , x, y);
    }
}


/*
** check whether cached closure in prototype 'p' may be reused, that is,
** whether there is a cached closure with the same upvalues needed by
** new closure to be created.
*/
static LClosure* getcached(Proto* p, UpVal** encup, StkId base) {
    LClosure* c = p->cache;
    if (c != NULL) {  /* is there a cached closure? */
        int nup = p->sizeupvalues;
        Upvaldesc* uv = p->upvalues;
        int i;
        for (i = 0; i < nup; i++) {  /* check whether it has right upvalues */
            TValue* v = uv[i].instack ? base + uv[i].idx : encup[uv[i].idx]->v;
            if (c->upvals[i]->v != v)
                return NULL;  /* wrong upvalue; cannot reuse closure */
        }
    }
    return c;  /* return cached closure (or NULL if no cached closure) */
}


/*
** 创建一个新的lua闭包, 将其压入栈中并且初始化upvalues.
**
** create a new Lua closure, push it in the stack, and initialize
** its upvalues. Note that the closure is not cached if prototype is
** already black (which means that 'cache' was already cleared by the
** GC).
创建一个新的Lua闭包，将其推入堆栈，并初始化其upvalues。 请注意，如果原型已经是黑色，则不会缓存闭包（这意味着GC已经清除'缓存'）。
*/
static void pushclosure(lua_State* L, Proto* p, UpVal** encup, StkId base,
    StkId ra) {
    int nup = p->sizeupvalues;
    Upvaldesc* uv = p->upvalues;
    int i;
    LClosure* ncl = luaF_newLclosure(L, nup);
    ncl->p = p;
    setclLvalue(L, ra, ncl);  /* anchor new closure in stack */
    for (i = 0; i < nup; i++) {  /* fill in its upvalues */
        if (uv[i].instack)  /* upvalue refers to local variable? */
            ncl->upvals[i] = luaF_findupval(L, base + uv[i].idx);
        else  /* get upvalue from enclosing function */
            ncl->upvals[i] = encup[uv[i].idx];
        ncl->upvals[i]->refcount++;
        /* new closure is white, so we do not need a barrier here */
    }
    if (!isblack(p))  /* cache will not break GC invariant? */
        p->cache = ncl;  /* save it on cache for reuse */
}


/*
** finish execution of an opcode interrupted by an yield
*/
void luaV_finishOp(lua_State* L) {
    CallInfo* ci = L->ci;
    StkId base = ci->u.l.base;
    Instruction inst = *(ci->u.l.savedpc - 1);  /* interrupted instruction */
    OpCode op = GET_OPCODE(inst);
    switch (op) {  /* finish its execution */
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_IDIV:
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
    case OP_MOD: case OP_POW:
    case OP_UNM: case OP_BNOT: case OP_LEN:
    case OP_GETTABUP: case OP_GETTABLE: case OP_SELF: {
        setobjs2s(L, base + GETARG_A(inst), --L->top);
        break;
    }
    case OP_LE: case OP_LT: case OP_EQ: {
        int res = !l_isfalse(L->top - 1);
        L->top--;
        if (ci->callstatus & CIST_LEQ) {  /* "<=" using "<" instead? */
            lua_assert(op == OP_LE);
            ci->callstatus ^= CIST_LEQ;  /* clear mark */
            res = !res;  /* negate result */
        }
        lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_JMP);
        if (res != GETARG_A(inst))  /* condition failed? */
            ci->u.l.savedpc++;  /* skip jump instruction */
        break;
    }
    case OP_CONCAT: {
        StkId top = L->top - 1;  /* top when 'luaT_trybinTM' was called */
        int b = GETARG_B(inst);      /* first element to concatenate */
        int total = cast_int(top - 1 - (base + b));  /* yet to concatenate */
        setobj2s(L, top - 2, top);  /* put TM result in proper position */
        if (total > 1) {  /* are there elements to concat? */
            L->top = top - 1;  /* top is one after last element (at top-2) */
            luaV_concat(L, total);  /* concat them (may yield again) */
        }
        /* move final result to final position */
        setobj2s(L, ci->u.l.base + GETARG_A(inst), L->top - 1);
        L->top = ci->top;  /* restore top */
        break;
    }
    case OP_TFORCALL: {
        lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_TFORLOOP);
        L->top = ci->top;  /* correct top */
        break;
    }
    case OP_CALL: {
        if (GETARG_C(inst) - 1 >= 0)  /* nresults >= 0? */
            L->top = ci->top;  /* adjust results */
        break;
    }
    case OP_TAILCALL: case OP_SETTABUP: case OP_SETTABLE:
        break;
    default: lua_assert(0);
    }
}




/*
** {==================================================================
** Function 'luaV_execute': main interpreter loop
** ===================================================================
*/


/*
** 'luaV_execute'中常见任务的一些宏
*/

/* 获取A寄存器中值在栈中的位置 */
#define RA(i) (base+GETARG_A(i))
/* 获取B寄存器中值在栈中的位置 */
#define RB(i) check_exp(getBMode(GET_OPCODE(i)) == OpArgR, base+GETARG_B(i))
/* 获取C寄存器中值在栈中的位置 */
#define RC(i) check_exp(getCMode(GET_OPCODE(i)) == OpArgR, base+GETARG_C(i))
#define RKB(i)  check_exp(getBMode(GET_OPCODE(i)) == OpArgK, \
  ISK(GETARG_B(i)) ? k+INDEXK(GETARG_B(i)) : base+GETARG_B(i))
#define RKC(i)  check_exp(getCMode(GET_OPCODE(i)) == OpArgK, \
  ISK(GETARG_C(i)) ? k+INDEXK(GETARG_C(i)) : base+GETARG_C(i))


/* 执行跳转指令 */
#define dojump(ci,i,e) \
  { int a = GETARG_A(i); \
    if (a != 0) luaF_close(L, ci->u.l.base + a - 1); \
    ci->u.l.savedpc += GETARG_sBx(i) + e; }

/* 对于测试指令，执行其后的跳转指令 */
#define donextjump(ci)  { i = *ci->u.l.savedpc; dojump(ci, i, 1); }


#define Protect(x)	{ {x;}; base = ci->u.l.base; }

#define checkGC(L,c)  \
	{ luaC_condGC(L, L->top = (c),  /* limit of live values */ \
                         Protect(L->top = ci->top));  /* restore top */ \
           luai_threadyield(L); }


/* fetch an instruction and prepare its execution */
#define vmfetch()	{ \
  i = *(ci->u.l.savedpc++); \
  if (L->hookmask & (LUA_MASKLINE | LUA_MASKCOUNT)) \
    Protect(luaG_traceexec(L)); \
  ra = RA(i); /* WARNING: any stack reallocation invalidates 'ra' */ \
  lua_assert(base == ci->u.l.base); \
  lua_assert(base <= L->top && L->top < L->stack + L->stacksize); \
}

#define vmdispatch(o)	switch(o)
#define vmcase(l)	case l:
#define vmbreak		break


/*
** copy of 'luaV_gettable', but protecting the call to potential
** metamethod (which can reallocate the stack)
*/
#define gettableProtected(L,t,k,v)  { const TValue *slot; \
  if (luaV_fastget(L,t,k,slot,luaH_get)) { setobj2s(L, v, slot); } \
  else Protect(luaV_finishget(L,t,k,v,slot)); }


/* same for 'luaV_settable' */
#define settableProtected(L,t,k,v) { const TValue *slot; \
  if (!luaV_fastset(L,t,k,slot,luaH_get,v)) \
    Protect(luaV_finishset(L,t,k,v,slot)); }



/*
** luaV_execute是Lua VM执行一段字节码的入口. Lua VM就是一个状态机,
** 从当前调用栈上次运行点开始解释字节码指令, 直到下一个C边界跳出点
** (可以是函数执行完毕, 也可以是一次协程 yield 操作).
** 每一次进入或退出一层Lua函数, luaV_execute并不会产生一次C层面的函数调用.
** 也就是说, 从Lua函数中调用另一个Lua函数, 并不会产生一次独立的luaV_execute调用.
*/
void luaV_execute(lua_State* L)
{
    CallInfo* ci = L->ci;   /* 当前调用的函数 */
    LClosure* cl;
    TValue* k;
    StkId base;
    ci->callstatus |= CIST_FRESH;  /* 新的"luaV_execute"调用 */
newframe:  /* reentry point when frame changes (call/return) */
    lua_assert(ci == L->ci);
    cl = clLvalue(ci->func);  /* 一个Lua闭包类型, 放置调用栈中当前函数对象, 从ci->func中获取 */
    k = cl->p->k;  /* 当前函数的常量表 */
    base = ci->u.l.base;  /* 当前数据栈 栈底的位置 */
    /* luaV_execute 解释字节码的过程也就是利用一个死循环, 依次解析字节码指令, 当前指令 i 从 ci->u.l.savepc 中获取 */
    /* 所有的指令都会操作寄存器 A, 从 Lua VM 的角度看, 寄存器就是数据栈上的变量, 所以可以将寄存器 A 所指变量预先取出放到局部变量ra中.
       ra = RA(i). 某些指令操作在 vm 运行过程中会改变数据栈的大小(伸缩), 而 ra 是一个指向数据栈的指针, 而不是一个索引.
       这种情况下, 一旦数据栈发生变化, 就需要重新获取ra的值.
       同理, base 变量是一个指向数据栈栈底的指针, 也会因为某些指令操作发生变化. 这个时候就需要重新对 base 重新赋值.
       栈底 base作为基址是一个参考量, 一直需要使用, 所以重置 base 的值很频繁, Lua 提供了一个 Protect 宏, 将重置base的操作包裹起来.
    */
    for (;;) {
        Instruction i;
        StkId ra;
        vmfetch();
        vmdispatch(GET_OPCODE(i)) {
            vmcase(OP_MOVE) {
                setobjs2s(L, ra, RB(i));
                vmbreak;
            }
            vmcase(OP_LOADK) {
                TValue* rb = k + GETARG_Bx(i);
                setobj2s(L, ra, rb);
                vmbreak;
            }
            vmcase(OP_LOADKX) {
                TValue* rb;
                lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_EXTRAARG);
                rb = k + GETARG_Ax(*ci->u.l.savedpc++);
                setobj2s(L, ra, rb);
                vmbreak;
            }
            vmcase(OP_LOADBOOL) {
                setbvalue(ra, GETARG_B(i));
                if (GETARG_C(i)) ci->u.l.savedpc++;  /* skip next instruction (if C) */
                vmbreak;
            }
            vmcase(OP_LOADNIL) {
                int b = GETARG_B(i);
                do {
                    setnilvalue(ra++);
                } while (b--);
                vmbreak;
            }
            vmcase(OP_GETUPVAL) {
                int b = GETARG_B(i);
                setobj2s(L, ra, cl->upvals[b]->v);
                vmbreak;
            }
            vmcase(OP_GETTABUP) {
                TValue* upval = cl->upvals[GETARG_B(i)]->v;
                TValue* rc = RKC(i);
                gettableProtected(L, upval, rc, ra);
                vmbreak;
            }
            vmcase(OP_GETTABLE) {
                StkId rb = RB(i);
                TValue* rc = RKC(i);
                gettableProtected(L, rb, rc, ra);
                vmbreak;
            }
            vmcase(OP_SETTABUP) {
                TValue* upval = cl->upvals[GETARG_A(i)]->v;
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                settableProtected(L, upval, rb, rc);
                vmbreak;
            }
            vmcase(OP_SETUPVAL) {
                UpVal* uv = cl->upvals[GETARG_B(i)];
                setobj(L, uv->v, ra);
                luaC_upvalbarrier(L, uv);
                vmbreak;
            }
            vmcase(OP_SETTABLE) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                settableProtected(L, ra, rb, rc);
                vmbreak;
            }
            vmcase(OP_NEWTABLE)
            {   // 新建表，使用 A、B、C 三个指令参数，A 用来接收创建的 table，
                // B 和 C 用于为新创建的 table 预分配 array 部分大小和 hash 部分大小。
                int b = GETARG_B(i);    // 获取 B 指令参数的值
                int c = GETARG_C(i);    // 获取 C 指令参数的值
                Table* t = luaH_new(L); // 创建 table
                sethvalue(L, ra, t);    // 将 table 保存在 A 中
                if (b != 0 || c != 0)   // 判断是否有预设表的大小
                    luaH_resize(L, t, luaO_fb2int(b), luaO_fb2int(c));  // 设置表的 array 和 hash 的大小
                checkGC(L, ra + 1);     // 如果有未偿还的债务，就执行一次 luaC_step
                vmbreak;
            }
            vmcase(OP_SELF) {
                const TValue* aux;
                StkId rb = RB(i);
                TValue* rc = RKC(i);
                TString* key = tsvalue(rc);  /* key must be a string */
                setobjs2s(L, ra + 1, rb);
                if (luaV_fastget(L, rb, key, aux, luaH_getstr)) {
                    setobj2s(L, ra, aux);
                }
                else Protect(luaV_finishget(L, rb, rc, ra, aux));
                vmbreak;
            }
            vmcase(OP_ADD) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Number nb; lua_Number nc;
                if (ttisinteger(rb) && ttisinteger(rc)) {
                    lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
                    setivalue(ra, intop(+, ib, ic));
                }
                else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
                    setfltvalue(ra, luai_numadd(L, nb, nc));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_ADD)); }
                vmbreak;
            }
            vmcase(OP_SUB) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Number nb; lua_Number nc;
                if (ttisinteger(rb) && ttisinteger(rc)) {
                    lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
                    setivalue(ra, intop(-, ib, ic));
                }
                else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
                    setfltvalue(ra, luai_numsub(L, nb, nc));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_SUB)); }
                vmbreak;
            }
            vmcase(OP_MUL) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Number nb; lua_Number nc;
                if (ttisinteger(rb) && ttisinteger(rc)) {
                    lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
                    setivalue(ra, intop(*, ib, ic));
                }
                else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
                    setfltvalue(ra, luai_nummul(L, nb, nc));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_MUL)); }
                vmbreak;
            }
            vmcase(OP_DIV) {  /* float division (always with floats) */
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Number nb; lua_Number nc;
                if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
                    setfltvalue(ra, luai_numdiv(L, nb, nc));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_DIV)); }
                vmbreak;
            }
            vmcase(OP_BAND) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Integer ib; lua_Integer ic;
                if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
                    setivalue(ra, intop(&, ib, ic));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_BAND)); }
                vmbreak;
            }
            vmcase(OP_BOR) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Integer ib; lua_Integer ic;
                if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
                    setivalue(ra, intop(| , ib, ic));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_BOR)); }
                vmbreak;
            }
            vmcase(OP_BXOR) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Integer ib; lua_Integer ic;
                if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
                    setivalue(ra, intop(^, ib, ic));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_BXOR)); }
                vmbreak;
            }
            vmcase(OP_SHL) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Integer ib; lua_Integer ic;
                if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
                    setivalue(ra, luaV_shiftl(ib, ic));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_SHL)); }
                vmbreak;
            }
            vmcase(OP_SHR) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Integer ib; lua_Integer ic;
                if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
                    setivalue(ra, luaV_shiftl(ib, -ic));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_SHR)); }
                vmbreak;
            }
            vmcase(OP_MOD) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Number nb; lua_Number nc;
                if (ttisinteger(rb) && ttisinteger(rc)) {
                    lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
                    setivalue(ra, luaV_mod(L, ib, ic));
                }
                else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
                    lua_Number m;
                    luai_nummod(L, nb, nc, m);
                    setfltvalue(ra, m);
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_MOD)); }
                vmbreak;
            }
            vmcase(OP_IDIV) {  /* floor division */
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Number nb; lua_Number nc;
                if (ttisinteger(rb) && ttisinteger(rc)) {
                    lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
                    setivalue(ra, luaV_div(L, ib, ic));
                }
                else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
                    setfltvalue(ra, luai_numidiv(L, nb, nc));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_IDIV)); }
                vmbreak;
            }
            vmcase(OP_POW) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                lua_Number nb; lua_Number nc;
                if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
                    setfltvalue(ra, luai_numpow(L, nb, nc));
                }
                else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_POW)); }
                vmbreak;
            }
            vmcase(OP_UNM) {
                TValue* rb = RB(i);
                lua_Number nb;
                if (ttisinteger(rb)) {
                    lua_Integer ib = ivalue(rb);
                    setivalue(ra, intop(-, 0, ib));
                }
                else if (tonumber(rb, &nb)) {
                    setfltvalue(ra, luai_numunm(L, nb));
                }
                else {
                    Protect(luaT_trybinTM(L, rb, rb, ra, TM_UNM));
                }
                vmbreak;
            }
            vmcase(OP_BNOT) {
                TValue* rb = RB(i);
                lua_Integer ib;
                if (tointeger(rb, &ib)) {
                    setivalue(ra, intop(^, ~l_castS2U(0), ib));
                }
                else {
                    Protect(luaT_trybinTM(L, rb, rb, ra, TM_BNOT));
                }
                vmbreak;
            }
            vmcase(OP_NOT) {
                TValue* rb = RB(i);
                int res = l_isfalse(rb);  /* next assignment may change this value */
                setbvalue(ra, res);
                vmbreak;
            }
            vmcase(OP_LEN) {
                Protect(luaV_objlen(L, ra, RB(i)));
                vmbreak;
            }
            /* 将R(B)到R(C)之间的所有值, 都以字符串方式连接起来, 把结果放到R(A)中. 这个连接过程是通过 luaV_concat 函数完成的.
                  函数逻辑:
                  1. 通过临时修改栈顶地址为 C, 然后连接 R(B) 到 R(C) 的值, 将结果临时存在 R(B) 中.
                  2. 再将R(B) 复制到R(A), 最后将栈顶位置调整回去.
                  3. R(B)到R(C) 以及之后的寄存器, 不能被后续指令读取. 也就是说R(B) 与R(C) 寄存器必须在栈顶工作.
                  由于字符串连接操作, 可能会触发元方法, 导致数据栈空间扩展. 所以必须在luaV_concat 函数调用完后重新获取 ra = RA(i) (因为ra不再指向原来的位置).
                  在OP_CONCAT 操作的最后, 重置了数据栈的栈顶.

                  Lua字节码以寄存器的方法来理解数据栈空间, 在大多数情况下, 用到多少寄存器是在编译期生成字节码的时候决定的.
                  所以在函数原型Proto 结构里有 maxstacksize 这个信息, 同时在运行时, 会把这段空间的top 记录在 CallInfo->top 中.
                  Lua VM 在运行时会以堆栈的方法利用这个数据栈, 这种栈形式利用数据堆栈都是临时行为, 使用完毕后应该重置数据栈栈顶.
              */
            vmcase(OP_CONCAT) {
                int b = GETARG_B(i);
                int c = GETARG_C(i);
                StkId rb;
                L->top = base + c + 1;  /* mark the end of concat operands */
                Protect(luaV_concat(L, c - b + 1));
                ra = RA(i);  /* 'luaV_concat' may invoke TMs and move the stack */
                rb = base + b;
                setobjs2s(L, ra, rb);
                checkGC(L, (ra >= rb ? ra + 1 : rb));
                L->top = ci->top;  /* restore top */
                vmbreak;
            }
            /* 逻辑和关系指令被统称为 test指令. JMP指令配合test指令来实现程序的条件跳转.
                 每个 test指令与 JMP指令配合, 都会将接下来生成的指令分为两个集合, 满足条件的为true集合, 否则为false集合.
                 当 test条件满足时, 则继续执行接下来的 JMP指令, 然后跳转到分支代码.
                 当 test条件不满足时候, PC + 1, 跳过紧跟的JMP指令, 然后继续执行.
             */
            vmcase(OP_JMP) {
                dojump(ci, i, 0);
                vmbreak;
            }
            /* EQ LT LE TEST TESTST 五个分支指令必须与后面的紧挨的JMP指令看做一体. 当条件成立时, 继续运行; 条件不成立时, 跳转到指定位置. */
              /* EQ LT LE 关系指令对 RK(B) 和 RK(C) 进行比较, 然后将比较结果与 A 寄存器指定的boolean值进行比较, 来决定最终的 boolean 值. */
            vmcase(OP_EQ) {
                TValue* rb = RKB(i);
                TValue* rc = RKC(i);
                Protect(
                    if (luaV_equalobj(L, rb, rc) != GETARG_A(i))
                        ci->u.l.savedpc++;
                    else
                        donextjump(ci);
                )
                    vmbreak;
            }
            vmcase(OP_LT) {
                Protect(
                    if (luaV_lessthan(L, RKB(i), RKC(i)) != GETARG_A(i))
                        ci->u.l.savedpc++;
                    else
                        donextjump(ci);
                )
                    vmbreak;
            }
            vmcase(OP_LE) {
                Protect(
                    if (luaV_lessequal(L, RKB(i), RKC(i)) != GETARG_A(i))
                        ci->u.l.savedpc++;
                    else
                        donextjump(ci);
                )
                    vmbreak;
            }
            /* TEST TESTSET 逻辑指令用于实现and和or逻辑运算符, 或者在条件语句中判断一个寄存器.
                  TESTSET将寄存器B转化成一个boolean值, 然后与C进行比较.
                  如果相等, 则将寄存器B的值赋给寄存器A, 然后继续执行; 反之如果不相等, 则跳过后面的JMP指令. */
            vmcase(OP_TEST) {
                if (GETARG_C(i) ? l_isfalse(ra) : !l_isfalse(ra))
                    ci->u.l.savedpc++;
                else
                    donextjump(ci);
                vmbreak;
            }
            vmcase(OP_TESTSET) {
                TValue* rb = RB(i);
                if (GETARG_C(i) ? l_isfalse(rb) : !l_isfalse(rb))
                    ci->u.l.savedpc++;
                else {
                    setobjs2s(L, ra, rb);
                    donextjump(ci);
                }
                vmbreak;
            }
            vmcase(OP_CALL) {
                int b = GETARG_B(i);
                int nresults = GETARG_C(i) - 1;
                if (b != 0) L->top = ra + b;  /* else previous instruction set top */
                if (luaD_precall(L, ra, nresults)) {  /* C function? */
                    if (nresults >= 0)
                        L->top = ci->top;  /* adjust results */
                    Protect((void)0);  /* update 'base' */
                }
                else {  /* Lua function */
                    ci = L->ci;
                    goto newframe;  /* restart luaV_execute over new Lua function */
                }
                vmbreak;
            }
            vmcase(OP_TAILCALL) {
                int b = GETARG_B(i);
                if (b != 0) L->top = ra + b;  /* else previous instruction set top */
                lua_assert(GETARG_C(i) - 1 == LUA_MULTRET);
                if (luaD_precall(L, ra, LUA_MULTRET)) {  /* C function? */
                    Protect((void)0);  /* update 'base' */
                }
                else {
                    /* tail call: put called frame (n) in place of caller one (o) */
                    CallInfo* nci = L->ci;  /* called frame */
                    CallInfo* oci = nci->previous;  /* caller frame */
                    StkId nfunc = nci->func;  /* called function */
                    StkId ofunc = oci->func;  /* caller function */
                    /* last stack slot filled by 'precall' */
                    StkId lim = nci->u.l.base + getproto(nfunc)->numparams;
                    int aux;
                    /* close all upvalues from previous call */
                    if (cl->p->sizep > 0) luaF_close(L, oci->u.l.base);
                    /* move new frame into old one */
                    for (aux = 0; nfunc + aux < lim; aux++)
                        setobjs2s(L, ofunc + aux, nfunc + aux);
                    oci->u.l.base = ofunc + (nci->u.l.base - nfunc);  /* correct base */
                    oci->top = L->top = ofunc + (L->top - nfunc);  /* correct top */
                    oci->u.l.savedpc = nci->u.l.savedpc;
                    oci->callstatus |= CIST_TAIL;  /* function was tail called */
                    ci = L->ci = oci;  /* remove new frame */
                    lua_assert(L->top == oci->u.l.base + getproto(ofunc)->maxstacksize);
                    goto newframe;  /* restart luaV_execute over new Lua function */
                }
                vmbreak;
            }
            vmcase(OP_RETURN) {
                int b = GETARG_B(i);
                if (cl->p->sizep > 0) luaF_close(L, base);
                b = luaD_poscall(L, ci, ra, (b != 0 ? b - 1 : cast_int(L->top - ra)));
                if (ci->callstatus & CIST_FRESH)  /* local 'ci' still from callee */
                    return;  /* external invocation: return */
                else {  /* invocation via reentry: continue execution */
                    ci = L->ci;
                    if (b) L->top = ci->top;
                    lua_assert(isLua(ci));
                    lua_assert(GET_OPCODE(*((ci)->u.l.savedpc - 1)) == OP_CALL);
                    goto newframe;  /* restart luaV_execute over new Lua function */
                }
            }
            vmcase(OP_FORLOOP) {
                if (ttisinteger(ra)) {  /* integer loop? */
                    lua_Integer step = ivalue(ra + 2);
                    lua_Integer idx = intop(+, ivalue(ra), step); /* increment index */
                    lua_Integer limit = ivalue(ra + 1);
                    if ((0 < step) ? (idx <= limit) : (limit <= idx)) {
                        ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
                        chgivalue(ra, idx);  /* update internal index... */
                        setivalue(ra + 3, idx);  /* ...and external index */
                    }
                }
                else {  /* floating loop */
                    lua_Number step = fltvalue(ra + 2);
                    lua_Number idx = luai_numadd(L, fltvalue(ra), step); /* inc. index */
                    lua_Number limit = fltvalue(ra + 1);
                    if (luai_numlt(0, step) ? luai_numle(idx, limit)
                        : luai_numle(limit, idx)) {
                        ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
                        chgfltvalue(ra, idx);  /* update internal index... */
                        setfltvalue(ra + 3, idx);  /* ...and external index */
                    }
                }
                vmbreak;
            }
            vmcase(OP_FORPREP) {
                TValue* init = ra;
                TValue* plimit = ra + 1;
                TValue* pstep = ra + 2;
                lua_Integer ilimit;
                int stopnow;
                if (ttisinteger(init) && ttisinteger(pstep) &&
                    forlimit(plimit, &ilimit, ivalue(pstep), &stopnow)) {
                    /* all values are integer */
                    lua_Integer initv = (stopnow ? 0 : ivalue(init));
                    setivalue(plimit, ilimit);
                    setivalue(init, intop(-, initv, ivalue(pstep)));
                }
                else {  /* try making all values floats */
                    lua_Number ninit; lua_Number nlimit; lua_Number nstep;
                    if (!tonumber(plimit, &nlimit))
                        luaG_runerror(L, "'for' limit must be a number");
                    setfltvalue(plimit, nlimit);
                    if (!tonumber(pstep, &nstep))
                        luaG_runerror(L, "'for' step must be a number");
                    setfltvalue(pstep, nstep);
                    if (!tonumber(init, &ninit))
                        luaG_runerror(L, "'for' initial value must be a number");
                    setfltvalue(init, luai_numsub(L, ninit, nstep));
                }
                ci->u.l.savedpc += GETARG_sBx(i);
                vmbreak;
            }
            vmcase(OP_TFORCALL) {
                StkId cb = ra + 3;  /* call base */
                setobjs2s(L, cb + 2, ra + 2);
                setobjs2s(L, cb + 1, ra + 1);
                setobjs2s(L, cb, ra);
                L->top = cb + 3;  /* func. + 2 args (state and index) */
                Protect(luaD_call(L, cb, GETARG_C(i)));
                L->top = ci->top;
                i = *(ci->u.l.savedpc++);  /* go to next instruction */
                ra = RA(i);
                lua_assert(GET_OPCODE(i) == OP_TFORLOOP);
                goto l_tforloop;
            }
            vmcase(OP_TFORLOOP) {
            l_tforloop:
                if (!ttisnil(ra + 1)) {  /* continue loop? */
                    setobjs2s(L, ra, ra + 1);  /* save control variable */
                    ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
                }
                vmbreak;
            }
            vmcase(OP_SETLIST) {
                int n = GETARG_B(i);
                int c = GETARG_C(i);
                unsigned int last;
                Table* h;
                if (n == 0) n = cast_int(L->top - ra) - 1;
                if (c == 0) {
                    lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_EXTRAARG);
                    c = GETARG_Ax(*ci->u.l.savedpc++);
                }
                h = hvalue(ra);
                last = ((c - 1) * LFIELDS_PER_FLUSH) + n;
                if (last > h->sizearray)  /* needs more space? */
                    luaH_resizearray(L, h, last);  /* preallocate it at once */
                for (; n > 0; n--) {
                    TValue* val = ra + n;
                    luaH_setint(L, h, last--, val);
                    luaC_barrierback(L, h, val);
                }
                L->top = ci->top;  /* correct top (in case of previous open call) */
                vmbreak;
            }
            vmcase(OP_CLOSURE) {
                Proto* p = cl->p->p[GETARG_Bx(i)];
                LClosure* ncl = getcached(p, cl->upvals, base);  /* cached closure */
                if (ncl == NULL)  /* no match? */
                    pushclosure(L, p, cl->upvals, base, ra);  /* create a new one */
                else
                    setclLvalue(L, ra, ncl);  /* push cashed closure */
                checkGC(L, ra + 1);
                vmbreak;
            }
            vmcase(OP_VARARG) {
                int b = GETARG_B(i) - 1;  /* required results */
                int j;
                int n = cast_int(base - ci->func) - cl->p->numparams - 1;
                if (n < 0)  /* less arguments than parameters? */
                    n = 0;  /* no vararg arguments */
                if (b < 0) {  /* B == 0? */
                    b = n;  /* get all var. arguments */
                    Protect(luaD_checkstack(L, n));
                    ra = RA(i);  /* previous call may change the stack */
                    L->top = ra + n;
                }
                for (j = 0; j < b && j < n; j++)
                    setobjs2s(L, ra + j, base - n + j);
                for (; j < b; j++)  /* complete required results with nil */
                    setnilvalue(ra + j);
                vmbreak;
            }
            vmcase(OP_EXTRAARG) {
                lua_assert(0);
                vmbreak;
            }
        }
    }
}

/* }================================================================== */

