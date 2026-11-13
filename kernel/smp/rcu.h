#ifndef BHARAT_RCU_H
#define BHARAT_RCU_H

/* RCU ("read, copy, update") for tables that are read far more often
 * than they change, such as the process and thread lookup tables.
 *
 * Readers take no lock at all: they just keep interrupts off while they
 * look (so their CPU cannot switch threads):
 *
 *     uint64_t f = rcu_read_lock();
 *     p = rcu_dereference(table[i]);  ... use p ...
 *     rcu_read_unlock(f);
 *
 * A writer (holding the table's normal lock) removes or replaces an entry
 * with rcu_assign_pointer, then calls rcu_synchronize() before freeing
 * the old object. rcu_synchronize waits until every other CPU has taken
 * an interrupt with interrupts on, which proves it finished any read
 * section that might still have seen the old pointer (a "grace period").
 * We get there by sending each CPU an IPI and waiting for the answer.
 *
 * Any code that runs with interrupts off (including while holding a
 * spinlock) counts as a reader too.
 *
 * Limits: rcu_synchronize must be called with interrupts on, no spinlock
 * held and outside any read section; it waits for every CPU (no batching
 * of grace periods). */

#include <stdint.h>
#include "cpu.h"
#include "percpu.h"

static inline uint64_t rcu_read_lock(void)
{
    uint64_t f = irq_save();
    this_cpu()->rcu_depth++;
    return f;
}

static inline void rcu_read_unlock(uint64_t flags)
{
    this_cpu()->rcu_depth--;
    irq_restore(flags);
}

#define rcu_dereference(p)       __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define rcu_assign_pointer(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

/* Wait for a grace period (see above). */
void rcu_synchronize(void);

/* Grace periods completed so far. */
uint64_t rcu_grace_periods(void);

#endif /* BHARAT_RCU_H */
