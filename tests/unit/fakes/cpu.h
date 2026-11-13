#ifndef BHARAT_CPU_H
#define BHARAT_CPU_H

/* Host stand-in for kernel/arch/x86_64/cpu.h. Unit tests run as ordinary
 * programs in user mode, where cli/sti would fault, and on one thread, so
 * the interrupt guards do nothing. Found before the real header because
 * tests/unit/fakes is passed with -iquote. */

#include <stdint.h>
#include <stdbool.h>

#define IRQ_GUARD() do { } while (0)

static inline uint64_t irq_save(void) { return 0; }
static inline void irq_restore(uint64_t flags) { (void)flags; }
static inline bool interrupts_enabled(void) { return false; }

/* A counter rather than the real TSC: deterministic timestamps. */
extern uint64_t unit_fake_tsc;
static inline uint64_t rdtsc(void) { return ++unit_fake_tsc; }

void host_exit(int code) __attribute__((noreturn));
static inline void __attribute__((noreturn)) halt_forever(void) { host_exit(3); }

#endif /* BHARAT_CPU_H */
