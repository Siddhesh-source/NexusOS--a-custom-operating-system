#include "clock.h"
#include "apic.h"
#include "acpi.h"
#include "interrupts.h"
#include "vmm.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "trace.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

#define NS_PER_SEC        1000000000ULL
#define CALIBRATE_NS      50000000ULL       /* 50 ms measuring window */
#define HPET_CAPS         0x000
#define HPET_CONFIG       0x010
#define HPET_COUNTER      0x0F0
#define HPET_ENABLE       1ULL
#define PIT_HZ            1193182ULL

static bool ready;
static uint64_t tsc_start;
static uint64_t tsc_hz, apic_hz;
/* ns = cycles * ns_mult >> 32;  count = ns * count_mult >> 32. Multiplying
 * then shifting avoids a division on every read; the 128-bit product
 * keeps it exact enough and never overflows. */
static uint64_t ns_mult, count_mult;
static const char *reference = "none";
static uint64_t alarms_set, alarms_fired, alarms_early;
static uint64_t alarm_deadline;          /* deadline of the pending alarm */
static uint64_t last_late_ns, max_late_ns;  /* interrupt arrival vs deadline */
static void (*alarm_handler)(void);
static volatile uint64_t *hpet_regs;
static uint64_t hpet_period_fs;

static inline uint64_t mul_shift32(uint64_t a, uint64_t b)
{
    return (uint64_t)(((unsigned __int128)a * b) >> 32);
}

bool clock_ready(void)
{
    return ready;
}

uint64_t clock_now_ns(void)
{
    return ready ? mul_shift32(rdtsc() - tsc_start, ns_mult) : 0;
}

void clock_delay_ns(uint64_t ns)
{
    uint64_t end = clock_now_ns() + ns;
    while (clock_now_ns() < end)
        __asm__ volatile("pause");
}

/* ---- calibration --------------------------------------------------------
 * Start the APIC timer counting down from its maximum, wait a known time
 * using the reference, and see how far the TSC and the APIC timer moved. */

static void start_apic_count(void)
{
    lapic_write(LAPIC_TIMER_DIV, 0x3);                 /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | CLOCK_TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
}

static bool calibrate_hpet(uint64_t *ns, uint64_t *cycles, uint64_t *counts)
{
    const struct acpi_info *a = acpi_get();
    if (!a->hpet_present)
        return false;
    volatile uint64_t *hpet = vmm_map_phys(a->hpet_phys, 1024, VMM_WRITE | VMM_NOCACHE);
    if (hpet == NULL)
        return false;
    uint64_t period_fs = hpet[HPET_CAPS / 8] >> 32;     /* femtoseconds per tick */
    if (period_fs == 0 || period_fs > 100000000ULL)      /* spec: at most 100 ns */
        return false;
    hpet[HPET_CONFIG / 8] |= HPET_ENABLE;
    hpet_regs = hpet;
    hpet_period_fs = period_fs;

    uint64_t ticks_needed = CALIBRATE_NS * 1000000ULL / period_fs;
    uint64_t h0 = hpet[HPET_COUNTER / 8];
    uint64_t c0 = rdtsc();
    start_apic_count();
    uint64_t h1;
    do {
        h1 = hpet[HPET_COUNTER / 8];
    } while (h1 - h0 < ticks_needed);
    *cycles = rdtsc() - c0;
    *counts = 0xFFFFFFFFu - lapic_read(LAPIC_TIMER_COUNT);
    *ns = (h1 - h0) * period_fs / 1000000ULL;
    reference = "HPET";
    return true;
}

/* PIT channel 2 counts down once (mode 0) while we watch its output bit
 * in port 0x61. It can measure at most ~55 ms; we use 50 ms. */
static bool calibrate_pit(uint64_t *ns, uint64_t *cycles, uint64_t *counts)
{
    uint64_t pit_ticks = PIT_HZ * CALIBRATE_NS / NS_PER_SEC;
    outb(0x61, (inb(0x61) & ~0x02) | 0x01);      /* gate on, speaker off */
    outb(0x43, 0xB0);                            /* channel 2, lo/hi, mode 0 */
    outb(0x42, pit_ticks & 0xFF);
    outb(0x42, (pit_ticks >> 8) & 0xFF);
    uint8_t v = inb(0x61) & ~0x01;               /* restart the count */
    outb(0x61, v);
    outb(0x61, v | 0x01);
    uint64_t c0 = rdtsc();
    start_apic_count();
    uint64_t spins = 0;
    while (!(inb(0x61) & 0x20)) {
        if (++spins > 100000000ULL)
            return false;                        /* no PIT either */
    }
    *cycles = rdtsc() - c0;
    *counts = 0xFFFFFFFFu - lapic_read(LAPIC_TIMER_COUNT);
    *ns = CALIBRATE_NS;
    reference = "PIT";
    return true;
}

/* ---- the alarm ---------------------------------------------------------- */

static void timer_interrupt(struct interrupt_frame *frame)
{
    (void)frame;
    alarms_fired++;
    uint64_t now = clock_now_ns();
    trace_event(TRACE_ALARM_FIRE, alarm_deadline, now);
    if (now + 50000 < alarm_deadline) {
        alarms_early++;
    } else {
        last_late_ns = now > alarm_deadline ? now - alarm_deadline : 0;
        if (last_late_ns > max_late_ns)
            max_late_ns = last_late_ns;
    }
    /* End-of-interrupt first: the handler may switch threads and not come
     * back here until this thread runs again. */
    lapic_eoi();
    if (alarm_handler != NULL)
        alarm_handler();
}

void clock_set_alarm(uint64_t deadline_ns)
{
    IRQ_GUARD();
    uint64_t now = clock_now_ns();
    uint64_t wait = deadline_ns > now ? deadline_ns - now : 0;
    uint64_t count = mul_shift32(wait, count_mult);
    if (count == 0)
        count = 1;
    if (count > 0xFFFFFFFFULL)
        count = 0xFFFFFFFFULL;                   /* fires early; caller re-arms */
    alarms_set++;
    alarm_deadline = deadline_ns;
    trace_event(TRACE_ALARM_SET, deadline_ns, now);
    lapic_write(LAPIC_LVT_TIMER, CLOCK_TIMER_VECTOR);   /* one-shot, unmasked */
    lapic_write(LAPIC_TIMER_INIT, (uint32_t)count);
}

void clock_cancel_alarm(void)
{
    IRQ_GUARD();
    lapic_write(LAPIC_TIMER_INIT, 0);            /* 0 stops the timer */
}

void clock_init(void (*on_alarm)(void))
{
    uint64_t ns = 0, cycles = 0, counts = 0;
    {
        IRQ_GUARD();
        if (!calibrate_hpet(&ns, &cycles, &counts) && !calibrate_pit(&ns, &cycles, &counts))
            panic("clock: no HPET or PIT to measure the CPU clock against");
        lapic_write(LAPIC_TIMER_INIT, 0);
        lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED | CLOCK_TIMER_VECTOR);
    }
    if (ns == 0 || cycles == 0 || counts == 0)
        panic("clock: calibration measured nothing");

    tsc_hz = cycles * NS_PER_SEC / ns;
    apic_hz = counts * NS_PER_SEC / ns;
    ns_mult = (NS_PER_SEC << 32) / tsc_hz;
    count_mult = (apic_hz << 32) / NS_PER_SEC;
    alarm_handler = on_alarm;
    interrupt_set_handler(CLOCK_TIMER_VECTOR, timer_interrupt);
    tsc_start = rdtsc();
    ready = true;
    kprintf("[clock] calibrated against %s: TSC %lu.%03lu MHz, APIC timer %lu.%03lu MHz; "
            "tickless one-shot alarms on vector 0x%x\n",
            reference, tsc_hz / 1000000, (tsc_hz / 1000) % 1000,
            apic_hz / 1000000, (apic_hz / 1000) % 1000, CLOCK_TIMER_VECTOR);
}

uint64_t clock_hpet_ns(void)
{
    if (hpet_regs == NULL)
        return 0;
    /* fs -> ns without a 128-bit division (the kernel has no helper for
     * one): split the count so neither product overflows. */
    uint64_t c = hpet_regs[HPET_COUNTER / 8];
    return (c / 1000000) * hpet_period_fs + (c % 1000000) * hpet_period_fs / 1000000;
}

void clock_get_info(struct clock_info *out)
{
    IRQ_GUARD();
    out->reference = reference;
    out->tsc_hz = tsc_hz;
    out->apic_timer_hz = apic_hz;
    out->alarms_set = alarms_set;
    out->alarms_fired = alarms_fired;
    out->alarms_early = alarms_early;
    out->last_late_ns = last_late_ns;
    out->max_late_ns = max_late_ns;
}
