#!/usr/bin/env python3
"""Generate src/core/build_info.h.

Writes a single header carrying every compile-time identity fact the runtime
exposes through SystemModule:

  MM_VERSION       — semver. Defaults to library.json (local/dev builds), but
                     the release pipeline overrides it with a -D flag (same
                     mechanism as MM_RELEASE / MM_FIRMWARE_NAME) so a published
                     build carries a precise version: the core semver for a
                     stable release, or a monotonic `<core>-dev.<N>` for a
                     moving `latest` build (see moondeck/build/compute_version.py).
  MM_BUILD_DATE    — __DATE__ " " __TIME__, evaluated by the compiler.
  MM_FIRMWARE_NAME — set by the build system as a -D flag (see
                     moondeck/build/build_esp32.py firmware_cmake_args() and
                     moondeck/build/package_desktop.py). The header carries an
                     #ifndef "unknown" fallback for builds that didn't set it.
                     "Firmware" is the compiled-binary variant; the physical
                     board is a separate concept the device cannot self-identify.
                     See docs/architecture.md § Firmware vs board.
  MM_RELEASE       — release-channel tag (`latest`, `v1.0.0`), set by the
                     release workflow as a -D flag. #ifndef "" fallback for
                     local/dev builds (no channel). MM_VERSION = what code;
                     MM_RELEASE = which channel.

The generator rewrites the whole file from this template each time
library.json changes; the #ifndef defaults below are part of the template,
so they survive regeneration.
"""

import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
LIBRARY_JSON = ROOT / "library.json"
OUT_FILE = ROOT / "src" / "core" / "build_info.h"

data = json.loads(LIBRARY_JSON.read_text())
version = data["version"]


def build_id() -> str:
    """The short git hash the binary was built from, with `+` if the tree was dirty.

    This is the answer to "which code is on this board?" — the question MM_BUILD_DATE
    cannot answer. `__DATE__`/`__TIME__` expand when the *including translation unit*
    compiles, and build_info.h is a header consumed by one TU: change a driver .cpp and
    that TU is NOT rebuilt, so the reported date FREEZES while the firmware moves on. A
    stale date reads as "the flash didn't take" and sends you debugging the wrong binary
    (measured the hard way, 2026-07-16). The hash comes from git at generate time and is
    regenerated on every build (the CMake rule is ALWAYS out-of-date by design), so it
    tracks the source, not a compile timestamp.

    Falls back to "nogit" for a tarball / no-git build — never fails the build.
    """
    def git(*args: str) -> str:
        return subprocess.run(("git", "-C", str(ROOT), *args),
                              capture_output=True, text=True, check=True).stdout.strip()
    try:
        h = git("rev-parse", "--short=8", "HEAD")
        dirty = "+" if git("status", "--porcelain") else ""
        return f"{h}{dirty}"
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        return "nogit"


build = build_id()

content = f'''#pragma once

// Auto-generated from library.json by moondeck/build/generate_build_info.py
// -- do not edit by hand. Regenerated when library.json changes.

// MM_VERSION defaults to the in-tree library.json semver, but the release
// pipeline overrides it with -DMM_VERSION="<computed>" (compute_version.py):
// the core semver for a stable tag, or `<core>-dev.<N>` for a moving `latest`
// build so successive latest builds are orderable. #ifndef so a local build
// needs no flag.
#ifndef MM_VERSION
#define MM_VERSION    "{version}"
#endif

// MM_BUILD_DATE — when the TU that includes this header was compiled. **Do NOT use it to
// tell which code is on a board.** __DATE__/__TIME__ expand at the *including* TU's
// compile, and only that TU's own dependencies trigger a rebuild — edit a driver .cpp and
// this date does not move, so a freshly flashed board still reports the OLD timestamp.
// Trusting it as a firmware-identity signal misleads: two different builds can carry the same date.
// Use MM_BUILD_ID for identity; this is a human-readable "roughly when" only.
#define MM_BUILD_DATE __DATE__ " " __TIME__

// MM_BUILD_ID — the short git hash this binary was built from, `+`-suffixed when the tree
// was dirty ("a1b2c3d4+"), or "nogit" without a git checkout. THIS is the firmware-identity
// signal: it is regenerated from git on every build (the CMake rule is deliberately always
// out-of-date), so it names the SOURCE rather than a compile timestamp. Read it off a
// running device to answer "did my flash land, and with what?" — the question that must be
// answerable before any bench measurement can be trusted.
#ifndef MM_BUILD_ID
#define MM_BUILD_ID   "{build}"
#endif

// Compile-time identity from build flags. The build script that knows the
// value passes it as a -D, and SystemModule surfaces it on the device card
// (and the OTA path reads it to pick a matching release asset).
//
// "Firmware" here is the compiled-binary variant (esp32 / esp32-eth /
// esp32-16mb / esp32s3-n16r8) — see docs/architecture.md § Firmware
// vs board. The physical hardware ("board") is a separate concept the
// device cannot identify on its own.
//
//   ESP32:   moondeck/build/build_esp32.py firmware_cmake_args() -> -DMM_FIRMWARE_NAME="<key>"
//   Desktop: moondeck/build/package_desktop.py for release builds; local
//            CMake builds fall through to "unknown" today (no harm:
//            local builds aren't published).
#ifndef MM_FIRMWARE_NAME
#define MM_FIRMWARE_NAME "unknown"
#endif

// MM_RELEASE — the release-channel tag this binary was published under
// (`latest`, `v1.0.0`, `v1.0.0-rc2`). Set by the release workflow as a -D
// flag (same mechanism as MM_FIRMWARE_NAME). MM_VERSION is the semver from
// library.json — what code this is; MM_RELEASE is which channel it shipped
// on — a moving `latest` build and a tagged release can share a semver but
// differ in channel. Empty default: a local / dev build has no channel, and
// SystemModule shows just the bare semver in that case.
#ifndef MM_RELEASE
#define MM_RELEASE ""
#endif

namespace mm {{

constexpr const char* kVersion      = MM_VERSION;
constexpr const char* kBuildDate    = MM_BUILD_DATE;
constexpr const char* kBuildId      = MM_BUILD_ID;
constexpr const char* kFirmwareName = MM_FIRMWARE_NAME;
constexpr const char* kRelease      = MM_RELEASE;

}} // namespace mm
'''

# Force UTF-8 on both read and write — Python's default on Windows is cp1252,
# which can't encode anything outside ASCII. Even though the template above is
# ASCII today, pinning the encoding makes the script robust if a future edit
# slips a non-ASCII character into the comments.
if OUT_FILE.exists() and OUT_FILE.read_text(encoding="utf-8") == content:
    pass  # only write if changed (avoid unnecessary rebuilds)
else:
    OUT_FILE.write_text(content, encoding="utf-8")
    print(f"Generated build_info.h: version={version}")
