/*
** $Id: ldo.h,v 2.29.1.1 2017/04/19 17:20:42 roberto Exp $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/

#ifndef ldo_h
#define ldo_h


#include "lobject.h"
#include "lstate.h"
#include "lzio.h"


/*
** 该宏来检查堆栈大小并在需要时增加堆栈。
** 参数'pre'/'pos'允许宏在重新分配时保留指向堆栈的指针, 仅在需要时才执行工作.
** 'condmovestack'用于重度测试，以在每次检查时强制执行堆栈重新分配。
*/
#define luaD_checkstackaux(L,n,pre,pos)  \
	if (L->stack_last - L->top <= (n)) \
	  { pre; luaD_growstack(L, n); pos; } else { condmovestack(L,pre,pos); }

/* 一般来说, 'pre'/'pos'是空的(无需保存) */
#define luaD_checkstack(L,n)	luaD_checkstackaux(L,n,(void)0,(void)0)

/* 获取p到L->stack(栈低)的距离 */
#define savestack(L,p)		((char *)(p) - (char *)L->stack)
/* 获取数据栈中位置为 L->stack+n 的数据 */
#define restorestack(L,n)	((TValue *)((char *)L->stack + (n)))


/* 受保护的函数类型, 由'runprotected'运行 */
typedef void (*Pfunc) (lua_State* L, void* ud);

LUAI_FUNC int luaD_protectedparser(lua_State* L, ZIO* z, const char* name,
    const char* mode);
LUAI_FUNC void luaD_hook(lua_State* L, int event, int line);
LUAI_FUNC int luaD_precall(lua_State* L, StkId func, int nresults);
LUAI_FUNC void luaD_call(lua_State* L, StkId func, int nResults);
LUAI_FUNC void luaD_callnoyield(lua_State* L, StkId func, int nResults);
LUAI_FUNC int luaD_pcall(lua_State* L, Pfunc func, void* u,
    ptrdiff_t oldtop, ptrdiff_t ef);
LUAI_FUNC int luaD_poscall(lua_State* L, CallInfo* ci, StkId firstResult,
    int nres);
LUAI_FUNC void luaD_reallocstack(lua_State* L, int newsize);
LUAI_FUNC void luaD_growstack(lua_State* L, int n);
LUAI_FUNC void luaD_shrinkstack(lua_State* L);
LUAI_FUNC void luaD_inctop(lua_State* L);

LUAI_FUNC l_noret luaD_throw(lua_State* L, int errcode);
LUAI_FUNC int luaD_rawrunprotected(lua_State* L, Pfunc f, void* ud);

#endif

