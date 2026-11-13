#ifndef BHARAT_UNIT_H
#define BHARAT_UNIT_H

/* Host-side unit tests: real kernel source files compiled as small
 * programs for the build machine and run there (`python build.py unit`).
 *
 * The programs are freestanding, like the kernel: no C library. Output
 * and exit go through two functions implemented once per host
 * (host_linux.c, host_windows.c). Kernel pieces the tested code calls
 * (page allocator, page tables, panic, serial port) are replaced by small
 * fakes in fakes.c. Printing uses the kernel's own kprintf.
 *
 * Each test program defines unit_main() and uses UCHECK. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void host_write(const char *s, size_t n);
__attribute__((noreturn)) void host_exit(int code);

int unit_main(void);

extern unsigned unit_passed, unit_failed;

/* Count every check; print only failures (thousands of checks run). */
#define UCHECK(cond, ...)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            unit_passed++;                                                  \
        } else {                                                            \
            unit_failed++;                                                  \
            kprintf("  [FAIL] %s:%d: ", __FILE__, __LINE__);                \
            kprintf(__VA_ARGS__);                                           \
            kprintf("\n");                                                  \
        }                                                                   \
    } while (0)

/* Print the summary and return the exit code for unit_main. */
int unit_report(const char *suite);

/* Deterministic pseudo-random numbers (xorshift64*), so a failure can be
 * reproduced from its seed. */
void unit_seed(uint64_t seed);
uint64_t unit_rand(void);
uint64_t unit_rand_below(uint64_t n);   /* 0 .. n-1 */

/* Set when the code under test calls panic(); see fakes.c. */
extern bool unit_panic_seen;

/* Run fn(); returns true if it called panic() (which then returns here
 * instead of stopping the program). State touched by fn may be half
 * updated afterwards, so tests re-initialise it. */
bool unit_expect_panic(void (*fn)(void));

/* Fake page allocator controls (fakes.c). */
extern uint64_t unit_frames_in_use;
extern int64_t unit_fail_frame_in;   /* <0 never; N: the (N+1)th allocation fails */

#endif /* BHARAT_UNIT_H */
