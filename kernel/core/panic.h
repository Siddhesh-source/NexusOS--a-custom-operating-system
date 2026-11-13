#ifndef BHARAT_PANIC_H
#define BHARAT_PANIC_H

#include <stdbool.h>

void panic(const char *msg) __attribute__((noreturn));

/* Start a crash report: interrupts off, other CPUs stopped. Returns false
 * if another CPU is already reporting (the caller should just halt). */
bool panic_begin(void);

/* The CPU reporting a crash, or -1 (read through the helpers below). */
extern volatile int panic_cpu;
bool kernel_panicking_slow(void);

/* Is THIS CPU in the middle of a crash report? (Locks are skipped then:
 * the other CPUs are stopped and may never release theirs.) Every lock
 * asks, so the usual answer (no crash) is one load. */
static inline bool kernel_panicking(void)
{
    return panic_cpu >= 0 && kernel_panicking_slow();
}

/* Has any CPU started a crash report? */
static inline bool kernel_panic_started(void)
{
    return panic_cpu >= 0;
}

#endif
