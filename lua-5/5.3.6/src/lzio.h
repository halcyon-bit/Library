/*
** $Id: lzio.h,v 1.31.1.1 2017/04/19 17:20:42 roberto Exp $
** Buffered streams
** See Copyright Notice in lua.h
*/


#ifndef lzio_h
#define lzio_h

#include "lua.h"

#include "lmem.h"

typedef struct Zio ZIO;

#define EOZ (-1)      /* end of stream */

/* 获取ZIO缓冲区中的一个数据 */
#define zgetc(z)  (((z)->n--)>0 ?  cast_uchar(*(z)->p++) : luaZ_fill(z))


typedef struct Mbuffer
{
    char* buffer;
    size_t n;
    size_t buffsize;
} Mbuffer;

/* 初始化Mbuffer结构 */
#define luaZ_initbuffer(L, buff) ((buff)->buffer = NULL, (buff)->buffsize = 0)

/* 获取Mbuffer.buffer */
#define luaZ_buffer(buff) ((buff)->buffer)
/* 获取buffer的大小 */
#define luaZ_sizebuffer(buff) ((buff)->buffsize)
/* 获取buffer的长度 */
#define luaZ_bufflen(buff)  ((buff)->n)
/* buffer中移除i个字节数据(最后i个) */
#define luaZ_buffremove(buff,i) ((buff)->n -= (i))
/* 重置buffer的长度 */
#define luaZ_resetbuffer(buff) ((buff)->n = 0)

/* 申请buffer空间, 大小size */
#define luaZ_resizebuffer(L, buff, size) \
  ((buff)->buffer = luaM_reallocvchar(L, (buff)->buffer, \
        (buff)->buffsize, size), \
  (buff)->buffsize = size)
/* 释放buffer空间 */
#define luaZ_freebuffer(L, buff)  luaZ_resizebuffer(L, buff, 0)

/* 初始化函数 */
LUAI_FUNC void luaZ_init(lua_State* L, ZIO* z, lua_Reader reader, void* data);
/* 读取n字节数据, 读取的数据在b所指的内存中 */
LUAI_FUNC size_t luaZ_read(ZIO* z, void* b, size_t n);  /* read next n bytes */


/* --------- Private Part ------------------ */
struct Zio
{
    size_t n;         /* 尚未阅读的字节数 */
    const char* p;        /* 缓冲区的当前位置 */
    lua_Reader reader;      /* 读取函数 */
    void* data;         /* 额外的数据 */
    lua_State* L;     /* Lua state (for reader) */
};

/* 调用实际的读取数据函数, z->reader */
LUAI_FUNC int luaZ_fill(ZIO* z);

#endif
