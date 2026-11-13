#ifndef BHARAT_SCHED_H
#define BHARAT_SCHED_H

/* Preemptive round-robin scheduler, tickless.
 *
 * Schedules THREADS (processes are just containers). Single CPU.
 *
 *   ready queue : FIFO of exactly the READY threads (except idle), kept in
 *                 sync by proc.c, which reports every state change through
 *                 sched_state_changed()
 *   sleep list  : BLOCKED threads waiting for a wake-up time
 *   idle thread : runs only when nothing else is READY
 *   time slice  : SCHED_QUANTUM_TICKS ticks (20 ms)
 *
 * Tickless: there is no periodic timer interrupt. Time comes from the
 * clock (clock_now_ns). Whenever the scheduler decides something, it sets
 * ONE alarm for the earliest moment it next has work: the end of the
 * running thread's slice (only if another thread is waiting for the CPU)
 * or the next sleeper's wake-up. With nothing to do, no alarm is set and
 * the idle CPU is not woken at all.
 *
 * A "tick" (SCHED_TICK_NS = 10 ms) remains the unit of the tick-based
 * calls (sched_sleep, sched_ticks, cpu_ticks) so callers did not change;
 * it is now just a measure of time, not an interrupt.
 *
 * Preemption: the alarm interrupt calls sched_timer_event() on the
 * interrupted thread's own kernel stack, which may switch threads right
 * there (see docs/phases/phase-5-scheduler.md for the frame layering). */

#include <stdint.h>
#include <stdbool.h>
#include "proc.h"

#define SCHED_TIMER_HZ       100                 /* ticks per second */
#define SCHED_TICK_NS        (1000000000ULL / SCHED_TIMER_HZ)
#define SCHED_QUANTUM_TICKS  2                   /* 20 ms time slice */
#define SCHED_QUANTUM_NS     (SCHED_QUANTUM_TICKS * SCHED_TICK_NS)

struct sched_stats {
    uint64_t ticks;            /* ticks of time since sched_start */
    uint64_t invocations;      /* scheduler entries: timer events + yield/
                                  sleep/exit, whether or not they switched */
    uint64_t switches;         /* actual context switches */
    uint64_t preemptions;      /* switches forced by an expired slice */
    uint64_t voluntary;        /* switches from yield/sleep/exit */
    uint64_t idle_ticks;       /* ticks of time spent in the idle thread */
    uint64_t idle_switches;    /* times the idle thread was switched to */
    uint64_t timer_events;     /* alarm interrupts handled */
    uint64_t idle_timer_events;/* ... of which arrived while idle */
    uint64_t alarms_armed;     /* alarms set by the scheduler */
    unsigned runnable;         /* READY threads in the queue */
    unsigned sleeping;         /* threads on the sleep list */
    tid_t current;             /* running thread */
};

/* Register the running boot flow and create the idle thread. Requires
 * proc_init(). Does not enable preemption yet. */
void sched_init(void);

/* Enable preemption. Requires the clock (clock_init with
 * sched_timer_event as its alarm handler). */
void sched_start(void);
bool sched_active(void);

/* Called from the alarm interrupt (interrupts off, EOI already sent). */
void sched_timer_event(void);

/* Give up the CPU to the next READY thread, if any (stays READY). */
void sched_yield(void);

/* Block the calling thread for at least `ticks` ticks (0 counts as 1). */
void sched_sleep(uint64_t ticks);

/* Block the calling thread for at least `ns` nanoseconds. */
void sched_sleep_ns(uint64_t ns);

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

#endif /* BHARAT_SCHED_H */
