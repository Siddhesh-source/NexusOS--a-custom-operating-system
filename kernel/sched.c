#include "sched.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"

/* All scheduler state is touched only with interrupts disabled: from the
 * timer IRQ (IF=0 by the interrupt gate) or inside IRQ_GUARD sections. On a
 * single CPU that makes every operation here atomic. */

struct sched_list {
    struct thread *head;
    struct thread *tail;
    unsigned count;
};

static struct sched_list ready_q;
static struct sched_list sleep_q;
static struct thread *idle;
static bool active;
static unsigned slice_used;        /* ticks the current thread has run */
static struct sched_stats stats;
static unsigned trace_left;

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
}

/* ---- core --------------------------------------------------------------- */

static void trace_switch(const struct thread *from, const struct thread *to, const char *why)
{
    if (trace_left == 0)
        return;
    trace_left--;
    kprintf("[SCHED] tick=%lu current=TID %u (%s) next=TID %u (%s) reason=%s switch_count=%lu\n",
            stats.ticks, from->tid, task_state_str(from->state), to->tid, to->name,
            why, stats.switches);
}

/* Pick the next thread and switch to it. Interrupts must be off.
 *
 *   - a RUNNING caller that is preempted/yielding goes to the queue tail
 *     (thread_switch makes it READY, which enqueues it);
 *   - a BLOCKED/TERMINATED caller is on no queue and is simply left;
 *   - with an empty queue a RUNNING caller keeps the CPU, anything else
 *     falls back to the idle thread. */
static void schedule(const char *why, bool preempt)
{
    struct thread *cur = thread_current();
    struct thread *next = ready_q.head;
    if (next == NULL) {
        if (cur->state == TASK_RUNNING)
            return;                 /* nobody else wants the CPU */
        next = idle;
    }
    if (next == cur)
        return;
    if (next->state != TASK_READY) {
        kprintf("[SCHED] FATAL: selected TID %u in state %s\n",
                next->tid, task_state_str(next->state));
        panic("scheduler selected a non-READY thread");
    }

    slice_used = 0;
    stats.switches++;
    if (preempt) {
        stats.preemptions++;
        cur->preempted_count++;
    } else {
        stats.voluntary++;
    }
    if (next == idle)
        stats.idle_switches++;
    trace_switch(cur, next, why);

    /* Phase 4 primitive: validates both contexts, updates states (which
     * requeues `cur` if it was RUNNING), switches CR3 if needed, then
     * context_switch. Returns here when `cur` is scheduled again. */
    if (thread_switch(next) != PROC_OK)
        panic("scheduler: thread_switch refused a READY thread");
}

static void wake_sleepers(void)
{
    struct thread *t = sleep_q.head;
    while (t != NULL) {
        struct thread *n = t->sched_next;
        if (stats.ticks >= t->wake_tick)
            thread_set_state(t, TASK_READY);   /* hook moves it to ready_q */
        t = n;
    }
}

void sched_tick(void)
{
    if (!active)
        return;
    stats.ticks++;
    stats.invocations++;           /* every tick is a scheduling opportunity */

    struct thread *cur = thread_current();
    cur->cpu_ticks++;
    if (cur == idle)
        stats.idle_ticks++;

    wake_sleepers();

    /* Idle yields the moment real work exists; others after a quantum. */
    if (cur == idle) {
        if (ready_q.head != NULL)
            schedule("work available", true);
        return;
    }
    if (++slice_used >= SCHED_QUANTUM_TICKS && ready_q.head != NULL)
        schedule("quantum expired", true);
}

void sched_yield(void)
{
    IRQ_GUARD();
    if (active) {
        stats.invocations++;
        schedule("yield", false);
    }
}

void sched_sleep(uint64_t ticks)
{
    IRQ_GUARD();
    if (!active)
        panic("sched_sleep before the scheduler started");
    struct thread *cur = thread_current();
    cur->wake_tick = stats.ticks + (ticks ? ticks : 1);
    thread_set_state(cur, TASK_BLOCKED);
    list_push_tail(&sleep_q, cur, SCHED_LIST_SLEEP);
    stats.invocations++;
    schedule("sleep", false);
}

void sched_reschedule(void)
{
    IRQ_GUARD();
    stats.invocations++;
    schedule(thread_current()->state == TASK_TERMINATED ? "exit" : "block", false);
}

/* ---- idle --------------------------------------------------------------- */

/* Runs only when no other thread is READY. Reclaims exited threads (their
 * stacks are certainly no longer in use once we are running here), then
 * halts until the next interrupt. */
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

    kprintf("[sched] Round-robin scheduler: %d Hz timer, quantum %d ticks (%d ms), "
            "idle TID %u; boot thread TID %u registered as current\n",
            SCHED_TIMER_HZ, SCHED_QUANTUM_TICKS, SCHED_QUANTUM_TICKS * 1000 / SCHED_TIMER_HZ,
            idle->tid, thread_current()->tid);
}

void sched_start(void)
{
    IRQ_GUARD();
    slice_used = 0;
    active = true;
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
    return stats.ticks;
}

void sched_get_stats(struct sched_stats *out)
{
    IRQ_GUARD();
    *out = stats;
    out->runnable = ready_q.count;
    out->sleeping = sleep_q.count;
    out->current = thread_current()->tid;
}

void sched_print_stats(void)
{
    struct sched_stats s;
    sched_get_stats(&s);
    kprintf("NEXUS SCHEDULER\n");
    kprintf("  Policy            : ROUND-ROBIN, quantum %d ticks\n", SCHED_QUANTUM_TICKS);
    kprintf("  Timer ticks       : %lu\n", s.ticks);
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

void sched_set_trace(unsigned lines)
{
    IRQ_GUARD();
    trace_left = lines;
}
