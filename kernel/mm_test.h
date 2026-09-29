#ifndef NEXUS_MM_TEST_H
#define NEXUS_MM_TEST_H

#include <stdbool.h>

/* Boot-time self-tests for the PMM, VMM, heap, page-fault reporting and
 * address-space isolation. Returns true if every check passed. */
bool mm_run_tests(void);

#endif /* NEXUS_MM_TEST_H */
