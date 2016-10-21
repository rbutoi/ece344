#include <valgrind/valgrind.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <ucontext.h>
#include <signal.h>
#include "thread.h"
#include "interrupt.h"

// TCB
/* ------------------------------------------------------------ */
typedef struct {
	unsigned char alive;

	ucontext_t ctx;
	void *stack;
} tcb;
/* ------------------------------------------------------------ */

// circular Q
/* ------------------------------------------------------------ */
// + 1 because of the need for an extra element to indicate empty vs full
#define Q_SIZE (THREAD_MAX_THREADS + 1)
typedef struct {
	Tid q[Q_SIZE];
	unsigned start, end; 
} circular_q;

void q_init(circular_q *q)
{
	q->start = q->end = 0;
}

int q_full(const circular_q *q)
{
	return q->start == ((q->end + 1) % Q_SIZE);
}

int q_empty(const circular_q *q)
{
	return q->start == q->end;
}

int q_size(const circular_q *q)
{
	unsigned size = q->end - q->start;
	return size < 0 ? size + Q_SIZE : size; 
}

void q_print(const circular_q *q)
{
	unsigned i = q->start;
	while (i != q->end) {
		printf("%d ", q->q[i]);
		i = (i + 1) % Q_SIZE;
	}
	printf("\n");
}

void q_enq(circular_q *q, int a)
{
	assert( !q_full(q) );

	q->q[q->end] = a;
	q->end = (q->end + 1) % Q_SIZE;
}

int q_deq(circular_q *q)
{
	assert( !q_empty(q) );

	int ret = q->q[q->start];
	q->start = (q->start + 1) % Q_SIZE;
	return ret;
}

void q_replace_once(circular_q *q, Tid from, Tid to)
{
	unsigned i = q->start;
	while (i != q->end) {
		if (q->q[i] == from) {
			q->q[i] = to;
			break;
		}
		i = (i + 1) % Q_SIZE;
	}
}

void q_delete_arbitrary(circular_q *q, Tid del)
{
	unsigned i = q->start;
	while (i != q->end) {
		if (q->q[i] == del) {
			q->q[i] = q_deq(q);
			break;
		}
		i = (i + 1) % Q_SIZE;
	}
}

/* ------------------------------------------------------------ */

// globals
Tid curr_th;
circular_q ready_q, kill_q; 
tcb tcbs[THREAD_MAX_THREADS];
volatile int setcontext_called;

// Utility funcs
int valid_thd(Tid tid)
{
	return tid >= 0 && tid < THREAD_MAX_THREADS;
}

Tid find_first_unused_tid()
{
	Tid t = 0;

	while (t < THREAD_MAX_THREADS && tcbs[t].alive) {
		++t;
	}

	if (t == THREAD_MAX_THREADS) {
		return THREAD_NOMORE;
	} else {
		return t;
	}
}

void
thread_stub(void (*thread_main)(void *), void *arg)
{
	Tid ret;

	interrupts_on();

	thread_main(arg); // call thread_main() function with arg
	ret = thread_exit();
	// we should only get here if we are the last thread. 
	assert(ret == THREAD_NONE);
	// all threads are done, so process should exit
	exit(0);
}

void thread_destroy(Tid tid)
{
	assert(tid != thread_id());

	free(tcbs[tid].stack);
	tcbs[tid].alive = 0;;
}

Tid actually_call_setcontext(Tid want_tid, int interrupts_enabled);

// thread API funcs
void
thread_init(void)
{
	curr_th = 0;
	q_init(&ready_q);

	tcbs[0].alive = 1;
	int i;
	for (i = 1; i < THREAD_MAX_THREADS; ++i) {
		tcbs[i].alive = 0;
	}
}

Tid
thread_id()
{
	return curr_th;
}

Tid
thread_create(void (*fn) (void *), void *parg)
{
	int enabled = interrupts_off();

	Tid tid = find_first_unused_tid();
	if (tid == THREAD_NOMORE) {
		interrupts_set(enabled);
		return tid;
	}

	assert( valid_thd(tid) );

	// set up tcb
	tcbs[tid].alive = 1;
	// make stack
	// + 8 is for 1/2 of 16, to snap to alignment
	void *stack = tcbs[tid].stack = malloc(THREAD_MIN_STACK + 8);
	VALGRIND_STACK_REGISTER(stack, stack + THREAD_MIN_STACK + 8);
	if (!stack) {
		interrupts_set(enabled);
		return THREAD_NOMEMORY;
	}

	// have it point to end
	stack += THREAD_MIN_STACK + 8;

	// align
	stack -= ((uintptr_t) stack % 16) + 8;

	// make context from current one
	int ret = getcontext(&tcbs[tid].ctx);
	assert(!ret);

	tcbs[tid].ctx.uc_mcontext.gregs[REG_RSP] = (uintptr_t) stack;
	tcbs[tid].ctx.uc_mcontext.gregs[REG_RIP] = (uintptr_t) thread_stub;
	tcbs[tid].ctx.uc_mcontext.gregs[REG_RDI] = (uintptr_t) fn;
	tcbs[tid].ctx.uc_mcontext.gregs[REG_RSI] = (uintptr_t) parg;

	// add to ready Q
	q_enq(&ready_q, tid);

	interrupts_set(enabled);

	return tid;
}

Tid
thread_yield(Tid want_tid)
{
	int enabled = interrupts_off();

	// delete shit if possible. for kill_thread
	if (!q_empty(&kill_q)) {
		int self_found = 0;
		while (!q_empty(&kill_q)) {
			Tid to_del = q_deq(&kill_q);
			if (to_del == thread_id()) {
				q_enq(&kill_q, to_del);
				if (self_found) {
					break;
				} else {
					self_found = 1;
				}
			} else {
				thread_destroy(to_del);
				q_delete_arbitrary(&ready_q, to_del);
			}
		}
	}

	switch (want_tid) {
	case THREAD_ANY:
		if (q_empty(&ready_q)) {
			interrupts_set(enabled);
			return THREAD_NONE;
		}
		if (q_size(&ready_q) == 1 && ready_q.q[ready_q.start] == thread_id()) {
			assert(0);
		}

		// yield to top thread in queue
		want_tid = q_deq(&ready_q);
		q_enq(&ready_q, thread_id());
		break;
	case THREAD_SELF:
		want_tid = thread_id();
		break;
	default:
		if (!valid_thd(want_tid) || !tcbs[want_tid].alive) {
			interrupts_set(enabled);
			return THREAD_INVALID;
		}
		q_replace_once(&ready_q, want_tid, thread_id());
	}
	return actually_call_setcontext(want_tid, enabled);
}

Tid actually_call_setcontext(Tid want_tid, int interrupts_enabled)
{
	assert(valid_thd(want_tid));

	setcontext_called = 0;
	int ret = getcontext( &tcbs[ thread_id() ].ctx );
	assert(!ret);

	if (setcontext_called) {
		interrupts_set(interrupts_enabled);
		return want_tid;
	}

	setcontext_called = 1;
	curr_th = want_tid;
	ret = setcontext( &tcbs[ want_tid ].ctx );

	assert(!ret);
	assert(0 && "BADNESS");
	return THREAD_FAILED;
}

Tid
thread_exit()
{
	int enabled = interrupts_off();

	if (q_empty(&ready_q)) {
		interrupts_set(enabled);
		return THREAD_NONE;
	}

	q_enq(&kill_q, thread_id());

	// yield to any thread
	Tid want_tid = q_deq(&ready_q);
	q_enq(&ready_q, thread_id());

	return actually_call_setcontext(want_tid, enabled);
}

Tid
thread_kill(Tid tid)
{
	int enabled = interrupts_off();

	if (tid == thread_id() || !valid_thd(tid) || !tcbs[tid].alive) {
		interrupts_set(enabled);
		return THREAD_INVALID;
	}

	q_enq(&kill_q, tid);

	interrupts_set(enabled);
	return tid;
}

/*******************************************************************
 * Important: The rest of the code should be implemented in Lab 3. *
 *******************************************************************/

/* This is the wait queue structure */
struct wait_queue {
	circular_q q;
};

struct wait_queue *
wait_queue_create()
{
	struct wait_queue *wq;

	wq = malloc(sizeof(struct wait_queue));
	assert(wq);

	q_init(&wq->q);

	return wq;
}

void
wait_queue_destroy(struct wait_queue *wq)
{
	assert(q_empty(&wq->q));

	free(wq);
}

Tid
thread_sleep(struct wait_queue *queue)
{
	int enabled = interrupts_off();

	if (!queue) {
		interrupts_set(enabled);
		return THREAD_INVALID;
	}

	if (q_empty(&ready_q)) {
		interrupts_set(enabled);
		return THREAD_NONE;
	}

	Tid want_tid = q_deq(&ready_q);
	q_enq(&queue->q, thread_id());
	return actually_call_setcontext(want_tid, enabled);
}

/* when the 'all' parameter is 1, wakeup all threads waiting in the queue.
 * returns whether a thread was woken up on not. */
int
thread_wakeup(struct wait_queue *queue, int all)
{
	int enabled = interrupts_off();

	int n = 0;
	if (!queue) {
		interrupts_set(enabled);
		return 0;
	}

	if (all) {
		while (!q_empty(&queue->q)) {
			q_enq(&ready_q, q_deq(&queue->q));
			++n;
		}
	} else {
		if (!q_empty(&queue->q)) {
			q_enq(&ready_q, q_deq(&queue->q));
			++n;
		}
	}

	interrupts_set(enabled);

	return n;
}

struct lock {
	struct wait_queue q;
	int avail;
	Tid who;
};

struct lock *
lock_create()
{
	struct lock *lock;

	lock = malloc(sizeof(struct lock));
	assert(lock);

	q_init(&lock->q.q);
	lock->avail = 1;

	return lock;
}

void
lock_destroy(struct lock *lock)
{
	int enabled = interrupts_off();

	assert(lock != NULL);

	assert(lock->avail);
	/* => */
	assert(q_empty(&lock->q.q));

	free(lock);

	interrupts_set(enabled);
}

void
lock_acquire(struct lock *lock)
{
	int enabled = interrupts_off();

	assert(lock != NULL);

	while (!lock->avail) {
		thread_sleep(&lock->q);
	}

	lock->avail = 0;
	lock->who = thread_id();

	interrupts_set(enabled);
}

void
lock_release(struct lock *lock)
{
	int enabled = interrupts_off();

	assert(lock != NULL);

	assert(!lock->avail);
	assert(lock->who == thread_id());

	lock->avail = 1;
	thread_wakeup(&lock->q, 1);

	interrupts_set(enabled);
}

struct cv {
	struct wait_queue q;
};

struct cv *
cv_create()
{
	struct cv *cv;

	cv = malloc(sizeof(struct cv));
	assert(cv);

	q_init(&cv->q.q);

	return cv;
}

void
cv_destroy(struct cv *cv)
{
	assert(cv != NULL);

	assert(q_empty(&cv->q.q));

	free(cv);
}

void
cv_wait(struct cv *cv, struct lock *lock)
{
	int enabled = interrupts_off();

	assert(cv != NULL);
	assert(lock != NULL);

	assert(!lock->avail && lock->who == thread_id());

	lock_release(lock);
	thread_sleep(&cv->q);

	interrupts_set(enabled);

	lock_acquire(lock);
}

void
cv_signal(struct cv *cv, struct lock *lock)
{
	int enabled = interrupts_off();

	assert(cv != NULL);
	assert(lock != NULL);

	assert(!lock->avail && lock->who == thread_id());

	if (!q_empty(&cv->q.q)) {
		q_enq(&ready_q, q_deq(&cv->q.q));
	}

	interrupts_set(enabled);
}

void
cv_broadcast(struct cv *cv, struct lock *lock)
{
	int enabled = interrupts_off();

	assert(cv != NULL);
	assert(lock != NULL);

	assert(!lock->avail && lock->who == thread_id());

	while (!q_empty(&cv->q.q)) {
		q_enq(&ready_q, q_deq(&cv->q.q));
	}

	interrupts_set(enabled);
}
