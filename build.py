#!/usr/bin/env python3
from __future__ import annotations
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def load_env(path: Path) -> None:
    if not path.exists():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, _, v = line.partition("=")
        v = v.strip().strip('"').strip("'")
        os.environ.setdefault(k.strip(), v)


load_env(ROOT / ".env")

KERNEL_DIR = ROOT / "kernel"
BOOT_DIR = ROOT / "boot"
BUILD_DIR = ROOT / "build"
TOOLS_DIR = ROOT / "tools"
LIMINE_DIR = TOOLS_DIR / "limine" / "limine-binary"

KERNEL_ELF = BUILD_DIR / "nexus.elf"
KERNEL_IMG = BUILD_DIR / "nexus.img"

IS_WIN = os.name == "nt"


class BuildError(Exception):
    pass


def info(m: str) -> None:
    print(f"  $ {m}")


def ok(m: str) -> None:
    print(f"[ OK ] {m}")


def run(cmd: list[str], *, check: bool = True) -> int:
    info(" ".join(str(c) for c in cmd))
    p = subprocess.run(cmd)
    if check and p.returncode != 0:
        raise BuildError(f"failed (exit {p.returncode}): {' '.join(cmd)}")
    return p.returncode


def resolve_tool(env_key: str, candidates: list[str]) -> str:
    p = os.environ.get(env_key)
    if p and Path(p).exists():
        return p
    for name in candidates:
        found = shutil.which(name)
        if found:
            return found
    raise BuildError(f"tool not found: set {env_key} in .env or install one of: {', '.join(candidates)}")


def find_limine() -> Path:
    candidates = []
    if IS_WIN:
        candidates.append(LIMINE_DIR / "limine-tool-windows-x86" / "limine.exe")
        candidates.append(LIMINE_DIR / "limine.exe")
    else:
        candidates.append(LIMINE_DIR / "limine")
        candidates.append(LIMINE_DIR / "limine-tool-linux-x86-64" / "limine")
        candidates.append(TOOLS_DIR / "limine" / "limine-tool-linux-x86-64" / "limine")
    for c in candidates:
        if c.exists():
            return c
    raise BuildError(f"limine not found under {LIMINE_DIR}; run: python scripts/fetch_limine.py")


TOOLS: dict[str, str] = {}


def configure() -> None:
    TOOLS["clang"] = resolve_tool("NEXUS_CLANG", ["clang", "clang-18", "clang-17"])
    TOOLS["ld"] = resolve_tool("NEXUS_LLD", ["ld.lld", "lld", "ld.lld-18"])
    TOOLS["objcopy"] = resolve_tool("NEXUS_OBJCOPY", ["llvm-objcopy", "objcopy"])
    TOOLS["nasm"] = resolve_tool("NEXUS_NASM", ["nasm"])
    TOOLS["qemu"] = resolve_tool("NEXUS_QEMU", ["qemu-system-x86_64"])
    TOOLS["limine"] = str(find_limine())


CFLAGS = [
    "-target", "x86_64-unknown-none-elf",
    "-ffreestanding", "-fno-stack-protector", "-fno-pie", "-fno-pic",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-mcmodel=kernel", "-m64", "-march=x86-64",
    "-Wall", "-Wextra", "-O2", "-g", "-std=gnu11",
    f"-I{KERNEL_DIR}",
]

LDFLAGS = [
    "-nostdlib", "-static",
    "-z", "max-page-size=0x1000", "-z", "noexecstack", "-z", "nocopyreloc",
    "-m", "elf_x86_64",
    "-T", str(KERNEL_DIR / "linker.ld"),
    "--image-base=0xffffffff80200000",
]

C_SOURCES = [
    "kernel.c", "serial.c", "panic.c", "kprintf.c", "string.c", "box.c",
    # Phase 2: descriptor tables, exceptions, timer
    "gdt.c", "idt.c", "interrupts.c", "timer.c", "test_exception.c",
    # Phase 3: memory management
    "mm.c", "pmm.c", "vmm.c", "heap.c", "pagefault.c", "mm_test.c",
    # Phase 4: processes, threads, context switching
    "kstack.c", "proc.c", "proc_test.c",
    # Phase 5: preemptive scheduler
    "sched.c", "sched_test.c",
]
ASM_SOURCES = ["interrupt_asm.asm", "probe.asm", "context.asm", "context_test.asm",
               "sched_spin.asm"]

# Opt-in fatal exception demos (see run_fault_demo() in kernel.c).
FAULT_DEMOS = ["pf", "null", "stack", "de", "ud", "df", "ctx", "tstack"]


def build_kernel(fault_demo: str | None = None) -> Path:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    entry_o = BUILD_DIR / "entry.o"
    run([TOOLS["nasm"], "-f", "elf64", "-F", "dwarf", "-g",
         str(KERNEL_DIR / "entry.asm"), "-o", str(entry_o)])
    objs = [entry_o]
    defines = [f"-DNEXUS_FAULT_DEMO_{fault_demo.upper()}"] if fault_demo else []
    for src in C_SOURCES:
        obj = BUILD_DIR / (Path(src).stem + ".o")
        run([TOOLS["clang"], *CFLAGS, *defines, "-c", str(KERNEL_DIR / src), "-o", str(obj)])
        objs.append(obj)
    for src in ASM_SOURCES:
        obj = BUILD_DIR / (Path(src).stem + ".o")
        run([TOOLS["nasm"], "-f", "elf64", "-F", "dwarf", "-g",
             str(KERNEL_DIR / src), "-o", str(obj)])
        objs.append(obj)
    run([TOOLS["ld"], *LDFLAGS, *[str(o) for o in objs], "-o", str(KERNEL_ELF)])
    ok(f"Built {KERNEL_ELF}")
    return KERNEL_ELF


def build_image(kernel_elf: Path) -> Path:
    from fat32 import build_nexus_image
    build_nexus_image(
        image_path=KERNEL_IMG,
        kernel_elf=kernel_elf,
        limine_dir=LIMINE_DIR,
        limine_exe=Path(TOOLS["limine"]),
        boot_dir=BOOT_DIR,
        size_mb=512,
    )
    ok(f"Built {KERNEL_IMG}")
    return KERNEL_IMG


def find_ovmf() -> tuple[Path | None, Path | None]:
    code = os.environ.get("NEXUS_OVMF_CODE")
    vars_t = os.environ.get("NEXUS_OVMF_VARS")
    code_p = Path(code) if code and Path(code).exists() else None
    vars_p = Path(vars_t) if vars_t and Path(vars_t).exists() else None
    if not IS_WIN:
        for c in [Path("/usr/share/OVMF/OVMF_CODE_4M.fd"),
                  Path("/usr/share/OVMF/OVMF_CODE.fd"),
                  Path("/usr/share/edk2/OVMF/OVMF_CODE_4M.fd")]:
            if c.exists():
                code_p = code_p or c
        for v in [Path("/usr/share/OVMF/OVMF_VARS_4M.fd"),
                  Path("/usr/share/OVMF/OVMF_VARS.fd"),
                  Path("/usr/share/edk2/OVMF/OVMF_VARS_4M.fd")]:
            if v.exists():
                vars_p = vars_p or v
    return code_p, vars_p


def run_qemu(image: Path, *, uefi: bool = False) -> int:
    cmd = [TOOLS["qemu"], "-m", "512M",
           "-drive", f"file={image},format=raw,media=disk,if=ide",
           "-no-reboot", "-no-shutdown"]
    if not uefi:
        cmd += ["-nographic"]
    else:
        code, vars_t = find_ovmf()
        if code and vars_t:
            vars_copy = BUILD_DIR / "OVMF_VARS.fd"
            shutil.copy(vars_t, vars_copy)
            cmd += ["-serial", "stdio", "-display", "none",
                    "-drive", f"if=pflash,format=raw,unit=0,file={code},readonly=on",
                    "-drive", f"if=pflash,format=raw,unit=1,file={vars_copy}"]
        else:
            print("[WARN] no OVMF found; falling back to BIOS")
            cmd += ["-nographic"]
    print(f"[qemu] {'UEFI' if uefi else 'BIOS'}")
    return run(cmd, check=False)


def run_test(image: Path) -> int:
    from test_boot import run_boot_test
    return run_boot_test(image)


def run_fault_tests() -> int:
    """Build and boot every fatal fault demo, checking each is reported."""
    from test_boot import run_fault_demo_test
    failures = []
    for demo in FAULT_DEMOS:
        print(f"\n===== fault demo: {demo} =====")
        image = build_image(build_kernel(fault_demo=demo))
        if run_fault_demo_test(image, demo) != 0:
            failures.append(demo)
    build_image(build_kernel())   # leave a normal image behind
    print("=" * 60)
    if failures:
        print(f"[FAIL] fault demos failed: {', '.join(failures)}")
        return 1
    print(f"[PASS] all {len(FAULT_DEMOS)} fault demos reported correctly.")
    return 0


def clean() -> None:
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    ok("Cleaned.")


def main() -> int:
    ap = argparse.ArgumentParser(prog="build.py",
                                 description="NEXUS OS build system")
    ap.add_argument("command",
                    choices=["build", "image", "run", "run-uefi", "test",
                             "test-faults", "clean", "help"],
                    nargs="?", default="build")
    ap.add_argument("--fault-demo", choices=FAULT_DEMOS,
                    help="build a kernel that deliberately triggers this fatal "
                         "exception after the memory tests")
    args = ap.parse_args()

    if args.command == "help":
        ap.print_help()
        return 0
    if args.command == "clean":
        clean()
        return 0

    try:
        configure()
        if args.command == "test-faults":
            return run_fault_tests()
        if args.command in ("build", "image", "run", "run-uefi", "test"):
            kernel = build_kernel(fault_demo=args.fault_demo)
            image = build_image(kernel)
        if args.command == "run":
            return run_qemu(image, uefi=False)
        if args.command == "run-uefi":
            return run_qemu(image, uefi=True)
        if args.command == "test":
            return run_test(image)
        return 0
    except BuildError as e:
        print(f"\n[ERROR] {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\n[ABORT] interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
