#ifndef NEXUS_PAGEFAULT_H
#define NEXUS_PAGEFAULT_H

#include <stdint.h>
#include <stdbool.h>
#include "interrupts.h"

/* Decoded #PF error code (Intel SDM Vol. 3, 4.7). */
#define PF_ERR_PRESENT   (1u << 0)   /* 0 = not-present page, 1 = protection */
#define PF_ERR_WRITE     (1u << 1)   /* 0 = read, 1 = write */
#define PF_ERR_USER      (1u << 2)   /* 0 = supervisor, 1 = user mode */
#define PF_ERR_RSVD      (1u << 3)   /* reserved bit set in a paging entry */
#define PF_ERR_IFETCH    (1u << 4)   /* instruction fetch */

struct page_fault_info {
    uint64_t address;     /* CR2: faulting virtual address */
    uint64_t rip;         /* instruction that faulted */
    uint64_t error_code;
    bool present;         /* page was present (protection violation) */
    bool write;
    bool user;
    bool instruction_fetch;
    bool reserved_bit;
    bool recovered;       /* resumed via a probe fixup */
};

/* Last page fault seen and total count, for tests and diagnostics. */
extern struct page_fault_info pf_last;
extern volatile uint64_t pf_count;

/* #PF entry point. Reports the fault; resumes only if it came from one of
 * the mm_probe_* routines below, otherwise halts the system (no demand
 * paging, swapping or copy-on-write exists yet). */
void page_fault_handler(struct interrupt_frame *frame);

/* If `frame` faulted inside an mm_probe_* routine, redirect it to return
 * 1 to the probe's caller and return true. */
bool fault_try_fixup(struct interrupt_frame *frame);

/* Probed accesses (probe.asm). Each returns 0 on success, or 1 if the access
 * faulted (#PF or #GP); the fault is still fully reported. */
int mm_probe_read(uint64_t addr, uint64_t *value_out);
int mm_probe_write(uint64_t addr, uint64_t value);
int mm_probe_exec(uint64_t addr);   /* call addr; target must be `ret` */

#endif /* NEXUS_PAGEFAULT_H */
