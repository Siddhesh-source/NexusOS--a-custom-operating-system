#include "kernel.h"
#include "serial.h"
#include "panic.h"
#include "limine.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "mm.h"
#include "mm_test.h"
#include "kstack.h"
#include "proc.h"
#include "proc_test.h"
#include "kprintf.h"
#include "cpu.h"

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

__attribute__((used, section(".limine_reqs")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0,
};

__attribute__((used, section(".limine_reqs_end")))
volatile uint64_t limine_requests_end_marker[2] = LIMINE_REQUESTS_END_MARKER;

/* Translate the Limine memory map into the boot-agnostic form used by the
 * memory manager. The copy lives in .bss, so it stays valid regardless of
 * what later happens to bootloader-reclaimable memory. */
static struct mem_region boot_regions[MM_MAX_REGIONS];

static void collect_boot_mem_info(struct boot_mem_info *info)
{
    if (memmap_request.response == NULL || hhdm_request.response == NULL
        || executable_address_request.response == NULL)
        panic("bootloader did not provide memmap/HHDM/executable address");

    struct limine_memmap_response *mm = memmap_request.response;
    size_t n = mm->entry_count;
    if (n > MM_MAX_REGIONS)
        panic("memory map has too many entries");

    for (size_t i = 0; i < n; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        enum mem_region_type t;
        switch (e->type) {
        case LIMINE_MEMMAP_USABLE:                 t = MEM_USABLE; break;
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       t = MEM_ACPI_RECLAIMABLE; break;
        case LIMINE_MEMMAP_ACPI_NVS:               t = MEM_ACPI_NVS; break;
        case LIMINE_MEMMAP_BAD_MEMORY:             t = MEM_BAD; break;
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: t = MEM_BOOTLOADER_RECLAIMABLE; break;
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: t = MEM_KERNEL_AND_MODULES; break;
        case LIMINE_MEMMAP_FRAMEBUFFER:            t = MEM_FRAMEBUFFER; break;
        default:                                   t = MEM_RESERVED; break;
        }
        boot_regions[i].base = e->base;
        boot_regions[i].length = e->length;
        boot_regions[i].type = t;
    }

    info->regions = boot_regions;
    info->region_count = n;
    info->hhdm_offset = hhdm_request.response->offset;
    info->kernel_phys_base = executable_address_request.response->physical_base;
    info->kernel_virt_base = executable_address_request.response->virtual_base;
}

/* Opt-in fatal exception demos, selected at build time with
 * `python build.py run --fault-demo <name>`. Each one halts the kernel. */
static void run_fault_demo(void)
{
#if defined(NEXUS_FAULT_DEMO_PF)
    kprintf("\n[demo] Writing to unmapped kernel address %p...\n", (void *)(MM_TEST_VBASE + 0xdead000));
    *(volatile uint64_t *)(MM_TEST_VBASE + 0xdead000) = 1;
#elif defined(NEXUS_FAULT_DEMO_NULL)
    kprintf("\n[demo] Dereferencing a null pointer...\n");
    (void)*(volatile uint64_t *)0;
#elif defined(NEXUS_FAULT_DEMO_STACK)
    kprintf("\n[demo] Overflowing the boot stack into its guard page...\n");
    test_stack_overflow();
#elif defined(NEXUS_FAULT_DEMO_DE)
    kprintf("\n[demo] Testing divide-by-zero exception...\n");
    test_divide_by_zero();
#elif defined(NEXUS_FAULT_DEMO_UD)
    kprintf("\n[demo] Testing invalid-opcode exception...\n");
    test_invalid_opcode();
#elif defined(NEXUS_FAULT_DEMO_DF)
    kprintf("\n[demo] Testing double-fault exception...\n");
    test_double_fault();
#elif defined(NEXUS_FAULT_DEMO_CTX)
    proc_demo_corrupt_context();
#elif defined(NEXUS_FAULT_DEMO_TSTACK)
    proc_demo_thread_stack_overflow();
#endif
}

void kernel_main(void)
{
    serial_init();

    serial_puts("\n============================================\n");
    serial_puts("            N E X U S   O S\n");
    serial_puts("   Phase 4: Processes, Threads & Switching\n");
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

    if (hhdm_request.response != NULL) {
        serial_puts("[boot] Higher-half direct map offset=");
        serial_put_hex(hhdm_request.response->offset);
        serial_puts("\n");
    }

    /* Phase 2: descriptor tables, exceptions, timer */
    serial_puts("[init] Setting up GDT...\n");
    gdt_init();

    serial_puts("[init] Setting up IDT...\n");
    idt_init();

    serial_puts("[init] Setting up timer...\n");
    timer_init();

    serial_puts("[init] Enabling interrupts...\n");
    __asm__ volatile("sti");
    serial_puts("[init] Interrupts enabled.\n");

    /* Phase 3: memory management */
    struct boot_mem_info mem_info;
    collect_boot_mem_info(&mem_info);
    mm_init(&mem_info);

    bool mm_ok = mm_run_tests();

    /* Phase 4: processes, threads, context switching */
    kstack_init();
    proc_init();
    bool proc_ok = proc_run_tests();

    serial_puts("\n[ok] Kernel started successfully.\n");
    if (mm_ok)
        serial_puts("[ok] Phase 3 memory management verified.\n");
    else
        serial_puts("[FAIL] Phase 3 memory tests reported failures.\n");
    if (proc_ok)
        serial_puts("[ok] Phase 4 process management verified.\n");
    else
        serial_puts("[FAIL] Phase 4 process tests reported failures.\n");

    run_fault_demo();

    /* Idle loop - wait for interrupts */
    for (;;)
        __asm__ volatile("hlt");
}
