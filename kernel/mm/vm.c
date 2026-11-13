#include "vm.h"
#include "pmm.h"
#include "heap.h"
#include "slab.h"
#include "pagefault.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "trace.h"
#include "spinlock.h"

#define OBJECT_MAGIC 0x564D4F42u        /* "VMOB" */
#define USER_TOP     0x0000800000000000ULL

struct vm_object {
    uint32_t magic;
    uint32_t refs;              /* holders + regions mapping it */
    uint64_t npages;
    uint64_t committed;         /* pages allocated so far */
    uint64_t *frames;           /* physical frame per page, 0 = not yet */
    struct vm_region *mappings; /* regions showing this object */
};

struct vm_region {
    uint64_t start, end;        /* [start, end), page-aligned */
    uint32_t flags;             /* VM_WRITE, VM_EXEC */
    struct vm_object *obj;
    uint64_t first;             /* object page shown at `start` */
    address_space_t *as;
    struct vm_region *next;     /* in the address space, by address */
    struct vm_region *next_map; /* in the object's mapping list */
};

/* One lock for all objects and regions. Public functions take it; the
 * *_locked helpers expect it held, so they can call each other. */
static struct spinlock vm_lock = SPINLOCK_INIT(LOCK_CLASS_VM);
static struct slab_cache *object_cache, *region_cache;
static struct vm_stats stats;

static bool init_caches(void)
{
    if (object_cache == NULL)
        object_cache = slab_create("vm_object", sizeof(struct vm_object), 16, 0);
    if (region_cache == NULL)
        region_cache = slab_create("vm_region", sizeof(struct vm_region), 16, 0);
    return object_cache != NULL && region_cache != NULL;
}

static bool object_valid(const struct vm_object *o)
{
    return o != NULL && o->magic == OBJECT_MAGIC && o->refs > 0;
}

/* ---- objects ------------------------------------------------------------ */

static struct vm_object *object_create_locked(uint64_t pages)
{
    if (pages == 0 || pages > VM_MAX_OBJECT_PAGES)
        return NULL;
    if (!init_caches())
        return NULL;
    struct vm_object *o = slab_zalloc(object_cache);
    if (o == NULL)
        return NULL;
    o->frames = kzalloc(pages * sizeof(uint64_t));
    if (o->frames == NULL) {
        slab_free(object_cache, o);
        return NULL;
    }
    o->magic = OBJECT_MAGIC;
    o->refs = 1;
    o->npages = pages;
    stats.objects++;
    return o;
}

struct vm_object *vm_object_create(uint64_t pages)
{
    SPIN_GUARD(&vm_lock);
    return object_create_locked(pages);
}

void vm_object_ref(struct vm_object *o)
{
    SPIN_GUARD(&vm_lock);
    if (!object_valid(o))
        panic("vm: reference to an invalid memory object");
    o->refs++;
}

static void object_release_locked(struct vm_object *o)
{
    if (!object_valid(o))
        panic("vm: release of an invalid memory object");
    if (--o->refs > 0)
        return;
    if (o->mappings != NULL)
        panic("vm: memory object freed while still mapped");
    for (uint64_t i = 0; i < o->npages; i++) {
        if (o->frames[i] != 0) {
            pmm_page_unref(o->frames[i]);      /* frees it unless a clone shares it */
            stats.pages_committed--;
        }
    }
    kfree(o->frames);
    o->magic = 0;
    slab_free(object_cache, o);
    stats.objects--;
}

void vm_object_release(struct vm_object *o)
{
    SPIN_GUARD(&vm_lock);
    object_release_locked(o);
}

uint64_t vm_object_pages(const struct vm_object *o)
{
    return object_valid(o) ? o->npages : 0;
}

uint64_t vm_object_committed(const struct vm_object *o)
{
    return object_valid(o) ? o->committed : 0;
}

uint64_t vm_object_frame(const struct vm_object *o, uint64_t index)
{
    return object_valid(o) && index < o->npages ? o->frames[index] : 0;
}

/* Remove page `index` of `o` from every address space showing it, so the
 * next access faults and maps whatever the object holds then. */
static void unmap_everywhere(struct vm_object *o, uint64_t index)
{
    for (struct vm_region *r = o->mappings; r != NULL; r = r->next_map) {
        if (index < r->first || index >= r->first + (r->end - r->start) / PAGE_SIZE)
            continue;
        uint64_t va = r->start + (index - r->first) * PAGE_SIZE;
        vmm_unmap_page(r->as, va, NULL);           /* not mapped yet is fine */
    }
}

/* Make every present mapping of `o` read-only (write faults then decide
 * between copying and reusing). */
static void write_protect_everywhere(struct vm_object *o)
{
    for (struct vm_region *r = o->mappings; r != NULL; r = r->next_map) {
        for (uint64_t va = r->start; va < r->end; va += PAGE_SIZE) {
            uint32_t f;
            if (vmm_get_flags(r->as, va, &f) == VMM_OK && (f & VMM_WRITE))
                vmm_protect_page(r->as, va, f & ~VMM_WRITE);
        }
    }
}

struct vm_object *vm_object_clone_cow(struct vm_object *o)
{
    SPIN_GUARD(&vm_lock);
    if (!object_valid(o))
        return NULL;
    struct vm_object *c = object_create_locked(o->npages);
    if (c == NULL)
        return NULL;
    for (uint64_t i = 0; i < o->npages; i++) {
        if (o->frames[i] != 0) {
            pmm_page_ref(o->frames[i]);           /* now shared by both */
            c->frames[i] = o->frames[i];
            c->committed++;
            stats.pages_committed++;
        }
    }
    write_protect_everywhere(o);
    return c;
}

/* ---- regions ------------------------------------------------------------ */

static bool range_ok(uint64_t virt, uint64_t pages)
{
    return pages > 0 && IS_PAGE_ALIGNED(virt) && virt >= PAGE_SIZE
           && pages <= VM_MAX_OBJECT_PAGES && virt < USER_TOP
           && pages <= (USER_TOP - virt) / PAGE_SIZE;
}

static vm_status_t map_locked(address_space_t *as, uint64_t virt, struct vm_object *o,
                              uint64_t first, uint64_t pages, uint32_t flags)
{
    if (as == NULL || as == vmm_kernel_space() || !range_ok(virt, pages)
        || (flags & ~(VM_WRITE | VM_EXEC)) != 0)
        return VM_ERR_INVALID;
    if (!object_valid(o) || first > o->npages || pages > o->npages - first)
        return VM_ERR_INVALID;
    uint64_t end = virt + pages * PAGE_SIZE;

    struct vm_region **link = &as->regions;
    while (*link != NULL && (*link)->end <= virt)
        link = &(*link)->next;
    if (*link != NULL && (*link)->start < end)
        return VM_ERR_OVERLAP;

    struct vm_region *r = slab_zalloc(region_cache);
    if (r == NULL)
        return VM_ERR_NO_MEMORY;
    r->start = virt;
    r->end = end;
    r->flags = flags;
    r->obj = o;
    r->first = first;
    r->as = as;
    r->next = *link;
    *link = r;
    r->next_map = o->mappings;
    o->mappings = r;
    o->refs++;
    stats.regions++;
    return VM_OK;
}

vm_status_t vm_map(address_space_t *as, uint64_t virt, struct vm_object *o,
                   uint64_t first, uint64_t pages, uint32_t flags)
{
    SPIN_GUARD(&vm_lock);
    return map_locked(as, virt, o, first, pages, flags);
}

vm_status_t vm_reserve(address_space_t *as, uint64_t virt, uint64_t pages, uint32_t flags)
{
    SPIN_GUARD(&vm_lock);
    struct vm_object *o = object_create_locked(pages);
    if (o == NULL)
        return VM_ERR_NO_MEMORY;
    vm_status_t st = map_locked(as, virt, o, 0, pages, flags);
    object_release_locked(o);        /* the region's reference is the only one */
    return st;
}

static void remove_region(struct vm_region **link)
{
    struct vm_region *r = *link;
    for (uint64_t va = r->start; va < r->end; va += PAGE_SIZE)
        vmm_unmap_page(r->as, va, NULL);
    *link = r->next;
    struct vm_region **m = &r->obj->mappings;
    while (*m != r)
        m = &(*m)->next_map;
    *m = r->next_map;
    struct vm_object *o = r->obj;
    slab_free(region_cache, r);
    stats.regions--;
    object_release_locked(o);
}

vm_status_t vm_unmap(address_space_t *as, uint64_t virt)
{
    if (as == NULL)
        return VM_ERR_INVALID;
    SPIN_GUARD(&vm_lock);
    for (struct vm_region **link = &as->regions; *link != NULL; link = &(*link)->next) {
        if ((*link)->start == virt) {
            remove_region(link);
            return VM_OK;
        }
    }
    return VM_ERR_NOT_FOUND;
}

void vm_space_release(address_space_t *as)
{
    SPIN_GUARD(&vm_lock);
    while (as != NULL && as->regions != NULL)
        remove_region(&as->regions);
}

/* ---- faults ------------------------------------------------------------- */

bool vm_handle_fault(uint64_t addr, uint64_t error_code)
{
    if (addr >= USER_TOP || (error_code & PF_ERR_RSVD))
        return false;
    SPIN_GUARD(&vm_lock);
    address_space_t *as = vmm_current_space();
    if (as == NULL)
        return false;
    struct vm_region *r = as->regions;
    while (r != NULL && r->end <= addr)
        r = r->next;
    if (r == NULL || addr < r->start)
        return false;                         /* not in any region */

    bool write = error_code & PF_ERR_WRITE;
    if ((write && !(r->flags & VM_WRITE)) || ((error_code & PF_ERR_IFETCH) && !(r->flags & VM_EXEC)))
        return false;                         /* forbidden: a real fault */

    uint64_t va = PAGE_ALIGN_DOWN(addr);
    struct vm_object *o = r->obj;
    uint64_t index = r->first + (va - r->start) / PAGE_SIZE;
    uint32_t map_flags = VMM_USER | (r->flags & (VM_WRITE | VM_EXEC));

    if (!(error_code & PF_ERR_PRESENT)) {
        /* Demand paging: first touch of this page in this space. */
        if (o->frames[index] == 0) {
            uint64_t f = pmm_alloc_zeroed_page();
            if (f == 0)
                return false;                 /* out of memory: reported as a fault */
            o->frames[index] = f;
            o->committed++;
            stats.pages_committed++;
            stats.demand_faults++;
        }
        /* A frame still shared with a clone is mapped read-only, so the
         * first write comes back here as a copy-on-write fault. */
        if (pmm_page_refcount(o->frames[index]) > 1)
            map_flags &= ~VMM_WRITE;
        if (vmm_map_page(as, va, o->frames[index], map_flags) != VMM_OK)
            return false;
        trace_event(TRACE_PAGE_FAULT, addr, 1);
        return true;
    }

    if (!write)
        return false;                         /* present, read: not ours */

    /* Copy-on-write: the page is present but read-only in a writable
     * region, because its frame is (or was) shared with a clone. */
    uint64_t old = o->frames[index];
    if (pmm_page_refcount(old) > 1) {
        uint64_t f = pmm_alloc_page();
        if (f == 0)
            return false;
        memcpy(phys_to_virt(f), phys_to_virt(old), PAGE_SIZE);
        o->frames[index] = f;
        pmm_page_unref(old);                  /* the clone keeps the original */
        unmap_everywhere(o, index);           /* every view picks up the copy */
        stats.cow_copies++;
        if (vmm_map_page(as, va, f, map_flags) != VMM_OK)
            return false;
    } else {
        /* Last sharer: no copy needed, just allow writing again. */
        if (vmm_protect_page(as, va, map_flags) != VMM_OK)
            return false;
        stats.cow_reuses++;
    }
    trace_event(TRACE_PAGE_FAULT, addr, 2);
    return true;
}

void vm_get_stats(struct vm_stats *out)
{
    SPIN_GUARD(&vm_lock);
    *out = stats;
}

const char *vm_status_str(vm_status_t s)
{
    switch (s) {
    case VM_OK:            return "ok";
    case VM_ERR_INVALID:   return "invalid address, size, flags or object";
    case VM_ERR_OVERLAP:   return "overlaps an existing region";
    case VM_ERR_NOT_FOUND: return "no region at that address";
    case VM_ERR_NO_MEMORY: return "out of memory";
    }
    return "unknown";
}
