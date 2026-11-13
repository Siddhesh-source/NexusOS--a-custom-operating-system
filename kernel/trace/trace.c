#include "trace.h"
#include "cpu.h"
#include "kprintf.h"

_Static_assert((TRACE_CAPACITY & (TRACE_CAPACITY - 1)) == 0,
               "TRACE_CAPACITY must be a power of two");

static struct trace_event ring[TRACE_CAPACITY];
static uint64_t total;          /* events recorded; next slot is total % cap */
static bool enabled = true;

static const char *const type_names[TRACE_TYPE_COUNT] = {
    [TRACE_NONE]          = "none",
    [TRACE_SCHED_SWITCH]  = "sched.switch",
    [TRACE_SCHED_PREEMPT] = "sched.preempt",
    [TRACE_THREAD_CREATE] = "thread.create",
    [TRACE_THREAD_EXIT]   = "thread.exit",
    [TRACE_PAGE_FAULT]    = "page.fault",
    [TRACE_HEAP_GROW]     = "heap.grow",
    [TRACE_TEST]          = "test",
};

const char *trace_type_name(trace_type_t type)
{
    return (unsigned)type < TRACE_TYPE_COUNT ? type_names[type] : "unknown";
}

void trace_event(trace_type_t type, uint64_t a, uint64_t b)
{
    IRQ_GUARD();
    if (!enabled)
        return;
    struct trace_event *e = &ring[total & (TRACE_CAPACITY - 1)];
    total++;
    e->tsc = rdtsc();
    e->seq = total;
    e->type = type;
    e->a = a;
    e->b = b;
}

unsigned trace_snapshot(struct trace_event *out, unsigned max)
{
    IRQ_GUARD();
    uint64_t have = total < TRACE_CAPACITY ? total : TRACE_CAPACITY;
    if (max > have)
        max = (unsigned)have;
    for (unsigned i = 0; i < max; i++)
        out[i] = ring[(total - max + i) & (TRACE_CAPACITY - 1)];
    return max;
}

uint64_t trace_total(void)
{
    IRQ_GUARD();
    return total;
}

bool trace_set_enabled(bool on)
{
    IRQ_GUARD();
    bool was = enabled;
    enabled = on;
    return was;
}

void trace_dump(unsigned count)
{
    /* Read in place rather than copying to the stack: this runs during a
     * panic, possibly on a nearly full stack. Interrupts stay off so the
     * ring cannot change underneath. */
    IRQ_GUARD();
    uint64_t have = total < TRACE_CAPACITY ? total : TRACE_CAPACITY;
    if (count > have)
        count = (unsigned)have;
    kprintf("Recent events (%u of %lu, oldest first):\n", count, total);
    if (count == 0)
        return;
    uint64_t base = ring[(total - count) & (TRACE_CAPACITY - 1)].tsc;
    for (unsigned i = 0; i < count; i++) {
        const struct trace_event *e = &ring[(total - count + i) & (TRACE_CAPACITY - 1)];
        kprintf("  [%lu] +%lu cycles  %s a=0x%lx b=0x%lx\n",
                e->seq, e->tsc - base, trace_type_name((trace_type_t)e->type),
                e->a, e->b);
    }
}
