#ifndef BHARAT_SMP_TEST_H
#define BHARAT_SMP_TEST_H

#include <stdbool.h>

/* Phase 9 self-tests: CPUs online, per-CPU data, spinlocks under load,
 * the lock-order checker, IPIs and TLB shootdown, RCU, per-CPU queues
 * with work stealing, and a multi-core dashboard. Turns work stealing on. */
bool smp_run_tests(void);

/* Fault demo: take two locks in opposite orders; the checker panics with
 * both call traces. */
void smp_demo_lock_inversion(void) __attribute__((noreturn));

#endif /* BHARAT_SMP_TEST_H */
