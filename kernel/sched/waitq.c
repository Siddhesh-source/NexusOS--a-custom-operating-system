#include "waitq.h"
#include "sched.h"
#include "percpu.h"
#include "panic.h"

void waitq_init(struct waitq *wq)
{
    spin_lock_init(&wq->lock, LOCK_CLASS_WAITQ);
    for (unsigned i = 0; i < WAITQ_MAX; i++) {
        wq->used[i] = false;
        wq->waiters[i] = 0;
    }
    wq->wakeups = 0;
}

void waitq_wait(struct waitq *wq, bool (*cond)(void *arg), void *arg)
{
    if (wq == NULL || cond == NULL)
        panic("waitq_wait: missing queue or condition");
    for (;;) {
        uint64_t flags;
        spin_lock_irqsave(&wq->lock, &flags);
        if (cond(arg)) {
            spin_unlock_irqrestore(&wq->lock, flags);
            return;
        }
        unsigned slot = WAITQ_MAX;
        for (unsigned i = 0; i < WAITQ_MAX && slot == WAITQ_MAX; i++)
            if (!wq->used[i])
                slot = i;
        if (slot == WAITQ_MAX) {
            /* Queue full: check again next tick instead. */
            spin_unlock_irqrestore(&wq->lock, flags);
            sched_sleep(1);
            continue;
        }

        struct thread *me = percpu_current();
        wq->used[slot] = true;
        wq->waiters[slot] = me->tid;

        /* Lock order: wait queue, then run queue. We mark ourselves
         * BLOCKED, let go of the queue, and switch away still holding our
         * run-queue lock: a waker on another CPU waits for that lock, so
         * it cannot make us READY before our registers are saved. */
        struct runqueue *rq = sched_lock_this_cpu();
        me->wait_on = wq;
        if (thread_set_state_locked(me, TASK_BLOCKED) != PROC_OK) {
            me->wait_on = NULL;
            wq->used[slot] = false;
            sched_unlock(rq);
            spin_unlock_irqrestore(&wq->lock, flags);
            continue;
        }
        spin_unlock(&wq->lock);
        sched_reschedule_locked(rq);         /* returns once woken */
        irq_restore(flags);
        /* Loop: the condition may be false again (another reader won). */
    }
}

unsigned waitq_wake_all(struct waitq *wq)
{
    unsigned woken = 0;
    uint64_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    for (unsigned i = 0; i < WAITQ_MAX; i++) {
        if (!wq->used[i])
            continue;
        wq->used[i] = false;
        /* Interrupts are off, so this is an RCU read section: the thread
         * found cannot be freed under us. */
        struct thread *t = thread_find(wq->waiters[i]);
        if (t == NULL)
            continue;                        /* killed and reaped meanwhile */
        struct runqueue *rq = sched_lock_thread(t);
        if (t->wait_on == wq && t->state == TASK_BLOCKED) {
            t->wait_on = NULL;
            if (thread_set_state_locked(t, TASK_READY) == PROC_OK)
                woken++;
        }
        sched_unlock(rq);
    }
    wq->wakeups += woken;
    spin_unlock_irqrestore(&wq->lock, flags);
    return woken;
}

unsigned waitq_sleepers(struct waitq *wq)
{
    unsigned n = 0;
    uint64_t flags;
    spin_lock_irqsave(&wq->lock, &flags);
    for (unsigned i = 0; i < WAITQ_MAX; i++)
        n += wq->used[i];
    spin_unlock_irqrestore(&wq->lock, flags);
    return n;
}
