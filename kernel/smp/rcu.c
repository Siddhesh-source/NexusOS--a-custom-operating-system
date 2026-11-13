#include "rcu.h"
#include "ipi.h"
#include "panic.h"

static volatile uint64_t grace_periods;

void rcu_synchronize(void)
{
    if (!interrupts_enabled() || this_cpu()->held_count > 0 || this_cpu()->rcu_depth > 0)
        panic("rcu_synchronize called with interrupts off, a lock held or inside an RCU read section");
    /* One CPU: we are not reading (interrupts are on), and no other
     * thread can be in the middle of a read (readers are not preempted). */
    smp_call(smp_online_mask(), 0, 0, true);
    __atomic_fetch_add(&grace_periods, 1, __ATOMIC_RELAXED);
}

uint64_t rcu_grace_periods(void)
{
    return grace_periods;
}
