# BharatOS: Project Overview

**A complete x86-64 operating system built from scratch (kernel,
multi-core scheduler, memory manager, drivers, TCP/IP stack, window
system and apps) that checks itself, explains itself, and is made for
the people who use it.**

| | |
|---|---|
| **Status** | Phases 1–11 done, plus two rounds of extras (network, apps, Hindi/Marathi, Rust). Next: Phase 12 (hardening), then user mode |
| **Size** | ~21,700 lines of kernel C11 and NASM, ~6,300 lines of in-kernel tests, a Rust service |
| **Runs on** | x86-64 PCs, BIOS and UEFI (Limine), 1 to 16 CPUs; developed and tested in QEMU |
| **Proof** | 707 self-checks inside the kernel on every boot; ~4 million host unit-test cases; 13 crash-on-purpose kernels; an end-to-end test that logs in, types commands, fetches a web page over its own TCP and drives the desktop |
| **Records** | 44 design decisions with rejected options ([UPDATES.md §10](UPDATES.md)); 23 hard bugs written up ([BUG_JOURNAL.md](BUG_JOURNAL.md)); benchmarks ([benchmarks.md](benchmarks.md)) |

---

## 1. The idea

Most people never think about their operating system until it hurts them:
a power cut corrupts a file, a cheap laptop crawls, an app quietly reads
their photos, or the screen freezes and no one can say why. BharatOS
answers those problems with simple, solid engineering, and proves every
part of itself on every boot.

It is also a study in building every layer yourself: there is no Linux,
no C library and no borrowed kernel code underneath. The only outside
pieces are the bootloader (Limine), fonts rendered at build time, and the
emulator used for testing.

## 2. What is built

| Phase | What works | The interesting part |
|---|---|---|
| 1. Boot | Limine hand-off into a higher-half 64-bit kernel, serial console | the kernel sees only what it maps itself |
| 2. CPU and interrupts | GDT/TSS, IDT, all 32 exceptions with full reports, IST emergency stacks | a kernel stack overflow is still reported (double fault on its own stack) |
| 3. Memory | physical page tracking, own 4-level page tables with W^X, kernel heap, an address space per process | the kernel's own mappings are built from scratch, not inherited |
| 4. Processes and threads | processes and kernel threads, guarded stacks, assembly context switch | corrupted thread state is detected before it is switched to |
| 5. Scheduler | preemptive round-robin, idle thread, sleep, statistics | fairness and register preservation proven under preemption |
| 6. Debug and hardening | symbolised call traces, event trace ring, stack protector, heap poisoning and red zones, GDB attach, host unit tests, benchmarks, CI | traces without DWARF: a second link embeds a symbol table and the build checks no code moved |
| 7. Modern hardware | ACPI, PCI, local and I/O APIC, HPET, TSC calibration, tickless timer, screen console | idle CPUs take no interrupts; alarms re-armed only when a deadline moves earlier |
| 8. Memory that scales | buddy allocator, slab caches, demand paging, copy-on-write, shared memory objects | per-page reference counts make COW and sharing one mechanism |
| 9. Multi-core | every CPU started, per-CPU queues, work stealing, named spinlocks with a lock-order checker, IPIs, TLB shootdown, RCU | inversions caught before they deadlock; RCU grace periods from one IPI round |
| 10. V1 core | PS/2 and serial input, in-memory files from a boot bundle, login, a shell | one input path for real keyboards and terminals; contained crashes |
| 11. V1 desktop | splash, graphical login, window manager, compositor, Terminal, Files, Monitor, Health, editor, About, a self-checking tour | damage-only redraws and anti-aliasing in integer maths (no floating point in the kernel) |
| Extras | e1000 driver and a TCP/IP stack (DHCP, ARP, IPv4, ICMP, UDP, DNS, TCP, HTTP); Browser, Player, Code; `why`; Hindi and Marathi login; a Rust service | the boot test fetches a real page over its own TCP; Devanagari shaped at build time |

Details of every phase, with tests and known limits: [phases/](phases/).

## 3. The five things that make BharatOS different

| USP | The problem | The design | Status |
|---|---|---|---|
| **1. Checks itself** | systems fail silently | the kernel's own checks run on every boot; results shown on the desktop and in `checks` | ✅ 707 checks every boot, live health badge |
| **2. Explains itself** | "not responding" helps no one | rules over real kernel data, offline, no AI (D15) | ✅ `why slow`, `why crash`; full service in Phase 15 |
| **3. Undo for your files** | power cuts and mistakes destroy work | copy-on-write file system: write new, then switch one pointer (D9) | Phase 17 (BharatFS) |
| **4. Apps ask first** | apps read data without asking | handles (capabilities) checked at one boundary; rights only shrink (D3) | Phase 13/18; crashes already contained today |
| **5. Made for Bharat** | expensive hardware, English only | 128 MB target, offline, Devanagari | ✅ Hindi and Marathi login; rest in Phase 22 |

## 4. Architecture

Today the kernel holds everything, with the apps as kernel threads behind
narrow APIs (`fs_*`, `input_*`, `ui_*`, `net_*`), so a later move is
mechanical (D28). The target is a **microkernel** (D1): drivers, the file
system and the network become restartable user-mode services; programs
talk to the kernel through **async system-call rings** (D2) and hold
**handles** instead of global names (D3). The first Rust service already
uses the request/answer shape those rings will have (D43).

How the pieces fit: [design/HLD.md](design/HLD.md). Exact structures:
[design/LLD.md](design/LLD.md). The plan, phase by phase:
[UPDATES.md](UPDATES.md).

## 5. How it is proven

- **On every boot:** 707 checks in 12 suites run inside the real kernel:
  allocator exhaustion and integrity, exact context-switch traces, a
  60,000-switch stress, preemption fairness, lock-order inversion caught,
  TLB shootdown, RCU, work stealing, demand paging and copy-on-write,
  input through the real interrupt paths, every file-system limit, the
  shell's commands, every desktop app driven by keys and clicks, the
  network stack and the Rust service.
- **End to end:** the boot test types a login and commands over serial,
  fetches a page served by the host over BharatOS's TCP, logs in on the
  graphical desktop with emulated keys, runs a self-checking tour, and
  checks a screenshot, the boot time and the size budget.
- **On the host:** ~4 million randomised cases of the real heap,
  scheduler, buddy and slab code against reference models, built with the
  kernel's own compiler and no C library (D17).
- **Failure paths:** 13 kernels crash on purpose (null pointer, stack
  overflow, double fault, corrupted context, stack smash, use-after-free,
  heap overflow, lock-order inversion, ...) and each report is checked.
- **Budgets:** kernel size, boot time and zero compiler warnings, checked
  on every test run and in CI.

## 6. Talking points

**Systems depth**
- "The scheduler is tickless and per-CPU, with work stealing. The
  run-queue lock is handed across the context switch, so no CPU can steal
  a thread whose registers are still being saved (D23)."
- "Every spinlock has a class. A lock-order checker learns the order
  classes are taken in and reports an inversion with both call traces the
  first time it happens, not when it finally deadlocks."
- "RCU readers run with interrupts off, so one IPI answered by every CPU
  is a grace period. TLB shootdowns can be sent while holding locks
  because waiters keep answering requests (D24, D25)."
- "Copy-on-write, demand paging and shared memory all rest on per-page
  reference counts in the buddy allocator."
- "A crash in an app thread is contained by rewriting where the interrupt
  returns, so the thread exits after a full report. The kernel refuses
  when that would be unsafe (lock held, double fault) (D30)."
- "The TCP/IP stack, from the e1000 descriptor rings to HTTP, is our own,
  and every test run proves it by fetching a page."

**Engineering discipline**
- "Every phase ends with boot tests, a demo and a write-up; every big
  choice is logged with the options we rejected; every hard bug has a
  guarding test. For example, going tickless first doubled the cost of a
  thread switch (bug 10), and RCU grace periods once took a third of a
  second (bug 16)."
- "Bad paths are tested as hard as good ones: 13 kernels crash on purpose
  and the reports are checked."
- "Limits are tests: kernel size and boot time fail the build when they
  grow past their budgets."

**Product thinking**
- "We started from people's problems (power cuts, 'why is it slow', apps
  snooping, cheap hardware), not from a feature list."
- "One rule, *write new data then switch one pointer*, will give both
  power-cut safety and undo."
- "`why` is a table of rules over data the kernel already collects:
  explainable, offline, no AI."

## 7. Honest limits today

- Everything runs in kernel mode; user mode, handles and rings are
  Phase 13. App crashes are contained, but an app is still kernel code.
- Files live in RAM (no disk driver yet); BharatFS is Phase 17.
- The network is polled (no interrupts), IPv4 only, no HTTPS yet.
- Tested in QEMU; a real-laptop boot is part of Phase 12.

## 8. Tech stack

| Area | Choice | Why |
|---|---|---|
| Kernel | C11, freestanding, no libc | direct hardware control, no runtime |
| Low level | NASM | entry, interrupt stubs, context switch, CPU start-up |
| Services | Rust `no_std` (optional) | safety where new code is most likely to have bugs (D4) |
| Toolchain | Clang + LLD | the same compiler on Windows, Linux and WSL |
| Bootloader | Limine | BIOS and UEFI with one setup |
| Test bed | QEMU (+ OVMF) | scriptable: serial, emulated keys, screenshots |
| Build and tests | Python 3 | one cross-platform script that also writes the disk image |
