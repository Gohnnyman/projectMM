#!/usr/bin/env python3
"""Run the merge-event gates: "this is now trunk".

Usage:
  uv run moondeck/event/premerge.py                  # against main
  uv run moondeck/event/premerge.py --base other-branch

Scope is the whole branch diff (merge-base to HEAD), because architectural drift is
visible across N commits in a way one commit hides. The judgment gates (Reviewer agent,
external review, lessons, PR description) are MANUAL by construction — an agent reports,
the product owner decides. Product-owner initiated; never runs itself, never merges.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _gates import (  # noqa: E402
    Gate, changed_files, mechanical_gates, run_gates, touches,
)

# Code that runs in the tick path: a change here means the perf snapshot is re-measured.
_TICK_PATH = ("src/light/", "src/core/Scheduler.h", "src/core/HttpServerModule.cpp",
              "src/platform/")


def build_gates(firmware):
    """The merge gate list: the shared mechanical checks re-run over the whole branch
    diff, plus the judgment gates only a human can settle.

    ESP32 stays a freshness check rather than a compile because CI already builds every
    variant on the PR; what CI cannot tell you is whether the binary on your bench matches
    the branch you are about to merge.

    Re-running the mechanical checks here is not redundant with the commit gates: an
    individually-green commit series can still land a drifted tree (a spec renamed in
    commit 3, its module edited in commit 5).
    """
    return mechanical_gates(firmware, esp32="freshness") + [
        Gate("Reviewer agent over the branch diff", None,
             manual_hint="start it FIRST so it runs while the rest proceed; scope: "
                         "boundaries, bespoke conventions, unnecessary abstractions, "
                         "duplication, hot path, spec conformance, bloat"),

        Gate("external review addressed", None,
             manual_hint="CodeRabbit + human findings fixed or accepted with a reason"),

        Gate("lessons carried forward", None,
             manual_hint="only when VERY important: a real gotcha to lessons.md, a major "
                         "decision to a new ADR, a hardened rule to CLAUDE.md"),

        Gate("docs sync", None,
             manual_hint="every new module / control / endpoint documented; plan text "
                         "moved into the PR description and the plan file deleted"),

        Gate("PR title and description match the diff", None,
             manual_hint="the description is the permanent record of what landed"),

        # Triggered like any other gate: run_gates drops it when the branch touches
        # nothing in the tick path, so the manual list never asks for a measurement that
        # cannot have changed.
        Gate("performance snapshot in performance.md", None,
             lambda f: touches(f, *_TICK_PATH),
             manual_hint="tick-path code changed on this branch — compare tick/FPS to the "
                         "previous committed values and explain significant changes"),

        Gate("permission review", None,
             manual_hint="prune .claude/settings.local.json, then snapshot the approved "
                         "list to .claude/settings.local.cleaned.json and commit it; never "
                         "broaden destructive or network-mutating permissions"),

        Gate("README / quick-start refresh", None,
             manual_hint="only if build, flash, or first-run UX changed"),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default="main",
                        help="Branch to diff against (default: main).")
    parser.add_argument("--firmware", default="esp32s3-n16r8",
                        help="ESP32 variant the firmware-freshness gate checks.")
    args = parser.parse_args()

    sys.exit(run_gates(build_gates(args.firmware), changed_files(base=args.base),
                       f"Merge gates (branch diff vs {args.base})",
                       next_step="The manual gates above are the product owner's call — "
                                 "this script never merges."))


if __name__ == "__main__":
    main()
