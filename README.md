<div align="center">

# NEXUS OS

**A from-scratch x86-64 operating system kernel, written in C and assembly.**

![Architecture](https://img.shields.io/badge/arch-x86--64-blue)
![Language](https://img.shields.io/badge/language-C%20%7C%20NASM-informational)
![Boot](https://img.shields.io/badge/boot-Limine%20(BIOS%20%2B%20UEFI)-orange)
![Tests](https://img.shields.io/badge/boot%20tests-223%20passing-brightgreen)
![Status](https://img.shields.io/badge/progress-Phase%205%20of%2010-yellow)

</div>

---

## Overview

NEXUS is a modular, 64-bit kernel built layer by layer: boot, interrupts,
memory, threads, then scheduling. Each layer ships with self-tests that run
**inside the kernel at every boot**, so every claim below is verified on
each run.

**Current state:** preemptive multitasking kernel (Phases 1–5 of 10).

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

Full boot log: [docs/BOOT_OUTPUT.md](docs/BOOT_OUTPUT.md)

## Features

| Area | What's implemented |
|---|---|
| **Boot** | Limine protocol, BIOS and UEFI, higher-half kernel, serial console |
| **CPU** | GDT/TSS with IST, IDT for all exceptions, detailed fault reports, PIT timer |
| **Memory** | Bitmap page-frame allocator, 4-level paging, W^X kernel sections, kernel heap, per-process address spaces |
| **Threads** | Processes and kernel threads, guarded kernel stacks, assembly context switch |
| **Scheduler** | Preemptive round-robin, 20 ms quantum, idle thread, sleep, live statistics |
| **Safety** | Guard pages, stack canaries, corrupted-context detection, preemption-safe allocators |

## Quick start

**Requirements:** Python 3.9+, Clang/LLD 14+, NASM, QEMU 7+ (plus OVMF for UEFI).

```bash
git clone https://github.com/Siddhesh-source/NexusOS--a-custom-operating-system.git
cd NexusOS--a-custom-operating-system
python scripts/fetch_limine.py     # one-time: download the bootloader
python build.py run                # build and boot in QEMU
```

Quit QEMU with **Ctrl+A**, then **X**.

## Commands

| Command | Description |
|---|---|
| `python build.py` | Build the kernel and bootable disk image |
| `python build.py run` | Boot in QEMU (BIOS), serial output in the terminal |
| `python build.py run-uefi` | Boot in QEMU through UEFI firmware (OVMF) |
| `python build.py test` | Boot and verify every self-test (exit code 0 = pass) |
| `python build.py test-faults` | Verify all 8 crash-handling demos |
| `python build.py run --fault-demo <name>` | Boot, then trigger a fault: `pf` `null` `stack` `de` `ud` `df` `ctx` `tstack` |
| `python build.py clean` | Remove build artifacts |

<details>
<summary><b>Windows setup</b></summary>

If the tools are not on `PATH`, create a `.env` file in the project root:

```ini
NEXUS_CLANG=C:\Program Files\LLVM\bin\clang.exe
NEXUS_LLD=C:\Program Files\LLVM\bin\ld.lld.exe
NEXUS_OBJCOPY=C:\Program Files\LLVM\bin\llvm-objcopy.exe
NEXUS_NASM=C:\Tools\NASM\nasm.exe
NEXUS_QEMU=C:\qemu\qemu-system-x86_64.exe
NEXUS_OVMF_CODE=C:\qemu\share\edk2-x86_64-code.fd
NEXUS_OVMF_VARS=C:\qemu\share\edk2-i386-vars.fd
```

Run `chcp 65001` first so the box-drawing characters in the output display
correctly.

</details>

<details>
<summary><b>Linux / WSL setup</b></summary>

```bash
sudo apt install python3 clang lld llvm nasm qemu-system-x86 ovmf
```

No `.env` is needed; tools and OVMF are detected automatically.

</details>

## Architecture

```text
┌──────────────────────────────────────────────────────────┐
│  Scheduler          round-robin · preemption · idle      │  Phase 5
├──────────────────────────────────────────────────────────┤
│  Processes/Threads  kernel stacks · context switch       │  Phase 4
├──────────────────────────────────────────────────────────┤
│  Memory             PMM · paging · heap · page faults    │  Phase 3
├──────────────────────────────────────────────────────────┤
│  CPU & Interrupts   GDT/TSS · IDT · exceptions · timer   │  Phase 2
├──────────────────────────────────────────────────────────┤
│  Boot               Limine · long mode · serial          │  Phase 1
└──────────────────────────────────────────────────────────┘
```

**Kernel virtual memory map**

| Address | Region |
|---|---|
| `0x0000000000000000` | User space (per process) |
| `0xffff800000000000` | Direct map of physical RAM |
| `0xffffc00000000000` | Kernel heap |
| `0xffffe00000000000` | Kernel thread stacks |
| `0xffffffff80200000` | Kernel image |

## Testing

Every boot runs **223 checks** across three suites, then a live multitasking
demo:

| Suite | Checks | Highlights |
|---|---|---|
| Memory | 114 | allocator exhaustion, page permissions, invalid-access faults, heap integrity |
| Processes | 83 | exact A/B switch trace, register preservation, 60 000-switch stress, stack isolation |
| Scheduler | 26 | preemption of non-yielding threads, all 15 registers preserved, fairness, idle |

All suites pass under BIOS and UEFI, with zero compiler warnings.

## Roadmap

- [x] **Phase 1:** bootable x86-64 kernel
- [x] **Phase 2:** interrupts, exceptions, timer
- [x] **Phase 3:** physical and virtual memory management
- [x] **Phase 4:** processes, threads, context switching
- [x] **Phase 5:** preemptive scheduler
- [ ] **Phase 6:** user mode (ring 3) and system calls
- [ ] **Phase 7:** userspace runtime and `nsh` shell
- [ ] **Phase 8+:** filesystem, IPC, networking, GUI

## Documentation

| Document | Contents |
|---|---|
| [Project overview](docs/PROJECT_OVERVIEW.md) | Problem statement, progress, concepts, tech stack |
| [Interrupts](docs/interrupt-handling-implementation.md) | GDT, IDT, exceptions, timer |
| [Memory management](docs/memory-management.md) | PMM, paging, heap, page faults |
| [Process model](docs/process-model.md) | Processes, threads, stacks, context switch |
| [Scheduler](docs/scheduler.md) | Round-robin, preemption, idle, statistics |
| [Boot output](docs/BOOT_OUTPUT.md) | Annotated full boot log |

## Project structure

```text
├── build.py          Build, run and test entry point
├── test_boot.py      Automated QEMU test harness
├── fat32.py          Bootable disk image writer (no external tools)
├── boot/             Limine configuration
├── scripts/          Bootloader download script
├── docs/             Design documents
└── kernel/
    ├── entry.asm, kernel.c, linker.ld     Boot and main
    ├── gdt, idt, interrupts, timer        CPU and interrupts
    ├── pmm, vmm, heap, pagefault          Memory management
    ├── kstack, proc, context.asm          Processes and threads
    ├── sched                              Scheduler
    └── *_test.c                           In-kernel self-tests
```

## Tech stack

**C11** (freestanding) · **NASM** · **Clang/LLD** · **Limine 12** ·
**QEMU** · **OVMF** · **Python 3** (build system and test harness)
