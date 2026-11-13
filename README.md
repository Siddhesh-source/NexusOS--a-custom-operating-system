<div align="center">

# BharatOS

**A from-scratch x86-64 operating system that checks itself, explains
itself, and is made for the people who use it.**

![Architecture](https://img.shields.io/badge/arch-x86--64-blue)
![Language](https://img.shields.io/badge/language-C%20%7C%20NASM-informational)
![Boot](https://img.shields.io/badge/boot-Limine%20(BIOS%20%2B%20UEFI)-orange)
![Tests](https://img.shields.io/badge/boot%20tests-566%20passing-brightgreen)
![Status](https://img.shields.io/badge/progress-Phase%2011%20of%2023-yellow)

</div>

---

## Why BharatOS

Operating systems fail people in everyday ways: a power cut breaks a file,
an old laptop crawls, an app reads your photos without asking, and nothing
ever tells you *why*. BharatOS is built around fixing those problems with
simple, solid engineering.

| | What it means for you | Status |
|---|---|---|
| ✅ **Checks itself on every boot** | 566 health checks run inside the kernel each time it starts; problems are reported, never hidden, and shown on the desktop | working today |
| 💬 **Explains itself** | `why slow`, `why crash`: plain sentences from real system data, offline | Phase 15 |
| ↩️ **Undo for your files** | saves never overwrite data, so power cuts can't corrupt files and every change can be undone | Phase 17 |
| 🔒 **Apps ask first** | apps start with no access; you allow or deny; every use is logged | Phase 18 |
| 🇮🇳 **Made for Bharat** | runs in 128 MB of RAM, works offline, speaks Hindi and Marathi | Phase 22 |

A first desktop works today (V1: windows, Terminal, Files, Monitor, Health); the full one comes in Phase 21.
More detail: [Project overview](docs/PROJECT_OVERVIEW.md).

## What works today

```text
[sched] Round-robin scheduler: 100 Hz timer, quantum 2 ticks (20 ms)
[test]   CPU handed out in this order: A B C A B C A B C A B C ...
  Thread A :  26 time slices,  52 CPU ticks
  Thread B :  26 time slices,  52 CPU ticks
  Thread C :  25 time slices,  50 CPU ticks
[test] ===== 26 passed, 0 failed =====

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
[ok] Phase 11 desktop verified.
[desktop] Login screen ready (type your name in the QEMU window).
[desktop] Desktop ready for asha.

BharatOS login: asha
Hello, asha! Type 'help' to see what you can do.
asha@bharat:~$ echo hi > a.txt
asha@bharat:~$ cat a.txt
hi
```

| Area | What's implemented |
|---|---|
| **Boot** | Limine (BIOS and UEFI), 64-bit kernel in high memory, serial console, a splash screen whose progress bar follows the real boot steps |
| **Hardware** | ACPI tables (CPUs, interrupt controllers, HPET, PCIe), PCI device scan, local and I/O APIC |
| **CPU** | crash handlers for all 32 CPU errors with full reports, emergency stack for double faults, calibrated clock with one-shot alarms |
| **Memory** | buddy page allocator, slab caches, kernel heap, own page tables (read-only code, no-execute data), one address space per process, pages allocated on first touch, copy-on-write, shareable memory objects |
| **Multi-core** | all CPUs started, per-CPU data and run queues, work stealing, named spinlocks with a lock-order checker, TLB shootdown, RCU lookups |
| **Threads** | processes and kernel threads, own guarded stack per thread, assembly context switch |
| **Scheduler** | preemptive time-sharing (20 ms slices), tickless (an idle CPU gets no timer interrupts), idle thread, sleep, live statistics |
| **Safety** | crashes of a demo thread are reported and contained (the system keeps running), guard pages, stack canaries, compiler stack protector, heap poisoning and red zones (use-after-free and overflow caught), corrupted-state detection, interrupt-safe allocators |
| **Input** | PS/2 keyboard and mouse for the desktop, typing in a terminal (serial) for the text console, both at once; threads sleep until a key arrives |
| **Files** | files and folders in memory, filled at boot from a bundle on the disk; size limits checked |
| **Terminal** | login with your name, a shell with 24 commands (`ls cat echo > cd mkdir rm cpus top ps kill checks lockdemo crashdemo` ...), history with the arrow keys |
| **Desktop** | graphical login with your name; windows you can focus, drag and close; a top bar with your apps, CPU count, health badge and clock; drawn in memory and copied to the screen only where it changed |
| **Apps** | Terminal (the shell in a window), Files (open, create, delete), Text editor, Monitor (every CPU's load live, work stealing), Health (checks, lock-order and crash demos with their reports), About; a scripted tour (F12) |
| **Debugging** | crash reports with named call traces and the last 16 kernel events, GDB/LLDB attach mode, host unit tests, benchmarks, size and boot-time budgets, CI |

Full boot log: [docs/logs/boot-output.md](docs/logs/boot-output.md)

## Quick start

**Requirements:** Python 3.9+, Clang/LLD 14+, NASM, QEMU 7+ (plus OVMF for UEFI).

```bash
git clone https://github.com/Siddhesh-source/NexusOS--a-custom-operating-system.git
cd NexusOS--a-custom-operating-system
python scripts/fetch_limine.py     # one-time: download the bootloader
python build.py demo               # build and boot with the desktop in a QEMU window
```

Type your name in the window and press Enter. Click into the window to
use the mouse (Ctrl+Alt+G lets it go); F12 starts a tour of the desktop.

Quit QEMU with **Ctrl+A**, then **X**.

| Command | What it does |
|---|---|
| `python build.py` | Build the kernel and bootable disk image |
| `python build.py demo` | Boot with the desktop in a QEMU window and the text console in the terminal; `--auto` logs in and runs the tour by itself |
| `python build.py run` | Boot in QEMU (BIOS) in the terminal; log in and type commands there |
| `python build.py run --window` | Same, plus a QEMU window showing the screen (type in either) |
| `python build.py run-uefi` | Boot through UEFI firmware (OVMF) |
| `python build.py test` | Boot and verify every self-test and the budgets (exit code 0 = pass); `--smp N` sets the CPU count (default 4) |
| `python build.py test-faults` | Verify all 13 crash-on-purpose demos are caught and reported |
| `python build.py run --fault-demo <name>` | Boot, then crash on purpose: `pf` `null` `stack` `de` `ud` `df` `ctx` `tstack` `panic` `smash` `uaf` `overflow` `lockorder` |
| `python build.py unit` | Run heap and scheduler unit tests on this machine (millions of cases, seconds) |
| `python build.py bench` | Measure switch, heap and trace costs; add a row to [docs/benchmarks.md](docs/benchmarks.md) |
| `python build.py debug` | Start QEMU paused for a debugger; then `gdb -x scripts/bharat.gdb` |
| `python build.py clean` | Remove build output |

<details>
<summary><b>Windows setup</b></summary>

If the tools are not on `PATH`, copy `.env.example` to `.env` and set:

```ini
BHARAT_CLANG=C:\Program Files\LLVM\bin\clang.exe
BHARAT_LLD=C:\Program Files\LLVM\bin\ld.lld.exe
BHARAT_OBJCOPY=C:\Program Files\LLVM\bin\llvm-objcopy.exe
BHARAT_NASM=C:\Tools\NASM\nasm.exe
BHARAT_QEMU=C:\qemu\qemu-system-x86_64.exe
BHARAT_OVMF_CODE=C:\qemu\share\edk2-x86_64-code.fd
BHARAT_OVMF_VARS=C:\qemu\share\edk2-i386-vars.fd
```

Run `chcp 65001` first so the box-drawing characters display correctly.

</details>

<details>
<summary><b>Linux / WSL setup</b></summary>

```bash
sudo apt install python3 clang lld llvm nasm qemu-system-x86 ovmf
```

No `.env` needed; tools and OVMF are found automatically.

</details>

## Architecture

```text
┌────────────────────────────────────────────────────────────────┐
│  Desktop & apps: Home · Terminal · Files · Why? · Settings     │  Ph 18
├────────────────────────────────────────────────────────────────┤
│  Services: shell · explain · files with undo · permissions     │  Ph 11–15
├══════════  async rings + handles (user ↔ kernel)  ═════════════┤  Ph 10
│  Debug & hardening  call traces · canaries · heap poisoning    │  ✅ 6
│  Scheduler          round-robin · preemption · idle            │  ✅ 5
│  Processes/Threads  kernel stacks · context switch             │  ✅ 4
│  Memory             pages · page tables · heap · page faults   │  ✅ 3
│  CPU & Interrupts   GDT/TSS · IDT · exceptions · timer         │  ✅ 2
│  Boot               Limine · 64-bit mode · serial              │  ✅ 1
└────────────────────────────────────────────────────────────────┘
```

Design details: [high-level design](docs/design/HLD.md) ·
[low-level design](docs/design/LLD.md) · [desktop look](docs/design/UI.md)

## Testing

Every boot runs **566 checks** inside the kernel, then logs in and types commands at the real prompt, logs in to the desktop with the emulated keyboard, runs the quick desktop tour (which checks its own results) and checks a screenshot:

| Suite | Checks | Highlights |
|---|---|---|
| Memory | 114 | allocator exhaustion, page permissions, invalid-access faults, heap integrity |
| Processes | 83 | exact switch trace, register preservation, 60 000-switch stress, stack isolation |
| Scheduler | 26 | preemption of non-yielding threads, all 15 registers preserved, fairness, idle |
| Debug & hardening | 29 | symbol lookup, call-trace walk, random canary, use-after-free and overflow detection, event trace |
| Hardware | 38 | ACPI tables, PCI list, screen text, PIC masked, sleep accuracy, tickless idle, I/O APIC routing |
| Memory that scales | 62 | buddy split/merge, slab limits and poisoning, demand paging, copy-on-write, shared objects |
| Multi-core | 29 | lost-update stress on all CPUs, lock-order inversion caught, TLB shootdown, RCU, work stealing |
| Input | 32 | key, terminal and mouse decoding, full queue, keys through the real IRQ 1 / IRQ 12 path, sleeping readers |
| Files | 46 | paths, create/read/write/list/delete, bad paths, every size limit, the boot bundle |
| Login and shell | 58 | line editor, login rules, scripted commands with checked output, contained crashes |
| Desktop | 50 | drawing and clipping, damage-only redraws, windows (focus, drag, close, limits), login screen, every app driven by keys and clicks |

Outside the kernel, `python build.py unit` runs about **2.8 million** checks
on the real heap and scheduler code (random operations against a model),
plus 1.26 million on the buddy allocator and slab caches,
and `test-faults` crashes the kernel on purpose 13 ways to check every
report. CI builds and boots every pull request.

All suites pass under BIOS and UEFI with zero compiler warnings.

## Roadmap

- [x] **1–5:** boot, interrupts, memory, threads, preemptive scheduler
- [x] **6:** debug and hardening: call traces, stack protector, heap poisoning, unit tests, benchmarks, CI
- [x] **7:** modern hardware: ACPI, APIC, PCI, tickless timer, screen console
- [x] **8:** memory that scales: buddy, slab, demand paging, copy-on-write
- [x] **9:** multi-core: per-CPU queues, spinlocks with lock-order checking, TLB shootdown, RCU
- [x] **10:** V1 core: keyboard and mouse, files in RAM, login with your name, terminal
- [x] **11:** V1 desktop and demo: windows, top bar, Terminal, Files, editor, Monitor, Health, tour
- [ ] **12:** V1 hardening: thorough tests, fuzzing, screenshots, real hardware
- [ ] **13–14:** user mode with handles and async rings; microkernel IPC and services
- [ ] **15:** self-check and explain (`why`)
- [ ] **16–17:** drivers as services; BharatFS with undo, safe from power cuts
- [ ] **18–19:** permissions and verified boot; networking
- [ ] **20–21:** resume, replay and live migration; desktop polish and WebAssembly apps
- [ ] **22–23:** made for Bharat (Hindi/Marathi, 128 MB, real hardware); extras

What's next and why: [WAY_AHEAD.md](WAY_AHEAD.md).

The current plan and its reasons: [docs/UPDATES.md](docs/UPDATES.md).

Step-by-step plan with tests for each phase: [docs/ROADMAP.md](docs/ROADMAP.md)

## Project structure

```text
├── build.py              build, run and test entry point
├── CLAUDE.md             guide for contributors and AI agents
├── PROMPT.md             ready-made prompts for AI-assisted development
├── boot/                 Limine configuration; bundle/ = files loaded at boot
├── .github/workflows/     CI: build, unit tests, one boot test
├── scripts/              disk image writer, QEMU test harness, symbol table, budgets, GDB script
├── kernel/
│   ├── arch/x86_64/      entry, linker script, GDT/TSS, IDT, interrupts, APIC, clock, context switch
│   ├── acpi/             ACPI tables (MADT, HPET, MCFG)
│   ├── pci/              PCI device scan
│   ├── core/             kernel_main, panic, contained crashes
│   ├── mm/               buddy allocator, slab caches, page tables, heap, memory objects, page faults
│   ├── proc/             processes, threads, kernel stacks
│   ├── sched/            scheduler (per-CPU run queues), wait queues
│   ├── smp/              CPU start-up, per-CPU data, spinlocks, IPIs, RCU
│   ├── drivers/          serial, screen console, real-time clock
│   ├── input/            keyboard, mouse and serial input, event queue
│   ├── fs/               files in memory (ramfs) and the boot bundle
│   ├── apps/             V1 apps: login console, shell, and the desktop apps
│   ├── ui/               graphics, boot splash, window manager, desktop, tour
│   ├── lib/              kprintf, string, box drawing
│   ├── trace/            call traces (symbol table) and the event trace
│   ├── include/          limine.h
│   └── tests/            boot-time self-tests and benchmarks
├── tests/unit/           host-side unit tests (heap, scheduler, buddy, slab)
├── user/                 user programs and apps (Phase 10+)
└── docs/
    ├── PROJECT_OVERVIEW.md, ROADMAP.md
    ├── design/           HLD, LLD, UI
    ├── phases/           one document per finished phase
    └── logs/             boot output captures
```

## Documentation

| Document | Contents |
|---|---|
| [Project overview](docs/PROJECT_OVERVIEW.md) | the problems, the 5 USPs, talking points |
| [Plan updates](docs/UPDATES.md) | **current** architecture, Phases 6–20, decision log (wins over older docs) |
| [Roadmap](docs/ROADMAP.md) | the original Phase 6–12 plan, step by step (see UPDATES §14 for the mapping) |
| [High-level design](docs/design/HLD.md) | layers, components, main flows, key decisions |
| [Low-level design](docs/design/LLD.md) | structs, constants, system calls, disk format |
| [Desktop design](docs/design/UI.md) | layout, colours, screens |
| Phase docs: [2](docs/phases/phase-2-interrupts.md) · [3](docs/phases/phase-3-memory.md) · [4](docs/phases/phase-4-processes.md) · [5](docs/phases/phase-5-scheduler.md) · [6](docs/phases/phase-6-debug-hardening.md) · [7](docs/phases/phase-7-modern-hardware.md) · [8](docs/phases/phase-8-memory-that-scales.md) · [9](docs/phases/phase-9-multi-core.md) · [10](docs/phases/phase-10-v1-core.md) | how each finished layer works |
| [Benchmarks](docs/benchmarks.md) · [Bug journal](docs/BUG_JOURNAL.md) | measured costs over time; hard bugs and how they were found |

## Tech stack

**C11** (freestanding) · **NASM** · **Clang/LLD** · **Limine** ·
**QEMU** · **OVMF** · **Python 3** (build system and test harness)
