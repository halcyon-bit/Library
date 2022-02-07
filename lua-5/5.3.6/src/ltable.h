/*
** $Id: ltable.h,v 2.23.1.2 2018/05/24 19:39:05 roberto Exp $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#ifndef ltable_h
#define ltable_h

#include "lobject.h"

/*
** table 举例: a = {3, 2, 4, x=10}, 则3, 2, 4存储在数组部分, x=10存储在hash表部分
*/

/*
**     lua表分为数组部分和哈希部分, 其中数组部分不像其他语言那样, 从0开始作为第一个索引,
** 而是从1开始. 哈希表部分可以存储任何其他不能存放在数组部分的数据, 唯一的要求就是键值不
** 能为nil. 尽管内部实现上区分了这两个部分, 但是对使用者而言却是透明的. 使用Lua表, 可以
** 模拟出其他各种数据结构------数组、链表、树等.
*/

/* 获取哈希表某位置处的的结点 */
#define gnode(t,i)	(&(t)->node[i])
/* 获取结点(Node)的值(value) */
#define gval(n)		(&(n)->i_val)
/* 获取结点(Node)的下一个结点的相对位置(next) */
#define gnext(n)	((n)->i_key.nk.next)

/*
** 以只读的方式获取结点(Node)的键(key), 'const'以避免错误的写, 以免弄乱字段'next'
*/
#define gkey(n)		cast(const TValue*, (&(n)->i_key.tvk))

/*
** 'gkey'的可写版本; 允许更新单个字段, 但不更新整个字段(类型不兼容)
*/
#define wgkey(n)		(&(n)->i_key.nk)

/* 使元方法无效 */
#define invalidateTMcache(t)	((t)->flags = 0)

/* 判断 table 的哈希表是否为 dummy(空表) */
#define isdummy(t)		((t)->lastfree == NULL)

/* 哈希表的大小, 不是哈希表中元素数量 */
#define allocsizenode(t)	(isdummy(t) ? 0 : sizenode(t))

/*
** 根据 value 获取该 Node 结点的 key, value和key在一块连续的内存中.
** 给定表条目的值, 返回键.
*/
#define keyfromval(v) \
  (gkey(cast(Node *, cast(char *, (v)) - offsetof(Node, i_val))))


LUAI_FUNC const TValue* luaH_getint(Table* t, lua_Integer key);
LUAI_FUNC void luaH_setint(lua_State* L, Table* t, lua_Integer key,
    TValue* value);
LUAI_FUNC const TValue* luaH_getshortstr(Table* t, TString* key);
LUAI_FUNC const TValue* luaH_getstr(Table* t, TString* key);
LUAI_FUNC const TValue* luaH_get(Table* t, const TValue* key);
LUAI_FUNC TValue* luaH_newkey(lua_State* L, Table* t, const TValue* key);
LUAI_FUNC TValue* luaH_set(lua_State* L, Table* t, const TValue* key);
LUAI_FUNC Table* luaH_new(lua_State* L);
LUAI_FUNC void luaH_resize(lua_State* L, Table* t, unsigned int nasize,
    unsigned int nhsize);
LUAI_FUNC void luaH_resizearray(lua_State* L, Table* t, unsigned int nasize);
LUAI_FUNC void luaH_free(lua_State* L, Table* t);
LUAI_FUNC int luaH_next(lua_State* L, Table* t, StkId key);
LUAI_FUNC lua_Unsigned luaH_getn(Table* t);


#if defined(LUA_DEBUG)
LUAI_FUNC Node* luaH_mainposition(const Table* t, const TValue* key);
LUAI_FUNC int luaH_isdummy(const Table* t);
#endif

/*
**     在table的实现中, 哈希表占绝大部分比重. 哈希表在解决冲突有两个常用的方法:
**
**     开放定址法: 当冲突发生时, 使用某种探查(亦称探测)技术在散列表中形成一个探查(测)序
** 列. 沿此序列逐个单元地查找, 直到找到给定的关键字, 或者碰到一个开放的地址(即该地址单元
** 为空)为止(若要插入, 在探查到开放的地址, 则可将待插入的新结点存入该地址单元). 查找时探
** 查到开放的地址则表明表中无待查的关键字, 即查找失败.
**
**     链地址法: 又叫拉链法, 所有关键字为同义词的结点链接在同一个单链表中. 若选定的散列
** 表长度为m, 则可将散列表定义为一个由m个头指针组成的指针数组T[0…m-1]. 凡是散列地址为i的
** 结点, 均插入到以T[i]为头指针的单链表中. T中各分量的初值均应为空指针. 在拉链法中, 装
** 填因子α可以大于1, 但一般均取α≤1.
**
**     开放定址法相比链地址法节省更多的内存, 但在插入和查找的时候拥有更高的复杂度. 但是
** lua中table的哈希表的实现结合了以上两种方法的一些特性: 查找和插入等同链地址法复杂度;
** 内存开销近似等同于开放定址法.
**     原因是table中的哈希表虽然采用链地址法的形式处理冲突, 但是链表中的额外的节点是哈希
** 表中的节点, 并不需要额外开辟链表节点.
*/

/*
**     lua的哈希表中, 如果有冲突的话, lua会找哈希表中一个空的位置(从后往前找, 假设为x),
** 然后把新的key放在这个空的位置x上, 并且让哈希表中hash(key)处的节点的nk.next指向x. 这
** 个意思就是, 假如有冲突的话, 不用重新分配空间来存储冲突的key, 而是利用哈希表上未用过的
** 空格来存储. 但是这样会引入另外一个问题, 本来key是不应该放在x的, 假设有另外一个key2,
** hash(key2)算出来的位置也在x的话, 那就表示本来x这个位置应该是给key2的, 但是由于x被key
** 占用了, 导致key2没地方放了. 这时候lua的处理方式是把key放到另外一个空格, 然后让key2占
** 回x. 当哈希表已经没有空格的时候, lua就会resize哈希表. 这样做的好处主要是不用动态申请
** 内存空间, 哈希表初始化的时候有多少内存空间就用多少, 不够就resize哈希表.
*/

#endif
