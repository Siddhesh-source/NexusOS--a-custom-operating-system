#ifndef BHARAT_SELFTEST_H
#define BHARAT_SELFTEST_H

/* Shared helpers for boot self-tests, and the running totals that the
 * shell's `checks` command shows ("N boot checks passed").
 *
 * Newer test files use st_check/st_section; older suites keep their own
 * counters and report their totals with selftest_record() at the end. */

#include <stdbool.h>

#define SELFTEST_MAX_SUITES 16

struct selftest_suite {
    const char *name;
    unsigned passed, failed;
};

/* Count one check and print a PASS/FAIL line. Returns `ok`. */
bool st_check(bool ok, const char *desc);
void st_section(const char *name);

/* Start a suite that uses st_check; st_end prints its totals, records
 * them and returns true if nothing failed. */
void st_begin(void);
bool st_end(const char *suite);

/* Record a finished suite's totals (older suites call this directly). */
void selftest_record(const char *suite, unsigned passed, unsigned failed);

/* Suites recorded so far, and the grand totals. */
unsigned selftest_suites(const struct selftest_suite **out);
void selftest_totals(unsigned *passed, unsigned *failed);

#endif /* BHARAT_SELFTEST_H */
