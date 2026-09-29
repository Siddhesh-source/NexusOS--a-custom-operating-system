# NEXUS OS

A minimal, portable x86-64 hobbyist kernel that boots on real hardware and
in QEMU via the Limine v12.x boot protocol. The build system is pure Python
and works identically on **Windows**, **Linux**, and **WSL**.

```
============================================
            N E X U S   O S
       Phase 3: Memory Management
============================================

[boot] CPU entered long mode (64-bit).
[boot] Bootloader: Limine 12.6.1
...
[pmm] Total physical memory :   523768 KiB
[pmm] Usable memory         :   522636 KiB
[vmm]   .text   0xffffffff80200000 - 0xffffffff80207000 -> phys 0x...  R-X
[vmm]   HHDM    0xffff800000000000 - ...  RW-
[heap] Kernel heap at 0xffffc00000000000, 64 KiB mapped, limit 256 MiB
...
[test] ===== 114 passed, 0 failed =====

[ok] Kernel started successfully.
[ok] Phase 3 memory management verified.
```

Implemented so far:

- **Phase 1:** boots via Limine and brings up the COM1 serial console.
- **Phase 2:** GDT/TSS, IDT, CPU exception reporting, PIT timer.
- **Phase 3:** physical page-frame allocator, 4-level paging, virtual
  mapping API, kernel heap, page-fault reporting, and per-process address
  spaces. See [docs/memory-management.md](docs/memory-management.md).

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
python build.py test       # build + boot + run memory self-tests
python build.py test-faults  # verify every fatal-exception demo is reported
python build.py clean      # remove build artifacts

python build.py run --fault-demo pf   # boot, then trigger a fatal page fault
                                      # (also: null, stack, de, ud, df)
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
7. The memory self-tests run, and the kernel idles on `hlt`.

## Cross-platform notes

- `build.py` resolves every tool via `.env` override → `PATH` lookup.
- `fat32.py` writes a portable MBR + FAT32 image with VFAT long names so
  Limine can find `limine-bios.sys`. No `mtools`/`mkfs.fat` needed.
- On Linux, `run-uefi` auto-detects OVMF at `/usr/share/OVMF/`.
- On Windows, set `NEXUS_OVMF_*` in `.env` for UEFI boot.
