#include "sched_test.h"
#include "sched.h"
#include "proc.h"
#include "heap.h"
#include "kstack.h"
#include "cpu.h"
#include "box.h"
#include "kprintf.h"

/* sched_spin.asm */
uint64_t preempt_reg_spin(uint64_t seed, volatile uint64_t *stop, volatile uint64_t *iterations);

static unsigned passed, failed;

#define CHECK(cond, desc) check((cond), (desc))

static bool check(bool ok, const char *desc)
{
    if (ok) {
        passed++;
        kprintf("[test]   PASS  %s\n", desc);
    } else {
        failed++;
        kprintf("[test]   FAIL  %s\n", desc);
    }
    return ok;
}

static void section(const char *name)
{
    kprintf("\n[test] --- %s ---\n", name);
}

static struct {
    bool preemption, registers, isolation, rr_order, fairness, termination, idle, sleep;
} result;

/* A thread is "gone" once it has TERMINATED, whether or not the idle
 * thread has reaped it yet. TIDs are never reused, so this is safe to ask
 * about a thread whose structure may already be freed. */
static bool thread_gone(tid_t tid)
{
    IRQ_GUARD();
    struct thread *t = thread_find(tid);
    return t == NULL || t->state == TASK_TERMINATED;
}

static void wait_gone(const tid_t *tids, unsigned n)
{
    for (unsigned tries = 0; tries < 50; tries++) {
        bool all = true;
        for (unsigned i = 0; i < n; i++)
            all &= thread_gone(tids[i]);
        if (all)
            return;
        sched_sleep(1);
    }
}

static struct thread *spawn(struct process *p, const char *name, thread_entry_t fn, void *arg)
{
    proc_status_t err;
    struct thread *t = thread_create(p, name, fn, arg, &err);
    if (t == NULL)
        kprintf("[test]   cannot create %s: %s\n", name, proc_status_str(err));
    return t;
}

/* ---- 1. preemption proof ------------------------------------------------ */

struct spinner {
    volatile uint64_t count;
    volatile uint64_t other_progress_seen;   /* peer advanced mid-loop */
    volatile uint64_t *other;
    volatile uint64_t preempted;             /* copied from the thread */
};

static volatile bool spin_stop;

/* Never yields, sleeps or calls the scheduler: a pure CPU-bound loop. */
static int64_t spinner_entry(void *arg)
{
    struct spinner *s = arg;
    uint64_t last_other = *s->other;
    while (!spin_stop) {
        s->count++;
        uint64_t o = *s->other;
        if (o != last_other) {
            s->other_progress_seen++;
            last_other = o;
        }
    }
    s->preempted = thread_current()->preempted_count;
    return 0;
}

static void test_preemption(void)
{
    section("Preemption: two threads that never yield");
    static struct spinner a, b;
    a = (struct spinner){ .other = &b.count };
    b = (struct spinner){ .other = &a.count };
    spin_stop = false;

    struct sched_stats s0, s1;
    sched_get_stats(&s0);
    struct thread *ta = spawn(process_kernel(), "spin-A", spinner_entry, &a);
    struct thread *tb = spawn(process_kernel(), "spin-B", spinner_entry, &b);
    if (!CHECK(ta && tb, "create two CPU-bound threads with no yield calls"))
        return;
    tid_t tids[2] = { ta->tid, tb->tid };

    sched_sleep(40);                 /* kmain blocks; A and B share the CPU */
    uint64_t a_count = a.count, b_count = b.count;
    sched_get_stats(&s1);
    spin_stop = true;
    wait_gone(tids, 2);

    kprintf("[test]   A: %lu iterations, saw B progress %lu times while inside its loop\n",
            a_count, a.other_progress_seen);
    kprintf("[test]   B: %lu iterations, saw A progress %lu times while inside its loop\n",
            b_count, b.other_progress_seen);
    kprintf("[test]   preemptions during the window: %lu (A preempted %lu, B preempted %lu)\n",
            s1.preemptions - s0.preemptions, a.preempted, b.preempted);

    bool both_ran = a_count > 0 && b_count > 0;
    bool interleaved = a.other_progress_seen >= 3 && b.other_progress_seen >= 3;
    bool timer_driven = a.preempted >= 3 && b.preempted >= 3
                     && s1.preemptions - s0.preemptions >= 6;
    CHECK(both_ran, "both non-yielding threads made progress");
    CHECK(interleaved,
          "each saw the other advance while itself still inside its loop (it was interrupted)");
    CHECK(timer_driven, "switches were forced by the timer (preemption counters)");
    CHECK(thread_gone(tids[0]) && thread_gone(tids[1]), "both threads exited when told to stop");
    result.preemption = both_ran && interleaved && timer_driven;
}

/* ---- 2. every register survives preemption ----------------------------- */

struct regspin {
    uint64_t seed;
    volatile uint64_t iterations;
    volatile uint64_t mismatches;
    volatile uint64_t preempted;
};

static volatile uint64_t regspin_stop;

static int64_t regspin_entry(void *arg)
{
    struct regspin *r = arg;
    r->mismatches = preempt_reg_spin(r->seed, &regspin_stop, &r->iterations);
    r->preempted = thread_current()->preempted_count;
    return 0;
}

static void test_registers(void)
{
    section("Context preservation: all 15 GPRs across preemption");
    static struct regspin r[3];
    struct thread *t[3];
    tid_t tids[3];
    regspin_stop = 0;
    for (int i = 0; i < 3; i++) {
        r[i] = (struct regspin){ .seed = 0x0123456789ABCDEFULL * (uint64_t)(i + 1) };
        t[i] = spawn(process_kernel(), "regspin", regspin_entry, &r[i]);
        if (t[i] == NULL)
            return;
        tids[i] = t[i]->tid;
    }

    sched_sleep(50);
    regspin_stop = 1;
    wait_gone(tids, 3);

    bool ok = true;
    for (int i = 0; i < 3; i++) {
        kprintf("[test]   regspin %d: %lu checks of all registers, %lu preemptions, %lu mismatches\n",
                i, r[i].iterations, r[i].preempted, r[i].mismatches);
        ok &= r[i].mismatches == 0 && r[i].iterations > 0 && r[i].preempted >= 2;
    }
    CHECK(ok, "rax..r15 intact through every preemption in three competing threads");
    result.registers = ok;
}

/* ---- 3. stack isolation under preemption -------------------------------- */

#define ISO_WORDS 256

struct isoworker {
    uint64_t seed;
    volatile uint64_t rounds;
    volatile uint64_t errors;
    volatile uint64_t preempted;
};

static volatile bool iso_stop;

static inline uint64_t mix(uint64_t seed, uint64_t i, uint64_t round)
{
    uint64_t x = seed ^ (i * 0x9E3779B97F4A7C15ULL) ^ (round << 40);
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ULL;
    return x ^ (x >> 29);
}

static int64_t iso_entry(void *arg)
{
    struct isoworker *w = arg;
    volatile uint64_t buf[ISO_WORDS];      /* 2 KiB on this thread's stack */
    uint64_t round = 0;
    for (uint64_t i = 0; i < ISO_WORDS; i++)
        buf[i] = mix(w->seed, i, round);

    while (!iso_stop) {                     /* no yields: preempted anywhere */
        for (uint64_t i = 0; i < ISO_WORDS; i++) {
            if (buf[i] != mix(w->seed, i, round))
                w->errors++;
            buf[i] = mix(w->seed, i, round + 1);
        }
        round++;
        w->rounds = round;
    }
    w->preempted = thread_current()->preempted_count;
    return 0;
}

static void test_isolation(void)
{
    section("Stack isolation: three threads rewriting stack data while preempted");
    static struct isoworker w[3];
    tid_t tids[3];
    iso_stop = false;
    for (int i = 0; i < 3; i++) {
        w[i] = (struct isoworker){ .seed = 0xA5A50000ULL + (uint64_t)i };
        struct thread *t = spawn(process_kernel(), "iso", iso_entry, &w[i]);
        if (t == NULL)
            return;
        tids[i] = t->tid;
    }
    sched_sleep(60);
    iso_stop = true;
    wait_gone(tids, 3);

    bool ok = true;
    for (int i = 0; i < 3; i++) {
        kprintf("[test]   iso %d: %lu rounds verified, %lu preemptions, %lu corrupted words\n",
                i, w[i].rounds, w[i].preempted, w[i].errors);
        ok &= w[i].errors == 0 && w[i].rounds > 0 && w[i].preempted >= 2;
    }
    CHECK(ok, "each thread's stack data stayed exactly as it left it");
    result.isolation = ok;
}

/* ---- 4. round-robin order ------------------------------------------------ */

#define RR_THREADS 3
#define RR_LOG     60

static volatile int rr_last = -1;
static volatile unsigned rr_len;
static int rr_log[RR_LOG];
static volatile bool rr_stop;

static int64_t rr_entry(void *arg)
{
    int me = (int)(uint64_t)arg;
    while (!rr_stop) {
        if (rr_last != me) {
            /* First instruction after being given the CPU: log who we are.
             * Tiny critical section so two loggers can't race on rr_len. */
            IRQ_GUARD();
            if (rr_len < RR_LOG)
                rr_log[rr_len++] = me;
            rr_last = me;
        }
    }
    return 0;
}

static void test_round_robin(void)
{
    section("Round-robin order: A -> B -> C -> A ...");
    tid_t tids[RR_THREADS];
    rr_stop = false;
    rr_last = -1;
    rr_len = 0;
    for (int i = 0; i < RR_THREADS; i++) {
        struct thread *t = spawn(process_kernel(), "rr", rr_entry, (void *)(uint64_t)i);
        if (t == NULL)
            return;
        tids[i] = t->tid;
    }
    sched_sleep(40);
    /* Snapshot the log atomically with the stop request: after it, threads
     * exit on seeing the flag without logging, which would make the tail
     * look like a skipped turn even though the scheduler was correct. */
    unsigned n;
    {
        IRQ_GUARD();
        n = rr_len;
        rr_stop = true;
    }
    wait_gone(tids, RR_THREADS);
    bool cyclic = n >= 2 * RR_THREADS;
    bool distinct = n >= RR_THREADS && rr_log[0] != rr_log[1] && rr_log[1] != rr_log[2]
                 && rr_log[0] != rr_log[2];
    for (unsigned i = RR_THREADS; cyclic && i < n; i++)
        cyclic = rr_log[i] == rr_log[i - RR_THREADS];

    kprintf("[test]   CPU handed out in this order:");
    for (unsigned i = 0; i < n && i < 24; i++)
        kprintf(" %c", 'A' + rr_log[i]);
    kprintf("%s\n", n > 24 ? " ..." : "");
    CHECK(distinct && cyclic, "strict FIFO rotation: every thread in turn, same order each cycle");
    result.rr_order = distinct && cyclic;
}

/* ---- 5. fairness ---------------------------------------------------------- */

#define FAIR_THREADS 3
#define FAIR_TICKS   150

struct fair {
    volatile uint64_t work;
};

static volatile bool fair_stop;

static int64_t fair_entry(void *arg)
{
    struct fair *f = arg;
    while (!fair_stop)
        f->work++;
    return 0;
}

static void test_fairness(void)
{
    section("Fairness: three CPU-bound threads for a fixed number of ticks");
    static struct fair f[FAIR_THREADS];
    struct thread *t[FAIR_THREADS];
    tid_t tids[FAIR_THREADS];
    fair_stop = false;
    for (int i = 0; i < FAIR_THREADS; i++) {
        f[i].work = 0;
        t[i] = spawn(process_kernel(), "fair", fair_entry, &f[i]);
        if (t[i] == NULL)
            return;
        tids[i] = t[i]->tid;
    }

    uint64_t start = sched_ticks();
    sched_sleep(FAIR_TICKS);
    uint64_t slices[FAIR_THREADS], cpu[FAIR_THREADS], work[FAIR_THREADS];
    {
        IRQ_GUARD();                 /* consistent snapshot; threads alive */
        for (int i = 0; i < FAIR_THREADS; i++) {
            slices[i] = t[i]->switch_count;
            cpu[i] = t[i]->cpu_ticks;
            work[i] = f[i].work;
        }
    }
    uint64_t elapsed = sched_ticks() - start;
    fair_stop = true;
    wait_gone(tids, FAIR_THREADS);

    uint64_t smin = slices[0], smax = slices[0], cmin = cpu[0], cmax = cpu[0];
    kprintf("\n  SCHEDULER FAIRNESS TEST (%lu ticks, quantum %d)\n\n", elapsed, SCHED_QUANTUM_TICKS);
    for (int i = 0; i < FAIR_THREADS; i++) {
        kprintf("  Thread %c : %3lu time slices, %3lu CPU ticks, %lu loop iterations\n",
                'A' + i, slices[i], cpu[i], work[i]);
        smin = slices[i] < smin ? slices[i] : smin;
        smax = slices[i] > smax ? slices[i] : smax;
        cmin = cpu[i] < cmin ? cpu[i] : cmin;
        cmax = cpu[i] > cmax ? cpu[i] : cmax;
    }
    kprintf("\n");

    bool no_starvation = smin > 0 && cmin > 0;
    bool balanced = smax - smin <= 2 && cmax - cmin <= 2 * SCHED_QUANTUM_TICKS;
    CHECK(no_starvation, "No runnable thread starved");
    CHECK(balanced, "time slices within 2 and CPU ticks within 2 quanta of each other");
    result.fairness = no_starvation && balanced;
}

/* ---- 6. termination ------------------------------------------------------ */

static volatile uint64_t never_ran;

static int64_t short_entry(void *arg)
{
    (void)arg;
    volatile uint64_t x = 0;
    for (int i = 0; i < 1000; i++)
        x += (uint64_t)i;
    return (int64_t)x;
}

static int64_t explicit_exit_entry(void *arg)
{
    (void)arg;
    volatile uint64_t spin = 0;
    uint64_t until = sched_ticks() + 5;      /* get preempted a few times */
    while (sched_ticks() < until)
        spin++;
    thread_exit(77);
}

static int64_t marker_entry(void *arg)
{
    (void)arg;
    never_ran++;
    return 0;
}

static int64_t long_sleeper_entry(void *arg)
{
    (void)arg;
    sched_sleep(100000);
    return 0;
}

static void test_termination(void)
{
    section("Termination under the scheduler");
    proc_status_t err;
    unsigned stacks0 = kstack_slots_in_use();

    struct thread *t1 = spawn(process_kernel(), "short", short_entry, NULL);
    struct thread *t2 = spawn(process_kernel(), "exit77", explicit_exit_entry, NULL);
    if (!t1 || !t2)
        return;
    tid_t tid1 = t1->tid, tid2 = t2->tid;
    sched_sleep(15);
    CHECK(thread_gone(tid1) && thread_gone(tid2),
          "running threads terminate by returning and by thread_exit");
    CHECK(sched_check(), "no TERMINATED thread left on any scheduler queue");

    /* A READY thread terminated before it ever gets the CPU must never run. */
    never_ran = 0;
    bool killed;
    {
        IRQ_GUARD();                 /* create + terminate before any tick */
        struct thread *t3 = spawn(process_kernel(), "marker", marker_entry, NULL);
        killed = t3 != NULL && thread_terminate(t3) == PROC_OK;
    }
    sched_sleep(10);
    CHECK(killed && never_ran == 0, "a terminated READY thread is never scheduled");

    /* Killing a process removes its sleeping thread from the sleep list. */
    struct process *p = process_create("sleeper", 0, &err);
    struct thread *t4 = p ? spawn(p, "sleeper", long_sleeper_entry, NULL) : NULL;
    sched_sleep(3);                  /* let it run and fall asleep */
    bool asleep = t4 != NULL && t4->state == TASK_BLOCKED && t4->sched_list == SCHED_LIST_SLEEP;
    CHECK(asleep, "thread is BLOCKED on the sleep list");
    CHECK(p != NULL && process_terminate(p) == PROC_OK && sched_check(),
          "terminating its process takes it off the sleep list consistently");

    /* Nothing else is runnable while we sleep, so the idle thread reaps. */
    sched_sleep(10);
    CHECK(kstack_slots_in_use() == stacks0,
          "idle thread reclaimed every exited thread's stack");
    result.termination = sched_check() && kstack_slots_in_use() == stacks0 && never_ran == 0;
}

/* ---- 7. idle and sleep --------------------------------------------------- */

static void test_idle_and_sleep(void)
{
    section("Idle thread and sleeping");
    struct sched_stats s0, s1;
    sched_get_stats(&s0);
    uint64_t t0 = sched_ticks();
    sched_sleep(20);
    uint64_t slept = sched_ticks() - t0;
    sched_get_stats(&s1);

    kprintf("[test]   slept %lu ticks; idle ran for %lu ticks, %lu switches to idle\n",
            slept, s1.idle_ticks - s0.idle_ticks, s1.idle_switches - s0.idle_switches);
    bool idle_ok = s1.idle_ticks - s0.idle_ticks >= 15 && s1.idle_switches > s0.idle_switches;
    bool sleep_ok = slept >= 20 && slept <= 20 + SCHED_QUANTUM_TICKS + 1;
    CHECK(idle_ok, "idle thread runs when nothing else is READY");
    CHECK(sleep_ok, "sleep(20) blocks for 20 ticks (within one quantum)");
    CHECK(thread_current()->state == TASK_RUNNING && sched_check(),
          "idle gives the CPU back as soon as a thread wakes");
    result.idle = idle_ok;
    result.sleep = sleep_ok;
}

/* ---- 8. overhead sanity -------------------------------------------------- */

#define YIELD_ROUNDS 5000

struct yielder {
    volatile uint64_t tsc_start, tsc_end;
};

static int64_t yield_entry(void *arg)
{
    struct yielder *y = arg;
    y->tsc_start = rdtsc();
    for (int i = 0; i < YIELD_ROUNDS; i++)
        sched_yield();
    y->tsc_end = rdtsc();
    return 0;
}

static void test_overhead(void)
{
    section("Scheduler overhead sanity check");

    /* Cycles per timer tick, from the TSC across a known number of ticks. */
    uint64_t k0 = sched_ticks(), c0 = rdtsc();
    sched_sleep(20);
    uint64_t cycles_per_tick = (rdtsc() - c0) / (sched_ticks() - k0);

    /* Cost of one switch: two threads yielding to each other. */
    static struct yielder y0, y1;
    struct sched_stats s0, s1;
    sched_get_stats(&s0);
    struct thread *a = spawn(process_kernel(), "yield-a", yield_entry, &y0);
    struct thread *b = spawn(process_kernel(), "yield-b", yield_entry, &y1);
    if (!a || !b)
        return;
    tid_t tids[2] = { a->tid, b->tid };
    wait_gone(tids, 2);
    sched_get_stats(&s1);

    uint64_t start = y0.tsc_start < y1.tsc_start ? y0.tsc_start : y1.tsc_start;
    uint64_t end = y0.tsc_end > y1.tsc_end ? y0.tsc_end : y1.tsc_end;
    uint64_t switches = s1.voluntary - s0.voluntary;
    uint64_t cycles_per_switch = switches ? (end - start) / switches : 0;
    uint64_t switches_per_sec = SCHED_TIMER_HZ / SCHED_QUANTUM_TICKS;
    /* overhead (parts per million) = switches/s * cycles/switch / cycles/s */
    uint64_t cycles_per_sec = cycles_per_tick * SCHED_TIMER_HZ;
    uint64_t ppm = cycles_per_sec ? switches_per_sec * cycles_per_switch * 1000000 / cycles_per_sec : 0;

    kprintf("[test]   timer frequency        : %d Hz (tick = %d ms)\n",
            SCHED_TIMER_HZ, 1000 / SCHED_TIMER_HZ);
    kprintf("[test]   scheduling interval    : %d ticks = %d ms\n",
            SCHED_QUANTUM_TICKS, SCHED_QUANTUM_TICKS * 1000 / SCHED_TIMER_HZ);
    kprintf("[test]   TSC cycles per tick    : %lu\n", cycles_per_tick);
    kprintf("[test]   yield switches measured: %lu\n", switches);
    kprintf("[test]   cycles per switch      : %lu\n", cycles_per_switch);
    kprintf("[test]   preemptive switch cost : ~%lu.%04lu%% of CPU at %lu switches/s\n",
            ppm / 10000, ppm % 10000, switches_per_sec);
    CHECK(switches >= 2 * YIELD_ROUNDS - 2, "every yield between two threads is a switch");
    CHECK(cycles_per_switch > 0 && ppm < 50000, "scheduling overhead below 5% of CPU time");
}

/* ---- live demo ------------------------------------------------------------ */

struct worker {
    const char *label;
    tid_t tid;
    volatile uint64_t iterations;
    volatile uint64_t errors;
    volatile uint64_t final_switches, final_cpu, final_preempted;
    volatile bool done;
};

static struct worker workers[3];
static volatile bool demo_stop;

static void worker_finish(struct worker *w)
{
    struct thread *self = thread_current();
    w->final_switches = self->switch_count;
    w->final_cpu = self->cpu_ticks;
    w->final_preempted = self->preempted_count;
    w->done = true;
}

/* CPU worker: counts primes by trial division. */
static int64_t cpu_worker(void *arg)
{
    struct worker *w = arg;
    for (uint64_t n = 3; !demo_stop; n += 2) {
        bool prime = true;
        for (uint64_t d = 3; d * d <= n; d += 2) {
            if (n % d == 0) {
                prime = false;
                break;
            }
        }
        if (prime)
            w->iterations++;
    }
    worker_finish(w);
    return 0;
}

/* Memory worker: allocate, fill, verify, free, and check the heap, while
 * being preempted in the middle of all of it. */
static int64_t memory_worker(void *arg)
{
    struct worker *w = arg;
    uint64_t seed = 12345;
    while (!demo_stop) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t size = 16 + (size_t)((seed >> 33) % 2048);
        uint8_t *p = kmalloc(size);
        if (p == NULL) {
            w->errors++;
            continue;
        }
        for (size_t i = 0; i < size; i++)
            p[i] = (uint8_t)(i ^ seed);
        for (size_t i = 0; i < size; i++) {
            if (p[i] != (uint8_t)(i ^ seed)) {
                w->errors++;
                break;
            }
        }
        kfree(p);
        if ((w->iterations & 63) == 0 && !heap_check())
            w->errors++;
        w->iterations++;
    }
    worker_finish(w);
    return 0;
}

/* Counter worker: the simplest possible busy loop. */
static int64_t counter_worker(void *arg)
{
    struct worker *w = arg;
    while (!demo_stop)
        w->iterations++;
    worker_finish(w);
    return 0;
}

struct snap_row {
    tid_t tid;
    task_state_t state;
    char name[TASK_NAME_LEN];
    const char *work;
    uint64_t iterations, switches, cpu;
};

static const char *work_label(tid_t tid, uint64_t *iters)
{
    for (int i = 0; i < 3; i++) {
        if (workers[i].tid == tid) {
            *iters = workers[i].iterations;
            return workers[i].label;
        }
    }
    *iters = 0;
    return NULL;
}

static void dashboard(unsigned frame, unsigned frames)
{
    struct snap_row rows[8];
    unsigned n = 0;
    struct sched_stats s;
    {
        IRQ_GUARD();                 /* one consistent snapshot of real state */
        for (unsigned i = 0; i < THREAD_MAX && n < 8; i++) {
            struct thread *t = thread_at(i);
            if (t == NULL)
                continue;
            struct snap_row *r = &rows[n++];
            r->tid = t->tid;
            r->state = t->state;
            for (int c = 0; c < TASK_NAME_LEN; c++)
                r->name[c] = t->name[c];
            r->work = work_label(t->tid, &r->iterations);
            r->switches = t->switch_count;
            r->cpu = t->cpu_ticks;
        }
        sched_get_stats(&s);
    }

    box_top();
    box_line("NEXUS SCHEDULER  (live, refresh %u/%u)", frame, frames);
    box_separator();
    box_line("%-4s %-10s %-14s %12s %9s %9s", "TID", "STATE", "WORK", "ITERATIONS",
             "SWITCHES", "CPU TICKS");
    box_line("%s", "───────────────────────────────────────────────────────────────────");
    for (unsigned i = 0; i < n; i++) {
        const struct snap_row *r = &rows[i];
        if (r->work != NULL)
            box_line("%-4u %-10s %-14s %12lu %9lu %9lu", r->tid, task_state_str(r->state),
                     r->work, r->iterations, r->switches, r->cpu);
        else
            box_line("%-4u %-10s %-14s %12s %9lu %9lu", r->tid, task_state_str(r->state),
                     r->name, "-", r->switches, r->cpu);
    }
    box_line("%s", "");
    box_line("Timer ticks       : %lu", s.ticks);
    box_line("Context switches  : %lu (%lu preemptive, %lu voluntary)",
             s.switches, s.preemptions, s.voluntary);
    box_line("Runnable threads  : %u    Current thread : TID %u (this dashboard)",
             s.runnable, s.current);
    box_line("Scheduling: ROUND-ROBIN, quantum %d ms", SCHED_QUANTUM_TICKS * 1000 / SCHED_TIMER_HZ);
    box_bottom();
}

static void run_demo(void)
{
    section("Live multitasking demo");
    workers[0] = (struct worker){ .label = "CPU Worker" };
    workers[1] = (struct worker){ .label = "Memory Worker" };
    workers[2] = (struct worker){ .label = "Counter" };
    thread_entry_t fns[3] = { cpu_worker, memory_worker, counter_worker };
    const char *names[3] = { "cpu", "memory", "counter" };
    demo_stop = false;

    struct process *p = process_create("workers", 0, NULL);
    if (p == NULL)
        return;
    {
        /* Publish all TIDs before any worker can be scheduled. */
        IRQ_GUARD();
        for (int i = 0; i < 3; i++) {
            struct thread *t = spawn(p, names[i], fns[i], &workers[i]);
            workers[i].tid = t ? t->tid : 0;
        }
    }

    kprintf("[demo] Tracing the first scheduling decisions (rate-limited):\n");
    sched_set_trace(6);
    const unsigned frames = 4;
    for (unsigned f = 1; f <= frames; f++) {
        sched_sleep(50);             /* workers run, preempting each other */
        if (f == 1)
            sched_set_trace(0);
        kprintf("\n");
        dashboard(f, frames);
    }

    demo_stop = true;
    tid_t tids[3] = { workers[0].tid, workers[1].tid, workers[2].tid };
    wait_gone(tids, 3);
}

static void final_panel(void)
{
    struct sched_stats s;
    sched_get_stats(&s);
    kprintf("\n");
    box_top();
    box_line("%s", "                    NEXUS SYSTEM STATUS");
    box_separator();
    box_line("Scheduler       : ROUND-ROBIN (quantum %d ms)", SCHED_QUANTUM_TICKS * 1000 / SCHED_TIMER_HZ);
    box_line("CPU             : x86-64, 1 CPU");
    box_line("Timer           : ACTIVE, PIT %d Hz", SCHED_TIMER_HZ);
    box_line("%s", "");
    box_line("%-4s %-14s %12s %9s %10s %9s", "TID", "WORK", "ITERATIONS", "SWITCHES",
             "PREEMPTED", "CPU TICKS");
    box_line("%s", "───────────────────────────────────────────────────────────────────");
    for (int i = 0; i < 3; i++) {
        const struct worker *w = &workers[i];
        box_line("%-4u %-14s %12lu %9lu %10lu %9lu", w->tid, w->label, w->iterations,
                 w->final_switches, w->final_preempted, w->final_cpu);
    }
    box_line("Memory worker errors: %lu", workers[1].errors);
    box_line("%s", "");
    box_line("Timer ticks       : %lu", s.ticks);
    box_line("Context switches  : %lu (%lu preemptive, %lu voluntary)",
             s.switches, s.preemptions, s.voluntary);
    box_line("Idle ticks        : %lu", s.idle_ticks);
    box_line("%s", "");
    box_line("[%s] Preemption (non-yielding threads interleaved)", result.preemption ? "PASS" : "FAIL");
    box_line("[%s] Context preservation (all 15 GPRs)", result.registers ? "PASS" : "FAIL");
    box_line("[%s] Thread isolation (stack data under preemption)", result.isolation ? "PASS" : "FAIL");
    box_line("[%s] Round-robin order", result.rr_order ? "PASS" : "FAIL");
    box_line("[%s] Scheduler fairness", result.fairness ? "PASS" : "FAIL");
    box_line("[%s] Termination (never rescheduled, reclaimed)", result.termination ? "PASS" : "FAIL");
    box_line("[%s] Idle thread and sleep", result.idle && result.sleep ? "PASS" : "FAIL");
    box_separator();
    box_line("checks: %u passed, %u failed", passed, failed);
    box_bottom();
}

bool sched_run_tests(void)
{
    passed = failed = 0;
    kprintf("\n[test] ===== Phase 5 scheduler tests =====\n");

    section("Scheduler state after start");
    struct thread *self = thread_current();
    CHECK(sched_active() && sched_check(), "scheduler active, queues consistent");
    CHECK(self->tid == 0 && self->state == TASK_RUNNING,
          "boot flow is the current RUNNING thread (TID 0)");
    struct thread *idle = sched_idle_thread();
    CHECK(idle != NULL && idle->state == TASK_READY && idle->sched_list == SCHED_LIST_NONE,
          "idle thread exists and is kept off the ready queue");

    test_preemption();
    test_registers();
    test_isolation();
    test_round_robin();
    test_fairness();
    test_termination();
    test_idle_and_sleep();
    test_overhead();
    run_demo();

    bool demo_ok = workers[0].iterations > 0 && workers[1].iterations > 0
                && workers[2].iterations > 0 && workers[1].errors == 0
                && workers[0].final_preempted > 0 && workers[1].final_preempted > 0
                && workers[2].final_preempted > 0;
    CHECK(demo_ok, "demo: all three workers progressed, were preempted, no memory errors");
    CHECK(sched_check(), "scheduler queues consistent after the demo");

    kprintf("\n[test] ===== %u passed, %u failed =====\n", passed, failed);
    kprintf("\n");
    sched_print_stats();
    final_panel();
    return failed == 0;
}
