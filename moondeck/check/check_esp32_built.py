#!/usr/bin/env python3
"""Check that a firmware binary exists and is newer than the sources that go into it.

The full `idf.py build` is the strongest check, but on a cold build directory it costs
minutes — enough that the gate list stops being run, which is worse than a weaker check
run often. This is the cheap alternative: it does not compile, it asks whether the binary
on disk was built *after* every source file that feeds it.

Freshness is measured against the SOURCES, not the clock. A wall-clock rule ("built within
the last hour") passes a binary that predates an edit made twenty minutes ago — precisely
the stale-artifact trap that costs a debugging session chasing a fix that was never in the
running image. Comparing timestamps answers the question the gate actually means: does this
binary include what is on disk right now?

Usage:
  uv run moondeck/check/check_esp32_built.py --firmware esp32s3-n16r8
  uv run moondeck/check/check_esp32_built.py --firmware esp32 --max-age-hours 24

Exit codes: 0 fresh · 1 missing or stale (the message says which, and what to run).
"""

import argparse
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# What feeds an ESP32 image. Kept in step with the gate's own trigger in
# moondeck/event/precommit.py — both answer "could this change alter the firmware?".
SOURCE_DIRS = ("src", "esp32")
SOURCE_FILES = ("CMakeLists.txt", "library.json")
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".cmake", ".json", ".txt", ".py", ".js",
                   ".html", ".css", ".defaults"}

# Build outputs and caches live under the source dirs; they are products, not inputs, and
# including them would compare the binary against itself.
SKIP_PARTS = {"build", "managed_components", "__pycache__", ".git"}

# Generated headers that sit in the source tree but are build PRODUCTS (both gitignored).
# A desktop build regenerates them after an ESP32 build, so treating them as inputs makes
# the firmware look stale forever, one minute after it was built. What actually feeds them
# — src/ui/*.js, library.json, the git hash — is already covered by the scan.
SKIP_FILES = {"src/ui/ui_embedded.h", "src/core/build_info.h"}

# The desktop-only platform never compiles into an ESP32 image, so an edit there cannot
# stale the firmware. Kept in step with the ESP32 gate's own trigger in precommit.py,
# which excludes the same path.
SKIP_PREFIXES = ("src/platform/desktop/",)


def newest_source():
    """The most recently modified source file that feeds a firmware image."""
    newest_path, newest_mtime = None, 0.0

    for rel in SOURCE_DIRS:
        base = ROOT / rel
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            rel_posix = path.relative_to(ROOT).as_posix()
            if SKIP_PARTS.intersection(path.relative_to(ROOT).parts):
                continue
            if rel_posix in SKIP_FILES or rel_posix.startswith(SKIP_PREFIXES):
                continue
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            mtime = path.stat().st_mtime
            if mtime > newest_mtime:
                newest_path, newest_mtime = path, mtime

    for rel in SOURCE_FILES:
        path = ROOT / rel
        if path.exists() and path.stat().st_mtime > newest_mtime:
            newest_path, newest_mtime = path, path.stat().st_mtime

    return newest_path, newest_mtime


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--firmware", required=True,
                        help="Firmware variant, e.g. esp32s3-n16r8 (its build dir is "
                             "build/esp32-<firmware>/).")
    parser.add_argument("--max-age-hours", type=float, default=0,
                        help="Also fail if the binary is older than this many hours, even "
                             "when no source is newer. 0 (default) disables the age rule — "
                             "source freshness is the check that matters.")
    args = parser.parse_args()

    binary = ROOT / "build" / f"esp32-{args.firmware}" / "projectMM.bin"
    build_cmd = f"uv run moondeck/build/build_esp32.py --firmware {args.firmware}"

    if not binary.exists():
        print(f"No firmware binary for {args.firmware}.")
        print(f"  expected: {binary.relative_to(ROOT)}")
        print(f"  build it: {build_cmd}")
        return 1

    built = binary.stat().st_mtime
    newest_path, newest_mtime = newest_source()
    age_h = (time.time() - built) / 3600

    if newest_path is not None and newest_mtime > built:
        stale_by = (newest_mtime - built) / 60
        print(f"Firmware for {args.firmware} is STALE: a source file is newer than the binary.")
        print(f"  binary : {binary.relative_to(ROOT)} (built {age_h:.1f}h ago)")
        print(f"  newer  : {newest_path.relative_to(ROOT)} ({stale_by:.0f} min after the build)")
        print(f"  rebuild: {build_cmd}")
        return 1

    if args.max_age_hours and age_h > args.max_age_hours:
        print(f"Firmware for {args.firmware} is older than {args.max_age_hours}h "
              f"({age_h:.1f}h) — no source is newer, but the age rule asks for a rebuild.")
        print(f"  rebuild: {build_cmd}")
        return 1

    print(f"Firmware for {args.firmware} is up to date "
          f"(built {age_h:.1f}h ago, newer than every source).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
