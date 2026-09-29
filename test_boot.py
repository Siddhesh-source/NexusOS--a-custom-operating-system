from __future__ import annotations
import os
import queue
import shutil
import subprocess
import sys
import threading
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
    "[ok] Phase 4 process management verified.",
    "[ok] Phase 5 scheduler verified.",
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
    "ctx":   ["FATAL context-switch error: saved instruction pointer lies outside kernel text",
              "RIP      : 0x00000000deadbeef", "KERNEL PANIC", "corrupted thread context"],
    # The overflowing push faults in the unmapped guard below the thread's
    # stack, somewhere in the kernel-stack region (0xffffe00000000000, 8 MiB;
    # the slot depends on how many threads exist, e.g. the idle thread).
    # #PF cannot be delivered on that stack, so the CPU raises #DF on IST1.
    "tstack": ["[demo] Thread TID", "[exception] Double fault (#DF)",
               "cr2=0xffffe00000"],
}

# Demos whose expected output legitimately contains a normally-forbidden marker.
FAULT_DEMO_ALLOW = {"ctx": ["KERNEL PANIC"]}


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
    # The kernel emits UTF-8 (box-drawing glyphs); don't let the platform
    # default codec (cp1252 on Windows) decode or re-encode it.
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, bufsize=1, encoding="utf-8", errors="replace")
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    # Read on a background thread so the deadlines below are always honoured,
    # even if the kernel hangs silently or only prints newline-less timer dots.
    q: "queue.Queue[str | None]" = queue.Queue()

    def reader() -> None:
        assert p.stdout is not None
        for raw in p.stdout:
            q.put(raw.rstrip())
        q.put(None)

    threading.Thread(target=reader, daemon=True).start()

    lines: List[str] = []
    deadline = time.time() + timeout
    success = False
    failed = False
    try:
        while time.time() < deadline:
            try:
                line = q.get(timeout=0.5)
            except queue.Empty:
                continue
            if line is None:
                break
            lines.append(line)
            print(f"  | {line}")
            if not failed and any(m in line for m in forbidden):
                # Keep collecting briefly so the full report is captured.
                failed = True
                deadline = min(deadline, time.time() + 2)
                continue
            if not failed and all(m in "\n".join(lines) for m in required):
                success = True
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
        print("[PASS] Phase 4 process/thread tests passed.")
        print("[PASS] Phase 5 scheduler tests passed.")
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
    expect = FAULT_DEMO_EXPECT[demo] + ["[ok] Phase 3 memory management verified.",
                                        "[ok] Phase 4 process management verified.",
                                        "[ok] Phase 5 scheduler verified."]
    forbidden = [m for m in ["KERNEL PANIC", "[FAIL]"]
                 if m not in FAULT_DEMO_ALLOW.get(demo, [])]
    success, full = boot_and_watch(image, expect, forbidden, timeout)
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
