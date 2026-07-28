#!/usr/bin/env python3
"""Complexity gate: fail on NEW over-complex functions, not on the ones we already live with.

lizard counts cyclomatic complexity (CCN) and length (NLOC) per function. Its job in this
stack is the NUMBER PER COMMIT that repo-health trends — clang-tidy can tell you a function
is complex today, only a trend tells you the codebase is getting worse (plan: "one rule, one
owner"; lizard owns complexity, clang-tidy's `readability-function-*` checks stay off).

A raw run reports 162 warnings, and a metric that can never reach zero is a poor gate: it
reads as permanently-failing, so people stop looking. Hence the baseline. `whitelizard.txt`
freezes today's set; this check fails only on a function that is NEW or newly over the
threshold. Bringing a baselined function under the limit and deleting its line is how the
number goes down — the same ratchet shape as repo-health.json.

The baseline matches on FILE + FUNCTION NAME, never line numbers, so it survives edits above
a function. That is lizard's own `-W/--whitelist` format (`file:name`, `#` comments), used
rather than reimplemented — the filtering is upstream's, we only generate and report.

Thresholds match collect_kpi.py so both tools report the same set: CCN > 10, NLOC > 60.

Usage:
  uv run moondeck/check/check_lizard.py             # report NEW violations, exit 1 if any
  uv run moondeck/check/check_lizard.py --all       # every violation, baseline ignored
  uv run moondeck/check/check_lizard.py --baseline  # rewrite whitelizard.txt from today
"""

import argparse
import csv
import io
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
# Not the repo root: lizard picks up `./whitelizard.txt` BY DEFAULT, and a baseline that
# applies itself silently is what zeroed the KPI's complexity count once this landed.
# Sitting in docs/metrics/ next to repo-health.* it is only ever applied on purpose, and
# it lives with the other measured-state files.
BASELINE = ROOT / "docs" / "metrics" / "whitelizard.txt"

# Same thresholds as collect_kpi.py — one definition of "too complex" across the tooling.
MAX_CCN = 10
MAX_NLOC = 60

# `src/ui/` is JavaScript served to the browser, not firmware C++.
LIZARD_ARGS = ["src/", "-l", "cpp", "-x", "src/ui/*"]


def measure(extra=None):
    """Every function lizard measured, as dicts. Public: collect_kpi.py calls this so the KPI
    number and this gate can never disagree about what lizard said.

    `--csv` so we parse a real format rather than a column-aligned report whose spacing shifts
    with the longest name. `-W /dev/null` disables lizard's own whitelist entirely: the raw
    measurement must never be filtered, or the KPI trend flatlines at 0 the moment a baseline
    exists and hides all future growth. Baselining is this script's job, applied explicitly
    below. (Belt and braces — the baseline no longer sits at lizard's default path either.)
    """
    cmd = ["uv", "run", "--with", "lizard", "python3", "-m", "lizard",
           *LIZARD_ARGS, "--csv", "-W", os.devnull, *(extra or [])]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=300)
    # lizard exits non-zero when warnings exist; that is data here, not a failure. A real
    # failure produces no parseable rows, which the caller detects.
    rows = list(csv.reader(io.StringIO(proc.stdout)))
    out = []
    for r in rows:
        # nloc,ccn,token,param,length,location,file,name,long_name,start,end
        if len(r) < 11:
            continue
        try:
            out.append({"nloc": int(r[0]), "ccn": int(r[1]), "token": int(r[2]),
                        "param": int(r[3]), "length": int(r[4]),
                        "file": r[6], "name": r[7], "start": int(r[9])})
        except ValueError:
            continue          # header or a malformed line
    if not out:
        print("lizard produced no parseable output — is it installed? (uv run --with lizard)",
              file=sys.stderr)
        print(proc.stderr.strip()[:500], file=sys.stderr)
        return None
    return out


def violations(funcs):
    """The functions over either threshold, worst-first so the report leads with the target."""
    bad = [f for f in funcs if f["ccn"] > MAX_CCN or f["nloc"] > MAX_NLOC]
    return sorted(bad, key=lambda f: (-f["ccn"], -f["nloc"]))


def _read_baseline():
    """The baselined (file, name) pairs. Absent file = no baseline, everything is new."""
    if not BASELINE.exists():
        return set()
    out = set()
    for line in BASELINE.read_text(encoding="utf-8").splitlines():
        line = line.split("#")[0].strip()
        if not line:
            continue
        # lizard's format is `file:name`; `::` in a C++ name is not the separator.
        pieces = line.replace("::", "##").split(":")
        if len(pieces) > 1:
            out.add((pieces[0], pieces[1].replace("##", "::")))
    return out


def _write_baseline(viol):
    lines = [
        "# lizard complexity baseline — the over-threshold functions as of this commit.",
        "#",
        "# Generated by `uv run moondeck/check/check_lizard.py --baseline`. Format is lizard's",
        "# own whitelist (`file:function`, matched by NAME so it survives edits above the",
        "# function). A line here means 'known, not yet fixed' — NOT 'acceptable'.",
        "#",
        "# This list only shrinks: simplify a function, delete its line. Adding a line means",
        "# admitting a new violation, which is what the check exists to prevent.",
        "#",
        f"# Thresholds: CCN > {MAX_CCN} or NLOC > {MAX_NLOC}.",
        "",
    ]
    for v in viol:
        lines.append(f"{v['file']}:{v['name']}   # CCN {v['ccn']}, NLOC {v['nloc']}")
    BASELINE.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _table(viol, indent="  "):
    """The violations as one aligned table, worst first.

    Flat and severity-ordered rather than grouped by file: the point of the list is "what do I
    fix next", and the worst function is the answer regardless of which file it lives in.
    (Grouping by file tripled the line count for 162 findings and buried the CCN 56 outlier
    under an alphabetically-earlier file.)

    Columns are space-padded rather than drawn with box characters — the output pane is a
    plain-text log, and space alignment survives copy-paste into an issue or commit message.
    """
    if not viol:
        return []

    # `mm::` is on essentially every symbol here, so it distinguishes nothing and costs width.
    # The directory likewise: the class name in the symbol already says where you are.
    def short(name):
        return name.removeprefix("mm::")

    def base(path):
        return path.rsplit("/", 1)[-1]

    name_w = min(max(*(len(short(v["name"])) for v in viol), len("FUNCTION")), 44)
    file_w = min(max(*(len(base(v["file"])) for v in viol), len("FILE")), 28)

    def clip(s, w):
        return s if len(s) <= w else s[: w - 1] + "…"

    # A `*` next to a number marks the threshold that actually tripped — a CCN 93 function and
    # a 178-line one need different fixes, and bare numbers do not say which rule fired.
    def cell(value, limit=None):
        return f"{value}{'*' if limit is not None and value > limit else ' '}"

    # The same five metrics lizard's own summary reports, so this table and the KPI line read
    # as one tool. Only CCN and NLOC are gated; TOKEN/PARAM/LINES are context that tells you
    # WHY a function is heavy (many branches vs a long argument list vs sheer size).
    out = [
        "",
        f"{indent}{'CCN':>5}  {'NLOC':>5}  {'TOKEN':>6}  {'PARAM':>5}  {'LINES':>5}  "
        f"{'FUNCTION':<{name_w}}  FILE",
        f"{indent}{'-' * 5}  {'-' * 5}  {'-' * 6}  {'-' * 5}  {'-' * 5}  "
        f"{'-' * name_w}  {'-' * file_w}",
    ]
    for v in sorted(viol, key=lambda v: (-v["ccn"], -v["nloc"])):
        out.append(f"{indent}{cell(v['ccn'], MAX_CCN):>5}  {cell(v['nloc'], MAX_NLOC):>5}  "
                   f"{v['token']:>6}  {v['param']:>5}  {v['length']:>5}  "
                   f"{clip(short(v['name']), name_w):<{name_w}}  {clip(base(v['file']), file_w)}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true", help="report every violation, ignore the baseline")
    ap.add_argument("--baseline", action="store_true", help="rewrite whitelizard.txt from today's run")
    ap.add_argument("--module", help="Report only this module's source files.")
    args = ap.parse_args()

    funcs = measure()
    if funcs is None:
        return 2

    # lizard measures per FILE (no translation units involved), so scoping is a plain filter.
    if args.module:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import check_clang_query
        only = check_clang_query.module_files(args.module)
        if not only:
            print(f"No source files for module '{args.module}'.", file=sys.stderr)
            return 2
        print(f"Filtered to {args.module}: {', '.join(only)}\n")
        funcs = [f for f in funcs if f["file"] in only]

    viol = violations(funcs)

    if args.baseline:
        _write_baseline(viol)
        print(f"Baseline written: {len(viol)} functions frozen in "
              f"{BASELINE.relative_to(ROOT)}")
        return 0

    if args.all:
        print(f"All violations: {len(viol)} (CCN > {MAX_CCN} or NLOC > {MAX_NLOC}) "
              f"of {len(funcs)} functions")
        print("\n".join(_table(viol)))
        return 0

    baseline = _read_baseline()
    # Scope the baseline to the same files, or the "no longer violates" report below compares a
    # module-sized violation set against the WHOLE baseline and calls every other module's entry
    # fixed — 159 phantom rows on one module, and acting on that advice wipes the baseline.
    if args.module:
        baseline = {b for b in baseline if b[0] in only}
    new = [v for v in viol if (v["file"], v["name"]) not in baseline]
    # A baselined function that is no longer a violation: the list should shrink to match.
    current = {(v["file"], v["name"]) for v in viol}
    fixed = [b for b in baseline if b not in current]

    print(f"lizard: {len(funcs)} functions, {len(viol)} over threshold "
          f"(CCN > {MAX_CCN} or NLOC > {MAX_NLOC}), {len(baseline)} baselined")

    if fixed:
        print(f"\n{len(fixed)} baselined function(s) no longer violate — "
              f"drop them from {BASELINE.name} (run --baseline):")
        for f, n in sorted(fixed)[:10]:
            print(f"  {n}  ({f})")

    if new:
        print(f"\nFAIL — {len(new)} NEW violation(s):")
        print("\n".join(_table(new)))
        print("\nSimplify it, or add it to whitelizard.txt with a reason if it is genuinely "
              "irreducible.")
        return 1

    print("\nNo new violations. ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
