from __future__ import annotations
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import List

REQUIRED = [
    "N E X U S   O S",
    "[idt] IDT initialized.",
    "[mm] Memory management online.",
    "failed =====",
    "[ok] Kernel started successfully.",
    "[ok] Phase 3 memory management verified.",
]

# Any of these means the boot went wrong, even if the markers above appear.
FORBIDDEN = [
    "[FAIL]",
    "KERNEL PANIC",
    "[exception]",
]

# Expected output of each fatal fault demo (build.py --fault-demo <name>).
FAULT_DEMO_EXPECT = {
    "pf":    ["[#PF]   access           : write", "memory-test window",
              "page not present", "Unhandled page fault - kernel halted"],
    "null":  ["[#PF]   faulting address : 0x0000000000000000 (null page)",
              "Unhandled page fault - kernel halted"],
    "stack": ["[exception] Double fault (#DF)"],
    "de":    ["[exception] Divide-by-zero (#DE)"],
    "ud":    ["[exception] Invalid opcode (#UD)"],
    "df":    ["[exception] Double fault (#DF)"],
}


def find_qemu() -> str:
    p = os.environ.get("NEXUS_QEMU")
    if p and Path(p).exists():
        return p
    return shutil.which("qemu-system-x86_64") or ""


def cmd_for(image: Path) -> List[str]:
    qemu = find_qemu()
    if not qemu:
        print("[ERROR] qemu-system-x86_64 not found", file=sys.stderr)
        return []
    return [qemu, "-m", "512M",
            "-drive", f"file={image},format=raw,media=disk,if=ide",
            "-nographic", "-no-reboot", "-no-shutdown"]


def boot_and_watch(image: Path, required: List[str], forbidden: List[str],
                   timeout: int) -> tuple[bool, str]:
    """Boot `image` until every `required` marker is seen (success), a
    `forbidden` marker appears, or `timeout` seconds pass."""
    cmd = cmd_for(image)
    if not cmd:
        return False, ""
    print(f"[TEST] {' '.join(cmd)}")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, bufsize=1, errors="replace")
    lines: List[str] = []
    start = time.time()
    success = False
    try:
        assert p.stdout is not None
        for line in p.stdout:
            line = line.rstrip()
            lines.append(line)
            print(f"  | {line}")
            if any(m in line for m in forbidden):
                # Keep reading briefly so the full report is captured.
                deadline = time.time() + 2
                for extra in p.stdout:
                    lines.append(extra.rstrip())
                    print(f"  | {extra.rstrip()}")
                    if time.time() > deadline or "Halting" in extra:
                        break
                break
            if all(m in "\n".join(lines) for m in required):
                success = True
                break
            if time.time() - start > timeout:
                break
    finally:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    return success, "\n".join(lines)


def run_boot_test(image: Path, timeout: int = 120) -> int:
    success, full = boot_and_watch(image, REQUIRED, FORBIDDEN, timeout)
    print("=" * 60)
    if success:
        print("[PASS] NEXUS OS booted successfully.")
        print("[PASS] Phase 3 memory-management tests passed.")
        return 0
    print("[FAIL] Boot did not complete cleanly.")
    for m in FORBIDDEN:
        if m in full:
            print(f"         - found {m!r}")
    for m in REQUIRED:
        if m not in full:
            print(f"         - missing {m!r}")
    return 1


def run_fault_demo_test(image: Path, demo: str, timeout: int = 120) -> int:
    expect = FAULT_DEMO_EXPECT[demo] + ["[ok] Phase 3 memory management verified."]
    success, full = boot_and_watch(image, expect, ["KERNEL PANIC", "[FAIL]"], timeout)
    if success:
        print(f"[PASS] fault demo '{demo}' reported as expected.")
        return 0
    print(f"[FAIL] fault demo '{demo}' missing:")
    for m in expect:
        if m not in full:
            print(f"         - {m!r}")
    return 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: test_boot.py <image>", file=sys.stderr)
        sys.exit(2)
    sys.exit(run_boot_test(Path(sys.argv[1])))
