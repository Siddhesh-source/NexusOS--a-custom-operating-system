#ifndef BHARAT_SPINLOCK_H
#define BHARAT_SPINLOCK_H

/* Spinlocks with names, and a checker for the order they are taken in.
 *
 * A spinlock protects data shared between CPUs: a CPU that finds it taken
 * waits in a loop ("spins") until the holder lets go. Interrupts stay off
 * while a lock is held, so the holder is never preempted (or interrupted
 * by code that wants the same lock) and holds it only briefly.
 *
 *     SPIN_GUARD(&heap_lock);    // interrupts off + lock, until scope end
 *
 * Every lock has a class (heap, page tables, run queue, ...). The
 * lock-order checker watches which classes are taken while others are
 * held. The first time class B is taken while A is held, it records
 * "A before B" with a call trace. If B is later held while taking A, two
 * CPUs could each hold one and wait forever for the other (a deadlock),
 * so the checker reports both call traces and panics, even if the
 * deadlock never actually happened in this run. It also catches a CPU
 * taking a lock it already holds, and releasing one it does not hold.
 *
 * Rules:
 *   - spin_lock / spin_trylock need interrupts off (spin_lock_irqsave
 *     and SPIN_GUARD do that for you);
 *   - never sleep or yield while holding a spinlock;
 *   - spin_trylock never waits, so it cannot deadlock and is not
 *     order-checked (it is still recorded as held).
 *
 * Limits: LOCK_MAX_HELD locks at once per CPU; at most 32 classes. */

#include <stdint.h>
#include <stdbool.h>
#include "cpu.h"

enum lock_class {
    LOCK_CLASS_NONE = 0,
    LOCK_CLASS_RUNQUEUE,    /* per-CPU scheduler queues and thread states */
    LOCK_CLASS_PROC,        /* process/thread tables and thread lists */
    LOCK_CLASS_VM,          /* memory objects and regions */
    LOCK_CLASS_KSTACK,      /* kernel stack slots */
    LOCK_CLASS_HEAP,        /* kmalloc */
    LOCK_CLASS_SLAB,        /* one per slab cache */
    LOCK_CLASS_VMM,         /* page tables */
    LOCK_CLASS_PMM,         /* physical pages */
    LOCK_CLASS_TRACE,       /* event ring */
    LOCK_CLASS_IOAPIC,
    LOCK_CLASS_PCI,
    LOCK_CLASS_TEST,        /* self-tests */
    LOCK_CLASS_TEST_A,      /* self-tests of the order checker */
    LOCK_CLASS_TEST_B,
    LOCK_CLASS_WAITQ,       /* wait queues (before run queues) */
    LOCK_CLASS_INPUT,       /* keyboard/mouse event queue */
    LOCK_CLASS_FS,          /* the in-memory file system */
    LOCK_CLASS_DEMO_A,      /* the shell's lock-order demo */
    LOCK_CLASS_DEMO_B,
    LOCK_CLASS_COUNT
};

_Static_assert(LOCK_CLASS_COUNT <= 32, "the order checker keeps classes in 32-bit masks");

struct spinlock {
    volatile uint32_t locked;
    uint16_t lock_class;
    volatile uint16_t owner_cpu;    /* LOCK_NO_OWNER when free */
};

#define LOCK_NO_OWNER 0xFFFF
#define SPINLOCK_INIT(cls) { .locked = 0, .lock_class = (cls), .owner_cpu = LOCK_NO_OWNER }

void spin_lock_init(struct spinlock *l, uint16_t lock_class);

void spin_lock(struct spinlock *l);
bool spin_trylock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

void spin_lock_irqsave(struct spinlock *l, uint64_t *flags);
void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags);

/* Does this CPU hold `l`? How many locks does it hold? */
bool spin_held(const struct spinlock *l);
unsigned spin_held_count(void);

const char *lock_class_name(unsigned lock_class);

/* ---- scoped guard -------------------------------------------------------- */

struct spin_guard {
    struct spinlock *lock;
    uint64_t flags;
};

static inline struct spin_guard spin_guard_enter(struct spinlock *l)
{
    struct spin_guard g = { .lock = l, .flags = 0 };
    spin_lock_irqsave(l, &g.flags);
    return g;
}

static inline void spin_guard_exit(struct spin_guard *g)
{
    spin_unlock_irqrestore(g->lock, g->flags);
}

#define SPIN_GUARD(l) \
    struct spin_guard IRQ_GUARD_CONCAT(spin_guard_, __LINE__) \
        __attribute__((cleanup(spin_guard_exit), unused)) = spin_guard_enter(l)

/* ---- lock-order checker -------------------------------------------------- */

/* Fatal (the default): an order problem panics. Non-fatal (self-tests
 * only): it is reported and counted. Returns the previous setting. */
bool lockcheck_set_fatal(bool fatal);

/* Order problems found so far (non-fatal mode). */
uint64_t lockcheck_violations(void);

/* Has "a before b" been recorded? */
bool lockcheck_order_known(unsigned a, unsigned b);

/* Forget everything recorded about one class (self-tests only). */
void lockcheck_forget(unsigned lock_class);

/* Number of distinct "A before B" orders recorded. */
unsigned lockcheck_order_count(void);

#endif /* BHARAT_SPINLOCK_H */
