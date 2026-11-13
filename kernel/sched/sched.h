#ifndef BHARAT_SCHED_H
#define BHARAT_SCHED_H

/* Preemptive round-robin scheduler, tickless, one run queue per CPU.
 *
 * Schedules THREADS (processes are just containers).
 *
 * Each CPU has its own run queue (struct runqueue), guarded by its own
 * spinlock, so CPUs schedule in parallel without waiting for each other:
 *
 *   ready list  : FIFO of the READY threads that belong to this CPU (not
 *                 the idle thread), kept in sync by proc.c, which reports
 *                 every state change through sched_state_changed()
 *   sleep list  : BLOCKED threads of this CPU waiting for a wake-up time
 *   idle thread : one per CPU; runs only when nothing else is READY there
 *   time slice  : SCHED_QUANTUM_TICKS ticks (20 ms)
 *
 * A thread belongs to one CPU at a time (thread->cpu). Its state and list
 * links are changed only while holding that CPU's run-queue lock
 * (sched_lock_thread). Moving work between CPUs:
 *
 *   - placement: a new thread that may run anywhere goes to the CPU with
 *     the fewest READY threads;
 *   - work stealing: a CPU about to go idle takes the oldest READY thread
 *     from another CPU's queue;
 *   - a CPU that queues work for another CPU, or wants an idle one to
 *     come and steal, sends it a "reschedule" IPI.
 * Pinned threads (thread_create_on with a CPU, idle threads, kmain) are
 * never moved.
 *
 * Context switches hold the run-queue lock across the switch itself: the
 * thread that runs next releases it (sched_finish_switch). So no other
 * CPU can touch the outgoing thread until its registers are saved.
 *
 * Tickless: there is no periodic timer interrupt. Time comes from the
 * clock (clock_now_ns). Whenever a CPU's scheduler decides something, it
 * sets ONE alarm on that CPU for the earliest moment it next has work:
 * the end of the running thread's slice (only if another thread is
 * waiting) or the next sleeper's wake-up. With nothing to do, no alarm is
 * set and the idle CPU is not woken at all.
 *
 * A "tick" (SCHED_TICK_NS = 10 ms) remains the unit of the tick-based
 * calls (sched_sleep, sched_ticks, cpu_ticks); it is just a measure of
 * time, not an interrupt.
 *
 * Limits: no priorities; stealing takes one thread at a time; only the
 * boot CPU's idle thread reaps exited threads. */

#include <stdint.h>
#include <stdbool.h>
#include "proc.h"

#define SCHED_TIMER_HZ       100                 /* ticks per second */
#define SCHED_TICK_NS        (1000000000ULL / SCHED_TIMER_HZ)
#define SCHED_QUANTUM_TICKS  2                   /* 20 ms time slice */
#define SCHED_QUANTUM_NS     (SCHED_QUANTUM_TICKS * SCHED_TICK_NS)

struct runqueue;

struct sched_stats {
    uint64_t ticks;            /* ticks of time since sched_start */
    uint64_t invocations;      /* scheduler entries: timer events + yield/
                                  sleep/exit, whether or not they switched */
    uint64_t switches;         /* actual context switches */
    uint64_t preemptions;      /* switches forced by an expired slice */
    uint64_t voluntary;        /* switches from yield/sleep/exit */
    uint64_t idle_ticks;       /* ticks of time spent in idle threads */
    uint64_t idle_switches;    /* times an idle thread was switched to */
    uint64_t timer_events;     /* alarm interrupts handled */
    uint64_t idle_timer_events;/* ... of which arrived while idle */
    uint64_t alarms_armed;     /* alarms set by the scheduler */
    uint64_t steals;           /* threads taken from another CPU's queue */
    uint64_t ipis;             /* reschedule requests handled */
    unsigned runnable;         /* READY threads in the queue(s) */
    unsigned sleeping;         /* threads on the sleep list(s) */
    tid_t current;             /* running thread */
};

/* Create the boot CPU's idle thread. Requires proc_init(). Does not
 * enable preemption yet. */
void sched_init(void);

/* Enable preemption. Requires the clock (clock_init with
 * sched_timer_event as its alarm handler). */
void sched_start(void);
bool sched_active(void);

/* Called from the alarm interrupt (interrupts off, EOI already sent). */
void sched_timer_event(void);

/* Called from the reschedule IPI (interrupts off, EOI already sent). */
void sched_ipi(void);

/* Give up the CPU to the next READY thread, if any (stays READY). */
void sched_yield(void);

/* Block the calling thread for at least `ticks` ticks (0 counts as 1). */
void sched_sleep(uint64_t ticks);

/* Block the calling thread for at least `ns` nanoseconds. */
void sched_sleep_ns(uint64_t ns);

/* ---- used by proc.c ---------------------------------------------------- */

/* Hook: proc.c reports every thread state change (t's run-queue lock
 * held). */
void sched_state_changed(struct thread *t, task_state_t from, task_state_t to);

/* Lock the run queue that thread `t` belongs to (or this CPU's). Needs
 * interrupts off. */
struct runqueue *sched_lock_thread(struct thread *t);
struct runqueue *sched_lock_this_cpu(void);
void sched_unlock(struct runqueue *rq);

/* Switch away from the calling thread, which has just marked itself
 * BLOCKED or TERMINATED (or was killed). `rq` is this CPU's run queue,
 * locked; the lock is released by the switch. */
void sched_reschedule_locked(struct runqueue *rq);

/* Second half of every context switch, run by the thread switched to:
 * releases this CPU's run-queue lock. */
void sched_finish_switch(void);

/* CPU for a new thread that may run anywhere. */
unsigned sched_pick_cpu(void);

/* ---- multi-core ------------------------------------------------------- */

/* Register `idle` (already RUNNING on that CPU) as CPU `cpu`'s idle
 * thread. */
void sched_register_idle(unsigned cpu, struct thread *idle);

/* An application processor (AP) enters its idle loop. Never returns. */
void sched_ap_main(void) __attribute__((noreturn));

/* Turn work stealing and spreading on or off; returns the old setting.
 * Off, every thread stays on the CPU it was created on. */
bool sched_set_stealing(bool on);
bool sched_stealing(void);

/* ---- introspection ---------------------------------------------------- */

/* This CPU's idle thread. */
struct thread *sched_idle_thread(void);
uint64_t sched_ticks(void);
/* All CPUs added up (`current` is this CPU's thread). */
void sched_get_stats(struct sched_stats *out);
/* One CPU's numbers. */
void sched_get_cpu_stats(unsigned cpu, struct sched_stats *out);
void sched_print_stats(void);

/* Verify queue invariants on every CPU: only READY threads queued, each
 * READY thread on its own CPU's queue exactly once, counts consistent.
 * Returns true if consistent. */
bool sched_check(void);

#endif /* BHARAT_SCHED_H */
