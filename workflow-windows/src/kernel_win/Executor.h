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

#ifndef _EXECUTOR_H_
#define _EXECUTOR_H_

#include <stddef.h>
#include <mutex>
#include "list.h"
#include "thrdpool.h"

// 线程执行器

// 执行队列
class ExecQueue
{
public:
	// 初始化
	int init();
	// 反初始化
	void deinit() { }

private:
	struct list_head task_list;  // 队列
	std::mutex mutex;

public:
	virtual ~ExecQueue() { }
	friend class Executor;
};

// 状态
#define ES_STATE_FINISHED	0  // 完成
#define ES_STATE_ERROR		1  // 错误
#define ES_STATE_CANCELED	2  // 取消

// 线程特有的任务基类
class ExecSession
{
private:
	virtual void execute() = 0;  // 执行任务
	virtual void handle(int state, int error) = 0;  // 任务(执行完成、取消、错误)后处理函数

protected:
	ExecQueue *get_queue() { return this->queue; }

private:
	ExecQueue *queue;

public:
	virtual ~ExecSession() { }
	friend class Executor;
};

// 线程执行器(内部包含线程池)
class Executor
{
public:
	// 初始化(线程数量)
	int init(size_t nthreads);
	// 反初始化
	void deinit();

	int request(ExecSession *session, ExecQueue *queue);

private:
	thrdpool_t *thrdpool;

private:
	static void executor_thread_routine(void *context);
	static void executor_cancel_tasks(const struct thrdpool_task *task);

public:
	virtual ~Executor() { }
};

#endif

