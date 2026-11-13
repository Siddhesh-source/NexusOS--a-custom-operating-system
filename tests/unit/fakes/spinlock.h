#ifndef BHARAT_SPINLOCK_H
#define BHARAT_SPINLOCK_H

/* Host stand-in for kernel/smp/spinlock.h. Unit-test programs run on one
 * host thread, so the locks do nothing; the lock classes and the guard
 * macro exist so the kernel files compile unchanged. */

#include <stdint.h>
#include <stdbool.h>
#include "cpu.h"

enum lock_class {
    LOCK_CLASS_NONE = 0,
    LOCK_CLASS_RUNQUEUE, LOCK_CLASS_PROC, LOCK_CLASS_VM, LOCK_CLASS_KSTACK,
    LOCK_CLASS_HEAP, LOCK_CLASS_SLAB, LOCK_CLASS_VMM, LOCK_CLASS_PMM,
    LOCK_CLASS_TRACE, LOCK_CLASS_IOAPIC, LOCK_CLASS_PCI, LOCK_CLASS_TEST,
    LOCK_CLASS_TEST_A, LOCK_CLASS_TEST_B, LOCK_CLASS_COUNT
};

struct spinlock {
    volatile uint32_t locked;
    uint16_t lock_class;
    volatile uint16_t owner_cpu;
};

#define LOCK_NO_OWNER 0xFFFF
#define SPINLOCK_INIT(cls) { .locked = 0, .lock_class = (cls), .owner_cpu = LOCK_NO_OWNER }

static inline void spin_lock_init(struct spinlock *l, uint16_t c) { l->lock_class = c; }
static inline void spin_lock(struct spinlock *l) { (void)l; }
static inline bool spin_trylock(struct spinlock *l) { (void)l; return true; }
static inline void spin_unlock(struct spinlock *l) { (void)l; }
static inline void spin_lock_irqsave(struct spinlock *l, uint64_t *f) { (void)l; *f = 0; }
static inline void spin_unlock_irqrestore(struct spinlock *l, uint64_t f) { (void)l; (void)f; }
static inline unsigned spin_held_count(void) { return 0; }

#define SPIN_GUARD(l) do { (void)(l); } while (0)

#endif /* BHARAT_SPINLOCK_H */
