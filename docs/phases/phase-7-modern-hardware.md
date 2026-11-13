# Phase 7: Modern Hardware

**Goal:** stop relying on 1980s PC parts. Find the hardware the way modern
systems do (ACPI tables, PCI scan), use the APIC interrupt controllers
instead of the old 8259 PIC, keep time precisely, and stop waking the CPU
100 times a second for nothing.

**Status:** done. 38 boot checks (`kernel/tests/hw_test.c`), all earlier
suites unchanged, all 12 fault demos, unit tests with a fake clock.

| Step | Where |
|---|---|
| 1. ACPI tables: MADT (CPUs, APICs, IRQ overrides), HPET, MCFG | `kernel/acpi/acpi.c` |
| 2. Local APIC on, PIC fully masked, I/O APIC routing | `kernel/arch/x86_64/apic.c` |
| 3. TSC and APIC timer calibrated against the HPET; one-shot alarms; tickless scheduler | `kernel/arch/x86_64/clock.c`, `kernel/sched/sched.c` |
| 4. PCI scan (ECAM when MCFG exists, I/O ports otherwise) | `kernel/pci/pci.c` |
| 5. Text on screen next to serial | `kernel/drivers/fbcon.c` |

---

## 1. ACPI: what hardware is there

Limine gives the address of the RSDP, the firmware's root pointer. From it
we read the list of tables (RSDT, or XSDT on ACPI 2.0+) and three tables
from it. Every table's checksum (its bytes must add up to 0) is verified;
a bad table is reported and ignored.

```text
[acpi] ACPI 1.0, OEM "BOCHS ", RSDT at 0x000000001ffe2329
[acpi]   FACP at 0x000000001ffe21dd, 116 bytes
[acpi]   APIC at 0x000000001ffe2251, 120 bytes
[acpi]   HPET at 0x000000001ffe22c9, 56 bytes
[acpi]   WAET at 0x000000001ffe2301, 40 bytes
[acpi] 1 CPU(s), local APIC at 0x00000000fee00000, 1 I/O APIC(s), 5 IRQ override(s), legacy PICs present
[acpi] HPET at 0x00000000fed00000
[acpi] no MCFG table: PCI config space via I/O ports
```

- **MADT** ("APIC"): each CPU's local APIC ID (Phase 9 starts the other
  CPUs from this list), the I/O APICs, and the *interrupt source
  overrides*. These say, for example, that the timer's old IRQ 0 actually
  arrives on global interrupt 2. `acpi_irq_to_gsi()` applies them.
- **HPET**: address of the high-precision event timer, used as the
  reference clock.
- **MCFG**: where PCI Express configuration space is mapped (ECAM). QEMU's
  default `pc` machine has none; `-M q35` has one.

Firmware tables and device registers often lie outside the RAM the kernel
maps, so `vmm_map_phys()` maps any physical range into the direct map on
demand (uncached for device registers).

No AML interpreter yet: power buttons, sleep states and device methods
need one, and none of the plan needs them before Phase 19.

## 2. Interrupt controllers

Before the scheduler starts, `apic_init()`:
1. masks every line of both 8259 PICs (they were remapped to vectors
   32–47 at boot, so a stray *spurious* IRQ 7/15 lands on a harmless
   vector and is ignored);
2. turns on the local APIC (spurious vector 0xFF, all local interrupt
   sources masked);
3. sets up every I/O APIC with all lines masked.

`ioapic_route_irq(irq, vector)` sends an ISA IRQ (after overrides) to a
vector on this CPU. Every vector 0–255 now has an entry stub, and
`interrupt_set_handler()` registers a C handler for a device vector.

**Tested by** routing the PIT (still counting at 100 Hz in the background)
through the I/O APIC: about 10 interrupts arrive per 100 ms, none once the
line is masked again. Doing this showed that QEMU's PIT in square-wave
mode (3) delivers two edges per period through the I/O APIC; the test
uses rate-generator mode (2), which gives exactly one (bug journal #11).

## 3. Clock, alarms and the tickless scheduler

**Calibration.** The TSC (the CPU's cycle counter) and the APIC timer run
at speeds that differ per machine. `clock_init()` starts the APIC timer
counting down, waits 50 ms by the HPET (whose rate is in its registers),
and sees how far both moved. Without an HPET it uses PIT channel 2.

```text
[clock] calibrated against HPET: TSC 3096.330 MHz, APIC timer 62.553 MHz; tickless one-shot alarms on vector 0x40
```

`clock_now_ns()` converts TSC cycles to nanoseconds with a multiply and a
shift (no division per read). `clock_set_alarm(t)` programs the APIC
timer in one-shot mode for time `t`.

**Tickless scheduling.** The old scheduler was driven by a 100 Hz PIT
interrupt. Now there is no periodic interrupt at all. Each time the
scheduler decides something, it sets one alarm for the earliest time it
next has work:
- the end of the running thread's 20 ms slice, *only if* another thread
  is waiting for the CPU;
- the next sleeping thread's wake-up time.

With nothing to do, no alarm is set and the idle CPU sleeps until a device
interrupts it.

Threads are charged CPU time in nanoseconds at each switch. The 10 ms
"tick" survives only as a unit (`sched_sleep(ticks)`, `sched_ticks()`,
`cpu_ticks`), so all callers and the Phase 5 tests stayed unchanged; they
all pass. `sched_sleep_ns()` sleeps for an exact duration.

**Lazy re-arming** (decision D20). The first version reprogrammed the
APIC timer on almost every switch, because the slice end kept moving. A
device-register write is slow, and the context-switch benchmark went from
1,927 to 4,506 cycles. Now the alarm is only reprogrammed when a deadline
moves *earlier*. An alarm that turns out early just fires, and the
scheduler looks again: at most one extra interrupt per slice, and no
register writes per switch (back to about 2,300 cycles). Waking sleepers
inside the alarm handler no longer arms an extra "run now" alarm that
fired again for nothing (found by the unit tests).

**Results** (QEMU on a Windows host):

```text
  sleep  1 ms -> best of 5: woke  169 us late; alarm interrupt  122 us late; kernel wake-up  46 us
  sleep 40 ms -> best of 5: woke  390 us late; alarm interrupt  342 us late; kernel wake-up  48 us
  worst (best of 5): 972 us end to end, 28 us in the kernel; worst single sample 2929 us
  idle: 1 timer interrupt(s) in 500 ms (a 100 Hz tick would give 50)
```

**Why the sleep test checks the kernel's share** (decision D21). Lateness
has two parts: how late the alarm *interrupt* arrives, and how long the
kernel then takes to get the sleeper running. The first part is the
emulator's. QEMU on Windows services its timers on a roughly 1 ms grid,
and a busy host adds more (one benchmark run on a loaded machine saw
15 ms). The kernel's own share is about 20–60 µs. So the test requires:
- no sleep ends early;
- the kernel wakes a sleeper within 1 ms of its alarm (best of 5 tries,
  which filters out host pauses);
- no sleep is more than 100 ms late, which only fails if an alarm is lost
  (a kernel bug).

End-to-end accuracy is printed. On a quiet host it is within 1 ms.

## 4. PCI

Every bus/device/function is checked once at boot: 8,192 reads, simple
and complete. Configuration space is read through ECAM (memory-mapped,
4 KiB per function) when MCFG gives its address, otherwise through ports
0xCF8/0xCFC.

```text
[pci] 6 device(s) found via I/O ports
[pci]   00:00.0  8086:1237  class 06.00  host bridge
[pci]   00:01.0  8086:7000  class 06.01  ISA bridge
[pci]   00:01.1  8086:7010  class 01.01  IDE controller
[pci]   00:01.3  8086:7113  class 06.80  bridge  (IRQ 9)
[pci]   00:02.0  1234:1111  class 03.00  VGA display
[pci]   00:03.0  8086:100e  class 02.00  Ethernet controller  (IRQ 11)
```

This matches QEMU's own `info pci` for the default machine exactly: the
same functions, IDs, classes and IRQs (checked through the QEMU monitor).

## 5. Screen console

Every byte written to the serial port is also drawn on Limine's
framebuffer, using a built-in public-domain 8×8 font drawn at double
height (8×16 cells; 160×50 characters at 1280×800). UTF-8 box-drawing
characters become `+ - |`.

Without a graphics driver, scrolling (moving 4 MB of pixels per line) is
too slow, so the console *wraps*: after the last row it continues at the
top, clearing each line before writing it and keeping one blank row as a
visible gap between the newest and the oldest text.

## How it is tested

`kernel/tests/hw_test.c` (38 checks):
- **ACPI:** tables valid, CPU and I/O APIC found, IRQ 0 → GSI 2 override,
  checksum rule, HPET.
- **PCI:** host bridge at 00:00.0, VGA and disk controllers present, IDs
  re-read match, ordered, invalid requests refused.
- **Screen:** at least 80×25, boot text drawn, a drawn `H` has the text
  colour where the font has pixels.
- **APIC and clock:** PICs masked (read back from the chips), APIC
  enabled, plausible TSC/APIC rates, a 2 ms delay measures 2 ms.
- **Sleep accuracy** and **tickless idle** as above, plus "the old PIT no
  longer interrupts".
- **I/O APIC routing** as above.

`tests/unit/sched_unit.c` drives the real `sched.c` with a fake clock
whose alarm fires when time passes its deadline. It checks exact
round-robin order, sleep length, that a stale alarm fires at most once,
that a clean sleep takes exactly one alarm, and that a thread running
alone causes no timer interrupts at all.

## Known limits

- xAPIC mode only (no x2APIC); interrupts go to the boot CPU (Phase 9).
- One alarm for one CPU; per-CPU alarms come with SMP.
- The TSC is assumed to run at a constant rate (true on modern CPUs and in
  QEMU).
- No AML: ACPI is used only for discovery.
- PCI BAR sizes are not probed yet (drivers in Phase 13 need them).
- The screen console wraps instead of scrolling and has no cursor.
