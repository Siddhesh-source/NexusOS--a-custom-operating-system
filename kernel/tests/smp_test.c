/* Phase 9 self-tests: multi-core.
 *
 * Most tests run one worker thread pinned to each CPU, so they also pass
 * (with less to prove) when QEMU gives the kernel a single CPU. */

#include "smp_test.h"
#include "percpu.h"
#include "spinlock.h"
#include "ipi.h"
#include "rcu.h"
#include "sched.h"
#include "proc.h"
#include "heap.h"
#include "pmm.h"
#include "slab.h"
#include "vmm.h"
#include "apic.h"
#include "clock.h"
#include "cpu.h"
#include "box.h"
#include "kprintf.h"
#include "string.h"
#include "panic.h"

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

static unsigned ncpu;

static bool cpu_ok(unsigned c)
{
    return c < SMP_MAX_CPUS && cpu_at(c)->online;
}

/* Run fn(arg_i) as one thread pinned to every online CPU, i = CPU number,
 * and wait (sleeping) until all have exited. Returns how many ran. */
struct worker {
    unsigned cpu;
    volatile bool done;
    volatile uint64_t result;
    volatile uint64_t errors;
    void *shared;
};

static struct worker workers[SMP_MAX_CPUS];

static unsigned run_on_every_cpu(thread_entry_t fn, void *shared)
{
    unsigned started = 0;
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        struct worker *w = &workers[c];
        memset(w, 0, sizeof(*w));
        w->cpu = c;
        w->shared = shared;
        w->done = true;
        if (!cpu_ok(c))
            continue;
        w->done = false;
        proc_status_t err;
        if (thread_create_on(process_kernel(), "smp-worker", fn, w, c, &err) == NULL) {
            kprintf("[test]   cannot start a worker on CPU %u: %s\n", c, proc_status_str(err));
            w->done = true;
            continue;
        }
        started++;
    }
    for (unsigned tries = 0; tries < 6000; tries++) {   /* up to 60 s */
        bool all = true;
        for (unsigned c = 0; c < SMP_MAX_CPUS; c++)
            all &= workers[c].done;
        if (all)
            break;
        sched_sleep(1);
    }
    return started;
}

/* ---- 1. CPUs and per-CPU data ------------------------------------------ */

static int64_t identity_worker(void *arg)
{
    struct worker *w = arg;
    uint64_t f = irq_save();
    struct percpu *c = this_cpu();
    bool ok = c->self == c && c->id == w->cpu && cpu_id() == w->cpu
              && c == cpu_at(w->cpu) && c->current == thread_current()
              && c->lapic_id == lapic_id() && interrupts_enabled() == false;
    irq_restore(f);
    w->result = ok;
    w->done = true;
    return 0;
}

static void test_cpus(void)
{
    section("CPUs online and per-CPU data");
    ncpu = smp_cpu_count();
    kprintf("[test]   %u CPU(s) online, online mask 0x%x\n", ncpu, smp_online_mask());
    CHECK(ncpu >= 1 && (smp_online_mask() & 1), "the boot CPU is online");

    bool distinct = true;
    for (unsigned a = 0; a < SMP_MAX_CPUS; a++)
        for (unsigned b = a + 1; b < SMP_MAX_CPUS; b++)
            if (cpu_ok(a) && cpu_ok(b) && cpu_at(a)->lapic_id == cpu_at(b)->lapic_id)
                distinct = false;
    CHECK(distinct, "every CPU has its own local APIC id");

    unsigned started = run_on_every_cpu(identity_worker, NULL);
    bool all = started == ncpu;
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++)
        if (cpu_ok(c))
            all &= workers[c].done && workers[c].result == 1;
    CHECK(all, "a thread pinned to each CPU finds its own per-CPU record through GS");
    CHECK(cpu_at(0)->idle != NULL && (ncpu == 1 || (cpu_ok(1) && cpu_at(1)->idle != NULL
                                                    && cpu_at(1)->idle != cpu_at(0)->idle)),
          "each CPU has its own idle thread");
}

/* ---- 2. spinlock: no lost updates ---------------------------------------- */

#define STRESS_TOTAL 1000000ULL

static struct spinlock counter_lock = SPINLOCK_INIT(LOCK_CLASS_TEST);
static volatile uint64_t locked_counter, unlocked_counter;

static int64_t counter_worker(void *arg)
{
    struct worker *w = arg;
    uint64_t share = STRESS_TOTAL / ncpu + (w->cpu < STRESS_TOTAL % ncpu ? 1 : 0);
    for (uint64_t i = 0; i < share; i++) {
        uint64_t f;
        spin_lock_irqsave(&counter_lock, &f);
        locked_counter = locked_counter + 1;      /* a plain read-modify-write */
        spin_unlock_irqrestore(&counter_lock, f);
    }
    for (uint64_t i = 0; i < share / 10; i++)
        unlocked_counter = unlocked_counter + 1;  /* no lock: updates can be lost */
    w->result = share;
    w->done = true;
    return 0;
}

static void test_counter(void)
{
    section("Spinlock under load: 1,000,000 increments from every CPU");
    locked_counter = unlocked_counter = 0;
    uint64_t t0 = clock_now_ns();
    run_on_every_cpu(counter_worker, NULL);
    uint64_t ms = (clock_now_ns() - t0) / 1000000;
    kprintf("[test]   %lu increments on %u CPU(s) in %lu ms; without the lock %lu of %lu "
            "updates survived\n", locked_counter, ncpu, ms, unlocked_counter, (uint64_t)(STRESS_TOTAL / 10));
    CHECK(locked_counter == STRESS_TOTAL, "no lost updates: the counter is exactly 1,000,000");
    CHECK(!counter_lock.locked, "the lock is free afterwards");
}

/* ---- 3. allocators from every CPU ---------------------------------------- */

#define ALLOC_ROUNDS 1500

static struct slab_cache *stress_cache;

static int64_t alloc_worker(void *arg)
{
    struct worker *w = arg;
    uint64_t seed = 0x9e3779b97f4a7c15ULL * (w->cpu + 1);
    for (unsigned i = 0; i < ALLOC_ROUNDS; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        size_t size = 16 + (seed >> 33) % 600;
        uint8_t *a = kmalloc(size);
        uint64_t *o = slab_alloc(stress_cache);
        uint64_t page = pmm_alloc_page();
        if (a == NULL || o == NULL || page == 0) {
            w->errors++;
        } else {
            uint8_t tag = (uint8_t)(w->cpu * 31 + i);
            memset(a, tag, size);
            o[0] = o[7] = seed;
            uint64_t *pv = phys_to_virt(page);
            pv[0] = pv[511] = seed;
            sched_yield();                       /* let others interleave */
            for (size_t k = 0; k < size; k++)
                if (a[k] != tag) {
                    w->errors++;
                    break;
                }
            if (o[0] != seed || o[7] != seed || pv[0] != seed || pv[511] != seed)
                w->errors++;
        }
        kfree(a);
        slab_free(stress_cache, o);
        if (page != 0)
            pmm_free_page(page);
    }
    w->done = true;
    return 0;
}

static void test_allocators(void)
{
    section("Heap, slab and page allocators used from every CPU at once");
    if (stress_cache == NULL)
        stress_cache = slab_create("smp_stress", 64, 16, 0);
    uint64_t free0 = pmm_free_page_count();
    run_on_every_cpu(alloc_worker, NULL);
    uint64_t errors = 0;
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++)
        errors += workers[c].errors;
    kprintf("[test]   %u CPU(s) x %d rounds of kmalloc + slab + page, %lu errors\n",
            ncpu, ALLOC_ROUNDS, errors);
    CHECK(errors == 0, "no allocation failed and no CPU's data was overwritten by another's");
    CHECK(heap_check() && pmm_check() && stress_cache != NULL && slab_check(stress_cache),
          "heap, page allocator and slab cache are consistent afterwards");
    pmm_drain_cache();
    CHECK(pmm_free_page_count() + 64 >= free0, "every page came back");
}

/* ---- 4. lock-order checker ------------------------------------------------ */

static struct spinlock lock_a = SPINLOCK_INIT(LOCK_CLASS_TEST_A);
static struct spinlock lock_b = SPINLOCK_INIT(LOCK_CLASS_TEST_B);

__attribute__((noinline)) static void take_a_then_b(void)
{
    uint64_t f;
    spin_lock_irqsave(&lock_a, &f);
    spin_lock(&lock_b);
    spin_unlock(&lock_b);
    spin_unlock_irqrestore(&lock_a, f);
}

__attribute__((noinline)) static void take_b_then_a(void)
{
    uint64_t f;
    spin_lock_irqsave(&lock_b, &f);
    spin_lock(&lock_a);
    spin_unlock(&lock_a);
    spin_unlock_irqrestore(&lock_b, f);
}

static void test_lock_order(void)
{
    section("Lock-order checker");
    kprintf("[test]   the running kernel has taught it %u lock orders so far\n",
            lockcheck_order_count());
    CHECK(lockcheck_order_known(LOCK_CLASS_VM, LOCK_CLASS_PMM)
          && lockcheck_order_known(LOCK_CLASS_RUNQUEUE, LOCK_CLASS_PROC),
          "it learned real orders (memory regions before pages, run queue before process table)");

    lockcheck_forget(LOCK_CLASS_TEST_A);
    lockcheck_forget(LOCK_CLASS_TEST_B);
    bool was_fatal = lockcheck_set_fatal(false);     /* report, don't panic */
    uint64_t v0 = lockcheck_violations();
    take_a_then_b();
    CHECK(lockcheck_order_known(LOCK_CLASS_TEST_A, LOCK_CLASS_TEST_B)
          && lockcheck_violations() == v0, "A then B is recorded, nothing reported");
    take_a_then_b();
    CHECK(lockcheck_violations() == v0, "the same order again is fine");
    kprintf("[test]   now taking them the other way round (a report follows):\n");
    take_b_then_a();
    CHECK(lockcheck_violations() == v0 + 1,
          "B then A is caught as an inversion, even though no deadlock happened");
    lockcheck_set_fatal(was_fatal);
    lockcheck_forget(LOCK_CLASS_TEST_A);
    lockcheck_forget(LOCK_CLASS_TEST_B);
}

void smp_demo_lock_inversion(void)
{
    kprintf("\n[demo] Taking two locks in opposite orders...\n");
    take_a_then_b();
    take_b_then_a();
    panic("lock order demo: the checker did not fire");
}

/* ---- 5. IPIs and TLB shootdown -------------------------------------------- */

#define SHOOT_VA (MM_TEST_VBASE + 0x7700000ULL)

struct shoot_probe {
    volatile uint64_t seen[SMP_MAX_CPUS];
};

static void read_probe(void *arg)
{
    struct shoot_probe *p = arg;
    p->seen[cpu_id()] = *(volatile uint64_t *)SHOOT_VA;
}

static bool all_saw(const struct shoot_probe *p, uint64_t v)
{
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++)
        if (cpu_ok(c) && c != 0 && p->seen[c] != v)
            return false;
    return true;
}

static void test_shootdown(void)
{
    section("IPIs and TLB shootdown");
    address_space_t *ks = vmm_kernel_space();
    uint64_t p1 = pmm_alloc_zeroed_page(), p2 = pmm_alloc_zeroed_page();
    if (p1 == 0 || p2 == 0) {
        CHECK(false, "two test pages");
        return;
    }
    *(uint64_t *)phys_to_virt(p1) = 0x1111;
    *(uint64_t *)phys_to_virt(p2) = 0x2222;
    struct shoot_probe probe;
    memset(&probe, 0, sizeof(probe));

    CHECK(vmm_map_page(ks, SHOOT_VA, p1, VMM_WRITE | VMM_GLOBAL) == VMM_OK, "map a test page");
    smp_call(smp_online_mask(), read_probe, &probe, false);
    CHECK(all_saw(&probe, 0x1111), "every other CPU reads it (and caches the mapping)");

    uint64_t sd0 = vmm_shootdown_count();
    vmm_unmap_page(ks, SHOOT_VA, NULL);
    CHECK(vmm_map_page(ks, SHOOT_VA, p2, VMM_WRITE | VMM_GLOBAL) == VMM_OK,
          "point the same address at a different page");
    smp_call(smp_online_mask(), read_probe, &probe, false);
    CHECK(all_saw(&probe, 0x2222), "every CPU sees the new page: no stale TLB entry anywhere");
    CHECK(ncpu == 1 || vmm_shootdown_count() > sd0, "the unmap sent a shootdown to the other CPUs");

    vmm_unmap_page(ks, SHOOT_VA, NULL);
    pmm_free_page(p1);
    pmm_free_page(p2);

    struct ipi_stats is;
    ipi_get_stats(&is);
    uint64_t served = 0;
    for (unsigned c = 1; c < SMP_MAX_CPUS; c++)
        if (cpu_ok(c))
            served += cpu_at(c)->calls_served;
    kprintf("[test]   %lu shootdowns so far; other CPUs answered %lu requests\n",
            vmm_shootdown_count(), served);
    CHECK(ncpu == 1 || served > 0, "the other CPUs answered requests");
}

/* ---- 6. RCU ------------------------------------------------------------- */

#define RCU_MAGIC   0x5243555f4f4b2121ULL
#define RCU_UPDATES 300

struct rcu_obj {
    uint64_t a, b;              /* a + b == RCU_MAGIC while alive */
};

static struct rcu_obj *rcu_ptr;
static volatile bool rcu_stop;

static int64_t rcu_reader(void *arg)
{
    struct worker *w = arg;
    while (!rcu_stop) {
        uint64_t f = rcu_read_lock();
        struct rcu_obj *o = rcu_dereference(rcu_ptr);
        for (int k = 0; k < 20; k++) {           /* stay a while: freed memory would show */
            if (o->a + o->b != RCU_MAGIC)
                w->errors++;
        }
        rcu_read_unlock(f);
        w->result++;
        /* Some work outside the read section, like a real reader. A loop
         * that is almost always inside one leaves the CPU nearly no
         * moment to take the grace-period IPI (QEMU checks for interrupts
         * only between blocks of instructions). */
        for (int k = 0; k < 64; k++)
            __asm__ volatile("pause");
    }
    w->done = true;
    return 0;
}

static void test_rcu(void)
{
    section("RCU: readers without locks, writer frees only after a grace period");
    rcu_ptr = kmalloc(sizeof(struct rcu_obj));
    rcu_ptr->a = 1;
    rcu_ptr->b = RCU_MAGIC - 1;
    rcu_stop = false;

    /* Readers on every CPU but this one (the writer). */
    unsigned readers = 0;
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        memset(&workers[c], 0, sizeof(workers[c]));
        workers[c].cpu = c;
        workers[c].done = true;
        if (c == cpu_id() || !cpu_ok(c))
            continue;
        workers[c].done = false;
        if (thread_create_on(process_kernel(), "rcu-reader", rcu_reader, &workers[c], c, NULL)) {
            readers++;
        } else {
            workers[c].done = true;
        }
    }
    sched_sleep(1);

    uint64_t gp0 = rcu_grace_periods();
    for (unsigned i = 0; i < RCU_UPDATES; i++) {
        struct rcu_obj *n = kmalloc(sizeof(*n));
        n->a = i + 2;
        n->b = RCU_MAGIC - n->a;
        struct rcu_obj *old = rcu_ptr;
        rcu_assign_pointer(rcu_ptr, n);
        rcu_synchronize();
        kfree(old);                              /* poisoned: a late reader would notice */
    }
    rcu_stop = true;
    for (unsigned tries = 0; tries < 500; tries++) {
        bool all = true;
        for (unsigned c = 0; c < SMP_MAX_CPUS; c++)
            all &= workers[c].done;
        if (all)
            break;
        sched_sleep(1);
    }
    uint64_t reads = 0, errors = 0;
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        reads += workers[c].result;
        errors += workers[c].errors;
    }
    kprintf("[test]   %u reader(s) did %lu reads during %d updates; %lu grace periods\n",
            readers, reads, RCU_UPDATES, rcu_grace_periods() - gp0);
    /* The boot CPU's reaper may add grace periods of its own. */
    CHECK(rcu_grace_periods() - gp0 >= RCU_UPDATES, "every update waited for a grace period");
    CHECK(errors == 0, "no reader ever saw a freed or half-written object");
    CHECK(readers == 0 || reads > 0, "the readers really ran alongside the writer");
    kfree(rcu_ptr);
    rcu_ptr = NULL;

    tid_t me = thread_current()->tid;
    CHECK(thread_find(me) == thread_current() && process_find(0) == process_kernel(),
          "process and thread lookups work lock-free (as RCU readers)");
}

/* ---- 7. per-CPU queues and work stealing ---------------------------------- */

#define BUSY_THREADS_PER_CPU 2
#define BUSY_WORK            12000000ULL     /* loop steps: several time slices */

struct busy {
    volatile bool done;
    volatile uint32_t ran_on;
    volatile uint64_t work;
    uint64_t amount;
};

static struct busy busy[SMP_MAX_CPUS * BUSY_THREADS_PER_CPU];

/* A fixed amount of pure CPU work: never sleeps or yields. */
static int64_t busy_entry(void *arg)
{
    struct busy *b = arg;
    while (b->work < b->amount)
        b->work++;
    b->ran_on = thread_current()->ran_on;
    b->done = true;
    return 0;
}

/* Start n busy threads (placed by the scheduler) and wait for them.
 * Returns the wall time in ms. */
static uint64_t run_busy(unsigned n, uint64_t amount, bool steal)
{
    for (unsigned i = 0; i < n; i++) {
        memset(&busy[i], 0, sizeof(busy[i]));
        busy[i].amount = amount;
        if (thread_create(process_kernel(), "busy", busy_entry, &busy[i], NULL) == NULL)
            busy[i].done = true;
    }
    uint64_t t0 = clock_now_ns();
    if (steal)
        sched_set_stealing(true);
    for (unsigned tries = 0; tries < 6000; tries++) {
        bool all = true;
        for (unsigned i = 0; i < n; i++)
            all &= busy[i].done;
        if (all)
            break;
        sched_sleep(1);
    }
    return (clock_now_ns() - t0) / 1000000;
}

static volatile bool victim_running;

static int64_t victim_entry(void *arg)
{
    (void)arg;
    victim_running = true;
    for (;;)
        __asm__ volatile("pause");
}

static void test_stealing(void)
{
    section("Per-CPU run queues and work stealing");
    unsigned n = ncpu * BUSY_THREADS_PER_CPU;
    struct sched_stats s0, s1;
    sched_get_stats(&s0);

    /* One thread alone first, as the yardstick for one CPU. */
    uint64_t one = run_busy(1, BUSY_WORK, false);

    /* Then n, all queued on this CPU (stealing still off), and let the
     * other CPUs take their share. */
    kprintf("[test]   one thread alone: %lu ms; now %u of them queued on CPU %u, "
            "then work stealing on\n", one, n, cpu_id());
    uint64_t ms = run_busy(n, BUSY_WORK, true);
    sched_get_stats(&s1);

    uint32_t cpus_used = 0;
    bool all_done = true;
    for (unsigned i = 0; i < n; i++) {
        cpus_used |= busy[i].ran_on;
        all_done &= busy[i].done;
    }
    /* How much faster than one CPU running them one after another. The
     * real gain depends on the host: QEMU runs guest CPUs in parallel
     * only when it has host cores to spare. Reported, not required. */
    uint64_t speedup10 = ms ? (uint64_t)n * one * 10 / ms : 0;
    kprintf("[test]   %u threads finished in %lu ms (one CPU: about %lu ms), speed-up %lu.%lux; "
            "%lu thread(s) stolen; ran on CPUs 0x%x\n", n, ms, (uint64_t)n * one,
            speedup10 / 10, speedup10 % 10, s1.steals - s0.steals, cpus_used);
    CHECK(all_done, "every thread finished");
    CHECK(ncpu == 1 || s1.steals > s0.steals, "idle CPUs stole work from the busy one");
    CHECK(ncpu == 1 || (uint32_t)__builtin_popcount(cpus_used) == ncpu,
          "the work spread over every CPU");
    CHECK(sched_check(), "every CPU's queues are consistent");

    if (ncpu > 1) {
        /* Kill a thread while it runs on another CPU. */
        victim_running = false;
        unsigned target = cpu_id() == 1 ? 0 : 1;
        struct thread *v = thread_create_on(process_kernel(), "victim", victim_entry, NULL,
                                            target, NULL);
        for (unsigned tries = 0; tries < 200 && !victim_running; tries++)
            sched_sleep(1);
        tid_t tid = v != NULL ? v->tid : 0;
        bool killed = v != NULL && victim_running && thread_terminate(v) == PROC_OK;
        bool gone = false;
        for (unsigned tries = 0; tries < 200 && killed && !gone; tries++) {
            uint64_t f = rcu_read_lock();
            struct thread *t = thread_find(tid);
            gone = t == NULL || t->state == TASK_TERMINATED;
            rcu_read_unlock(f);
            if (!gone)
                sched_sleep(1);
        }
        CHECK(killed && gone, "a thread running on another CPU is stopped by thread_terminate");
    }
}

/* ---- 8. dashboard --------------------------------------------------------- */

static void dashboard(const char *title)
{
    box_top();
    box_line("BHARAT OS - %u CPU DASHBOARD  (%s)", ncpu, title);
    box_separator();
    box_line("%-4s %-10s %9s %8s %7s %7s %6s", "CPU", "RUNNING", "SWITCHES", "STOLEN", "IPIS",
             "QUEUED", "IDLE%");
    uint64_t ticks = sched_ticks();
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        if (!cpu_ok(c))
            continue;
        struct sched_stats s;
        sched_get_cpu_stats(c, &s);
        struct thread *cur = cpu_at(c)->current;
        box_line("%-4u %-10s %9lu %8lu %7lu %7u %5lu%%", c, cur != NULL ? cur->name : "?",
                 s.switches, s.steals, cpu_at(c)->ipis_received, s.runnable,
                 ticks ? s.idle_ticks * 100 / ticks : 0);
    }
    box_bottom();
}

static void test_dashboard(void)
{
    section("Multi-core dashboard");
    unsigned n = ncpu * BUSY_THREADS_PER_CPU;
    for (unsigned i = 0; i < n; i++) {
        memset(&busy[i], 0, sizeof(busy[i]));
        busy[i].amount = BUSY_WORK * 2;
        if (thread_create(process_kernel(), "worker", busy_entry, &busy[i], NULL) == NULL)
            busy[i].done = true;
    }
    for (int frame = 0; frame < 3; frame++) {
        sched_sleep(SCHED_TIMER_HZ / 10);
        dashboard(frame == 0 ? "work starting" : frame == 1 ? "all CPUs busy" : "settling");
    }
    for (unsigned tries = 0; tries < 3000; tries++) {
        bool all = true;
        for (unsigned i = 0; i < n; i++)
            all &= busy[i].done;
        if (all)
            break;
        sched_sleep(1);
    }
    CHECK(sched_check(), "queues consistent after the dashboard run");
}

/* ---- entry ---------------------------------------------------------------- */

bool smp_run_tests(void)
{
    kprintf("\n========== Phase 9: Multi-core ==========\n");
    passed = failed = 0;
    test_cpus();
    test_counter();
    test_allocators();
    test_lock_order();
    test_shootdown();
    test_rcu();
    test_stealing();
    test_dashboard();
    sched_set_stealing(false);       /* later demos and benches expect one CPU */
    kprintf("\n===== Multi-core tests: %u passed, %u failed =====\n", passed, failed);
    return failed == 0;
}
