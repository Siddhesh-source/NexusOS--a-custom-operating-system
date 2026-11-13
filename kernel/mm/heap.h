#ifndef BHARAT_HEAP_H
#define BHARAT_HEAP_H

/* Kernel heap: a first-fit allocator over one contiguous virtual region
 * starting at KERNEL_HEAP_BASE.
 *
 * The region is tiled by blocks, each a 32-byte header followed by its
 * payload, kept on an address-ordered doubly-linked list. Allocation splits
 * the first free block that fits; freeing coalesces with free neighbours.
 * When nothing fits, the heap grows by mapping fresh PMM frames at its end
 * (up to KERNEL_HEAP_MAX). Pages are never returned to the PMM.
 *
 * Every payload is at least 16-byte aligned. Not SMP-safe.
 *
 * Memory error checks (a light version of Linux's KASAN):
 *   - free memory is filled with HEAP_POISON_FREE. Handing it out again
 *     checks the fill; a changed byte means use after free -> panic.
 *   - each allocation is followed by a red zone of at least HEAP_REDZONE
 *     bytes filled with HEAP_POISON_REDZONE; kfree checks it, a changed
 *     byte means a write past the end -> panic.
 *   - heap_check() reports both without panicking.
 * New allocations are not zeroed: they hold HEAP_POISON_FREE, which makes
 * use of uninitialised memory easier to spot. Use kzalloc for zeroes.
 * Limits: writes that skip over the red zone, and stray reads, are not
 * caught. */

#include "mm.h"

#define HEAP_MIN_ALIGN       16
#define HEAP_MAX_ALIGN       PAGE_SIZE
#define HEAP_REDZONE         16
#define HEAP_POISON_FREE     0xDE
#define HEAP_POISON_REDZONE  0xFB

typedef enum {
    HEAP_OK = 0,
    HEAP_ERR_INVALID_PTR,   /* not a pointer returned by kmalloc */
    HEAP_ERR_DOUBLE_FREE,   /* block is already free */
} heap_status_t;

struct heap_stats {
    uint64_t mapped_bytes;   /* virtual space currently backed by frames */
    uint64_t used_bytes;     /* payload bytes in allocated blocks */
    uint64_t free_bytes;     /* payload bytes in free blocks */
    uint64_t block_count;
    uint64_t free_block_count;
    uint64_t largest_free;
    uint64_t alloc_failures; /* kmalloc calls that returned NULL (size > 0) */
};

void heap_init(void);

/* Returns NULL for size 0, on bad alignment, or when out of memory. */
void *kmalloc(size_t size);
void *kzalloc(size_t size);
/* `align` must be a power of two no larger than HEAP_MAX_ALIGN. */
void *kmalloc_aligned(size_t size, size_t align);

/* kfree(NULL) is a no-op. Invalid and double frees are rejected, reported,
 * and leave the heap unchanged. */
heap_status_t kfree(void *ptr);

void heap_get_stats(struct heap_stats *out);
void heap_print_stats(void);

/* Walk the heap and verify its invariants. Returns true if consistent. */
bool heap_check(void);

#endif /* BHARAT_HEAP_H */
