#ifndef BHARAT_PERCPU_H
#define BHARAT_PERCPU_H

/* Per-CPU data.
 *
 * Every CPU has one `struct percpu`. The CPU's GS segment base points at
 * it, so `this_cpu()` is a single memory read, with no table lookup and
 * no lock:
 *
 *     this_cpu()->current      the thread running on this CPU
 *     cpu_id()                 0 for the boot CPU, then 1, 2, ...
 *
 * Only the owning CPU writes most fields. Other CPUs may read `current`
 * and `online` (for example to find where a thread runs).
 *
 * A thread that can be preempted may move to another CPU between two
 * reads, so code that uses the CPU number for more than one access must
 * run with interrupts off. Reading `current` is one instruction and is
 * always correct: whichever CPU runs it, the answer is the caller.
 *
 * Limits: at most SMP_MAX_CPUS CPUs (extra ones stay parked). */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SMP_MAX_CPUS   16
#define LOCK_MAX_HELD  16      /* spinlocks one CPU may hold at once */

struct thread;
struct spinlock;
struct address_space;

struct percpu {
    struct percpu *self;            /* gs:0, so this_cpu() is one load */
    uint32_t id;                    /* gs:8 */
    uint32_t lapic_id;              /* gs:12, local APIC id from ACPI/Limine */
    uint64_t boot_stack;            /* gs:16, stack top an AP starts on */
    struct thread *current;         /* gs:24 */
    struct thread *idle;            /* gs:32 */
    struct address_space *space;    /* page tables loaded in CR3 */
    volatile uint32_t online;       /* set once the CPU is running */

    /* Spinlocks held right now, innermost last (lock-order checker). */
    uint32_t held_count;
    struct spinlock *held[LOCK_MAX_HELD];
    uint32_t rcu_depth;             /* nesting of rcu_read_lock() */

    /* Counters for the dashboard. */
    uint64_t ipis_received;
    uint64_t calls_served;          /* requests from other CPUs run here */
};

_Static_assert(offsetof(struct percpu, id) == 8, "ap_entry.asm and cpu_id() rely on this");
_Static_assert(offsetof(struct percpu, boot_stack) == 16, "ap_entry.asm relies on this");
_Static_assert(offsetof(struct percpu, current) == 24, "thread_current() relies on this");

static inline struct percpu *this_cpu(void)
{
    struct percpu *p;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(p));
    return p;
}

static inline uint32_t cpu_id(void)
{
    uint32_t v;
    __asm__ volatile("movl %%gs:8, %0" : "=r"(v));
    return v;
}

static inline struct thread *percpu_current(void)
{
    struct thread *t;
    __asm__ volatile("movq %%gs:24, %0" : "=r"(t));
    return t;
}

/* Point GS at CPU 0's record. First thing in kernel_main. */
void percpu_init_boot(void);

/* Load this CPU's GS base (again: reloading the GS selector clears it). */
void percpu_load_gs(struct percpu *c);

/* CPU `id`'s record (any id below SMP_MAX_CPUS; check ->online). */
struct percpu *cpu_at(unsigned id);

/* Number of CPUs running, and a bit mask of them (bit n = CPU n). */
unsigned smp_cpu_count(void);
uint32_t smp_online_mask(void);
void smp_mark_online(struct percpu *c);

#endif /* BHARAT_PERCPU_H */
