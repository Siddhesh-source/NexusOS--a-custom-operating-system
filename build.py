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
SCRIPTS_DIR = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))   # fat32.py, test_boot.py
LIMINE_DIR = TOOLS_DIR / "limine" / "limine-binary"

KERNEL_ELF = BUILD_DIR / "bharat.elf"
KERNEL_IMG = BUILD_DIR / "bharat.img"

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
    TOOLS["clang"] = resolve_tool("BHARAT_CLANG", ["clang", "clang-18", "clang-17"])
    TOOLS["ld"] = resolve_tool("BHARAT_LLD", ["ld.lld", "lld", "ld.lld-18"])
    TOOLS["objcopy"] = resolve_tool("BHARAT_OBJCOPY", ["llvm-objcopy", "objcopy"])
    TOOLS["nasm"] = resolve_tool("BHARAT_NASM", ["nasm"])
    TOOLS["qemu"] = resolve_tool("BHARAT_QEMU", ["qemu-system-x86_64"])
    TOOLS["limine"] = str(find_limine())


ARCH_DIR = KERNEL_DIR / "arch" / "x86_64"
ENTRY_ASM = ARCH_DIR / "entry.asm"      # linked first
LINKER_SCRIPT = ARCH_DIR / "linker.ld"

# Every kernel folder that holds headers is on the include path, so code
# keeps writing #include "pmm.h" no matter which folder pmm.h lives in.
INCLUDE_DIRS = sorted({h.parent for h in KERNEL_DIR.rglob("*.h")})

CFLAGS = [
    "-target", "x86_64-unknown-none-elf",
    "-ffreestanding", "-fno-pie", "-fno-pic",
    # Frame pointers in every function (leaf ones too) so panics can walk
    # the stack and print a call trace.
    "-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer",
    # Canary check in every function with a local array or address-taken
    # local; the canary is the global __stack_chk_guard (stack_protector.c).
    "-fstack-protector-strong", "-mstack-protector-guard=global",
    "-mno-red-zone", "-mno-mmx", "-mno-sse", "-mno-sse2",
    "-mcmodel=kernel", "-m64", "-march=x86-64",
    "-Wall", "-Wextra", "-O2", "-g", "-std=gnu11",
    *[f"-I{d}" for d in INCLUDE_DIRS],
]

LDFLAGS = [
    "-nostdlib", "-static",
    "-z", "max-page-size=0x1000", "-z", "noexecstack", "-z", "nocopyreloc",
    "-m", "elf_x86_64",
    "-T", str(LINKER_SCRIPT),
    "--image-base=0xffffffff80200000",
]

# Sources are found automatically: drop a .c or .asm file into any folder
# under kernel/ and it is built. Sorted so builds are repeatable.
C_SOURCES = sorted(KERNEL_DIR.rglob("*.c"))
ASM_SOURCES = sorted(a for a in KERNEL_DIR.rglob("*.asm") if a != ENTRY_ASM)


def object_for(src: Path) -> Path:
    """build/<folder path with _>_<name>.o, so equal names never collide."""
    rel = src.relative_to(KERNEL_DIR).with_suffix("")
    return BUILD_DIR / ("_".join(rel.parts) + ".o")


# Opt-in fatal exception demos (see run_fault_demo() in kernel.c).
FAULT_DEMOS = ["pf", "null", "stack", "de", "ud", "df", "ctx", "tstack",
               "panic", "smash", "uaf", "overflow", "lockorder"]

# CPUs QEMU gives the kernel; `--smp N` changes it for run/test/debug/bench.
DEFAULT_SMP = 4
SMP = DEFAULT_SMP


def build_kernel(fault_demo: str | None = None, bench: bool = False,
                 demo_auto: bool = False) -> Path:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    nasm = [TOOLS["nasm"], "-f", "elf64", "-F", "dwarf", "-g"]
    entry_o = object_for(ENTRY_ASM)
    run([*nasm, str(ENTRY_ASM), "-o", str(entry_o)])
    objs = [entry_o]
    defines = [f"-DBHARAT_FAULT_DEMO_{fault_demo.upper()}"] if fault_demo else []
    if bench:
        defines.append("-DBHARAT_BENCH")
    if demo_auto:
        defines.append("-DBHARAT_DEMO_AUTO")    # log in and run the tour by itself
    for src in C_SOURCES:
        obj = object_for(src)
        run([TOOLS["clang"], *CFLAGS, *defines, "-c", str(src), "-o", str(obj)])
        objs.append(obj)
    for src in ASM_SOURCES:
        obj = object_for(src)
        run([*nasm, str(src), "-o", str(obj)])
        objs.append(obj)
    link_with_symbols(objs, defines)
    ok(f"Built {KERNEL_ELF}")
    return KERNEL_ELF


def link_with_symbols(objs: list[Path], defines: list[str]) -> None:
    """Link twice: first with an empty symbol table to learn every
    function's address, then with the real table (scripts/ksyms.py). The
    table is read-only data, placed after all code, so the second link must
    not move any function; that is checked rather than assumed."""
    from ksyms import read_functions, write_table
    table_c = BUILD_DIR / "ksyms_table.c"
    table_o = BUILD_DIR / "ksyms_table.o"

    def link(funcs: list[tuple[int, int, str]]) -> None:
        write_table(funcs, table_c)
        run([TOOLS["clang"], *CFLAGS, *defines, "-c", str(table_c), "-o", str(table_o)])
        run([TOOLS["ld"], *LDFLAGS, *[str(o) for o in objs], str(table_o),
             "-o", str(KERNEL_ELF)])

    link([])
    funcs = read_functions(KERNEL_ELF)
    link(funcs)
    if read_functions(KERNEL_ELF) != funcs:
        raise BuildError("adding the symbol table moved kernel code; "
                         "check the section order in linker.ld")
    ok(f"Symbol table: {len(funcs)} functions")


def build_image(kernel_elf: Path) -> Path:
    from fat32 import build_bharat_image
    build_bharat_image(
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
    code = os.environ.get("BHARAT_OVMF_CODE")
    vars_t = os.environ.get("BHARAT_OVMF_VARS")
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


def run_qemu(image: Path, *, uefi: bool = False, window: bool = False) -> int:
    cmd = [TOOLS["qemu"], "-m", "512M", "-smp", str(SMP),
           "-drive", f"file={image},format=raw,media=disk,if=ide",
           "-no-reboot", "-no-shutdown"]
    if window and not uefi:
        # A window with the screen (keys typed there arrive as a PS/2
        # keyboard), and the serial console still in this terminal.
        cmd += ["-serial", "mon:stdio"]
        print("[qemu] The desktop is in the QEMU window: type your name there. Click in the "
              "window to use the mouse (Ctrl+Alt+G lets it go). F12 there starts a tour.")
        print("[qemu] The text console is here. Quit: close the window, or Ctrl+A then X here.")
    elif not uefi:
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
    print(f"[qemu] {'UEFI' if uefi else 'BIOS'}, {SMP} CPU(s)")
    return run(cmd, check=False)


def run_debug(image: Path) -> int:
    """Start QEMU paused, waiting for a debugger on TCP port 1234."""
    gdb_script = SCRIPTS_DIR / "bharat.gdb"
    print("[debug] QEMU is paused and waiting for a debugger on localhost:1234.")
    print("[debug] In a second terminal, from the repo folder, run one of:")
    print(f"          gdb -x {gdb_script.relative_to(ROOT).as_posix()}")
    print(f"          lldb {KERNEL_ELF.relative_to(ROOT).as_posix()} "
          "-o \"gdb-remote 1234\" -o \"breakpoint set -n kernel_main\" -o continue")
    print("[debug] Serial output appears here. Quit QEMU: Ctrl+A then X.", flush=True)
    cmd = [TOOLS["qemu"], "-m", "512M", "-smp", str(SMP),
           "-drive", f"file={image},format=raw,media=disk,if=ide",
           "-nographic", "-no-reboot", "-no-shutdown",
           "-s", "-S"]          # -s: gdb server on :1234, -S: start paused
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


# ---- benchmarks -----------------------------------------------------------

BENCH_FILE = ROOT / "docs" / "benchmarks.md"
BENCH_NAMES = ["context_switch", "kmalloc_kfree_64", "kmalloc_kfree_4096", "trace_event",
               "pmm_page", "kmalloc_kfree_176", "slab_176"]
# Added later: recorded after boot_s so older rows keep their columns.
BENCH_LATER = ["context_switch_par"]
BENCH_HEADER = """# Benchmarks

Recorded by `python build.py bench`, which boots a kernel built with
`BHARAT_BENCH` and appends one row per run. Numbers are CPU time stamp
counter (TSC) cycles per operation, best of 5 rounds, under QEMU without
hardware acceleration (TCG). Use them to compare BharatOS with itself over
time; they are not real-hardware numbers. Heap numbers include the heap
poisoning and red-zone checks.

- **context_switch**: one thread switch through `sched_yield`
  (scheduler decision plus register save and restore).
- **kmalloc_kfree_64 / _4096**: one allocation and free of that size.
- **trace_event**: recording one event in the trace ring.
- **pmm_page**: one physical page allocated and freed (batches of 256).
- **kmalloc_kfree_176 / slab_176**: a thread-record-sized object from the
  general heap versus from a slab cache.
- **context_switch_par**: the same switch measured on every CPU at once
  (two threads pinned per CPU), averaged over CPUs. Close to
  context_switch means switching scales per CPU. Bench runs give QEMU one
  host thread per guest CPU (`thread=multi`), so the CPUs really run in
  parallel; on a host with fewer free cores the number grows.
- **cpus**: CPUs the bench kernel ran on (`--smp`).
- **boot_s**: seconds from starting QEMU to the bench results (includes
  all boot self-tests).

""" + "| Date | Commit | Host | " + " | ".join(BENCH_NAMES) + " | boot_s | " \
    + " | ".join(BENCH_LATER) + " | cpus |\n" + "|---" * (len(BENCH_NAMES) + len(BENCH_LATER) + 5) + "|\n"


def run_bench() -> int:
    import datetime
    import platform
    import re
    import test_boot
    from test_boot import boot_and_watch, FORBIDDEN
    image = build_image(build_kernel(bench=True))
    test_boot.QEMU_THREADS = "multi"     # guest CPUs really in parallel
    success, out, seconds = boot_and_watch(image, ["[bench] done"], FORBIDDEN, 300)
    test_boot.QEMU_THREADS = "single"
    build_image(build_kernel())   # leave a normal image behind
    results = dict(re.findall(r"\[bench\] (\w+): (\d+) cycles/op", out))
    missing = [n for n in BENCH_NAMES + BENCH_LATER if n not in results]
    if not success or missing:
        print(f"[FAIL] benchmark run incomplete; missing: {', '.join(missing) or 'done marker'}")
        return 1
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                            capture_output=True, text=True).stdout.strip() or "?"
    row = (f"| {datetime.date.today().isoformat()} | {commit} | {platform.system()} | "
           + " | ".join(results[n] for n in BENCH_NAMES) + f" | {seconds:.1f} | "
           + " | ".join(results[n] for n in BENCH_LATER) + f" | {SMP} |\n")
    text = BENCH_FILE.read_text(encoding="utf-8") if BENCH_FILE.exists() else BENCH_HEADER
    BENCH_FILE.write_text(text + row, encoding="utf-8", newline="\n")
    print("=" * 60)
    for n in BENCH_NAMES + BENCH_LATER:
        print(f"[bench] {n:20s} {results[n]:>8} cycles/op")
    print(f"[PASS] results added to {BENCH_FILE.relative_to(ROOT).as_posix()}")
    return 0


# ---- host unit tests -------------------------------------------------------

UNIT_DIR = ROOT / "tests" / "unit"
UNIT_BUILD = BUILD_DIR / "unit"

# Each program: the kernel files under test (the shared support files are
# added to every program).
UNIT_PROGRAMS = {
    "heap": [KERNEL_DIR / "mm" / "heap.c", KERNEL_DIR / "trace" / "trace.c",
             UNIT_DIR / "fake_pmm.c"],
    "mm": [KERNEL_DIR / "mm" / "pmm.c", KERNEL_DIR / "mm" / "slab.c"],
    "sched": [KERNEL_DIR / "sched" / "sched.c", KERNEL_DIR / "trace" / "trace.c"],
}
UNIT_COMMON = [UNIT_DIR / "fakes.c", KERNEL_DIR / "lib" / "kprintf.c",
               KERNEL_DIR / "lib" / "string.c"]


def run_unit_tests() -> int:
    """Build kernel code as small freestanding programs for this machine
    and run them. No C library is used, so the same clang + lld that build
    the kernel are enough on Windows and Linux (see tests/unit/unit.h)."""
    clang = resolve_tool("BHARAT_CLANG", ["clang", "clang-18", "clang-17"])
    UNIT_BUILD.mkdir(parents=True, exist_ok=True)
    cflags = ["-ffreestanding", "-nostdlibinc", "-fno-stack-protector",
              "-O2", "-g", "-std=gnu11", "-Wall", "-Wextra",
              "-iquote", str(UNIT_DIR / "fakes"), "-iquote", str(UNIT_DIR),
              "-include", str(UNIT_DIR / "fakes" / "unit_config.h"),
              *[f"-I{d}" for d in INCLUDE_DIRS]]
    if IS_WIN:
        dlltool = resolve_tool("BHARAT_DLLTOOL", [
            str(Path(clang).parent / "llvm-dlltool.exe"), "llvm-dlltool"])
        implib = UNIT_BUILD / "kernel32.lib"
        run([dlltool, "-m", "i386:x86-64", "-d", str(UNIT_DIR / "kernel32.def"),
             "-l", str(implib)])
        # Windows `long` is 32-bit, so the kernel's %lu with uint64_t warns
        # here; kprintf reads 64 bits for %l, so the output is still right.
        cflags = ["-target", "x86_64-pc-windows-msvc", "-mno-stack-arg-probe",
                  *cflags, "-Wno-format"]
        host = UNIT_DIR / "host_windows.c"
        ldflags = ["-fuse-ld=lld", "-nostdlib", "-Wl,/entry:unit_start",
                   "-Wl,/subsystem:console", "-Wl,/nodefaultlib", str(implib)]
        exe = ".exe"
    elif sys.platform.startswith("linux"):
        cflags = ["-target", "x86_64-unknown-linux-gnu", "-fno-pie", *cflags]
        host = UNIT_DIR / "host_linux.c"
        ldflags = ["-fuse-ld=lld", "-nostdlib", "-static", "-no-pie"]
        exe = ""
    else:
        raise BuildError(f"unit tests support Windows and Linux hosts, not {sys.platform}")

    failures = []
    for name, sources in UNIT_PROGRAMS.items():
        out = UNIT_BUILD / f"{name}_unit{exe}"
        srcs = [UNIT_DIR / f"{name}_unit.c", *sources, *UNIT_COMMON, host]
        run([clang, *cflags, *[str(s) for s in srcs], *ldflags, "-o", str(out)])
        print(f"[unit] running {out.name}", flush=True)
        if subprocess.run([str(out)]).returncode != 0:
            failures.append(name)
    print("=" * 60)
    if failures:
        print(f"[FAIL] unit tests failed: {', '.join(failures)}")
        return 1
    print(f"[PASS] all {len(UNIT_PROGRAMS)} unit test programs passed.")
    return 0


def clean() -> None:
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    ok("Cleaned.")


def main() -> int:
    ap = argparse.ArgumentParser(prog="build.py",
                                 description="BharatOS build system")
    ap.add_argument("command",
                    choices=["build", "image", "run", "run-uefi", "demo", "debug", "test",
                             "test-faults", "unit", "bench", "clean", "help"],
                    nargs="?", default="build")
    ap.add_argument("--fault-demo", choices=FAULT_DEMOS,
                    help="build a kernel that deliberately triggers this fatal "
                         "exception after the memory tests")
    ap.add_argument("--window", action="store_true",
                    help="run: also open a QEMU window showing the screen")
    ap.add_argument("--auto", action="store_true",
                    help="demo: log in as 'demo' and run the desktop tour by itself")
    ap.add_argument("--smp", type=int, default=DEFAULT_SMP, metavar="N",
                    help=f"number of CPUs for QEMU (1-16, default {DEFAULT_SMP})")
    args = ap.parse_args()
    if not 1 <= args.smp <= 16:
        ap.error("--smp must be between 1 and 16")
    global SMP
    SMP = args.smp
    import test_boot
    test_boot.QEMU_SMP = SMP

    if args.command == "help":
        ap.print_help()
        return 0
    if args.command == "clean":
        clean()
        return 0

    try:
        if args.command == "unit":      # needs only clang and lld
            return run_unit_tests()
        configure()
        if args.command == "test-faults":
            return run_fault_tests()
        if args.command == "bench":
            return run_bench()
        if args.command in ("build", "image", "run", "run-uefi", "debug", "test"):
            kernel = build_kernel(fault_demo=args.fault_demo)
            image = build_image(kernel)
        if args.command == "demo":
            image = build_image(build_kernel(demo_auto=args.auto))
            return run_qemu(image, uefi=False, window=True)
        if args.command == "run":
            return run_qemu(image, uefi=False, window=args.window)
        if args.command == "run-uefi":
            return run_qemu(image, uefi=True)
        if args.command == "debug":
            return run_debug(image)
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
