#include "trace.h"
#include "cpu.h"
#include "kprintf.h"
#include "percpu.h"

_Static_assert((TRACE_CAPACITY & (TRACE_CAPACITY - 1)) == 0,
               "TRACE_CAPACITY must be a power of two");

/* Lock-free: a writer claims the next slot with one atomic add, fills
 * it, and writes `seq` last; a reader trusts a slot only when its `seq`
 * is the one it expects. Every CPU records on every context switch, so a
 * shared lock here would make all CPUs wait for each other. */
static struct trace_event ring[TRACE_CAPACITY];
static volatile uint64_t total; /* events claimed; next slot is total % cap */
static volatile bool enabled = true;

static const char *const type_names[TRACE_TYPE_COUNT] = {
    [TRACE_NONE]          = "none",
    [TRACE_SCHED_SWITCH]  = "sched.switch",
    [TRACE_SCHED_PREEMPT] = "sched.preempt",
    [TRACE_THREAD_CREATE] = "thread.create",
    [TRACE_THREAD_EXIT]   = "thread.exit",
    [TRACE_PAGE_FAULT]    = "page.fault",
    [TRACE_HEAP_GROW]     = "heap.grow",
    [TRACE_TEST]          = "test",
    [TRACE_ALARM_SET]     = "alarm.set",
    [TRACE_ALARM_FIRE]    = "alarm.fire",
    [TRACE_SCHED_STEAL]   = "sched.steal",
};

const char *trace_type_name(trace_type_t type)
{
    return (unsigned)type < TRACE_TYPE_COUNT ? type_names[type] : "unknown";
}

void trace_event(trace_type_t type, uint64_t a, uint64_t b)
{
    if (!enabled)
        return;
    uint64_t f = irq_save();         /* cpu_id() must match this CPU */
    uint64_t n = __atomic_add_fetch(&total, 1, __ATOMIC_RELAXED);
    struct trace_event *e = &ring[(n - 1) & (TRACE_CAPACITY - 1)];
    __atomic_store_n(&e->seq, 0, __ATOMIC_RELAXED);    /* being written */
    e->tsc = rdtsc();
    e->type = type;
    e->cpu = cpu_id();
    e->a = a;
    e->b = b;
    __atomic_store_n(&e->seq, n, __ATOMIC_RELEASE);
    irq_restore(f);
}

/* Copy event number `n` (1-based) if it is complete and not yet
 * overwritten. */
static bool read_event(uint64_t n, struct trace_event *out)
{
    const struct trace_event *e = &ring[(n - 1) & (TRACE_CAPACITY - 1)];
    if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != n)
        return false;
    *out = *e;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&e->seq, __ATOMIC_RELAXED) == n;
}

unsigned trace_snapshot(struct trace_event *out, unsigned max)
{
    uint64_t end = __atomic_load_n(&total, __ATOMIC_ACQUIRE);
    uint64_t have = end < TRACE_CAPACITY ? end : TRACE_CAPACITY;
    if (max > have)
        max = (unsigned)have;
    unsigned got = 0;
    for (uint64_t n = end - max + 1; n <= end; n++)
        if (read_event(n, &out[got]))
            got++;                    /* skips events still being written */
    return got;
}

uint64_t trace_total(void)
{
    return __atomic_load_n(&total, __ATOMIC_ACQUIRE);
}

bool trace_set_enabled(bool on)
{
    bool was = enabled;
    enabled = on;
    return was;
}

void trace_dump(unsigned count)
{
    /* Reads in place rather than copying to the stack: this runs during a
     * panic, possibly on a nearly full stack. */
    uint64_t end = __atomic_load_n(&total, __ATOMIC_ACQUIRE);
    uint64_t have = end < TRACE_CAPACITY ? end : TRACE_CAPACITY;
    if (count > have)
        count = (unsigned)have;
    kprintf("Recent events (%u of %lu, oldest first):\n", count, end);
    if (count == 0)
        return;
    uint64_t base = 0;
    for (uint64_t n = end - count + 1; n <= end; n++) {
        struct trace_event e;
        if (!read_event(n, &e))
            continue;                 /* being written, or overwritten */
        if (base == 0)
            base = e.tsc;
        kprintf("  [%lu] +%lu cycles  cpu%u %s a=0x%lx b=0x%lx\n",
                e.seq, e.tsc - base, e.cpu, trace_type_name((trace_type_t)e.type), e.a, e.b);
    }
}
