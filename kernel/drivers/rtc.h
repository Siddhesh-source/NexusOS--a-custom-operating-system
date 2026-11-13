#ifndef BHARAT_RTC_H
#define BHARAT_RTC_H

/* Real-time clock: the time of day from the PC's battery-backed CMOS
 * clock, for the desktop's clock. QEMU sets it to the host's time (UTC).
 *
 * Limits: time of day only (no date); not locked, so one reader (the
 * desktop thread). */

#include <stdbool.h>

/* False if the clock could not be read (it kept updating). */
bool rtc_time(unsigned *hour, unsigned *min, unsigned *sec);

#endif /* BHARAT_RTC_H */
