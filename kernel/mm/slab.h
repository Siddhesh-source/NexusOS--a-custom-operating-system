#ifndef BHARAT_SLAB_H
#define BHARAT_SLAB_H

/* Slab caches: fast allocation of many objects of one size.
 *
 * A cache holds objects of one kind (threads, processes; later handles
 * and messages). It takes whole pages from the buddy allocator and cuts
 * each into equal slots: one page = one "slab", with a small header at
 * its start holding a bitmap of which slots are in use. Allocating finds a
 * free bit in a partly used slab; freeing clears it. No searching through
 * a heap, no per-object headers, no fragmentation between sizes.
 *
 * Safety, as in the heap: free slots are filled with SLAB_POISON; handing
 * a slot out checks the fill (use after free -> panic); freeing a slot
 * that is not in use is refused (double free). A cache may have a limit
 * on live objects, so one kind of object cannot use up all memory.
 *
 * Each slab can be found from any object in it (round the address down
 * to its page), so slab_free() needs no lookup.
 *
 * Limits: objects up to SLAB_MAX_OBJECT bytes; one page per slab; at most
 * SLAB_MAX_CACHES caches; one empty slab is kept per cache for reuse, the
 * rest go back to the buddy allocator. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SLAB_MAX_OBJECT  1024
#define SLAB_MAX_CACHES  32
#define SLAB_POISON      0xDE

typedef enum {
    SLAB_OK = 0,
    SLAB_ERR_INVALID_PTR,   /* not an object of this cache */
    SLAB_ERR_DOUBLE_FREE,   /* slot is already free */
} slab_status_t;

struct slab_cache;

struct slab_stats {
    const char *name;
    uint32_t object_size;     /* slot size after rounding */
    uint32_t per_slab;        /* slots in one page */
    uint64_t in_use;
    uint64_t slabs;           /* pages held */
    uint64_t allocs, frees, failures;
    uint64_t limit;           /* 0 = no limit */
};

/* Create a cache for objects of `size` bytes aligned to `align` (power of
 * two, at least 8). `limit` caps live objects (0 = none). Returns NULL if
 * the arguments are bad or the cache table is full. */
struct slab_cache *slab_create(const char *name, size_t size, size_t align, uint64_t limit);

/* Returns NULL when the limit is reached or memory is exhausted. The
 * object's contents are undefined (slab_zalloc zeroes them). */
void *slab_alloc(struct slab_cache *c);
void *slab_zalloc(struct slab_cache *c);

/* slab_free(c, NULL) is a no-op. Invalid and double frees are refused,
 * reported, and change nothing. */
slab_status_t slab_free(struct slab_cache *c, void *obj);

void slab_get_stats(const struct slab_cache *c, struct slab_stats *out);
void slab_print_all(void);

/* Verify a cache: list membership, counts, and that every free slot still
 * holds the poison pattern. Reports and returns false on any problem. */
bool slab_check(struct slab_cache *c);

/* Free the cache's empty slabs back to the page allocator. */
void slab_shrink(struct slab_cache *c);

#endif /* BHARAT_SLAB_H */
