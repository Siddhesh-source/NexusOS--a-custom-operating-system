"""Budgets: limits the kernel must stay within, checked by every
`python build.py test` (and so by CI).

- Kernel size: lines of kernel code (C, headers, assembly), not counting
  self-tests (kernel/tests) or third-party headers (kernel/include). A
  small kernel is easier to review and trust; growing past the limit is a
  prompt to simplify or to raise the limit on purpose, in the same change.
- Boot time: seconds from starting QEMU until every boot self-test has
  passed. This catches a change that makes boot or the tests much slower.
  It is measured under QEMU without hardware acceleration, so it is
  generous and only meant to catch big regressions.

Each run prints the measured values, so the log records them.
"""
from __future__ import annotations
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KERNEL_DIR = ROOT / "kernel"

LIMITS = {
    # Raised from 8,000 when Phases 7-8 (ACPI, PCI, APIC, tickless clock,
    # buddy, slab, demand paging, copy-on-write) took it to about 7,700,
    # then from 10,000 for V1 (input, files, login, shell, desktop:
    # Phases 10-11), as planned in docs/UPDATES.md §7.
    "kernel_lines": 16000,
    "boot_seconds": 90,      # about 15 s locally; CI machines are slower
}

EXCLUDED = [KERNEL_DIR / "tests", KERNEL_DIR / "include"]


def kernel_lines() -> int:
    total = 0
    for path in sorted(KERNEL_DIR.rglob("*")):
        if path.suffix not in (".c", ".h", ".asm"):
            continue
        if any(path.is_relative_to(e) for e in EXCLUDED):
            continue
        total += len(path.read_text(encoding="utf-8", errors="replace").splitlines())
    return total


def check_budgets(boot_seconds: float) -> bool:
    measured = {"kernel_lines": kernel_lines(), "boot_seconds": round(boot_seconds, 1)}
    ok = True
    for name, value in measured.items():
        limit = LIMITS[name]
        within = value <= limit
        ok = ok and within
        print(f"[budget] {name}: {value} (limit {limit}) {'ok' if within else 'OVER LIMIT'}")
    return ok
