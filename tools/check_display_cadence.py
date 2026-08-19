#!/usr/bin/env python3
"""Fail if the live physical-gauge render cadence drops below the contract."""

from __future__ import annotations

import argparse
import json
import time
from urllib.request import urlopen

MIN_MEDIAN_RENDER_FPS = 60
DEFAULT_SECONDS = 12
SAMPLE_INTERVAL_SECONDS = 0.25


def fetch_display(base_url: str) -> dict[str, int]:
    with urlopen(f"{base_url.rstrip('/')}/api/v1/state", timeout=3) as response:
        state = json.load(response)
    return state["display"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://192.168.1.100")
    parser.add_argument("--seconds", type=float, default=DEFAULT_SECONDS)
    args = parser.parse_args()

    deadline = time.monotonic() + args.seconds
    render_fps: list[int] = []
    while time.monotonic() < deadline:
        display = fetch_display(args.url)
        fps = int(display["renderFps"])
        if fps:
            render_fps.append(fps)
        time.sleep(SAMPLE_INTERVAL_SECONDS)

    # Metrics are one-second buckets. The first reported value can straddle a
    # bucket reset, so assess sustained cadence by its median after warmup.
    warmup_samples = render_fps[4:]
    if not warmup_samples:
        raise SystemExit("insufficient physical render samples after warmup")
    observed_min = min(warmup_samples)
    observed_median = sorted(warmup_samples)[len(warmup_samples) // 2]
    print(f"physical render FPS: min={observed_min} median={observed_median} samples={len(warmup_samples)}")
    if observed_median < MIN_MEDIAN_RENDER_FPS:
        raise SystemExit(
            "physical render cadence regressed: "
            f"median={observed_median}; expected at least {MIN_MEDIAN_RENDER_FPS} FPS"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
