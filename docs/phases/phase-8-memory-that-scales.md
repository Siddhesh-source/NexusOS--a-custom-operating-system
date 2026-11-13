# Phase 8: Memory That Scales

**Goal:** replace the simple first-version allocators with the standard
designs a serious kernel needs, and add the virtual-memory features later
phases depend on: pages allocated on first use, copy-on-write, and memory
as an object that can be shared.

**Status:** done. 62 boot checks (`kernel/tests/vm_test.c`), the 114
original memory checks unchanged and passing, a host program with one
million random allocator operations, all 12 fault demos.

| Step | Where |
|---|---|
| 1. Buddy allocator (same `pmm_` API) | `kernel/mm/pmm.c` |
| 2. Slab caches (threads, processes, memory objects, regions) | `kernel/mm/slab.c` |
| 3. Demand paging | `kernel/mm/vm.c` (`vm_reserve`, `vm_handle_fault`) |
| 4. Copy-on-write | `kernel/mm/vm.c` (`vm_object_clone_cow`) |
| 5. Memory regions as objects | `kernel/mm/vm.c` (`vm_object`, `vm_map`) |

---

## 1. Buddy allocator

Free memory is kept as blocks of 2^order contiguous pages (order 0 = one
page, order 10 = 4 MiB), one free list per order:
- **Allocating** takes the smallest free block that fits and splits it in
  halves until it is the right size; the unused halves go on their lists.
- **Freeing** checks the block's *buddy* (the other half of the block it
  was split from, found by flipping one bit of the frame number). If the
  buddy is free and the same size, the two merge, and this repeats upward.

```text
[pmm] Buddy allocator: 130954 frames tracked, 1534 KiB of frame records at phys 0x0000000000100000
[pmm] Free blocks by order  : 3 3 3 4 2 1 0 2 1 1 126
```

Every frame has a 12-byte record (state, block order, reference count,
list links) instead of one bit. That costs 0.3% of RAM, and it buys three
things the bitmap could not do:
- **contiguous blocks:** `pmm_alloc_pages(order)` gives 2^order pages
  aligned to their size (for DMA buffers, large slabs, huge pages later);
- **reference counts:** `pmm_page_ref()` / `pmm_page_unref()` let several
  owners share one frame, which copy-on-write needs. The frame is freed
  when the last owner lets go;
- **checking:** `pmm_check()` verifies that the counts match, blocks are
  aligned and not overlapping, and every possible merge was done.

Error handling is unchanged (unaligned, unmanaged, double free), plus
`PMM_ERR_BAD_ORDER` for freeing part of a block or with the wrong size.

**Hot-page cache.** Most allocations are single pages that are soon freed.
Without help, each would split a 4 MiB block down to one page and merge
it all the way back. A 64-page cache of recently freed pages sits in
front of the lists (Linux does the same). It drains into the buddy lists
when full, or when a larger block is needed.

**Is it faster?** Honestly, not for single pages. Measured natively on
the host with the same workload (allocate 256 pages, free 256), the old
next-fit bitmap took 16 cycles per page and the buddy allocator 37. On
nearly full, fragmented memory it was 15 against 17. Inside QEMU, both are
about 1,000 cycles, mostly emulation overhead, and too noisy to separate.
The buddy allocator is kept for the capabilities above, not for speed
(decision D22); per-CPU page caches in Phase 9 target this path.

## 2. Slab caches

A cache serves objects of one size. It takes whole pages from the buddy
allocator and cuts each into equal slots. A small header at the start of
the page holds a bitmap of used slots. Allocating finds a free bit in a
partly used slab; freeing clears it. The slab is found from any object by
rounding its address down to the page, so freeing needs no search.

```text
[slab] cache            size /page  in use pages    allocs
[slab] thread            176    22       1     1       296
[slab] process            80    50       0     1        77
[slab] vm_object          48    84       0     1         8
[slab] vm_region          64    63       0     1         5
```

- **Safety, as in the heap:** free slots hold the poison byte `0xDE`.
  Handing out a slot checks it (a write after free panics); freeing a slot
  that is not in use, or a pointer that is not an object of this cache, is
  refused and reported. `slab_check()` verifies a whole cache without
  panicking.
- **Limits:** a cache can cap its live objects (the thread cache allows
  `THREAD_MAX`). Phase 10 will use this for per-process budgets of handles
  and messages.
- **Reuse:** one empty slab is kept for the next allocation; the rest go
  back to the buddy allocator. `slab_shrink()` returns all empty slabs.

Threads and processes now come from their own caches. The Phase 4 leak
tests count slab pages separately (`proc_cache_pages()`,
`proc_trim_caches()`), so a "leak" still means a real leak. Handle and
message caches will be created with those objects in Phases 10–11.

**Speed:** a 176-byte object (a thread record) costs 2,284 cycles from its
slab against 2,632 from the heap, about 14% faster. The first version was
*slower* (4,625) because its poison check compared one byte at a time; it
now compares 8 bytes at a time, like the heap.

## 3–5. Memory objects, demand paging, copy-on-write

A **memory object** (`vm_object`) is a number of pages owned by nobody in
particular: a reference count keeps it alive. Its pages are allocated only
when first touched. A **region** (`vm_region`) shows part of an object at
an address in a user address space, with permissions. Each address space
keeps its regions in a sorted list.

The page-fault handler asks `vm_handle_fault()` first:

| Fault | What happens |
|---|---|
| page not present, inside a region, access allowed | allocate a zeroed frame if the object has none at that index, map it: **demand paging** |
| write to a present read-only page of a writable region, frame shared (refcount > 1) | copy the frame, give the copy to this object, unmap the old one everywhere the object is shown: **copy-on-write** |
| same, but this object is the last user | just make the page writable again: no copy |
| no region, or a forbidden access (write to read-only, run non-executable) | not ours: the normal fault report (and, from Phase 10, the process is killed) |

- `vm_reserve()` = a new object mapped at an address: memory that costs
  nothing until it is touched.
- Mapping the **same object** in two address spaces **shares** it (the
  base for shared memory in Phase 11).
- `vm_object_clone_cow()` makes a new object with the same pages (each
  frame's reference count goes up) and write-protects existing mappings.
  The first write on either side copies just that page. This is what
  checkpoints (Phase 17) and the copy-on-write file system (Phase 14) need.
- When a process ends, `vm_space_release()` removes its regions and frees
  whatever memory only they held.

Faults that are resolved this way print nothing; they go into the event
trace as `page.fault` with `b = 1` (demand) or `b = 2` (copy-on-write).

## How it is tested

**Boot (`vm_test.c`, 62 checks), with interrupts off while page tables
are switched:**
- **Buddy:** blocks of 1–32 pages aligned to their size and fully marked;
  one page splits a block and freeing merges back to the exact same
  layout; a freed page is reused at once (hot cache); wrong order, middle
  of a block and double free refused; a 4 MiB block; with every 4 MiB
  block taken, single pages still come from smaller blocks; reference
  counts share, keep, free, and refuse free pages.
- **Slab:** bad sizes refused; 200 objects over several slabs, aligned and
  non-overlapping; freed slots reused first; double free, heap pointer and
  interior pointer refused; a write after free caught by `slab_check`; a
  limit of 5 refuses the 6th until one is freed; empty pages returned.
- **Demand paging:** reserving 16 pages uses no memory; touching 2 pages
  costs exactly 2 faults and 2 frames, zero-filled; untouched pages stay
  unmapped; an access just past the region still faults; a read-only
  region refuses writes; overlapping, unaligned and kernel-half regions
  refused; unmapping frees everything.
- **Copy-on-write:** a clone shares frames (refcount 2); the clone, mapped
  in another space, sees the data without copying; its first write copies
  one page; an untouched page is demand-filled, not copied; the original's
  write to a page it no longer shares is a reuse, not a copy; a shared view
  of the original in the second space sees the original's writes while the
  clone keeps its own; releasing everything frees every object, region and
  frame.

**Host (`tests/unit/mm_unit.c`, about 1.26 million checks):** the real
`pmm.c` and `slab.c` run over a 64 MiB array with a memory map that has
holes and unaligned edges.
- One million random operations (1 page to 4 MiB, frees, shared
  references) against a shadow map of who owns every frame. Every block
  must be free in the shadow, inside usable memory and aligned.
  `pmm_check()` runs every 50,000 operations, and freeing everything must
  restore the exact starting layout.
- Every error path, and exhausting all pages one by one.
- 200,000 random slab operations in three sizes with content checks, and
  a use-after-free that must panic.

A planted bug (buddies never merge) made the host test fail at once.

## Known limits

- One CPU (`IRQ_GUARD`); per-CPU page caches and slab magazines in Phase 9.
- Slabs are one page each; objects up to 1 KiB.
- Regions are a sorted list (fine for tens of regions; a tree if processes
  get many).
- No swapping, no file-backed objects yet (Phase 14), no huge pages.
- Kernel code touching user pages directly is only done by tests; from
  Phase 10 it must go through `copy_from_user`/`copy_to_user` with SMAP on.
