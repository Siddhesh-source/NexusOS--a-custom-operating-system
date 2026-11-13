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
