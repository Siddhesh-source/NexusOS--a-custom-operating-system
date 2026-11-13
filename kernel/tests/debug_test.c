#include "debug_test.h"
#include "selftest.h"
#include "backtrace.h"
#include "trace.h"
#include "kernel.h"
#include "panic.h"
#include "kprintf.h"
#include "string.h"
#include "heap.h"
#include "cpu.h"
#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

static unsigned passed, failed;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) {                                         \
            passed++;                                       \
            kprintf("  [PASS] %s\n", msg);                  \
        } else {                                            \
            failed++;                                       \
            kprintf("  [FAIL] %s\n", msg);                  \
        }                                                   \
    } while (0)

extern uint64_t __stack_chk_guard;

/* ---- symbols and call traces ------------------------------------------ */

static bool name_is(const struct ksym *s, const char *name)
{
    return s != NULL && strcmp(s->name, name) == 0;
}

static void test_symbols(void)
{
    kprintf("\n[debug] Symbol table\n");
    CHECK(ksym_count > 100, "symbol table has the kernel's functions");

    bool sorted = true;
    for (uint64_t i = 1; i < ksym_count; i++)
        sorted = sorted && ksym_table[i - 1].addr < ksym_table[i].addr;
    CHECK(sorted, "symbols are sorted by address");

    uint64_t off = 1;
    const struct ksym *s = ksym_lookup((uint64_t)kernel_main, &off);
    CHECK(name_is(s, "kernel_main") && off == 0, "kernel_main resolves to kernel_main+0x0");

    s = ksym_lookup((uint64_t)debug_run_tests + 5, &off);
    CHECK(name_is(s, "debug_run_tests") && off == 5, "an address inside a function gives name+offset");

    CHECK(ksym_lookup(0, NULL) == NULL && ksym_lookup((uint64_t)&passed, NULL) == NULL,
          "data and null addresses are not reported as functions");
}

/* A fixed chain of calls. disable_tail_calls stops the compiler from
 * turning a final call into a jump, which would remove a frame. */
static uint64_t bt_seen[BACKTRACE_MAX_DEPTH];
static unsigned bt_count;

__attribute__((noinline)) static unsigned bt_level3(unsigned x)
{
    bt_count = backtrace_collect((uint64_t)__builtin_frame_address(0),
                                 bt_seen, BACKTRACE_MAX_DEPTH);
    __asm__ volatile("" ::: "memory");
    return x + bt_count;
}

__attribute__((noinline, disable_tail_calls)) static unsigned bt_level2(unsigned x)
{
    return bt_level3(x + 1) + 1;
}

__attribute__((noinline, disable_tail_calls)) static unsigned bt_level1(unsigned x)
{
    return bt_level2(x + 1) + 1;
}

/* noinline: the expected trace names this function. */
__attribute__((noinline)) static void test_backtrace(void)
{
    kprintf("\n[debug] Call traces\n");
    (void)bt_level1(0);
    CHECK(bt_count >= 4, "walk finds at least four frames");

    const char *want[] = { "bt_level2", "bt_level1", "test_backtrace" };
    bool match = bt_count >= 3;
    for (unsigned i = 0; match && i < 3; i++)
        match = name_is(ksym_lookup(bt_seen[i] - 1, NULL), want[i]);
    CHECK(match, "frames are bt_level2 <- bt_level1 <- test_backtrace");

    kprintf("  sample trace from here:\n");
    backtrace_print_here();

    CHECK(backtrace_collect(0, bt_seen, 4) == 0, "a null frame pointer gives an empty trace");
    CHECK(backtrace_collect(0x1234, bt_seen, 4) == 0, "an unmapped frame pointer gives an empty trace");
}

/* ---- stack protector ---------------------------------------------------- */

static void test_stack_guard(void)
{
    kprintf("\n[debug] Stack protector\n");
    CHECK(__stack_chk_guard != 0x2f8e6a1b7c3d5900ULL && __stack_chk_guard != 0,
          "canary was randomised at boot");
    CHECK((__stack_chk_guard & 0xff) == 0, "canary's low byte is 0 (stops string copies)");
}

/* ---- heap poisoning and red zones -------------------------------------- */

static bool all_bytes(const uint8_t *p, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != v)
            return false;
    return true;
}

static void test_heap_poison(void)
{
    kprintf("\n[debug] Heap poisoning and red zones\n");
    /* Nothing else may allocate while freed memory is deliberately dirty,
     * or that allocation would (correctly) panic. */
    IRQ_GUARD();

    CHECK(heap_check(), "heap is consistent before the test");

    uint8_t *p = kmalloc(20);
    CHECK(p != NULL && all_bytes(p + 20, HEAP_REDZONE, HEAP_POISON_REDZONE),
          "allocation is followed by a red zone");
    CHECK(p != NULL && all_bytes(p, 20, HEAP_POISON_FREE),
          "new allocation holds the poison pattern, not old data");
    if (p == NULL)
        return;

    memset(p, 0x11, 20);                 /* filling all 20 bytes is fine */
    CHECK(heap_check(), "writing exactly the requested size leaves the red zone intact");

    p[20] = 0x22;                        /* one byte too far */
    kprintf("  (a red-zone report is expected next)\n");
    CHECK(!heap_check(), "write one byte past the end is caught");
    p[20] = HEAP_POISON_REDZONE;         /* repair so kfree does not panic */
    CHECK(heap_check(), "heap is consistent again once repaired");

    CHECK(kfree(p) == HEAP_OK, "free the block");
    CHECK(all_bytes(p, 20 + HEAP_REDZONE, HEAP_POISON_FREE), "freed memory is poisoned");

    uint8_t saved = p[8];
    p[8] = 0x33;                         /* use after free */
    kprintf("  (a use-after-free report is expected next)\n");
    CHECK(!heap_check(), "write into freed memory is caught");
    p[8] = saved;
    CHECK(heap_check(), "heap is consistent again once repaired");

    uint8_t *z = kzalloc(40);
    CHECK(z != NULL && all_bytes(z, 40, 0) && all_bytes(z + 40, 8, HEAP_POISON_REDZONE),
          "kzalloc zeroes only the requested bytes");
    kfree(z);
    CHECK(heap_check(), "heap is consistent after the test");
}

/* ---- event trace -------------------------------------------------------- */

static void test_trace(void)
{
    kprintf("\n[debug] Event trace\n");
    struct trace_event ev[4];

    uint64_t before = trace_total();
    CHECK(before > 0, "boot already recorded events (threads, switches)");

    trace_event(TRACE_TEST, 1, 10);
    trace_event(TRACE_TEST, 2, 20);
    trace_event(TRACE_TEST, 3, 30);
    /* Interrupts may add switch events in between; look for ours. */
    unsigned n = trace_snapshot(ev, 4);
    unsigned found = 0;
    uint64_t last_seq = 0, last_tsc = 0;
    bool ordered = true;
    for (unsigned i = 0; i < n; i++) {
        if (ev[i].type != TRACE_TEST)
            continue;
        found++;
        ordered = ordered && ev[i].a == found && ev[i].b == found * 10
                  && ev[i].seq > last_seq && ev[i].tsc >= last_tsc;
        last_seq = ev[i].seq;
        last_tsc = ev[i].tsc;
    }
    CHECK(found == 3 && ordered, "events come back in order with their arguments and timestamps");

    /* Overwrite the whole ring: only the newest TRACE_CAPACITY survive. */
    bool was = trace_set_enabled(true);
    for (unsigned i = 0; i < TRACE_CAPACITY + 10; i++)
        trace_event(TRACE_TEST, 1000 + i, 0);
    trace_set_enabled(was);
    struct trace_event last;
    CHECK(trace_snapshot(&last, 1) == 1 && last.type == TRACE_TEST, "newest event survives a full wrap");
    CHECK(trace_total() >= before + 3 + TRACE_CAPACITY + 10, "total counts overwritten events too");

    trace_set_enabled(false);
    uint64_t frozen = trace_total();
    trace_event(TRACE_TEST, 0, 0);
    CHECK(trace_total() == frozen, "disabled trace records nothing");
    trace_set_enabled(true);

    CHECK(strcmp(trace_type_name(TRACE_SCHED_SWITCH), "sched.switch") == 0
          && strcmp(trace_type_name((trace_type_t)999), "unknown") == 0,
          "event type names, including out-of-range types");
}

bool debug_run_tests(void)
{
    passed = failed = 0;
    kprintf("\n===== Debug and hardening tests =====\n");
    test_symbols();
    test_backtrace();
    test_stack_guard();
    test_heap_poison();
    test_trace();
    kprintf("\n===== Debug tests: %u passed, %u failed =====\n", passed, failed);
    selftest_record("Debug and hardening", passed, failed);
    return failed == 0;
}

/* ---- fault demos -------------------------------------------------------- */

__attribute__((noinline)) static void demo_panic_inner(void)
{
    panic("forced panic to show a call trace (fault demo)");
}

__attribute__((noinline)) void debug_demo_panic(void)
{
    kprintf("\n[demo] Forcing a kernel panic from a nested call...\n");
    demo_panic_inner();
}

/* Overflows a 16-byte local buffer: writes from its start up to (not
 * including) the saved frame pointer. The canary always lies in between,
 * whatever padding the compiler chose, while the saved RBP and return
 * address stay intact so the call trace still works. */
__attribute__((noinline)) static void stack_smash_victim(void)
{
    char buf[16];
    volatile char *p = buf;
    size_t n = (size_t)((char *)__builtin_frame_address(0) - buf);
    kprintf("[demo] Writing %lu bytes into a 16-byte stack buffer...\n", (uint64_t)n);
    for (size_t i = 0; i < n; i++)
        p[i] = 'A';
    __asm__ volatile("" :: "r"(buf) : "memory");
}

__attribute__((noinline)) void debug_demo_stack_smash(void)
{
    kprintf("\n[demo] Overflowing a stack buffer past its canary...\n");
    stack_smash_victim();
    kprintf("[demo] ERROR: the overflow was not caught\n");
}

/* Frees a block, writes into it, then allocates blocks of the same size
 * until the allocator reaches the damaged memory and reports it. */
__attribute__((noinline)) void debug_demo_use_after_free(void)
{
    kprintf("\n[demo] Writing into freed heap memory...\n");
    uint8_t *p = kmalloc(64);
    if (p == NULL || kfree(p) != HEAP_OK)
        return;
    p[0] = 0x42;
    kprintf("[demo] Allocating until the freed block is handed out again...\n");
    for (int i = 0; i < 100000; i++)
        if (kmalloc(64) == NULL)
            break;
    kprintf("[demo] ERROR: the use after free was not caught\n");
}

/* Writes 4 bytes past the end of a 20-byte block, then frees it. */
__attribute__((noinline)) void debug_demo_heap_overflow(void)
{
    kprintf("\n[demo] Writing past the end of a 20-byte heap block...\n");
    volatile uint8_t *p = kmalloc(20);
    if (p == NULL)
        return;
    for (int i = 20; i < 24; i++)
        p[i] = 0x42;
    kfree((void *)p);
    kprintf("[demo] ERROR: the overflow was not caught\n");
}
