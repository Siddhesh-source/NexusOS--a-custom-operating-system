#include "fbcon.h"
#include "panic.h"
#include "serial.h"
#include "backtrace.h"
#include "trace.h"
#include "percpu.h"
#include "ipi.h"
#include "cpu.h"

/* The CPU handling a panic, or -1. Only the first CPU to panic reports;
 * the others are stopped with an NMI (or stop themselves). */
volatile int panic_cpu = -1;

bool kernel_panicking_slow(void)
{
    return panic_cpu == (int)cpu_id();
}

bool panic_begin(void)
{
    __asm__ volatile("cli");   /* nothing may run after the kernel gives up */
    int expect = -1;
    if (!__atomic_compare_exchange_n(&panic_cpu, &expect, (int)cpu_id(), false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return expect == (int)cpu_id();   /* a panic inside the panic: carry on */
    smp_halt_others();
    fbcon_resume();            /* the desktop may own the screen */
    return true;
}

void panic(const char *msg)
{
    if (!panic_begin())
        halt_forever();           /* another CPU is already reporting */
    serial_puts("\n");
    serial_puts("╔══════════════════════════════════════════╗\n");
    serial_puts("║          *** KERNEL PANIC ***            ║\n");
    serial_puts("╚══════════════════════════════════════════╝\n\n");
    if (msg != NULL) {
        serial_puts("PANIC: ");
        serial_puts(msg);
        serial_puts("\n");
    } else {
        serial_puts("PANIC: <no message provided>\n");
    }
    serial_puts("\n");
    /* Start from panic's own frame: the first line is its caller. */
    backtrace_print(0, (uint64_t)__builtin_frame_address(0));
    serial_puts("\n");
    trace_dump(TRACE_DUMP_ON_CRASH);
    serial_puts("\nSystem halted.\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}
