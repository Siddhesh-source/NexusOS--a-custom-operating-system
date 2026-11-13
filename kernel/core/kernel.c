#include "kernel.h"
#include "serial.h"
#include "panic.h"
#include "limine.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "mm.h"
#include "vmm.h"
#include "mm_test.h"
#include "kstack.h"
#include "proc.h"
#include "proc_test.h"
#include "sched.h"
#include "sched_test.h"
#include "debug_test.h"
#include "bench.h"
#include "fbcon.h"
#include "acpi.h"
#include "pci.h"
#include "hw_test.h"
#include "apic.h"
#include "clock.h"
#include "vm_test.h"
#include "kprintf.h"
#include "string.h"
#include "cpu.h"
#include "percpu.h"
#include "ipi.h"
#include "smp.h"
#include "smp_test.h"
#include "input.h"
#include "inputdev.h"
#include "input_test.h"
#include "fs.h"
#include "fs_test.h"
#include "shell_test.h"
#include "selftest.h"
#include "console.h"

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

__attribute__((used, section(".limine_reqs")))
volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID, .revision = 0,
};

__attribute__((used, section(".limine_reqs")))
volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID, .revision = 0, .flags = 0,   /* xAPIC mode */
};

__attribute__((used, section(".limine_reqs")))
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID, .revision = 0,
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
__attribute__((noinline)) static void run_fault_demo(void)
{
#if defined(BHARAT_FAULT_DEMO_PF)
    kprintf("\n[demo] Writing to unmapped kernel address %p...\n", (void *)(MM_TEST_VBASE + 0xdead000));
    *(volatile uint64_t *)(MM_TEST_VBASE + 0xdead000) = 1;
#elif defined(BHARAT_FAULT_DEMO_NULL)
    kprintf("\n[demo] Dereferencing a null pointer...\n");
    (void)*(volatile uint64_t *)0;
#elif defined(BHARAT_FAULT_DEMO_STACK)
    kprintf("\n[demo] Overflowing the boot stack into its guard page...\n");
    test_stack_overflow();
#elif defined(BHARAT_FAULT_DEMO_DE)
    kprintf("\n[demo] Testing divide-by-zero exception...\n");
    test_divide_by_zero();
#elif defined(BHARAT_FAULT_DEMO_UD)
    kprintf("\n[demo] Testing invalid-opcode exception...\n");
    test_invalid_opcode();
#elif defined(BHARAT_FAULT_DEMO_DF)
    kprintf("\n[demo] Testing double-fault exception...\n");
    test_double_fault();
#elif defined(BHARAT_FAULT_DEMO_CTX)
    proc_demo_corrupt_context();
#elif defined(BHARAT_FAULT_DEMO_TSTACK)
    proc_demo_thread_stack_overflow();
#elif defined(BHARAT_FAULT_DEMO_PANIC)
    debug_demo_panic();
#elif defined(BHARAT_FAULT_DEMO_SMASH)
    debug_demo_stack_smash();
#elif defined(BHARAT_FAULT_DEMO_UAF)
    debug_demo_use_after_free();
#elif defined(BHARAT_FAULT_DEMO_OVERFLOW)
    debug_demo_heap_overflow();
#elif defined(BHARAT_FAULT_DEMO_LOCKORDER)
    smp_demo_lock_inversion();
#endif
}

/* Fill the in-memory file system from the boot bundle: a tar archive
 * that Limine loads next to the kernel (boot/limine.conf). Without it the
 * system still runs, with empty folders. */
static bool fs_bundle_loaded;

static void load_boot_bundle(void)
{
    fs_init();
    struct limine_module_response *r = module_request.response;
    for (uint64_t i = 0; r != NULL && i < r->module_count; i++) {
        struct limine_file *f = r->modules[i];
        if (f->string == NULL || strcmp(f->string, "bundle") != 0)
            continue;
        /* Modules sit next to the kernel image, which the direct map
         * leaves out on purpose; map just the bundle, read-only. */
        const void *data = vmm_map_phys((uint64_t)f->address - hhdm_offset, f->size, 0);
        if (data == NULL) {
            kprintf("[fs] cannot map the boot bundle; starting with empty folders\n");
            return;
        }
        int n = fs_load_tar(data, f->size);
        if (n < 0) {
            kprintf("[fs] boot bundle is damaged; some files may be missing\n");
        } else {
            kprintf("[fs] Files in memory: %d files loaded from the boot bundle (%lu bytes)\n",
                    n, f->size);
            fs_bundle_loaded = true;
        }
        return;
    }
    kprintf("[fs] no boot bundle; starting with empty folders\n");
}

/* Mirror the console to the screen, if the bootloader set up a
 * framebuffer we can draw on. */
static void start_screen_console(void)
{
    struct limine_framebuffer_response *r = framebuffer_request.response;
    if (r == NULL || r->framebuffer_count == 0)
        return;
    struct limine_framebuffer *f = r->framebuffers[0];
    if (!fbcon_init(f->address, f->width, f->height, f->pitch, f->bpp,
                    f->red_mask_shift, f->green_mask_shift, f->blue_mask_shift))
        serial_puts("[fbcon] framebuffer format not supported; serial only\n");
}

void kernel_main(void)
{
    /* Per-CPU data first: locks and the console need cpu_id(). */
    percpu_init_boot();
    serial_init();
    start_screen_console();

    serial_puts("\n============================================\n");
    serial_puts("           B H A R A T   O S\n");
    serial_puts("  Phase 10: V1 core (input, files, login, terminal)\n");
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

    /* Phase 7: discover the hardware (needs the memory manager to map
     * firmware tables and device registers). */
    acpi_init(rsdp_request.response != NULL ? (uint64_t)rsdp_request.response->address : 0);
    pci_init();

    bool mm_ok = mm_run_tests();

    /* Phase 4: processes, threads, context switching */
    kstack_init();
    proc_init();
    bool proc_ok = proc_run_tests();

    /* Phase 7: leave the 1980s timer behind. Mask the PIC, turn on the
     * local and I/O APICs, calibrate the clock, and give the scheduler a
     * one-shot alarm instead of a periodic tick. */
    if (!apic_init())
        panic("this machine has no APIC described by ACPI; BharatOS needs one");
    ipi_init();
    clock_init(sched_timer_event);

    /* Phase 5: preemptive scheduling. The boot flow (TID 0) is already the
     * registered current thread; from here on it is scheduled like any
     * other thread. */
    sched_init();
    sched_start();

    /* Phase 9: wake the other CPUs. They idle until there is work for
     * them. The Phase 5-8 tests below check single-CPU behaviour (exact
     * round-robin order, one queue's fairness), so work stealing stays
     * off until the multi-core tests turn it on: their threads all stay
     * on this CPU while the others are online and idle. */
    smp_init((void *)mp_request.response);

    bool sched_ok = sched_run_tests();

    /* Phase 6: debugging and hardening tools. They are active from boot;
     * this only checks them. */
    bool debug_ok = debug_run_tests();

    /* Phase 7: modern hardware (ACPI, PCI, screen console, APIC timer). */
    bool hw_ok = hw_run_tests();

    /* Phase 8: buddy allocator, slab caches, demand paging, copy-on-write. */
    bool vm_ok = vm_run_tests();

    /* Phase 9: multi-core (locks, lock order, IPIs, TLB shootdown, RCU,
     * per-CPU queues with work stealing). */
    bool smp_ok = smp_run_tests();

    /* Phase 10: V1 core. Keyboard, mouse and serial input. */
    inputdev_init();
    bool input_ok = input_run_tests();

    /* Phase 10: files in memory, seeded from the boot bundle. */
    load_boot_bundle();
    bool fs_ok = fs_run_tests(fs_bundle_loaded);
    {
        struct fbcon_info fi;
        fbcon_get_info(&fi);
        if (fi.active)
            input_set_screen((int32_t)fi.width, (int32_t)fi.height);
    }

    /* Phase 10: login rules, line editor and the shell's commands. */
    bool shell_ok = shell_run_tests();

    serial_puts("\n[ok] Kernel started successfully.\n");
    if (mm_ok)
        serial_puts("[ok] Phase 3 memory management verified.\n");
    else
        serial_puts("[FAIL] Phase 3 memory tests reported failures.\n");
    if (proc_ok)
        serial_puts("[ok] Phase 4 process management verified.\n");
    else
        serial_puts("[FAIL] Phase 4 process tests reported failures.\n");
    if (sched_ok)
        serial_puts("[ok] Phase 5 scheduler verified.\n");
    else
        serial_puts("[FAIL] Phase 5 scheduler tests reported failures.\n");
    if (debug_ok)
        serial_puts("[ok] Phase 6 debug and hardening verified.\n");
    else
        serial_puts("[FAIL] Phase 6 debug tests reported failures.\n");
    if (hw_ok)
        serial_puts("[ok] Phase 7 modern hardware verified.\n");
    else
        serial_puts("[FAIL] Phase 7 hardware tests reported failures.\n");
    if (vm_ok)
        serial_puts("[ok] Phase 8 memory that scales verified.\n");
    else
        serial_puts("[FAIL] Phase 8 memory tests reported failures.\n");
    if (smp_ok)
        serial_puts("[ok] Phase 9 multi-core verified.\n");
    else
        serial_puts("[FAIL] Phase 9 multi-core tests reported failures.\n");
    if (input_ok)
        serial_puts("[ok] Phase 10 input verified.\n");
    else
        serial_puts("[FAIL] Phase 10 input tests reported failures.\n");
    if (fs_ok)
        serial_puts("[ok] Phase 10 files in memory verified.\n");
    else
        serial_puts("[FAIL] Phase 10 file tests reported failures.\n");
    if (shell_ok)
        serial_puts("[ok] Phase 10 login and shell verified.\n");
    else
        serial_puts("[FAIL] Phase 10 login and shell tests reported failures.\n");
    {
        unsigned passed, failed;
        selftest_totals(&passed, &failed);
        kprintf("[ok] %u boot self-checks passed, %u failed.\n", passed, failed);
    }

    bench_run();          /* only does something in the bench build */
    run_fault_demo();

    /* From here on every CPU shares the work. */
    sched_set_stealing(true);

    /* V1: the console app (login, then the shell). kmain then just checks
     * on it once a second and restarts it if it crashed; it sleeps in
     * between, so the idle thread (not this loop) halts the CPU. */
    console_start();
    for (;;) {
        sched_sleep(SCHED_TIMER_HZ);
        console_watch();
    }
}
