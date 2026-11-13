#include "ipi.h"
#include "percpu.h"
#include "apic.h"
#include "interrupts.h"
#include "sched.h"
#include "cpu.h"
#include "panic.h"

/* One mailbox per sending CPU. The sender fills fn/arg, then sets one
 * `pending` bit per target; each target clears its bit when done. A CPU
 * sends one request at a time (it waits for the answers), so its mailbox
 * is never overwritten while in use, and two CPUs can send at once. */
struct call_slot {
    smp_call_fn fn;
    void *arg;
    bool quiescent;
    volatile uint32_t pending;
};

static struct call_slot slots[SMP_MAX_CPUS];
static volatile uint64_t calls_sent, reschedules_sent;

#define ICR_NMI          (4u << 8)
#define ICR_ALL_BUT_SELF (3u << 18)

/* Run every request addressed to this CPU. From a waiting loop
 * (`from_irq` false) quiescent requests are left for the interrupt: a CPU
 * spinning with interrupts off might be inside an RCU read section. */
static void serve(bool from_irq)
{
    struct percpu *c = this_cpu();
    uint32_t me = 1u << c->id;
    for (unsigned s = 0; s < SMP_MAX_CPUS; s++) {
        struct call_slot *sl = &slots[s];
        if (!(__atomic_load_n(&sl->pending, __ATOMIC_ACQUIRE) & me))
            continue;
        if (sl->quiescent && !from_irq)
            continue;
        if (sl->fn != 0)
            sl->fn(sl->arg);
        c->calls_served++;
        __atomic_fetch_and(&sl->pending, ~me, __ATOMIC_RELEASE);
    }
}

void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
    if (smp_online_mask() & ~1u)
        serve(false);
}

static void call_interrupt(struct interrupt_frame *f)
{
    (void)f;
    this_cpu()->ipis_received++;
    lapic_eoi();
    serve(true);
}

static void reschedule_interrupt(struct interrupt_frame *f)
{
    (void)f;
    this_cpu()->ipis_received++;
    lapic_eoi();
    sched_ipi();
}

void ipi_init(void)
{
    interrupt_set_handler(IPI_CALL_VECTOR, call_interrupt);
    interrupt_set_handler(IPI_RESCHEDULE_VECTOR, reschedule_interrupt);
}

void ipi_reschedule(unsigned cpu)
{
    struct percpu *c = cpu_at(cpu);
    if (c == 0 || !c->online || cpu == cpu_id())
        return;
    __atomic_fetch_add(&reschedules_sent, 1, __ATOMIC_RELAXED);
    lapic_send_ipi(c->lapic_id, IPI_RESCHEDULE_VECTOR);
}

void smp_call(uint32_t mask, smp_call_fn fn, void *arg, bool quiescent)
{
    uint64_t flags = irq_save();     /* no preemption while our mailbox is busy */
    struct percpu *c = this_cpu();
    mask &= smp_online_mask() & ~(1u << c->id);
    if (mask == 0) {
        irq_restore(flags);
        return;
    }
    if (quiescent && (c->held_count > 0 || c->rcu_depth > 0 || !(flags & (1ULL << 9))))
        panic("rcu_synchronize called with interrupts off, a lock held or inside an RCU read section");

    struct call_slot *sl = &slots[c->id];
    sl->fn = fn;
    sl->arg = arg;
    sl->quiescent = quiescent;
    __atomic_store_n(&sl->pending, mask, __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < SMP_MAX_CPUS; i++)
        if (mask & (1u << i))
            lapic_send_ipi(cpu_at(i)->lapic_id, IPI_CALL_VECTOR);

    /* Answer others while we wait: they may be waiting for us. */
    while (__atomic_load_n(&sl->pending, __ATOMIC_ACQUIRE) != 0) {
        __asm__ volatile("pause" ::: "memory");
        serve(false);
    }
    __atomic_fetch_add(&calls_sent, 1, __ATOMIC_RELAXED);
    irq_restore(flags);
}

void smp_halt_others(void)
{
    if (smp_online_mask() & ~(1u << cpu_id()))
        lapic_send_ipi(0, ICR_ALL_BUT_SELF | ICR_NMI);
}

void ipi_get_stats(struct ipi_stats *out)
{
    out->calls_sent = calls_sent;
    out->reschedules_sent = reschedules_sent;
}
