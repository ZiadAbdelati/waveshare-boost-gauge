#!/usr/bin/env python3
"""Host-side check of the DS3231 civil-date -> UTC epoch conversion.

Standalone on purpose: no pytest, no third-party imports. Run it with

    python tools/test_rtc_epoch.py

It reimplements the days-from-civil arithmetic that lives in main/boost_sensors.c
(rtc_epoch_ms) so a typo in either copy shows up as a failing case rather than as
a wrong wall clock. The DS3231 read path converts BCD registers with this and
rejects any result below the 2023 floor (1700000000000).

Prints PASS/FAIL per case and exits non-zero if anything failed.
"""

import datetime
import sys

# Howard Hinnant's days-from-civil; mirrors rtc_epoch_ms() in boost_sensors.c.
def rtc_epoch_ms(year, mon, day, hour, minute, sec):
    y = year - (1 if mon <= 2 else 0)
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (mon + (-3 if mon > 2 else 9)) + 2) // 5 + day - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    days = era * 146097 + doe - 719468
    return (days * 86400 + hour * 3600 + minute * 60 + sec) * 1000


def main():
    cases = [
        (1970, 1, 1, 0, 0, 0, 0),
        (2000, 1, 1, 0, 0, 0, 946684800000),
        (2023, 11, 14, 22, 13, 20, 1700000000000),  # the firmware validity floor
        (2024, 2, 29, 0, 0, 0, 1709164800000),      # leap year
        (2026, 8, 17, 12, 34, 56, 1786970096000),
        (2099, 12, 31, 23, 59, 59, 4102444799000),  # DS3231 year range ceiling
    ]
    failed = 0
    for args in cases:
        got = rtc_epoch_ms(*args[:6])
        ok = got == args[6]
        # Cross-check a handful against the stdlib regardless of the anchor.
        if args[:6] in {(2024, 2, 29, 0, 0, 0), (2026, 8, 17, 12, 34, 56)}:
            dt = datetime.datetime(*args[:6], tzinfo=datetime.timezone.utc)
            ok = ok and got == int(dt.timestamp() * 1000)
        print(f"{'PASS' if ok else 'FAIL'} {args[:6]} -> {got} (want {args[6]})")
        failed += 0 if ok else 1
    return failed


if __name__ == "__main__":
    sys.exit(main())
