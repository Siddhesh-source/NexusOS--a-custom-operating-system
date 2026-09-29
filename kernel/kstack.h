#ifndef NEXUS_KSTACK_H
#define NEXUS_KSTACK_H

/* Kernel thread stacks.
 *
 * The region at KSTACK_REGION_BASE is divided into fixed 32 KiB slots. A
 * stack occupies the top KSTACK_SIZE bytes of its slot; the rest of the slot
 * below it is never mapped, so running off the bottom of a stack hits an
 * unmapped guard area (#PF, escalating to #DF on the IST stack) instead of
 * silently corrupting a neighbouring stack.
 *
 *   slot i:  [ guard (unmapped, 16 KiB) | stack (mapped RW NX, 16 KiB) ]
 *            ^ slot base                 ^ kstack.base           kstack.top ^
 *
 * Stack pages are PMM frames mapped through the VMM into the shared kernel
 * half, so a thread's stack is valid in every address space. Stacks are
 * zeroed on allocation and carry a canary in their lowest qword. */

#include "mm.h"

#define KSTACK_SIZE        (16 * 1024)
#define KSTACK_SLOT_SIZE   (32 * 1024)
#define KSTACK_MAX_SLOTS   256
#define KSTACK_CANARY      0x4B53544B43414E59ULL   /* "KSTKCANY" */

typedef enum {
    KSTACK_OK = 0,
    KSTACK_ERR_NO_SLOT,      /* all slots in use */
    KSTACK_ERR_NO_MEMORY,    /* PMM exhausted */
    KSTACK_ERR_INVALID,      /* not a stack returned by kstack_alloc */
} kstack_status_t;

struct kstack {
    uint64_t base;   /* lowest usable address (holds the canary) */
    uint64_t top;    /* one past the highest address; 16-byte aligned */
    int slot;        /* slot index, or -1 for a stack not owned by us */
};

void kstack_init(void);

/* All-or-nothing: on failure nothing stays allocated or mapped. */
kstack_status_t kstack_alloc(struct kstack *out);

/* Unmap and release a stack. Must not be the stack currently in use. */
kstack_status_t kstack_free(struct kstack *ks);

bool kstack_canary_intact(const struct kstack *ks);

/* Deepest stack usage so far, found by scanning up from the base for the
 * first non-zero qword (stacks start zeroed). */
uint64_t kstack_high_water(const struct kstack *ks);

unsigned kstack_slots_in_use(void);
const char *kstack_status_str(kstack_status_t s);

#endif /* NEXUS_KSTACK_H */
