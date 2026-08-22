#!/usr/bin/env python3
"""Shared gate runner for the lifecycle event scripts (precommit / premerge / prerelease).

A **gate** is one check with an objective trigger: it runs only when the change makes it
applicable, and reports PASS / FAIL / SKIP / MANUAL. The event scripts declare *which*
gates and *what triggers them*; everything about running them, deciding applicability from
the changed-file set, and printing the report lives here (one mechanism, three callers).

Outcome vocabulary, printed and returned:
  PASS    the check ran and succeeded
  FAIL    the check ran and failed (the event script exits non-zero)
  SKIP    the trigger did not match this change, so the check does not apply
  MANUAL  the check cannot be automated (hardware, a human decision); listed for the
          product owner to confirm, never auto-failed

The changed-file set comes from git and is the single input every trigger reads, so a
trigger is a pure function of the diff rather than a guess.
"""

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

# The desktop build dir is PER HOST (build/windows, build/macos, build/linux) — build_desktop.py
# owns that mapping, so import it rather than re-deriving it here. Two gates hardcoded a bare
# "build" and both were wrong off macOS: the build gate failed with "not a CMake build directory",
# and — far worse — `ctest --test-dir build` found no tests and exited 0, so the unit-test gate
# reported PASS without running anything. A gate that passes vacuously is worse than no gate.
sys.path.insert(0, str(ROOT / "moondeck" / "build"))
from build_desktop import host_build_dir  # noqa: E402  (path set immediately above)

PASS = "PASS"
FAIL = "FAIL"
SKIP = "SKIP"
MANUAL = "MANUAL"

# ANSI colors, disabled when the output is not a terminal (CI logs, pipes).
_TTY = sys.stdout.isatty()
_C = {
    PASS: "\033[32m" if _TTY else "",
    FAIL: "\033[31m" if _TTY else "",
    SKIP: "\033[90m" if _TTY else "",
    MANUAL: "\033[33m" if _TTY else "",
}
_RESET = "\033[0m" if _TTY else ""


def changed_files(base=None):
    """The paths this event covers, as repo-relative POSIX strings.

    Without `base`: the working tree + index against HEAD, i.e. "what would this commit
    contain" — the right question for the commit event. With `base` (e.g. "main"): every
    file the branch touches, via the merge-base, which is what the merge and release
    events ask about.
    """
    if base:
        cmd = ["git", "diff", "--name-only", f"{base}...HEAD"]
    else:
        # Staged + unstaged + untracked: the pre-commit gates run before `git add`, so
        # an unstaged edit must still trigger its gate.
        cmd = ["git", "status", "--porcelain"]

    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True).stdout
    if base:
        return [line.strip() for line in out.splitlines() if line.strip()]
    # Porcelain lines are "XY path" (or "XY old -> new" for a rename; take the new path).
    paths = []
    for line in out.splitlines():
        if len(line) < 4:
            continue
        path = line[3:].strip().strip('"')
        if " -> " in path:
            path = path.split(" -> ", 1)[1].strip().strip('"')
        paths.append(path)
    return paths


def touches(files, *prefixes, exclude=()):
    """True when any changed file starts with one of `prefixes` and none of `exclude`.

    The one predicate every trigger is written in, so a trigger reads as the rule it
    encodes: `touches(files, "src/", exclude=("src/platform/desktop/",))`.
    """
    for f in files:
        if any(f.startswith(e) for e in exclude):
            continue
        if any(f.startswith(p) for p in prefixes):
            return True
    return False


class Gate:
    """One check: a name, a trigger, and how to run it.

    `applies` is a callable taking the changed-file list and returning a bool — or None
    for a gate that always runs. `command` is the argv to execute; a gate with no command
    is MANUAL (a human confirms it).
    """

    def __init__(self, name, command=None, applies=None, manual_hint=None):
        self.name = name
        self.command = command
        self.applies = applies
        self.manual_hint = manual_hint


UV = ["uv", "run"]


def _child_env():
    """The environment a gate's subprocess runs in: this one, plus UTF-8 stdio.

    The check scripts print ✓, →, — and box-drawing characters. A child's stdout is a PIPE
    here, so Python picks `locale.getpreferredencoding()` for it — cp1252 on a Windows bench —
    and the FIRST such character raises UnicodeEncodeError *inside the child*. Two gates died
    that way after their real work had already succeeded and printed "None new since the
    baseline": a green check reported as FAIL because of a tick mark. PYTHONIOENCODING fixes
    every gate at once rather than each script re-deriving it, and the matching encoding= on
    the parent's side stops the mojibake (— arriving as a replacement char) in captured output.
    """
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    return env


def _have_gcc():
    """True when a REAL GCC is installed — the same names build_desktop.py --gcc looks for.

    Kept in step with gcc_pair() there; if that list grows a version, this one follows. Not
    imported from it because that function EXITS when it finds nothing, which is right for a
    build and wrong for a trigger.
    """
    return any(shutil.which(cxx) for cxx in ("g++-16", "g++-15", "g++-14", "g++-13"))

# What "this change could affect the desktop binary" means, and what it means for an
# ESP32 image. Named once here because every event script asks the same two questions;
# re-typing the tuples per script is how the lists drift apart.
COMPILES_DESKTOP = ("src/", "test/", "CMakeLists.txt", "library.json")
COMPILES_ESP32 = ("src/", "esp32/", "CMakeLists.txt", "library.json")
# The desktop-only platform never reaches an ESP32 image.
_NOT_ESP32 = ("src/platform/desktop/",)


def mechanical_gates(firmware, esp32="freshness", triggered=True):
    """The checks every lifecycle event runs, in order.

    One definition, three callers: the commit, merge and release lists differ only in how
    they treat the ESP32 firmware and whether triggers apply, so those are parameters
    rather than a reason to re-declare the whole list per script.

    `esp32`: "freshness" checks the binary is newer than its sources (a tenth of a second);
    "build" compiles for real (minutes, right before a release); "none" omits it.
    `triggered`: False makes every gate unconditional — the release event validates the
    tagged tree as a whole, where "nothing changed in src/" is not a reason to skip.
    """
    def when(*prefixes, exclude=()):
        return None if not triggered else (lambda f: touches(f, *prefixes, exclude=exclude))

    gates = [
        Gate("spec check", UV + ["moondeck/check/check_specs.py"]),
        Gate("desktop build (zero warnings)", ["cmake", "--build", host_build_dir()],
             when(*COMPILES_DESKTOP)),
        # --no-tests=error, because "no tests found" is a BROKEN GATE, not a pass: ctest exits 0
        # on an empty project, so a wrong --test-dir reported PASS in 0.1s while running nothing.
        # The flag turns that into the failure it always was.
        #
        # -C Release names the CONFIGURATION, which a multi-config generator (Visual Studio, the
        # Windows default) requires and a single-config one (Makefiles, Ninja) ignores — so one
        # spelling serves every host.
        Gate("unit tests",
             ["ctest", "--test-dir", host_build_dir(), "--output-on-failure",
              "--no-tests=error", "-C", "Release"],
             when(*COMPILES_DESKTOP)),
        # Scenarios also re-run when only a scenario JSON changed.
        Gate("scenario tests", UV + ["moondeck/scenario/run_scenario.py"],
             when(*COMPILES_DESKTOP, "test/scenarios/")),
        Gate("platform boundary", UV + ["moondeck/check/check_platform_boundary.py"],
             when("src/", exclude=("src/platform/",))),
        # The clang build above cannot see what CI sees: GCC warns where clang is silent
        # (-Wstringop-truncation, -Wformat-truncation) and does not leak standard headers
        # transitively, so a missing #include is green locally and red on every CI job. With
        # -Werror those are hard failures discovered only after a push. Compiling with the real
        # thing answers it here — see build_desktop.py --gcc for the four cycles that cost once.
        #
        # Conditional on GCC EXISTING, which on a Windows/MSVC bench it does not: an absent
        # toolchain is "this check does not apply here", the definition of SKIP, and reporting it
        # as FAIL trains the reader to scroll past a red line — the one habit a gate list cannot
        # afford. CI runs Linux, so the check still guards every push.
        Gate("GCC build (CI's toolchain)",
             UV + ["moondeck/build/build_desktop.py", "--gcc", "--tests"],
             (lambda f: _have_gcc() and (not triggered or touches(f, *COMPILES_DESKTOP)))),
        # The other half of "green here, red on CI": this bench is arm64 and has a MoonLive
        # backend, while every x86-64 desktop (Windows, Linux, Intel macOS, and CI's runners)
        # has none. A test that presumes a script compiles therefore passes locally and fails
        # only after a push. Building with the backend gated out runs the suite the way those
        # hosts see it. Triggered by MoonLive sources and by the tests that exercise them.
        Gate("no-backend build (the x86-64 desktop's view)",
             UV + ["moondeck/build/build_desktop.py", "--no-jit", "--tests"],
             lambda f: touches(f, "src/core/moonlive/", "src/light/moonlive/",
                               "src/platform/desktop/moonlive", "test/unit/core/unit_moonlive",
                               "test/unit/light/unit_MoonLive")),
        # Reports what the compiler proved about THIS change: -Wfunction-effects checks the
        # render path transitively, and `--incremental` restricts the rebuild to what the commit
        # touched, so the gate answers "did this add a blocking call" in ~1s rather than
        # re-reporting the whole 107-entry baseline. It never fails the event — a new blocking
        # call may be legitimate (a driver that must wait for hardware), so this states the
        # finding and the product owner judges it. Full picture: the clang-hotpath card.
        Gate("hot-path discipline",
             UV + ["moondeck/check/check_nonblocking.py", "--incremental"],
             when("src/")),
    ]

    if esp32 == "build":
        gates.append(Gate(f"ESP32 build ({firmware})",
                          UV + ["moondeck/build/build_esp32.py", "--firmware", firmware],
                          when(*COMPILES_ESP32, exclude=_NOT_ESP32)))
    elif esp32 == "freshness":
        gates.append(Gate(f"ESP32 firmware up to date ({firmware})",
                          UV + ["moondeck/check/check_esp32_built.py", "--firmware", firmware],
                          when(*COMPILES_ESP32, exclude=_NOT_ESP32)))
    return gates


def run_gates(gates, files, title, next_step=""):
    """Run every applicable gate, print the report, and return the exit code.

    Gates run in declaration order and do NOT stop at the first failure: the product
    owner wants the whole picture in one pass, not a bisect-by-rerun.

    `next_step` is what the caller wants said once everything is green (e.g. "waiting for
    commit now"). It rides inside the closing DONE block so the end marker is genuinely the
    last thing printed — a reader scrolling to the bottom sees the verdict, not a trailing
    remark after it.
    """
    print(f"\n{title}")
    print(f"{len(files)} changed file(s)\n")

    results = []
    for gate in gates:
        # The trigger decides first, for manual gates too: a human check that does not
        # apply to this change (an Improv smoke test on a diff that touches no
        # provisioning code) should drop out of the report rather than be listed as
        # something to confirm. Without this the event scripts had to filter their own
        # manual gates by display name, which duplicated the mechanism and broke on a
        # label rename.
        if gate.applies is not None and not gate.applies(files):
            if gate.command is None:
                continue   # a manual gate that does not apply is simply not mentioned
            results.append((gate.name, SKIP, "trigger did not match"))
            print(f"  {_C[SKIP]}{SKIP:<6}{_RESET} {gate.name}")
            continue

        if gate.command is None:
            results.append((gate.name, MANUAL, gate.manual_hint or ""))
            print(f"  {_C[MANUAL]}{MANUAL:<6}{_RESET} {gate.name}"
                  f"{' — ' + gate.manual_hint if gate.manual_hint else ''}")
            continue

        # Show what is running (a firmware build takes minutes of silence), then overwrite
        # that line with the verdict. \033[K clears to end-of-line so the longer "running"
        # text cannot leave a tail behind the shorter result line. Terminal only: piped
        # output (MoonDeck's pane, CI logs) has no cursor to move, so the placeholder would
        # stack up as a duplicate line above every result instead of being replaced.
        if _TTY:
            print(f"  ...    {gate.name}", end="\r", flush=True)
        started = time.time()
        proc = subprocess.run(gate.command, cwd=ROOT, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", env=_child_env())
        elapsed = time.time() - started
        status = PASS if proc.returncode == 0 else FAIL
        detail = "" if status == PASS else (proc.stdout + proc.stderr)
        results.append((gate.name, status, detail))
        clear = "\033[K" if _TTY else ""
        print(f"  {_C[status]}{status:<6}{_RESET} {gate.name} ({elapsed:.1f}s){clear}")

    failed = [r for r in results if r[1] == FAIL]
    for name, _, detail in failed:
        print(f"\n--- {name} output ---")
        # The tail is where a build/test failure states its reason; the head is setup noise.
        tail = detail.strip().splitlines()[-40:]
        print("\n".join(tail))

    counts = {s: sum(1 for r in results if r[1] == s) for s in (PASS, FAIL, SKIP, MANUAL)}
    print(f"\n{counts[PASS]} passed, {counts[FAIL]} failed, "
          f"{counts[SKIP]} skipped, {counts[MANUAL]} manual")

    manual = [r for r in results if r[1] == MANUAL]
    if manual:
        print("\nManual gates — the product owner confirms these:")
        for name, _, hint in manual:
            print(f"  - {name}{': ' + hint if hint else ''}")

    # An explicit end marker. Without it a reader watching a long run cannot tell "still
    # working on a silent gate" from "finished" — the gates print nothing while a firmware
    # build runs, so silence is ambiguous right up until the process exits.
    if failed:
        print("\nA failing gate blocks the event. Fix it, or skip it deliberately with a "
              "one-line reason in the commit body / PR description / release notes.")
        print(f"\n=== {title}: DONE — {counts[FAIL]} FAILED ===")
        return 1

    if next_step:
        print(f"\n{next_step}")
    print(f"\n=== {title}: DONE — all gates green ===")
    return 0
