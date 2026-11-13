#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "trace.h"

#define MAGIC_USED  0x55534544u   /* "USED" */
#define MAGIC_FREE  0x46524545u   /* "FREE" */

#define HEAP_INITIAL_PAGES 16     /* 64 KiB */

struct block {
    uint32_t magic;
    uint32_t requested;      /* bytes the caller asked for (used blocks) */
    uint64_t size;           /* payload bytes, multiple of HEAP_MIN_ALIGN;
                                includes the red zone after `requested` */
    struct block *prev;
    struct block *next;
};

#define HDR_SIZE        sizeof(struct block)
#define MIN_SPLIT       (HDR_SIZE + HEAP_MIN_ALIGN)

_Static_assert(sizeof(struct block) % HEAP_MIN_ALIGN == 0,
               "block header must preserve payload alignment");

static struct block *head;
static struct block *tail;
static uint64_t heap_end;         /* first unmapped byte */
static uint64_t alloc_failures;

static inline uint64_t payload_of(struct block *b)
{
    return (uint64_t)b + HDR_SIZE;
}

static inline uint64_t end_of(struct block *b)
{
    return payload_of(b) + b->size;
}

static inline uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* Insert `nb` into the list directly after `b`. */
static void link_after(struct block *b, struct block *nb)
{
    nb->prev = b;
    nb->next = b->next;
    if (b->next != NULL)
        b->next->prev = nb;
    else
        tail = nb;
    b->next = nb;
}

/* Merge `b->next` into `b`. Both must be free. The absorbed header becomes
 * free payload, so it is poisoned like the rest (which also stops the
 * stale header from looking like a block). */
static void absorb_next(struct block *b)
{
    struct block *n = b->next;
    b->size += HDR_SIZE + n->size;
    b->next = n->next;
    if (n->next != NULL)
        n->next->prev = b;
    else
        tail = b;
    memset(n, HEAP_POISON_FREE, HDR_SIZE);
}

/* ---- poisoning ----------------------------------------------------------
 * Invariant: every payload byte of a free block equals HEAP_POISON_FREE,
 * and every byte of a used block after `requested` equals
 * HEAP_POISON_REDZONE. A changed byte proves a stray write: into freed
 * memory (use after free) or past the end of an allocation (overflow). */

/* Offset of the first byte in [start, start+len) that is not `byte`, or
 * `len` if all match. Compares 8 bytes at a time where it can. */
static uint64_t first_changed(uint64_t start, uint64_t len, uint8_t byte)
{
    const uint8_t *p = (const uint8_t *)start;
    uint64_t i = 0;
    uint64_t word = 0x0101010101010101ULL * byte;
    while (i < len && ((start + i) & 7) != 0) {
        if (p[i] != byte)
            return i;
        i++;
    }
    while (i + 8 <= len && *(const uint64_t *)(p + i) == word)
        i += 8;
    while (i < len && p[i] == byte)
        i++;
    return i;
}

static uint64_t redzone_len(const struct block *b)
{
    return b->size - b->requested;
}

static bool redzone_intact(const struct block *b, uint64_t *bad)
{
    uint64_t off = first_changed(payload_of((struct block *)b) + b->requested,
                                 redzone_len(b), HEAP_POISON_REDZONE);
    *bad = payload_of((struct block *)b) + b->requested + off;
    return off == redzone_len(b);
}

static bool free_poison_intact(const struct block *b, uint64_t *bad)
{
    uint64_t off = first_changed(payload_of((struct block *)b), b->size, HEAP_POISON_FREE);
    *bad = payload_of((struct block *)b) + off;
    return off == b->size;
}

/* Map `pages` fresh frames at heap_end. All-or-nothing: on failure every
 * frame mapped by this call is unmapped and returned to the PMM. */
static bool map_heap_pages(uint64_t pages)
{
    address_space_t *ks = vmm_kernel_space();
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = heap_end + i * PAGE_SIZE;
        uint64_t pa = pmm_alloc_page();
        vmm_status_t st = VMM_ERR_NO_MEMORY;
        if (pa != 0)
            st = vmm_map_page(ks, va, pa, VMM_WRITE | VMM_GLOBAL);
        if (st != VMM_OK) {
            if (pa != 0)
                pmm_free_page(pa);
            while (i-- > 0) {
                uint64_t old;
                if (vmm_unmap_page(ks, heap_end + i * PAGE_SIZE, &old) == VMM_OK)
                    pmm_free_page(old);
            }
            return false;
        }
    }
    return true;
}

/* Extend the heap so that at least `bytes` more are available at its end.
 * The new space is merged into the trailing free block or becomes one. */
static bool grow(uint64_t bytes)
{
    uint64_t pages = PAGE_ALIGN_UP(bytes) / PAGE_SIZE;
    uint64_t mapped = heap_end - KERNEL_HEAP_BASE;
    if (pages > (KERNEL_HEAP_MAX - mapped) / PAGE_SIZE)
        return false;
    if (!map_heap_pages(pages))
        return false;

    uint64_t old_end = heap_end;
    heap_end += pages * PAGE_SIZE;
    trace_event(TRACE_HEAP_GROW, heap_end, pages);
    /* New space is free payload (or a header written just below). */
    memset((void *)old_end, HEAP_POISON_FREE, heap_end - old_end);

    if (tail != NULL && tail->magic == MAGIC_FREE) {
        tail->size += heap_end - old_end;
    } else {
        struct block *nb = (struct block *)old_end;
        nb->magic = MAGIC_FREE;
        nb->size = heap_end - old_end - HDR_SIZE;
        nb->prev = NULL;
        nb->next = NULL;
        if (tail == NULL) {
            head = tail = nb;
        } else {
            link_after(tail, nb);
        }
    }
    return true;
}

/* Find where an aligned payload of `size` bytes fits inside free block `b`.
 * If the aligned payload doesn't start at b's own payload, the gap in front
 * must be large enough to remain a valid free block. */
static bool fit(struct block *b, uint64_t size, uint64_t align, uint64_t *payload_out)
{
    uint64_t start = payload_of(b);
    uint64_t payload = align_up(start, align);
    if (payload != start && payload - start < MIN_SPLIT)
        payload = align_up(start + MIN_SPLIT, align);
    if (payload + size > end_of(b))
        return false;
    *payload_out = payload;
    return true;
}

/* Carve [payload, payload+size) out of free block `b`; returns the block.
 * The carved bytes must still hold the free poison: anything else means
 * code wrote to this memory after freeing it. */
static struct block *carve(struct block *b, uint64_t payload, uint64_t size)
{
    uint64_t off = first_changed(payload, size, HEAP_POISON_FREE);
    if (off != size) {
        kprintf("[heap] freed memory at %p was changed (byte is 0x%x, expected 0x%x)\n",
                (void *)(payload + off), *(const uint8_t *)(payload + off), HEAP_POISON_FREE);
        panic("heap: use after free detected: memory was written after it was freed");
    }

    if (payload != payload_of(b)) {
        /* Split off the leading gap; it stays free. */
        struct block *nb = (struct block *)(payload - HDR_SIZE);
        nb->magic = MAGIC_FREE;
        nb->size = end_of(b) - payload;
        link_after(b, nb);
        b->size = (uint64_t)nb - payload_of(b);
        b = nb;
    }
    if (b->size - size >= MIN_SPLIT) {
        /* Split off the trailing remainder. */
        struct block *rest = (struct block *)(payload + size);
        rest->magic = MAGIC_FREE;
        rest->size = b->size - size - HDR_SIZE;
        link_after(b, rest);
        b->size = size;
    }
    b->magic = MAGIC_USED;
    return b;
}

void heap_init(void)
{
    heap_end = KERNEL_HEAP_BASE;
    head = tail = NULL;
    if (!grow(HEAP_INITIAL_PAGES * PAGE_SIZE))
        panic("heap: cannot map initial heap pages");
    kprintf("[heap] Kernel heap at %p, %lu KiB mapped, limit %lu MiB\n",
            (void *)KERNEL_HEAP_BASE, (heap_end - KERNEL_HEAP_BASE) / 1024,
            KERNEL_HEAP_MAX >> 20);
}

void *kmalloc_aligned(size_t size, size_t align)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    if (size == 0)
        return NULL;
    if (align == 0 || (align & (align - 1)) != 0 || align > HEAP_MAX_ALIGN) {
        alloc_failures++;
        return NULL;
    }
    if (align < HEAP_MIN_ALIGN)
        align = HEAP_MIN_ALIGN;
    if (size > KERNEL_HEAP_MAX) {
        alloc_failures++;
        return NULL;
    }
    uint64_t requested = size;
    /* Room for at least HEAP_REDZONE guard bytes after the caller's data. */
    size = align_up(size + HEAP_REDZONE, HEAP_MIN_ALIGN);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = head; b != NULL; b = b->next) {
            uint64_t payload;
            if (b->magic == MAGIC_FREE && fit(b, size, align, &payload)) {
                struct block *nb = carve(b, payload, size);
                nb->requested = (uint32_t)requested;
                memset((void *)(payload + requested), HEAP_POISON_REDZONE,
                       redzone_len(nb));
                return (void *)payload;
            }
        }
        /* Worst case the aligned payload needs a split-off gap in front of
         * it plus alignment slack, in addition to its own header. */
        if (attempt == 0 && !grow(size + align + MIN_SPLIT + HDR_SIZE))
            break;
    }

    alloc_failures++;
    return NULL;
}

void *kmalloc(size_t size)
{
    return kmalloc_aligned(size, HEAP_MIN_ALIGN);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p != NULL)
        memset(p, 0, size);
    return p;
}

/* Resolve a payload pointer to its block, or NULL if it is not one. Walks
 * the list, so arbitrary garbage pointers are never dereferenced. */
static struct block *find_block(void *ptr)
{
    uint64_t p = (uint64_t)ptr;
    if (p < KERNEL_HEAP_BASE + HDR_SIZE || p >= heap_end || (p % HEAP_MIN_ALIGN) != 0)
        return NULL;
    for (struct block *b = head; b != NULL; b = b->next) {
        if (payload_of(b) == p)
            return b;
        if ((uint64_t)b > p)
            break;
    }
    return NULL;
}

heap_status_t kfree(void *ptr)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    if (ptr == NULL)
        return HEAP_OK;

    struct block *b = find_block(ptr);
    if (b == NULL) {
        kprintf("[heap] rejected kfree(%p): not a heap allocation\n", ptr);
        return HEAP_ERR_INVALID_PTR;
    }
    if (b->magic == MAGIC_FREE) {
        kprintf("[heap] rejected kfree(%p): double free\n", ptr);
        return HEAP_ERR_DOUBLE_FREE;
    }
    if (b->magic != MAGIC_USED)
        panic("heap: corrupted block header");

    uint64_t bad;
    if (!redzone_intact(b, &bad)) {
        kprintf("[heap] allocation %p of %u bytes: byte %p past its end was changed\n",
                ptr, b->requested, (void *)bad);
        panic("heap: overflow detected: a write went past the end of an allocation");
    }

    b->magic = MAGIC_FREE;
    b->requested = 0;
    memset(ptr, HEAP_POISON_FREE, b->size);
    if (b->next != NULL && b->next->magic == MAGIC_FREE)
        absorb_next(b);
    if (b->prev != NULL && b->prev->magic == MAGIC_FREE)
        absorb_next(b->prev);
    return HEAP_OK;
}

void heap_get_stats(struct heap_stats *out)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    memset(out, 0, sizeof(*out));
    out->mapped_bytes = heap_end - KERNEL_HEAP_BASE;
    out->alloc_failures = alloc_failures;
    for (struct block *b = head; b != NULL; b = b->next) {
        out->block_count++;
        if (b->magic == MAGIC_FREE) {
            out->free_block_count++;
            out->free_bytes += b->size;
            if (b->size > out->largest_free)
                out->largest_free = b->size;
        } else {
            out->used_bytes += b->size;
        }
    }
}

void heap_print_stats(void)
{
    struct heap_stats s;
    heap_get_stats(&s);
    kprintf("[heap] mapped=%lu KiB used=%lu B free=%lu B blocks=%lu (free %lu) "
            "largest_free=%lu B failures=%lu\n",
            s.mapped_bytes / 1024, s.used_bytes, s.free_bytes, s.block_count,
            s.free_block_count, s.largest_free, s.alloc_failures);
}

bool heap_check(void)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    uint64_t expect = KERNEL_HEAP_BASE;
    struct block *prev = NULL;
    for (struct block *b = head; b != NULL; b = b->next) {
        if ((uint64_t)b != expect || b->prev != prev) {
            kprintf("[heap] check: block %p breaks tiling/links\n", (void *)b);
            return false;
        }
        if (b->magic != MAGIC_USED && b->magic != MAGIC_FREE) {
            kprintf("[heap] check: block %p has bad magic %x\n", (void *)b, b->magic);
            return false;
        }
        if (prev != NULL && prev->magic == MAGIC_FREE && b->magic == MAGIC_FREE) {
            kprintf("[heap] check: adjacent free blocks at %p not coalesced\n", (void *)b);
            return false;
        }
        uint64_t bad;
        if (b->magic == MAGIC_FREE && !free_poison_intact(b, &bad)) {
            kprintf("[heap] check: freed memory at %p was written after free\n", (void *)bad);
            return false;
        }
        if (b->magic == MAGIC_USED && !redzone_intact(b, &bad)) {
            kprintf("[heap] check: red zone byte %p of block %p was overwritten\n",
                    (void *)bad, (void *)payload_of(b));
            return false;
        }
        expect = end_of(b);
        prev = b;
    }
    if (expect != heap_end || prev != tail) {
        kprintf("[heap] check: blocks end at %p, heap ends at %p\n",
                (void *)expect, (void *)heap_end);
        return false;
    }
    return true;
}
