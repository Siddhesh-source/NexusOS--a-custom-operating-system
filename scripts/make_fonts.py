#!/usr/bin/env python3
"""Render the desktop's fonts into kernel/ui/fonts.bin.

Only needed when the fonts change: the output is checked in, so the
normal build needs nothing extra. This script needs Pillow
(`pip install pillow`) and downloads IBM Plex Sans and IBM Plex Mono
(SIL Open Font License) from the Google Fonts repository into
build/fonts-src/ the first time.

Each font is drawn once per size, smoothed (8-bit coverage, "alpha"),
for the printable ASCII characters 0x20-0x7E. The kernel blends those
coverage values with the colours behind them (kernel/ui/font.c).

File layout (little endian):
  "BFN1", u32 font_count
  per font: u16 line_height, u16 baseline, u16 mono_advance (0 if
            proportional), u16 0, then 95 glyphs of
            i16 x_off, i16 y_off (from the top of the line), u16 w, u16 h,
            u16 advance, u16 0, u32 offset into the coverage data
  coverage data: w*h bytes per glyph, rows top to bottom
"""
from __future__ import annotations
import struct
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "build" / "fonts-src"
OUT = ROOT / "kernel" / "ui" / "fonts.bin"
LICENSE_OUT = ROOT / "kernel" / "ui" / "fonts-OFL.txt"
BASE = "https://raw.githubusercontent.com/google/fonts/main/ofl/"
FILES = {
    "sans": ("ibmplexsans/IBMPlexSans%5Bwdth,wght%5D.ttf", "IBMPlexSans-var.ttf"),
    "mono": ("ibmplexmono/IBMPlexMono-Regular.ttf", "IBMPlexMono-Regular.ttf"),
    "license": ("ibmplexsans/OFL.txt", "OFL.txt"),
}

# Order = enum font_id in kernel/ui/font.h.
FONTS = [
    # name,          file,  size, weight, line height
    ("FONT_SMALL",   "sans", 13, 400, 18),
    ("FONT_UI",      "sans", 15, 400, 22),
    ("FONT_MEDIUM",  "sans", 15, 500, 22),
    ("FONT_HEADING", "sans", 22, 600, 30),
    ("FONT_DISPLAY", "sans", 56, 300, 72),
    ("FONT_MONO",    "mono", 15, 400, 19),
]


def fetch() -> None:
    SRC.mkdir(parents=True, exist_ok=True)
    for remote, local in FILES.values():
        dest = SRC / local
        if not dest.exists():
            print(f"[fonts] downloading {local}")
            urllib.request.urlretrieve(BASE + remote, dest)


def load(kind: str, size: int, weight: int):
    from PIL import ImageFont
    f = ImageFont.truetype(str(SRC / FILES[kind][1]), size)
    if kind == "sans":
        axes = f.get_variation_axes()
        values = []
        for a in axes:
            name = a.get("name", b"")
            name = name.decode() if isinstance(name, bytes) else str(name)
            values.append(weight if name.lower().startswith("weight") else a["default"])
        f.set_variation_by_axes(values)
    return f


def render(kind: str, size: int, weight: int, line_h: int) -> tuple[bytes, bytes]:
    from PIL import Image, ImageDraw
    f = load(kind, size, weight)
    asc, desc = f.getmetrics()
    baseline = (line_h - (asc + desc)) // 2 + asc
    mono_adv = round(f.getlength("M")) if kind == "mono" else 0
    head = bytearray(struct.pack("<HHHH", line_h, baseline, mono_adv, 0))
    data = bytearray()
    for code in range(0x20, 0x7F):
        ch = chr(code)
        adv = mono_adv or round(f.getlength(ch))
        left, top, right, bottom = f.getbbox(ch, anchor="ls")
        w, h = max(0, right - left), max(0, bottom - top)
        cov = b""
        if w and h:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-left, -top), ch, fill=255, font=f, anchor="ls")
            cov = img.tobytes()
        head += struct.pack("<hhHHHHI", left, baseline + top, w, h, adv, 0, len(data))
        data += cov
    return bytes(head), bytes(data)


def main() -> int:
    try:
        import PIL  # noqa: F401
    except ImportError:
        print("make_fonts.py needs Pillow: pip install pillow", file=sys.stderr)
        return 1
    fetch()
    heads, blob = [], bytearray()
    for name, kind, size, weight, line_h in FONTS:
        head, data = render(kind, size, weight, line_h)
        # Offsets are relative to the start of all coverage data.
        fixed = bytearray(head[:8])
        for i in range(95):
            g = list(struct.unpack_from("<hhHHHHI", head, 8 + i * 16))
            g[6] += len(blob)
            fixed += struct.pack("<hhHHHHI", *g)
        heads.append(bytes(fixed))
        blob += data
        print(f"[fonts] {name}: {kind} {size}px weight {weight}, {len(data)} bytes")
    out = b"BFN1" + struct.pack("<I", len(FONTS)) + b"".join(heads) + bytes(blob)
    OUT.write_bytes(out)
    LICENSE_OUT.write_text("IBM Plex Sans and IBM Plex Mono, used in fonts.bin:\n\n"
                           + (SRC / "OFL.txt").read_text(encoding="utf-8"), encoding="utf-8")
    print(f"[fonts] wrote {OUT.relative_to(ROOT)} ({len(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
