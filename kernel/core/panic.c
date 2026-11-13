#include "panic.h"
#include "serial.h"

void panic(const char *msg)
{
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
    serial_puts("\nSystem halted.\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}
