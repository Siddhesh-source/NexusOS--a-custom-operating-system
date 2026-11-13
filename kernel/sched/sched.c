#include "sched.h"
#include "clock.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "trace.h"
#include "percpu.h"
#include "spinlock.h"
#include "ipi.h"

/* Locking: a run queue's lists, timing fields and stats, and the state of
 * every thread belonging to that CPU, change only under rq->lock (with
 * interrupts off). The alarm fields (armed_at, slice_end) are used only
 * by the owning CPU, because the alarm is that CPU's own APIC timer. */

/* Deadlines this close count as reached: the alarm is set in whole timer
 * counts and may fire a few microseconds early. */
#define SCHED_SLACK_NS 50000ULL
#define NO_DEADLINE    (~0ULL)
#define STEAL_TRIES    64

struct sched_list {
    struct thread *head;
    struct thread *tail;
    unsigned count;
};

struct runqueue {
    struct spinlock lock;
    struct sched_list ready;
    struct sched_list sleep;
    struct thread *idle;
    uint64_t slice_end;         /* when the running thread's slice ends */
    uint64_t last_account;      /* last time the running thread was charged */
    uint64_t idle_ns;
    uint64_t armed_at;          /* pending alarm, if any */
    bool waking;                /* timer event is waking sleepers */
    struct sched_stats stats;
} __attribute__((aligned(64)));  /* one cache line apart: no false sharing */

static struct runqueue rqs[SMP_MAX_CPUS] = {
    [0 ... SMP_MAX_CPUS - 1] = {
        .lock = SPINLOCK_INIT(LOCK_CLASS_RUNQUEUE),
        .armed_at = NO_DEADLINE,
    },
};
static volatile bool active;
static volatile bool stealing;
static uint64_t start_ns;
static volatile uint32_t idle_mask;     /* CPUs running their idle thread */

static inline struct runqueue *my_rq(void)
{
    return &rqs[cpu_id()];
}

static inline bool is_idle(const struct thread *t)
{
    return (t->flags & THREAD_IDLE) != 0;
}

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
    __atomic_store_n(&l->count, l->count + 1, __ATOMIC_RELAXED);
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
    __atomic_store_n(&l->count, l->count - 1, __ATOMIC_RELAXED);
}

/* ---- locking ------------------------------------------------------------ */

struct runqueue *sched_lock_thread(struct thread *t)
{
    /* t->cpu can change (stealing) until we hold the right lock. */
    for (;;) {
        unsigned c = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
        struct runqueue *rq = &rqs[c];
        spin_lock(&rq->lock);
        if (__atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE) == c)
            return rq;
        spin_unlock(&rq->lock);
    }
}

struct runqueue *sched_lock_this_cpu(void)
{
    struct runqueue *rq = my_rq();
    spin_lock(&rq->lock);
    return rq;
}

void sched_unlock(struct runqueue *rq)
{
    spin_unlock(&rq->lock);
}

/* ---- time --------------------------------------------------------------- */

/* Charge the running thread for the time since it was last charged. */
static void account(struct runqueue *rq, struct thread *cur, uint64_t now)
{
    uint64_t d = now - rq->last_account;
    rq->last_account = now;
    cur->cpu_ns += d;
    cur->cpu_ticks = cur->cpu_ns / SCHED_TICK_NS;
    if (cur == rq->idle)
        rq->idle_ns += d;
}

/* Set this CPU's one alarm for when `run` (about to run, or running) next
 * needs the scheduler: its slice end if others wait for the CPU (at once
 * if `run` is idle), or the earliest sleeper's wake-up. No work, no
 * alarm. `rq` must be this CPU's: the alarm is this CPU's own timer. */
static void arm_for(struct runqueue *rq, const struct thread *run, bool others_ready, uint64_t now)
{
    if (!active)
        return;
    uint64_t deadline = NO_DEADLINE;
    if (others_ready)
        deadline = run == rq->idle ? now : rq->slice_end;
    for (struct thread *t = rq->sleep.head; t != NULL; t = t->sched_next)
        if (t->wake_ns < deadline)
            deadline = t->wake_ns;

    /* Lazy re-arming: programming the timer is a device-register write,
     * slow next to a thread switch. Only reprogram when the new deadline
     * is EARLIER than the pending alarm. A pending alarm that is earlier
     * than needed (or no longer needed) just fires, and the scheduler
     * looks again: at most one extra interrupt, instead of a register
     * write on every switch. */
    if (deadline >= rq->armed_at)
        return;
    rq->armed_at = deadline;
    rq->stats.alarms_armed++;
    clock_set_alarm(deadline);
}

/* An idle CPU's steal can fail only because the other queue was locked
 * at that moment (it only try-locks). Look again soon rather than sleep
 * until someone happens to send an IPI. */
#define STEAL_RETRY_NS 1000000ULL

static void arm_retry(struct runqueue *rq, uint64_t now)
{
    uint64_t deadline = now + STEAL_RETRY_NS;
    if (deadline >= rq->armed_at)
        return;
    rq->armed_at = deadline;
    rq->stats.alarms_armed++;
    clock_set_alarm(deadline);
}

/* ---- spreading work ----------------------------------------------------- */

/* Wake up to `count` idle CPUs (never `except` or us) to come and steal.
 * Where the search starts rotates, so every idle CPU gets its turn;
 * starting from a fixed place would keep waking the same few and leave
 * the others asleep (they are tickless: nothing else wakes them). */
static volatile uint32_t kick_rotor;

static void kick_idle_cpus(unsigned except, unsigned count)
{
    uint32_t idle = __atomic_load_n(&idle_mask, __ATOMIC_RELAXED) & smp_online_mask()
                    & ~(1u << except) & ~(1u << cpu_id());
    unsigned start = __atomic_fetch_add(&kick_rotor, 1, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < SMP_MAX_CPUS && count > 0 && idle != 0; i++) {
        unsigned c = (start + i) % SMP_MAX_CPUS;
        if (idle & (1u << c)) {
            ipi_reschedule(c);
            idle &= ~(1u << c);
            count--;
        }
    }
}

static void kick_idle_cpu(unsigned except)
{
    kick_idle_cpus(except, 1);
}

/* Threads still wait in our queue after a decision: wake one idle CPU
 * per waiting thread to come and take it. Repeats at every decision, at
 * most once per slice. */
static void balance(struct runqueue *rq)
{
    if (stealing)
        kick_idle_cpus(cpu_id(), __atomic_load_n(&rq->ready.count, __ATOMIC_RELAXED));
}

unsigned sched_pick_cpu(void)
{
    unsigned me = cpu_id();
    if (!stealing)
        return me;
    unsigned best = me, best_load = ~0u;
    uint32_t online = smp_online_mask();
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        if (!(online & (1u << c)))
            continue;
        /* Queue length, plus one if the CPU is busy with a real thread. */
        unsigned load = __atomic_load_n(&rqs[c].ready.count, __ATOMIC_RELAXED)
                        + !(__atomic_load_n(&idle_mask, __ATOMIC_RELAXED) & (1u << c));
        if (load < best_load) {
            best = c;
            best_load = load;
        }
    }
    return best;
}

/* Take the oldest unpinned READY thread from another CPU's queue and put
 * it on ours. `me` is locked; the victim is only try-locked, so two CPUs
 * stealing from each other can never wait on each other forever. */
static struct thread *steal(struct runqueue *me)
{
    unsigned self = cpu_id();
    uint32_t online = smp_online_mask();
    for (unsigned i = 1; i < SMP_MAX_CPUS; i++) {
        unsigned c = (self + i) % SMP_MAX_CPUS;
        struct runqueue *v = &rqs[c];
        if (!(online & (1u << c)) || __atomic_load_n(&v->ready.count, __ATOMIC_RELAXED) == 0)
            continue;
        bool locked = false;
        for (int n = 0; n < STEAL_TRIES && !(locked = spin_trylock(&v->lock)); n++)
            cpu_relax();
        if (!locked)
            continue;
        struct thread *t = v->ready.head;
        while (t != NULL && t->pinned)
            t = t->sched_next;
        if (t != NULL) {
            list_remove(&v->ready, t);
            __atomic_store_n(&t->cpu, self, __ATOMIC_RELEASE);
            list_push_tail(&me->ready, t, SCHED_LIST_READY);
            me->stats.steals++;
        }
        spin_unlock(&v->lock);
        if (t != NULL) {
            trace_event(TRACE_SCHED_STEAL, t->tid, c);
            return t;
        }
    }
    return NULL;
}

/* Could an idle CPU find work somewhere else? (A hint, read unlocked.) */
static bool work_elsewhere(void)
{
    if (!stealing)
        return false;
    uint32_t online = smp_online_mask();
    unsigned self = cpu_id();
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++)
        if (c != self && (online & (1u << c))
            && __atomic_load_n(&rqs[c].ready.count, __ATOMIC_RELAXED) > 0)
            return true;
    return false;
}

/* ---- state changes ------------------------------------------------------ */

void sched_state_changed(struct thread *t, task_state_t from, task_state_t to)
{
    struct runqueue *rq = &rqs[t->cpu];

    /* Leaving whatever list the thread was on. */
    if (t->sched_list == SCHED_LIST_READY && to != TASK_READY)
        list_remove(&rq->ready, t);
    else if (t->sched_list == SCHED_LIST_SLEEP && from == TASK_BLOCKED)
        list_remove(&rq->sleep, t);

    /* READY threads join the tail of the FIFO: that is round-robin. The
     * idle thread is never queued; it is the fallback when the queue is
     * empty. A thread still running on its CPU (it made itself READY) is
     * queued when it is switched out. */
    if (to == TASK_READY && !is_idle(t) && t->sched_list == SCHED_LIST_NONE
        && cpu_at(t->cpu)->current != t)
        list_push_tail(&rq->ready, t, SCHED_LIST_READY);

    /* A new or woken thread now waits for a CPU. If it is ours, the
     * running thread may have had no slice alarm (it was alone), so set
     * one. If it is another CPU's, tell that CPU. Switches (RUNNING ->
     * READY) are handled by schedule() itself. */
    if (active && to == TASK_READY && from != TASK_RUNNING && !is_idle(t)) {
        if (t->cpu == cpu_id()) {
            if (!rq->waking)
                arm_for(rq, percpu_current(), true, clock_now_ns());
        } else {
            ipi_reschedule(t->cpu);
        }
        if (stealing && !t->pinned)
            kick_idle_cpu(t->cpu);
    }
}

/* ---- core --------------------------------------------------------------- */

/* Pick the next thread and switch to it. `rq` is this CPU's run queue,
 * locked, with interrupts off; the lock is released before this returns
 * (by the switch, or directly when nothing changes).
 *
 *   - a RUNNING caller that is preempted/yielding goes to the queue tail
 *     (thread_switch_to makes it READY, which enqueues it);
 *   - a BLOCKED/TERMINATED caller is on no queue and is simply left;
 *   - a caller killed by another CPU is TERMINATED here;
 *   - with an empty queue, the CPU tries to steal; failing that, a
 *     RUNNING caller keeps the CPU and anything else falls back to idle. */
static void schedule(struct runqueue *rq, bool preempt)
{
    uint64_t now = clock_now_ns();
    struct thread *cur = percpu_current();
    account(rq, cur, now);

    if (cur->kill && cur->state != TASK_TERMINATED && !is_idle(cur)) {
        cur->exit_code = -1;
        thread_set_state_locked(cur, TASK_TERMINATED);
    }

    struct thread *next = rq->ready.head;
    bool retry = false;
    if (next == NULL && stealing) {
        next = steal(rq);
        retry = next == NULL && work_elsewhere();
    }
    if (next == NULL) {
        if (cur->state == TASK_RUNNING) {     /* nobody else wants the CPU */
            arm_for(rq, cur, false, now);
            if (retry && cur == rq->idle)
                arm_retry(rq, now);
            spin_unlock(&rq->lock);
            return;
        }
        next = rq->idle;
    }
    if (next == cur) {
        arm_for(rq, cur, rq->ready.count > 1, now);
        spin_unlock(&rq->lock);
        return;
    }
    if (next->state != TASK_READY) {
        kprintf("[SCHED] FATAL: CPU %u selected TID %u in state %s\n",
                cpu_id(), next->tid, task_state_str(next->state));
        panic("scheduler selected a non-READY thread");
    }

    rq->stats.switches++;
    if (preempt) {
        rq->stats.preemptions++;
        cur->preempted_count++;
    } else {
        rq->stats.voluntary++;
    }
    if (next == rq->idle)
        rq->stats.idle_switches++;
    trace_event(preempt ? TRACE_SCHED_PREEMPT : TRACE_SCHED_SWITCH, cur->tid, next->tid);

    /* After the switch, others wait if more than `next` is queued, or if
     * `cur` goes back to the queue. */
    bool others = (next == rq->idle ? rq->ready.count : rq->ready.count - 1) > 0
                  || (cur->state == TASK_RUNNING && cur != rq->idle);
    rq->slice_end = now + SCHED_QUANTUM_NS;
    arm_for(rq, next, others, now);
    if (retry && next == rq->idle)
        arm_retry(rq, now);
    balance(rq);

    /* Validates both contexts, updates states (which requeues `cur` if it
     * was RUNNING), switches CR3 if needed, then context_switch. Returns
     * here when `cur` is scheduled again (maybe on another CPU), with the
     * lock already released by sched_finish_switch. */
    thread_switch_to(next);
}

void sched_finish_switch(void)
{
    struct runqueue *rq = my_rq();
    uint32_t bit = 1u << cpu_id();
    if (percpu_current() == rq->idle)
        __atomic_fetch_or(&idle_mask, bit, __ATOMIC_RELAXED);
    else
        __atomic_fetch_and(&idle_mask, ~bit, __ATOMIC_RELAXED);
    spin_unlock(&rq->lock);
}

static void wake_sleepers(struct runqueue *rq, uint64_t now)
{
    struct thread *t = rq->sleep.head;
    while (t != NULL) {
        struct thread *n = t->sched_next;
        if (t->wake_ns <= now + SCHED_SLACK_NS)
            thread_set_state_locked(t, TASK_READY);   /* hook moves it to ready */
        t = n;
    }
}

void sched_timer_event(void)
{
    if (!active)
        return;
    struct runqueue *rq = my_rq();
    spin_lock(&rq->lock);
    struct thread *cur = percpu_current();
    rq->stats.timer_events++;
    rq->stats.invocations++;
    if (cur == rq->idle)
        rq->stats.idle_timer_events++;
    rq->armed_at = NO_DEADLINE;                /* the one-shot alarm has fired */

    uint64_t now = clock_now_ns();
    /* The decision below sets the alarm; waking must not set its own
     * (it would fire again for nothing right after the switch). */
    rq->waking = true;
    wake_sleepers(rq, now);
    rq->waking = false;

    /* Idle gives way the moment real work exists; others when their slice
     * is over, or at once if another CPU killed them. */
    if (cur->kill
        || (rq->ready.head != NULL && (cur == rq->idle || now + SCHED_SLACK_NS >= rq->slice_end))
        || (cur == rq->idle && work_elsewhere())) {
        schedule(rq, true);
    } else {
        arm_for(rq, cur, rq->ready.head != NULL, now);
        spin_unlock(&rq->lock);
    }
}

void sched_ipi(void)
{
    if (!active)
        return;
    struct runqueue *rq = my_rq();
    spin_lock(&rq->lock);
    struct thread *cur = percpu_current();
    rq->stats.ipis++;
    rq->stats.invocations++;
    if (cur->kill || (cur == rq->idle && (rq->ready.head != NULL || work_elsewhere()))) {
        schedule(rq, cur != rq->idle);
    } else {
        /* Work was queued behind a running thread: make sure its slice
         * ends on time. */
        if (rq->ready.head != NULL)
            arm_for(rq, cur, true, clock_now_ns());
        spin_unlock(&rq->lock);
    }
}

static void no_locks_held(const char *what)
{
    if (spin_held_count() != 0) {
        kprintf("[sched] %s while holding %u spinlock(s)\n", what, spin_held_count());
        panic("scheduler: a thread tried to give up the CPU while holding a spinlock");
    }
}

void sched_yield(void)
{
    uint64_t flags = irq_save();
    if (active) {
        no_locks_held("yield");
        struct runqueue *rq = my_rq();
        spin_lock(&rq->lock);
        rq->stats.invocations++;
        schedule(rq, false);
    }
    irq_restore(flags);
}

void sched_sleep_ns(uint64_t ns)
{
    uint64_t flags = irq_save();
    if (!active)
        panic("sched_sleep before the scheduler started");
    no_locks_held("sleep");
    struct runqueue *rq = my_rq();
    spin_lock(&rq->lock);
    struct thread *cur = percpu_current();
    cur->wake_ns = clock_now_ns() + (ns ? ns : 1);
    if (thread_set_state_locked(cur, TASK_BLOCKED) == PROC_OK)
        list_push_tail(&rq->sleep, cur, SCHED_LIST_SLEEP);
    rq->stats.invocations++;
    schedule(rq, false);
    irq_restore(flags);
}

void sched_sleep(uint64_t ticks)
{
    sched_sleep_ns((ticks ? ticks : 1) * SCHED_TICK_NS);
}

void sched_reschedule_locked(struct runqueue *rq)
{
    rq->stats.invocations++;
    schedule(rq, false);
}

/* ---- idle --------------------------------------------------------------- */

/* Runs only when no other thread is READY on this CPU. The boot CPU's
 * idle thread also reclaims exited threads (their stacks are certainly no
 * longer in use once we are running here). Then halt until the next
 * interrupt. With no alarm set, that may be a long time: tickless idle. */
static void __attribute__((noreturn)) idle_loop(void)
{
    for (;;) {
        if (cpu_id() == 0)
            proc_reap();
        __asm__ volatile("cli");
        if (active && (my_rq()->ready.head != NULL || work_elsewhere())) {
            /* Someone queued work without telling us; go and get it. */
            struct runqueue *rq = my_rq();
            spin_lock(&rq->lock);
            schedule(rq, false);
        }
        __asm__ volatile("sti; hlt");
    }
}

static int64_t idle_entry(void *arg)
{
    (void)arg;
    idle_loop();
}

/* ---- setup -------------------------------------------------------------- */

void sched_init(void)
{
    proc_status_t err;
    struct thread *idle = thread_create_on(process_kernel(), "idle", idle_entry, NULL, 0, &err);
    if (idle == NULL)
        panic("sched: cannot create idle thread");

    /* thread_create queued it as READY; idle lives outside the queue. */
    uint64_t flags = irq_save();
    struct runqueue *rq = sched_lock_thread(idle);
    idle->flags |= THREAD_IDLE;
    if (idle->sched_list == SCHED_LIST_READY)
        list_remove(&rq->ready, idle);
    rq->idle = idle;
    this_cpu()->idle = idle;
    sched_unlock(rq);
    irq_restore(flags);

    kprintf("[sched] Round-robin scheduler, tickless, per-CPU queues: slice %d ms, "
            "idle TID %u; boot thread TID %u registered as current\n",
            (int)(SCHED_QUANTUM_NS / 1000000), idle->tid, thread_current()->tid);
}

void sched_start(void)
{
    uint64_t flags = irq_save();
    if (!clock_ready())
        panic("sched_start: the clock is not running");
    struct runqueue *rq = sched_lock_this_cpu();
    start_ns = clock_now_ns();
    rq->last_account = start_ns;
    rq->slice_end = start_ns + SCHED_QUANTUM_NS;
    active = true;
    arm_for(rq, thread_current(), rq->ready.head != NULL, start_ns);
    unsigned n = rq->ready.count;
    sched_unlock(rq);
    irq_restore(flags);
    kprintf("[sched] Preemption enabled (%u threads READY).\n", n);
}

void sched_register_idle(unsigned cpu, struct thread *idle)
{
    rqs[cpu].idle = idle;
}

void sched_ap_main(void)
{
    struct runqueue *rq = my_rq();
    uint64_t flags = irq_save();
    spin_lock(&rq->lock);
    rq->last_account = clock_now_ns();
    rq->slice_end = rq->last_account + SCHED_QUANTUM_NS;
    __atomic_fetch_or(&idle_mask, 1u << cpu_id(), __ATOMIC_RELAXED);
    spin_unlock(&rq->lock);
    irq_restore(flags);
    idle_loop();
}

bool sched_set_stealing(bool on)
{
    bool was = stealing;
    stealing = on;
    if (on && !was) {
        /* Idle CPUs sleep until told otherwise: wake them to look. */
        uint32_t online = smp_online_mask();
        for (unsigned c = 0; c < SMP_MAX_CPUS; c++)
            if ((online & (1u << c)) && c != cpu_id())
                ipi_reschedule(c);
    }
    return was;
}

bool sched_stealing(void)
{
    return stealing;
}

bool sched_active(void)
{
    return active;
}

/* ---- introspection ------------------------------------------------------ */

struct thread *sched_idle_thread(void)
{
    return my_rq()->idle;
}

uint64_t sched_ticks(void)
{
    return active ? (clock_now_ns() - start_ns) / SCHED_TICK_NS : 0;
}

void sched_get_cpu_stats(unsigned cpu, struct sched_stats *out)
{
    struct runqueue *rq = &rqs[cpu < SMP_MAX_CPUS ? cpu : 0];
    uint64_t flags = irq_save();
    spin_lock(&rq->lock);
    *out = rq->stats;
    out->idle_ticks = rq->idle_ns / SCHED_TICK_NS;
    out->runnable = rq->ready.count;
    out->sleeping = rq->sleep.count;
    struct thread *cur = cpu_at(cpu)->current;
    out->current = cur != NULL ? cur->tid : 0;
    spin_unlock(&rq->lock);
    irq_restore(flags);
    out->ticks = sched_ticks();
}

void sched_get_stats(struct sched_stats *out)
{
    struct sched_stats sum = { 0 };
    uint32_t online = smp_online_mask();
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        if (!(online & (1u << c)))
            continue;
        struct sched_stats s;
        sched_get_cpu_stats(c, &s);
        sum.invocations += s.invocations;
        sum.switches += s.switches;
        sum.preemptions += s.preemptions;
        sum.voluntary += s.voluntary;
        sum.idle_ticks += s.idle_ticks;
        sum.idle_switches += s.idle_switches;
        sum.timer_events += s.timer_events;
        sum.idle_timer_events += s.idle_timer_events;
        sum.alarms_armed += s.alarms_armed;
        sum.steals += s.steals;
        sum.ipis += s.ipis;
        sum.runnable += s.runnable;
        sum.sleeping += s.sleeping;
    }
    sum.ticks = sched_ticks();
    sum.current = thread_current()->tid;
    *out = sum;
}

void sched_print_stats(void)
{
    struct sched_stats s;
    sched_get_stats(&s);
    kprintf("BHARAT SCHEDULER\n");
    kprintf("  Policy            : ROUND-ROBIN, quantum %d ticks, tickless, %u CPU(s)\n",
            SCHED_QUANTUM_TICKS, smp_cpu_count());
    kprintf("  Elapsed ticks     : %lu\n", s.ticks);
    kprintf("  Timer events      : %lu (%lu while idle), %lu alarms set\n",
            s.timer_events, s.idle_timer_events, s.alarms_armed);
    kprintf("  Scheduler calls   : %lu\n", s.invocations);
    kprintf("  Context switches  : %lu (%lu preemptive, %lu voluntary)\n",
            s.switches, s.preemptions, s.voluntary);
    kprintf("  Work stolen       : %lu threads\n", s.steals);
    kprintf("  Runnable threads  : %u\n", s.runnable);
    kprintf("  Sleeping threads  : %u\n", s.sleeping);
    kprintf("  Idle ticks        : %lu\n", s.idle_ticks);
    kprintf("  Current thread    : TID %u\n", s.current);
}

static bool check_rq(unsigned c, struct runqueue *rq)
{
    unsigned n = 0;
    struct thread *prev = NULL;
    for (struct thread *t = rq->ready.head; t != NULL; t = t->sched_next) {
        if (t->state != TASK_READY || t->sched_list != SCHED_LIST_READY || t->cpu != c
            || t->sched_prev != prev || t == rq->idle || t == cpu_at(c)->current) {
            kprintf("[sched] check: bad ready-queue entry TID %u (%s) on CPU %u\n",
                    t->tid, task_state_str(t->state), c);
            return false;
        }
        prev = t;
        n++;
    }
    if (n != rq->ready.count || prev != rq->ready.tail)
        return false;

    n = 0;
    for (struct thread *t = rq->sleep.head; t != NULL; t = t->sched_next, n++) {
        if (t->state != TASK_BLOCKED || t->sched_list != SCHED_LIST_SLEEP || t->cpu != c)
            return false;
    }
    return n == rq->sleep.count;
}

bool sched_check(void)
{
    uint64_t flags = irq_save();
    bool ok = true;
    uint32_t online = smp_online_mask();
    for (unsigned c = 0; c < SMP_MAX_CPUS && ok; c++) {
        if (!(online & (1u << c)))
            continue;
        spin_lock(&rqs[c].lock);
        ok = check_rq(c, &rqs[c]);
        spin_unlock(&rqs[c].lock);
    }

    /* Every READY thread (other than idle) must be queued on its CPU.
     * Interrupts are off, so the table entries stay valid (RCU). */
    for (unsigned i = 0; i < THREAD_MAX && ok; i++) {
        struct thread *t = thread_at(i);
        if (t == NULL)
            continue;
        struct runqueue *rq = sched_lock_thread(t);
        if (!is_idle(t) && t->state == TASK_READY && t->sched_list != SCHED_LIST_READY) {
            kprintf("[sched] check: READY TID %u missing from CPU %u's queue\n", t->tid, t->cpu);
            ok = false;
        }
        sched_unlock(rq);
    }
    ok = ok && thread_current()->state == TASK_RUNNING;
    irq_restore(flags);
    return ok;
}
