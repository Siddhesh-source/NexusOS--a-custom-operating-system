# NEXUS OS

A minimal, portable x86-64 hobbyist kernel that boots on real hardware and
in QEMU via the Limine v12.x boot protocol. The build system is pure Python
and works identically on **Windows**, **Linux**, and **WSL**.

```
============================================
            N E X U S   O S
     Phase 5: Preemptive Scheduler
============================================

[boot] CPU entered long mode (64-bit).
[boot] Bootloader: Limine 12.6.1
...
[pmm] Total physical memory :   523768 KiB
[vmm]   .text   0xffffffff80200000 - 0xffffffff80207000 -> phys 0x...  R-X
[heap] Kernel heap at 0xffffc00000000000, 64 KiB mapped, limit 256 MiB
[test] ===== 114 passed, 0 failed =====
[kstack] 256 slots of 32 KiB (16 KiB stack + guard) at 0xffffe00000000000
[proc] Kernel process PID 0 created; boot flow adopted as TID 0 ...
[A] counter = 1  (TID 4, rsp in own stack)
[B] counter = 101  (TID 5, rsp in own stack)
[A] counter = 2  (TID 4, rsp in own stack)
...
[test] ===== 83 passed, 0 failed =====
[sched] Round-robin scheduler: 100 Hz timer, quantum 2 ticks (20 ms), ...
[test]   CPU handed out in this order: A B C A B C A B C A B C ...
  Thread A :  26 time slices,  52 CPU ticks, ...
  Thread B :  26 time slices,  52 CPU ticks, ...
  Thread C :  25 time slices,  50 CPU ticks, ...
[test] ===== 26 passed, 0 failed =====

[ok] Kernel started successfully.
[ok] Phase 3 memory management verified.
[ok] Phase 4 process management verified.
[ok] Phase 5 scheduler verified.
```

Implemented so far:

- **Phase 1:** boots via Limine and brings up the COM1 serial console.
- **Phase 2:** GDT/TSS, IDT, CPU exception reporting, PIT timer.
- **Phase 3:** physical page-frame allocator, 4-level paging, virtual
  mapping API, kernel heap, page-fault reporting, and per-process address
  spaces. See [docs/memory-management.md](docs/memory-management.md).
- **Phase 4:** processes and kernel threads, a per-thread kernel stack
  with guard page and canary, x86-64 context switching, lifecycle and
  reclamation, and a process/thread registry with diagnostics. No
  scheduler yet. See [docs/process-model.md](docs/process-model.md).
- **Phase 5:** timer-driven preemptive round-robin scheduler (100 Hz PIT,
  20 ms quantum), ready queue, idle thread, tick-based sleep, preemption-safe
  allocators and console, and scheduler statistics with a live dashboard.
  See [docs/scheduler.md](docs/scheduler.md).

## Prerequisites

| Tool | Purpose | Windows | Linux / WSL |
|---|---|---|---|
| Python 3.9+ | build system | python.org | `sudo apt install python3` |
| Clang + LLD 14+ | C compiler + linker | [LLVM installer](https://github.com/llvm/llvm-project/releases) | `sudo apt install clang lld llvm` |
| NASM 2.14+ | x86 assembler | [nasm.us](https://www.nasm.us/) | `sudo apt install nasm` |
| QEMU 7+ | emulator | [qemu.org](https://www.qemu.org/download/) | `sudo apt install qemu-system-x86` |
| OVMF (UEFI only) | UEFI firmware | bundled with QEMU | `sudo apt install ovmf` |
| Limine 12.6.1 | bootloader | fetched by script | fetched by script |

## One-time setup

### 1. Get the source

```bash
git clone <your-repo-url> nexus
cd nexus
```

### 2. Fetch the Limine bootloader (both platforms)

```bash
python scripts/fetch_limine.py
```

This downloads the Limine binary release and (on Windows) the `.exe` host
tool into `tools/limine/`. No internet access is needed after this step.

### 3. Configure tool paths (optional)

The build auto-detects tools on your `PATH`. If a tool is not on `PATH`
(common on Windows), create a `.env` file in the project root:

```ini
# .env  -- Windows example
NEXUS_CLANG=C:\Program Files\LLVM\bin\clang.exe
NEXUS_LLD=C:\Program Files\LLVM\bin\ld.lld.exe
NEXUS_OBJCOPY=C:\Program Files\LLVM\bin\llvm-objcopy.exe
NEXUS_NASM=C:\Tools\NASM\nasm.exe
NEXUS_QEMU=D:\qemu\qemu-system-x86_64.exe
NEXUS_OVMF_CODE=D:\qemu\share\edk2-x86_64-code.fd
NEXUS_OVMF_VARS=D:\qemu\share\edk2-i386-vars.fd
```

On Linux/WSL you usually do **not** need a `.env` — everything is on `PATH`.

## Build & run

```bash
python build.py            # compile kernel + build bootable image
python build.py run        # boot in QEMU (BIOS, serial to stdout)
python build.py run-uefi   # boot in QEMU (UEFI via OVMF)
python build.py test       # build + boot + run memory and process self-tests
python build.py test-faults  # verify every fatal-exception demo is reported
python build.py clean      # remove build artifacts

python build.py run --fault-demo pf   # boot, then trigger a fatal page fault
                                      # (also: null, stack, de, ud, df,
                                      #  ctx = corrupted thread context,
                                      #  tstack = thread stack overflow)
```

`python build.py test` exits `0` only if every boot-time check passes.

## Project layout

```
nexus/
├── .env                 # optional tool paths (gitignored)
├── README.md
├── build.py             # build orchestrator (build/run/test/clean)
├── fat32.py             # FAT32 + MBR image writer (pure Python)
├── test_boot.py         # automated QEMU boot test
├── scripts/
│   └── fetch_limine.py  # download Limine v12.6.1
├── boot/
│   └── limine.conf      # Limine boot menu
├── kernel/
│   ├── entry.asm        # 64-bit entry stub
│   ├── kernel.c         # C entry point (Limine v12.x protocol)
│   ├── serial.c/.h      # COM1 UART driver
│   ├── kprintf.c/.h     # formatted serial output
│   ├── string.c/.h      # memset/memcpy/memmove/memcmp
│   ├── panic.c/.h       # panic handler
│   ├── cpu.h            # port I/O, control registers, MSRs, invlpg
│   ├── gdt.c/.h         # GDT + TSS (IST1 stack for #DF)
│   ├── idt.c/.h         # IDT setup
│   ├── interrupt_asm.asm  # exception/IRQ entry stubs
│   ├── interrupts.c/.h  # dispatcher + exception reports
│   ├── timer.c/.h       # PIC remap + PIT
│   ├── mm.c/.h          # memory layout, boot memory info, mm_init
│   ├── pmm.c/.h         # physical page-frame allocator (bitmap)
│   ├── vmm.c/.h         # page tables, mapping API, address spaces
│   ├── heap.c/.h        # kernel heap (kmalloc/kfree)
│   ├── pagefault.c/.h   # #PF reporting + probe fixups
│   ├── probe.asm        # fault-tolerant memory probes for tests
│   ├── mm_test.c/.h     # boot-time memory tests
│   ├── kstack.c/.h      # kernel thread stacks (guard page + canary)
│   ├── context.h/.asm   # CPU context layout + context_switch
│   ├── proc.c/.h        # processes, threads, registry, thread_switch
│   ├── proc_test.c/.h   # boot-time process/thread tests + summary panel
│   ├── context_test.asm # register-preservation probe for tests
│   ├── sched.c/.h       # preemptive round-robin scheduler, idle, sleep
│   ├── sched_test.c/.h  # scheduler tests, live dashboard, status panel
│   ├── sched_spin.asm   # all-GPR preemption probe for tests
│   ├── box.c/.h         # boxed diagnostic output
│   ├── test_exception.c # deliberate fatal exceptions (fault demos)
│   ├── kernel.h
│   ├── linker.ld        # higher-half ELF linker script
│   └── limine.h         # upstream Limine protocol header
└── tools/limine/        # fetched bootloader binaries (gitignored)
```

## How it boots

1. QEMU (SeaBIOS or OVMF) loads Limine from the FAT32 ESP.
2. Limine reads `limine.conf`, loads `nexus.elf`, and jumps to `_start`.
3. `entry.asm` zeroes BSS, sets up the stack, calls `kernel_main()`.
4. `kernel.c` initialises COM1 and prints boot diagnostics using the
   Limine response structures (bootloader info, executable address, HHDM).
5. GDT/TSS, IDT, and the timer come up and interrupts are enabled.
6. The Limine memory map is handed to `mm_init()`, which starts the PMM,
   builds and loads the kernel's own page tables, and creates the heap.
7. The memory self-tests run.
8. The stack allocator and process manager start, and the boot flow
   becomes thread 0 of the kernel process (PID 0).
9. The process/thread self-tests switch between real kernel threads.
10. The scheduler starts: the timer now preempts threads every 20 ms. The
    scheduler tests and the live multitasking demo run. Then kmain sleeps
    in a heartbeat loop, and the idle thread halts the CPU.

## Cross-platform notes

- `build.py` resolves every tool via `.env` override → `PATH` lookup.
- `fat32.py` writes a portable MBR + FAT32 image with VFAT long names so
  Limine can find `limine-bios.sys`. No `mtools`/`mkfs.fat` needed.
- On Linux, `run-uefi` auto-detects OVMF at `/usr/share/OVMF/`.
- On Windows, set `NEXUS_OVMF_*` in `.env` for UEFI boot.
