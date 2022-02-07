/*
** $Id: lstring.h,v 1.61.1.1 2017/04/19 17:20:42 roberto Exp $
** String table (keep all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#ifndef lstring_h
#define lstring_h

#include "lgc.h"
#include "lobject.h"
#include "lstate.h"

/*
**    lua字符串中的合法字符可以是任何的1字节数据, 包括C语言中表示字符串结束的'\0'字
** 符. lua字符串内部将以带长度的内存块的形式存储, 存储的是二进制数据，解释器遇到'\0'
** 字符并不会截断数据.
**    lua字符串一旦被创建, 就不可被改写. lua的值对象若为字符串类型, 则它将以引用方式
** 存在. 字符串对象属于需要垃圾收集器管理的对象, 也就是说一个字符串一旦没有被任何地方
** 引用就可以回收它.
*/

/*
** lua字符串对象 = TString结构+实际字符串数据
** TString结构 = GCObject*指针+字符串信息数据
*/

/*
** 长短字符串的划分
**    字符串将以两种内部形式保存在lua_State中: 短字符串和长字符串. Lua中每个基本内建
** 类型都对应了一个宏定义, 其中字符串类型对应于LUA_TSTRING宏定义. 对于长短字符串, Lua
** 在LUA_TSTRING宏上扩展了两个小类型LUA_TSHRSTR和LUA_TLNGSTR, 这两个类型在类型字节高
** 四位存放0和1加以区别. 这两个小类型为内部使用, 不为外部API所见, 因此对于最终用户来说,
** 只见到LUA_TSTRING一种类型.
**    长短字符串的界限是由定义在luaconf.h中的宏LUAI_MAXSHORTLEN来决定的, 其默认设置为
** 40(字节). 在Lua的设计中, 元方法名和保留字必须是短字符串, 所以短字符串长度不得短于最
** 长的元方法__newindex和保留字function的长度, 也就是说LUAI_MAXSHORTLEN最小不可以设
** 置低于10(字节).
*/

/*
** 短字符串的内部化
**    Lua中所有的短字符串均被存放在全局状态表(global_State)的strt域中, strt是stringtable的
** 简写, 它是一个哈希表. 相同的短字符串在同一个lua_State中将只存在唯一一份实例, 这被称为字符
** 串的内部化. 合并相同的字符串可以大量减少内存占用, 缩短比较字符串的时间. 由于相同的字符串只
** 需要保存一份在内存中, 当用这个字符串做键匹配时, 比较字符串只需要比较地址是否相同就够了, 而
** 不必逐字节比较.
*/

/*
** 字符串的比较操作
**    由于长短字符串实现的不同, 在比较两个字符串是否相同时, 需要区分长短字符串. 在进行字符串比
** 较操作时, 首先子类型不同(长短字符串)的字符串自然不是相同的字符串, 然后如果子类型相同, 那么根
** 据长短字符串使用不同策略进行比较.
**
** 短字符串
**     短字符串由于经过内部化操作, 所以不必进行字符串内容比较, 仅需比较对象地址是否相等即可. Lua
** 使用一个宏eqshrstr来高效地实现这个操作.
**
** 长字符串
**    首先对象地址相等的两个长字符串属于同一个实例, 因此它们是相等的; 然后对象地址不相等的情况下,
** 当字符串长度不同时, 自然是不同的字符串, 而长度相同时, 则需要进行逐字节比较.
*/

/* 计算字符串在内存中的实际大小(因为字符串有些头部信息) (参数: 字符串的字符个数) */
#define sizelstring(l)  (sizeof(union UTString) + ((l) + 1) * sizeof(char))

/* udate的大小 (参数: udate中数据部分的大小) */
#define sizeludata(l)	(sizeof(union UUdata) + (l))
/* udate的大小 (参数: udate结构) */
#define sizeudata(u)	sizeludata((u)->len)

/* 创建字符串 */
#define luaS_newliteral(L, s)	(luaS_newlstr(L, "" s, \
                                 (sizeof(s)/sizeof(char))-1))


/*
** 判断该短字符串是否是保留字符串(保留字)(保留字的extra>0)
*/
#define isreserved(s)	((s)->tt == LUA_TSHRSTR && (s)->extra > 0)


/*
** 比较两个短字符串是否相同
** 直接比较地址, 因为在lua中相同的短字符串只会存在一份
*/
#define eqshrstr(a,b)	check_exp((a)->tt == LUA_TSHRSTR, (a) == (b))


LUAI_FUNC unsigned int luaS_hash(const char* str, size_t l, unsigned int seed);
LUAI_FUNC unsigned int luaS_hashlongstr(TString* ts);
LUAI_FUNC int luaS_eqlngstr(TString* a, TString* b);
LUAI_FUNC void luaS_resize(lua_State* L, int newsize);
LUAI_FUNC void luaS_clearcache(global_State* g);
LUAI_FUNC void luaS_init(lua_State* L);
LUAI_FUNC void luaS_remove(lua_State* L, TString* ts);
LUAI_FUNC Udata* luaS_newudata(lua_State* L, size_t s);
LUAI_FUNC TString* luaS_newlstr(lua_State* L, const char* str, size_t l);
LUAI_FUNC TString* luaS_new(lua_State* L, const char* str);
LUAI_FUNC TString* luaS_createlngstrobj(lua_State* L, size_t l);

/*
** lua字符串说明
**     lua中字符串分为长字符串和短字符串(依据字符个数是否小于LUAI_MAXSHORTLEN)
**
**     对于短字符串(标识: 4), 保存在global_State.strt结构中, strt.hash是二维数组, 字
** 符串采用hash表的形式保存, 即计算短字符串的hash值, 保存在hash值下标对应的位置中. 但是
** hash值可能相同, 相同hash值的字符串, 则以链表的形式保存.
**     短字符串在内存中只有一份, 其他相同的字符串都是引用该字符串. 同时短字符串(需要回收
** 的)也在global_State.allgc链表上
**     TString.extra > 0 表明该短字符串是系统保留的关键字, 用于词法分析, extra的值直接
** 对应着词法分析时的一个token值, 同时不被GC回收.
**
**     对于长字符串(标识: 20), 保存在global_State.allgc链表上, 并且不唯一, 长字符串的
** hash值, 创建时并不计算其hash值, 用的时候(即长字符串当做索引时), 只计算一次, 保存在
** TString.hash中. TString.extra为1, 表示已经计算过hash
**
**     字符串有一个临时缓存表global_State.strcache, 用于保存刚刚创建的字符串, 若创建
** 字符串时, 该字符串存在于临时表中, 则直接获取临时表中的字符串即可. 所以比较长字符串时,
** 有存在地址的比较.
** 建议: 在字符串中尽量少用字符串连接操作符(..), 因为每一次都会生成一个新的字符串
*/

/*
**     在Lua中, 字符串实际上是被内化(internalization)的一种数据. 简单来说, 每个存放
** Lua字符串的变量, 实际上存放的并不是一份字符串的数据副本, 而是这份字符串数据的引用.
** 在这个理念下, 每当新创建一个字符串时, 首先都会去检查当前系统中是否已经有一份相同的
** 字符串数据了. 如果存在的话就直接复用, 将引用指向这个已经存在的字符串数据, 否则就重新
** 创建出一份新的字符串数据.(以上只针对短字符串)
*/

/*
**     字符串在Lua中是一个不可变的数据, 改变一个字符串变量的数据并不会影响原来字符串的数据,
** 只是将变量指向了新的字符串
**     变量存放的仅是字符串的引用, 而不是实际内容
*/

/*
**     使用散列桶来存放数据, 又有一个问题需要考虑: 当数据量非常大时, 分配到每个桶上的数据也
** 会非常多, 这样一次查找也退化成了一次线性的查找过程. Lua中也考虑了这种情况, 所以有一个重
** 新散列(rehash)的过程, 这就是当字符串数据非常多时, 会重新分配桶的数量, 降低每个桶上分配
** 到的数据数量, 这个过程在函数luaS_resize中.
*/
#endif
