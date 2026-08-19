#!/usr/bin/env python3
"""Shared HTTP plumbing for the board-facing bench tools.

Every bench tool used to reimplement api_get/api_put/api_post/wait_online with
its own copy - and the copies drifted (PUT timeouts of 5 s and 8 s coexisted,
and wait_online raised different exception types). One shared module keeps the
semantics identical everywhere; a tool that needs a different timeout passes it
explicitly at the call site.

`base` is the board's root URL (e.g. http://192.168.1.100); `path` is the
full API path including the /api/v1 prefix.
"""
from __future__ import annotations

import json
import time
from urllib.request import Request, urlopen

DEFAULT_URL = "http://192.168.1.100"


def api_get(base: str, path: str, timeout: float = 5.0) -> dict:
    with urlopen(f"{base}{path}", timeout=timeout) as response:
        return json.load(response)


def api_put(base: str, path: str, body: dict, timeout: float = 5.0) -> dict:
    request = Request(
        f"{base}{path}",
        data=json.dumps(body).encode(),
        method="PUT",
        headers={"Content-Type": "application/json"},
    )
    with urlopen(request, timeout=timeout) as response:
        return json.load(response)


def api_post(base: str, path: str, timeout: float = 3.0) -> None:
    """POST and swallow the reply: a restart normally drops the connection
    before the board can answer."""
    request = Request(f"{base}{path}", data=b"", method="POST")
    try:
        urlopen(request, timeout=timeout)
    except Exception:
        pass


def wait_online(base: str, timeout_s: float = 30.0) -> None:
    deadline = time.monotonic() + timeout_s
    last_err = None
    while time.monotonic() < deadline:
        try:
            api_get(base, "/api/v1/state")
            return
        except Exception as error:  # noqa: BLE001
            last_err = error
            time.sleep(0.3)
    raise SystemExit(f"device did not come back online: {last_err}")


def wait_display_metrics(base: str, timeout_s: float = 10.0) -> dict:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        state = api_get(base, "/api/v1/state")
        if state["display"].get("renderFps"):
            return state
        time.sleep(0.25)
    raise RuntimeError("display metrics did not resume after restart")
