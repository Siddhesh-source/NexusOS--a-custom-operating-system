"""Turn a folder of PNG frames into a .bvid file that the Player app plays.

Rarely run; the result is checked in (boot/bundle/media/bunny.bvid), like
kernel/ui/fonts.bin. Needs Pillow.

The frames came from Big Buck Bunny (c) Blender Foundation, CC BY 3.0,
https://peach.blender.org/ :

    ffmpeg -t 10 -i Big_Buck_Bunny_360_10s_1MB.mp4 -an \
           -vf "fps=12,scale=192:108:flags=lanczos" frame_%03d.png
    python scripts/make_video.py <frames folder> boot/bundle/media/bunny.bvid

The second clip is drawn by this script itself:

    python scripts/make_video.py --waves boot/bundle/media/waves.bvid

The format is as simple as a video can be, so the kernel needs no codec:

    offset 0   "BVID"
           4   u16 width, u16 height, u16 frames per second, u16 frame count
          12   20 bytes of zeros (room to grow)
          32   palette: 256 colours, 3 bytes each (red, green, blue)
         800   frames, one after another, width*height bytes each; every
               byte is a palette index

One palette is shared by every frame (picked from all of them together),
so playing is a table lookup per pixel.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

from PIL import Image


def waves(out_path: str) -> int:
    """A 3-second clip drawn here (no outside footage): coloured waves
    rolling under a sunrise, so the Player has a second video to play."""
    import math
    from PIL import ImageDraw
    w, h, fps, n = 192, 108, 12, 36
    imgs = []
    for f in range(n):
        im = Image.new("RGB", (w, h))
        d = ImageDraw.Draw(im)
        for y in range(h):                       # sky: saffron to deep blue
            t = y / (h - 1)
            d.line([(0, y), (w, y)], fill=(int(255 * (1 - t) + 20 * t), int(153 * (1 - t) + 40 * t),
                                           int(51 * (1 - t) + 90 * t)))
        sun_y = 70 - f * 0.8
        d.ellipse([96 - 18, sun_y - 18, 96 + 18, sun_y + 18], fill=(255, 236, 170))
        for k, col in enumerate([(19, 136, 8), (16, 110, 120), (10, 60, 120)]):
            base = 62 + k * 14
            pts = [(x, base + 5 * math.sin(x / 14 + f * 2 * math.pi / n * (k + 1))) for x in range(w + 1)]
            d.polygon(pts + [(w, h), (0, h)], fill=col)
        imgs.append(im)
    return write_bvid(imgs, w, h, fps, out_path)


def write_bvid(imgs, w: int, h: int, fps: int, out_path: str) -> int:
    # One palette for the whole clip: quantise a tall image made of a
    # sample of frames, then map every frame onto it.
    sample = imgs[:: max(1, len(imgs) // 16)]
    strip = Image.new("RGB", (w, h * len(sample)))
    for i, im in enumerate(sample):
        strip.paste(im, (0, i * h))
    pal_img = strip.quantize(colors=256, method=Image.Quantize.MEDIANCUT)
    palette = (pal_img.getpalette() or [])[: 256 * 3]
    palette += [0] * (256 * 3 - len(palette))
    out = bytearray(b"BVID" + struct.pack("<HHHH", w, h, fps, len(imgs)) + bytes(20))
    out += bytes(palette)
    for im in imgs:
        q = im.quantize(palette=pal_img, dither=Image.Dither.FLOYDSTEINBERG)
        out += q.tobytes()
    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    Path(out_path).write_bytes(out)
    print(f"{out_path}: {w}x{h}, {len(imgs)} frames, {len(out)} bytes")
    return 0


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "--waves":
        return waves(sys.argv[2])
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    frames = sorted(Path(sys.argv[1]).glob("*.png"))
    if not frames:
        print("no PNG frames found")
        return 1
    imgs = [Image.open(f).convert("RGB") for f in frames]
    w, h = imgs[0].size
    return write_bvid(imgs, w, h, 12, sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
