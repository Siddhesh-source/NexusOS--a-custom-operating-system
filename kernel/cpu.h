#ifndef NEXUS_CPU_H
#define NEXUS_CPU_H

#include <stdint.h>
#include <stdbool.h>

/* Thin wrappers around privileged x86-64 instructions. */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Short delay for slow legacy devices (PIC): write to an unused port. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

static inline uint64_t read_cr0(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

static inline void invlpg(uint64_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* Disable interrupts, returning the previous RFLAGS for irq_restore(). */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    if (flags & (1ULL << 9))
        __asm__ volatile("sti" : : : "memory");
}

/* Critical section for the rest of the enclosing scope (single CPU).
 *
 *     IRQ_GUARD();    // interrupts off until the function/block exits
 *
 * Restores the previous interrupt state on every exit path, including early
 * returns. With preemptive scheduling this is what keeps shared kernel data
 * (heap, PMM, VMM, stacks, scheduler queues) consistent: a thread cannot be
 * preempted halfway through modifying them. Guards nest. For SMP this is the
 * single place to add a spinlock alongside the interrupt disable. */
static inline void irq_guard_release(uint64_t *flags)
{
    irq_restore(*flags);
}

#define IRQ_GUARD_CONCAT2(a, b) a##b
#define IRQ_GUARD_CONCAT(a, b)  IRQ_GUARD_CONCAT2(a, b)
#define IRQ_GUARD() \
    uint64_t IRQ_GUARD_CONCAT(irq_guard_, __LINE__) \
        __attribute__((cleanup(irq_guard_release), unused)) = irq_save()

static inline bool interrupts_enabled(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & (1ULL << 9)) != 0;
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void __attribute__((noreturn)) halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

#define CR0_WP        (1ULL << 16)
#define CR4_PGE       (1ULL << 7)
#define MSR_EFER      0xC0000080u
#define EFER_NXE      (1ULL << 11)

#endif /* NEXUS_CPU_H */
