/*
** $Id: lgc.h,v 2.91.1.1 2017/04/19 17:39:34 roberto Exp $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#ifndef lgc_h
#define lgc_h


#include "lobject.h"
#include "lstate.h"

/*
** lua采用标记清除(Mark and Sweep) GC算法, 算法简述:
** 标记: 每次执行GC时, 先以若干根节点开始, 逐个把直接或间接和它们相关的
** 节点都做上标记;
** 清除: 当标记完成后, 遍历整个对象链表, 把被标记为需要删除的节点一一删
** 除即可.
**
** 标记即是颜色, Lua用白、灰、黑三色来标记一个对象的可回收状态(白色又分为
** 白1、白2)
** 白色: 可回收状态.
** 详解: 如果该对象未被GC标记过则此时白色代表当前对象为待访问状态. 例如:
** 新创建的对象的初始状态为白色, 因为该对象还没有被GC标记到, 所以保持初始
** 颜色不变(白色). 如果该对象在GC标记阶段结束后, 仍然为白色, 则代表当前对
** 象为可回收状态. 但其实本质上白色的设定就是为了表示可回收.
**
** 灰色: 中间状态
** 详解: 当前对象为待标记状态. 例如: 当前对象已经被GC访问过, 但该对象引用
** 的其他对象还没有被标记.
**
** 黑色: 不可回收状态
** 详解: 当前对象为已标记对象. 例如: 当前对象已经被GC访问过, 并且对象引用
** 的其他对象也被标记了.
**
** 备注: 白色又分为白1和白2. 因为在GC标记阶段结束而清除阶段尚未开始时, 如
** 果新建一个对象, 由于其未被发现引用关系, 原则上应该是白色, 于是之后的清
** 除阶段就按照白色被清除的规则将新建的对象清除. 这样不合理. 于是Lua采用两
** 中白色进行标识, 如果发生上述情况, Lua依然会将新建对象标识为白色, 不过是
** "当前白"(如白1). 而Lua在清除阶段只会清扫"旧白"(如白2), 在清扫结束之后,
** 则会更新"当前白", 即将白2作为当前白. 下一轮GC将会清扫作为"旧白"的白1标识
** 对象.
*/

/*
** lua是用新增使用内存量达到一定数字时触发GC执行
** lua使用GCdebt变量来预设下次触发完整GC时机和触发下次单步GC时机(时机: 新使用内存增量
** 事件间隔.)
*/

/*
**     垃圾收集器在标记对象时的主要不变性是, 黑色对象永远不能指向白色对象. 此外,
** 任何灰色物体都必须在"灰色列表"中(gray, grayagain, weak, allweak, ephemeron),
** 以便在完成收集周期之前可以再次对其进行访问. 当不强制执行不变式时(例如, 扫描阶段),
** 这些列表没有意义.
*/

/* 所有需要 GC 管理生命周期的对象都在 allgc 的链表上 */

/* GC 实现为"增量标记-清除回收器"，每次调用luaC_step 都会执行一次增加回收 */

/* 在下一个GC步骤之前要分配多少 */
#if !defined(GCSTEPSIZE)
/* ~100 small strings */
#define GCSTEPSIZE	(cast_int(100 * sizeof(TString)))
#endif


/*
** 垃圾收集器的可能状态
*/
#define GCSpropagate	0  	/* 2(标记) */
#define GCSatomic	1  		/* 3(原子标记)在 */
#define GCSswpallgc	2  		/* 4(回收allgc) */
#define GCSswpfinobj	3  	/* 5(回收finobj) */
#define GCSswptobefnz	4  	/* 6(tobefnz) */
#define GCSswpend	5  		/* 7 */
#define GCScallfin	6  		/* 8 */
#define GCSpause	7		/* 1(开始) */

/* 判断GC是否处于清除阶段 */
#define issweepphase(g)  \
	(GCSswpallgc <= (g)->gcstate && (g)->gcstate <= GCSswpend)


/*
**     用于指示何时必须保留主要不变式(黑色对象不能指向白色对象). 在gc期间, 清除
** 阶段可能会破坏不变性, 因为变成黑色的对象可能指向仍然是白色的对象. 当扫描结束
** 并且所有对象再次变为白色时, 将恢复不变性.
*/
/* 判断GC是否在标记阶段 */
#define keepinvariant(g)	((g)->gcstate <= GCSatomic)


/*
** some useful bit tricks
*/
#define resetbits(x,m)		((x) &= cast(lu_byte, ~(m)))
#define setbits(x,m)		((x) |= (m))
#define testbits(x,m)		((x) & (m))
#define bitmask(b)		(1<<(b))
#define bit2mask(b1,b2)		(bitmask(b1) | bitmask(b2))
#define l_setbit(x,b)		setbits(x, bitmask(b))
#define resetbit(x,b)		resetbits(x, bitmask(b))
#define testbit(x,b)		testbits(x, bitmask(b))


/* GC对象的颜色标记属性: */
/* 白色(2种, 二进制 01或10) 灰色(00) 黑色(100) */
#define WHITE0BIT	0  /* 白色 (type 0) */
#define WHITE1BIT	1  /* 白色 (type 1) */
#define BLACKBIT	2  /* 黑色 */
#define FINALIZEDBIT	3  /* 用于标记没有被引用, 需要回收的udata */
/* bit 7 is currently used by tests (luaL_checkmemory) */

#define WHITEBITS	bit2mask(WHITE0BIT, WHITE1BIT)  /* 二进制(11) */

/* 判断是否为白色 */
#define iswhite(x)      testbits((x)->marked, WHITEBITS)
/* 判断是否为黑色 */
#define isblack(x)      testbit((x)->marked, BLACKBIT)
/* 判断是否为灰色, 即不是白色也不是黑色 */
#define isgray(x)  \
	(!testbits((x)->marked, WHITEBITS | bitmask(BLACKBIT)))

/* 判断是否为FINALIZEDBIT */
#define tofinalize(x)	testbit((x)->marked, FINALIZEDBIT)

/* 获取另一种白色 */
#define otherwhite(g)	((g)->currentwhite ^ WHITEBITS)
/* 判断当前标记(m)中另一种白色(ow)的标记是否为1, ow为另一种白色, m为标记 */
#define isdeadm(ow,m)	(!(((m) ^ WHITEBITS) & (ow)))
/* 判断对象v是否已经无效(死亡) */ /* v的颜色是旧白, 判断当前白(新白)对应的另一种白(即旧白)是否与v相同 */
#define isdead(g,v)	isdeadm(otherwhite(g), (v)->marked)

/* 改变x的白色为另一种白 */
#define changewhite(x)	((x)->marked ^= WHITEBITS)
/* 灰色变成黑色 */
#define gray2black(x)	l_setbit((x)->marked, BLACKBIT)

/* 获取当前GC阶段的白色 */
#define luaC_white(g)	cast(lu_byte, (g)->currentwhite & WHITEBITS)


/*
** 当GCdebt大于零时, 即会触发自动GC.
*/
#define luaC_condGC(L,pre,pos) \
	{ if (G(L)->GCdebt > 0) { pre; luaC_step(L); pos;}; \
	  condchangemem(L,pre,pos); }

/*
**     检查是否需要GC, lua在每次分配新的内存时, 会主动检查是否满足GC条
** 件, 大部分会引起内存增长的API中, 都调用了luaC_checkGC, 从而实现GC可
** 以随内存使用增加而自动进行, 而触发条件是通过g->GCdebt、g->totalbytes
** 等参数计算得来的. 当lua使用的内存达到阀值, 便会触发GC.
*/
#define luaC_checkGC(L)		luaC_condGC(L,(void)0,(void)0)


#define luaC_barrier(L,p,v) (  \
	(iscollectable(v) && isblack(p) && iswhite(gcvalue(v))) ?  \
	luaC_barrier_(L,obj2gco(p),gcvalue(v)) : cast_void(0))

#define luaC_barrierback(L,p,v) (  \
	(iscollectable(v) && isblack(p) && iswhite(gcvalue(v))) ? \
	luaC_barrierback_(L,p) : cast_void(0))

#define luaC_objbarrier(L,p,o) (  \
	(isblack(p) && iswhite(o)) ? \
	luaC_barrier_(L,obj2gco(p),obj2gco(o)) : cast_void(0))

#define luaC_upvalbarrier(L,uv) ( \
	(iscollectable((uv)->v) && !upisopen(uv)) ? \
         luaC_upvalbarrier_(L,uv) : cast_void(0))

LUAI_FUNC void luaC_fix(lua_State* L, GCObject* o);
LUAI_FUNC void luaC_freeallobjects(lua_State* L);
LUAI_FUNC void luaC_step(lua_State* L);
LUAI_FUNC void luaC_runtilstate(lua_State* L, int statesmask);
LUAI_FUNC void luaC_fullgc(lua_State* L, int isemergency);
LUAI_FUNC GCObject* luaC_newobj(lua_State* L, int tt, size_t sz);
LUAI_FUNC void luaC_barrier_(lua_State* L, GCObject* o, GCObject* v);
LUAI_FUNC void luaC_barrierback_(lua_State* L, Table* o);
LUAI_FUNC void luaC_upvalbarrier_(lua_State* L, UpVal* uv);
LUAI_FUNC void luaC_checkfinalizer(lua_State* L, GCObject* o, Table* mt);
LUAI_FUNC void luaC_upvdeccount(lua_State* L, UpVal* uv);


#endif
