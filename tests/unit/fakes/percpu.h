#ifndef BHARAT_PERCPU_H
#define BHARAT_PERCPU_H

/* Host stand-in for kernel/smp/percpu.h: one CPU (number 0), whose record
 * is the plain array entry unit_cpus[0] instead of a GS-based lookup. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SMP_MAX_CPUS   16
#define LOCK_MAX_HELD  16

struct thread;
struct address_space;

struct percpu {
    struct percpu *self;
    uint32_t id;
    uint32_t lapic_id;
    uint64_t boot_stack;
    struct thread *current;
    struct thread *idle;
    struct address_space *space;
    volatile uint32_t online;
    uint32_t held_count;
    uint32_t rcu_depth;
    uint64_t ipis_received;
    uint64_t calls_served;
};

extern struct percpu unit_cpus[SMP_MAX_CPUS];

static inline struct percpu *this_cpu(void) { return &unit_cpus[0]; }
static inline uint32_t cpu_id(void) { return 0; }
static inline struct thread *percpu_current(void) { return unit_cpus[0].current; }
static inline struct percpu *cpu_at(unsigned id) { return id < SMP_MAX_CPUS ? &unit_cpus[id] : NULL; }
static inline uint32_t smp_online_mask(void) { return 1; }
static inline unsigned smp_cpu_count(void) { return 1; }

#endif /* BHARAT_PERCPU_H */
