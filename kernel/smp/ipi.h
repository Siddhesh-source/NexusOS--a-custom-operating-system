#ifndef BHARAT_IPI_H
#define BHARAT_IPI_H

/* Messages between CPUs (inter-processor interrupts, IPIs).
 *
 *   ipi_reschedule(cpu)    "look at your run queue": a thread was queued
 *                          there, or a thread running there was killed
 *   smp_call(mask, fn)     run fn on every CPU in `mask` (except this one)
 *                          and wait until all have done it; used for TLB
 *                          shootdown and RCU grace periods
 *   smp_halt_others()      stop every other CPU (panic)
 *
 * smp_call is deadlock-free even when the caller holds spinlocks: every
 * CPU that waits with interrupts off (for a spinlock, or for its own
 * smp_call) keeps answering requests from inside its waiting loop
 * (cpu_relax). Requests marked `quiescent` (RCU) are only answered from
 * the interrupt itself, i.e. when the CPU had interrupts on, which is
 * exactly what proves it is not inside an RCU read section.
 *
 * Limits: one request in flight per sending CPU; xAPIC physical
 * destination mode; at most SMP_MAX_CPUS CPUs. */

#include <stdint.h>
#include <stdbool.h>

#define IPI_CALL_VECTOR        0xF0
#define IPI_RESCHEDULE_VECTOR  0xF1

typedef void (*smp_call_fn)(void *arg);

/* Install the IPI handlers (once, on the boot CPU). */
void ipi_init(void);

void ipi_reschedule(unsigned cpu);

/* Run fn(arg) on every online CPU in `mask` other than this one; return
 * when all have finished. `quiescent` requests count only once the target
 * took the interrupt (RCU). fn may be NULL (just wait for the answer). */
void smp_call(uint32_t mask, smp_call_fn fn, void *arg, bool quiescent);

/* Spin-wait helper: `pause`, and answer pending requests from other CPUs.
 * Use it in every loop that waits for another CPU. */
void cpu_relax(void);

/* Stop all other CPUs (NMI). Used by panic; does not wait. */
void smp_halt_others(void);

struct ipi_stats {
    uint64_t calls_sent;        /* smp_call requests from this CPU */
    uint64_t reschedules_sent;
};
void ipi_get_stats(struct ipi_stats *out);

#endif /* BHARAT_IPI_H */
