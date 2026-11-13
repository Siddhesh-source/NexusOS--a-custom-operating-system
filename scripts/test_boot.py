from __future__ import annotations
import codecs
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
    "B H A R A T   O S",
    "[idt] IDT initialized.",
    "[mm] Memory management online.",
    "failed =====",
    "[ok] Kernel started successfully.",
    "[ok] Phase 3 memory management verified.",
    "[ok] Phase 4 process management verified.",
    "[ok] Phase 5 scheduler verified.",
    "[ok] Phase 6 debug and hardening verified.",
    "[ok] Phase 7 modern hardware verified.",
    "[ok] Phase 8 memory that scales verified.",
    "[ok] Phase 9 multi-core verified.",
    "[ok] Phase 10 input verified.",
    "[ok] Phase 10 files in memory verified.",
    "[ok] Phase 10 login and shell verified.",
]

# CPUs QEMU gives the kernel (build.py --smp sets this).
QEMU_SMP = 4
# "single": one host thread runs every guest CPU in turn, so timing checks
# do not depend on how busy the host's cores are. "multi" (benchmarks, or
# a manual stress run): one host thread per guest CPU, truly in parallel.
QEMU_THREADS = "single"

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
    # A panic prints a call trace with function names, innermost first,
    # then the latest events from the trace ring.
    "panic": ["PANIC: forced panic to show a call trace", "Call trace",
              "demo_panic_inner+0x", "debug_demo_panic+0x", "kernel_main+0x",
              "Recent events"],
    # The overflow overwrites only the canary; the check catches it before
    # the function returns, and the trace names the guilty function.
    "smash": ["PANIC: stack smashing detected", "Call trace",
              "stack_smash_victim+0x", "debug_demo_stack_smash+0x"],
    # Heap poisoning: the next allocation of the damaged memory panics;
    # kfree checks the red zone after a block.
    "uaf": ["freed memory at", "PANIC: heap: use after free detected",
            "kmalloc", "debug_demo_use_after_free+0x"],
    "overflow": ["allocation", "of 20 bytes", "PANIC: heap: overflow detected",
                 "kfree+0x", "debug_demo_heap_overflow+0x"],
    # Two locks taken in opposite orders: the lock-order checker names
    # both classes and prints the earlier order's call trace and today's.
    "lockorder": ["[lockcheck] Lock order problem",
                  'taking "test A" while holding "test B"',
                  "Earlier order", "take_a_then_b+0x", "take_b_then_a+0x",
                  "PANIC: lock order problem"],
}

# Demos whose expected output legitimately contains a normally-forbidden marker.
FAULT_DEMO_ALLOW = {"ctx": ["KERNEL PANIC"], "panic": ["KERNEL PANIC"],
                    "smash": ["KERNEL PANIC"], "uaf": ["KERNEL PANIC"],
                    "overflow": ["KERNEL PANIC"], "lockorder": ["KERNEL PANIC"]}


def find_qemu() -> str:
    p = os.environ.get("BHARAT_QEMU")
    if p and Path(p).exists():
        return p
    return shutil.which("qemu-system-x86_64") or ""


def cmd_for(image: Path) -> List[str]:
    qemu = find_qemu()
    if not qemu:
        print("[ERROR] qemu-system-x86_64 not found", file=sys.stderr)
        return []
    return [qemu, "-m", "512M", "-smp", str(QEMU_SMP), "-accel", f"tcg,thread={QEMU_THREADS}",
            "-drive", f"file={image},format=raw,media=disk,if=ide",
            "-nographic", "-no-reboot", "-no-shutdown"]


# After the boot checks pass, a short session typed at the real login
# prompt: (text to wait for, keys to send). Each wait looks only at output
# that came after the previous step, so a prompt seen earlier does not
# count again. Serial input proves typed keys reach the shell through
# IRQ 4; "\x01c" switches QEMU's console to its monitor, where `sendkey`
# presses keys on the emulated PS/2 keyboard (IRQ 1), then back.
PROMPT = "tester@bharat:~$ "
SESSION = [
    ("BharatOS login: ", "tester\n"),
    (PROMPT, "echo typed at the prompt > t.txt\n"),
    (PROMPT, "cat t.txt\n"),
    ("typed at the prompt", None),
    # The monitor echoes its own commands around the typed letters, so
    # check for a phrase only the command's output contains.
    (PROMPT, "\x01c" + "".join(f"sendkey {k}\n" for k in "about") + "sendkey ret\n" + "\x01c"),
    ("Self-checks:", None),
    (PROMPT, "checks\n"),
    ("All good", None),
    (PROMPT, None),
]


def boot_and_watch(image: Path, required: List[str], forbidden: List[str],
                   timeout: int, session: List[tuple] | None = None) -> tuple[bool, str, float]:
    """Boot `image` until every `required` marker is seen, a `forbidden`
    marker appears, or `timeout` seconds pass. Then, if a `session` is
    given, type it (see SESSION); success needs every step. Also returns
    the seconds from starting QEMU until the required markers were seen."""
    cmd = cmd_for(image)
    if not cmd:
        return False, "", 0.0
    print(f"[TEST] {' '.join(cmd)}")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         stdin=subprocess.PIPE)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    # Read raw chunks on a background thread so the deadlines below are
    # always honoured, and so prompts without a newline are seen. The
    # kernel emits UTF-8 (box-drawing glyphs); decode it as such, not with
    # the platform default codec (cp1252 on Windows).
    q: "queue.Queue[str | None]" = queue.Queue()

    def reader() -> None:
        assert p.stdout is not None
        dec = codecs.getincrementaldecoder("utf-8")(errors="replace")
        fd = p.stdout.fileno()
        while True:
            chunk = os.read(fd, 4096)
            if not chunk:
                break
            q.put(dec.decode(chunk))
        q.put(None)

    threading.Thread(target=reader, daemon=True).start()

    text = ""            # everything so far (with \r), for markers
    partial = ""         # last line, not yet printed
    started = time.time()
    boot_seconds = 0.0
    deadline = started + timeout
    booted = False
    success = False
    failed = False
    steps = list(session or [])
    step_from = 0        # session waits look at text[step_from:]
    try:
        while time.time() < deadline:
            try:
                chunk = q.get(timeout=0.5)
            except queue.Empty:
                continue
            if chunk is None:
                break
            check_from = max(0, len(text) - 64)
            text += chunk
            partial += chunk
            *done, partial = partial.split("\n")
            for line in done:
                print(f"  | {line.rstrip()}")
            if not failed and any(m in text[check_from:] for m in forbidden):
                # Keep collecting briefly so the full report is captured.
                failed = True
                deadline = min(deadline, time.time() + 2)
                continue
            if failed:
                continue
            if not booted and all(m in text for m in required):
                booted = True
                boot_seconds = time.time() - started
                step_from = len(text) - len(chunk)
            if booted:
                while steps and steps[0][0] in text[step_from:]:
                    wait_for, send = steps.pop(0)
                    step_from = text.index(wait_for, step_from) + len(wait_for)
                    if send is not None:
                        assert p.stdin is not None
                        p.stdin.write(send.encode())
                        p.stdin.flush()
                if not steps:
                    success = True
                    break
    finally:
        if partial:
            print(f"  | {partial.rstrip()}")
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    if session and booted and not success:
        print(f"[FAIL] typed session stopped waiting for {steps[0][0]!r}")
    return success, text.replace("\r", ""), boot_seconds or time.time() - started


def run_boot_test(image: Path, timeout: int = 120) -> int:
    success, full, seconds = boot_and_watch(image, REQUIRED, FORBIDDEN, timeout, SESSION)
    print("=" * 60)
    if success:
        from budgets import check_budgets
        if not check_budgets(seconds):
            return 1
        print("[PASS] BharatOS booted successfully.")
        print("[PASS] Phase 3 memory-management tests passed.")
        print("[PASS] Phase 4 process/thread tests passed.")
        print("[PASS] Phase 5 scheduler tests passed.")
        print("[PASS] Phase 6 debug and hardening tests passed.")
        print("[PASS] Phase 7 modern hardware tests passed.")
        print("[PASS] Phase 8 memory-that-scales tests passed.")
        print(f"[PASS] Phase 9 multi-core tests passed ({QEMU_SMP} CPUs).")
        print("[PASS] Phase 10 input, files, login and shell tests passed.")
        print("[PASS] Keys typed at the login prompt (serial and PS/2 keyboard) reached the shell.")
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
                                        "[ok] Phase 5 scheduler verified.",
                                        "[ok] Phase 6 debug and hardening verified.",
                                        "[ok] Phase 7 modern hardware verified.",
                                        "[ok] Phase 8 memory that scales verified.",
                                        "[ok] Phase 9 multi-core verified."]
    forbidden = [m for m in ["KERNEL PANIC", "[FAIL]"]
                 if m not in FAULT_DEMO_ALLOW.get(demo, [])]
    success, full, _ = boot_and_watch(image, expect, forbidden, timeout)
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
