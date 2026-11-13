#ifndef BHARAT_VM_H
#define BHARAT_VM_H

/* Memory objects, regions, demand paging and copy-on-write.
 *
 *   memory object (vm_object)  some pages of memory, owned by nobody in
 *                              particular: counted references keep it
 *                              alive. Its pages are allocated only when
 *                              first touched. In Phase 10 a handle will
 *                              point to one; for now kernel code holds it.
 *   region (vm_region)         a range of a user address space showing
 *                              (part of) one object, with permissions.
 *
 * The page-fault handler asks vm_handle_fault() first:
 *   - a fault on a page not yet mapped inside a region -> allocate it
 *     (zero-filled) if the object has no page there yet, and map it:
 *     DEMAND PAGING;
 *   - a write to a read-only page of a writable region whose frame is
 *     shared by a copy-on-write clone -> copy the frame, or, if this was
 *     the last sharer, simply make it writable: COPY-ON-WRITE.
 * Anything else (no region, or a forbidden access) is a real fault.
 *
 * Mapping the same object in two address spaces shares its memory (the
 * basis for shared memory in Phase 11). vm_object_clone_cow() makes a
 * new object that starts with the same contents but separates on the
 * first write to each page (for checkpoints and the file system).
 *
 * Limits: user half only; 4 KiB pages; regions kept in a sorted list;
 * single CPU (IRQ_GUARD). */

#include <stdint.h>
#include <stdbool.h>
#include "vmm.h"

#define VM_READ   0u              /* every mapped page is readable */
#define VM_WRITE  VMM_WRITE
#define VM_EXEC   VMM_EXEC
#define VM_MAX_OBJECT_PAGES (1u << 18)       /* 1 GiB per object */

typedef enum {
    VM_OK = 0,
    VM_ERR_INVALID,      /* bad address, size, flags or object */
    VM_ERR_OVERLAP,      /* range overlaps an existing region */
    VM_ERR_NOT_FOUND,    /* no region starts at that address */
    VM_ERR_NO_MEMORY,
} vm_status_t;

struct vm_object;
struct vm_region;

struct vm_stats {
    uint64_t objects, regions;
    uint64_t pages_committed;      /* pages actually allocated, all objects */
    uint64_t demand_faults;        /* pages filled in on first touch */
    uint64_t cow_copies;           /* shared pages copied on write */
    uint64_t cow_reuses;           /* last sharer: made writable, no copy */
};

/* A new object of `pages` pages, nothing allocated yet; the caller holds
 * one reference. */
struct vm_object *vm_object_create(uint64_t pages);
void vm_object_ref(struct vm_object *o);
/* Drop a reference; at zero, every page the object holds is released. */
void vm_object_release(struct vm_object *o);
uint64_t vm_object_pages(const struct vm_object *o);
uint64_t vm_object_committed(const struct vm_object *o);
/* Physical frame behind page `index`, or 0 if not allocated yet. */
uint64_t vm_object_frame(const struct vm_object *o, uint64_t index);

/* A copy-on-write clone: same contents, pages shared until written.
 * Existing writable mappings of `o` become read-only so that writes on
 * either side are caught. The caller holds one reference to the clone. */
struct vm_object *vm_object_clone_cow(struct vm_object *o);

/* Show `pages` pages of `o`, starting at page `first`, at `virt` in `as`
 * (user half, page-aligned, no overlap). The region takes its own
 * reference to the object. Nothing is mapped until touched. */
vm_status_t vm_map(address_space_t *as, uint64_t virt, struct vm_object *o,
                   uint64_t first, uint64_t pages, uint32_t flags);

/* Reserve `pages` pages of fresh zeroed memory at `virt`: a new object
 * mapped there, owned by the region alone. */
vm_status_t vm_reserve(address_space_t *as, uint64_t virt, uint64_t pages, uint32_t flags);

/* Remove the region starting at `virt` (its pages are unmapped; the
 * object loses the region's reference). */
vm_status_t vm_unmap(address_space_t *as, uint64_t virt);

/* Remove every region (before destroying the address space). */
void vm_space_release(address_space_t *as);

/* Called by the page-fault handler. Returns true if the fault was a
 * demand-paging or copy-on-write fault and has been resolved. */
bool vm_handle_fault(uint64_t addr, uint64_t error_code);

void vm_get_stats(struct vm_stats *out);
const char *vm_status_str(vm_status_t s);

#endif /* BHARAT_VM_H */
