/*
** $Id: lmem.h,v 1.43.1.1 2017/04/19 17:20:42 roberto Exp $
** 内存管理器的接口
** See Copyright Notice in lua.h
*/

#ifndef lmem_h
#define lmem_h


#include <stddef.h>

#include "llimits.h"
#include "lua.h"


/*
** 此宏将数组'b'从'on'重新分配给'n'个元素, 其中每个元素的大小为'e'. 如果乘积
** 'n'*'e'发生算术溢出, 则会引发错误(称为"luaM_toobig"). 由于"e"始终是常数,
** 因此避免了运行时除法MAX_SIZET/(e).
**
** (为了避免警告, 宏有些复杂: 'sizeof'比较避免了在不会发生溢出时的运行时比较.
** 编译器应该能够自行优化真实的测试, 但是当它执行时, 它可能会发出"由于数据类型
** 范围有限, 比较始终为假"的警告; +1会欺骗编译器, 既避免了这种警告, 又避免了这
** 种优化.)
*/
/*
** 描述: 使数组b的大小从on重新分配为n, 其中每个数组元素大小为e.
** L: lua状态机
** b: 数组指针
** on: 数组重新分配前的长度
** n: 数组重新分配后的长度
** e: 数组元素大小
*/
#define luaM_reallocv(L,b,on,n,e) \
  (((sizeof(n) >= sizeof(size_t) && cast(size_t, (n)) + 1 > MAX_SIZET/(e)) \
      ? luaM_toobig(L) : cast_void(0)) , \
   luaM_realloc_(L, (b), (on)*(e), (n)*(e)))

/*
** 描述: 使字符数组b的大小从on重新分配为n, 其中每个数组元素大小为sizeof(char).
** L: lua状态机
** b: 数组指针
** on: 数组重新分配前的长度
** n: 数组重新分配后的长度
*/
#define luaM_reallocvchar(L,b,on,n)  \
        cast(char *, luaM_realloc_(L, (b), (on)*sizeof(char), (n)*sizeof(char)))

/*
** 描述: 释放b指向的内存块空间.
** L: lua状态机
** b: 内存块指针
** s: 内存块大小
*/
#define luaM_freemem(L, b, s)    luaM_realloc_(L, (b), (s), 0)

/*
** 描述: 释放b指向的内存块空间(b表示某种对象类型指针).
** L: lua状态机
** b: 内存块指针, 同时表示某种对象类型指针
*/
#define luaM_free(L, b)   luaM_realloc_(L, (b), sizeof(*(b)), 0)

/*
** 描述: 释放b指向的内存块空间(b表示某种类型对象的数组指针).
** L: lua状态机
** b: 内存指针, 同时表示某种类型对象的数组指针
** n: 数组长度
*/
#define luaM_freearray(L, b, n)   luaM_realloc_(L, (b), (n)*sizeof(*(b)), 0)

/*
** 描述: 分配一块大小为s的内存块空间.
** L: lua状态机
** s: 将要分配的内存块空间大小
*/
#define luaM_malloc(L,s)  luaM_realloc_(L, NULL, 0, (s))

/*
** 描述: 分配一块内存块空间, 大小为sizeof(t).
** L: lua状态机
** t: 某种数据类型
*/
#define luaM_new(L,t)   cast(t *, luaM_malloc(L, sizeof(t)))

/*
** 描述: 分配一个长度为n的数组空间, 数组元素为类型t.
** L: lua状态机
** n: 数组长度
** t: 数组元素类型
*/
#define luaM_newvector(L,n,t) \
        cast(t *, luaM_reallocv(L, NULL, 0, n, sizeof(t)))

/*
** 描述: 分配一块大小为s的内存块空间, 其将要容纳的Lua数据类型为tag表示的类型.
** L: lua状态机
** tag: Lua数据类型
** s: 分配的内存块大小
*/
#define luaM_newobject(L,tag,s) luaM_realloc_(L, NULL, tag, (s))

/*
** 描述: 在数组空间不足以容纳下一个元素的情况下增长空间大小(原空间大小 * 2).
** L: lua状态机
** v: 数组指针
** nelems: 正在使用的元素个数
** size: 数组元素个数, 传入表示原始数组大小, 传出表示重新分配后数组大小
** t: (数组元素的)数据类型
** limit: 数组元素最大个数限制
** e: 提示信息字符串
*/
#define luaM_growvector(L,v,nelems,size,t,limit,e) \
        if ((nelems)+1 > (size)) \
            ((v)=cast(t *, luaM_growaux_(L,v,&(size),sizeof(t),limit,e)))

/*
** 描述: 重新分配数组空间大小
** L: lua状态机
** v: 数组指针
** oldn: 重新分配前数组大小
** n: 重新分配后数组大小
** t: (数组元素的)数据类型
*/
#define luaM_reallocvector(L, v,oldn,n,t) \
        ((v)=cast(t *, luaM_reallocv(L, v, oldn, n, sizeof(t))))

LUAI_FUNC l_noret luaM_toobig(lua_State* L);

/* 不要直接调用以下函数, 使用上面的宏 */
/* 内存操作 */
LUAI_FUNC void* luaM_realloc_(lua_State* L, void* block, size_t oldsize, size_t size);
/* 管理可变长数组 */
LUAI_FUNC void* luaM_growaux_(lua_State* L, void* block, int* size, size_t size_elem,
    int limit, const char* what);


#endif

