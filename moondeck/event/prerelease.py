#!/usr/bin/env python3
"""Run the release-event gates: "end users will use this".

Usage:
  uv run moondeck/event/prerelease.py                # against the previous tag
  uv run moondeck/event/prerelease.py --base v3.0.0

The release envelope is mostly human judgment — real hardware, release criteria, known
bugs — so most gates here are MANUAL by design. What IS automated is the mechanical
readiness of the tagged tree. Product-owner initiated; never runs itself, never tags.
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _gates import (  # noqa: E402
    ROOT, UV, Gate, changed_files, mechanical_gates, run_gates,
)


def previous_tag():
    """The most recent tag, the natural diff base for a release. Empty when none exists."""
    out = subprocess.run(["git", "describe", "--tags", "--abbrev=0"],
                         cwd=ROOT, capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else ""


def build_gates(firmware):
    """The release gate list.

    Two differences from the other events. The ESP32 firmware is **compiled**, not just
    checked for freshness: this is the one event where the binary itself ships, so minutes
    of build time are cheap against tagging a release whose firmware was never compiled
    from the tagged tree. And the mechanical checks are **untriggered** — a release
    validates the tagged tree as a whole, where "nothing changed under src/ since the last
    tag" is not a reason to skip the tests.
    """
    return mechanical_gates(firmware, esp32="build", triggered=False) + [
        Gate("device-model catalog", UV + ["moondeck/check/check_devices.py"]),
        Gate("firmware list", UV + ["moondeck/check/check_firmwares.py"]),

        Gate("all merge gates passed on the tagged commit", None,
             manual_hint="every PR merged into this tag cleared its own gates"),

        Gate("real-hardware test", None,
             manual_hint="PRODUCT OWNER ONLY: at minimum one ESP32, plus every other "
                         "target this release claims to support"),

        Gate("no known release-blockers", None,
             manual_hint="open issues reviewed; anything flagged blocking is closed or "
                         "downgraded"),

        Gate("per-release criteria done", None,
             manual_hint="every criterion the product owner set for this tag"),

        Gate("release notes drafted", None,
             manual_hint="in the GitHub release body; skip only for a pre-1.0 unreleased tag"),

        Gate("cross-platform smoke", None,
             manual_hint="scenarios on every supported platform — required when the "
                         "release claims new platform support or bumps major/minor"),

        Gate("principles audit", None,
             manual_hint="sweep docs/ (except backlog, history, adr) and src/ for "
                         "forward-looking language and principle violations; the Reviewer "
                         "agent can run this end-to-end"),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default=None,
                        help="Tag or ref to diff against (default: the previous tag).")
    parser.add_argument("--firmware", default="esp32s3-n16r8",
                        help="ESP32 variant to build (CI builds every shipped variant).")
    args = parser.parse_args()

    base = args.base or previous_tag()
    files = changed_files(base=base) if base else []
    label = f"since {base}" if base else "no previous tag"

    sys.exit(run_gates(build_gates(args.firmware), files, f"Release gates ({label})",
                       next_step="The manual gates above are the product owner's call — "
                                 "this script never tags."))


if __name__ == "__main__":
    main()
