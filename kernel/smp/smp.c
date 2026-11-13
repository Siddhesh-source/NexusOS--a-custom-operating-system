#include "smp.h"
#include "percpu.h"
#include "limine.h"
#include "gdt.h"
#include "idt.h"
#include "apic.h"
#include "clock.h"
#include "vmm.h"
#include "sched.h"
#include "proc.h"
#include "ipi.h"
#include "cpu.h"
#include "kprintf.h"

#define START_TIMEOUT_NS 1000000000ULL   /* 1 s per CPU */

extern void ap_entry(struct limine_mp_info *info);
void ap_main(struct percpu *c) __attribute__((noreturn));

/* First C code on an AP, on its idle thread's stack, interrupts off.
 * Until gdt_init_cpu loads GS, this_cpu() must not be used. */
void ap_main(struct percpu *c)
{
    gdt_init_cpu(c->id);
    idt_load();
    apic_init_cpu();
    clock_init_cpu();
    c->space = vmm_kernel_space();
    __atomic_fetch_or(&c->space->cpus, 1u << c->id, __ATOMIC_SEQ_CST);
    smp_mark_online(c);
    sched_ap_main();
}

static bool start_cpu(struct limine_mp_info *info, unsigned id)
{
    struct percpu *c = cpu_at(id);
    struct thread *idle = thread_create_idle(id);
    if (idle == NULL) {
        kprintf("[smp] no memory for CPU %u's idle thread\n", id);
        return false;
    }
    c->lapic_id = info->lapic_id;
    c->idle = idle;
    c->current = idle;
    c->boot_stack = idle->stack.top;
    sched_register_idle(id, idle);

    info->extra_argument = (uint64_t)c;
    __atomic_store_n(&info->goto_address, (limine_goto_address)ap_entry, __ATOMIC_SEQ_CST);

    uint64_t deadline = clock_now_ns() + START_TIMEOUT_NS;
    while (!__atomic_load_n(&c->online, __ATOMIC_ACQUIRE)) {
        if (clock_now_ns() > deadline) {
            kprintf("[smp] CPU %u (APIC id %u) did not start\n", id, info->lapic_id);
            return false;
        }
        cpu_relax();
    }
    return true;
}

unsigned smp_init(void *mp_response)
{
    struct limine_mp_response *r = mp_response;
    cpu_at(0)->lapic_id = lapic_id();
    if (r == NULL || r->cpu_count <= 1) {
        kprintf("[smp] 1 CPU online (no other CPUs reported)\n");
        return 1;
    }

    unsigned next = 1, skipped = 0;
    for (uint64_t i = 0; i < r->cpu_count; i++) {
        struct limine_mp_info *info = r->cpus[i];
        if (info->lapic_id == r->bsp_lapic_id)
            continue;
        if (next == SMP_MAX_CPUS) {
            skipped++;
            continue;
        }
        start_cpu(info, next);     /* a CPU that fails keeps its number */
        next++;
    }
    kprintf("[smp] %u CPUs online (boot CPU APIC id %u", smp_cpu_count(), r->bsp_lapic_id);
    for (unsigned i = 1; i < next; i++)
        if (cpu_at(i)->online)
            kprintf(", CPU %u APIC id %u", i, cpu_at(i)->lapic_id);
    kprintf(")\n");
    if (skipped)
        kprintf("[smp] %u more CPU(s) left parked (limit %d)\n", skipped, SMP_MAX_CPUS);
    return smp_cpu_count();
}
