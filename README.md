# NEXUS OS

A minimal, portable x86-64 hobbyist kernel that boots on real hardware and
in QEMU via the Limine v12.x boot protocol. The build system is pure Python
and works identically on **Windows**, **Linux**, and **WSL**.

```
============================================
            N E X U S   O S
        Phase 1: Kernel Foundation
============================================

[boot] CPU entered long mode (64-bit).
[boot] Bootloader: Limine 12.6.1
[boot] Kernel loaded: phys=0x... virt=0xffffffff80200000
[boot] COM1 serial initialised @ 115200 baud.
[boot] Higher-half direct map offset=0xffff800000000000

[ok] Kernel started successfully.
[ok] Phase 1 boot verified. Halting CPU.
```

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
python build.py test       # build + boot + verify startup banner
python build.py clean      # remove build artifacts
```

That's it. `python build.py test` exits `0` on success.

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
│   ├── panic.c/.h       # panic handler
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

## Cross-platform notes

- `build.py` resolves every tool via `.env` override → `PATH` lookup.
- `fat32.py` writes a portable MBR + FAT32 image with VFAT long names so
  Limine can find `limine-bios.sys`. No `mtools`/`mkfs.fat` needed.
- On Linux, `run-uefi` auto-detects OVMF at `/usr/share/OVMF/`.
- On Windows, set `NEXUS_OVMF_*` in `.env` for UEFI boot.
