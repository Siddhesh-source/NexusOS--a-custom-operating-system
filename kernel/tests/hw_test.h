#ifndef BHARAT_HW_TEST_H
#define BHARAT_HW_TEST_H

/* Boot-time self-tests for hardware discovery and timers: ACPI tables,
 * PCI scan, screen console, local/I-O APIC and the tickless timer. */

#include <stdbool.h>

bool hw_run_tests(void);

#endif /* BHARAT_HW_TEST_H */
