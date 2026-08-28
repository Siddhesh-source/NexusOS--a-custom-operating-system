from __future__ import annotations
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import List

ROOT = Path(__file__).resolve().parent.parent
LIMINE_DIR = ROOT / "tools" / "limine" / "limine-binary"

REQUIRED = [
    "N E X U S   O S",
    "Phase 1: Kernel Foundation",
    "[ok] Kernel started successfully.",
]


def find_qemu() -> str:
    for key in ["NEXUS_QEMU"]:
        p = os.environ.get(key)
        if p and Path(p).exists():
            return p
    for name in ["qemu-system-x86_64"]:
        found = shutil.which(name)
        if found:
            return found
    return ""


def cmd_for(image: Path) -> List[str]:
    qemu = find_qemu()
    if not qemu:
        print("[ERROR] qemu-system-x86_64 not found", file=sys.stderr)
        return []
    return [qemu, "-m", "512M",
            "-drive", f"file={image},format=raw,media=disk,if=ide",
            "-nographic", "-no-reboot", "-no-shutdown"]


def run_boot_test(image: Path, limine_dir: Path = LIMINE_DIR,
                  timeout: int = 60) -> int:
    cmd = cmd_for(image)
    if not cmd:
        return 1
    print(f"[TEST] {' '.join(cmd)}")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, bufsize=1)
    lines: List[str] = []
    start = time.time()
    success = False
    try:
        assert p.stdout is not None
        for line in p.stdout:
            line = line.rstrip()
            lines.append(line)
            print(f"  | {line}")
            if all(m in "\n".join(lines) for m in REQUIRED):
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

    full = "\n".join(lines)
    print("=" * 60)
    if success:
        print("[PASS] NEXUS OS booted successfully.")
        print("[PASS] Startup banner verified.")
        return 0
    print("[FAIL] Boot did not reach the startup banner.")
    print("[FAIL] Missing:")
    for m in REQUIRED:
        if m not in full:
            print(f"         - {m!r}")
    return 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: test_boot.py <image>", file=sys.stderr)
        sys.exit(2)
    sys.exit(run_boot_test(Path(sys.argv[1])))
