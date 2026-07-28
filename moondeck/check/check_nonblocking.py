#!/usr/bin/env python3
"""Hot-path discipline, checked by the compiler: what the render path calls that can block.

`MoonModule::tick/tick20ms/tick1s` carry `MM_NONBLOCKING` (platform.h). Clang 20+ then verifies
under `-Wfunction-effects` that nothing they reach allocates or blocks — TRANSITIVELY, through
the whole call graph, which is the half `check_hotpath.py`'s regex cannot see: that one reads the
text of a tick body and is blind to what its callees do.

Reports unique SITES. A header included by N translation units yields N copies of the same
warning, so a raw build prints ~1350 lines for ~175 real findings; deduplicating on
(file, line) is most of this script's value.

Not a gate. `-Wno-error=function-effects` in CMakeLists keeps the build green while these are
triaged; each finding is a judgement — fix it, annotate the callee, or accept it with a scoped
reason — and the answer differs per site.

Usage:
  uv run moondeck/check/check_nonblocking.py            # summary by callee, then every site
  uv run moondeck/check/check_nonblocking.py --module AudioService
"""

import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_clang_query  # noqa: E402  — the module→files resolver, one owner

# `path:line:col: warning: <msg> [-Wfunction-effects]`
_WARN = re.compile(r"^(?P<file>\S+?):(?P<line>\d+):(?P<col>\d+): warning: "
                   r"(?P<msg>.*?) \[-Wfunction-effects\]")
_CALLEE = re.compile(r"non-'nonblocking' function '(?P<name>[^']+)'")

# Clang follows each warning with a `note:` saying WHY the callee is not nonblocking — either it
# reaches another blocking function, or it has a construct that rules it out (a static local
# needs a guard variable and a lock). That note is the difference between "this line is flagged"
# and "here is what to fix", so it becomes the WHY column.
_NOTE = re.compile(r"note: function cannot be inferred 'nonblocking' because it "
                   r"(?:calls non-'nonblocking' function '(?P<via>[^']+)'|(?P<reason>.+))")

MAX_ROWS = 40

# The enclosing method of a call site. Clang names the call and the callee but NOT the function
# they sit in, and that is the column that says which tick tier is affected — tick() every frame
# vs tick1s() once a second is an order of magnitude difference in what a blocking call costs.
# Walked backwards from the site to the nearest definition at class-member indent.
# Excludes control-flow keywords, which otherwise match `if (...) {` and report `IN: if`.
# Handles both in-class definitions and out-of-line `void Foo::tick() {`.
_KEYWORDS = ("if", "for", "while", "switch", "catch", "else", "do", "return")
_DEF = re.compile(r"^(?P<indent>\s*)(?:[\w:<>,&*\s]+?\s)?(?:(?P<cls>\w+)::)?(?P<name>\w+)\s*"
                  r"\([^;]*\)\s*(?:const\s*)?(?:MM_NONBLOCKING\s*)?(?:override\s*)?"
                  r"(?:noexcept\s*)?\{")

# Which tick tier a function belongs to, once resolved through the enclosing method.
TIERS = ("tick", "tick20ms", "tick1s")


def enclosing_function(rel_path, line, _cache={}):
    """The method a call site sits in, or "" when it cannot be determined."""
    src = _cache.get(rel_path)
    if src is None:
        f = ROOT / rel_path
        src = _cache[rel_path] = (f.read_text(encoding="utf-8", errors="replace").split("\n")
                                  if f.exists() else [])
    for i in range(min(line, len(src)) - 1, -1, -1):
        m = _DEF.match(src[i])
        if m and len(m.group("indent")) <= 4 and m.group("name") not in _KEYWORDS:
            return m.group("name")
    return ""


def build_output(build_dir):
    """A full rebuild's warnings. `--clean-first` because an incremental build only recompiles
    what changed, and a cached TU prints nothing — which would read as "no findings"."""
    proc = subprocess.run(["cmake", "--build", str(build_dir), "--clean-first"],
                          cwd=ROOT, capture_output=True, text=True)
    return proc.stdout + proc.stderr


def collect(out):
    """Unique findings keyed on (file, line, col), each with the root cause clang gives.

    A warning line names the call; the `note:` line that follows names why that callee blocks.
    Both are needed to act on a finding, so they are carried together.
    """
    rows, last = {}, None
    for line in out.splitlines():
        clean = line.replace(str(ROOT) + "/", "")
        m = _WARN.match(clean)
        if m:
            f = m["file"]
            last = None
            if f.startswith(("src/", "test/")):
                c = _CALLEE.search(m["msg"])
                key = (f, int(m["line"]), int(m["col"]))
                # Two violation kinds: a call to something that blocks, or a construct that
                # blocks by itself (a static local needs a guard variable and a one-time lock).
                # The second has no callee, so name the construct instead of truncating the
                # warning text into the CALLS column.
                rows.setdefault(key, {
                    "file": f, "line": int(m["line"]),
                    "callee": c["name"] if c else "(static local variable)",
                    "why": "" if c else "guard variable + one-time lock on first use",
                    "fn": enclosing_function(f, int(m["line"])),
                })
                last = key
            continue
        # The note belongs to the warning just above it.
        if last:
            n = _NOTE.search(clean)
            if n and not rows[last]["why"]:
                rows[last]["why"] = (f"calls {n['via']}" if n["via"]
                                     else n["reason"].strip().rstrip("."))
    return sorted(rows.values(), key=lambda r: (r["file"], r["line"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--module", help="Only findings in this module's source files.")
    args = ap.parse_args()

    build_dir = check_clang_query.check_clang_tidy._host_build_dir()
    if not (build_dir / "CMakeCache.txt").exists():
        print(f"No build in {build_dir.relative_to(ROOT)} — run "
              f"`uv run moondeck/build/build_desktop.py` first.", file=sys.stderr)
        return 2

    rows = collect(build_output(build_dir))

    if args.module:
        only = check_clang_query.module_files(args.module)
        if not only:
            print(f"No source files for module '{args.module}'.", file=sys.stderr)
            return 2
        print(f"Filtered to {args.module}: {', '.join(only)}\n")
        rows = [r for r in rows if r["file"] in only]

    print(f"{len(rows)} call(s) from the render path that can block or allocate.")
    if not rows:
        print("\nNone. \u2713  (If that seems wrong, confirm the build actually recompiled \u2014 a "
              "cached TU prints no warnings.)")
        return 0

    # Split by tick tier: tick() runs every frame, tick1s() once a second, so the same blocking
    # call costs three orders of magnitude more in one than the other. Pooling them hides that.
    def tier_of(r):
        return r["fn"] if r["fn"] in TIERS else "other"

    def table(title, subset, note):
        if not subset:
            return
        callee_w = min(max(len(r["callee"]) for r in subset), 36)
        fn_w = min(max(len(r["fn"] or "?") for r in subset), 18)
        why_w = min(max((len(r["why"]) for r in subset), default=0), 44) or 1
        loc_w = min(max(len(f"{r['file']}:{r['line']}") for r in subset), 44)

        def clip(s, w):
            return s if len(s) <= w else s[: w - 1] + "\u2026"

        print(f"\n{title} \u2014 {len(subset)}   ({note})")
        print(f"  {'CALLS':<{callee_w}}  {'IN':<{fn_w}}  {'WHY IT BLOCKS':<{why_w}}  FILE:LINE")
        print(f"  {'-' * callee_w}  {'-' * fn_w}  {'-' * why_w}  {'-' * loc_w}")
        for r in subset[:MAX_ROWS]:
            print(f"  {clip(r['callee'], callee_w):<{callee_w}}  "
                  f"{clip(r['fn'] or '?', fn_w):<{fn_w}}  "
                  f"{clip(r['why'] or '\u2014', why_w):<{why_w}}  "
                  f"{r['file']}:{r['line']}")
        if len(subset) > MAX_ROWS:
            print(f"  \u2026 {len(subset) - MAX_ROWS} more. Use --module <name> to scope.")

    for tier, note in (("tick", "every frame \u2014 the hot path"),
                       ("tick20ms", "50x a second"),
                       ("tick1s", "once a second \u2014 sub-hot path"),
                       ("other", "reached from a tick, tier not resolved")):
        table(tier + "()" if tier != "other" else "OTHER",
              [r for r in rows if tier_of(r) == tier], note)
    return 0


if __name__ == "__main__":
    sys.exit(main())
