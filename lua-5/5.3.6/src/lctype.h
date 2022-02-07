﻿/*
** $Id: lctype.h,v 1.12.1.1 2013/04/12 18:48:47 roberto Exp $
** 'ctype' functions for Lua
** See Copyright Notice in lua.h
*/

#ifndef lctype_h
#define lctype_h

#include "lua.h"


/*
** WARNING: the functions defined here do not necessarily correspond
** to the similar functions in the standard C ctype.h. They are
** optimized for the specific needs of Lua
*/

#if !defined(LUA_USE_CTYPE)

#if 'A' == 65 && '0' == 48
/* ASCII case: can use its own tables; faster and fixed */
#define LUA_USE_CTYPE	0
#else
/* must use standard C ctype */
#define LUA_USE_CTYPE	1
#endif

#endif


#if !LUA_USE_CTYPE	/* { */

#include <limits.h>

#include "llimits.h"


#define ALPHABIT	0
#define DIGITBIT	1
#define PRINTBIT	2
#define SPACEBIT	3
#define XDIGITBIT	4


#define MASK(B)		(1 << (B))


/*
** add 1 to char to allow index -1 (EOZ)
*/
#define testprop(c,p)	(luai_ctype_[(c)+1] & (p))

/*
** 'lalpha'（Lua字母）和'lalnum'（Lua字母数字）都包括'_'
*/
/* 判断是否是字母 */
#define lislalpha(c)	testprop(c, MASK(ALPHABIT))
/* 判断是否是字母或者数字 */
#define lislalnum(c)	testprop(c, (MASK(ALPHABIT) | MASK(DIGITBIT)))
/* 判断是否是数字 */
#define lisdigit(c)	    testprop(c, MASK(DIGITBIT))
/* 判断是否是空格 */
#define lisspace(c)	    testprop(c, MASK(SPACEBIT))
/* 判断是否是可打印字符 */
#define lisprint(c)	    testprop(c, MASK(PRINTBIT))
/* 判断是否是数字(0-9)或者字母(a-f A-F) 16进制 */
#define lisxdigit(c)	testprop(c, MASK(XDIGITBIT))

/*
** this 'ltolower' only works for alphabetic characters
*/
#define ltolower(c)	((c) | ('A' ^ 'a'))


/* two more entries for 0 and -1 (EOZ) */
LUAI_DDEC const lu_byte luai_ctype_[UCHAR_MAX + 2];


#else			/* }{ */

/*
** use standard C ctypes
*/

#include <ctype.h>


#define lislalpha(c)	(isalpha(c) || (c) == '_')
#define lislalnum(c)	(isalnum(c) || (c) == '_')
#define lisdigit(c)	    (isdigit(c))
#define lisspace(c)	    (isspace(c))
#define lisprint(c)	    (isprint(c))
/* 检查参数c是否为16进制数字 */
#define lisxdigit(c)	(isxdigit(c))

#define ltolower(c)	(tolower(c))

#endif			/* } */

#endif

