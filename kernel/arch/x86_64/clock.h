#ifndef BHARAT_CLOCK_H
#define BHARAT_CLOCK_H

/* Time and the one-shot alarm used by the tickless scheduler.
 *
 *   clock_now_ns()       nanoseconds since clock_init(), from the CPU's
 *                        time stamp counter (TSC): cheap to read, no
 *                        interrupts needed
 *   clock_set_alarm()    one interrupt at (about) the given time, from the
 *                        local APIC timer in one-shot mode
 *
 * Both run at speeds that differ per machine, so clock_init() measures
 * them against a reference with a known rate: the HPET (its rate is in
 * its registers) or, without one, PIT channel 2 (1.193182 MHz).
 *
 * There is no periodic tick: if nobody sets an alarm, the timer stays
 * silent. The alarm calls the handler given to clock_init() from the
 * interrupt, after the end-of-interrupt is sent.
 *
 * Limits: assumes the TSC runs at a constant rate (true on CPUs from the
 * last 15 years and in QEMU); one CPU; the alarm's longest single wait is
 * limited by the 32-bit APIC counter (tens of seconds; longer waits fire
 * early and the caller re-arms). */

#include <stdint.h>
#include <stdbool.h>

#define CLOCK_TIMER_VECTOR  0x40

/* Calibrate, then route the APIC timer to `on_alarm`. Requires the local
 * APIC (apic_init) and ACPI. */
void clock_init(void (*on_alarm)(void));

bool clock_ready(void);
uint64_t clock_now_ns(void);

/* Fire once at `deadline_ns` (clock_now_ns time). A deadline in the past
 * fires almost at once. Replaces any earlier alarm. */
void clock_set_alarm(uint64_t deadline_ns);
void clock_cancel_alarm(void);

/* Busy-wait (interrupts may stay on). For calibration and short delays. */
void clock_delay_ns(uint64_t ns);

struct clock_info {
    const char *reference;      /* "HPET" or "PIT" */
    uint64_t tsc_hz;
    uint64_t apic_timer_hz;     /* after the divide-by-16 */
    uint64_t alarms_set;
    uint64_t alarms_fired;
    uint64_t alarms_early;      /* fired more than 50 us before the deadline */
    uint64_t last_late_ns;      /* how late the last alarm interrupt arrived */
    uint64_t max_late_ns;
};
void clock_get_info(struct clock_info *out);

/* The HPET's own count in nanoseconds (0 without an HPET): an independent
 * second clock for cross-checks. */
uint64_t clock_hpet_ns(void);

#endif /* BHARAT_CLOCK_H */
