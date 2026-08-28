#include "kernel.h"
#include "serial.h"
#include "panic.h"
#include "limine.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

__attribute__((used, section(".limine_reqs")))
volatile uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_reqs_start")))
volatile uint64_t limine_requests_start_marker[4] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_reqs")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0,
};

__attribute__((used, section(".limine_reqs")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0,
};

__attribute__((used, section(".limine_reqs")))
volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0,
};

__attribute__((used, section(".limine_reqs")))
volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0,
};

__attribute__((used, section(".limine_reqs_end")))
volatile uint64_t limine_requests_end_marker[2] = LIMINE_REQUESTS_END_MARKER;

void kernel_main(void)
{
    serial_init();

    serial_puts("\n============================================\n");
    serial_puts("            N E X U S   O S\n");
    serial_puts("        Phase 1: Kernel Foundation\n");
    serial_puts("============================================\n\n");

    serial_puts("[boot] CPU entered long mode (64-bit).\n");

    if (bootloader_info_request.response != NULL
        && bootloader_info_request.response->name != NULL) {
        serial_puts("[boot] Bootloader: ");
        serial_puts(bootloader_info_request.response->name);
        if (bootloader_info_request.response->version != NULL) {
            serial_puts(" ");
            serial_puts(bootloader_info_request.response->version);
        }
        serial_puts("\n");
    }

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        serial_puts("[boot] Limine base revision honoured.\n");
    }

    if (executable_address_request.response != NULL) {
        serial_puts("[boot] Kernel loaded: phys=");
        serial_put_hex(executable_address_request.response->physical_base);
        serial_puts(" virt=");
        serial_put_hex(executable_address_request.response->virtual_base);
        serial_puts("\n");
    }

    serial_puts("[boot] COM1 serial initialised @ 115200 baud.\n");

    if (hhdm_request.response != NULL) {
        serial_puts("[boot] Higher-half direct map offset=");
        serial_put_hex(hhdm_request.response->offset);
        serial_puts("\n");
    }

    serial_puts("\n[ok] Kernel started successfully.\n");
    serial_puts("[ok] Phase 1 boot verified. Halting CPU.\n");

    for (;;)
        __asm__ volatile("hlt");
}
