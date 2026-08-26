#!/usr/bin/env python3
"""Run the commit-event gates: "this snapshot is internally consistent".

Usage:
  uv run moondeck/event/precommit.py                 # every applicable gate
  uv run moondeck/event/precommit.py --build-esp32   # compile the firmware for real
  uv run moondeck/event/precommit.py --firmware esp32s3-n16r8

Each gate states its own trigger, so a docs-only change runs the spec check and nothing
else, while a `src/` change runs the full set. Product-owner initiated (see CLAUDE.md
§ The Process); this script never runs itself and never commits.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _gates import (  # noqa: E402
    UV, Gate, changed_files, mechanical_gates, run_gates, touches,
)


def build_gates(firmware, full_esp32=False):
    """The commit gate list: the shared mechanical checks, plus the ones only a commit runs.

    ESP32 defaults to a freshness check rather than a compile — the binary being newer
    than every source costs a tenth of a second and catches the case that matters (an
    edit that was never compiled), whereas a cold `idf.py build` costs minutes, and a
    gate list too slow to run is one that stops being run. `--build-esp32` forces the
    real compile for the moments it is worth waiting for (after an sdkconfig or toolchain
    change); CI builds every variant on every PR regardless.
    """
    return mechanical_gates(firmware, esp32="build" if full_esp32 else "freshness") + [
        # --no-live-capture keeps this a few seconds instead of ~80: a live ESP32 tick
        # reading opens the serial port for 15 s and needs a bench board attached, which
        # makes the gate's cost unpredictable. Run collect_kpi.py without the flag when
        # composing the commit message, where the fresh reading is the point.
        # Triggers on everything the repo-health snapshot measures, not just `src/`: a
        # moondeck/ or docs-only commit still moves lines of code, comment density and the
        # docs inventory, and a snapshot that skipped those commits would drift out of
        # step with the tree it claims to describe.
        Gate("KPI collection",
             UV + ["moondeck/check/collect_kpi.py", "--commit", "--no-live-capture"],
             lambda f: touches(f, "src/", "test/", "moondeck/", "docs/", "CLAUDE.md")),

        Gate("device-model catalog",
             UV + ["moondeck/check/check_devices.py"],
             lambda f: touches(f, "mooninstaller/deviceModels.json",
                               "moondeck/check/check_devices.py")),

        Gate("firmware list",
             UV + ["moondeck/check/check_firmwares.py"],
             lambda f: touches(f, "moondeck/build/build_esp32.py",
                               "mooninstaller/firmwares.json",
                               "moondeck/check/check_firmwares.py")),

        # The cross-language contracts ctest cannot reach: the Improv frame wire format
        # and the WLED /json shape. Deps ride in each test file's PEP-723 block.
        Gate("host tests (Python)",
             UV + ["--with", "pytest", "--with", "pyserial", "--with", "markdown",
                   "--with", "wled", "pytest", "test/python", "-q"],
             lambda f: touches(f, "moondeck/", "test/python/")),

        Gate("host tests (JS)",
             ["node", "--test", "test/js/**/*.test.mjs"],
             lambda f: touches(f, "mooninstaller/", "test/js/", "src/ui/")),

        # Needs a board plugged in, so it is recommended rather than blocking. Its trigger
        # is the provisioning path it covers; run_gates drops it from the report entirely
        # when the change doesn't touch that, so the manual list stays honest.
        Gate("Improv smoke test", None,
             lambda f: touches(f, "src/core/ImprovFrame.h",
                               "src/platform/esp32/platform_esp32_improv.cpp",
                               "mooninstaller/index.html", "src/ui/install-picker.js",
                               "moondeck/build/improv_"),
             manual_hint="recommended with an ESP32 connected: "
                         "uv run moondeck/build/improv_smoke_test.py --port <port>"),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--firmware", default="esp32s3-n16r8",
                        help="ESP32 variant the firmware gate checks "
                             "(the local gate covers one variant; CI covers all).")
    parser.add_argument("--build-esp32", action="store_true",
                        help="Compile the ESP32 firmware instead of only checking that the "
                             "existing binary is newer than every source. Minutes rather "
                             "than a second; worth it before a release or after an "
                             "sdkconfig / toolchain change.")
    args = parser.parse_args()

    sys.exit(run_gates(build_gates(args.firmware, full_esp32=args.build_esp32),
                       changed_files(), "Commit gates",
                       next_step="Waiting for the product owner to say \"commit now\" — "
                                 "this script never commits."))


if __name__ == "__main__":
    main()
