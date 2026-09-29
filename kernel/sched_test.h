#ifndef NEXUS_SCHED_TEST_H
#define NEXUS_SCHED_TEST_H

#include <stdbool.h>

/* Phase 5 tests and live demo. Must run on the boot thread after
 * sched_start(). Worker threads never call a switch/yield function unless a
 * test is specifically about voluntary switching. Returns true if all pass. */
bool sched_run_tests(void);

#endif /* NEXUS_SCHED_TEST_H */
