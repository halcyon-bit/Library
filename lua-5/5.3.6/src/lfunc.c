/*
** $Id: lfunc.c,v 2.45.1.1 2017/04/19 17:39:34 roberto Exp $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#define lfunc_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"


/* 构造一个新的C闭包 */
CClosure* luaF_newCclosure(lua_State* L, int n)
{
    GCObject* o = luaC_newobj(L, LUA_TCCL, sizeCclosure(n));
    CClosure* c = gco2ccl(o);
    c->nupvalues = cast_byte(n);
    return c;
}

/* 构造一个新的Lua闭包 */
LClosure* luaF_newLclosure(lua_State* L, int n)
{
    GCObject* o = luaC_newobj(L, LUA_TLCL, sizeLclosure(n));
    LClosure* c = gco2lcl(o);
    c->p = NULL;
    c->nupvalues = cast_byte(n);  /* upvalues的数量 */
    while (n--)
    {
        c->upvals[n] = NULL;
    }
    return c;
}

/*
** 使用关闭的upvalue填充lua闭包
*/
void luaF_initupvals(lua_State* L, LClosure* cl)
{
    int i;
    for (i = 0; i < cl->nupvalues; i++)
    {
        UpVal* uv = luaM_new(L, UpVal);
        uv->refcount = 1;
        uv->v = &uv->u.value;  /* 该upvalue是关闭的 */
        setnilvalue(uv->v);
        cl->upvals[i] = uv;
    }
}

/*
** 在数据栈上查找开放的upvalue
** 在创建新的lua闭包时调用
*/
UpVal* luaF_findupval(lua_State* L, StkId level)
{
    UpVal** pp = &L->openupval;
    UpVal* p;
    UpVal* uv;
    lua_assert(isintwups(L) || L->openupval == NULL);
    while (*pp != NULL && (p = *pp)->v >= level)
    {
        lua_assert(upisopen(p));
        if (p->v == level)  /* 找到了相应的upvalue? */
            return p;  /* 返回 */
        pp = &p->u.open.next;
    }
    /* 没有找到: 创建一个新的upvalue */
    uv = luaM_new(L, UpVal);
    uv->refcount = 0;
    uv->u.open.next = *pp;  /* 添加到开放的upvalue列表上 */
    uv->u.open.touched = 1;
    *pp = uv;
    uv->v = level;  /* 当前的upvalue在数据栈上 */
    if (!isintwups(L))
    {   /* 线程不在具有upvalues的线程列表中? */
        L->twups = G(L)->twups;  /* 添加到列表中 */
        G(L)->twups = L;
    }
    return uv;
}


void luaF_close(lua_State* L, StkId level) {
    UpVal* uv;
    while (L->openupval != NULL && (uv = L->openupval)->v >= level) {
        lua_assert(upisopen(uv));
        L->openupval = uv->u.open.next;  /* remove from 'open' list */
        if (uv->refcount == 0)  /* no references? */
            luaM_free(L, uv);  /* free upvalue */
        else {
            setobj(L, &uv->u.value, uv->v);  /* move value to upvalue slot */
            uv->v = &uv->u.value;  /* now current value lives here */
            luaC_upvalbarrier(L, uv);
        }
    }
}


Proto* luaF_newproto(lua_State* L) {
    GCObject* o = luaC_newobj(L, LUA_TPROTO, sizeof(Proto));
    Proto* f = gco2p(o);
    f->k = NULL;
    f->sizek = 0;
    f->p = NULL;
    f->sizep = 0;
    f->code = NULL;
    f->cache = NULL;
    f->sizecode = 0;
    f->lineinfo = NULL;
    f->sizelineinfo = 0;
    f->upvalues = NULL;
    f->sizeupvalues = 0;
    f->numparams = 0;
    f->is_vararg = 0;
    f->maxstacksize = 0;
    f->locvars = NULL;
    f->sizelocvars = 0;
    f->linedefined = 0;
    f->lastlinedefined = 0;
    f->source = NULL;
    return f;
}


void luaF_freeproto(lua_State* L, Proto* f) {
    luaM_freearray(L, f->code, f->sizecode);
    luaM_freearray(L, f->p, f->sizep);
    luaM_freearray(L, f->k, f->sizek);
    luaM_freearray(L, f->lineinfo, f->sizelineinfo);
    luaM_freearray(L, f->locvars, f->sizelocvars);
    luaM_freearray(L, f->upvalues, f->sizeupvalues);
    luaM_free(L, f);
}


/*
** Look for n-th local variable at line 'line' in function 'func'.
** Returns NULL if not found.
*/
const char* luaF_getlocalname(const Proto* f, int local_number, int pc) {
    int i;
    for (i = 0; i < f->sizelocvars && f->locvars[i].startpc <= pc; i++) {
        if (pc < f->locvars[i].endpc) {  /* is variable active? */
            local_number--;
            if (local_number == 0)
                return getstr(f->locvars[i].varname);
        }
    }
    return NULL;  /* not found */
}

