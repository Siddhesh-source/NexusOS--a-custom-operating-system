#include "spinlock.h"
#include "percpu.h"
#include "ipi.h"
#include "backtrace.h"
#include "kprintf.h"
#include "panic.h"

static const char *const class_names[LOCK_CLASS_COUNT] = {
    [LOCK_CLASS_NONE]     = "none",
    [LOCK_CLASS_RUNQUEUE] = "run queue",
    [LOCK_CLASS_PROC]     = "process table",
    [LOCK_CLASS_VM]       = "memory regions",
    [LOCK_CLASS_KSTACK]   = "kernel stacks",
    [LOCK_CLASS_HEAP]     = "heap",
    [LOCK_CLASS_SLAB]     = "slab cache",
    [LOCK_CLASS_VMM]      = "page tables",
    [LOCK_CLASS_PMM]      = "physical pages",
    [LOCK_CLASS_TRACE]    = "trace ring",
    [LOCK_CLASS_IOAPIC]   = "I/O APIC",
    [LOCK_CLASS_PCI]      = "PCI config",
    [LOCK_CLASS_TEST]     = "test",
    [LOCK_CLASS_TEST_A]   = "test A",
    [LOCK_CLASS_TEST_B]   = "test B",
};

const char *lock_class_name(unsigned c)
{
    return c < LOCK_CLASS_COUNT && class_names[c] != 0 ? class_names[c] : "?";
}

/* ---- order checker --------------------------------------------------------
 * before[a] has bit b set once a lock of class b was taken while one of
 * class a was held: "a before b". Taking a while holding b after that is
 * the opposite order. For the report, the call trace of the first time
 * each order was seen is kept (a small table; once full, orders are
 * still checked, just reported without the earlier trace). */

#define TRACE_DEPTH 8
#define MAX_RECORDS 64

struct order_record {
    uint16_t first, second;
    uint16_t cpu;
    uint16_t depth;
    uint64_t trace[TRACE_DEPTH];
};

static uint32_t before[LOCK_CLASS_COUNT];
static struct order_record records[MAX_RECORDS];
static unsigned record_count;
static volatile uint32_t records_busy;       /* tiny lock for the table */
static volatile bool fatal = true;
static volatile uint64_t violations;

static void records_lock(void)
{
    while (__atomic_exchange_n(&records_busy, 1, __ATOMIC_ACQUIRE))
        cpu_relax();
}

static void records_unlock(void)
{
    __atomic_store_n(&records_busy, 0, __ATOMIC_RELEASE);
}

static unsigned collect_here(uint64_t *out)
{
    return backtrace_collect((uint64_t)__builtin_frame_address(0), out, TRACE_DEPTH);
}

static void record_order(unsigned a, unsigned b)
{
    uint64_t trace[TRACE_DEPTH];
    unsigned depth = collect_here(trace);
    records_lock();
    if (!(__atomic_load_n(&before[a], __ATOMIC_RELAXED) & (1u << b))) {
        if (record_count < MAX_RECORDS) {
            struct order_record *r = &records[record_count++];
            r->first = (uint16_t)a;
            r->second = (uint16_t)b;
            r->cpu = (uint16_t)cpu_id();
            r->depth = (uint16_t)depth;
            for (unsigned i = 0; i < depth; i++)
                r->trace[i] = trace[i];
        }
        __atomic_fetch_or(&before[a], 1u << b, __ATOMIC_SEQ_CST);
    }
    records_unlock();
}

static const struct order_record *find_record(unsigned a, unsigned b)
{
    for (unsigned i = 0; i < record_count; i++)
        if (records[i].first == a && records[i].second == b)
            return &records[i];
    return 0;
}

/* Report a problem; panics unless a self-test switched to non-fatal. */
static void report(const char *what, const struct spinlock *taking,
                   const struct spinlock *holding)
{
    unsigned b = taking->lock_class, a = holding->lock_class;
    kprintf("\n[lockcheck] %s on CPU %u:\n", what, cpu_id());
    kprintf("[lockcheck]   taking \"%s\" while holding \"%s\"\n",
            lock_class_name(b), lock_class_name(a));
    if (a != b) {
        kprintf("[lockcheck]   but earlier \"%s\" was held while taking \"%s\".\n",
                lock_class_name(b), lock_class_name(a));
        kprintf("[lockcheck]   Two CPUs doing these at the same time would each wait\n"
                "[lockcheck]   for the other forever (a deadlock).\n");
        records_lock();
        const struct order_record *r = find_record(b, a);
        if (r != 0) {
            kprintf("[lockcheck] Earlier order (\"%s\" then \"%s\", CPU %u):\n",
                    lock_class_name(b), lock_class_name(a), r->cpu);
            backtrace_print_list(r->trace, r->depth);
        }
        records_unlock();
    }
    kprintf("[lockcheck] Now:\n");
    uint64_t trace[TRACE_DEPTH];
    backtrace_print_list(trace, collect_here(trace));
    if (fatal)
        panic("lock order problem: two locks are taken in opposite orders");
    __atomic_fetch_add(&violations, 1, __ATOMIC_SEQ_CST);
}

static void check_order(struct percpu *c, const struct spinlock *l)
{
    unsigned b = l->lock_class;
    for (unsigned i = 0; i < c->held_count; i++) {
        const struct spinlock *h = c->held[i];
        unsigned a = h->lock_class;
        if (a == b) {
            report("Two locks of the same class held", l, h);
        } else if (__atomic_load_n(&before[b], __ATOMIC_RELAXED) & (1u << a)) {
            report("Lock order problem", l, h);
        } else if (!(__atomic_load_n(&before[a], __ATOMIC_RELAXED) & (1u << b))) {
            record_order(a, b);
        }
    }
}

bool lockcheck_set_fatal(bool on)
{
    bool was = fatal;
    fatal = on;
    return was;
}

uint64_t lockcheck_violations(void)
{
    return violations;
}

bool lockcheck_order_known(unsigned a, unsigned b)
{
    return a < LOCK_CLASS_COUNT && b < LOCK_CLASS_COUNT && (before[a] & (1u << b));
}

void lockcheck_forget(unsigned cls)
{
    if (cls >= LOCK_CLASS_COUNT)
        return;
    records_lock();
    __atomic_store_n(&before[cls], 0, __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < LOCK_CLASS_COUNT; i++)
        __atomic_fetch_and(&before[i], ~(1u << cls), __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < record_count; i++) {
        if (records[i].first == cls || records[i].second == cls) {
            records[i] = records[--record_count];
            i--;
        }
    }
    records_unlock();
}

unsigned lockcheck_order_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < LOCK_CLASS_COUNT; i++)
        n += (unsigned)__builtin_popcount(before[i]);
    return n;
}

/* ---- the locks themselves ------------------------------------------------- */

void spin_lock_init(struct spinlock *l, uint16_t cls)
{
    l->locked = 0;
    l->lock_class = cls;
    l->owner_cpu = LOCK_NO_OWNER;
}

static void push_held(struct percpu *c, struct spinlock *l)
{
    if (c->held_count == LOCK_MAX_HELD)
        panic("spinlock: this CPU holds too many locks at once");
    c->held[c->held_count++] = l;
}

static void pop_held(struct percpu *c, struct spinlock *l)
{
    for (unsigned i = c->held_count; i-- > 0;) {
        if (c->held[i] == l) {
            for (unsigned j = i; j + 1 < c->held_count; j++)
                c->held[j] = c->held[j + 1];
            c->held_count--;
            return;
        }
    }
    panic("spinlock: released a lock missing from this CPU's held list");
}

static void check_entry(struct percpu *c, const struct spinlock *l)
{
    if (l->locked && l->owner_cpu == c->id)
        panic("spinlock: this CPU already holds this lock (it would wait for itself forever)");
}

static void irqs_must_be_off(void)
{
    if (interrupts_enabled())
        panic("spin_lock with interrupts on: use spin_lock_irqsave or SPIN_GUARD");
}

/* The lock itself; interrupts are known to be off. */
static void lock_irqs_off(struct spinlock *l)
{
    if (kernel_panicking())
        return;                     /* other CPUs are stopped; just go */
    struct percpu *c = this_cpu();
    check_entry(c, l);
    if (c->held_count != 0)
        check_order(c, l);
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
            cpu_relax();
    }
    l->owner_cpu = (uint16_t)c->id;
    push_held(c, l);
}

void spin_lock(struct spinlock *l)
{
    irqs_must_be_off();
    lock_irqs_off(l);
}

bool spin_trylock(struct spinlock *l)
{
    if (kernel_panicking())
        return true;
    irqs_must_be_off();
    struct percpu *c = this_cpu();
    check_entry(c, l);
    if (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
        return false;
    l->owner_cpu = (uint16_t)c->id;
    push_held(c, l);
    return true;
}

void spin_unlock(struct spinlock *l)
{
    if (kernel_panicking())
        return;
    struct percpu *c = this_cpu();
    if (!l->locked || l->owner_cpu != c->id)
        panic("spinlock: released a lock this CPU does not hold");
    pop_held(c, l);
    l->owner_cpu = LOCK_NO_OWNER;
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

void spin_lock_irqsave(struct spinlock *l, uint64_t *flags)
{
    *flags = irq_save();
    lock_irqs_off(l);             /* just turned them off: no need to check */
}

void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags)
{
    spin_unlock(l);
    irq_restore(flags);
}

bool spin_held(const struct spinlock *l)
{
    return l->locked && l->owner_cpu == cpu_id();
}

unsigned spin_held_count(void)
{
    return this_cpu()->held_count;
}
