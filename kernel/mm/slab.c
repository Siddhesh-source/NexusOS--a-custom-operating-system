#include "slab.h"
#include "pmm.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"

#define SLAB_MAGIC   0x534C4142u        /* "SLAB" */
#define MAX_SLOTS    256                /* PAGE_SIZE / 16 */

enum { LIST_EMPTY, LIST_PARTIAL, LIST_FULL, LIST_COUNT };

/* Header at the start of every slab page. */
struct slab {
    uint32_t magic;
    uint16_t in_use;
    uint8_t list;                       /* which list of its cache */
    uint8_t reserved;
    struct slab_cache *cache;
    struct slab *next, *prev;
    uint64_t used[MAX_SLOTS / 64];      /* bit set = slot in use */
};

struct slab_cache {
    const char *name;
    uint32_t size;                      /* slot size */
    uint32_t first;                     /* offset of slot 0 in the page */
    uint32_t per_slab;
    uint64_t limit;
    uint64_t in_use, slabs, allocs, frees, failures;
    struct slab *lists[LIST_COUNT];
};

static struct slab_cache caches[SLAB_MAX_CACHES];
static unsigned cache_count;

static inline uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static inline uint8_t *slot_addr(struct slab *s, unsigned i)
{
    return (uint8_t *)s + s->cache->first + (uint64_t)i * s->cache->size;
}

/* ---- lists -------------------------------------------------------------- */

static void list_add(struct slab_cache *c, struct slab *s, uint8_t which)
{
    s->list = which;
    s->prev = NULL;
    s->next = c->lists[which];
    if (s->next != NULL)
        s->next->prev = s;
    c->lists[which] = s;
}

static void list_del(struct slab_cache *c, struct slab *s)
{
    if (s->prev != NULL)
        s->prev->next = s->next;
    else
        c->lists[s->list] = s->next;
    if (s->next != NULL)
        s->next->prev = s->prev;
    s->next = s->prev = NULL;
}

static void move(struct slab_cache *c, struct slab *s, uint8_t which)
{
    if (s->list == which)
        return;
    list_del(c, s);
    list_add(c, s, which);
}

/* ---- caches ------------------------------------------------------------- */

struct slab_cache *slab_create(const char *name, size_t size, size_t align, uint64_t limit)
{
    if (align < 8)
        align = 8;
    if (size == 0 || size > SLAB_MAX_OBJECT || (align & (align - 1)) != 0 || align > 256)
        return NULL;
    IRQ_GUARD();
    if (cache_count == SLAB_MAX_CACHES)
        return NULL;
    struct slab_cache *c = &caches[cache_count++];
    memset(c, 0, sizeof(*c));
    c->name = name;
    c->size = (uint32_t)align_up(size < 16 ? 16 : size, align);
    c->first = (uint32_t)align_up(sizeof(struct slab), align);
    c->per_slab = (uint32_t)((PAGE_SIZE - c->first) / c->size);
    if (c->per_slab > MAX_SLOTS)
        c->per_slab = MAX_SLOTS;
    c->limit = limit;
    return c;
}

static struct slab *grow(struct slab_cache *c)
{
    uint64_t phys = pmm_alloc_page();
    if (phys == 0)
        return NULL;
    struct slab *s = phys_to_virt(phys);
    memset(s, 0, sizeof(*s));
    s->magic = SLAB_MAGIC;
    s->cache = c;
    memset((uint8_t *)s + c->first, SLAB_POISON, (uint64_t)c->per_slab * c->size);
    list_add(c, s, LIST_EMPTY);
    c->slabs++;
    return s;
}

static bool poisoned(const uint8_t *p, uint32_t n, uint32_t *bad)
{
    /* Slots are at least 8-byte aligned and sized: compare words. */
    const uint64_t word = 0x0101010101010101ULL * SLAB_POISON;
    uint32_t i = 0;
    while (i + 8 <= n && *(const uint64_t *)(p + i) == word)
        i += 8;
    for (; i < n; i++) {
        if (p[i] != SLAB_POISON) {
            *bad = i;
            return false;
        }
    }
    return true;
}

void *slab_alloc(struct slab_cache *c)
{
    if (c == NULL)
        return NULL;
    IRQ_GUARD();
    if (c->limit != 0 && c->in_use >= c->limit) {
        c->failures++;
        return NULL;
    }
    struct slab *s = c->lists[LIST_PARTIAL];
    if (s == NULL)
        s = c->lists[LIST_EMPTY];
    if (s == NULL)
        s = grow(c);
    if (s == NULL) {
        c->failures++;
        return NULL;
    }

    unsigned slot = MAX_SLOTS;
    for (unsigned w = 0; w < MAX_SLOTS / 64 && slot == MAX_SLOTS; w++) {
        if (~s->used[w] != 0) {
            unsigned b = (unsigned)__builtin_ctzll(~s->used[w]);
            if (w * 64 + b < c->per_slab)
                slot = w * 64 + b;
        }
    }
    if (slot == MAX_SLOTS)
        panic("slab: a slab on the free lists has no free slot");

    uint8_t *obj = slot_addr(s, slot);
    uint32_t bad;
    if (!poisoned(obj, c->size, &bad)) {
        kprintf("[slab] %s: freed object %p was changed at byte %u (use after free)\n",
                c->name, (void *)obj, bad);
        panic("slab: use after free detected: an object was written after it was freed");
    }
    s->used[slot / 64] |= 1ULL << (slot % 64);
    s->in_use++;
    move(c, s, s->in_use == c->per_slab ? LIST_FULL : LIST_PARTIAL);
    c->in_use++;
    c->allocs++;
    return obj;
}

void *slab_zalloc(struct slab_cache *c)
{
    void *p = slab_alloc(c);
    if (p != NULL)
        memset(p, 0, c->size);
    return p;
}

slab_status_t slab_free(struct slab_cache *c, void *obj)
{
    if (obj == NULL)
        return SLAB_OK;
    IRQ_GUARD();
    struct slab *s = (struct slab *)((uint64_t)obj & ~(PAGE_SIZE - 1));
    uint64_t off = (uint64_t)obj - (uint64_t)s;
    /* Check the page really is a slab of this cache before trusting it:
     * only HHDM addresses of allocated frames can hold slabs. */
    bool ok = c != NULL && (uint64_t)s >= hhdm_offset
              && pmm_is_allocated((uint64_t)s - hhdm_offset)
              && s->magic == SLAB_MAGIC && s->cache == c
              && off >= c->first && (off - c->first) % c->size == 0
              && (off - c->first) / c->size < c->per_slab;
    if (!ok) {
        kprintf("[slab] rejected free of %p: not an object of cache %s\n",
                obj, c != NULL ? c->name : "(null)");
        return SLAB_ERR_INVALID_PTR;
    }
    unsigned slot = (unsigned)((off - c->first) / c->size);
    if (!(s->used[slot / 64] & (1ULL << (slot % 64)))) {
        kprintf("[slab] rejected free of %p in %s: double free\n", obj, c->name);
        return SLAB_ERR_DOUBLE_FREE;
    }
    s->used[slot / 64] &= ~(1ULL << (slot % 64));
    memset(obj, SLAB_POISON, c->size);
    s->in_use--;
    c->in_use--;
    c->frees++;

    if (s->in_use == 0) {
        /* Keep one empty slab for the next allocation; return the rest. */
        if (c->lists[LIST_EMPTY] != NULL) {
            list_del(c, s);
            s->magic = 0;
            c->slabs--;
            pmm_free_page((uint64_t)s - hhdm_offset);
        } else {
            move(c, s, LIST_EMPTY);
        }
    } else {
        move(c, s, LIST_PARTIAL);
    }
    return SLAB_OK;
}

void slab_shrink(struct slab_cache *c)
{
    IRQ_GUARD();
    while (c->lists[LIST_EMPTY] != NULL) {
        struct slab *s = c->lists[LIST_EMPTY];
        list_del(c, s);
        s->magic = 0;
        c->slabs--;
        pmm_free_page((uint64_t)s - hhdm_offset);
    }
}

/* ---- introspection ------------------------------------------------------ */

void slab_get_stats(const struct slab_cache *c, struct slab_stats *out)
{
    IRQ_GUARD();
    out->name = c->name;
    out->object_size = c->size;
    out->per_slab = c->per_slab;
    out->in_use = c->in_use;
    out->slabs = c->slabs;
    out->allocs = c->allocs;
    out->frees = c->frees;
    out->failures = c->failures;
    out->limit = c->limit;
}

void slab_print_all(void)
{
    kprintf("[slab] %-14s %6s %5s %7s %5s %9s\n", "cache", "size", "/page", "in use", "pages",
            "allocs");
    for (unsigned i = 0; i < cache_count; i++) {
        struct slab_stats s;
        slab_get_stats(&caches[i], &s);
        kprintf("[slab] %-14s %6u %5u %7lu %5lu %9lu\n", s.name, s.object_size, s.per_slab,
                s.in_use, s.slabs, s.allocs);
    }
}

bool slab_check(struct slab_cache *c)
{
    IRQ_GUARD();
    uint64_t in_use = 0, slabs = 0;
    for (unsigned l = 0; l < LIST_COUNT; l++) {
        struct slab *prev = NULL;
        for (struct slab *s = c->lists[l]; s != NULL; s = s->next) {
            if (s->magic != SLAB_MAGIC || s->cache != c || s->list != l || s->prev != prev) {
                kprintf("[slab] check %s: bad slab header at %p\n", c->name, (void *)s);
                return false;
            }
            unsigned used = 0;
            for (unsigned i = 0; i < c->per_slab; i++) {
                bool busy = s->used[i / 64] & (1ULL << (i % 64));
                uint32_t bad;
                if (busy) {
                    used++;
                } else if (!poisoned(slot_addr(s, i), c->size, &bad)) {
                    kprintf("[slab] check %s: free object %p written after free (byte %u)\n",
                            c->name, (void *)slot_addr(s, i), bad);
                    return false;
                }
            }
            bool right_list = (l == LIST_EMPTY && used == 0)
                              || (l == LIST_FULL && used == c->per_slab)
                              || (l == LIST_PARTIAL && used > 0 && used < c->per_slab);
            if (used != s->in_use || !right_list) {
                kprintf("[slab] check %s: slab %p count %u/%u on the wrong list\n",
                        c->name, (void *)s, used, s->in_use);
                return false;
            }
            in_use += used;
            slabs++;
            prev = s;
        }
    }
    if (in_use != c->in_use || slabs != c->slabs) {
        kprintf("[slab] check %s: counted %lu objects in %lu slabs, expected %lu in %lu\n",
                c->name, in_use, slabs, c->in_use, c->slabs);
        return false;
    }
    return true;
}
