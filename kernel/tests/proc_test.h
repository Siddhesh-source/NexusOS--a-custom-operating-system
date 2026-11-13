#ifndef BHARAT_PROC_TEST_H
#define BHARAT_PROC_TEST_H

#include <stdbool.h>

/* Boot-time tests for processes, threads, kernel stacks and context
 * switching. Deterministic: threads run only when explicitly switched to.
 * Ends with the process-manager summary panel. Returns true if all pass. */
bool proc_run_tests(void);

/* Opt-in fatal demos (build.py --fault-demo ctx / tstack). Never return. */
void proc_demo_corrupt_context(void);
void proc_demo_thread_stack_overflow(void);

#endif /* BHARAT_PROC_TEST_H */
