/* Kernel micro-benchmarks, built only into the bench kernel
 * (`python build.py bench` defines BHARAT_BENCH). Each prints
 *     [bench] <name>: <cycles> cycles/op
 * which build.py reads and records in docs/benchmarks.md.
 *
 * Times are CPU time stamp counter (TSC) cycles under QEMU, so they are
 * for comparing BharatOS with itself over time, not with real hardware.
 * Each result is the best of several rounds, which filters out rounds hit
 * by a timer interrupt. Heap numbers include the heap poisoning checks. */

#include "bench.h"

#ifdef BHARAT_BENCH

#include "cpu.h"
#include "heap.h"
#include "pmm.h"
#include "slab.h"
#include "kprintf.h"
#include "proc.h"
#include "sched.h"
#include "trace.h"
#include "percpu.h"

#define ROUNDS 5

static uint64_t best(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

/* ---- context switch ----------------------------------------------------- */

#define SWITCH_ITERS 2000

static volatile bool partner_stop;
static volatile bool partner_done;

static int64_t switch_partner(void *arg)
{
    (void)arg;
    while (!partner_stop)
        sched_yield();
    partner_done = true;
    return 0;
}

/* The boot thread and one partner yield to each other: every yield is one
 * full thread switch (scheduler decision + register save/restore). */
static void bench_context_switch(void)
{
    proc_status_t err;
    partner_stop = partner_done = false;
    if (thread_create(process_kernel(), "bench-partner", switch_partner, NULL, &err) == NULL) {
        kprintf("[bench] cannot create partner thread: %s\n", proc_status_str(err));
        return;
    }
    sched_yield();                         /* let the partner start */

    uint64_t min = ~0ULL;
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t t0 = rdtsc();
        for (int i = 0; i < SWITCH_ITERS; i++)
            sched_yield();                 /* there and back: 2 switches */
        min = best(min, (rdtsc() - t0) / (2 * SWITCH_ITERS));
    }
    partner_stop = true;
    while (!partner_done)
        sched_yield();
    kprintf("[bench] context_switch: %lu cycles/op\n", min);
}

/* ---- context switch on every CPU at once --------------------------------- */

/* Per CPU, two threads pinned there yield to each other, all CPUs at the
 * same time. Each CPU has its own run queue and lock, so the cost per
 * switch should stay close to the one-CPU number: that is what "scales
 * per CPU" means. Reported: the average over CPUs. */
struct pair {
    volatile bool stop, done_a, done_b;
    volatile uint64_t cycles;
};

static struct pair pairs[SMP_MAX_CPUS];

static int64_t pair_partner(void *arg)
{
    struct pair *p = arg;
    while (!p->stop)
        sched_yield();
    p->done_b = true;
    return 0;
}

static int64_t pair_timer(void *arg)
{
    struct pair *p = arg;
    sched_yield();                         /* let the partner start */
    uint64_t min = ~0ULL;
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t t0 = rdtsc();
        for (int i = 0; i < SWITCH_ITERS; i++)
            sched_yield();
        min = best(min, (rdtsc() - t0) / (2 * SWITCH_ITERS));
    }
    p->cycles = min;
    p->stop = true;
    p->done_a = true;
    return 0;
}

static void bench_context_switch_parallel(void)
{
    unsigned n = 0;
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        struct pair *p = &pairs[c];
        p->stop = false;
        p->done_a = p->done_b = true;
        p->cycles = 0;
        if (!cpu_at(c)->online)
            continue;
        p->done_a = p->done_b = false;
        if (thread_create_on(process_kernel(), "bench-b", pair_partner, p, c, NULL) == NULL
            || thread_create_on(process_kernel(), "bench-a", pair_timer, p, c, NULL) == NULL) {
            kprintf("[bench] cannot create the threads for CPU %u\n", c);
            p->stop = p->done_a = p->done_b = true;
            continue;
        }
        n++;
    }
    for (;;) {
        bool all = true;
        for (unsigned c = 0; c < SMP_MAX_CPUS; c++)
            all &= pairs[c].done_a && pairs[c].done_b;
        if (all)
            break;
        sched_sleep(1);
    }
    uint64_t sum = 0, worst = 0;
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        sum += pairs[c].cycles;
        worst = pairs[c].cycles > worst ? pairs[c].cycles : worst;
    }
    kprintf("[bench] %u CPUs switching at once: average %lu, slowest CPU %lu cycles/switch\n",
            n, n ? sum / n : 0, worst);
    kprintf("[bench] context_switch_par: %lu cycles/op\n", n ? sum / n : 0);
}

/* ---- heap --------------------------------------------------------------- */

#define HEAP_ITERS 5000

static void bench_heap(const char *name, size_t size)
{
    uint64_t min = ~0ULL;
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t t0 = rdtsc();
        for (int i = 0; i < HEAP_ITERS; i++) {
            void *p = kmalloc(size);
            if (p == NULL) {
                kprintf("[bench] %s: allocation failed\n", name);
                return;
            }
            kfree(p);
        }
        min = best(min, (rdtsc() - t0) / HEAP_ITERS);
    }
    kprintf("[bench] %s: %lu cycles/op\n", name, min);
}

/* ---- page allocator ----------------------------------------------------- */

#define PMM_BATCH 256

/* Allocate a batch of pages, then free them all: the pattern of a process
 * starting and exiting. Cost per page (alloc + free). */
static void bench_pmm(void)
{
    static uint64_t pages[PMM_BATCH];
    uint64_t min = ~0ULL;
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t t0 = rdtsc();
        for (int i = 0; i < PMM_BATCH; i++)
            pages[i] = pmm_alloc_page();
        for (int i = 0; i < PMM_BATCH; i++)
            pmm_free_page(pages[i]);
        min = best(min, (rdtsc() - t0) / PMM_BATCH);
    }
    kprintf("[bench] pmm_page: %lu cycles/op\n", min);
}

/* ---- slab --------------------------------------------------------------- */

/* Same object size as a thread record (176 bytes): slab cache versus the
 * general heap. */
static void bench_slab(void)
{
    struct slab_cache *c = slab_create("bench176", 176, 16, 0);
    if (c == NULL)
        return;
    uint64_t min = ~0ULL;
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t t0 = rdtsc();
        for (int i = 0; i < HEAP_ITERS; i++)
            slab_free(c, slab_alloc(c));
        min = best(min, (rdtsc() - t0) / HEAP_ITERS);
    }
    kprintf("[bench] slab_176: %lu cycles/op\n", min);
}

/* ---- trace -------------------------------------------------------------- */

#define TRACE_ITERS 20000

static void bench_trace(void)
{
    uint64_t min = ~0ULL;
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t t0 = rdtsc();
        for (int i = 0; i < TRACE_ITERS; i++)
            trace_event(TRACE_TEST, (uint64_t)i, 0);
        min = best(min, (rdtsc() - t0) / TRACE_ITERS);
    }
    kprintf("[bench] trace_event: %lu cycles/op\n", min);
}

void bench_run(void)
{
    kprintf("\n===== Benchmarks (TSC cycles, best of %d rounds) =====\n", ROUNDS);
    bench_context_switch();
    bench_context_switch_parallel();
    bench_heap("kmalloc_kfree_64", 64);
    bench_heap("kmalloc_kfree_4096", 4096);
    bench_pmm();
    bench_heap("kmalloc_kfree_176", 176);
    bench_slab();
    bench_trace();
    kprintf("[bench] done\n");
}

#else

void bench_run(void)
{
}

#endif /* BHARAT_BENCH */
