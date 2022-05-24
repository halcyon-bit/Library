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

#ifndef _THRDPOOL_H_
#define _THRDPOOL_H_

#include <stddef.h>

// 线程池
typedef struct __thrdpool thrdpool_t;

// 线程池中的任务
struct thrdpool_task
{
	void (*routine)(void *);  // 回调函数
	void *context;  // 数据
};

// 创建线程池(nthreads: 线程池数量, stacksize: 未用到)
thrdpool_t *thrdpool_create(size_t nthreads, size_t stacksize);
// 添加任务到线程池中(task: 任务, pool: 线程池)
int thrdpool_schedule(const struct thrdpool_task *task, thrdpool_t *pool);
// 扩大线程池, 增加1个线程
int thrdpool_increase(thrdpool_t *pool);
// 判断当前线程是否在线程池中
int thrdpool_in_pool(thrdpool_t *pool);
// 中止线程池(pending: 对线程池中残留的任务做额外的处理, pool: 线程池)
void thrdpool_destroy(void (*pending)(const struct thrdpool_task *),
					  thrdpool_t *pool);
// pending: 主要是处理任务数据, 用户是否有申请的内存需要释放
#endif

