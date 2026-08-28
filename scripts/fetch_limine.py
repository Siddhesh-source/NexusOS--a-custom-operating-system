#!/usr/bin/env python3
from __future__ import annotations
import io
import os
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools" / "limine"
VERSION = "v12.6.1"
BASE = "https://github.com/Limine-Bootloader/Limine/releases/download"
BIN_URL = f"{BASE}/{VERSION}/limine-binary.tar.xz"


def fetch(url: str) -> bytes:
    print(f"[fetch] {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "nexus-build/1.0"})
    with urllib.request.urlopen(req, timeout=300) as r:
        return r.read()


def build_linux_tool(bin_dir: Path) -> None:
    limine = bin_dir / "limine"
    if limine.exists():
        return
    print("[build] compiling limine host tool")
    r = subprocess.run(["make", "-C", str(bin_dir)],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0 or not limine.exists():
        print(r.stdout)
        raise RuntimeError("failed to build limine host tool; ensure gcc/cc is installed")


def main() -> int:
    TOOLS.mkdir(parents=True, exist_ok=True)
    bin_dir = TOOLS / "limine-binary"

    if not bin_dir.exists():
        with tarfile.open(fileobj=io.BytesIO(fetch(BIN_URL)), mode="r:xz") as tf:
            tf.extractall(TOOLS)

    if os.name != "nt":
        build_linux_tool(bin_dir)

    print("[done] limine tooling ready.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
