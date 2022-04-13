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

// �̳߳�
typedef struct __thrdpool thrdpool_t;

// �̳߳��е�����
struct thrdpool_task
{
	void (*routine)(void *);  // �ص�����
	void *context;  // ����
};

// �����̳߳�(nthreads: �̳߳�����, stacksize: δ�õ�)
thrdpool_t *thrdpool_create(size_t nthreads, size_t stacksize);
// ��������̳߳���(task: ����, pool: �̳߳�)
int thrdpool_schedule(const struct thrdpool_task *task, thrdpool_t *pool);
// �����̳߳�, ����1���߳�
int thrdpool_increase(thrdpool_t *pool);
// �жϵ�ǰ�߳��Ƿ����̳߳���
int thrdpool_in_pool(thrdpool_t *pool);
// ��ֹ�̳߳�(pending: ���̳߳��в���������������Ĵ���, pool: �̳߳�)
void thrdpool_destroy(void (*pending)(const struct thrdpool_task *),
					  thrdpool_t *pool);
// pending: ��Ҫ�Ǵ�����������, �û��Ƿ���������ڴ���Ҫ�ͷ�
#endif

