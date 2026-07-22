#!/usr/bin/env python3
"""Capture the live AMOLED panel and save it as a PNG.

The firmware exposes a debug endpoint that renders the active LVGL screen into
a buffer and streams it as raw little-endian RGB565 (466 x 466). This turns
that stream into a viewable image, which is how the panel gets verified without
physically looking at it.

Usage:
  python tools/fetch_panel_snapshot.py --url http://192.168.1.42 --out panel.png
"""

from __future__ import annotations

import argparse
import sys
import urllib.request

import numpy as np
from PIL import Image

DEFAULT_W = 466
DEFAULT_H = 466
ENDPOINT = "/api/v1/debug/snapshot"


def rgb565_to_png(raw: bytes, width: int, height: int, out_path: str) -> None:
    expected = width * height * 2
    if len(raw) != expected:
        raise SystemExit(f"expected {expected} bytes for {width}x{height} RGB565, got {len(raw)}")

    packed = np.frombuffer(raw, dtype="<u2").reshape(height, width)
    # Expand 5/6/5 to full 8-bit range; compute in uint16 to avoid overflow.
    r = (((packed >> 11) & 0x1F).astype(np.uint16) * 255 // 31).astype(np.uint8)
    g = (((packed >> 5) & 0x3F).astype(np.uint16) * 255 // 63).astype(np.uint8)
    b = ((packed & 0x1F).astype(np.uint16) * 255 // 31).astype(np.uint8)
    Image.fromarray(np.dstack([r, g, b]), "RGB").save(out_path)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True, help="device base URL, e.g. http://192.168.1.42")
    ap.add_argument("--out", default="panel.png")
    ap.add_argument("--width", type=int, default=DEFAULT_W)
    ap.add_argument("--height", type=int, default=DEFAULT_H)
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    url = args.url.rstrip("/") + ENDPOINT
    try:
        with urllib.request.urlopen(url, timeout=args.timeout) as resp:
            raw = resp.read()
    except Exception as exc:  # noqa: BLE001 - surface any transport failure plainly
        print(f"snapshot fetch failed: {exc}", file=sys.stderr)
        return 1

    rgb565_to_png(raw, args.width, args.height, args.out)
    print(f"wrote {args.out} ({args.width}x{args.height}, {len(raw)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
