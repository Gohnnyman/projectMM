#!/usr/bin/env python3
"""Measure the repo's current state into `docs/metrics/` — the lean-o-meter.

The v4 goal is a system that stays small as it gains features, and the honest way to know
whether that is happening is to measure it every commit rather than to assert it. This
writes ONE small file holding only the CURRENT numbers: flash per firmware variant, tick
and FPS per target, lines of code by area, comment density, test counts, docs inventory.

**The file never grows, because the history is git's.** `git log -p docs/metrics/repo-health.json`
is the trend; the file itself is a snapshot. That is the whole design: no accumulating
log, no rolling window, no second source of truth to prune later.

**Soft ratchet, by intent.** Nothing here fails a build or blocks a commit. The gate
prints the delta against the committed file before overwriting it, so growth is visible
while you work, and the file's diff shows it again in review. The judgment stays human:
these numbers count things, they cannot tell a valuable comment from a restating one, or
a load-bearing test from a trivial one.

Every measurement is deterministic and dependency-free (git ls-files, file reads, stat),
so two machines agree and a number never moves for a reason nobody can explain.

Usage:
  uv run moondeck/check/repo_health.py            # print the snapshot + delta, write nothing
  uv run moondeck/check/repo_health.py --write    # write docs/metrics/repo-health.* (KPI gate does this)
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# The firmware registry, so carry-forward can drop rows for variants that no longer exist
# (see merge_carry_forward). Same single source of truth check_firmwares.py reads.
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from build_esp32 import FIRMWARES  # noqa: E402
HEALTH_FILE = ROOT / "docs" / "metrics" / "repo-health.json"
# The same snapshot as a table a human reads: units applied, ratios as percentages, areas
# grouped. The JSON stays the source the delta is computed from; this is the view. Both
# are generated from one measurement, so they cannot disagree.
HEALTH_MD = ROOT / "docs" / "metrics" / "repo-health.md"

# Source areas measured separately: the core/light split is the one the architecture cares
# about (core is meant to grow slower than the domain), and the rest are the other places
# code accumulates.
AREAS = {
    "core": "src/core",
    "light": "src/light",
    "platform": "src/platform",
    "ui": "src/ui",
    "test": "test",
    "moondeck": "moondeck",
}

CODE_SUFFIXES = {".h", ".cpp", ".c", ".js", ".py"}


def _git_files(prefix):
    """Tracked files under `prefix`. Tracked-only on purpose: a build artifact or a local
    scratch file is not repo state, and counting it would make the number depend on
    whatever happens to sit in the working tree."""
    out = subprocess.run(["git", "ls-files", prefix], cwd=ROOT,
                         capture_output=True, text=True).stdout
    return [ROOT / line for line in out.splitlines() if line.strip()]


def _read(path):
    return path.read_text(encoding="utf-8", errors="replace")


def measure_loc():
    """Lines of code per area, counting only source files."""
    loc = {}
    for area, prefix in AREAS.items():
        total = 0
        for f in _git_files(prefix):
            if f.suffix in CODE_SUFFIXES and f.is_file():
                total += _read(f).count("\n")
        loc[area] = total
    return loc


def measure_comments():
    """Comment lines and their share of each area's source.

    Crude by design — it counts `//`, `///` and `#` line comments, not block comments or
    trailing ones. The absolute number is not the point; the *direction* is, and this
    catches the drift the comment-diet rule exists to notice.
    """
    out = {}
    for area, prefix in AREAS.items():
        comment_lines = code_lines = 0
        for f in _git_files(prefix):
            if f.suffix not in CODE_SUFFIXES or not f.is_file():
                continue
            for line in _read(f).splitlines():
                s = line.strip()
                if not s:
                    continue
                code_lines += 1
                if s.startswith("//") or (f.suffix == ".py" and s.startswith("#")):
                    comment_lines += 1
        out[area] = {
            "lines": comment_lines,
            "ratio": round(comment_lines / code_lines, 3) if code_lines else 0.0,
        }
    return out


def measure_flash():
    """Built firmware size per variant, in bytes.

    Only variants present in build/ are reported. A variant that was not built this run
    carries its previous number forward (see merge_carry_forward) rather than vanishing:
    a docs-only commit genuinely did not change any firmware size.
    """
    flash = {}
    build = ROOT / "build"
    if not build.exists():
        return flash
    for d in sorted(build.glob("esp32-*")):
        binary = d / "projectMM.bin"
        if binary.exists():
            flash[d.name.replace("esp32-", "", 1)] = binary.stat().st_size
    desktop = ROOT / "build" / "projectMM"
    if desktop.exists():
        flash["desktop"] = desktop.stat().st_size
    return flash


def measure_docs():
    """Documentation inventory — the counts the docs-bloat conversation actually turns on."""
    md = [f for f in _git_files("docs") if f.suffix == ".md"]
    plans = [f for f in md if "history/plans" in f.as_posix()]
    lessons = ROOT / "docs" / "history" / "lessons.md"
    claude = ROOT / "CLAUDE.md"
    backlog = [f for f in md if "backlog" in f.as_posix()]
    return {
        "md_files": len(md),
        "md_lines": sum(_read(f).count("\n") for f in md if f.is_file()),
        "plans_files": len(plans),
        "backlog_lines": sum(_read(f).count("\n") for f in backlog if f.is_file()),
        "lessons_lines": _read(lessons).count("\n") if lessons.exists() else 0,
        "claude_md_lines": _read(claude).count("\n") if claude.exists() else 0,
    }


def measure_tests():
    """Test counts. `cases` comes from the test sources rather than a run, so this needs
    no build and cannot report a stale binary's numbers."""
    cases = 0
    for f in _git_files("test"):
        if f.suffix == ".cpp" and f.is_file():
            cases += _read(f).count("TEST_CASE(")
    scenarios = [f for f in _git_files("test/scenarios") if f.suffix == ".json"]
    return {"cases": cases, "scenarios": len(scenarios)}


def measure_complexity():
    """Complexity, the number lizard owns (docs/testing.md § Static analysis).

    Deliberately the RAW count, not the baselined one: the gate (check_lizard.py) subtracts
    whitelizard.txt so it fails only on new violations, but the TREND has to see the whole
    number or it flatlines at 0 the moment a baseline lands and hides all future growth.

    Imported lazily and tolerantly — repo_health must stay runnable when lizard is not
    installed, and a missing tool should carry the previous value forward rather than write a
    misleading 0. (Returning {} lets merge_carry_forward do exactly that.)
    """
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import check_lizard
        funcs = check_lizard.measure()
        if not funcs:
            return {}
        viol = check_lizard.violations(funcs)
        return {
            "functions": len(funcs),
            "over_threshold": len(viol),
            "worst_ccn": max((f["ccn"] for f in funcs), default=0),
        }
    except Exception:
        return {}


def _head():
    """The short SHA the measurement describes."""
    return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                          capture_output=True, text=True).stdout.strip()


def snapshot(perf=None):
    """The full current-state measurement. `perf` is the tick/FPS block the KPI collector
    already gathered — passed in rather than re-measured, since it needs a running device."""
    head = _head()
    return {
        "commit": head,
        "flash": measure_flash(),
        "perf": perf or {},
        "loc": measure_loc(),
        "comments": measure_comments(),
        "tests": measure_tests(),
        "docs": measure_docs(),
        "complexity": measure_complexity(),
    }


def _valid_snapshot(data, source):
    """A snapshot mapping, or {}, saying so out loud when it is neither.

    merge_carry_forward does `old.get(section, {})`, so a JSON list or scalar would raise, and a
    section holding a list would corrupt the merge. A malformed baseline must not read as "no
    previous numbers" either: that silently reports every metric as new, which looks like a clean
    slate rather than a broken file.
    """
    if not isinstance(data, dict):
        print(f"repo-health: ignoring {source}: expected an object, got {type(data).__name__}",
              file=sys.stderr)
        return {}
    for key in ("flash", "perf", "complexity"):
        if key in data and not isinstance(data[key], dict):
            print(f"repo-health: ignoring {source}: section '{key}' is "
                  f"{type(data[key]).__name__}, expected an object", file=sys.stderr)
            return {}
    return data


def load_previous():
    """The COMMITTED snapshot, read from git rather than from the working tree.

    The delta is meant to answer "what did this commit change", so the baseline has to be the last
    commit, not whatever a previous run left on disk. Reading the working-tree file made every run
    after the first compare against the run before it, so a second run inside one commit showed a
    delta of ~0 and the real change vanished. Running the check twice must give the same answer.

    Falls back to the working-tree file when git cannot answer (a fresh clone with no commit yet, or
    the file untracked), which keeps a first-ever run working.
    """
    try:
        out = subprocess.run(["git", "show", f"HEAD:{HEALTH_FILE.relative_to(ROOT).as_posix()}"],
                             cwd=ROOT, capture_output=True, text=True, timeout=10)
        if out.returncode == 0:
            return _valid_snapshot(json.loads(out.stdout), "the committed snapshot")
    except json.JSONDecodeError:
        print("repo-health: the committed snapshot is not valid JSON, "
              "falling back to the working tree", file=sys.stderr)
    except (subprocess.SubprocessError, OSError):
        pass
    if not HEALTH_FILE.exists():
        return {}
    try:
        return _valid_snapshot(json.loads(_read(HEALTH_FILE)), str(HEALTH_FILE.name))
    except json.JSONDecodeError:
        print(f"repo-health: {HEALTH_FILE.name} is not valid JSON, treating as empty",
              file=sys.stderr)
        return {}


def load_working_tree():
    """The snapshot on disk, whatever produced it.

    Carry-forward needs the NEWEST numbers, not the committed ones: a board-attached run writes fresh
    perf figures here, and a later boardless run must preserve them rather than reverting to what the
    last commit happened to hold. That is the opposite of what the delta baseline wants, which is why
    the two read from different places.
    """
    if not HEALTH_FILE.exists():
        return {}
    try:
        return _valid_snapshot(json.loads(_read(HEALTH_FILE)), str(HEALTH_FILE.name))
    except json.JSONDecodeError:
        return {}


def merge_carry_forward(new, old):
    """Keep the previous value for anything this run could not measure.

    A commit that did not build the P4 firmware, or ran without a bench board, should not
    silently drop those numbers — the alternative is a file whose contents depend on which
    targets happened to be built, which makes every diff unreadable.

    Carrying forward is bounded by the firmware REGISTRY, not by history: a renamed or
    deleted variant would otherwise linger forever, since nothing ever measures it again to
    overwrite the stale row. `esp32p4-eth` and `esp32p4-eth-wifi` outlived their rename to
    `esp32p4rev1-*` exactly this way. Only `flash` is keyed by firmware; `perf` and
    `complexity` are keyed by platform/metric and are carried forward as they were.
    """
    # Drop rows for ESP32 variants that no longer exist, but keep everything else: `flash`
    # also holds non-firmware targets (`desktop`), which are not in FIRMWARES and must not be
    # filtered out. So the rule is "an esp32* key that is not a known firmware is a ghost",
    # which is exactly what a rename leaves behind and nothing else.
    known = set(FIRMWARES)
    for key in ("flash", "perf", "complexity"):
        merged = dict(old.get(key, {}))
        if key == "flash":
            merged = {k: v for k, v in merged.items()
                      if not k.startswith("esp32") or k in known}
        merged.update(new.get(key, {}))
        new[key] = merged
    return new


def _flatten(d, prefix=""):
    """Nested dict → {dotted.key: number}, so the delta is one flat comparison."""
    flat = {}
    for k, v in d.items():
        key = f"{prefix}{k}"
        if isinstance(v, dict):
            flat.update(_flatten(v, f"{key}."))
        elif isinstance(v, (int, float)):
            flat[key] = v
    return flat


def format_delta(new, old):
    """The lines that make growth visible at commit time. Only what moved is printed."""
    if not old:
        return ["repo-health: first snapshot, no previous numbers to compare"]
    new_flat, old_flat = _flatten(new), _flatten(old)
    changes = []
    for key, value in sorted(new_flat.items()):
        before = old_flat.get(key)
        if before is None or before == value:
            continue
        diff = value - before
        sign = "+" if diff > 0 else ""
        if isinstance(value, float):
            changes.append(f"  {key}: {before} → {value} ({sign}{round(diff, 3)})")
        else:
            changes.append(f"  {key}: {before} → {value} ({sign}{diff})")
    if not changes:
        return ["repo-health: unchanged"]
    return [f"repo-health: {len(changes)} metric(s) moved"] + changes


def _kb(n):
    return f"{round(n / 1024):,} KB" if n else "—"


def _pct(r):
    return f"{round(r * 100, 1)} %"


def _arrow(new, old, key, fmt=str, lower_is_better=True):
    """`value (±delta)` when the number moved, plain value when it didn't.

    The delta is the point of the table: an absolute number answers "how big", but only
    the change answers "which way is this going", which is the question the ratchet asks.
    """
    if old is None or key not in old or old[key] == new:
        return fmt(new)
    diff = new - old[key]
    sign = "+" if diff > 0 else "−"
    mark = "" if lower_is_better is None else (" ⚠" if (diff > 0) == lower_is_better else " ✓")
    return f"{fmt(new)} ({sign}{fmt(abs(diff))}){mark}"


def render_markdown(new, old):
    """The snapshot as a table, with units and per-metric deltas against the last commit."""
    o = old or {}
    # The page lives in docs/, so the link to its generator climbs one level. Same `../`
    # count GitHub's raw view uses, so the link resolves in both places.
    L = [f"# Repo health", "",
         f"Measured at `{new.get('commit', '?')}`. Generated by "
         f"[`moondeck/check/repo_health.py`](../../moondeck/check/repo_health.py) on every "
         f"KPI-gate run. **Do not edit by hand.**", "",
         "Current state only; the trend is this file's git history "
         "(`git log -p docs/metrics/repo-health.md`). Nothing here fails a build: the numbers make "
         "growth visible, the judgment stays human.", ""]

    if new.get("flash"):
        L += ["## Firmware size", "", "| Target | Flash |", "|---|---:|"]
        for k, v in sorted(new["flash"].items()):
            L.append(f"| {k} | {_arrow(v, o.get('flash'), k, _kb)} |")
        L.append("")

    if new.get("perf"):
        L += ["## Render performance", "", "| Target | Tick | FPS |", "|---|---:|---:|"]
        for k, v in sorted(new["perf"].items()):
            prev = (o.get("perf") or {}).get(k, {})
            tick = _arrow(v.get("tick_us", 0), prev, "tick_us", lambda n: f"{n:,} µs")
            fps = _arrow(v.get("fps") or 0, prev, "fps", lambda n: f"{n:,}", lower_is_better=False)
            L.append(f"| {k} | {tick} | {fps} |")
        L.append("")

    L += ["## Code", "", "| Area | Lines | Comments | Comment share |", "|---|---:|---:|---:|"]
    for area in new.get("loc", {}):
        c = new.get("comments", {}).get(area, {})
        oc = (o.get("comments") or {}).get(area)
        L.append(
            f"| {area} "
            f"| {_arrow(new['loc'][area], o.get('loc'), area, lambda n: f'{n:,}')} "
            f"| {c.get('lines', 0):,} "
            f"| {_arrow(c.get('ratio', 0), oc, 'ratio', _pct)} |")
    L.append("")

    t, ot = new.get("tests", {}), o.get("tests")
    L += ["## Tests", "", "| Kind | Count |", "|---|---:|",
          f"| unit cases | {_arrow(t.get('cases', 0), ot, 'cases', lambda n: f'{n:,}', lower_is_better=False)} |",
          f"| scenarios | {_arrow(t.get('scenarios', 0), ot, 'scenarios', str, lower_is_better=False)} |",
          ""]

    cx, ocx = new.get("complexity", {}), o.get("complexity")
    if cx:
        # `functions` rising is neutral-to-good (the codebase grows); the two that matter are
        # how many are over threshold and how bad the worst one is.
        L += ["## Complexity", "", "| Metric | Value |", "|---|---:|",
              f"| functions | {_arrow(cx.get('functions', 0), ocx, 'functions', lambda n: f'{n:,}', lower_is_better=False)} |",
              f"| over threshold | {_arrow(cx.get('over_threshold', 0), ocx, 'over_threshold', str)} |",
              f"| worst CCN | {_arrow(cx.get('worst_ccn', 0), ocx, 'worst_ccn', str)} |",
              ""]

    d, od = new.get("docs", {}), o.get("docs")
    L += ["## Documentation", "", "| Metric | Value |", "|---|---:|",
          f"| markdown files | {_arrow(d.get('md_files', 0), od, 'md_files', str)} |",
          f"| markdown lines | {_arrow(d.get('md_lines', 0), od, 'md_lines', lambda n: f'{n:,}')} |",
          f"| plan files | {_arrow(d.get('plans_files', 0), od, 'plans_files', str)} |",
          f"| backlog lines | {_arrow(d.get('backlog_lines', 0), od, 'backlog_lines', lambda n: f'{n:,}')} |",
          f"| lessons lines | {_arrow(d.get('lessons_lines', 0), od, 'lessons_lines', str)} |",
          f"| CLAUDE.md lines | {_arrow(d.get('claude_md_lines', 0), od, 'claude_md_lines', str)} |",
          ""]
    return "\n".join(L)


def write(perf=None, quiet=False):
    """Measure, print the delta, and rewrite both views. Called by the KPI gate."""
    old = load_previous()                                  # committed → what the delta compares against
    new = merge_carry_forward(snapshot(perf), load_working_tree())   # newest → what unmeasured metrics keep
    if not quiet:
        for line in format_delta(new, old):
            print(line)
    HEALTH_FILE.write_text(json.dumps(new, indent=2) + "\n", encoding="utf-8")
    HEALTH_MD.write_text(render_markdown(new, old) + "\n", encoding="utf-8")
    return new


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write", action="store_true",
                        help="Rewrite repo-health.json. Without it, measure and print only.")
    args = parser.parse_args()

    old = load_previous()
    new = merge_carry_forward(snapshot(), load_working_tree())
    for line in format_delta(new, old):
        print(line)
    if args.write:
        HEALTH_FILE.write_text(json.dumps(new, indent=2) + "\n", encoding="utf-8")
        HEALTH_MD.write_text(render_markdown(new, old) + "\n", encoding="utf-8")
        print(f"\nwrote {HEALTH_FILE.name} + {HEALTH_MD.name}")
    else:
        print("\n" + render_markdown(new, old))
    return 0


if __name__ == "__main__":
    sys.exit(main())
