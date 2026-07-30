#!/usr/bin/env python3
"""Measure Night City physical cadence with HUD gradient fill off and on.

Each arm starts from a fresh boot, selects Night City, enables demo mode, and
disables pixel shift so its full-screen repaint cannot contaminate a run. The
initial theme and display settings are restored before exit.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from pathlib import Path
from urllib.request import Request, urlopen


def api_get(base: str, path: str) -> dict:
    with urlopen(f"{base}{path}", timeout=5) as response:
        return json.load(response)


def api_put(base: str, path: str, body: dict) -> dict:
    request = Request(
        f"{base}{path}",
        data=json.dumps(body).encode(),
        method="PUT",
        headers={"Content-Type": "application/json"},
    )
    with urlopen(request, timeout=8) as response:
        return json.load(response)


def api_post(base: str, path: str) -> None:
    request = Request(f"{base}{path}", data=b"", method="POST")
    try:
        urlopen(request, timeout=3)
    except Exception:
        pass  # Restart normally drops the connection before replying.


def wait_online(base: str, timeout_s: float = 30.0) -> None:
    deadline = time.monotonic() + timeout_s
    last_error = None
    while time.monotonic() < deadline:
        try:
            api_get(base, "/api/v1/state")
            return
        except Exception as error:  # noqa: BLE001
            last_error = error
            time.sleep(0.3)
    raise RuntimeError(f"device did not come back online: {last_error}")


def wait_display_metrics(base: str, timeout_s: float = 10.0) -> dict:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        state = api_get(base, "/api/v1/state")
        if state["display"].get("renderFps"):
            return state
        time.sleep(0.25)
    raise RuntimeError("display metrics did not resume after restart")


def collect_arm(base: str, gradient: bool, settle_s: float, sample_s: float) -> dict:
    api_post(base, "/api/v1/restart")
    time.sleep(2)
    wait_online(base)
    api_put(base, "/api/v1/themes/active", {"id": "night-city"})
    config = api_put(base, "/api/v1/themes/config", {
        "demoMode": True,
        "demoFastSweep": False,
        "hudGradient": gradient,
        "pixelShift": False,
        "teSync": True,
        "regionDBuf": True,
    })
    state = api_get(base, "/api/v1/state")
    if config.get("hudGradient") is not gradient:
        raise RuntimeError(f"hudGradient did not take effect: {config}")
    if state.get("activeThemeId") != "night-city" or state.get("demo") is not True:
        raise RuntimeError(f"Night City demo arm not active: {state}")
    if config.get("teSync") is not True or config.get("regionDBuf") is not True:
        raise RuntimeError(f"display controls did not take effect: {config}")

    wait_display_metrics(base)
    time.sleep(settle_s)
    start_state = api_get(base, "/api/v1/state")
    rows = []
    deadline = time.monotonic() + sample_s
    while time.monotonic() < deadline:
        display = api_get(base, "/api/v1/state")["display"]
        if display.get("renderFps"):
            rows.append(display)
        time.sleep(1.05)
    if not rows:
        raise RuntimeError("no display samples collected")

    deduped = []
    previous = None
    for row in rows:
        key = (row["renderFps"], row["worstRenderUs"], row["teWaits"], row["teSkips"])
        if key != previous:
            deduped.append(row)
        previous = key

    def values(key: str, source=rows) -> list[int]:
        return [int(row[key]) for row in source]

    return {
        "gradient": gradient,
        "firmwareVersion": start_state.get("firmwareVersion"),
        "uptimeMsAtStart": start_state.get("uptimeMs"),
        "samples": len(rows),
        "distinctWindows": len(deduped),
        "renderFpsMin": min(values("renderFps")),
        "renderFpsMedian": statistics.median(values("renderFps")),
        "renderGapP50UsMedian": statistics.median(values("renderGapP50Us")),
        "renderGapMaxUsMax": max(values("renderGapMaxUs")),
        "worstRenderUsMedian": statistics.median(values("worstRenderUs")),
        "worstRenderUsMax": max(values("worstRenderUs")),
        "framesOverBudgetMedian": statistics.median(values("framesOverBudget", deduped)),
        "framesOverBudgetSum": sum(values("framesOverBudget", deduped)),
        "pixelsPerSecondMedian": statistics.median(values("pixelsPerSecond")),
        "flushesPerSecondMedian": statistics.median(values("flushesPerSecond")),
        "teWaitsSum": sum(values("teWaits", deduped)),
        "teSkipsSum": sum(values("teSkips", deduped)),
        "teTimeoutsSum": sum(values("teTimeouts", deduped)),
        "raw": rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://192.168.50.102")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--settle", type=float, default=10.0)
    parser.add_argument("--sample", type=float, default=60.0,
                        help="seconds per arm; 60 covers four complete 15 s demo envelopes")
    parser.add_argument("--out", type=Path, default=Path("hud_gradient_results.json"))
    args = parser.parse_args()
    base = args.url.rstrip("/")
    initial = api_get(base, "/api/v1/themes")
    restore_config = {
        "demoMode": bool(initial["demoMode"]),
        "demoFastSweep": bool(initial["demoFastSweep"]),
        "hudGradient": bool(initial["hudGradient"]),
        "pixelShift": bool(initial["pixelShift"]),
        "pixelShiftSec": int(initial["pixelShiftSec"]),
        "teSync": bool(initial["teSync"]),
        "regionDBuf": bool(initial["regionDBuf"]),
    }

    results = []
    try:
        for round_number in range(1, args.rounds + 1):
            # Alternate order so time drift cannot consistently favor one arm.
            arm_order = (False, True) if round_number % 2 else (True, False)
            for gradient in arm_order:
                label = "ON" if gradient else "OFF"
                print(f"round {round_number}/{args.rounds}, gradient {label}", file=sys.stderr)
                result = collect_arm(base, gradient, args.settle, args.sample)
                result["round"] = round_number
                results.append(result)
                print(json.dumps({k: v for k, v in result.items() if k != "raw"}, indent=2))
    finally:
        print("restoring initial board state", file=sys.stderr)
        wait_online(base)
        api_put(base, "/api/v1/themes/config", restore_config)
        api_put(base, "/api/v1/themes/active", {"id": initial["activeThemeId"]})

    output = {"initial": initial, "runs": results}
    args.out.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(f"raw results written to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
