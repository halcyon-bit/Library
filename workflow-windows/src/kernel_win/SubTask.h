﻿/*
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

#ifndef _SUBTASK_H_
#define _SUBTASK_H_

#include <stddef.h>
#include <atomic>

class ParallelTask;
// 所有任务的基类
class SubTask
{
public:
	// 任务派发
	virtual void dispatch() = 0;

private:
	// 任务执行完成后会调用
	virtual SubTask *done() = 0;

protected:
	// 任务执行完成后调用, 内部调用实际任务的 done()
	void subtask_done();

public:
	ParallelTask *get_parent_task() const { return this->parent; }
	void *get_pointer() const { return this->pointer; }
	void set_pointer(void *pointer) { this->pointer = pointer; }

private:
	ParallelTask *parent;
	SubTask **entry;
	void *pointer;

public:
	SubTask()
	{
		this->parent = NULL;
		this->entry = NULL;
		this->pointer = NULL;
	}

	virtual ~SubTask() { }
	friend class ParallelTask;
};

class ParallelTask : public SubTask
{
public:
	ParallelTask(SubTask **subtasks, size_t n)
	{
		this->subtasks = subtasks;
		this->subtasks_nr = n;
	}

	SubTask **get_subtasks(size_t *n)
	{
		*n = this->subtasks_nr;
		return this->subtasks;
	}

	void set_subtasks(SubTask **subtasks, size_t n)
	{
		this->subtasks = subtasks;
		this->subtasks_nr = n;
	}

public:
	virtual void dispatch();

protected:
	SubTask **subtasks;
	size_t subtasks_nr;

private:
    std::atomic<size_t> nleft;
	friend class SubTask;
};

#endif

