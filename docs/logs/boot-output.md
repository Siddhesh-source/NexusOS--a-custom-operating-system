# BharatOS — Boot Output (Phase 5 build, with Phase 6 additions)

This is the complete serial console output of one BharatOS boot in QEMU.
It was captured from `python build.py run` (legacy BIOS boot, 512 MiB RAM)
on the Phase 5 build (commit `8239086`), and is reproduced verbatim from
`output.txt`.

> **About the capture.** It was taken from an 80-column terminal, so a few
> long lines are clipped at the right edge (e.g. `(RAM-backe)`, `... ped`),
> and the firmware banner overlaps the first BHARAT line. The kernel's full
> lines appear untruncated in `python build.py test` output.

## Results at a glance

| Area | Result from this boot |
|---|---|
| Boot | Limine 12.6.1, long mode, higher-half kernel at `0xffffffff80200000` |
| Memory detected | 511 MiB RAM (523 768 KiB), 130 608 managed 4 KiB pages |
| Kernel page tables | `.text` R-X, `.rodata` R--, `.data` RW-, 519 page-table pages |
| Phase 3: memory tests | **114 passed, 0 failed** |
| Phase 4: process/thread tests | **83 passed, 0 failed** |
| Phase 5: scheduler tests | **26 passed, 0 failed** |
| A/B context-switch ping-pong | exact trace `A1 B101 … A10 B110` |
| Context-switch stress | 60 003 switches, all registers intact |
| Preemption proof | 24 timer preemptions of two threads that never yield |
| All-register check | 3 × ~4.2 million checks of all 15 GPRs, **0 mismatches** |
| Round-robin order | `A B C A B C …` strict rotation |
| Fairness (154 ticks) | 26 / 26 / 25 time slices |
| Switch cost | 2 339 cycles/switch, ~0.0039 % of CPU at 50 switches/s |
| Live demo | CPU, Memory and Counter workers each preempted 40 times, 0 memory errors |
| Final state | all phases verified; heartbeat dot every second |

## Contents

1. [Firmware and kernel banner](#1-firmware-and-kernel-banner)
2. [Boot and CPU initialisation (Phases 1–2)](#2-boot-and-cpu-initialisation-phases-12)
3. [Memory manager start-up (Phase 3)](#3-memory-manager-start-up-phase-3)
4. [Phase 3 memory tests](#4-phase-3-memory-tests)
5. [Phase 4 process and thread tests](#5-phase-4-process-and-thread-tests)
6. [Phase 5 scheduler tests and live demo](#6-phase-5-scheduler-tests-and-live-demo)
7. [Phase 6 changes to this output](#7-phase-6-changes-to-this-output)
8. [Final status](#8-final-status)

## 1. Firmware and kernel banner

SeaBIOS hands over to Limine, which loads the kernel.

```text
SeaBIOS (version rel-1.17.0-0-gb52ca86e094d-prebuilt.qemu.org)
============================================
            B H A R A T   O S
     Phase 5: Preemptive Scheduler0 PCI2.10 PnP PMM+1EFD1F90+1EF31F90 CA00
============================================                                   
```

## 2. Boot and CPU initialisation (Phases 1–2)

Long mode is confirmed, boot information is read from Limine, and the GDT/TSS, IDT and PIT timer are brought up.

```text
[boot] CPU entered long mode (64-bit).
[boot] Bootloader: Limine 12.6.1
[boot] Limine base revision honoured.
[boot] Kernel loaded: phys=0x000000001fef1000 virt=0xffffffff80200000
[boot] Higher-half direct map offset=0xffff800000000000
[init] Setting up GDT...
[gdt] GDT and TSS initialized.
[init] Setting up IDT...
[idt] IDT initialized.
[init] Setting up timer...
[timer] PIT initialized (~100Hz).
[init] Enabling interrupts...
[init] Interrupts enabled.
```

## 3. Memory manager start-up (Phase 3)

The physical memory map is read, the page-frame bitmap is built, the kernel's own page tables are loaded, and the heap is created.

```text
[mm] Initialising physical memory manager...
[pmm] Physical memory map (14 regions):
[pmm]   0000000000001000 - 0000000000072000       452 KiB  bootloader reclaimable
[pmm]   0000000000072000 - 000000000009f000       180 KiB  usable
[pmm]   000000000009fc00 - 00000000000a0000         1 KiB  reserved
[pmm]   00000000000f0000 - 0000000000100000        64 KiB  reserved
[pmm]   0000000000100000 - 000000001feec000    522160 KiB  usable
[pmm]   000000001feec000 - 000000001fef1000        20 KiB  bootloader reclaimable
[pmm]   000000001fef1000 - 000000001ff1e000       180 KiB  kernel and modules
[pmm]   000000001ff1e000 - 000000001ff6f000       324 KiB  bootloader reclaimable
[pmm]   000000001ff6f000 - 000000001ff8a000       108 KiB  usable
[pmm]   000000001ff8a000 - 000000001ffe0000       344 KiB  bootloader reclaimable
[pmm]   000000001ffe0000 - 0000000020000000       128 KiB  reserved
[pmm]   00000000fd000000 - 00000000fd3e8000      4000 KiB  framebuffer
[pmm]   00000000fffc0000 - 0000000100000000       256 KiB  reserved
[pmm]   000000fd00000000 - 0000010000000000  12582912 KiB  reserved
[pmm] Bitmap: 130954 frames tracked, 16376 bytes at phys 0x0000000000072000
[pmm] Total physical memory :   523768 KiB
[pmm] Usable memory         :   522448 KiB
[pmm] Reserved memory       :     1320 KiB (kernel, bootloader, ACPI)
[pmm] Free memory           :   522432 KiB (130608 pages)
[pmm] Allocated pages       :        0 of 130608 managed
[pmm] Non-RAM ranges        : 12587361 KiB (firmware/MMIO, excluded)
[mm] Initialising virtual memory manager...
[vmm] Building kernel page tables (PML4 at phys 0x0000000000076000)
[vmm]   .text   0xffffffff80200000 - 0xffffffff80210000 -> phys 0x000000001fef1000  R-X
[vmm]   .rodata 0xffffffff80210000 - 0xffffffff80216000 -> phys 0x000000001ff01000  R--
[vmm]   .data   0xffffffff80216000 - 0xffffffff8022d000 -> phys 0x000000001ff07000  RW-
[vmm]   HHDM    0xffff800000000000 - 0xffff8000fd3e8000  RW-  (527588 KiB of RAM-backe)
[vmm] Switched to kernel page tables (CR3=0x0000000000076000), 519 page-table pages usd
[vmm] Boot stack guard page at 0xffffffff80217000
[mm] Initialising kernel heap...
[heap] Kernel heap at 0xffffc00000000000, 64 KiB mapped, limit 256 MiB
[mm] Memory management online.
[pmm] Total physical memory :   523768 KiB
[pmm] Usable memory         :   522448 KiB
[pmm] Reserved memory       :     1320 KiB (kernel, bootloader, ACPI)
[pmm] Free memory           :   520284 KiB (130071 pages)
[pmm] Allocated pages       :      537 of 130608 managed
[pmm] Non-RAM ranges        : 12587361 KiB (firmware/MMIO, excluded)
```

## 4. Phase 3 memory tests

The `[#PF]` blocks are deliberate: each invalid access is triggered on purpose, reported in full, and then recovered so the next test can run.

```text
[test] ===== Phase 3 memory-management tests =====

[test] --- PMM: allocate / release ---
[test]   PASS  allocate two physical pages
[test]   PASS  frames are 4 KiB aligned
[test]   PASS  frames are distinct
[test]   PASS  frames marked allocated
[test]   PASS  free count dropped by 2
[test]   PASS  frame usable through the HHDM
[test]   PASS  release a page
[test]   PASS  released frame marked free
[pmm] rejected free of 0x00000000002f0000: double free
[test]   PASS  double free detected
[pmm] rejected free of 0x00000000002f1008: address not page-aligned
[test]   PASS  unaligned free rejected
[pmm] rejected free of 0x0000000000000000: frame not managed by the PMM
[test]   PASS  free of physical page 0 rejected
[pmm] rejected free of 0x000000001fef1000: frame not managed by the PMM
[test]   PASS  free of kernel-image frame rejected
[pmm] rejected free of 0x0000200000000000: frame not managed by the PMM
[test]   PASS  free beyond physical memory rejected
[test]   PASS  release second page
[test]   PASS  free count restored after rejected frees
[test]   PASS  zeroed allocation is all zero

[test] --- PMM: exhaustion and out-of-memory handling ---
[test]   allocated 130071 pages (520284 KiB) before exhaustion
[test]   PASS  every free page could be allocated
[test]   PASS  free count is zero when exhausted
[test]   PASS  allocation fails cleanly when exhausted
[test]   PASS  zeroed allocation fails cleanly when exhausted
[test]   PASS  vmm_map_page reports out-of-memory for page tables
[test]   PASS  address-space creation reports out-of-memory
[test]   PASS  kmalloc returns NULL when heap cannot grow
[test]   PASS  partial heap growth fails...
[test]   PASS  ...and returns every frame it took
[test]   PASS  all pages released, free count restored

[test] --- VMM: map / translate / protect / unmap ---
[test]   PASS  map a virtual page
[test]   PASS  translate returns the mapped frame
[test]   PASS  translate preserves the page offset
[test]   PASS  write through mapping is visible through the HHDM alias
[test]   PASS  mapping flags read back as RW, NX, supervisor

[test] --- VMM: invalid mapping requests ---
[test]   PASS  remap of mapped page rejected
[test]   PASS  unaligned virtual address rejected
[test]   PASS  unaligned physical address rejected
[test]   PASS  non-canonical address rejected
[test]   PASS  physical address beyond 52 bits rejected
[test]   PASS  user mapping in kernel half rejected
[test]   PASS  unknown flag rejected
[test]   PASS  translate of non-canonical address rejected
[test]   PASS  translate of unmapped page fails

[test] --- VMM: permission changes ---
[test]   PASS  change page to read-only

[#PF] Page fault
[#PF]   faulting address : 0xffffd00000000000 (memory-test window)
[#PF]   instruction ptr  : 0xffffffff8020fc9b
[#PF]   error code       : 0x3 [P=1 W=1 U=0 RSVD=0 I/D=0]
[#PF]   access           : write
[#PF]   mode             : kernel
[#PF]   cause            : protection violation (page present)
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  write to read-only page faults (present, write)
[test]   PASS  read-only page still readable
[test]   PASS  restoring write permission allows writes

[#PF] Page fault
[#PF]   faulting address : 0xffffd00000000000 (memory-test window)
[#PF]   instruction ptr  : 0xffffd00000000000
[#PF]   error code       : 0x11 [P=1 W=0 U=0 RSVD=0 I/D=1]
[#PF]   access           : instruction fetch
[#PF]   mode             : kernel
[#PF]   cause            : protection violation (page present)
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  executing an NX page faults (instruction fetch)
[test]   PASS  granting execute permission allows execution
[test]   PASS  revoke execute permission

[test] --- VMM: unmap ---
[test]   PASS  unmap returns the old frame
[test]   PASS  unmapped page no longer translates

[#PF] Page fault
[#PF]   faulting address : 0xffffd00000000000 (memory-test window)
[#PF]   instruction ptr  : 0xffffffff8020fc92
[#PF]   error code       : 0x0 [P=0 W=0 U=0 RSVD=0 I/D=0]
[#PF]   access           : read
[#PF]   mode             : kernel
[#PF]   cause            : page not present
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  access after unmap faults (TLB entry was flushed)
[test]   PASS  double unmap rejected
[test]   PASS  protect of unmapped page rejected
[test]   PASS  remapping to a new frame shows the new contents

[test] --- VMM: range mapping across a page-table boundary ---
[test]   PASS  three pages spanning two page tables map and translate

[test] --- VMM: kernel image permissions ---
[test]   PASS  .text is read + execute
[test]   PASS  .rodata is read-only, NX
[test]   PASS  .data is read-write, NX
[test]   PASS  kernel image is mapped to its contiguous physical load address
[test]   PASS  HHDM maps physical memory at hhdm_offset + phys

[test] --- Page faults: invalid accesses are reported ---

[#PF] Page fault
[#PF]   faulting address : 0xffffffff80200000 (kernel .text)
[#PF]   instruction ptr  : 0xffffffff8020fc9b
[#PF]   error code       : 0x3 [P=1 W=1 U=0 RSVD=0 I/D=0]
[#PF]   access           : write
[#PF]   mode             : kernel
[#PF]   cause            : protection violation (page present)
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  write to kernel .text faults

[#PF] Page fault
[#PF]   faulting address : 0xffffffff80215d30 (kernel .rodata)
[#PF]   instruction ptr  : 0xffffffff8020fc9b
[#PF]   error code       : 0x3 [P=1 W=1 U=0 RSVD=0 I/D=0]
[#PF]   access           : write
[#PF]   mode             : kernel
[#PF]   cause            : protection violation (page present)
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  write to kernel .rodata faults

[#PF] Page fault
[#PF]   faulting address : 0xffffffff80216000 (kernel .data/.bss)
[#PF]   instruction ptr  : 0xffffffff80216000
[#PF]   error code       : 0x11 [P=1 W=0 U=0 RSVD=0 I/D=1]
[#PF]   access           : instruction fetch
[#PF]   mode             : kernel
[#PF]   cause            : protection violation (page present)
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  executing kernel .data faults (NX)

[#PF] Page fault
[#PF]   faulting address : 0x0000000000000000 (null page)
[#PF]   instruction ptr  : 0xffffffff8020fc92
[#PF]   error code       : 0x0 [P=0 W=0 U=0 RSVD=0 I/D=0]
[#PF]   access           : read
[#PF]   mode             : kernel
[#PF]   cause            : page not present
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  null-pointer read faults (page 0 unmapped)

[#PF] Page fault
[#PF]   faulting address : 0x0000000000400000 (user space)
[#PF]   instruction ptr  : 0xffffffff8020fc9b
[#PF]   error code       : 0x2 [P=0 W=1 U=0 RSVD=0 I/D=0]
[#PF]   access           : write
[#PF]   mode             : kernel
[#PF]   cause            : page not present
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  write to unmapped user-half address faults

[#PF] Page fault
[#PF]   faulting address : 0xffffffff80217000 (boot stack guard page)
[#PF]   instruction ptr  : 0xffffffff8020fc92
[#PF]   error code       : 0x0 [P=0 W=0 U=0 RSVD=0 I/D=0]
[#PF]   access           : read
[#PF]   mode             : kernel
[#PF]   cause            : page not present
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  boot-stack guard page faults

[#PF] Page fault
[#PF]   faulting address : 0xffffa00000000000 (unmapped kernel space)
[#PF]   instruction ptr  : 0xffffffff8020fc92
[#PF]   error code       : 0x0 [P=0 W=0 U=0 RSVD=0 I/D=0]
[#PF]   access           : read
[#PF]   mode             : kernel
[#PF]   cause            : page not present
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  HHDM beyond physical memory is unmapped

[#PF] Page fault
[#PF]   faulting address : 0xffffc0000ffff000 (kernel heap)
[#PF]   instruction ptr  : 0xffffffff8020fc92
[#PF]   error code       : 0x0 [P=0 W=0 U=0 RSVD=0 I/D=0]
[#PF]   access           : read
[#PF]   mode             : kernel
[#PF]   cause            : page not present
[#PF]   outcome          : recovered (memory probe fixup)
[test]   PASS  unbacked kernel heap space is unmapped
[#GP] General protection fault at rip=0xffffffff8020fca4 (error 0x0) - recovered by prp
[test]   PASS  non-canonical access faults (reported as #GP)
[test]   PASS  non-canonical access raised #GP, not #PF
[test]   PASS  every invalid access produced exactly one page-fault report

[test] --- Heap: allocation and release ---
[test]   PASS  kmalloc(0) returns NULL
[test]   PASS  small allocation is in the heap and 16-byte aligned
[test]   PASS  allocation is writable
[test]   PASS  free small allocation
[test]   PASS  64 allocations of varied sizes succeed
[test]   PASS  allocations do not overlap (patterns intact)
[test]   PASS  freeing odd blocks leaves even blocks intact
[test]   PASS  heap consistent with interleaved free blocks
[test]   PASS  all blocks coalesce back into one free block

[test] --- Heap: alignment ---
[test]   PASS  64-byte aligned allocation
[test]   PASS  page-aligned small allocation
[test]   PASS  page-aligned multi-page allocation
[test]   PASS  heap consistent after aligned splits
[test]   PASS  non-power-of-two alignment rejected
[test]   PASS  alignment above page size rejected

[test] --- Heap: coalescing ---
[test]   PASS  freeing middle, first, last coalesces fully

[test] --- Heap: invalid and double frees ---
[test]   PASS  free between two live blocks
[heap] rejected kfree(0xffffc00000000080): double free
[test]   PASS  double free detected
[heap] rejected kfree(0xffffffff80226f50): not a heap allocation
[test]   PASS  free of stack pointer rejected
[heap] rejected kfree(0xffffc00000000028): not a heap allocation
[test]   PASS  free of misaligned interior pointer rejected
[heap] rejected kfree(0xffffc00000000030): not a heap allocation
[test]   PASS  free of aligned interior pointer rejected
[test]   PASS  kfree(NULL) is a no-op
[test]   PASS  heap consistent after rejected frees

[test] --- Heap: growth and out-of-memory ---
[test]   PASS  1 MiB allocation grows the heap
[test]   PASS  every page of the large allocation is backed
[test]   PASS  allocation above heap limit returns NULL
[test]   PASS  SIZE_MAX allocation returns NULL (no overflow)
[test]   PASS  allocation exceeding remaining heap space returns NULL
[test]   PASS  failures counted
[test]   PASS  heap consistent after failed allocations
[test]   PASS  kzalloc returns zeroed memory (even when reusing a dirty block)

[test] --- Address spaces: isolation foundation ---
[test]   PASS  create two address spaces
[test]   PASS  map the same user address in both spaces
[test]   PASS  space A translates to frame A
[test]   PASS  space B translates to frame B
[test]   PASS  kernel space has no user mapping
[test]   PASS  global user mapping rejected
[test]   PASS  later kernel mappings are shared by all spaces
[test]   PASS  running in space A sees frame A
[test]   PASS  running in space B sees frame B at the same address
[test]   PASS  kernel remains accessible after switching spaces
[test]   PASS  destroying the kernel space is refused
[test]   PASS  destroy both address spaces
[test]   PASS  all page-table pages returned to the PMM

[test] ===== 114 passed, 0 failed =====
[pmm] Total physical memory :   523768 KiB
[pmm] Usable memory         :   522448 KiB
[pmm] Reserved memory       :     1320 KiB (kernel, bootloader, ACPI)
[pmm] Free memory           :   519244 KiB (129811 pages)
[pmm] Allocated pages       :      797 of 130608 managed
[pmm] Non-RAM ranges        : 12587361 KiB (firmware/MMIO, excluded)
[heap] mapped=1092 KiB used=0 B free=1118176 B blocks=1 (free 1) largest_free=1118176 7
```

## 5. Phase 4 process and thread tests

Process and thread creation, the deterministic A/B ping-pong, register and stack isolation, lifecycle and memory tests. The `[proc] ... failed` lines are expected: they show invalid requests being rejected.

```text
[kstack] 256 slots of 32 KiB (16 KiB stack + guard) at 0xffffe00000000000
[proc] Kernel process PID 0 created; boot flow adopted as TID 0 (stack 0xffffffff80218)

[test] ===== Phase 4 process / thread tests =====

[test] --- Process and thread creation ---
[test]   PASS  process receives a valid PID
[test]   PASS  process is registered and found by PID
[test]   PASS  new process is NEW with no threads
[test]   PASS  process has its own address space
[test]   PASS  kernel half is shared into the process address space
[test]   PASS  second process: distinct PID, shares the kernel address space
[proc] process_create failed: invalid object or argument
[test]   PASS  unknown process flags rejected
[test]   PASS  threads receive valid, distinct TIDs
[test]   PASS  threads found by TID
[test]   PASS  new threads start READY
[test]   PASS  threads are linked to their owning process
[test]   PASS  process state follows its threads (READY)
[test]   PASS  kernel stack: 16 KiB, 16-byte aligned top, in the stack region
[test]   PASS  each thread has an independent, non-overlapping stack
[test]   PASS  stack pages are mapped; the page below the stack is an unmapped guard
[test]   PASS  stack canary installed
[test]   PASS  initial context: rip=trampoline, r12=thread, IF off, at stack top
[test]   PASS  running thread has no saved context
[test]   PASS  READY-thread lookup walks the table and wraps
[proc] thread_create failed: invalid object or argument
[test]   PASS  thread in NULL process rejected
[proc] thread_create failed: invalid object or argument
[test]   PASS  thread without entry point rejected
[test]   PASS  process_terminate terminates the process and its threads
[proc] thread_create (process terminated) failed: operation not allowed in current stae
[test]   PASS  no new threads in a terminated process
[test]   diagnostics for a freshly created thread:
[BHARAT PROC] ------------------------------------------
  PID      : 1 (proc-a)
  TID      : 1 (t1)
  STATE    : READY
  KSTACK   : 0xffffe00000004000 - 0xffffe00000008000 (16 KiB, peak use 56 B, canary in)
  RSP      : 0xffffe00000007fb0 (saved)
  RIP      : 0xffffffff8020fccd (saved return address)
  ADDR SPC : PML4 0x00000000003ef000 (own address space)
  SWITCHES : 0
[test]   PASS  terminate process with two unstarted threads
[test]   PASS  reaping removes the terminated threads and processes

[test] --- Context switching: A <-> B ping-pong ---
[BHARAT] Creating thread A
[BHARAT] Creating thread B
[test]   PASS  create two processes, one thread each

[BHARAT] Process table before the first switch:
╔══════════════════════════════════════════════════════════════════════════╗
║ BHARAT PROCESS MANAGER                                                   ║
╠══════════════════════════════════════════════════════════════════════════╣
║ PID  TID  STATE       NAME       KSTACK TOP         RSP                  ║
║ 0    0    RUNNING     kmain      0xffffffff80227000 0xffffffff80226e00   ║
║ 3    4    READY       thread-A   0xffffe00000008000 0xffffe00000007fb0   ║
║ 4    5    READY       thread-B   0xffffe00000010000 0xffffe0000000ffb0   ║
╠══════════════════════════════════════════════════════════════════════════╣
║ PID 0   kernel     RUNNING     thr=1   PML4=0x0000000000076000 kernel    ║
║ PID 3   demo-a     READY       thr=1   PML4=0x00000000003ef000 own       ║
║ PID 4   demo-b     READY       thr=1   PML4=0x00000000003f0000 own       ║
╚══════════════════════════════════════════════════════════════════════════╝

[A] counter = 1  (TID 4, rsp in own stack)
[B] counter = 101  (TID 5, rsp in own stack)
[A] counter = 2  (TID 4, rsp in own stack)
[B] counter = 102  (TID 5, rsp in own stack)
[A] counter = 3  (TID 4, rsp in own stack)
[B] counter = 103  (TID 5, rsp in own stack)
[A] counter = 4  (TID 4, rsp in own stack)
[B] counter = 104  (TID 5, rsp in own stack)
[A] counter = 5  (TID 4, rsp in own stack)
[B] counter = 105  (TID 5, rsp in own stack)
[A] counter = 6  (TID 4, rsp in own stack)
[B] counter = 106  (TID 5, rsp in own stack)
[A] counter = 7  (TID 4, rsp in own stack)
[B] counter = 107  (TID 5, rsp in own stack)
[A] counter = 8  (TID 4, rsp in own stack)
[B] counter = 108  (TID 5, rsp in own stack)
[A] counter = 9  (TID 4, rsp in own stack)
[B] counter = 109  (TID 5, rsp in own stack)
[A] counter = 10  (TID 4, rsp in own stack)
[B] counter = 110  (TID 5, rsp in own stack)
[test]   PASS  kmain switches to thread A and is resumed
[test]   PASS  kernel address space restored on return to kmain
[test]   PASS  A finished all rounds and exited; B is suspended READY
[test]   PASS  B resumes where it stopped and finishes
[test]   PASS  trace is exactly A1 B101 A2 B102 ... A10 B110
[test]   PASS  each thread always ran in its own process's address space
[test]   PASS  every switch succeeded
[test]   PASS  reaping returns all stacks, structures and PML4s

[test] --- Context preservation: callee-saved registers and RFLAGS.IF ---
[test]   PASS  create two register-test threads
[test]   PASS  both threads completed every round (execution resumed correctly)
[test]   PASS  rbx, rbp, r12-r15 preserved across 50 switches with different values ped
[test]   PASS  each thread keeps its own interrupt-enable flag across switches

[test] --- Stress: 3-thread ring, 60000 switches, interrupts enabled ---
[test]   PASS  create a ring of three threads
[test]   60003 switches into ring threads; 3 timer interrupts arrived meanwhile
[test]   PASS  every member completed all laps with its local counter intact
[test]   PASS  execution followed the exact ring order A -> B -> C -> A
[test]   PASS  callee-saved registers intact on every one of the switches
[test]   PASS  thousands of switches performed

[test] --- Stack isolation: 8 KiB of locals + recursion per thread ---
[test]   PASS  create two stack-isolation threads
[test]   iso-1 locals at 0xffffe00000005fa0, iso-2 locals at 0xffffe0000000dfa0
[test]   PASS  each thread's locals live inside its own kernel stack
[test]   PASS  both threads ran to completion
[test]   PASS  every word was verified after every switch
[test]   PASS  A's stack data == expected A state, B's == expected B state
[test]   PASS  stack canaries intact after heavy stack use
[test]   peak stack use: iso-1 10528 B, iso-2 10528 B of 16384 B

[test] --- Lifecycle: termination, cleanup, invalid transitions ---
[proc] set_state: only thread_switch may make TID 13 RUNNING
[test]   PASS  only thread_switch can make a thread RUNNING
[proc] invalid state transition for TID 13: READY -> BLOCKED
[test]   PASS  READY -> BLOCKED is not a valid transition
[test]   PASS  thread returns from its entry: TERMINATED with exit code 42
[test]   PASS  process terminates with its last thread
[test]   PASS  terminated thread keeps its stack until reaped (no premature free)
[proc] invalid state transition for TID 13: TERMINATED -> READY
[test]   PASS  a TERMINATED thread cannot be revived
[proc] thread_switch: TID 13 is TERMINATED, not a READY thread
[test]   PASS  switching to a TERMINATED thread is refused
[test]   PASS  reap destroys the thread and its empty process
[test]   PASS  reaped thread's stack is unmapped and its frames freed
[test]   PASS  create three threads in one process
[test]   PASS  thread_exit(7) from inside a thread
[test]   PASS  process stays alive while it has live threads
[test]   PASS  destroying a live thread is refused
[test]   PASS  a never-run thread can be terminated and destroyed
[test]   PASS  thread blocks itself and switches away (BLOCKED)
[proc] thread_switch: TID 16 is BLOCKED, not a READY thread
[test]   PASS  a BLOCKED thread cannot be switched to
[test]   PASS  BLOCKED -> READY, then it resumes and finishes
[test]   PASS  process terminates when its last thread exits
[proc] thread_switch: invalid thread 0x0000000000000000
[test]   PASS  switch to NULL rejected
[proc] thread_switch: invalid thread 0xffffffff80226e30
[test]   PASS  switch to an unregistered thread rejected
[proc] thread_switch: TID 0 is the current thread, not a READY thread
[test]   PASS  switch to the running thread rejected
[test]   PASS  the running thread cannot be destroyed or terminated from outside
[test]   PASS  the running thread's process cannot be terminated

[test] --- Memory integration: stack sourcing, exhaustion, table limits ---
[test]   PASS  a thread's stack is exactly 4 PMM frames
[test]   PASS  destroying the thread returns them
[test]   PASS  the process was reaped together with its only thread
[proc] thread_create failed: invalid object or argument
[test]   PASS  a reaped (dangling) process pointer is rejected, not dereferenced
[proc] thread_create (kernel stack) failed: out of memory
[test]   PASS  thread creation fails cleanly when no stack frames are left
[proc] process_create (address space) failed: out of memory
[test]   PASS  process creation fails cleanly when no PML4 frame is left
[test]   PASS  failed creations leak nothing
[proc] thread_create failed: table full
[test]   created 255 threads before the table filled (256 live, 255 stacks)
[test]   PASS  thread table fills up at THREAD_MAX and reports it
[test]   PASS  every one of them has its own kernel stack
[proc] process_create failed: table full
[test]   PASS  process table fills up at PROC_MAX and reports it
[test]   PASS  no leaks: PMM frames, heap bytes, stacks, table entries all restored

[test] ===== 83 passed, 0 failed =====

[BHARAT] Process table after all tests (only the kernel remains):
╔══════════════════════════════════════════════════════════════════════════╗
║ BHARAT PROCESS MANAGER                                                   ║
╠══════════════════════════════════════════════════════════════════════════╣
║ PID  TID  STATE       NAME       KSTACK TOP         RSP                  ║
║ 0    0    RUNNING     kmain      0xffffffff80227000 0xffffffff80226e00   ║
╠══════════════════════════════════════════════════════════════════════════╣
║ PID 0   kernel     RUNNING     thr=1   PML4=0x0000000000076000 kernel    ║
╚══════════════════════════════════════════════════════════════════════════╝

╔══════════════════════════════════════════════════════════════════════════╗
║ BHARAT PROCESS MANAGER - Phase 4 results                                 ║
╠══════════════════════════════════════════════════════════════════════════╣
║ Context Switch Test                                                      ║
║                                                                          ║
║ kmain → Thread A (PID 3, TID 4) → Thread B (PID 4, TID 5) → A ...        ║
║                                                                          ║
║ A counter: 10   (started at 0, 10 rounds)                                ║
║ B counter: 110  (started at 100, 10 rounds)                              ║
║ switches into A and B: 22                                                ║
║                                                                          ║
.║ [PASS] Thread switching (exact A/B interleaving)                         ║
║ [PASS] Context preservation (registers, RFLAGS.IF)                       ║
║ [PASS] Stack isolation                                                   ║
║ [PASS] Lifecycle and cleanup                                             ║
║ [PASS] Memory integration, no leaks                                      ║
╠══════════════════════════════════════════════════════════════════════════╣
║ checks: 83 passed, 0 failed                                              ║
╚══════════════════════════════════════════════════════════════════════════╝
```

## 6. Phase 5 scheduler tests and live demo

Timer-driven preemption, all-register preservation, round-robin order, fairness, termination, idle and sleep, overhead, and four live dashboard refreshes, all built from real kernel state.

```text
[sched] Round-robin scheduler: 100 Hz timer, quantum 2 ticks (20 ms), idle TID 273; bot
[sched] Preemption enabled (0 threads READY).

[test] ===== Phase 5 scheduler tests =====

[test] --- Scheduler state after start ---
[test]   PASS  scheduler active, queues consistent
[test]   PASS  boot flow is the current RUNNING thread (TID 0)
[test]   PASS  idle thread exists and is kept off the ready queue

[test] --- Preemption: two threads that never yield ---
[test]   PASS  create two CPU-bound threads with no yield calls
[test]   A: 39497641 iterations, saw B progress 11 times while inside its loop
[test]   B: 38243653 iterations, saw A progress 10 times while inside its loop
[test]   preemptions during the window: 24 (A preempted 12, B preempted 11)
[test]   PASS  both non-yielding threads made progress
[test]   PASS  each saw the other advance while itself still inside its loop (it was i)
[test]   PASS  switches were forced by the timer (preemption counters)
[test]   PASS  both threads exited when told to stop

[test] --- Context preservation: all 15 GPRs across preemption ---
[test]   regspin 0: 4297549 checks of all registers, 9 preemptions, 0 mismatches
[test]   regspin 1: 4213743 checks of all registers, 9 preemptions, 0 mismatches
[test]   regspin 2: 4210754 checks of all registers, 9 preemptions, 0 mismatches
[test]   PASS  rax..r15 intact through every preemption in three competing threads

[test] --- Stack isolation: three threads rewriting stack data while preempted ---
[test]   iso 0: 143749 rounds verified, 11 preemptions, 0 corrupted words
[test]   iso 1: 154509 rounds verified, 11 preemptions, 0 corrupted words
[test]   iso 2: 137921 rounds verified, 10 preemptions, 0 corrupted words
[test]   PASS  each thread's stack data stayed exactly as it left it

[test] --- Round-robin order: A -> B -> C -> A ... ---
[test]   CPU handed out in this order: A B C A B C A B C A B C A B C A B C A B C A
[test]   PASS  strict FIFO rotation: every thread in turn, same order each cycle

[test] --- Fairness: three CPU-bound threads for a fixed number of ticks ---

  SCHEDULER FAIRNESS TEST (154 ticks, quantum 2)

  Thread A :  26 time slices,  52 CPU ticks, 123288859 loop iterations
  Thread B :  26 time slices,  52 CPU ticks, 123578181 loop iterations
  Thread C :  25 time slices,  50 CPU ticks, 117762496 loop iterations

[test]   PASS  No runnable thread starved
[test]   PASS  time slices within 2 and CPU ticks within 2 quanta of each other

[test] --- Termination under the scheduler ---
[test]   PASS  running threads terminate by returning and by thread_exit
[test]   PASS  no TERMINATED thread left on any scheduler queue
[test]   PASS  a terminated READY thread is never scheduled
[test]   PASS  thread is BLOCKED on the sleep list
[test]   PASS  terminating its process takes it off the sleep list consistently
[test]   PASS  idle thread reclaimed every exited thread's stack

[test] --- Idle thread and sleeping ---
[test]   slept 20 ticks; idle ran for 20 ticks, 1 switches to idle
[test]   PASS  idle thread runs when nothing else is READY
[test]   PASS  sleep(20) blocks for 20 ticks (within one quantum)
[test]   PASS  idle gives the CPU back as soon as a thread wakes

[test] --- Scheduler overhead sanity check ---
[test]   timer frequency        : 100 Hz (tick = 10 ms)
[test]   scheduling interval    : 2 ticks = 20 ms
[test]   TSC cycles per tick    : 29773224
[test]   yield switches measured: 10003
[test]   cycles per switch      : 2339
[test]   preemptive switch cost : ~0.0039% of CPU at 50 switches/s
[test]   PASS  every yield between two threads is a switch
[test]   PASS  scheduling overhead below 5% of CPU time

[test] --- Live multitasking demo ---
[demo] Tracing the first scheduling decisions (rate-limited):
[SCHED] tick=450 current=TID 0 (BLOCKED) next=TID 294 (cpu) reason=sleep switch_count=1
[SCHED] tick=452 current=TID 294 (RUNNING) next=TID 295 (memory) reason=quantum expire2
[SCHED] tick=454 current=TID 295 (RUNNING) next=TID 296 (counter) reason=quantum expir3
[SCHED] tick=456 current=TID 296 (RUNNING) next=TID 294 (cpu) reason=quantum expired s4
[SCHED] tick=458 current=TID 294 (RUNNING) next=TID 295 (memory) reason=quantum expire5
[SCHED] tick=460 current=TID 295 (RUNNING) next=TID 296 (counter) reason=quantum expir6

╔══════════════════════════════════════════════════════════════════════════╗
║ BHARAT SCHEDULER  (live, refresh 1/4)                                    ║
╠══════════════════════════════════════════════════════════════════════════╣
║ TID  STATE      WORK             ITERATIONS  SWITCHES CPU TICKS          ║
║ ───────────────────────────────────────────────────────────────────      ║
║ 0    RUNNING    kmain                     -        32         4          ║
║ 273  READY      idle                      -        12        79          ║
║ 294  READY      CPU Worker            52696         9        18          ║
║ 295  READY      Memory Worker         21276         9        18          ║
║ 296  READY      Counter            39048116         9        18          ║
║                                                                          ║
║ Timer ticks       : 504                                                  ║
║ Context switches  : 10258 (221 preemptive, 10037 voluntary)              ║
║ Runnable threads  : 3    Current thread : TID 0 (this dashboard)         ║
║ Scheduling: ROUND-ROBIN, quantum 20 ms                                   ║
╚══════════════════════════════════════════════════════════════════════════╝

╔══════════════════════════════════════════════════════════════════════════╗
║ BHARAT SCHEDULER  (live, refresh 2/4)                                    ║
╠══════════════════════════════════════════════════════════════════════════╣
║ TID  STATE      WORK             ITERATIONS  SWITCHES CPU TICKS          ║
║ ───────────────────────────────────────────────────────────────────      ║
║ 0    RUNNING    kmain                     -        34         7          ║
║ 273  READY      idle                      -        12        79          ║
║ 294  READY      CPU Worker            88889        19        38          ║
║ 295  READY      Memory Worker         47254        19        38          ║
║ 296  READY      Counter            86347150        19        38          ║
║                                                                          ║
║ Timer ticks       : 567                                                  ║
║ Context switches  : 10290 (252 preemptive, 10038 voluntary)              ║
║ Runnable threads  : 3    Current thread : TID 0 (this dashboard)         ║
║ Scheduling: ROUND-ROBIN, quantum 20 ms                                   ║
╚══════════════════════════════════════════════════════════════════════════╝

╔══════════════════════════════════════════════════════════════════════════╗
║ BHARAT SCHEDULER  (live, refresh 3/4)                                    ║
╠══════════════════════════════════════════════════════════════════════════╣
║ TID  STATE      WORK             ITERATIONS  SWITCHES CPU TICKS          ║
║ ───────────────────────────────────────────────────────────────────      ║
║ 0    RUNNING    kmain                     -        36        10          ║
║ 273  READY      idle                      -        12        79          ║
║ 294  READY      CPU Worker           118015        29        58          ║
║ 295  READY      Memory Worker         73903        29        58          ║
║ 296  READY      Counter           135056020        29        58          ║
║                                                                          ║
║ Timer ticks       : 630                                                  ║
║ Context switches  : 10322 (283 preemptive, 10039 voluntary)              ║
║ Runnable threads  : 3    Current thread : TID 0 (this dashboard)         ║
║ Scheduling: ROUND-ROBIN, quantum 20 ms                                   ║
╚══════════════════════════════════════════════════════════════════════════╝

╔══════════════════════════════════════════════════════════════════════════╗
║ BHARAT SCHEDULER  (live, refresh 4/4)                                    ║
╠══════════════════════════════════════════════════════════════════════════╣
║ TID  STATE      WORK             ITERATIONS  SWITCHES CPU TICKS          ║
║ ───────────────────────────────────────────────────────────────────      ║
║ 0    RUNNING    kmain                     -        38        13          ║
║ 273  READY      idle                      -        12        79          ║
║ 294  READY      CPU Worker           144596        39        78          ║
║ 295  READY      Memory Worker         99433        39        78          ║
║ 296  READY      Counter           183605757        39        78          ║
║                                                                          ║
║ Timer ticks       : 693                                                  ║
║ Context switches  : 10354 (314 preemptive, 10040 voluntary)              ║
║ Runnable threads  : 3    Current thread : TID 0 (this dashboard)         ║
║ Scheduling: ROUND-ROBIN, quantum 20 ms                                   ║
╚══════════════════════════════════════════════════════════════════════════╝
[test]   PASS  demo: all three workers progressed, were preempted, no memory errors
[test]   PASS  scheduler queues consistent after the demo

[test] ===== 26 passed, 0 failed =====

BHARAT SCHEDULER
  Policy            : ROUND-ROBIN, quantum 2 ticks
  Timer ticks       : 703
  Scheduler calls   : 10747
  Context switches  : 10363 (319 preemptive, 10044 voluntary)
  Runnable threads  : 0
  Sleeping threads  : 0
  Idle ticks        : 80
  Current thread    : TID 0

╔══════════════════════════════════════════════════════════════════════════╗
║                     BHARAT SYSTEM STATUS                                 ║
╠══════════════════════════════════════════════════════════════════════════╣
║ Scheduler       : ROUND-ROBIN (quantum 20 ms)                            ║
║ CPU             : x86-64, 1 CPU                                          ║
║ Timer           : ACTIVE, PIT 100 Hz                                     ║
║                                                                          ║
║ TID  WORK             ITERATIONS  SWITCHES  PREEMPTED CPU TICKS          ║
║ ───────────────────────────────────────────────────────────────────      ║
║ 294  CPU Worker           147112        41         40        80          ║
║ 295  Memory Worker        102059        41         40        80          ║
║ 296  Counter           188387511        41         40        80          ║
║ Memory worker errors: 0                                                  ║
║                                                                          ║
║ Timer ticks       : 704                                                  ║
║ Context switches  : 10363 (319 preemptive, 10044 voluntary)              ║
║ Idle ticks        : 80                                                   ║
║                                                                          ║
║ [PASS] Preemption (non-yielding threads interleaved)                     ║
║ [PASS] Context preservation (all 15 GPRs)                                ║
║ [PASS] Thread isolation (stack data under preemption)                    ║
║ [PASS] Round-robin order                                                 ║
║ [PASS] Scheduler fairness                                                ║
║ [PASS] Termination (never rescheduled, reclaimed)                        ║
║ [PASS] Idle thread and sleep                                             ║
╠══════════════════════════════════════════════════════════════════════════╣
║ checks: 26 passed, 0 failed                                              ║
╚══════════════════════════════════════════════════════════════════════════╝
```

## 7. Phase 6 changes to this output

This capture predates Phase 6. A current boot differs in three places
(full details in `../phases/phase-6-debug-hardening.md`):

- The banner's second line reads `Phase 6: Debug and Hardening Tools`.
- In the live demo, the six `[SCHED] tick=...` lines are replaced by the
  latest six events from the kernel event trace, for example:

  ```text
  [demo] Latest scheduling decisions from the event trace:
  Recent events (6 of 10594, oldest first):
    [10589] +0 cycles  sched.preempt a=0x126 b=0x127
    [10590] +60855604 cycles  sched.preempt a=0x127 b=0x128
  ```

- After the scheduler tests, a new suite runs (29 checks, below), and the
  final status gains `[ok] Phase 6 debug and hardening verified.`

The new suite's output. The two `[heap] check:` lines are deliberate bad
writes being detected, as the test intends:

```text
===== Debug and hardening tests =====

[debug] Symbol table
  [PASS] symbol table has the kernel's functions
  [PASS] symbols are sorted by address
  [PASS] kernel_main resolves to kernel_main+0x0
  [PASS] an address inside a function gives name+offset
  [PASS] data and null addresses are not reported as functions

[debug] Call traces
  [PASS] walk finds at least four frames
  [PASS] frames are bt_level2 <- bt_level1 <- test_backtrace
  sample trace from here:
Call trace (most recent call first):
  #0 0xffffffff80207ff2  test_backtrace+0xe2
  #1 0xffffffff80207771  debug_run_tests+0x241
  #2 0xffffffff80200af3  kernel_main+0x313
  #3 0xffffffff8020018a  _start+0x4a
  [PASS] a null frame pointer gives an empty trace
  [PASS] an unmapped frame pointer gives an empty trace

[debug] Stack protector
  [PASS] canary was randomised at boot
  [PASS] canary's low byte is 0 (stops string copies)

[debug] Heap poisoning and red zones
  [PASS] heap is consistent before the test
  [PASS] allocation is followed by a red zone
  [PASS] new allocation holds the poison pattern, not old data
  [PASS] writing exactly the requested size leaves the red zone intact
  (a red-zone report is expected next)
[heap] check: red zone byte 0xffffc00000000114 of block 0xffffc00000000100 was overwritten
  [PASS] write one byte past the end is caught
  [PASS] heap is consistent again once repaired
  [PASS] free the block
  [PASS] freed memory is poisoned
  (a use-after-free report is expected next)
[heap] check: freed memory at 0xffffc00000000108 was written after free
  [PASS] write into freed memory is caught
  [PASS] heap is consistent again once repaired
  [PASS] kzalloc zeroes only the requested bytes
  [PASS] heap is consistent after the test

[debug] Event trace
  [PASS] boot already recorded events (threads, switches)
  [PASS] events come back in order with their arguments and timestamps
  [PASS] newest event survives a full wrap
  [PASS] total counts overwritten events too
  [PASS] disabled trace records nothing
  [PASS] event type names, including out-of-range types

===== Debug tests: 29 passed, 0 failed =====
```

## 8. Final status

Every phase reports verified. The dots are the one-per-second heartbeat while the idle thread halts the CPU.

```text
[ok] Kernel started successfully.
[ok] Phase 3 memory management verified.
[ok] Phase 4 process management verified.
[ok] Phase 5 scheduler verified.
[ok] Phase 6 debug and hardening verified.   (added in Phase 6)
......................................................................................
```

---

## Phase 10 additions (V1 core)

Captured by `python build.py test` (4 CPUs). Passing checks are left out;
the end is the typed session: the harness logs in as `tester` and types
commands over the serial line and (for `about`) on the PS/2 keyboard.

```text
[input] PS/2 keyboard on (IRQ 1), PS/2 mouse on (IRQ 12), serial input on (IRQ 4)

[test] ===== Phase 10: input =====

[test] --- Keyboard decoder (scancode set 1) ---

[test] --- Terminal (serial) decoder ---

[test] --- Mouse decoder (PS/2 packets) ---

[test] --- Event queue ---

[test] --- PS/2 bytes through the driver ---

[test] --- Devices and interrupts ---

[test] --- Sleeping until input arrives ---

===== Input tests: 32 passed, 0 failed =====
[fs] Files in memory: 4 files loaded from the boot bundle (10240 bytes)

[test] ===== Phase 10: files in memory =====

[test] --- Paths ---

[test] --- Create, write, read, list, delete ---

[test] --- Bad paths and wrong types ---

[test] --- Limits ---

[test] --- Boot bundle (tar) ---

===== Files tests: 46 passed, 0 failed =====

[test] ===== Phase 10: login and shell =====

[test] --- Line editor ---

[test] --- Login rules ---

[test] --- Shell: files ---

[test] --- Shell: system commands ---

[test] --- Shell: threads, checks and demos ---

[lockcheck] Lock order problem on CPU 0:
[lockcheck]   taking "demo A" while holding "demo B"
[lockcheck]   but earlier "demo A" was held while taking "demo B".
[lockcheck]   Two CPUs doing these at the same time would each wait
[lockcheck]   for the other forever (a deadlock).
[lockcheck] Earlier order ("demo A" then "demo B", CPU 0):
Call trace (most recent call first):
  #0 0xffffffff8020463d  demo_lock_pair+0x2d
  #1 0xffffffff80203f96  cmd_lockdemo+0x66
  #2 0xffffffff80201d3f  shell_run+0x2af
  #3 0xffffffff80228881  shell_run_tests+0x2021
  #4 0xffffffff80206454  kernel_main+0x4d4
  #5 0xffffffff8020022a  _start+0x4a
[lockcheck] Now:
Call trace (most recent call first):
  #0 0xffffffff80217eaf  lock_irqs_off+0x8f
  #1 0xffffffff8020463d  demo_lock_pair+0x2d
  #2 0xffffffff80203fba  cmd_lockdemo+0x8a
  #3 0xffffffff80201d3f  shell_run+0x2af
  #4 0xffffffff80228881  shell_run_tests+0x2021
  #5 0xffffffff80206454  kernel_main+0x4d4
  #6 0xffffffff8020022a  _start+0x4a

[#PF] Page fault
[#PF]   faulting address : 0x0000000000000000 (null page)
[#PF]   instruction ptr  : 0xffffffff802046a9
[#PF]   error code       : 0x0 [P=0 W=0 U=0 RSVD=0 I/D=0]
[#PF]   access           : read
[#PF]   mode             : kernel
[#PF]   cause            : page not present

[crash] Thread "crashdemo" (TID 339) crashed: Page fault (#PF)
[crash] It touched address 0x0000000000000000, which is not usable memory.
[crash] Registers:
  rip=0xffffffff802046a9 cs=0008 rflags=0x0000000000010297
  rsp=0xffffe00000027fb0 ss=0010
  rax=0x0000000000000000 rbx=0x0000000000000000 rcx=0x0000000000000000
  rdx=0x0000000000000000 rsi=0x0000000000000001 rdi=0x0000000000000000
  rbp=0xffffe00000027fc0 r8 =0x0000000000000000 r9 =0x0000000000003068
  r10=0x0000000000000338 r11=0x00000000000000d8 r12=0xffff800000a88380
  r13=0x0000000000000000 r14=0x0000000000000000 r15=0x0000000000000000
  cr2=0x0000000000000000 cr3=0x000000001fdd6000
Call trace (most recent call first):
  #0 0xffffffff802046a9  crash_thread+0x39
  #1 0xffffffff80213b8a  thread_bootstrap+0x2a
  #2 0xffffffff8022d595  thread_trampoline+0x8
[crash] Only this thread was stopped; the rest of the system keeps running.

[crash] Thread "crashdemo" (TID 340) crashed: Divide-by-zero (#DE)
[crash] Registers:
  rip=0xffffffff8020469a cs=0008 rflags=0x0000000000010246
  rsp=0xffffe00000027fb0 ss=0010
  rax=0x0000000000000001 rbx=0x0000000000000001 rcx=0x0000000000000000
  rdx=0x0000000000000000 rsi=0x0000000000000001 rdi=0x0000000000000001
  rbp=0xffffe00000027fc0 r8 =0x0000000000000000 r9 =0x0000000000003071
  r10=0x0000000000000380 r11=0x00000000000000d8 r12=0xffff800000a88380
  r13=0x0000000000000000 r14=0x0000000000000000 r15=0x0000000000000000
  cr2=0x0000000000000000 cr3=0x000000001fdd6000
Call trace (most recent call first):
  #0 0xffffffff8020469a  crash_thread+0x2a
  #1 0xffffffff80213b8a  thread_bootstrap+0x2a
  #2 0xffffffff8022d595  thread_trampoline+0x8
[crash] Only this thread was stopped; the rest of the system keeps running.

[crash] Thread "crashdemo" (TID 341) crashed: Invalid opcode (#UD)
[crash] Registers:
  rip=0xffffffff8020469e cs=0008 rflags=0x0000000000010246
  rsp=0xffffe00000027fb0 ss=0010
  rax=0xffff800000a88380 rbx=0x0000000000000002 rcx=0x0000000000000000
  rdx=0x0000000000000000 rsi=0x0000000000000001 rdi=0x0000000000000002
  rbp=0xffffe00000027fc0 r8 =0x0000000000000000 r9 =0x0000000000003079
  r10=0x00000000000003c0 r11=0x00000000000000d8 r12=0xffff800000a88380
  r13=0x0000000000000000 r14=0x0000000000000000 r15=0x0000000000000000
  cr2=0x0000000000000000 cr3=0x000000001fdd6000
Call trace (most recent call first):
  #0 0xffffffff8020469e  crash_thread+0x2e
  #1 0xffffffff80213b8a  thread_bootstrap+0x2a
  #2 0xffffffff8022d595  thread_trampoline+0x8
[crash] Only this thread was stopped; the rest of the system keeps running.

===== Login and shell tests: 58 passed, 0 failed =====

[ok] Kernel started successfully.
[ok] Phase 3 memory management verified.
[ok] Phase 4 process management verified.
[ok] Phase 5 scheduler verified.
[ok] Phase 6 debug and hardening verified.
[ok] Phase 7 modern hardware verified.
[ok] Phase 8 memory that scales verified.
[ok] Phase 9 multi-core verified.
[ok] Phase 10 input verified.
[ok] Phase 10 files in memory verified.
[ok] Phase 10 login and shell verified.
[ok] 516 boot self-checks passed, 0 failed.

   B H A R A T   O S   -   V1 core
============================================
Log in with any name (letters and digits). Example: asha

BharatOS login: tester

Welcome to BharatOS!

Type 'help' to see what you can do. Your files are in memory: they are
kept while the machine runs and start fresh at the next boot.

Hello, tester! Type 'help' to see what you can do.
tester@bharat:~$ echo typed at the prompt > t.txt
tester@bharat:~$ cat t.txt
typed at the prompt
tester@bharat:~$ QEMU 11.1.0 monitor - type 'help' for more information
bout
BharatOS V1 core: keyboard, mouse, files in memory, login and terminal
An operating system written from scratch for x86-64 PCs, in C and assembly.
  CPUs:        4, each with its own run queue; idle CPUs steal work
  Memory:      511 MiB; buddy page allocator, slab caches, demand paging
  Scheduler:   preemptive, tickless (no timer interrupt when idle)
  Safety:      lock-order checker, heap poisoning, stack canaries,
               crash reports with function names
  Self-checks: 516 passed at this boot
tester@bharat:~$ checks
Boot self-tests (run at every start):
  Memory management         114 passed
  Processes and threads      83 passed
  Scheduler                  26 passed
  Debug and hardening        29 passed
  Modern hardware            37 passed
  Memory that scales         62 passed
  Multi-core                 29 passed
  Input                      32 passed
  Files                      46 passed
  Login and shell            58 passed
  Total                     516 passed, 0 failed
Live checks, now:
  [ok] kernel heap is consistent
  [ok] physical page allocator is consistent
  [ok] scheduler queues are consistent
  [ok] file tree is consistent
  [ok] a file can be written, read and deleted
  [ok] a thread can sleep and be woken
  [ok] the lock-order checker is learning
  [ok] every other CPU answers a request
All good: 516 boot checks and 8 live checks passed.
tester@bharat:~$
```

## Phase 11 additions (V1 desktop)

The screen now starts with a splash (the boot log runs below it), and a
desktop suite runs after the shell's. Then the desktop thread starts next
to the text console. The boot test types `tester` into the graphical
login on the emulated PS/2 keyboard, presses Shift+F12 for the quick
tour, and saves a screenshot. Captured with `python build.py test`
(4 CPUs):

```text
[test] ===== Phase 11: desktop =====

[test] --- Drawing ---
[test]   PASS  a filled rectangle covers exactly its pixels
[test]   PASS  drawing stays inside the clip rectangle
[test]   PASS  a rectangle off the surface draws nothing (and does not crash)
[test]   PASS  text: 'H' is 8 pixels wide, its stroke drawn, its middle gap left
[test]   PASS  rectangle union and intersection
[test]   PASS  present copies only the damaged rectangle
[test]   PASS  the boot splash moved on at every boot step

[test] --- Text buffer ---
[test]   PASS  a 10x3 buffer is made; a 0-wide one is refused
[test]   PASS  text and newline land in the right cells
[test]   PASS  a long line wraps; the oldest line scrolls away
[test]   PASS  escape codes clear the screen and move home
[test]   PASS  box-drawing characters become - | and others ?
[test]   PASS  the window manager starts (drawing into memory only)

[test] --- Login screen ---
[test]   PASS  a name with spaces is refused with a message
[test]   PASS  a user with a password is asked for it
[test]   PASS  a wrong password is refused
[desktop] tester logged in.
[test]   PASS  a plain name logs in
[test]   PASS  the desktop is drawn with the app buttons on the top bar

[test] --- Windows ---
[test]   PASS  two windows open; the newer one has the focus
[test]   PASS  focus moves to the window brought forward
[test]   PASS  clicking a title bar focuses the window; dragging moves it
[test]   PASS  a window cannot be moved off the screen
[test]   PASS  moving the pointer redraws only a small damaged area
[test]   PASS  nothing changed: nothing is redrawn
[test]   PASS  windows can be opened up to the limit, then are refused
[test]   PASS  the x closes a window; focus goes back
[test]   PASS  the top bar opens an app, and a second click does not open it twice
[test]   PASS  F3 opens the Monitor
[test]   PASS  Ctrl+W closes the focused window

[test] --- Terminal window ---
[test]   PASS  a command typed in the window runs; its output and a new prompt appear
[test]   PASS  the shell's folder and prompt follow cd
[test]   PASS  clear empties the window
[test]   PASS  logout in a terminal closes it

[test] --- Files and the text editor ---
[test]   PASS  Files opens in the user's home folder
[test]   PASS  New file asks for a name, creates it and selects it
[test]   PASS  New folder creates a folder
[test]   PASS  a '/' cannot be typed into a name
[test]   PASS  Delete removes the selected folder
[test]   PASS  Enter opens a folder
[test]   PASS  Backspace goes up again
[test]   PASS  Enter on a file opens it in the text editor
[test]   PASS  typing, arrows, Home, End and Backspace edit at the cursor
[test]   PASS  Ctrl+S saves the file
[test]   PASS  the editor refuses a missing file and a folder
[test]   PASS  files and folders made by the test are deleted again

[test] --- Monitor, Health and About ---
[test]   PASS  the monitor samples every CPU (busy between 0 and 100%)
[test]   PASS  CPU 0's bar is drawn
[test]   PASS  Health runs the live checks and shows the result
[test]   PASS  About shows what the system is
[desktop] tester logged out.
[test]   PASS  Log out returns to the login screen

===== Desktop tests: 50 passed, 0 failed =====
...
[ok] Phase 10 login and shell verified.
[ok] Phase 11 desktop verified.
[ok] 566 boot self-checks passed, 0 failed.
...
[desktop] Login screen ready (type your name in the QEMU window).
tester

Welcome to BharatOS!

Type 'help' to see what you can do. Your files are in memory: they are
kept while the machine runs and start fresh at the next boot.

Hello, tester! Type 'help' to see what you can do.
tester@bharat:~$ echo typed at the prompt > t.txt
tester@bharat:~$ cat t.txt
typed at the prompt
tester@bharat:~$ QEMU 11.1.0 monitor - type 'help' for more information
(qemu) sendkey t
(qemu) sendkey e
(qemu) sendkey s
(qemu) sendkey t
(qemu) sendkey e
(qemu) sendkey r
(qemu) sendkey ret
(qemu)
[desktop] tester logged in.
[desktop] Desktop ready for tester.
(qemu) sendkey shift-f12
(qemu)
[tour] Starting the quick tour of the desktop.
[tour] Terminal: files and CPUs
[tour] check passed: Terminal
[tour] check passed: hello from the tour
[tour] Files: create, open, edit and delete
[tour] check passed: Files
[tour] check passed: Files shows the file
[tour] check passed: the edited file was saved
[tour] check passed: Files shows the file
[tour] Monitor: a busy thread on every CPU
[tour] check passed: Monitor
[tour] Busiest CPU over the last second: 85%
[tour] check passed: the monitor shows the load
[tour] Health: checks, the lock-order checker, a contained crash
[tour] check passed: Health
[tour] check passed: All good

[lockcheck] Lock order problem on CPU 0:
[lockcheck]   taking "demo A" while holding "demo B"
[lockcheck]   but earlier "demo A" was held while taking "demo B".
[lockcheck]   Two CPUs doing these at the same time would each wait
[lockcheck]   for the other forever (a deadlock).
[lockcheck] Earlier order ("demo A" then "demo B", CPU 0):
Call trace (most recent call first):
  #0 0xffffffff80206dbd  demo_lock_pair+0x2d
  #1 0xffffffff80206716  cmd_lockdemo+0x66
  #2 0xffffffff802044bf  shell_run+0x2af
  #3 0xffffffff8020326f  health_run+0x8f
  #4 0xffffffff802340e5  tour_tick+0x2c5
  #5 0xffffffff802325ff  desktop_main+0x10f
  #6 0xffffffff80216caa  thread_bootstrap+0x2a
  #7 0xffffffff80235e85  thread_trampoline+0x8
[lockcheck] Now:
Call trace (most recent call first):
  #0 0xffffffff8021afcf  lock_irqs_off+0x8f
  #1 0xffffffff80206dbd  demo_lock_pair+0x2d
  #2 0xffffffff8020673a  cmd_lockdemo+0x8a
  #3 0xffffffff802044bf  shell_run+0x2af
  #4 0xffffffff8020326f  health_run+0x8f
  #5 0xffffffff802340e5  tour_tick+0x2c5
  #6 0xffffffff802325ff  desktop_main+0x10f
  #7 0xffffffff80216caa  thread_bootstrap+0x2a
[tour] check passed: The checker caught it

[#PF] Page fault
[#PF]   faulting address : 0x0000000000000000 (null page)
[#PF]   instruction ptr  : 0xffffffff80206e29
[#PF]   error code       : 0x0 [P=0 W=0 U=0 RSVD=0 I/D=0]
[#PF]   access           : read
[#PF]   mode             : kernel
[#PF]   cause            : page not present

[crash] Thread "crashdemo" (TID 350) crashed: Page fault (#PF)
[crash] It touched address 0x0000000000000000, which is not usable memory.
[crash] Registers:
  rip=0xffffffff80206e29 cs=0008 rflags=0x0000000000010297
  rsp=0xffffe0000005ffb0 ss=0010
  rax=0x0000000000000000 rbx=0x0000000000000000 rcx=0x0000000000000000
  rdx=0x0000000000000000 rsi=0x0000000000000001 rdi=0x0000000000000000
  rbp=0xffffe0000005ffc0 r8 =0x0000000000000000 r9 =0x00000000000037df
  r10=0x00000000000006f0 r11=0x0000000000000000 r12=0xffff800000aca930
  r13=0x0000000000000000 r14=0x0000000000000000 r15=0x0000000000000000
  cr2=0x0000000000000000 cr3=0x000000001fd94000
Call trace (most recent call first):
  #0 0xffffffff80206e29  crash_thread+0x39
  #1 0xffffffff80216caa  thread_bootstrap+0x2a
  #2 0xffffffff80235e85  thread_trampoline+0x8
[crash] Only this thread was stopped; the rest of the system keeps running.
[tour] check passed: The crash was contained
[tour] About
[tour] check passed: About
[tour] Demo finished (0 checks failed).
(qemu) screendump build/desktop.ppm
(qemu)
checks
... (the checks table: every suite, then 8 live checks) ...
All good: 566 boot checks and 8 live checks passed.
tester@bharat:~$
```
