#ifndef BHARAT_DEBUG_TEST_H
#define BHARAT_DEBUG_TEST_H

/* Boot-time self-tests for the debugging and hardening tools: symbol
 * lookup, call traces, the stack protector canary, heap poisoning and red
 * zones, and the event trace. */

#include <stdbool.h>

bool debug_run_tests(void);

/* Fault demos (build.py --fault-demo panic / smash / uaf / overflow).
 * None of them returns. */
void debug_demo_panic(void);
void debug_demo_stack_smash(void);
void debug_demo_use_after_free(void);
void debug_demo_heap_overflow(void);

#endif /* BHARAT_DEBUG_TEST_H */
