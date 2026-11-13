#include "sched.h"
#include "clock.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "trace.h"

/* All scheduler state is touched only with interrupts disabled: from the
 * alarm interrupt (IF=0 by the interrupt gate) or inside IRQ_GUARD
 * sections. On a single CPU that makes every operation here atomic. */

/* Deadlines this close count as reached: the alarm is set in whole timer
 * counts and may fire a few microseconds early. */
#define SCHED_SLACK_NS 50000ULL
#define NO_DEADLINE    (~0ULL)

struct sched_list {
    struct thread *head;
    struct thread *tail;
    unsigned count;
};

static struct sched_list ready_q;
static struct sched_list sleep_q;
static struct thread *idle;
static bool active;
static struct sched_stats stats;
static uint64_t start_ns;          /* clock time at sched_start */
static uint64_t slice_end;         /* when the running thread's slice ends */
static uint64_t last_account;      /* last time the running thread was charged */
static uint64_t idle_ns;
static uint64_t armed_at = NO_DEADLINE;   /* pending alarm, if any */

/* ---- intrusive lists ---------------------------------------------------- */

static void list_push_tail(struct sched_list *l, struct thread *t, uint8_t which)
{
    t->sched_list = which;
    t->sched_next = NULL;
    t->sched_prev = l->tail;
    if (l->tail != NULL)
        l->tail->sched_next = t;
    else
        l->head = t;
    l->tail = t;
    l->count++;
}

static void list_remove(struct sched_list *l, struct thread *t)
{
    if (t->sched_prev != NULL)
        t->sched_prev->sched_next = t->sched_next;
    else
        l->head = t->sched_next;
    if (t->sched_next != NULL)
        t->sched_next->sched_prev = t->sched_prev;
    else
        l->tail = t->sched_prev;
    t->sched_prev = t->sched_next = NULL;
    t->sched_list = SCHED_LIST_NONE;
    l->count--;
}

/* ---- time --------------------------------------------------------------- */

/* Charge the running thread for the time since it was last charged. */
static void account(struct thread *cur, uint64_t now)
{
    uint64_t d = now - last_account;
    last_account = now;
    cur->cpu_ns += d;
    cur->cpu_ticks = cur->cpu_ns / SCHED_TICK_NS;
    if (cur == idle)
        idle_ns += d;
}

/* Set the one alarm for when `run` (about to run, or running) next needs
 * the scheduler: its slice end if others wait for the CPU (at once if
 * `run` is idle), or the earliest sleeper's wake-up. No work, no alarm. */
static void arm_for(const struct thread *run, bool others_ready, uint64_t now)
{
    if (!active)
        return;
    uint64_t deadline = NO_DEADLINE;
    if (others_ready)
        deadline = run == idle ? now : slice_end;
    for (struct thread *t = sleep_q.head; t != NULL; t = t->sched_next)
        if (t->wake_ns < deadline)
            deadline = t->wake_ns;

    if (deadline == armed_at)
        return;
    armed_at = deadline;
    if (deadline == NO_DEADLINE) {
        clock_cancel_alarm();
        return;
    }
    stats.alarms_armed++;
    clock_set_alarm(deadline);
}

void sched_state_changed(struct thread *t, task_state_t from, task_state_t to)
{
    /* Leaving whatever list the thread was on. */
    if (t->sched_list == SCHED_LIST_READY && to != TASK_READY)
        list_remove(&ready_q, t);
    else if (t->sched_list == SCHED_LIST_SLEEP && from == TASK_BLOCKED)
        list_remove(&sleep_q, t);

    /* READY threads join the tail of the FIFO: that is round-robin. The idle
     * thread is never queued; it is the fallback when the queue is empty. */
    if (to == TASK_READY && t != idle && t->sched_list == SCHED_LIST_NONE)
        list_push_tail(&ready_q, t, SCHED_LIST_READY);

    /* A new or woken thread now waits for the CPU: the running thread may
     * have had no slice alarm (it was alone), so set one. Switches
     * (RUNNING -> READY) are handled by schedule() itself. */
    if (active && to == TASK_READY && from != TASK_RUNNING && t != idle)
        arm_for(thread_current(), true, clock_now_ns());
}

/* ---- core --------------------------------------------------------------- */

/* Pick the next thread and switch to it. Interrupts must be off.
 *
 *   - a RUNNING caller that is preempted/yielding goes to the queue tail
 *     (thread_switch makes it READY, which enqueues it);
 *   - a BLOCKED/TERMINATED caller is on no queue and is simply left;
 *   - with an empty queue a RUNNING caller keeps the CPU, anything else
 *     falls back to the idle thread. */
static void schedule(bool preempt)
{
    uint64_t now = clock_now_ns();
    struct thread *cur = thread_current();
    struct thread *next = ready_q.head;
    account(cur, now);

    if (next == NULL) {
        if (cur->state == TASK_RUNNING) {     /* nobody else wants the CPU */
            arm_for(cur, false, now);
            return;
        }
        next = idle;
    }
    if (next == cur) {
        arm_for(cur, ready_q.count > 1, now);
        return;
    }
    if (next->state != TASK_READY) {
        kprintf("[SCHED] FATAL: selected TID %u in state %s\n",
                next->tid, task_state_str(next->state));
        panic("scheduler selected a non-READY thread");
    }

    stats.switches++;
    if (preempt) {
        stats.preemptions++;
        cur->preempted_count++;
    } else {
        stats.voluntary++;
    }
    if (next == idle)
        stats.idle_switches++;
    trace_event(preempt ? TRACE_SCHED_PREEMPT : TRACE_SCHED_SWITCH, cur->tid, next->tid);

    /* After the switch, others wait if more than `next` is queued, or if
     * `cur` goes back to the queue. */
    bool others = (next == idle ? ready_q.count : ready_q.count - 1) > 0
                  || (cur->state == TASK_RUNNING && cur != idle);
    slice_end = now + SCHED_QUANTUM_NS;
    arm_for(next, others, now);

    /* Validates both contexts, updates states (which requeues `cur` if it
     * was RUNNING), switches CR3 if needed, then context_switch. Returns
     * here when `cur` is scheduled again. */
    if (thread_switch(next) != PROC_OK)
        panic("scheduler: thread_switch refused a READY thread");
}

static void wake_sleepers(uint64_t now)
{
    struct thread *t = sleep_q.head;
    while (t != NULL) {
        struct thread *n = t->sched_next;
        if (t->wake_ns <= now + SCHED_SLACK_NS)
            thread_set_state(t, TASK_READY);   /* hook moves it to ready_q */
        t = n;
    }
}

void sched_timer_event(void)
{
    if (!active)
        return;
    struct thread *cur = thread_current();
    stats.timer_events++;
    stats.invocations++;
    if (cur == idle)
        stats.idle_timer_events++;
    armed_at = NO_DEADLINE;                /* the one-shot alarm has fired */

    uint64_t now = clock_now_ns();
    wake_sleepers(now);

    /* Idle gives way the moment real work exists; others when their slice
     * is over. */
    if (ready_q.head != NULL && (cur == idle || now + SCHED_SLACK_NS >= slice_end))
        schedule(true);
    else
        arm_for(cur, ready_q.head != NULL, now);
}

void sched_yield(void)
{
    IRQ_GUARD();
    if (active) {
        stats.invocations++;
        schedule(false);
    }
}

void sched_sleep_ns(uint64_t ns)
{
    IRQ_GUARD();
    if (!active)
        panic("sched_sleep before the scheduler started");
    struct thread *cur = thread_current();
    cur->wake_ns = clock_now_ns() + (ns ? ns : 1);
    thread_set_state(cur, TASK_BLOCKED);
    list_push_tail(&sleep_q, cur, SCHED_LIST_SLEEP);
    stats.invocations++;
    schedule(false);
}

void sched_sleep(uint64_t ticks)
{
    sched_sleep_ns((ticks ? ticks : 1) * SCHED_TICK_NS);
}

void sched_reschedule(void)
{
    IRQ_GUARD();
    stats.invocations++;
    schedule(false);
}

/* ---- idle --------------------------------------------------------------- */

/* Runs only when no other thread is READY. Reclaims exited threads (their
 * stacks are certainly no longer in use once we are running here), then
 * halts until the next interrupt. With no alarm set, that may be a long
 * time: tickless idle. */
static int64_t idle_entry(void *arg)
{
    (void)arg;
    for (;;) {
        proc_reap();
        __asm__ volatile("sti; hlt");
    }
}

/* ---- setup -------------------------------------------------------------- */

void sched_init(void)
{
    proc_status_t err;
    idle = thread_create(process_kernel(), "idle", idle_entry, NULL, &err);
    if (idle == NULL)
        panic("sched: cannot create idle thread");

    /* thread_create queued it as READY; idle lives outside the queue. */
    IRQ_GUARD();
    if (idle->sched_list == SCHED_LIST_READY)
        list_remove(&ready_q, idle);

    kprintf("[sched] Round-robin scheduler, tickless: slice %d ms, "
            "idle TID %u; boot thread TID %u registered as current\n",
            (int)(SCHED_QUANTUM_NS / 1000000), idle->tid, thread_current()->tid);
}

void sched_start(void)
{
    IRQ_GUARD();
    if (!clock_ready())
        panic("sched_start: the clock is not running");
    start_ns = last_account = clock_now_ns();
    slice_end = start_ns + SCHED_QUANTUM_NS;
    active = true;
    arm_for(thread_current(), ready_q.head != NULL, start_ns);
    kprintf("[sched] Preemption enabled (%u threads READY).\n", ready_q.count);
}

bool sched_active(void)
{
    return active;
}

/* ---- introspection ------------------------------------------------------ */

struct thread *sched_idle_thread(void)
{
    return idle;
}

uint64_t sched_ticks(void)
{
    return active ? (clock_now_ns() - start_ns) / SCHED_TICK_NS : 0;
}

void sched_get_stats(struct sched_stats *out)
{
    IRQ_GUARD();
    *out = stats;
    out->ticks = sched_ticks();
    out->idle_ticks = idle_ns / SCHED_TICK_NS;
    out->runnable = ready_q.count;
    out->sleeping = sleep_q.count;
    out->current = thread_current()->tid;
}

void sched_print_stats(void)
{
    struct sched_stats s;
    sched_get_stats(&s);
    kprintf("BHARAT SCHEDULER\n");
    kprintf("  Policy            : ROUND-ROBIN, quantum %d ticks, tickless\n", SCHED_QUANTUM_TICKS);
    kprintf("  Elapsed ticks     : %lu\n", s.ticks);
    kprintf("  Timer events      : %lu (%lu while idle), %lu alarms set\n",
            s.timer_events, s.idle_timer_events, s.alarms_armed);
    kprintf("  Scheduler calls   : %lu\n", s.invocations);
    kprintf("  Context switches  : %lu (%lu preemptive, %lu voluntary)\n",
            s.switches, s.preemptions, s.voluntary);
    kprintf("  Runnable threads  : %u\n", s.runnable);
    kprintf("  Sleeping threads  : %u\n", s.sleeping);
    kprintf("  Idle ticks        : %lu\n", s.idle_ticks);
    kprintf("  Current thread    : TID %u\n", s.current);
}

bool sched_check(void)
{
    IRQ_GUARD();
    unsigned n = 0;
    struct thread *prev = NULL;
    for (struct thread *t = ready_q.head; t != NULL; t = t->sched_next) {
        if (t->state != TASK_READY || t->sched_list != SCHED_LIST_READY
            || t->sched_prev != prev || t == idle || t == thread_current()) {
            kprintf("[sched] check: bad ready-queue entry TID %u (%s)\n",
                    t->tid, task_state_str(t->state));
            return false;
        }
        prev = t;
        n++;
    }
    if (n != ready_q.count || prev != ready_q.tail)
        return false;

    n = 0;
    for (struct thread *t = sleep_q.head; t != NULL; t = t->sched_next, n++) {
        if (t->state != TASK_BLOCKED || t->sched_list != SCHED_LIST_SLEEP)
            return false;
    }
    if (n != sleep_q.count)
        return false;

    /* Every READY thread (other than idle) must be queued exactly once. */
    for (unsigned i = 0; i < THREAD_MAX; i++) {
        struct thread *t = thread_at(i);
        if (t != NULL && t != idle && t->state == TASK_READY && t->sched_list != SCHED_LIST_READY) {
            kprintf("[sched] check: READY TID %u missing from the queue\n", t->tid);
            return false;
        }
    }
    return thread_current()->state == TASK_RUNNING;
}
