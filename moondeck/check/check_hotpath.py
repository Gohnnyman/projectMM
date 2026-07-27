#!/usr/bin/env python3
"""Flag hot-path discipline violations: allocation and blocking in the render path.

The rule (CLAUDE.md § Principles, Minimalism; architecture.md § Hot path discipline): in
the render loop and everything it calls there is no heap allocation and no blocking. A
violation does not fail the build — it produces a frame hitch or, on a long-running ESP32,
fragmentation that degrades over hours. That makes it exactly the kind of defect a review
misses and a user reports as "it stutters after a day".

This is a LINT, not a proof. It reads the text of the functions that make up the render
path and reports the banned constructs it can see:

    allocation   new / malloc / push_back / std::string / make_unique / make_shared
    blocking     delay / sleep / mutex.lock  (try_lock is the sanctioned form)

What it cannot see: a call into a helper that allocates, a container that grows behind an
innocent-looking method, allocation inside a template instantiated elsewhere. So a clean
run means "no violation is visible in the render path's own source", not "the hot path is
allocation-free". Treat a finding as a question to answer, not an automatic bug.

Usage:
  uv run moondeck/check/check_hotpath.py            # report findings, exit 1 if any
  uv run moondeck/check/check_hotpath.py --list     # list the scanned functions and exit
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "src"

# The methods that ARE the render path. `tick()` is the render loop itself; the periodic
# ticks share the same thread between two frames, so a heavy or allocating one steals from
# the render budget just as directly (architecture.md § Hot path discipline, sub-hot path).
HOT_METHODS = ("tick", "tick20ms", "tick1s")

# One pattern per banned construct, with the reason the reader needs at the point of the
# finding — a bare "push_back found" teaches nothing.
BANNED = [
    (re.compile(r'\bnew\s+[A-Za-z_]'), "heap allocation (`new`)",
     "allocate in setup()/prepare() and reuse the buffer"),
    (re.compile(r'\bmalloc\s*\('), "heap allocation (`malloc`)",
     "allocate in setup()/prepare() and reuse the buffer"),
    (re.compile(r'\.push_back\s*\('), "heap allocation (`push_back` may grow)",
     "pre-size the container in prepare(), or use a fixed array"),
    (re.compile(r'\bstd::string\b'), "heap allocation (`std::string`)",
     "use a fixed char buffer; std::string allocates on construction and on append"),
    (re.compile(r'\bmake_unique\s*<|\bmake_shared\s*<'), "heap allocation (smart-pointer factory)",
     "allocate in setup()/prepare()"),
    (re.compile(r'\bdelay\s*\(|\bdelayMs\s*\('), "blocking (`delay`)",
     "gate on millis() instead; blocking the render task shows as a visible glitch"),
    (re.compile(r'\bsleep\s*\(|sleep_for\s*\('), "blocking (`sleep`)",
     "gate on millis() instead"),
    (re.compile(r'\.lock\s*\(\s*\)'), "blocking (`mutex.lock`)",
     "use try_lock and skip the work this tick"),
]

# A line carrying this marker is a deliberate, explained exception. The rule is the same
# one the codebase uses for a -Wno- suppression: the escape hatch exists, but it has to
# state its reason at the site, so a reviewer sees the justification rather than silence.
ALLOW_MARKER = "hot-path-ok:"

# The desktop platform's own run loop is not the device's render path: it is host-side
# glue, free to allocate and block. (Only `src/` is scanned, so the test tree needs no
# entry here.)
SKIP_PARTS = {"platform/desktop"}


def hot_path_bodies(path, text):
    """Yield (method_name, start_line, body_text) for each hot method defined in `text`.

    Brace-matched from the method's opening `{`, so a nested block or a lambda inside the
    body stays part of it. Deliberately simple: this is a lint over well-formed project
    source, not a C++ parser.
    """
    for method in HOT_METHODS:
        # `void tick() override {` / `void tick1s() {` — the definition, not a call.
        for m in re.finditer(rf'\b(?:void|bool|int)\s+{method}\s*\([^)]*\)[^;{{]*\{{', text):
            start = m.end() - 1
            depth, i = 0, start
            while i < len(text):
                if text[i] == '{':
                    depth += 1
                elif text[i] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            body = text[start:i]
            yield method, text[:start].count('\n') + 1, body


def scan_file(path):
    findings = []
    rel = path.relative_to(ROOT).as_posix()
    if any(part in rel for part in SKIP_PARTS):
        return findings

    text = path.read_text(encoding="utf-8", errors="replace")
    for method, method_line, body in hot_path_bodies(path, text):
        for offset, line in enumerate(body.splitlines()):
            stripped = line.strip()
            # Skip comments and deliberate, explained exceptions.
            if stripped.startswith("//") or stripped.startswith("*"):
                continue
            if ALLOW_MARKER in line:
                continue
            # Match against CODE only. A trailing comment ("// fade dead on new game")
            # otherwise reads as an allocation — prose is full of the banned words, and a
            # lint that cries wolf on comments gets muted, which costs the real findings.
            code = line.split("//", 1)[0]
            if not code.strip():
                continue
            for pattern, what, fix in BANNED:
                if pattern.search(code):
                    findings.append({
                        "file": rel,
                        "line": method_line + offset,
                        "method": method,
                        "what": what,
                        "fix": fix,
                        "code": stripped[:100],
                    })
                    break   # one finding per line is enough to send the reader there
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true",
                        help="List the hot-path methods that would be scanned, then exit.")
    args = parser.parse_args()

    files = sorted(SRC.rglob("*.h")) + sorted(SRC.rglob("*.cpp"))

    if args.list:
        for path in files:
            rel = path.relative_to(ROOT).as_posix()
            if any(part in rel for part in SKIP_PARTS):
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for method, line, _ in hot_path_bodies(path, text):
                print(f"{rel}:{line}  {method}()")
        return 0

    findings = []
    for path in files:
        findings.extend(scan_file(path))

    if not findings:
        print("Hot-path check passed: no allocation or blocking visible in the render path.")
        return 0

    print(f"Hot-path findings ({len(findings)}):\n")
    for f in findings:
        print(f"  {f['file']}:{f['line']}  in {f['method']}()")
        print(f"      {f['what']}")
        print(f"      {f['code']}")
        print(f"      -> {f['fix']}\n")

    print(f"A finding is a question, not a verdict: this lint reads only the method's own "
          f"source.\nIf a line is a justified exception, mark it `// {ALLOW_MARKER} <reason>` "
          f"so the reason lives at the site.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
