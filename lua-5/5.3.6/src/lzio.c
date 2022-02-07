/*
** $Id: lzio.c,v 1.37.1.1 2017/04/19 17:20:42 roberto Exp $
** Buffered streams
** See Copyright Notice in lua.h
*/

#define lzio_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "llimits.h"
#include "lmem.h"
#include "lstate.h"
#include "lzio.h"


/* 从z->reader中读取数据, 保存在z->p中 */
int luaZ_fill(ZIO* z)
{
    size_t size;
    lua_State* L = z->L;
    const char* buff;
    lua_unlock(L);
    buff = z->reader(L, z->data, &size);
    lua_lock(L);
    if (buff == NULL || size == 0)
        return EOZ;
    z->n = size - 1;  /* discount char being returned */
    z->p = buff;
    return cast_uchar(*(z->p++));
}


/* 初始化 */
void luaZ_init(lua_State* L, ZIO* z, lua_Reader reader, void* data)
{
    z->L = L;
    z->reader = reader;
    z->data = data;
    z->n = 0;
    z->p = NULL;
}


/* 读取缓冲区中的数据 */
size_t luaZ_read(ZIO* z, void* b, size_t n)
{
    while (n)
    {
        size_t m;
        if (z->n == 0)
        {   /* 缓冲区中没有未读字符? */
            if (luaZ_fill(z) == EOZ)    /* 尝试读取更多数据 */
                return n;               /* 没有更多的输入; 返回丢失的字节数 */
            else
            {
                z->n++;  /* luaZ_fill消耗了第一个字节; 把它放回去 */
                z->p--;
            }
        }
        m = (n <= z->n) ? n : z->n;  /* 获取最小可读数. n 与 z->n 两者之间 */
        memcpy(b, z->p, m);
        z->n -= m;
        z->p += m;
        b = (char*)b + m;
        n -= m;
    }
    return 0;
}

