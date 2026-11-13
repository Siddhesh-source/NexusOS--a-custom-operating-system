/* Shared support for unit-test programs: reporting, random numbers, and
 * fakes for the kernel pieces the tested code calls. */
#include "unit.h"
#include "kprintf.h"
#include "panic.h"
#include "serial.h"

unsigned unit_passed, unit_failed;
uint64_t unit_fake_tsc;
bool unit_panic_seen;


int unit_report(const char *suite)
{
    kprintf("[unit] %s: %u checks passed, %u failed\n", suite, unit_passed, unit_failed);
    return unit_failed == 0 ? 0 : 1;
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

void unit_seed(uint64_t seed)
{
    rng_state = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

uint64_t unit_rand(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545f4914f6cdd1dULL;
}

uint64_t unit_rand_below(uint64_t n)
{
    return n ? unit_rand() % n : 0;
}

/* ---- serial port: straight to the host's standard output --------------- */

void serial_write(const char *buf, uint64_t n)
{
    host_write(buf, (size_t)n);
}

/* ---- panic: either expected by a test (jump back) or a real failure ----- */

static void *panic_jump[5];
static bool panic_armed;

bool unit_expect_panic(void (*fn)(void))
{
    unit_panic_seen = false;
    panic_armed = true;
    if (__builtin_setjmp(panic_jump) == 0)
        fn();
    panic_armed = false;
    return unit_panic_seen;
}

void panic(const char *msg)
{
    unit_panic_seen = true;
    if (panic_armed)
        __builtin_longjmp(panic_jump, 1);
    kprintf("[unit] unexpected panic: %s\n", msg);
    host_exit(2);
}
