<div align="center">

# BharatOS

**A from-scratch x86-64 operating system that checks itself, explains
itself, and is made for the people who use it.**

![Architecture](https://img.shields.io/badge/arch-x86--64-blue)
![Language](https://img.shields.io/badge/language-C%20%7C%20NASM-informational)
![Boot](https://img.shields.io/badge/boot-Limine%20(BIOS%20%2B%20UEFI)-orange)
![Tests](https://img.shields.io/badge/boot%20tests-223%20passing-brightgreen)
![Status](https://img.shields.io/badge/progress-Phase%205%20of%2012-yellow)

</div>

---

## Why BharatOS

Operating systems fail people in everyday ways: a power cut breaks a file,
an old laptop crawls, an app reads your photos without asking, and nothing
ever tells you *why*. BharatOS is built around fixing those problems with
simple, solid engineering.

| | What it means for you | Status |
|---|---|---|
| ✅ **Checks itself on every boot** | 223 health checks run inside the kernel each time it starts; problems are reported, never hidden | working today |
| 💬 **Explains itself** | `why slow`, `why crash`: plain sentences from real system data, offline | Phase 8 |
| ↩️ **Undo for your files** | saves never overwrite data, so power cuts can't corrupt files and every change can be undone | Phase 9 |
| 🔒 **Apps ask first** | apps start with no access; you allow or deny; every use is logged | Phase 10 |
| 🇮🇳 **Made for Bharat** | runs in 128 MB of RAM, works offline, speaks Hindi and Marathi | Phase 12 |

All of it comes together in a clean, simple desktop (Phase 11).
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
```

| Area | What's implemented |
|---|---|
| **Boot** | Limine (BIOS and UEFI), 64-bit kernel in high memory, serial console |
| **CPU** | crash handlers for all 32 CPU errors with full reports, emergency stack for double faults, 100 Hz timer |
| **Memory** | page allocator, own page tables, read-only code and no-execute data, kernel heap, one address space per process |
| **Threads** | processes and kernel threads, own guarded stack per thread, assembly context switch |
| **Scheduler** | preemptive time-sharing (20 ms slices), idle thread, sleep, live statistics |
| **Safety** | guard pages, stack canaries, corrupted-state detection, interrupt-safe allocators |

Full boot log: [docs/logs/boot-output.md](docs/logs/boot-output.md)

## Quick start

**Requirements:** Python 3.9+, Clang/LLD 14+, NASM, QEMU 7+ (plus OVMF for UEFI).

```bash
git clone https://github.com/Siddhesh-source/NexusOS--a-custom-operating-system.git
cd NexusOS--a-custom-operating-system
python scripts/fetch_limine.py     # one-time: download the bootloader
python build.py run                # build and boot in QEMU
```

Quit QEMU with **Ctrl+A**, then **X**.

| Command | What it does |
|---|---|
| `python build.py` | Build the kernel and bootable disk image |
| `python build.py run` | Boot in QEMU (BIOS), output in the terminal |
| `python build.py run-uefi` | Boot through UEFI firmware (OVMF) |
| `python build.py test` | Boot and verify every self-test (exit code 0 = pass) |
| `python build.py test-faults` | Verify all 8 crash-on-purpose demos are caught and reported |
| `python build.py run --fault-demo <name>` | Boot, then crash on purpose: `pf` `null` `stack` `de` `ud` `df` `ctx` `tstack` |
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
│  Desktop & apps: Home · Terminal · Files · Why? · Settings     │  Ph 11
├────────────────────────────────────────────────────────────────┤
│  Explain · Files with undo · Permissions · Programs & shell    │  Ph 7–10
├══════════════  system-call wall (user ↔ kernel)  ══════════════┤  Ph 6
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

Every boot runs **223 checks** inside the kernel, then a live multitasking demo:

| Suite | Checks | Highlights |
|---|---|---|
| Memory | 114 | allocator exhaustion, page permissions, invalid-access faults, heap integrity |
| Processes | 83 | exact switch trace, register preservation, 60 000-switch stress, stack isolation |
| Scheduler | 26 | preemption of non-yielding threads, all 15 registers preserved, fairness, idle |

All suites pass under BIOS and UEFI with zero compiler warnings.

## Roadmap

- [x] **1–5:** boot, interrupts, memory, threads, preemptive scheduler
- [ ] **6:** apps run walled off: user mode and system calls
- [ ] **7:** programs, keyboard and the `bsh` shell
- [ ] **8:** explain-it layer (`why`) and visible self-check (`health`)
- [ ] **9:** files with undo, safe from power cuts (BharatFS)
- [ ] **10:** apps ask first (permissions)
- [ ] **11:** desktop: a clean visual interface
- [ ] **12:** made for Bharat: Hindi/Marathi, 128 MB, real hardware

Step-by-step plan with tests for each phase: [docs/ROADMAP.md](docs/ROADMAP.md)

## Project structure

```text
├── build.py              build, run and test entry point
├── CLAUDE.md             guide for contributors and AI agents
├── PROMPT.md             ready-made prompts for AI-assisted development
├── boot/                 Limine configuration
├── scripts/              disk image writer, QEMU test harness, bootloader download
├── kernel/
│   ├── arch/x86_64/      entry, linker script, GDT/TSS, IDT, interrupts, timer, context switch
│   ├── core/             kernel_main, panic
│   ├── mm/               page allocator, page tables, heap, page faults
│   ├── proc/             processes, threads, kernel stacks
│   ├── sched/            scheduler
│   ├── drivers/          serial (keyboard, disk, mouse, screen to come)
│   ├── lib/              kprintf, string, box drawing
│   ├── include/          limine.h
│   └── tests/            boot-time self-tests
├── user/                 user programs and apps (Phase 6+)
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
| [Roadmap](docs/ROADMAP.md) | Phases 6–12, step by step, with tests and demos |
| [High-level design](docs/design/HLD.md) | layers, components, main flows, key decisions |
| [Low-level design](docs/design/LLD.md) | structs, constants, system calls, disk format |
| [Desktop design](docs/design/UI.md) | layout, colours, screens |
| Phase docs: [2](docs/phases/phase-2-interrupts.md) · [3](docs/phases/phase-3-memory.md) · [4](docs/phases/phase-4-processes.md) · [5](docs/phases/phase-5-scheduler.md) | how each finished layer works |

## Tech stack

**C11** (freestanding) · **NASM** · **Clang/LLD** · **Limine** ·
**QEMU** · **OVMF** · **Python 3** (build system and test harness)
