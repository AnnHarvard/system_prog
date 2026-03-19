#include "thread_pool.h"

#include <pthread.h>
#include <atomic>
#include <deque>
#include <vector>

enum task_state {
	TASK_STATE_NEW = 0,
	TASK_STATE_QUEUED,
	TASK_STATE_RUNNING,
	TASK_STATE_FINISHED,
	TASK_STATE_JOINED,
};

struct thread_task {
	thread_task_f function;

	pthread_mutex_t mutex;
	pthread_cond_t cond;

	struct thread_pool *pool;
	task_state state;

	bool detached;
};

struct thread_pool {
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	std::vector<pthread_t> threads;
	std::deque<thread_task *> queue;

	int max_threads;
	std::atomic<int> task_count;
	bool stopping;
};

static void *
thread_pool_worker(void *arg)
{
	thread_pool *pool = static_cast<thread_pool *>(arg);

	for (;;) {
		pthread_mutex_lock(&pool->mutex);

		while (pool->queue.empty() && !pool->stopping) {
			pthread_cond_wait(&pool->cond, &pool->mutex);
		}

		if (pool->stopping && pool->queue.empty()) {
			pthread_mutex_unlock(&pool->mutex);
			return nullptr;
		}

		thread_task *task = pool->queue.front();
		pool->queue.pop_front();

		pthread_mutex_unlock(&pool->mutex);

		pthread_mutex_lock(&task->mutex);
		task->state = TASK_STATE_RUNNING;
		pthread_mutex_unlock(&task->mutex);

		try {
			task->function();
		} catch (...) {
		}

		pthread_mutex_lock(&task->mutex);
		task->state = TASK_STATE_FINISHED;
		pthread_cond_broadcast(&task->cond);

		bool need_delete = task->detached;
		thread_pool *task_pool = task->pool;
		pthread_mutex_unlock(&task->mutex);

		if (need_delete) {
			pthread_cond_destroy(&task->cond);
			pthread_mutex_destroy(&task->mutex);
			delete task;

			task_pool->task_count.fetch_sub(1);
		}
	}
}

int
thread_pool_new(int thread_count, struct thread_pool **pool)
{
	if (pool == nullptr || thread_count <= 0 || thread_count > TPOOL_MAX_THREADS) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	thread_pool *result = new thread_pool;

	pthread_mutex_init(&result->mutex, nullptr);
	pthread_cond_init(&result->cond, nullptr);

	result->max_threads = thread_count;
	result->task_count.store(0);
	result->stopping = false;

	*pool = result;
	return 0;
}

int
thread_pool_delete(struct thread_pool *pool)
{
	if (pool == nullptr) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	if (pool->task_count.load() != 0) {
		return TPOOL_ERR_HAS_TASKS;
	}

	pthread_mutex_lock(&pool->mutex);
	pool->stopping = true;
	pthread_cond_broadcast(&pool->cond);
	pthread_mutex_unlock(&pool->mutex);

	for (pthread_t tid : pool->threads) {
		pthread_join(tid, nullptr);
	}

	pthread_cond_destroy(&pool->cond);
	pthread_mutex_destroy(&pool->mutex);

	delete pool;
	return 0;
}

int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	if (pool == nullptr || task == nullptr) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	int expected = pool->task_count.load();
	while (true) {
		if (expected >= TPOOL_MAX_TASKS) {
			return TPOOL_ERR_TOO_MANY_TASKS;
		}
		if (pool->task_count.compare_exchange_weak(expected, expected + 1)) {
			break;
		}
	}

	pthread_mutex_lock(&task->mutex);

	if (task->pool != nullptr || task->state == TASK_STATE_QUEUED ||
	    task->state == TASK_STATE_RUNNING || task->state == TASK_STATE_FINISHED) {
		pthread_mutex_unlock(&task->mutex);
		pool->task_count.fetch_sub(1);
		return TPOOL_ERR_TASK_IN_POOL;
	}

	task->pool = pool;
	task->state = TASK_STATE_QUEUED;
	pthread_mutex_unlock(&task->mutex);

	pthread_mutex_lock(&pool->mutex);

	pool->queue.push_back(task);

	while (static_cast<int>(pool->threads.size()) < pool->max_threads &&
	       pool->queue.size() > pool->threads.size()) {
		pthread_t tid;
		if (pthread_create(&tid, nullptr, thread_pool_worker, pool) != 0) {
			break;
		}
		pool->threads.push_back(tid);
	}

	pthread_cond_broadcast(&pool->cond);
	pthread_mutex_unlock(&pool->mutex);

	return 0;
}

int
thread_task_new(struct thread_task **task, const thread_task_f &function)
{
	if (task == nullptr) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	thread_task *result = new thread_task;

	result->function = function;
	pthread_mutex_init(&result->mutex, nullptr);
	pthread_cond_init(&result->cond, nullptr);

	result->pool = nullptr;
	result->state = TASK_STATE_NEW;
	result->detached = false;

	*task = result;
	return 0;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
	if (task == nullptr) {
		return false;
	}

	pthread_mutex_lock(const_cast<pthread_mutex_t *>(&task->mutex));
	bool result = (task->state == TASK_STATE_JOINED);
	pthread_mutex_unlock(const_cast<pthread_mutex_t *>(&task->mutex));

	return result;
}

bool
thread_task_is_running(const struct thread_task *task)
{
	if (task == nullptr) {
		return false;
	}

	pthread_mutex_lock(const_cast<pthread_mutex_t *>(&task->mutex));
	bool result = (task->state == TASK_STATE_RUNNING);
	pthread_mutex_unlock(const_cast<pthread_mutex_t *>(&task->mutex));

	return result;
}

int
thread_task_join(struct thread_task *task)
{
	if (task == nullptr) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	pthread_mutex_lock(&task->mutex);

	if (task->state == TASK_STATE_NEW) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	while (task->state != TASK_STATE_FINISHED && task->state != TASK_STATE_JOINED) {
		pthread_cond_wait(&task->cond, &task->mutex);
	}

	if (task->state == TASK_STATE_FINISHED) {
		thread_pool *pool = task->pool;
		task->state = TASK_STATE_JOINED;
		task->pool = nullptr;
		pthread_mutex_unlock(&task->mutex);

		if (pool != nullptr) {
			pool->task_count.fetch_sub(1);
		}

		return 0;
	}

	pthread_mutex_unlock(&task->mutex);
	return 0;
}

#if NEED_TIMED_JOIN

int
thread_task_timed_join(struct thread_task *task, double timeout)
{
	if (task == nullptr) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	pthread_mutex_lock(&task->mutex);

	if (task->state == TASK_STATE_NEW) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	if (task->state == TASK_STATE_FINISHED || task->state == TASK_STATE_JOINED) {
		task->state = TASK_STATE_JOINED;
		task->pool->task_count.fetch_sub(1);
		pthread_mutex_unlock(&task->mutex);
		return 0;
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	ts.tv_sec += static_cast<time_t>(timeout);
	ts.tv_nsec += static_cast<long>((timeout - static_cast<time_t>(timeout)) * 1e9);

	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += ts.tv_nsec / 1000000000L;
		ts.tv_nsec %= 1000000000L;
	}

	int rc = 0;
	while (task->state != TASK_STATE_FINISHED && task->state != TASK_STATE_JOINED) {
		rc = pthread_cond_timedwait(&task->cond, &task->mutex, &ts);
		if (rc == ETIMEDOUT) {
			pthread_mutex_unlock(&task->mutex);
			return TPOOL_ERR_TIMEOUT;
		}
	}

	if (task->state == TASK_STATE_FINISHED) {
		task->state = TASK_STATE_JOINED;
		task->pool->task_count.fetch_sub(1);
	}

	pthread_mutex_unlock(&task->mutex);
	return 0;
}

#endif

int
thread_task_delete(struct thread_task *task)
{
	if (task == nullptr) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	pthread_mutex_lock(&task->mutex);

	if (task->state == TASK_STATE_QUEUED ||
	    task->state == TASK_STATE_RUNNING ||
	    task->state == TASK_STATE_FINISHED) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_IN_POOL;
	}

	pthread_mutex_unlock(&task->mutex);

	pthread_cond_destroy(&task->cond);
	pthread_mutex_destroy(&task->mutex);
	delete task;

	return 0;
}

#if NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
	if (task == nullptr) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	pthread_mutex_lock(&task->mutex);

	if (task->pool == nullptr) {
		pthread_mutex_unlock(&task->mutex);
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	if (task->state == TASK_STATE_FINISHED) {
		pthread_mutex_unlock(&task->mutex);

		pthread_cond_destroy(&task->cond);
		pthread_mutex_destroy(&task->mutex);

		task->pool->task_count.fetch_sub(1);
		delete task;
		return 0;
	}

	task->detached = true;

	pthread_mutex_unlock(&task->mutex);
	return 0;
}

#endif
