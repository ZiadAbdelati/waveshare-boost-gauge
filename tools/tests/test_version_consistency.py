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
      DOWNGRADED the shipped app. Documentation drifted the same way: AGENTS.md
      still claimed v0.9.7 while the tags and release/ were at v0.9.9.

CANONICAL SOURCE
  `version.txt` (repo root): bare MAJOR.MINOR.PATCH, no leading 'v'. The release
  tag is `v` + this value. Nothing else in the tree may hold a version literal.

SOURCE MODE (runs in tools/test_suite.py on every commit)
  1  version.txt is well formed, committed, and not behind the highest git tag
  2  firmware will bake it: nothing shadows it for ESP-IDF (a `set(PROJECT_VER ...)`
     in ANY CMakeLists, `CONFIG_APP_PROJECT_VER*` in any sdkconfig defaults file)
  3  android: versionName == version.txt, versionCode a positive integer
  4  ios: project.yml AND every occurrence in the generated pbxproj agree
  5  the mock server derives its firmwareVersion from version.txt
  6  no stale release literal survives in code, and the canonical version is never a
     contiguous literal in ordinary code (only in build settings). Literals split
     ACROSS lines are not reassembled - a stated residual, since this sweep is a
     secondary guard and the primary is the --release descriptor check
  7  every document that states the current release names this version

RELEASE MODE (--release - the release gate, run before publishing)
  8  every required artifact exists, and no artifact named for another version
     remains in release/ (checked for EVERY artifact, not just the IPA)
  9  the app image embeds esp_app_desc.version == version.txt, and the merged
     image carries the same descriptor at the app partition offset
 10  BOTH iOS packages - the sideload IPA and the devicectl .app zip - report the
     version and build number, and agree with each other
 11  the APK reports the same versionName/versionCode as the Gradle source
 12  build numbers moved: versionCode and CFBundleVersion exceed the values at the
     highest git tag (a duplicate store build number is rejected by the stores)
 13  flash geometry agrees: release/flash_args offsets match partitions.csv, and
     release/flash.sh flashes the same image geometry
 14  SHA256SUMS matches the actual bytes on disk AND covers every shipped file

Every check must fail on a real drift. Two were proven non-vacuous while this was
written: the tracking check failed on the then-untracked version.txt, and bumping
version.txt without rebuilding fails eight checks.

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
PARTITIONS_CSV = REPO_ROOT / "partitions.csv"

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
# A version embedded in a released filename, e.g. BoostGauge-0.9.9-android.apk.
VERSION_IN_NAME = re.compile(r"(?<![\d.])(\d+\.\d+\.\d+)(?![\d.])")

# '#' starts a comment only in these languages. In C a leading '#' introduces a
# preprocessor directive, so treating it as a comment hides a `#define ... "0.9.9"`.
HASH_COMMENT_SUFFIXES = {".py", ".yml", ".yaml"}


def code_lines(path: pathlib.Path, text: str):
    """Yield (lineno, code-only text) with comments removed.

    A line-prefix test is wrong in BOTH directions: `*out = "0.9.9";` is C code
    that merely starts with '*', while `static int x = 1; /* was 0.9.5 */` is code
    whose literal sits inside a comment. This tracks block comments and quoted
    strings so a version literal is judged on what the compiler actually sees.
    """
    hash_comments = path.suffix in HASH_COMMENT_SUFFIXES
    in_block = False
    for lineno, line in enumerate(text.splitlines(), 1):
        code: list[str] = []
        quote: str | None = None
        i = 0
        while i < len(line):
            ch = line[i]
            nxt = line[i + 1] if i + 1 < len(line) else ""
            if in_block:
                if ch == "*" and nxt == "/":
                    in_block = False
                    i += 2
                    continue
                i += 1
                continue
            if quote is not None:
                code.append(ch)
                if ch == "\\":
                    if nxt:
                        code.append(nxt)
                        i += 2
                        continue
                elif ch == quote:
                    quote = None
                i += 1
                continue
            if ch in "\"'":
                quote = ch
                code.append(ch)
                i += 1
                continue
            if hash_comments and ch == "#":
                break
            if ch == "/" and nxt == "/":
                break
            if ch == "/" and nxt == "*":
                in_block = True
                i += 2
                continue
            code.append(ch)
            i += 1
        yield lineno, "".join(code)
# Version literals are legitimate ONLY inside build settings; a literal in ordinary
# code is drift waiting to happen.
VERSION_LITERAL_ALLOWED = {"build.gradle.kts", "project.yml"}
# Simulator test doubles may carry a configurable default, but it must announce
# itself with a '-sim' suffix rather than look like a release number.
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


def git(*args: str) -> subprocess.CompletedProcess | None:
    try:
        return subprocess.run(["git", *args], cwd=str(REPO_ROOT),
                              capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None


def tag_versions() -> list[tuple[int, int, int]]:
    """Every version-shaped git tag as a comparable tuple.

    Matches a leading `vX.Y.Z` (so `v1.0.0-rc1` counts as 1.0.0) and tolerates
    two- or four-component tags.
    """
    proc = git("tag", "--list")
    if proc is None or proc.returncode != 0:
        return []
    out = []
    for line in proc.stdout.splitlines():
        m = re.match(r"v?(\d+)\.(\d+)(?:\.(\d+))?", line.strip())
        if m:
            out.append((int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)))
    return out


def tagged_file(tag_tuple: tuple[int, int, int], relpath: str) -> str | None:
    """The content of `relpath` at the highest tag matching `tag_tuple`."""
    proc = git("tag", "--list")
    if proc is None:
        return None
    name = "v" + ".".join(str(p) for p in tag_tuple)
    candidates = [t.strip() for t in proc.stdout.splitlines()
                  if t.strip().startswith(name)]
    for candidate in sorted(candidates):
        shown = git("show", f"{candidate}:{relpath}")
        if shown is not None and shown.returncode == 0:
            return shown.stdout
    return None


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


def flash_args() -> tuple[dict[str, int], str]:
    """(name -> offset, the flash-parameter line) from release/flash_args."""
    path = RELEASE_DIR / "flash_args"
    if not path.is_file():
        return {}, ""
    offsets: dict[str, int] = {}
    params = ""
    for line in path.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("--"):
            params = stripped
            continue
        m = re.match(r"^(0x[0-9a-fA-F]+)\s+(\S+)\s*$", stripped)
        if m:
            offsets[pathlib.PurePosixPath(m.group(2)).name] = int(m.group(1), 16)
    return offsets, params


def partition_offsets() -> dict[str, int]:
    if not PARTITIONS_CSV.is_file():
        return {}
    out = {}
    for line in PARTITIONS_CSV.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) >= 4:
            try:
                out[parts[0]] = int(parts[3], 16)
            except ValueError:
                continue
    return out


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


def zip_app_plist(path: pathlib.Path) -> dict | None:
    """The Info.plist of the .app inside a zip/ipa, whatever the layout."""
    try:
        with zipfile.ZipFile(path) as archive:
            for name in archive.namelist():
                if name.endswith(".app/Info.plist"):
                    return plistlib.loads(archive.read(name))
    except (zipfile.BadZipFile, OSError, plistlib.InvalidFileException):
        return None
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
    probe = git("rev-parse", "--is-inside-work-tree")
    if probe is not None and probe.returncode == 0 and probe.stdout.strip() == "true":
        tracked = git("ls-files", "--error-unmatch", "version.txt")
        result.check(tracked is not None and tracked.returncode == 0,
                     "version.txt is tracked by git (a fresh clone builds the right version)",
                     (tracked.stderr.strip()[:140] if tracked is not None else "git unavailable"))
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

    # 2 - the firmware must bake version.txt, not something else. CMake command
    # names are case-insensitive and an `include()`d file can set it too, so scan
    # every CMakeLists in the tree case-insensitively.
    offenders = []
    for cmake in sorted(REPO_ROOT.rglob("CMakeLists.txt")):
        if "build" in cmake.parts or "managed_components" in cmake.parts:
            continue
        if re.search(r"(?i)\bset\s*\(\s*PROJECT_VER\b", cmake.read_text(encoding="utf-8", errors="replace")):
            offenders.append(str(cmake.relative_to(REPO_ROOT)))
    result.check(not offenders, "no `set(PROJECT_VER ...)` anywhere in the build tree",
                 f"found in: {offenders}")

    # ESP-IDF auto-loads sdkconfig.defaults AND sdkconfig.defaults.<target>, and
    # honours $SDKCONFIG_DEFAULTS; CONFIG_APP_PROJECT_VER* overrides the version.
    config_files = sorted(REPO_ROOT.glob("sdkconfig.defaults*")) + [REPO_ROOT / "sdkconfig"]
    extra = os.environ.get("SDKCONFIG_DEFAULTS")
    if extra:
        config_files.append(pathlib.Path(extra))
    bad_config = []
    for path in config_files:
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if re.search(r"(?m)^\s*CONFIG_APP_PROJECT_VER", text):
            bad_config.append(path.name)
    result.check(not bad_config, "no CONFIG_APP_PROJECT_VER* override in any sdkconfig source",
                 f"found in: {bad_config}")

    # 3 - android
    gradle_name = gradle_code = None
    if ANDROID_GRADLE.is_file():
        gradle = ANDROID_GRADLE.read_text(encoding="utf-8")
        name_m = re.search(r'versionName\s*=\s*"([^"]+)"', gradle)
        code_m = re.search(r"versionCode\s*=\s*(\d+)", gradle)
        gradle_name = name_m.group(1) if name_m else None
        gradle_code = code_m.group(1) if code_m else None
        result.check(gradle_name == version, "android versionName matches version.txt",
                     f"gradle={gradle_name}")
        result.check(gradle_code is not None and int(gradle_code) > 0,
                     "android versionCode is a positive integer", f"gradle={gradle_code}")
    else:
        result.note("apps/android module not found; android checks skipped")

    # 4 - ios: the XcodeGen spec AND the generated project must agree
    yml_build = None
    if IOS_PROJECT_YML.is_file() and IOS_PBXPROJ.is_file():
        yml = IOS_PROJECT_YML.read_text(encoding="utf-8")
        pbx = IOS_PBXPROJ.read_text(encoding="utf-8")
        yml_mv = re.search(r'MARKETING_VERSION:\s*"([^"]+)"', yml)
        yml_cv = re.search(r"CURRENT_PROJECT_VERSION:\s*(\d+)", yml)
        pbx_mv = [v.strip().strip('"')
                  for v in re.findall(r"MARKETING_VERSION\s*=\s*([^;]+);", pbx)]
        pbx_cv = [v.strip().strip('"')
                  for v in re.findall(r"CURRENT_PROJECT_VERSION\s*=\s*([^;]+);", pbx)]
        yml_build = yml_cv.group(1) if yml_cv else None

        result.check(yml_mv is not None and yml_mv.group(1) == version,
                     "iOS project.yml MARKETING_VERSION matches version.txt",
                     f"project.yml={yml_mv.group(1) if yml_mv else 'missing'}")
        result.check(bool(pbx_mv) and set(pbx_mv) == {version},
                     "every iOS pbxproj MARKETING_VERSION matches version.txt",
                     f"pbxproj={sorted(set(pbx_mv))}")
        result.check(
            yml_mv is not None and bool(pbx_mv) and yml_mv.group(1) == pbx_mv[0],
            "iOS project.yml and generated pbxproj agree (regeneration cannot downgrade)",
            f"project.yml={yml_mv.group(1) if yml_mv else '?'} pbxproj={sorted(set(pbx_mv))}",
        )
        result.check(yml_build is not None and len(set(pbx_cv)) == 1
                     and yml_build == pbx_cv[0],
                     "iOS build number agrees between project.yml and pbxproj",
                     f"project.yml={yml_build} pbxproj={sorted(set(pbx_cv))}")
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

    # 6 - no stale literal, and no hardcoded canonical version in ordinary code
    stale: list[str] = []
    hardcoded: list[str] = []
    self_path = pathlib.Path(__file__).resolve()
    literal_pattern = re.compile(r"(?<![\d.])" + re.escape(version) + r"(?![\d.])")
    for path in sweep_targets():
        if path.resolve() == self_path:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for lineno, line in code_lines(path, text):
            if not line.strip():
                continue
            where = f"{path.relative_to(REPO_ROOT)}:{lineno}: {line.strip()[:90]}"
            if STALE_LITERAL.search(line):
                stale.append(where)
            if path.name in VERSION_LITERAL_ALLOWED:
                continue
            if literal_pattern.search(line):
                hardcoded.append(where)
                continue
            # Adjacent literals concatenate at compile time: `"1.0." "0"` is the
            # same version to the compiler, so compare the de-quoted line too -
            # but with the SAME boundaries, or `31.0.0` matches as a substring.
            if literal_pattern.search(re.sub(r"[\s\"']", "", line)):
                hardcoded.append(where + " (split or concatenated)")
    result.check(not stale, "no superseded release literal left in code",
                 "; ".join(stale[:4]) + (" ..." if len(stale) > 4 else ""))
    result.check(not hardcoded,
                 "the canonical version is never a contiguous literal in ordinary code",
                 "; ".join(hardcoded[:4]) + (" ..." if len(hardcoded) > 4 else ""))

    # The simulator test doubles may carry a default, but it must announce itself.
    for name in SIM_DOUBLE_DIRS:
        base = REPO_ROOT / "tools" / name
        if not base.is_dir():
            continue
        bad = [f"{p.relative_to(REPO_ROOT)}"
               for p in sorted(base.rglob("*.swift"))
               for line in p.read_text(encoding="utf-8", errors="replace").splitlines()
               if STALE_LITERAL.search(line) and "-sim" not in line]
        result.check(not bad, f"tools/{name} version defaults are marked '-sim'",
                     "; ".join(bad[:3]))

    # 7 - the documents that state the current release must state THIS one. The
    # originating incident was exactly this drift (AGENTS.md said v0.9.7 while the
    # tags said v0.9.9), and nothing used to read these files.
    agents = REPO_ROOT / "AGENTS.md"
    if agents.is_file():
        text = agents.read_text(encoding="utf-8")
        result.check(
            re.search(r"Current release is\s*\*\*`v?" + re.escape(version) + r"`\*\*", text)
            is not None,
            "AGENTS.md 'Current release' names this version",
            f"expected `v{version}` in the 'Current release is' line",
        )
    notes = REPO_ROOT / "docs" / "release-notes.md"
    if notes.is_file():
        text = notes.read_text(encoding="utf-8")
        result.check(f"## v{version}" in text,
                     "docs/release-notes.md has a section for this version")
    parity = REPO_ROOT / "apps" / "PARITY.md"
    if parity.is_file():
        result.check(version in parity.read_text(encoding="utf-8"),
                     "apps/PARITY.md names this version")


def check_release(result: Result, version: str, ios_build: str | None) -> None:
    ipa_name = f"BoostGauge-{version}-ios.ipa"
    required = [
        "boost_gauge.bin", "bootloader.bin", "partition-table.bin",
        "ota_data_initial.bin", "boost_gauge_merged.bin",
        "flash.sh", "flash_args", "README.md", "SHA256SUMS",
        "BoostGauge-android-debug.apk", ipa_name, "BoostGauge-ios-app.zip",
    ]
    missing = [n for n in required if not (RELEASE_DIR / n).is_file()]
    result.check(not missing, "release/ contains every required artifact", f"missing: {missing}")

    # Nothing else may sit in release/. An unexpected file (a second APK, a stale
    # .bin) is a download trap, and the checksum coverage check would happily
    # hash it - so the expected set has to be exact.
    unexpected = sorted(p.name for p in RELEASE_DIR.iterdir()
                        if p.is_file() and p.name not in set(required))
    result.check(not unexpected, "release/ contains no unexpected file",
                 f"unexpected: {unexpected}")

    # 8 - ANY artifact named for another version is a download trap, not just the
    # IPA (a leftover BoostGauge-0.9.9-android.apk used to pass silently).
    superseded = []
    for path in sorted(RELEASE_DIR.iterdir()):
        if not path.is_file():
            continue
        found = VERSION_IN_NAME.search(path.name)
        if found and found.group(1) != version:
            superseded.append(path.name)
    result.check(not superseded, "no artifact named for another version in release/",
                 f"found: {superseded}")

    # 9 - the firmware binary is the authority for "the build reflects the version"
    app_bin = RELEASE_DIR / "boost_gauge.bin"
    if app_bin.is_file():
        desc = find_app_desc(app_bin.read_bytes())
        result.check(desc is not None and desc["version"] == version,
                     "shipped boost_gauge.bin embeds the release version",
                     f"app_desc={desc['version'] if desc else 'not found'} want={version}")
        result.check(desc is not None and desc["project_name"] == "boost_gauge",
                     "shipped app image descriptor identifies the project",
                     f"project_name={desc['project_name'] if desc else 'not found'}")

    offsets, params = flash_args()
    merged = RELEASE_DIR / "boost_gauge_merged.bin"
    if merged.is_file() and "boost_gauge.bin" in offsets:
        base = offsets["boost_gauge.bin"]
        desc = find_app_desc(merged.read_bytes(), base=base)
        result.check(desc is not None and desc["version"] == version,
                     "merged flash image carries the same app descriptor",
                     f"app_desc={desc['version'] if desc else 'not found'} at +0x{base:x}")

    # 10 - BOTH iOS packages, checked as shipped bytes. The devicectl zip is a
    # documented install path, so it cannot be exempt from the version check.
    packages = {
        "IPA": RELEASE_DIR / ipa_name,
        "app zip": RELEASE_DIR / "BoostGauge-ios-app.zip",
    }
    plists: dict[str, dict] = {}
    for label, path in packages.items():
        if not path.is_file():
            continue
        info = zip_app_plist(path)
        if info is None:
            result.check(False, f"shipped {label} contains a readable .app/Info.plist")
            continue
        plists[label] = info
        result.check(info.get("CFBundleShortVersionString") == version,
                     f"shipped {label} reports the release version",
                     f"CFBundleShortVersionString={info.get('CFBundleShortVersionString')}")
        if ios_build:
            result.check(str(info.get("CFBundleVersion")) == ios_build,
                         f"shipped {label} reports the matching build number",
                         f"CFBundleVersion={info.get('CFBundleVersion')} want={ios_build}")
    if len(plists) == 2:
        result.check(
            plists["IPA"].get("CFBundleShortVersionString")
            == plists["app zip"].get("CFBundleShortVersionString")
            and plists["IPA"].get("CFBundleVersion") == plists["app zip"].get("CFBundleVersion"),
            "the IPA and the devicectl .app zip are the same build",
            f"ipa={plists['IPA'].get('CFBundleShortVersionString')}"
            f"({plists['IPA'].get('CFBundleVersion')}) "
            f"zip={plists['app zip'].get('CFBundleShortVersionString')}"
            f"({plists['app zip'].get('CFBundleVersion')})",
        )

    # 11 - the APK, read by the Android toolchain itself
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

    # 12 - build numbers must MOVE between releases. Two builds that agree with
    # each other but repeat the previous release's number are rejected by the
    # stores, and the app cannot be updated in place.
    tags = tag_versions()
    if not tags:
        result.check(False, "git tags are visible to the release gate",
                     "the gate cannot verify monotonicity without them (shallow clone?)")
    else:
        current = tuple(int(p) for p in version.split("."))
        # The reference is the newest release BEFORE this one. Comparing against
        # max(tags) skipped the check entirely in the tag-first ordering, where
        # version.txt already equals its own tag.
        earlier = sorted(t for t in tags if t < current)
        if not earlier:
            result.note(f"no release earlier than {version} is tagged; "
                        "build-number monotonicity not checked")
        else:
            reference = earlier[-1]
            reference_str = ".".join(str(p) for p in reference)
            previous_gradle = tagged_file(reference, str(ANDROID_GRADLE.relative_to(REPO_ROOT)))
            previous_yml = tagged_file(reference, str(IOS_PROJECT_YML.relative_to(REPO_ROOT)))
            if previous_gradle and gradle_code:
                prev = re.search(r"versionCode\s*=\s*(\d+)", previous_gradle)
                result.check(prev is not None and int(gradle_code) > int(prev.group(1)),
                             f"android versionCode moved past v{reference_str}",
                             f"now={gradle_code} at v{reference_str}="
                             f"{prev.group(1) if prev else '?'}")
            else:
                result.note(f"versionCode monotonicity not checked (no v{reference_str} gradle)")
            if previous_yml and ios_build:
                prev = re.search(r"CURRENT_PROJECT_VERSION:\s*(\d+)", previous_yml)
                result.check(prev is not None and int(ios_build) > int(prev.group(1)),
                             f"iOS build number moved past v{reference_str}",
                             f"now={ios_build} at v{reference_str}="
                             f"{prev.group(1) if prev else '?'}")
            else:
                result.note(f"iOS build monotonicity not checked "
                            f"(no v{reference_str} project.yml)")

    # 13 - flash geometry: the shipped offsets must match the partition table, or
    # the flasher writes the app over the wrong region.
    if offsets:
        parts = partition_offsets()
        expected = {
            "bootloader.bin": 0x0,
            "partition-table.bin": 0x8000,
            "ota_data_initial.bin": parts.get("otadata"),
            "boost_gauge.bin": parts.get("ota_0"),
        }
        wrong = {name: (offsets.get(name), want)
                 for name, want in expected.items()
                 if want is not None and offsets.get(name) != want}
        result.check(not wrong, "release/flash_args offsets match partitions.csv",
                     f"mismatched: {wrong}")
        flash_sh = RELEASE_DIR / "flash.sh"
        if flash_sh.is_file() and params:
            text = flash_sh.read_text(encoding="utf-8")
            result.check("boost_gauge_merged.bin" in text,
                         "release/flash.sh flashes the merged image")
            # The write offset matters as much as the file name: 0x20000 would
            # flash the merged image over the running app partition.
            offset_m = re.search(
                r"(0x[0-9a-fA-F]+)\s+\"?\$?\{?DIR\}?/?boost_gauge_merged\.bin", text)
            result.check(offset_m is not None and int(offset_m.group(1), 16) == 0x0,
                         "release/flash.sh writes the merged image at offset 0x0",
                         f"found {offset_m.group(1) if offset_m else 'no offset'}")
            for flag in ("flash_mode", "flash_freq", "flash_size"):
                want = re.search(rf"--{flag}\s+(\S+)", params)
                got = re.search(rf"--{flag}\s+(\S+)", text)
                result.check(want is not None and got is not None
                             and want.group(1) == got.group(1),
                             f"release/flash.sh matches flash_args --{flag}",
                             f"flash_args={want.group(1) if want else '?'} "
                             f"flash.sh={got.group(1) if got else '?'}")

    # 14 - checksums must describe the bytes actually in the directory
    sums = RELEASE_DIR / "SHA256SUMS"
    if sums.is_file():
        entries: dict[str, str] = {}
        for line in sums.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            parts = line.split(None, 1)
            if len(parts) == 2:
                entries[parts[1].strip()] = parts[0].strip()
        absent, mismatched = [], []
        for name, digest in entries.items():
            path = RELEASE_DIR / name
            if not path.is_file():
                absent.append(name)
                continue
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                mismatched.append(name)
        result.check(not absent, "every SHA256SUMS entry exists on disk", f"absent: {absent}")
        result.check(not mismatched, "every SHA256SUMS digest matches the file bytes",
                     f"mismatched: {mismatched}")
        shipped = {p.name for p in RELEASE_DIR.iterdir()
                   if p.is_file() and p.name not in {"README.md", "SHA256SUMS"}}
        uncovered = sorted(shipped - set(entries))
        result.check(not uncovered, "SHA256SUMS covers every shipped file",
                     f"uncovered: {uncovered}")

    readme = RELEASE_DIR / "README.md"
    if readme.is_file():
        result.check(f"v{version}" in readme.read_text(encoding="utf-8"),
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

    ios_build = None
    if IOS_PROJECT_YML.is_file():
        m = re.search(r"CURRENT_PROJECT_VERSION:\s*(\d+)",
                      IOS_PROJECT_YML.read_text(encoding="utf-8"))
        ios_build = m.group(1) if m else None

    if args.release:
        print(f"=== version {version} (release artifacts) ===")
        check_release(result, version, ios_build)

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
