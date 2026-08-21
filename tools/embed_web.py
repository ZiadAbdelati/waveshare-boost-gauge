#!/usr/bin/env python3
"""Pack the dependency-free web UI into C strings for ESP-IDF.

Usage: tools/embed_web.py web main/generated_web_assets.c main/generated_web_assets.h
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
from pathlib import Path

MIME = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".ttf": "font/ttf",
    ".txt": "text/plain; charset=utf-8",
}


def ident(path: str) -> str:
    return "web_" + "".join(ch if ch.isalnum() else "_" for ch in path).strip("_")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("web_root", type=Path)
    ap.add_argument("out_c", type=Path)
    ap.add_argument("out_h", type=Path)
    args = ap.parse_args()

    files = [p for p in sorted(args.web_root.iterdir()) if p.is_file() and p.suffix.lower() in MIME]
    if not files:
        raise SystemExit(f"no web assets found under {args.web_root}")

    entries: list[tuple[str, str, bytes, str]] = []
    for path in files:
        rel = path.relative_to(args.web_root).as_posix()
        route = "/" if rel == "index.html" else "/" + rel
        raw = path.read_bytes()
        packed = gzip.compress(raw, compresslevel=9, mtime=0)
        mime = MIME.get(path.suffix.lower(), "application/octet-stream")
        entries.append((route, ident(rel), packed, mime))

    guard = "BOOST_GENERATED_WEB_ASSETS_H"
    args.out_h.parent.mkdir(parents=True, exist_ok=True)
    args.out_h.write_text(
        f"""#ifndef {guard}\n#define {guard}\n\n#include <stddef.h>\n#include <stdint.h>\n\ntypedef struct {{\n    const char *path;\n    const char *content_type;\n    const uint8_t *gzip_data;\n    size_t gzip_size;\n    const char *etag;\n}} boost_web_asset_t;\n\nconst boost_web_asset_t *boost_web_asset_find(const char *path);\n\n#endif\n"""
    )

    lines = ['#include "generated_web_assets.h"', "#include <string.h>", ""]
    for _, name, data, _ in entries:
        bytestr = ",".join(f"0x{b:02x}" for b in data)
        lines.append(f"static const uint8_t {name}[] = {{{bytestr}}};")
    lines.append("")
    lines.append("static const boost_web_asset_t s_assets[] = {")
    for route, name, data, mime in entries:
        etag = hashlib.sha256(data).hexdigest()[:16]
        lines.append(f'    {{"{route}", "{mime}", {name}, sizeof({name}), "\\"{etag}\\""}},')
    lines += [
        "};",
        "",
        "const boost_web_asset_t *boost_web_asset_find(const char *path)",
        "{",
        "    if (path == NULL) return NULL;",
        "    for (size_t i = 0; i < sizeof(s_assets) / sizeof(s_assets[0]); ++i) {",
        "        if (strcmp(path, s_assets[i].path) == 0) return &s_assets[i];",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    args.out_c.write_text("\n".join(lines))
    print(f"embedded {len(entries)} assets ({sum(len(e[2]) for e in entries)} gzip bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
