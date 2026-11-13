#include "percpu.h"
#include "cpu.h"

#define MSR_GS_BASE 0xC0000101u

static struct percpu cpus[SMP_MAX_CPUS];
static volatile uint32_t online_mask;

void percpu_load_gs(struct percpu *c)
{
    wrmsr(MSR_GS_BASE, (uint64_t)c);
}

void percpu_init_boot(void)
{
    for (unsigned i = 0; i < SMP_MAX_CPUS; i++) {
        cpus[i].self = &cpus[i];
        cpus[i].id = i;
    }
    percpu_load_gs(&cpus[0]);
    smp_mark_online(&cpus[0]);
}

struct percpu *cpu_at(unsigned id)
{
    return id < SMP_MAX_CPUS ? &cpus[id] : (struct percpu *)0;
}

void smp_mark_online(struct percpu *c)
{
    __atomic_store_n(&c->online, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_or(&online_mask, 1u << c->id, __ATOMIC_SEQ_CST);
}

uint32_t smp_online_mask(void)
{
    return __atomic_load_n(&online_mask, __ATOMIC_ACQUIRE);
}

unsigned smp_cpu_count(void)
{
    return (unsigned)__builtin_popcount(smp_online_mask());
}
