#!/usr/bin/env python3
"""Version consistency guard - one canonical version, enforced on every surface.

WHY THIS EXISTS
  The v0.9.x line shipped releases whose artifacts disagreed with their own tag.
  Two independent mechanisms caused it, and this test closes both:

    * The firmware version was derived from `git describe` at BUILD time
      (CMakeLists.txt declared no PROJECT_VER and sdkconfig left
      CONFIG_APP_PROJECT_VER_FROM_CONFIG unset), so a binary built one commit
      after the tag reported `v0.9.6-3-gee90519` in a release named v0.9.7. It is
      now a build INPUT: repo-root `version.txt`, which ESP-IDF reads in
      project.cmake (`__project_get_revision_from_version_file`).
    * The native app versions were hand-edited literals in several files that had
      drifted apart - the iOS XcodeGen spec said 0.9.2 while its own generated
      pbxproj said 0.9.7, so regenerating the project would have silently
      DOWNGRADED the shipped app.

CANONICAL SOURCE
  `version.txt` (repo root): bare MAJOR.MINOR.PATCH, no leading 'v'. The release
  tag is `v` + this value. Nothing else in the tree may hold a version literal.

SOURCE MODE (runs in tools/test_suite.py on every commit)
  1  version.txt is well formed and is not behind the highest git tag
  2  firmware will bake it: version.txt present, nothing shadows it
     (no PROJECT_VER override, CONFIG_APP_PROJECT_VER_FROM_CONFIG off)
  3  android: versionName == version.txt, versionCode a positive integer
  4  ios: project.yml AND every occurrence in the generated pbxproj agree
  5  the mock server derives its firmwareVersion from version.txt
  6  no stale release literal survives anywhere in code

RELEASE MODE (--release - the release gate, run before publishing)
  7  every required artifact exists, and no superseded versioned artifact remains
  8  the app image embeds esp_app_desc.version == version.txt, and the merged
     image carries the same descriptor at the app partition offset
  9  the IPA's Payload/*.app/Info.plist reports the version and build number
 10  the APK reports the same versionName/versionCode as the Gradle source
 11  SHA256SUMS matches the actual bytes on disk AND covers every shipped file

Stdlib only. Run:
    python3 tools/tests/test_version_consistency.py
    python3 tools/tests/test_version_consistency.py --release
"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import plistlib
import re
import struct
import subprocess
import sys
import zipfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
VERSION_FILE = REPO_ROOT / "version.txt"
RELEASE_DIR = REPO_ROOT / "release"
ANDROID_GRADLE = REPO_ROOT / "apps" / "android" / "BoostGauge" / "app" / "build.gradle.kts"
IOS_PROJECT_YML = REPO_ROOT / "apps" / "ios" / "BoostGauge" / "project.yml"
IOS_PBXPROJ = (
    REPO_ROOT / "apps" / "ios" / "BoostGauge" / "BoostGauge.xcodeproj" / "project.pbxproj"
)
MOCK_SERVER = REPO_ROOT / "tools" / "mock_server.py"
ROOT_CMAKE = REPO_ROOT / "CMakeLists.txt"
SDKCONFIG_FILES = (REPO_ROOT / "sdkconfig.defaults", REPO_ROOT / "sdkconfig")

SEMVER = re.compile(r"^\d+\.\d+\.\d+$")
APP_DESC_MAGIC = 0xABCD5432
# esp_app_desc_t: magic(4) secure_version(4) reserv1[2](8) version[32] project_name[32]
DESC_VERSION_OFF = 16
DESC_PROJECT_OFF = 48

# A version literal from the superseded release line. Any hit in code is drift:
# the release number must come from version.txt or a build setting, never a literal.
# No leading \b: it would fail inside `"v0.9.5-sim"` (v and 0 are both word
# characters), which is exactly the literal that survived the last cleanup.
STALE_LITERAL = re.compile(r"(?<![\d.])0\.9\.\d+(?![\d.])")
# Directories holding deliberate SIMULATOR TEST DOUBLES whose default firmware
# string is configurable and explicitly marked '-sim' (asserted below). They are
# not release surfaces, so they are exempt from the stale-literal sweep.
SIM_DOUBLE_DIRS = ("ble_gauge_sim", "ble_central_test")


class Result:
    def __init__(self) -> None:
        self.checks = 0
        self.failures: list[str] = []
        self.notes: list[str] = []

    def check(self, ok: bool, label: str, detail: str = "") -> None:
        self.checks += 1
        if ok:
            print(f"PASS {label}")
        else:
            self.failures.append(label)
            print(f"FAIL {label}  [{detail if detail else 'assertion failed'}]")

    def note(self, text: str) -> None:
        self.notes.append(text)
        print(f"NOTE {text}")


def read_version() -> str:
    return VERSION_FILE.read_text(encoding="utf-8").strip()


def tag_versions() -> list[tuple[int, int, int]]:
    """Every version-shaped git tag, as comparable tuples."""
    try:
        proc = subprocess.run(
            ["git", "tag", "--list"],
            cwd=str(REPO_ROOT), capture_output=True, text=True, timeout=20,
        )
    except (OSError, subprocess.SubprocessError):
        return []
    if proc.returncode != 0:
        return []
    out = []
    for line in proc.stdout.splitlines():
        m = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", line.strip())
        if m:
            out.append(tuple(int(g) for g in m.groups()))  # type: ignore[arg-type]
    return out


def find_app_desc(data: bytes, base: int = 0, window: int = 0x1000) -> dict | None:
    """Locate the ESP-IDF app descriptor near `base` and decode it."""
    magic = struct.pack("<I", APP_DESC_MAGIC)
    idx = data.find(magic, base, min(len(data), base + window))
    if idx < 0:
        return None
    version = data[idx + DESC_VERSION_OFF: idx + DESC_VERSION_OFF + 32]
    project = data[idx + DESC_PROJECT_OFF: idx + DESC_PROJECT_OFF + 32]
    return {
        "offset": idx,
        "version": version.split(b"\0", 1)[0].decode("utf-8", "replace"),
        "project_name": project.split(b"\0", 1)[0].decode("utf-8", "replace"),
    }


def flash_arg_offsets() -> dict[str, int]:
    """name -> partition offset, from release/flash_args (what the flasher uses)."""
    path = RELEASE_DIR / "flash_args"
    if not path.is_file():
        return {}
    offsets = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^(0x[0-9a-fA-F]+)\s+(\S+)\s*$", line.strip())
        if m:
            offsets[pathlib.PurePosixPath(m.group(2)).name] = int(m.group(1), 16)
    return offsets


def find_aapt2() -> pathlib.Path | None:
    home = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    candidates = []
    if home:
        candidates.append(pathlib.Path(home))
    candidates.append(pathlib.Path.home() / "Library" / "Android" / "sdk")
    for root in candidates:
        build_tools = root / "build-tools"
        if not build_tools.is_dir():
            continue
        for tool_dir in sorted(build_tools.iterdir(), reverse=True):
            aapt2 = tool_dir / "aapt2"
            if aapt2.is_file():
                return aapt2
    return None


def sweep_targets() -> list[pathlib.Path]:
    globs = [
        "main/*.c", "main/*.h",
        "sim/*.c", "sim/*.h",
        "apps/android/**/*.kt", "apps/android/**/*.kts",
        "apps/ios/**/*.swift", "apps/ios/**/*.yml",
        "tools/*.py", "tools/tests/*.py",
    ]
    out: list[pathlib.Path] = []
    for pattern in globs:
        out.extend(sorted(REPO_ROOT.glob(pattern)))
    return out


def check_sources(result: Result, version: str) -> None:
    # 1 - the canonical file itself
    result.check(VERSION_FILE.is_file(), "version.txt exists at the repo root")
    result.check(bool(SEMVER.match(version)), "version.txt is bare MAJOR.MINOR.PATCH",
                 f"got {version!r}")

    # version.txt must be COMMITTED. An untracked copy means a fresh clone has no
    # version file, ESP-IDF falls back to `git describe`, and the build silently
    # reports something that is not the release - the original v0.9.7 failure.
    try:
        probe = subprocess.run(["git", "rev-parse", "--is-inside-work-tree"],
                               cwd=str(REPO_ROOT), capture_output=True, text=True,
                               timeout=20)
    except (OSError, subprocess.SubprocessError):
        probe = None
    if probe is not None and probe.returncode == 0 and probe.stdout.strip() == "true":
        try:
            tracked = subprocess.run(["git", "ls-files", "--error-unmatch", "version.txt"],
                                     cwd=str(REPO_ROOT), capture_output=True, text=True,
                                     timeout=20)
            result.check(tracked.returncode == 0,
                         "version.txt is tracked by git (a fresh clone builds the right version)",
                         tracked.stderr.strip()[:140])
        except (OSError, subprocess.SubprocessError) as error:
            result.check(False, "version.txt tracking could be checked", str(error)[:140])
    else:
        result.note("not a git worktree; version.txt tracking not checked")

    tags = tag_versions()
    if tags:
        highest = max(tags)
        result.check(
            tuple(int(p) for p in version.split(".")) >= highest,
            "version is not behind the highest git tag",
            f"version.txt={version} highest tag={'.'.join(map(str, highest))}",
        )
    else:
        result.note("no version-shaped git tags found; monotonicity not checked")

    # 2 - the firmware must bake version.txt, not `git describe`
    result.check("set(PROJECT_VER" not in ROOT_CMAKE.read_text(encoding="utf-8").replace(" ", ""),
                 "no PROJECT_VER override in the top-level CMakeLists.txt")
    for path in SDKCONFIG_FILES:
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        result.check(
            "CONFIG_APP_PROJECT_VER_FROM_CONFIG=y" not in text,
            f"{path.name} does not override the project version from config",
        )

    # 3 - android
    if ANDROID_GRADLE.is_file():
        gradle = ANDROID_GRADLE.read_text(encoding="utf-8")
        name_m = re.search(r'versionName\s*=\s*"([^"]+)"', gradle)
        code_m = re.search(r"versionCode\s*=\s*(\d+)", gradle)
        result.check(name_m is not None and name_m.group(1) == version,
                     "android versionName matches version.txt",
                     f"gradle={name_m.group(1) if name_m else 'missing'}")
        result.check(code_m is not None and int(code_m.group(1)) > 0,
                     "android versionCode is a positive integer",
                     f"gradle={code_m.group(1) if code_m else 'missing'}")
    else:
        result.note("apps/android module not found; android checks skipped")

    # 4 - ios: the XcodeGen spec AND the generated project must agree
    if IOS_PROJECT_YML.is_file() and IOS_PBXPROJ.is_file():
        yml = IOS_PROJECT_YML.read_text(encoding="utf-8")
        pbx = IOS_PBXPROJ.read_text(encoding="utf-8")
        yml_mv = re.search(r'MARKETING_VERSION:\s*"([^"]+)"', yml)
        yml_cv = re.search(r"CURRENT_PROJECT_VERSION:\s*(\d+)", yml)
        pbx_mv = re.findall(r"MARKETING_VERSION\s*=\s*([^;]+);", pbx)
        pbx_cv = re.findall(r"CURRENT_PROJECT_VERSION\s*=\s*([^;]+);", pbx)
        pbx_mv = [v.strip().strip('"') for v in pbx_mv]
        pbx_cv = [v.strip().strip('"') for v in pbx_cv]

        result.check(yml_mv is not None and yml_mv.group(1) == version,
                     "iOS project.yml MARKETING_VERSION matches version.txt",
                     f"project.yml={yml_mv.group(1) if yml_mv else 'missing'}")
        result.check(pbx_mv and set(pbx_mv) == {version},
                     "every iOS pbxproj MARKETING_VERSION matches version.txt",
                     f"pbxproj={sorted(set(pbx_mv))}")
        result.check(
            yml_mv is not None and pbx_mv and yml_mv.group(1) == pbx_mv[0],
            "iOS project.yml and generated pbxproj agree (regeneration cannot downgrade)",
            f"project.yml={yml_mv.group(1) if yml_mv else '?'} pbxproj={sorted(set(pbx_mv))}",
        )
        result.check(yml_cv is not None and len(set(pbx_cv)) == 1
                     and yml_cv.group(1) == pbx_cv[0],
                     "iOS build number agrees between project.yml and pbxproj",
                     f"project.yml={yml_cv.group(1) if yml_cv else '?'} "
                     f"pbxproj={sorted(set(pbx_cv))}")
    else:
        result.note("iOS project not found; ios checks skipped")

    # 5 - the mock must not pin a release identity of its own
    if MOCK_SERVER.is_file():
        mock = MOCK_SERVER.read_text(encoding="utf-8")
        result.check(
            "version.txt" in mock and not re.search(
                r'"firmwareVersion"\s*:\s*"v?\d+\.\d+', mock),
            "mock server derives firmwareVersion from version.txt",
        )

    # 6 - no stale literal may survive in code
    stale: list[str] = []
    self_path = pathlib.Path(__file__).resolve()
    for path in sweep_targets():
        if path.resolve() == self_path:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for lineno, line in enumerate(text.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith(("#", "//", "*", "/*")):
                continue  # a comment may discuss history
            if STALE_LITERAL.search(line):
                stale.append(f"{path.relative_to(REPO_ROOT)}:{lineno}: {stripped[:90]}")
    result.check(not stale, "no superseded release literal left in code",
                 "; ".join(stale[:4]) + (" ..." if len(stale) > 4 else ""))

    # The simulator test doubles may carry a default, but it must announce itself.
    for name in SIM_DOUBLE_DIRS:
        base = REPO_ROOT / "tools" / name
        if not base.is_dir():
            continue
        offenders = [
            f"{p.relative_to(REPO_ROOT)}"
            for p in sorted(base.rglob("*.swift"))
            for line in p.read_text(encoding="utf-8", errors="replace").splitlines()
            if STALE_LITERAL.search(line) and "-sim" not in line
        ]
        result.check(not offenders, f"tools/{name} version defaults are marked '-sim'",
                     "; ".join(offenders[:3]))


def check_release(result: Result, version: str) -> None:
    ipa_name = f"BoostGauge-{version}-ios.ipa"
    required = [
        "boost_gauge.bin", "bootloader.bin", "partition-table.bin",
        "ota_data_initial.bin", "boost_gauge_merged.bin",
        "flash.sh", "flash_args", "README.md", "SHA256SUMS",
        "BoostGauge-android-debug.apk", ipa_name, "BoostGauge-ios-app.zip",
    ]
    missing = [n for n in required if not (RELEASE_DIR / n).is_file()]
    result.check(not missing, "release/ contains every required artifact",
                 f"missing: {missing}")

    superseded = [
        p.name for p in RELEASE_DIR.glob("BoostGauge-*-ios.ipa") if p.name != ipa_name
    ]
    result.check(not superseded, "no superseded versioned artifact left in release/",
                 f"found: {superseded}")

    # 8 - the firmware binary is the authority for "the build reflects the version"
    app_bin = RELEASE_DIR / "boost_gauge.bin"
    if app_bin.is_file():
        desc = find_app_desc(app_bin.read_bytes())
        result.check(
            desc is not None and desc["version"] == version,
            "shipped boost_gauge.bin embeds the release version",
            f"app_desc={desc['version'] if desc else 'not found'} want={version}",
        )
        result.check(desc is not None and desc["project_name"] == "boost_gauge",
                     "shipped app image descriptor identifies the project",
                     f"project_name={desc['project_name'] if desc else 'not found'}")

    merged = RELEASE_DIR / "boost_gauge_merged.bin"
    offsets = flash_arg_offsets()
    if merged.is_file() and "boost_gauge.bin" in offsets:
        base = offsets["boost_gauge.bin"]
        desc = find_app_desc(merged.read_bytes(), base=base)
        result.check(
            desc is not None and desc["version"] == version,
            "merged flash image carries the same app descriptor",
            f"app_desc={desc['version'] if desc else 'not found'} at +0x{base:x}",
        )

    # 9 - the IPA is checked as shipped bytes, not as source
    ipa = RELEASE_DIR / ipa_name
    if ipa.is_file():
        try:
            with zipfile.ZipFile(ipa) as archive:
                plists = [n for n in archive.namelist()
                          if n.startswith("Payload/") and n.endswith(".app/Info.plist")]
                if not plists:
                    result.check(False, "IPA contains Payload/*.app/Info.plist")
                else:
                    info = plistlib.loads(archive.read(plists[0]))
                    result.check(
                        info.get("CFBundleShortVersionString") == version,
                        "shipped IPA reports the release version",
                        f"CFBundleShortVersionString={info.get('CFBundleShortVersionString')}",
                    )
                    yml = IOS_PROJECT_YML.read_text(encoding="utf-8")
                    build_m = re.search(r"CURRENT_PROJECT_VERSION:\s*(\d+)", yml)
                    if build_m:
                        result.check(
                            str(info.get("CFBundleVersion")) == build_m.group(1),
                            "shipped IPA reports the matching build number",
                            f"CFBundleVersion={info.get('CFBundleVersion')} "
                            f"want={build_m.group(1)}",
                        )
        except (zipfile.BadZipFile, OSError) as error:
            result.check(False, "IPA is readable", str(error))

    # 10 - the APK, read by the Android toolchain itself
    apk = RELEASE_DIR / "BoostGauge-android-debug.apk"
    gradle_name = gradle_code = None
    if ANDROID_GRADLE.is_file():
        gradle = ANDROID_GRADLE.read_text(encoding="utf-8")
        m = re.search(r'versionName\s*=\s*"([^"]+)"', gradle)
        c = re.search(r"versionCode\s*=\s*(\d+)", gradle)
        gradle_name = m.group(1) if m else None
        gradle_code = c.group(1) if c else None
    if apk.is_file():
        aapt2 = find_aapt2()
        if aapt2 is None:
            result.check(False, "aapt2 available to verify the shipped APK",
                         "set ANDROID_HOME or install the Android SDK build-tools")
        else:
            proc = subprocess.run([str(aapt2), "dump", "badging", str(apk)],
                                  capture_output=True, text=True, timeout=120)
            m = re.search(r"versionCode='(\d+)' versionName='([^']*)'", proc.stdout)
            if m:
                result.check(m.group(2) == gradle_name,
                             "shipped APK reports the release versionName",
                             f"apk={m.group(2)} gradle={gradle_name}")
                result.check(m.group(1) == gradle_code,
                             "shipped APK reports the same versionCode as gradle",
                             f"apk={m.group(1)} gradle={gradle_code}")
            else:
                result.check(False, "aapt2 could parse the shipped APK",
                             (proc.stderr or proc.stdout)[:200])

    # 11 - checksums must describe the bytes actually in the directory
    sums = RELEASE_DIR / "SHA256SUMS"
    if sums.is_file():
        entries: dict[str, str] = {}
        for line in sums.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            parts = line.split(None, 1)
            if len(parts) == 2:
                entries[parts[1].strip()] = parts[0].strip()
        mismatched, absent = [], []
        for name, digest in entries.items():
            path = RELEASE_DIR / name
            if not path.is_file():
                absent.append(name)
                continue
            actual = hashlib.sha256(path.read_bytes()).hexdigest()
            if actual != digest:
                mismatched.append(name)
        result.check(not absent, "every SHA256SUMS entry exists on disk",
                     f"absent: {absent}")
        result.check(not mismatched, "every SHA256SUMS digest matches the file bytes",
                     f"mismatched: {mismatched}")

        shipped = {
            p.name for p in RELEASE_DIR.iterdir()
            if p.is_file() and p.name not in {"README.md", "SHA256SUMS"}
        }
        uncovered = sorted(shipped - set(entries))
        result.check(not uncovered, "SHA256SUMS covers every shipped file",
                     f"uncovered: {uncovered}")

    readme = RELEASE_DIR / "README.md"
    if readme.is_file():
        result.check(version in readme.read_text(encoding="utf-8"),
                     "release/README.md names the release version")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", action="store_true",
                        help="also verify the shipped release/ artifacts (release gate)")
    args = parser.parse_args(argv)

    if not VERSION_FILE.is_file():
        print(f"FAIL version.txt missing at {VERSION_FILE}")
        return 1
    version = read_version()

    result = Result()
    print(f"=== version {version} (source surfaces) ===")
    check_sources(result, version)
    if args.release:
        print(f"=== version {version} (release artifacts) ===")
        check_release(result, version)

    for note in result.notes:
        print(f"NOTE {note}")
    if result.failures:
        print(f"\n{len(result.failures)}/{result.checks} checks FAILED:")
        for failure in result.failures:
            print(f"  - {failure}")
        return 1
    print(f"\nPASS ({result.checks} checks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
