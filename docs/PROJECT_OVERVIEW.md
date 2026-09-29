# NEXUS OS — Project Overview

**A modular x86-64 operating system, built from scratch.**

| | |
|---|---|
| **Status** | Phases 1–5 complete (**~50% of the planned roadmap**) |
| **Architecture** | x86-64 (long mode), single CPU |
| **Boot** | Limine bootloader, BIOS and UEFI |
| **Languages** | C (freestanding) and x86-64 assembly (NASM) |
| **Verification** | 223 automated boot-time checks + 8 crash-path demos, run in QEMU |

---

## 1. Problem statement

Modern operating systems are huge. Linux has tens of millions of lines of
code, which makes it very hard to see *how* an OS really works. How does a
computer go from power-on to running several programs at once, each
protected from the others?

Textbooks explain the ideas, but the hard part is the **real hardware
interface**: page tables, interrupt frames, privilege levels, stack
switching and context switches. Tiny mistakes there cause silent
corruption or instant reboots.

**NEXUS answers that problem by building an OS from the ground up:**

1. **From scratch.** No existing kernel code is reused; every subsystem is
   written and understood.
2. **Modular.** Each phase adds one well-defined layer on top of the
   previous one.
3. **Verified.** Every phase ships with automated tests that run inside the
   real kernel at boot, so "it boots" is never mistaken for "it works".
4. **Documented.** Design decisions, data layouts and known limitations are
   written down as they are made.

The end goal is a small but complete OS: protected userspace programs, a
shell, a filesystem, and eventually higher-level features, all built on a
foundation that is correct and understood at every layer.

---

## 2. Progress so far (~50%)

### Roadmap at a glance

| Phase | Title | Status |
|---|---|---|
| 1 | Bootable x86-64 kernel | ✅ Done |
| 2 | Interrupts, exceptions, timer | ✅ Done |
| 3 | Physical & virtual memory management | ✅ Done |
| 4 | Processes, threads & context switching | ✅ Done |
| 5 | Preemptive scheduler | ✅ Done |
| 6 | User mode (ring 3) + system calls | ⏳ Next |
| 7 | Userspace runtime + `nsh` shell + real user programs | Planned |
| 8+ | Filesystem, IPC, networking, GUI, AI assistant | Planned |

**The foundation is complete.** Everything an OS needs *inside the kernel*
now works and is tested:

- booting
- handling the CPU and its faults
- managing memory
- running many threads at once with automatic time-sharing

The second half of the roadmap is about what runs *on top* of the kernel:
user programs, a shell, and higher-level services.

### Test status (current build)

| Suite | Checks | Result |
|---|---|---|
| Phase 3: memory management | 114 | ✅ all pass |
| Phase 4: processes & threads | 83 | ✅ all pass |
| Phase 5: scheduler | 26 | ✅ all pass |
| Crash-path demos (`test-faults`) | 8 | ✅ all reported correctly |

The suites also pass under both BIOS and UEFI boot, in repeated boots, and
with zero compiler warnings.

### Talking points, phase by phase

#### Phase 1: Bootable x86-64 kernel
- The kernel boots in QEMU through the **Limine** bootloader, on both
  legacy BIOS and UEFI firmware.
- The CPU is already in **64-bit long mode** when our code starts.
- The kernel is linked into the **higher half** of memory
  (`0xffffffff80200000`), like real OS kernels.
- A **serial console** (COM1) is used for all output, which makes it easy
  to test automatically.
- The build system is pure Python and works on Windows, Linux and WSL.

#### Phase 2: Interrupts, exceptions and timer
- **GDT** (segment descriptors) and **TSS** (task state segment), with a
  separate emergency stack (IST1) for double faults.
- An **IDT** covering all 32 CPU exceptions plus the timer interrupt.
- Every exception prints a readable report: name, error code, instruction
  pointer and all registers.
- A **timer** (PIT at 100 Hz), with the legacy interrupt controller (PIC)
  remapped so hardware interrupts don't collide with CPU exceptions.
- Several subtle hardware-level bugs were found and fixed, such as a wrong
  TSS field size that caused instant reboots (triple faults).

#### Phase 3: Memory management
- **Physical memory manager:** tracks every 4 KiB page of RAM in a bitmap,
  using the bootloader's memory map. It detects double frees and invalid
  frees.
- **Paging:** the kernel builds its own 4-level page tables.
  - Kernel code is read-only + executable.
  - Kernel data is writable but not executable.
  - The null page and unused regions stay unmapped, so bad accesses crash
    loudly instead of silently corrupting memory.
- **Virtual memory API:** map, unmap, translate and change permissions,
  with full validation of every argument.
- **Kernel heap:** `kmalloc` and `kfree` with alignment support, coalescing
  of free blocks, and clean out-of-memory handling.
- **Page-fault reports:** the faulting address and which region it is in,
  read/write/execute, kernel/user, and present/not-present.
- **Address spaces:** separate page tables per process, all sharing one
  kernel half. This is the foundation for process isolation.

#### Phase 4: Processes, threads and context switching
- **Processes and threads:**
  - A **process** is a container: an ID and an address space.
  - A **thread** is something that runs: an ID, a state, its own stack,
    and saved CPU registers.
- **Kernel stacks:** every thread gets its own stack with an unmapped
  **guard page** below it and a **canary** value, so stack overflows are
  caught.
- **Context switch:** a small assembly routine that saves one thread's CPU
  registers and restores another's. It saves exactly the registers the
  C calling convention requires.
- **Lifecycle and safety:**
  - States: NEW, READY, RUNNING, BLOCKED, TERMINATED.
  - Finished threads are cleaned up safely later, never while their stack
    is still in use.
  - A **corrupted thread context** is detected *before* the kernel ever
    loads it.

#### Phase 5: Preemptive scheduler
- **Round-robin:** threads take turns from a FIFO queue, with a 20 ms time
  slice.
- **Preemption:** the timer interrupt forcibly switches threads, so a
  thread stuck in an endless loop can no longer hog the CPU.
- **Idle and sleep:**
  - An **idle thread** runs when nothing else can.
  - Threads can **sleep** for a number of timer ticks.
- **Safe shared data:** the heap, memory allocators and console are
  protected, so a thread interrupted halfway through an operation can't
  corrupt them.
- **Statistics and dashboard:**
  - timer ticks and context switches, with preemptive and voluntary counts
    kept separately
  - CPU ticks per thread
  - a live dashboard printed from real kernel state
- **Proven by tests:**
  - Threads that never give up the CPU still take turns.
  - All 15 general-purpose registers survive being interrupted.
  - Scheduling is strictly fair: 26 / 26 / 25 time slices for three
    competing threads.

---

## 3. System architecture

```text
┌──────────────────────────────────────────────────────────────┐
│                 (Phase 6+) User programs                     │
├──────────────────────────────────────────────────────────────┤
│  Scheduler (Phase 5)        round-robin, preemption, idle    │
├──────────────────────────────────────────────────────────────┤
│  Processes & threads (4)    registry, stacks, context switch │
├──────────────────────────────────────────────────────────────┤
│  Memory (Phase 3)           PMM · paging/VMM · heap · #PF    │
├──────────────────────────────────────────────────────────────┤
│  CPU & interrupts (2)       GDT/TSS · IDT · exceptions · PIT │
├──────────────────────────────────────────────────────────────┤
│  Boot (Phase 1)             Limine · long mode · serial      │
├──────────────────────────────────────────────────────────────┤
│                 x86-64 hardware (QEMU)                       │
└──────────────────────────────────────────────────────────────┘
```

**Kernel virtual memory layout**

```text
0x0000000000000000 – 0x00007fffffffffff   user space (private per process)
0xffff800000000000 + phys                  direct map of physical RAM (HHDM)
0xffffc00000000000                         kernel heap (up to 256 MiB)
0xffffd00000000000                         memory-test window
0xffffe00000000000                         kernel thread stacks (with guards)
0xffffffff80200000                         kernel image (.text / .rodata / .data)
```

**What happens at boot**

```text
Firmware → Limine → _start (entry.asm)
  → serial console
  → GDT/TSS → IDT → timer → interrupts on
  → memory manager (PMM → page tables → heap) → memory tests
  → process manager (boot flow becomes thread 0) → process/thread tests
  → scheduler starts → scheduler tests + live multitasking demo
  → heartbeat (the idle thread halts the CPU between ticks)
```

---

## 4. Core concepts the project is built around

This section explains, in plain terms, every idea NEXUS relies on.

### 4.1 Booting and long mode
- **Firmware** (BIOS or UEFI) starts the machine and loads a
  **bootloader**.
- **Limine** is the bootloader NEXUS uses. It loads the kernel file,
  switches the CPU into **64-bit long mode**, and hands over useful
  information: the memory map, where the kernel was loaded, and a direct
  map of physical memory.
- **Higher-half kernel:** the kernel lives at very high virtual addresses,
  leaving the low half free for user programs.

### 4.2 Privilege and segmentation (GDT, TSS)
- x86 has privilege **rings**: ring 0 is the kernel, ring 3 is user
  programs.
- The **GDT** (Global Descriptor Table) describes code and data segments
  and their privilege level. In 64-bit mode segmentation is mostly flat,
  but the GDT is still required.
- The **TSS** (Task State Segment) tells the CPU which stack to use in
  special situations. NEXUS uses its **IST** (Interrupt Stack Table) to
  give double faults their own emergency stack.

### 4.3 Interrupts and exceptions (IDT)
- **Exceptions** are raised by the CPU itself: divide by zero, invalid
  instruction, page fault, and so on.
- **Hardware interrupts** come from devices, such as the timer.
- The **IDT** (Interrupt Descriptor Table) maps each interrupt number to a
  handler.
- On an interrupt, the CPU pushes an **interrupt frame** (instruction
  pointer, flags, stack pointer). NEXUS's assembly entry code saves the
  remaining registers so the C handler sees the full CPU state.
- A **double fault** happens when handling an exception itself fails. A
  **triple fault** resets the machine, which is why the double-fault
  handler runs on its own stack.

### 4.4 Timer (PIT and PIC)
- The **PIT** (Programmable Interval Timer) raises an interrupt 100 times
  per second.
- The **PIC** (interrupt controller) routes device interrupts to the CPU.
  It must be remapped so they don't overlap CPU exception numbers, and
  told when each interrupt is handled (**EOI**, end of interrupt).
- The timer drives the scheduler.

### 4.5 Physical memory management
- RAM is divided into **4 KiB pages** (frames).
- The bootloader's **memory map** says which ranges are usable RAM and
  which are reserved (firmware, devices, the kernel itself).
- NEXUS keeps a **bitmap**, one bit per frame (free or used), to hand out
  and take back pages.

### 4.6 Virtual memory and paging
- Programs use **virtual addresses**. The CPU translates them to physical
  addresses through **page tables**, a 4-level tree on x86-64: PML4 → PDPT
  → PD → PT.
- Each page has **permission bits**:
  - present
  - writable
  - user-accessible
  - **NX** (no-execute)
- **Guard pages** (deliberately unmapped pages) turn bugs like stack
  overflows into immediate, reported faults.
- **Page faults** occur when a translation is missing or forbidden. The
  CPU reports the address (in the CR2 register) and the reason.
- **Address spaces:** each process can have its own page-table tree. This
  is how programs are kept isolated from each other.
- **HHDM** (higher-half direct map): all physical RAM is also visible at a
  fixed virtual offset, so the kernel can read and write any physical page.
- The **TLB** is the CPU's translation cache, which must be flushed when
  mappings change.

### 4.7 Kernel heap
- A general-purpose allocator (`kmalloc`/`kfree`) for kernel data
  structures.
- Built on top of paging: when it runs out of room it maps more physical
  pages.
- Uses **first-fit** allocation with block **splitting** and
  **coalescing** of neighbouring free blocks to limit fragmentation.

### 4.8 Processes and threads
- A **process** is an environment: an ID and an address space.
- A **thread** is an execution context: registers, a stack, and a state.
  A process can own several threads.
- **Thread states:** NEW → READY → RUNNING → (BLOCKED) → TERMINATED.

### 4.9 Kernel stacks
- Every thread has its **own** stack; sharing one would corrupt data
  instantly.
- NEXUS stacks are 16 KiB, with an unmapped guard region below them and a
  canary value at the bottom to detect overflows.

### 4.10 Context switching
- Switching threads means saving the running thread's CPU registers and
  loading another thread's.
- NEXUS saves exactly what the **SysV x86-64 calling convention**
  requires (`rbx, rbp, r12–r15`, the stack pointer and flags), because a
  switch is always a function call from the compiler's point of view.
- A brand-new thread starts through a **trampoline** that calls its entry
  function.

### 4.11 Scheduling and preemption
- The **scheduler** decides which READY thread runs next.
- **Round-robin:** threads take turns in FIFO order, each getting a fixed
  **time slice** (quantum), here 20 ms.
- **Preemption:** the timer interrupt takes the CPU away when a thread's
  slice ends, so no thread can starve the others.
- **Voluntary switching:** a thread can also **yield** or **sleep**.
- The **idle thread** runs only when nothing else can, and halts the CPU
  to save power.

### 4.12 Concurrency safety
- With preemption, a thread can be interrupted *anywhere*, including
  halfway through updating shared data such as the heap.
- On a single CPU, the simplest correct protection is to **briefly disable
  interrupts** around such updates (a critical section).
- NEXUS wraps this in one macro (`IRQ_GUARD()`), the single place to add
  real locks when multiple CPUs are supported.

### 4.13 Verification by self-tests
- Each phase includes tests that run **inside the kernel at boot**, not in
  a simulator.
- The tests use **deterministic** designs where possible (for example, only
  switching threads explicitly in Phase 4), plus assembly probes that check
  every CPU register.
- **Crash demos** deliberately trigger faults (null pointer, stack
  overflow, corrupted context) to prove error handling reports them
  correctly.

---

## 5. Tech stack

| Area | Technology | Why |
|---|---|---|
| Kernel language | **C (C11, freestanding)** | direct hardware control, no runtime needed |
| Low-level code | **x86-64 assembly (NASM)** | entry point, interrupt stubs, context switch, test probes |
| Compiler | **Clang / LLVM** (`x86_64-unknown-none-elf`) | cross-compiles from Windows, Linux or WSL |
| Linker | **LLD** + custom linker script | higher-half layout, page-aligned sections |
| Bootloader | **Limine 12.x** | modern boot protocol; supports BIOS and UEFI |
| Emulator | **QEMU** (`qemu-system-x86_64`) | fast testing; serial output to the terminal |
| UEFI firmware | **OVMF / EDK2** | tests the UEFI boot path |
| Build system | **Python 3** (`build.py`, `fat32.py`) | one cross-platform script; creates the bootable FAT32 disk image with no external tools |
| Test harness | **Python** (`test_boot.py`) | boots QEMU, watches serial output, reports PASS/FAIL |
| Version control | **Git / GitHub** | one commit per phase |

**Key compiler settings:** `-ffreestanding`, `-mcmodel=kernel`,
`-mno-red-zone`, and `-mno-sse -mno-mmx` (so no floating-point state has
to be saved on a context switch).

---

## 6. Repository layout

```text
NexusOS/
├── build.py            build / run / test / test-faults / clean
├── fat32.py            bootable disk image writer
├── test_boot.py        automated QEMU boot tests
├── boot/limine.conf    boot menu
├── scripts/            fetch_limine.py
├── docs/               design docs for each phase (this file included)
└── kernel/
    ├── entry.asm, kernel.c, linker.ld        boot and main
    ├── gdt, idt, interrupts, timer           Phase 2
    ├── pmm, vmm, heap, pagefault, mm         Phase 3
    ├── kstack, proc, context.asm             Phase 4
    ├── sched                                 Phase 5
    ├── *_test.c, *.asm probes                self-tests
    └── serial, kprintf, box, string, panic   support code
```

Detailed design documents:

- [memory-management.md](memory-management.md): Phase 3
- [process-model.md](process-model.md): Phase 4
- [scheduler.md](scheduler.md): Phase 5
- [interrupt-handling-implementation.md](interrupt-handling-implementation.md): Phase 2

---

## 7. How to run it

```bash
python scripts/fetch_limine.py   # one-time: download the bootloader
python build.py run              # build + boot in QEMU (BIOS)
python build.py run-uefi         # boot through UEFI firmware
python build.py test             # build + boot + verify all tests
python build.py test-faults      # verify all 8 crash demos
python build.py run --fault-demo pf   # boot, then crash on purpose
```

- **Quit QEMU:** press **Ctrl+A**, then **X**.
- **On Windows:** run `chcp 65001` first so the box-drawing characters
  display correctly.

---

## 8. Engineering highlights

These points are worth raising in a review or presentation:

- **Tests that prove behaviour, not just booting.** Examples:
  - **Scheduler fairness:** measured, not assumed.
  - **Preemption:** proven with threads that *never* give up the CPU.
  - **Register preservation:** checked for all 15 registers.
- **Real bugs found by the tests and fixed.** Examples:
  - a TSS layout error causing reboots
  - interrupt handlers returning to the wrong place
  - race conditions that only appear once threads can be preempted
- **Defensive design:**
  - guard pages and stack canaries
  - validation of every saved CPU context before it is loaded
  - registry checks that reject dangling pointers instead of following
    them
- **Honest limits:** each design document lists what is *not* handled
  yet.

---

## 9. Current limitations(Further project will cover this)

- Single CPU only; no SMP (multi-core) support yet.
- No user mode yet: everything runs in the kernel (ring 0). This is
  Phase 6.
- No filesystem, drivers beyond serial and timer, networking, or GUI.
- The scheduler is round-robin only, with no priorities.
- Floating-point/SSE state is not saved. This is fine while the kernel
  avoids SSE, but required for user programs.
- Mutual exclusion uses interrupt disabling, not real locks.

---

## 10. Next steps

**Phase 6: user mode + system calls**

- Run programs in **ring 3**, with their own memory and stacks.
- Add a **system call** interface (`exit`, `write`, `getpid`, `yield`,
  `sleep`) with strict checking of pointers passed in from user programs.
- A crashing user program is terminated, while the kernel keeps running.
- Prove that user programs cannot touch kernel memory or each other's
  memory.

**Phase 7: userspace runtime + `nsh` shell + real user programs**, the
first interactive NEXUS experience.
