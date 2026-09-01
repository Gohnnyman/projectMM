#!/usr/bin/env python3
"""Run the host test suites: the Python ones, the JS ones, or both.

These are the tests that live outside the C++ binary, and they cover what it cannot reach: the
cross-language contracts (the Improv frame's wire format, WLED's /json shape), the MoonDeck scripts
themselves, the browser code in src/ui, and the claim that every shipped MoonLive script is valid
C++. The commit gate and CI both run them; this is the same command with a card in front of it.

  uv run moondeck/test/test_host.py            # both suites
  uv run moondeck/test/test_host.py --python   # just Python
  uv run moondeck/test/test_host.py --js       # just JS

The Python deps ride in a PEP-723 block per test file, so they are named here rather than
installed: `uv run --with` resolves them per run and leaves no environment behind.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Named here rather than in a requirements file: each is a test's own dependency (pyserial for the
# flash tests, wled for the /json shim, markdown for the docs checks), and `uv run --with` is how
# every other script in MoonDeck reaches one.
PY_DEPS = ("pytest", "pyserial", "markdown", "wled")


def run(cmd, label):
    print(f"\n=== {label} ===", flush=True)
    r = subprocess.run(cmd, cwd=ROOT)
    return r.returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--python", action="store_true", help="run only the Python suite")
    ap.add_argument("--js", action="store_true", help="run only the JS suite")
    args = ap.parse_args()
    both = not (args.python or args.js)

    rc = 0
    if both or args.python:
        cmd = ["uv", "run"]
        for d in PY_DEPS:
            cmd += ["--with", d]
        cmd += ["pytest", "test/python", "-q"]
        rc |= run(cmd, "Python (test/python)")

    if both or args.js:
        # node, not uv: the JS suite is the browser code's own runner. Reported rather than failed
        # when node is absent, because a Python-only bench is a normal setup and a missing runtime
        # is "this does not apply here", which is what SKIP means.
        if shutil.which("node") is None:
            print("\n=== JS (test/js) ===\nSKIP: node is not on PATH", flush=True)
        else:
            # NODE expands this pattern, not a shell: the call takes a list and no shell=True, so
            # the literal `**` reaches node, which globs it itself (node 22+). The gate and CI spell
            # the same command inside a shell, where the shell expands it first; both reach the same
            # files, by two different mechanisms.
            rc |= run(["node", "--test", "test/js/**/*.test.mjs"], "JS (test/js)")

    print("\nDONE" if rc == 0 else "\nFAILED", flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
