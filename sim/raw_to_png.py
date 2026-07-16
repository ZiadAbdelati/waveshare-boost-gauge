#!/usr/bin/env python3
"""Convert boost_gauge_sim RGBA dumps to PNG/GIF."""

from __future__ import annotations

import struct
import sys
from pathlib import Path

from PIL import Image


def load_raw(path: Path) -> Image.Image:
    data = path.read_bytes()
    if len(data) < 12 or data[:4] != b"RGBA":
        raise ValueError(f"bad raw header: {path}")
    w, h = struct.unpack_from("<II", data, 4)
    px = data[12:]
    expected = w * h * 4
    if len(px) < expected:
        raise ValueError(f"short pixel data in {path}: {len(px)} < {expected}")
    # LVGL ARGB8888 on little-endian is typically stored B,G,R,A in memory for
    # true-color; lv_color_format ARGB8888 is 0xAARRGGBB in native order.
    # On LE that becomes B,G,R,A bytes. Pillow wants RGBA.
    img = Image.frombytes("RGBA", (w, h), px[:expected], "raw", "BGRA")
    return img.convert("RGB")


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "preview/sim")
    if not root.exists():
        print(f"missing dir: {root}", file=sys.stderr)
        return 1

    frames: list[Image.Image] = []
    for raw in sorted(root.glob("gauge_*.raw")):
        img = load_raw(raw)
        out = raw.with_suffix(".png")
        img.save(out)
        print(f"wrote {out}")

    frame_dir = root / "frames"
    if frame_dir.is_dir():
        for raw in sorted(frame_dir.glob("frame_*.raw")):
            frames.append(load_raw(raw))
        if frames:
            gif = root / "gauge_sweep.gif"
            frames[0].save(
                gif,
                save_all=True,
                append_images=frames[1:],
                duration=80,
                loop=0,
            )
            print(f"wrote {gif}")

    # contact sheet if we have the four named states
    names = ["vac", "atmo", "boost", "over"]
    tiles = []
    for n in names:
        p = root / f"gauge_{n}.png"
        if p.exists():
            tiles.append(Image.open(p))
    if len(tiles) == 4:
        w, h = tiles[0].size
        sheet = Image.new("RGB", (w * 2 + 24, h * 2 + 24), (20, 20, 24))
        for i, im in enumerate(tiles):
            x = 8 + (i % 2) * (w + 8)
            y = 8 + (i // 2) * (h + 8)
            sheet.paste(im, (x, y))
        out = root / "gauge_sheet.png"
        sheet.save(out)
        print(f"wrote {out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
