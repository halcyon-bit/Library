/*
** $Id: lmem.c,v 1.91.1.1 2017/04/19 17:20:42 roberto Exp $
** 内存管理器的接口
** See Copyright Notice in lua.h
*/

#define lmem_c
#define LUA_CORE

#include "lprefix.h"

#include <stddef.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"

/*
** 关于realloc的功能:
** void *frealloc(void *ud, void *ptr, size_t osize, size_t nsize);
** ('osize'是旧的大小, 'nsize'是新的大小)
**
** *frealloc(ud, NULL, x, s)创建一个大小为's'的新内存块(不管'x').
**
** *frealloc(ud, p, x, 0) 释放内存块'p'
** (在这种情况下, frealloc必须返回NULL);
** 特别的是, frealloc(ud, NULL, 0, 0)什么都不做
** (相当于ISO C中的free(NULL))
**
** 如果无法创建或者重新分配内存, frealloc都会返回NULL
** (任何重新分配到相同的或者更小的大小都不会失败!)
*/

#define MINSIZEARRAY  4

/*
** 描述: 管理可变长数组, 当数组空间不够时, 扩大为原来空间的两倍. 内存操作使用宏
**       luaM_reallocv, 该宏针对数组操作, 根据新的数据元素个数重新分配内存.
** lua_State *L:        数组长度（最大容纳元素个数），传入表示原始数组长度，传出表示重新分配后数组长度；
** void *block:
** int *size: 数组长度(最大容纳元素个数), 传入表示原始数组长度, 传出表示重新分配
**            后数组长度.
** size_t size_elems: 单个数组元素的大小.
** int limit: 数组元素最大容纳个数限制.
** const char *what: 提示信息字符串.
** return(void*): 新的内存指针.
*/
void* luaM_growaux_(lua_State* L, void* block, int* size, size_t size_elems,
    int limit, const char* what)
{
    void* newblock;
    int newsize;
    if (*size >= limit / 2)
    {   /* 无法扩展为原先的两倍 */
        if (*size >= limit)  /* 当前数组大小已达上限 */
            luaG_runerror(L, "too many %s (limit is %d)", what, limit);
        newsize = limit;  /* 数组空间扩展为上限 */
    }
    else
    {
        newsize = (*size) * 2;
        if (newsize < MINSIZEARRAY)
            newsize = MINSIZEARRAY;  /* 最小限制 */
    }
    newblock = luaM_reallocv(L, block, *size, newsize, size_elems);
    *size = newsize;  /* 更新数组大小 */
    return newblock;
}

l_noret luaM_toobig(lua_State* L)
{
    luaG_runerror(L, "memory allocation error: block too big");
}

/*
** 描述: 内存操作, 通过调用global_State.frealloc进行内存管理工作, 根据传入的
**       osize和nsize调整内部感知的内存大小(设置GCdebt), 并在内存不够用的时候
**       主动尝试做GC操作.
** lua_State *L: lua状态机.
** void *block: 非NULL表明指向一个已分配的内存块指针, NULL表示将分配一块nsize
**              大小的新内存块.
** size_t osize: 原始内存块大小, 若block为NULL, 则osize为0.
** size_t nsize: 新的内存块大小, 当nsize为0时, 为内存释放.
** return(void*): 新的内存指针.
*/
void* luaM_realloc_(lua_State* L, void* block, size_t osize, size_t nsize)
{
    // 1. 调用 g->frealloc 进行分配内存，如果分配失败的话，执行一次 fullgc，再次尝试分配
    // 2. 将新增使用内存量，加入到 global_State 中的 GCdebt 字段中
    void* newblock;
    global_State* g = G(L);
    size_t realosize = (block) ? osize : 0;
    lua_assert((realosize == 0) == (block == NULL));
#if defined(HARDMEMTESTS)
    if (nsize > realosize && g->gcrunning)
        luaC_fullgc(L, 1);  /* 强行使用GC */
#endif
    newblock = (*g->frealloc)(g->ud, block, osize, nsize);
    if (newblock == NULL && nsize > 0)  /* nsize为0时, 为释放内存 */
    {
        /* 创建新内存或增大内存失败 */
        lua_assert(nsize > realosize);  /* 缩小内存块时不会失败 */
        if (g->version)     /* 版本号 */
        {   /* is state fully built? */
            luaC_fullgc(L, 1);  /* 尝试释放一些内存... */
            newblock = (*g->frealloc)(g->ud, block, osize, nsize);  /* 再试一次 */
        }
        if (newblock == NULL)
            luaD_throw(L, LUA_ERRMEM);  /* 内存申请失败 */
    }
    lua_assert((nsize == 0) == (newblock == NULL));
    g->GCdebt = (g->GCdebt + nsize) - realosize;    /* 更新当前使用的内存大小 */
    return newblock;
}

