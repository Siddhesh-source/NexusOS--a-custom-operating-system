# NEXUS OS — Phase 3: Memory Management

Phase 3 adds the memory-management foundation that later process and
userspace support will build on: a physical page-frame allocator, the
kernel's own 4-level page tables, a virtual-mapping API, a kernel heap,
page-fault reporting, and per-process address spaces (without processes).

Out of scope for this phase: processes, threads, scheduling, userspace,
demand paging, swapping, copy-on-write.

## Boot sequence

```
kernel_main
 ├─ gdt_init / idt_init / timer_init / sti          (Phase 2)
 ├─ collect_boot_mem_info   Limine memmap/HHDM/kernel address -> struct boot_mem_info
 └─ mm_init
     ├─ pmm_init            bitmap allocator over MEM_USABLE regions
     ├─ vmm_init            build kernel page tables, switch CR3
     └─ heap_init           map the first 64 KiB of the kernel heap
 └─ mm_run_tests            114 boot-time checks
```

`kernel.c` is the only place that knows about Limine. It converts the
Limine memory map into `struct mem_region` entries (see `mm.h`), so the
PMM/VMM are independent of the boot protocol.

## Virtual address layout

| Range | Contents | Permissions |
|---|---|---|
| `0x0000000000000000 – 0x00007fffffffffff` | user half, private per address space (empty for now) | — |
| `0xffff800000000000 + phys` | HHDM: direct map of RAM-backed physical memory | RW, NX, global |
| `0xffffc00000000000` (+256 MiB) | kernel heap | RW, NX, global |
| `0xffffd00000000000` | scratch window used by the memory tests | varies |
| `0xffffe00000000000` | kernel thread stacks, 256 × 32 KiB slots, guard below each (Phase 4) | RW, NX, global |
| `0xffffffff80200000` | kernel `.text` | R-X |
| next page | kernel `.rodata` | R-- |
| next page | kernel `.data` + `.bss` | RW- |

Everything else is unmapped. In particular:

- **Page 0** is never mapped, so null dereferences fault.
- **Reserved, bad, and non-existent physical memory** is not in the HHDM.
  Only usable, bootloader-reclaimable, ACPI, and framebuffer regions are.
- **The kernel image is not in the HHDM**, so there is no writable alias of
  `.text`.
- **The lowest page of the boot stack** is an unmapped guard page. An
  overflow raises #PF, which the CPU can't push onto the dead stack, so it
  escalates to #DF. #DF runs on its own IST1 stack and reports the failure.

The linker script page-aligns `.rodata` and `.data` and exports the
`__text_start`, `__rodata_start`, `__data_start`, and `__kernel_end` symbols
the VMM uses.

## Physical memory manager (`pmm.c`)

- **Structure:** one bit per 4 KiB frame, from physical 0 up to the end of the
  highest usable region (16 KiB of bitmap per 512 MiB of RAM). The bitmap
  lives at the start of the first usable region large enough to hold it.
- **Initialisation:** every frame starts reserved. The allocator then frees
  frames that lie wholly inside `MEM_USABLE` regions, and re-reserves page 0
  and the bitmap's own frames. Everything else stays reserved automatically:
  the kernel image, bootloader structures (including Limine responses still
  in use), ACPI tables, and MMIO.
- **Allocation:** next-fit scan that skips full 64-bit words. Page 0 is never
  handed out, so `pmm_alloc_page()` can return `0` to mean out of memory.
- **Release:** `pmm_free_page()` rejects unaligned addresses, frames the PMM
  doesn't own (page 0, the kernel, MMIO, addresses past RAM, the bitmap), and
  double frees. Each rejection returns a status and leaves state unchanged.
- **Statistics:** total RAM, usable, reserved (kernel/bootloader/ACPI), free,
  and allocated. Firmware-reserved and MMIO ranges are reported separately
  and are not counted as RAM. QEMU's memory map, for example, contains a
  12 GiB reserved hole at `0xfd00000000`.

## Paging and the VMM (`vmm.c`)

- 4-level paging (PML4 → PDPT → PD → PT) with 4 KiB pages only.
- `vmm_map_page`, `vmm_unmap_page`, `vmm_translate`, `vmm_get_flags`,
  `vmm_protect_page`, and `vmm_map_range` (which rolls back on failure).
- Permission flags are `VMM_WRITE`, `VMM_EXEC` (pages are NX without it),
  `VMM_USER`, `VMM_GLOBAL`, and `VMM_NOCACHE`. Intermediate table entries are
  permissive, so the leaf PTE alone decides the effective permissions.
- **Validation:** a call fails with a `vmm_status_t` if the address is
  unaligned or non-canonical, if it would map over an existing mapping, or if
  the page isn't mapped (for unmap, protect, or translate). It also fails if
  the physical address is 2^52 or higher, if the flags are unknown, or if it
  asks for a user page in the kernel half or a global page in the user half.
- **TLB:** every change runs `invlpg` for kernel-half addresses, which are
  shared by all spaces, and for pages in the currently loaded space.
- **CPU setup:** `vmm_init` sets `EFER.NXE`, `CR0.WP` (so ring 0 honours
  read-only pages), and `CR4.PGE`. After loading CR3 it toggles PGE to flush
  any global TLB entries left over from Limine's tables.

### Address spaces: the isolation foundation

```
Process A → address_space A → PML4 A ─┬─ entries 0-255:   private user half
Process B → address_space B → PML4 B ─┤
                                      └─ entries 256-511: shared kernel PDPTs
```

At boot the VMM pre-allocates all 256 kernel-half PDPTs (1 MiB). Kernel PML4
entries therefore never change after boot. `vmm_create_address_space()`
copies them by reference into each new PML4, so any kernel mapping made
later, in any space, is visible in every space.

- `vmm_destroy_address_space()` frees the user-half page tables and the
  PML4. It refuses to destroy the kernel space or the active space.
- `vmm_switch()` loads CR3.
- The VMM owns only page-table pages. Mapped frames belong to the caller,
  which will be the future process code.

A process layer can be added on top without changing the PMM or VMM.

## Kernel heap (`heap.c`)

- **Allocator:** first fit over one contiguous region at `KERNEL_HEAP_BASE`.
  The region is tiled by blocks, each a 32-byte header plus payload, on an
  address-ordered doubly linked list.
- **Allocation:** `kmalloc`, `kzalloc`, and `kmalloc_aligned(size, align)`.
  Every payload is at least 16-byte aligned; `align` can be any power of two
  up to 4096. When the aligned payload doesn't start at the block's own
  payload, the leading gap is split off as a separate free block.
- **Release:** `kfree` coalesces with free neighbours. It walks the block list
  to validate the pointer, so pointers to the stack, into the middle of a
  block, or already freed are rejected with a status instead of corrupting
  the heap.
- **Growth:** when nothing fits, the heap maps fresh PMM frames at its end,
  up to 256 MiB. Growth is all or nothing: if a frame or page-table
  allocation fails partway, every frame taken so far is unmapped and freed.
- **Out of memory:** `kmalloc` returns `NULL`. It never panics.
- **Diagnostics:** `heap_check()` verifies tiling, links, magic numbers, and
  coalescing. `heap_get_stats()` reports usage.

## Page faults (`pagefault.c`)

Every #PF prints the following:

- the faulting address (CR2), with the region it falls in (null page, user
  space, kernel `.text`, heap, HHDM, guard page, …)
- the instruction pointer
- the raw error code and its decoded bits
- the access type: read, write, or instruction fetch
- the mode: kernel or user
- the cause: not-present page, protection violation, or reserved bit

With no demand paging yet, every fault is fatal except one case. A fault
raised by one of the `mm_probe_read/write/exec` routines (`probe.asm`) is
recovered through an exception-table fixup, and the probe returns `1`. The
fixup matches exact instruction addresses, so no other kernel fault can
resume by accident. The same fixup handles #GP, which is what a
non-canonical access raises.

The tests use these probes to exercise invalid accesses without halting.

## Tests

`mm_run_tests()` runs at every boot. `python build.py test` passes only if
all checks pass and no `[FAIL]`, panic, or unexpected exception appears.

| Area | Covered |
|---|---|
| PMM | allocate, release, alignment, distinctness, double free, unaligned/page-0/kernel/out-of-range frees, zeroed pages |
| Exhaustion | allocate every free frame, allocation failure, VMM page-table OOM, address-space OOM, heap OOM, partial heap-growth rollback, full recovery |
| VMM | map, translate (with offset), flags readback, RO/RW/NX/exec transitions, unmap + TLB flush, remap, ranges across page-table boundaries, every invalid-argument case |
| Kernel layout | `.text` R-X, `.rodata` R--, `.data` RW- NX, contiguous physical backing, HHDM correctness |
| Invalid access | writes to `.text`/`.rodata`, executing `.data`, null, user half, guard page, HHDM hole, unbacked heap, non-canonical (#GP) |
| Heap | alignment, 64 mixed allocations, overlap check, coalescing, double/invalid frees, 1 MiB growth, OOM limits, kzalloc |
| Address spaces | same VA → different frames in two spaces, CR3 switches, shared kernel half, destruction frees every table |

Fatal paths are covered by opt-in demos that halt the kernel:

```bash
python build.py run --fault-demo pf      # write to an unmapped kernel address
python build.py run --fault-demo null    # null-pointer dereference
python build.py run --fault-demo stack   # stack overflow -> guard page -> #DF
python build.py run --fault-demo de|ud|df
python build.py test-faults              # build + verify all of the above
```

## Known limitations and next steps

- **4 KiB pages only.** The HHDM of 512 MiB costs about 1 MiB of page tables.
  Large RAM sizes would benefit from 2 MiB pages.
- **Bootloader-reclaimable memory is never reclaimed.** Limine's responses
  live there.
- **Page tables are never freed.** Emptied tables are reclaimed only when an
  address space is destroyed, and kernel-half tables never are.
- **The heap never shrinks,** and `kfree` validation is O(number of blocks).
- **No locking.** All of this is single-core only. Locks come with SMP or the
  scheduler.
