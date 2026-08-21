#!/usr/bin/env python3
"""Generate the firmware Doto subset from the web font."""

from pathlib import Path
import subprocess
import sys
from fontTools.ttLib import TTFont


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "web" / "doto.ttf"
OUTPUT = ROOT / "main" / "fonts" / "doto_big.c"


def main() -> int:
    font = TTFont(SOURCE)
    period = font["glyf"][font.getBestCmap()[ord(".")]]
    if period.numberOfContours != 1:
        raise SystemExit("web/doto.ttf must retain its single-module period")

    command = [
        "npx", "--no-install", "lv_font_conv",
        "--font", str(SOURCE),
        "--size", "126",
        "--bpp", "4",
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--no-compress",
        "-r", "46,0x30-0x39",
        "-o", str(OUTPUT),
    ]
    subprocess.run(command, check=True, shell=sys.platform == "win32")

    text = OUTPUT.read_text()
    marker = " * Bpp: 4\n"
    note = (
        " * Source: web/doto.ttf (SIL OFL 1.1; see web/OFL-Doto.txt)\n"
        " * The source period is the single Doto module used by the custom sign.\n"
    )
    opts_start = text.index(" * Opts:")
    opts_end = text.index("\n", opts_start)
    opts = (
        " * Opts: --font web/doto.ttf --size 126 --bpp 4 --format lvgl "
        "--lv-include lvgl.h --no-compress -r 46,0x30-0x39 "
        "-o main/fonts/doto_big.c"
    )
    text = text[:opts_start] + opts + text[opts_end:]
    OUTPUT.write_text(text.replace(marker, marker + note, 1).rstrip() + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
