#include "selftest.h"
#include "kprintf.h"

static struct selftest_suite suites[SELFTEST_MAX_SUITES];
static unsigned nsuites;
static unsigned cur_passed, cur_failed;

bool st_check(bool ok, const char *desc)
{
    if (ok)
        cur_passed++;
    else
        cur_failed++;
    kprintf("[test]   %s  %s\n", ok ? "PASS" : "FAIL", desc);
    return ok;
}

void st_section(const char *name)
{
    kprintf("\n[test] --- %s ---\n", name);
}

void st_begin(void)
{
    cur_passed = cur_failed = 0;
}

bool st_end(const char *suite)
{
    kprintf("\n===== %s tests: %u passed, %u failed =====\n", suite, cur_passed, cur_failed);
    selftest_record(suite, cur_passed, cur_failed);
    return cur_failed == 0;
}

void selftest_record(const char *suite, unsigned passed, unsigned failed)
{
    for (unsigned i = 0; i < nsuites; i++) {
        if (suites[i].name == suite) {      /* a rerun replaces the old result */
            suites[i].passed = passed;
            suites[i].failed = failed;
            return;
        }
    }
    if (nsuites < SELFTEST_MAX_SUITES)
        suites[nsuites++] = (struct selftest_suite){ suite, passed, failed };
}

unsigned selftest_suites(const struct selftest_suite **out)
{
    *out = suites;
    return nsuites;
}

void selftest_totals(unsigned *passed, unsigned *failed)
{
    unsigned p = 0, f = 0;
    for (unsigned i = 0; i < nsuites; i++) {
        p += suites[i].passed;
        f += suites[i].failed;
    }
    *passed = p;
    *failed = f;
}
