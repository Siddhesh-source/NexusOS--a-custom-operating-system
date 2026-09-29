#ifndef NEXUS_SCHED_H
#define NEXUS_SCHED_H

/* Preemptive round-robin scheduler (Phase 5).
 *
 * Schedules THREADS (processes are just containers). Single CPU.
 *
 *   ready queue : FIFO of exactly the READY threads (except idle), kept in
 *                 sync by proc.c, which reports every state change through
 *                 sched_state_changed()
 *   sleep list  : BLOCKED threads waiting for a tick count
 *   idle thread : runs only when nothing else is READY
 *   quantum     : SCHED_QUANTUM_TICKS timer ticks
 *
 * Preemption: the PIT interrupt handler calls sched_tick() on the interrupted
 * thread's own kernel stack. When the running thread's quantum expires and
 * another thread is READY, sched_tick switches threads right there (see
 * docs/scheduler.md for the interrupt-frame / cpu_context layering). */

#include <stdint.h>
#include <stdbool.h>
#include "proc.h"

#define SCHED_TIMER_HZ       100   /* PIT rate set in timer.c */
#define SCHED_QUANTUM_TICKS  2     /* 20 ms time slice */

struct sched_stats {
    uint64_t ticks;            /* timer ticks since sched_start */
    uint64_t invocations;      /* scheduler entries: ticks + yield/sleep/exit,
                                  whether or not they switched */
    uint64_t switches;         /* actual context switches */
    uint64_t preemptions;      /* switches forced by quantum expiry */
    uint64_t voluntary;        /* switches from yield/sleep/exit */
    uint64_t idle_ticks;       /* ticks spent in the idle thread */
    uint64_t idle_switches;    /* times the idle thread was switched to */
    unsigned runnable;         /* READY threads in the queue */
    unsigned sleeping;         /* threads on the sleep list */
    tid_t current;             /* running thread */
};

/* Register the running boot flow and create the idle thread. Requires
 * proc_init(). Does not enable preemption yet. */
void sched_init(void);

/* Enable timer-driven preemption. */
void sched_start(void);
bool sched_active(void);

/* Called from the timer IRQ handler (interrupts off, EOI already sent). */
void sched_tick(void);

/* Give up the CPU to the next READY thread, if any (stays READY). */
void sched_yield(void);

/* Block the calling thread for at least `ticks` timer ticks. */
void sched_sleep(uint64_t ticks);

/* Switch away from a thread that has just marked itself BLOCKED or
 * TERMINATED. Used by thread_exit; does not return for TERMINATED. */
void sched_reschedule(void);

/* Hook: proc.c reports every thread state change (interrupts off). */
void sched_state_changed(struct thread *t, task_state_t from, task_state_t to);

struct thread *sched_idle_thread(void);
uint64_t sched_ticks(void);
void sched_get_stats(struct sched_stats *out);
void sched_print_stats(void);

/* Verify queue invariants: only READY threads queued, no TERMINATED thread
 * reachable, counts consistent. Returns true if consistent. */
bool sched_check(void);

/* Rate-limited switch tracing: log at most `lines` switches, then stop.
 * 0 disables. Off by default so tracing cannot distort the demo. */
void sched_set_trace(unsigned lines);

#endif /* NEXUS_SCHED_H */
