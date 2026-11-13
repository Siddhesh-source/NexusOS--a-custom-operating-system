#include "panic.h"
#include "serial.h"
#include "backtrace.h"
#include "trace.h"

void panic(const char *msg)
{
    __asm__ volatile("cli");   /* nothing may run after the kernel gives up */
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
