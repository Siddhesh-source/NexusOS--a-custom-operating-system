#ifndef BHARAT_WAITQ_H
#define BHARAT_WAITQ_H

/* Wait queues: let a thread sleep until something happens (a key is
 * pressed, data arrives), instead of checking again and again.
 *
 *     waitq_wait(&wq, has_data, arg);   // sleeps until has_data(arg) is true
 *     ...
 *     make the condition true, then:
 *     waitq_wake_all(&wq);              // from a thread or an interrupt
 *
 * The condition is checked with the queue's lock held, and the waker
 * takes the same lock, so a wake-up that happens between "check" and
 * "sleep" is never lost: either the waiter sees the new data, or it is
 * already on the queue when the waker looks.
 *
 * Waiters are remembered by thread ID, not by pointer, so a waiter that
 * is killed while asleep leaves no dangling pointer behind: the waker
 * looks the ID up and skips it if it is gone.
 *
 * Limits: WAITQ_MAX sleepers per queue; extra ones fall back to checking
 * every tick. No timeouts. */

#include <stdbool.h>
#include <stdint.h>
#include "spinlock.h"
#include "proc.h"

#define WAITQ_MAX 8

struct waitq {
    struct spinlock lock;
    tid_t waiters[WAITQ_MAX];       /* 0 = free slot (TID 0 is kmain) */
    bool used[WAITQ_MAX];
    uint64_t wakeups;               /* threads woken so far */
};

#define WAITQ_INIT { .lock = SPINLOCK_INIT(LOCK_CLASS_WAITQ) }

void waitq_init(struct waitq *wq);

/* Sleep until cond(arg) is true. cond runs with interrupts off and the
 * queue's lock held: it must be quick and must not take sleeping paths.
 * Needs the scheduler running and no spinlock held by the caller. */
void waitq_wait(struct waitq *wq, bool (*cond)(void *arg), void *arg);

/* Wake every thread sleeping on the queue. Safe from interrupts. Returns
 * the number of threads woken. */
unsigned waitq_wake_all(struct waitq *wq);

/* Threads sleeping on the queue right now. */
unsigned waitq_sleepers(struct waitq *wq);

#endif /* BHARAT_WAITQ_H */
