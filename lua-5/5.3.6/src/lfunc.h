/*
** $Id: lfunc.h,v 2.15.1.1 2017/04/19 17:39:34 roberto Exp $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#ifndef lfunc_h
#define lfunc_h


#include "lobject.h"

/* 计算C闭包的大小(包括upvalue) */
#define sizeCclosure(n) (cast(int, sizeof(CClosure)) + \
                         cast(int, sizeof(TValue)*((n)-1)))

/* 计算Lua闭包的大小(包括upvalue) */
#define sizeLclosure(n) (cast(int, sizeof(LClosure)) + \
                         cast(int, sizeof(TValue *)*((n)-1)))

/* 测试线程是否在'twups'列表中 */
#define isintwups(L)  (L->twups != L)

/* 闭包中的最大upvalues数(包括C和Lua). (值必须适合VM寄存器) */
#define MAXUPVAL  255

/*
Upvalue指在闭包生成的那一刻，与函数原型绑定在一起的那些外部变量。
这些变量原本是上一层函数的局部变量或 upvalue ，可以在上层返回返回后，
继续被闭包引用。遍历当前所有开放的upvalue利用的是当前线程中记录的链表 openupval。
这是一个双向链表，所以在UpVal结构中有两个指针指向前后节点。链表指针保存在一个联合中。
当upvalue被关闭后，就不再需要这两个指针了。
所谓关闭upvalue，就是当upvalue引用的数据栈上的数据不再存在于栈上时(通常是由申请局部变量的函数返回引起的)，
需要把upvalue从开放链表中拿掉，并把其引用的数据栈上的变量值换一个安全的地方存放。这个安全所在就是UpVal结构体内。无须用特别的标记位区分一个UpVal在开放还是关闭状态。
当upvalue关闭时，UpVal中的指针v一定 指向结构内部的value 。
*/

/*
** Lua闭包中的Upvalues
** 闭包是函数与upvalue的结合
** upvalue是指与函数原型绑定在一起的那些外部变量. 这些变量原本是上一层函数的局部变量或upvalue,
** 可以在上层返回后, 继续被闭包使用
*/
struct UpVal
{
    /* 引用lua值变量, 当被引用的变量还在数据栈上时, 这个指针直接指向栈上的地址. 这个upvalue是开放的.
       由于lua的数据栈的大小可以扩展, 当数据栈延展时, 其内存地址回发生改变, 需修正UpVal结构中的指针. ldo.c中correctstack
       遍历当前所有开放的upvalue利用的是当前线程中记录的链表openupval.
       当upvalue关闭后, 即upvalue引用的数据栈上的数据不在存在于栈上时(一些局部变量). 需要将upvalue从开放链表中拿掉,
       并将其引用的数据栈上的变量值换一个安全的地方存放, 即UpVal结构中的value */
    TValue* v;  /* 指向栈上的数据(开放的upvalue)或下面联合中的value(关闭的upvalue) */
    lu_mem refcount;  /* 引用计数 */
    union {
        struct {  /* upvalue是开放的(在数据栈上) */
            UpVal* next;  /* 链表(下一节点) */
            int touched;  /* 标记以避免具有死线程的循环 */
        } open;
        TValue value;  /* upvalue是关闭的(拷贝到value中) */
    } u;
};

/* 判断Upval是否是开放的 */
#define upisopen(up)  ((up)->v != &(up)->u.value)

/* 创建函数原型 */
LUAI_FUNC Proto* luaF_newproto(lua_State* L);
/* 释放函数原型 */
LUAI_FUNC void luaF_freeproto(lua_State* L, Proto* f);
/* 构造一个新的C闭包 */
LUAI_FUNC CClosure* luaF_newCclosure(lua_State* L, int nelems);
/* 构造一个新的Lua闭包 */
LUAI_FUNC LClosure* luaF_newLclosure(lua_State* L, int nelems);
/* 使用关闭的upvalue填充一个Lua闭包, upvalue不在数据栈上 */
LUAI_FUNC void luaF_initupvals(lua_State* L, LClosure* cl);
/* 在数据栈上查找开放的upvalue */
LUAI_FUNC UpVal* luaF_findupval(lua_State* L, StkId level);
/* 删除无人引用的upvalue, 把数据从数据栈上复制到UpVal结构中, 并修正UpVal中的指针v */
LUAI_FUNC void luaF_close(lua_State* L, StkId level);
LUAI_FUNC const char* luaF_getlocalname(const Proto* func, int local_number,
    int pc);


#endif
