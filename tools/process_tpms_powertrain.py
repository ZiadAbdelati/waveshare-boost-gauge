#!/usr/bin/env python3
"""Build the device/web TPMS powertrain art from the original RGBA PNG.

The source contains useful white line art in its alpha channel and unrelated RGB
values under fully transparent pixels. Always composite with alpha first. The
result is scaled offline into native 466-space, sharpened without adding a glow,
and emitted both as a browser PNG and little-endian RGB565 C data.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

SIZE = 466
CENTER = SIZE / 2.0
# The previous fit-to-circle asset used 0.616 source pixels per output pixel.
# 0.493 is exactly 80% of that visible extent, per the physical-panel review.
SOURCE_SCALE = 0.493
ALPHA_BBOX_THRESHOLD = 12
BLACK_CUTOFF = 20
WHITE_POINT = 224


def load_alpha_correct_luma(path: Path) -> np.ndarray:
    rgba = np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32)
    alpha = rgba[:, :, 3] / 255.0
    # The source art is neutral white, but max-channel preserves its finest edge
    # samples if an exporter wrote slightly unequal RGB channels.
    luma = rgba[:, :, :3].max(axis=2) * alpha
    luma[alpha <= 0.0] = 0.0
    return np.clip(np.rint(luma), 0, 255).astype(np.uint8)


def crop_visible(luma: np.ndarray) -> np.ndarray:
    ys, xs = np.where(luma > ALPHA_BBOX_THRESHOLD)
    if not len(xs):
        raise ValueError("source PNG has no visible powertrain pixels")
    return luma[ys.min() : ys.max() + 1, xs.min() : xs.max() + 1]


def resize_and_sharpen(crop: np.ndarray) -> np.ndarray:
    height, width = crop.shape
    out_w = max(1, round(width * SOURCE_SCALE))
    out_h = max(1, round(height * SOURCE_SCALE))
    image = Image.fromarray(crop, "L").resize((out_w, out_h), Image.Resampling.LANCZOS)
    # A small-radius unsharp pass tightens the downsampled white edge. It does not
    # invent a halo; the black cutoff below removes every low-level ringing pixel.
    image = image.filter(ImageFilter.UnsharpMask(radius=0.65, percent=150, threshold=2))
    # Promote before subtracting: uint8 subtraction wraps below zero and turns
    # the black face into bright speckles before clipping.
    values = np.asarray(image, dtype=np.int32)
    values = ((values - BLACK_CUTOFF) * 255 + (WHITE_POINT - BLACK_CUTOFF) // 2) // (
        WHITE_POINT - BLACK_CUTOFF
    )
    return np.clip(values, 0, 255).astype(np.uint8)


def center_on_face(art: np.ndarray) -> np.ndarray:
    face = np.zeros((SIZE, SIZE), dtype=np.uint8)
    height, width = art.shape
    x = round(CENTER - width / 2.0)
    y = round(CENTER - height / 2.0)
    if x < 0 or y < 0 or x + width > SIZE or y + height > SIZE:
        raise ValueError(f"scaled art {width}x{height} does not fit {SIZE}x{SIZE}")
    face[y : y + height, x : x + width] = art
    return face


def write_rgb565_header(face: np.ndarray, path: Path) -> None:
    r5 = (face.astype(np.uint16) >> 3) & 0x1F
    g6 = (face.astype(np.uint16) >> 2) & 0x3F
    words = (r5 << 11) | (g6 << 5) | r5
    flat = words.ravel()
    raw = np.empty(flat.size * 2, dtype=np.uint8)
    raw[0::2] = flat & 0xFF
    raw[1::2] = flat >> 8

    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define TPMS_POWERTRAIN_W {SIZE}",
        f"#define TPMS_POWERTRAIN_H {SIZE}",
        "#define TPMS_POWERTRAIN_OFF_X 0",
        f"#define TPMS_POWERTRAIN_IMG_W {SIZE}",
        "",
        f"static const uint8_t tpms_powertrain_rgb565[{raw.size}] = {{",
    ]
    for offset in range(0, raw.size, 16):
        chunk = ",".join(f"0x{value:02x}" for value in raw[offset : offset + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path("tpmspowertrain.png"))
    parser.add_argument("--web", type=Path, default=Path("web/tpms_powertrain.png"))
    parser.add_argument("--header", type=Path, default=Path("main/tpms_powertrain_rgb565.h"))
    args = parser.parse_args()

    crop = crop_visible(load_alpha_correct_luma(args.source))
    art = resize_and_sharpen(crop)
    face = center_on_face(art)
    args.web.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.repeat(face[:, :, None], 3, axis=2), "RGB").save(args.web, optimize=True)
    write_rgb565_header(face, args.header)

    ys, xs = np.where(face > BLACK_CUTOFF)
    print(
        f"source crop={crop.shape[1]}x{crop.shape[0]} art={art.shape[1]}x{art.shape[0]} "
        f"visible bbox=({xs.min()},{ys.min()})-({xs.max()},{ys.max()})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
