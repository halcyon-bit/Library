/*
  Copyright (c) 2019 Sogou, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Authors: Xie Han (xiehan@sogou-inc.com)
           Wu Jiaxu (wujiaxu@sogou-inc.com)
*/

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <thread>
#include <mutex>
#include <set>
#include <condition_variable>
#include "list.h"
#include "thrdpool.h"

// 线程池结构体
struct __thrdpool
{
	struct list_head task_queue;  // 任务队列(双向链表)  实际的任务类型是 __thrdpool_task_entry
	std::mutex mutex;
	std::condition_variable cond;
	std::vector<std::thread *> threads;  // 线程
	std::set<std::thread::id> threadids;  // 线程id
	bool terminate;  // 是否中止线程
};
// 任务
struct __thrdpool_task_entry
{
	struct list_head list;
	struct thrdpool_task task;
};
// 线程函数
static void __thrdpool_routine(thrdpool_t *pool)
{
	// 任务队列
	struct list_head **pos = &pool->task_queue.next;
	__thrdpool_task_entry *entry;  // 具体的任务
	std::unique_lock<std::mutex> lock(pool->mutex, std::defer_lock);  // 没有立即上锁

	while (1)
	{
		lock.lock();
		// 等待任务
		while (!pool->terminate && list_empty(&pool->task_queue))
			pool->cond.wait(lock);

		// 是否中止线程
		if (pool->terminate)
			break;

		// 获取任务
		entry = list_entry(*pos, __thrdpool_task_entry, list);
		list_del(*pos);  // 从队列中移除
		lock.unlock();

		// 执行任务
		entry->task.routine(entry->task.context);
		// 执行完成后删除
		delete entry;
	}
}
// 中止线程池(内部)
static void __thrdpool_terminate(thrdpool_t *pool)
{
	std::unique_lock<std::mutex> lock(pool->mutex);

	pool->terminate = true;
	pool->cond.notify_all();
	for (size_t i = 0; i < pool->threads.size(); i++)
	{
		std::thread *th = pool->threads[i];
		lock.unlock();  // 线程函数需要获取锁(为什么锁在这? 对 pool 的保护? thrdpool_increase?)
		th->join();
		lock.lock();
	}
}
// 创建线程池(内部)
static int __thrdpool_create_threads(size_t nthreads, size_t stacksize,
									 thrdpool_t *pool)
{
	// not support stacksize;
	for (size_t i = 0; i < nthreads; i++)
	{
		auto *th = new std::thread(__thrdpool_routine, pool);

		pool->threads.push_back(th);
		pool->threadids.emplace(th->get_id());
	}

	return 0;
}
// 创建线程池
thrdpool_t *thrdpool_create(size_t nthreads, size_t stacksize)
{
	thrdpool_t *pool = new __thrdpool;

	INIT_LIST_HEAD(&pool->task_queue);  // 初始化任务链表
	pool->threads.clear();  // 这里是否可以 reserve
	pool->terminate = false;
	// 创建线程池
	if (__thrdpool_create_threads(nthreads, stacksize, pool) >= 0)
		return pool;

	delete pool;
	return NULL;
}
// 添加任务到线程池中(内部)
void __thrdpool_schedule(const struct thrdpool_task *task, void *buf,
						 thrdpool_t *pool)
{
	__thrdpool_task_entry *entry = (__thrdpool_task_entry *)buf;
	entry->task = *task;
	std::lock_guard<std::mutex> lock(pool->mutex);
	// 添加到链表尾部, 唤醒线程
	list_add_tail(&entry->list, &pool->task_queue);
	pool->cond.notify_one();
}
// 添加任务到线程池中
int thrdpool_schedule(const struct thrdpool_task *task, thrdpool_t *pool)
{
	__thrdpool_schedule(task, new __thrdpool_task_entry, pool);
	return 0;
}
// 扩大线程池, 增加1个线程
int thrdpool_increase(thrdpool_t *pool)
{
	// 是否需要判断 pool->terminate, 若已终止则无需创建线程, 否则创建后就被销毁了
	std::lock_guard<std::mutex> lock(pool->mutex);
	auto *th = new std::thread(__thrdpool_routine, pool);

	pool->threads.push_back(th);
	pool->threadids.emplace(th->get_id());
	return 0;
}
// 判断当前线程是否在线程池中
int thrdpool_in_pool(thrdpool_t *pool)
{
	std::lock_guard<std::mutex> lock(pool->mutex);
	return pool->threadids.count(std::this_thread::get_id()) > 0;
}
// 中止线程池
void thrdpool_destroy(void (*pending)(const struct thrdpool_task *),
					  thrdpool_t *pool)
{
	__thrdpool_task_entry *entry;
	struct list_head *pos, *tmp;
	// 中止线程池
	__thrdpool_terminate(pool);
	// 遍历任务列表, 销毁任务
	list_for_each_safe(pos, tmp, &pool->task_queue)
	{
		entry = list_entry(pos, __thrdpool_task_entry, list);
		list_del(pos);
		if (pending)
			pending(&entry->task);  // 用户对任务做额外的处理

		delete entry;
	}

	for (auto& th : pool->threads)
		delete th;

	delete pool;
}

