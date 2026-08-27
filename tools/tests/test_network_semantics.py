#!/usr/bin/env python3
"""Network configuration semantics - source-contract test (no radio).

Guards from the regression ledger and current source:
  * Saved-network list is capped at BOOST_NET_MAX_SAVED (5): the insert path
    only adds when saved_count < MAX and the NVS restore clamps the count
    (ledger 2026-08-15 "Wi-Fi STA scans before connecting across up to 5 saved
    NVS networks").
  * Scans/reconnects are suspended while SoftAP clients are connected or STA
    already has an IP, so an out-of-range saved SSID cannot starve the SoftAP
    (ledger 2026-08-15 SoftAP-starvation row).
  * STA auto-reconnect is timer-driven with a non-trivial backoff; user-initiated
    reconnect stays immediate (same row; "timer is the only auto-retry path").
  * AP credentials: BOOST_AP_PASSWORD == "boost1234", AP SSID format
    "BoostGauge-%02X%02X" from the MAC. The QR overlay encodes the same literal
    (single source of truth; AGENTS.md WIFI:T:WPA;S:<ssid>;P:boost1234;;).
  * The HTTP API's saved list is [{ssid}] items; MRU moves to front on update;
    delete removes by ssid (boost_web.c network_status_json/delete semantics).

Stdlib only. Run:  python3 tools/tests/test_network_semantics.py
"""

from __future__ import annotations

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
NET_C = REPO_ROOT / "main" / "boost_network.c"
NET_H = REPO_ROOT / "main" / "boost_network.h"
WEB_C = REPO_ROOT / "main" / "boost_web.c"
PAGE_C = REPO_ROOT / "main" / "boost_page.c"


class Result:
    def __init__(self) -> None:
        self.checks = 0
        self.failures: list[str] = []

    def check(self, ok: bool, label: str, detail: str = "") -> None:
        self.checks += 1
        if ok:
            print(f"PASS {label}")
        else:
            self.failures.append(label)
            print(f"FAIL {label}  [{detail if detail else 'assertion failed'}]")


def main() -> int:
    result = Result()
    net_c = NET_C.read_text(encoding="utf-8")
    net_h = NET_H.read_text(encoding="utf-8")
    web_c = WEB_C.read_text(encoding="utf-8")
    page_c = PAGE_C.read_text(encoding="utf-8")

    # --- Saved-network list cap: 5 ------------------------------------------
    result.check(re.search(r"#define\s+BOOST_NET_MAX_SAVED\s+5\b", net_h) is not None,
                 "BOOST_NET_MAX_SAVED == 5")
    result.check("s_cfg.saved_count < BOOST_NET_MAX_SAVED" in net_c,
                 "insert path only grows the saved list below the cap")
    result.check("saved_cnt > BOOST_NET_MAX_SAVED" in net_c
                 and "saved_cnt = BOOST_NET_MAX_SAVED" in net_c,
                 "NVS restore clamps a corrupt saved_cnt to the cap")

    # --- Scan suspension while AP clients connected / STA has IP -------------
    result.check("if (s_ap_clients > 0 || s_sta_got_ip)" in net_c,
                 "scan task skips when AP clients are connected or STA has IP")
    result.check("!s_sta_got_ip && s_ap_clients == 0" in net_c,
                 "reconnect timer only rearms when idle (no AP clients, no IP)")

    # --- Auto-reconnect is timer-driven with backoff -------------------------
    m = re.search(r"#define\s+WIFI_SCAN_RETRY_DELAY_MS\s+(\d+)", net_c)
    result.check(m is not None and int(m.group(1)) >= 1000,
                 "WIFI_SCAN_RETRY_DELAY_MS is a multi-second backoff",
                 f"got {m.group(1) if m else None}")
    result.check("xTimerCreate(" in net_c and "reconnect_timer_cb" in net_c,
                 "auto-reconnect path is a timer callback (not an immediate loop)")
    result.check(re.search(r"xTimerReset\(s_reconnect_timer, 0\)", net_c) is not None
                 and "WIFI_EVENT_STA_DISCONNECTED" in net_c,
                 "STA disconnect only (re)arms the backoff timer")

    # --- AP credentials -------------------------------------------------------
    m = re.search(r'#define\s+BOOST_AP_PASSWORD\s+"([^"]+)"', net_h)
    result.check(m is not None and m.group(1) == "boost1234",
                 "BOOST_AP_PASSWORD == boost1234", f"got {m.group(1) if m else None}")
    result.check('"BoostGauge-%02X%02X"' in net_c,
                 "AP SSID format BoostGauge-<MAC[4]><MAC[5]>")
    result.check('snprintf(s_ap_ssid' in net_c and "mac[4]" in net_c and "mac[5]" in net_c,
                 "AP SSID derived from the MAC tail bytes")

    # --- QR payload uses the same AP ssid + password literal -------------------
    m = re.search(r'snprintf\(payload,\s*sizeof\(payload\),\s*"WIFI:T:WPA;S:%s;P:%s;;",\s*net\.ap_ssid,\s*BOOST_AP_PASSWORD\)',
                  page_c)
    result.check(m is not None, "QR encodes WIFI:T:WPA;S:<ap_ssid>;P:<BOOST_AP_PASSWORD>;;")

    # --- HTTP API saved list semantics -----------------------------------------
    result.check('\\"saved\\":[' in web_c or '"saved":[' in web_c.replace('\\"', '"'),
                 "network response serializes a saved array")
    result.check('\\"ssid\\":\\"%s\\"' in web_c,
                 "saved items are {ssid} objects")
    # MRU move-to-front in update, delete-by-ssid, JSON body on DELETE.
    result.check("saved[0] = entry" in net_c and "found_idx" in net_c,
                 "updated saved entry moves to front (MRU)")
    result.check("boost_network_delete_saved(" in net_c and "strcmp(" in net_c,
                 "delete removes the matching saved ssid")

    # network PUT / DELETE input rules on the wire (boost_web.c).
    result.check('"missing_ssid"' in web_c, "DELETE /network 400 missing_ssid")
    result.check('"invalid_mode"' in web_c, "PUT /network 400 invalid_mode")
    result.check('"network_update_failed"' in web_c,
                 "PUT /network 400 network_update_failed when the update is rejected")

    passed = result.checks - len(result.failures)
    print(f"\n{passed}/{result.checks} checks passed, {len(result.failures)} failed")
    if result.failures:
        for label in result.failures:
            print(f"  - {label}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
