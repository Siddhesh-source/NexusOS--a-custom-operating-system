#include "rtc.h"
#include "cpu.h"

static unsigned cmos(unsigned reg)
{
    outb(0x70, (uint8_t)reg);
    return inb(0x71);
}

static unsigned from_bcd(unsigned v)
{
    return (v & 0x0F) + (v >> 4) * 10;
}

bool rtc_time(unsigned *hour, unsigned *min, unsigned *sec)
{
    /* Read twice and accept matching reads taken outside an update, so a
     * second ticking over mid-read cannot give 12:59:00 for 13:00:00. */
    for (unsigned tries = 0; tries < 10; tries++) {
        for (unsigned spin = 0; spin < 100000 && (cmos(0x0A) & 0x80); spin++)
            ;                                    /* an update takes < 2 ms */
        unsigned s = cmos(0x00), m = cmos(0x02), h = cmos(0x04);
        if (cmos(0x00) != s || cmos(0x02) != m || cmos(0x04) != h)
            continue;
        unsigned fmt = cmos(0x0B);
        bool pm = h & 0x80;
        h &= 0x7F;
        if (!(fmt & 0x04)) {                     /* BCD, the usual case */
            s = from_bcd(s);
            m = from_bcd(m);
            h = from_bcd(h);
        }
        if (!(fmt & 0x02))                       /* 12-hour clock */
            h = (h % 12) + (pm ? 12 : 0);
        *hour = h;
        *min = m;
        *sec = s;
        return h < 24 && m < 60 && s < 60;
    }
    return false;
}
