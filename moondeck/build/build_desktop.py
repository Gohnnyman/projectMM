#!/usr/bin/env python3
"""Build the desktop target into a host-named build directory.

``build/macos/`` on macOS arm64, ``build/linux/`` on Linux, ``build/windows/``
on Windows — so a single machine could (in principle) build multiple host
flavours without one wiping the other, and so the layout matches the
ESP32 side (``build/esp32-<board>/``, one dir per target).

Builds ONE target, not the whole project: the firmware binary (``projectMM``) by
default, or the test binaries (``--tests`` → ``mm_tests`` + ``mm_scenarios``).
Keeping them separate means the "just give me the binary" build doesn't wait on
the ~130-file test suite (and vice-versa).
"""

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def host_build_dir() -> str:
    """Map platform.system() to the desktop build-dir name.

    Returns a slash-string (not Path) so the cmake -B argument stays
    portable across shells.
    """
    sys_name = platform.system()
    return {
        "Darwin":  "build/macos",
        "Linux":   "build/linux",
        "Windows": "build/windows",
    }.get(sys_name, f"build/{sys_name.lower()}")


def gcc_pair():
    """(cc, cxx) for a real GCC, or exit with how to get one.

    WHY THIS MODE EXISTS: CI compiles with GCC, every local gate compiles with clang, and the two
    disagree — GCC emits -Wstringop-truncation / -Wformat-truncation / -Wformat-zero-length /
    -Wunused-function where clang stays silent, and clang leaks standard headers transitively where
    GCC does not. With -Werror a hard rule, every divergence is a CI failure you cannot see locally.
    That cost four push-and-discover cycles once; this flag is so it costs zero.
    """
    for cxx in ("g++-16", "g++-15", "g++-14", "g++-13"):
        if shutil.which(cxx):
            return cxx.replace("g++", "gcc"), cxx
    sys.exit("no GCC found — `brew install gcc` (macOS) or `apt install g++` (Linux).\n"
             "Note /usr/bin/g++ on macOS is clang in disguise; it will NOT catch these.")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gcc", action="store_true",
                    help="build with GCC instead of the default compiler — the toolchain CI uses. "
                         "Catches the warnings clang does not emit.")
    ap.add_argument("--tests", action="store_true",
                    help="compile the test binaries (mm_tests + mm_scenarios) instead of the firmware. "
                         "The default build makes only projectMM; the ~130 test units are a separate, "
                         "slower compile, run afterwards by test_desktop.py / run_scenario.py.")
    args = ap.parse_args()

    bdir = host_build_dir()
    is_windows = platform.system() == "Windows"
    extra = []
    build_type = "Release"
    if args.gcc:
        cc, cxx = gcc_pair()
        bdir = "build/gcc"          # its own dir — do not clobber the clang build's cache
        # DEBUG, because that is what CI's job builds. Release turns on more inlining, which lets GCC
        # prove more about buffer sizes and emit ~17 further -Wformat-truncation/-Wstringop-* warnings
        # that CI never sees — real, but a separate cleanup, not a merge gate. Match CI, not more.
        build_type = "Debug"
        extra = [f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}"]
        print(f"Using GCC ({cxx}) in Debug — the exact toolchain + build type CI uses.")
    what = "test binaries" if args.tests else "desktop target"
    print(f"Building {what} into {bdir}/ ...")
    # CMAKE_BUILD_TYPE is honoured by single-config generators (Ninja, Make).
    # Visual Studio is multi-config and ignores it — we pass --config Release at
    # build time below. Setting CMAKE_BUILD_TYPE on multi-config is harmless.
    r = subprocess.run(
        ["cmake", "-B", bdir, f"-DCMAKE_BUILD_TYPE={build_type}", *extra],
        cwd=ROOT,
    )
    if r.returncode != 0:
        sys.exit(r.returncode)

    # Build a SPECIFIC target, never the whole "all" — the firmware and the ~130 test translation units are
    # separate concerns. Default builds only projectMM (the "just give me the desktop binary" path stays
    # fast; a header edit no longer drags the test suite through the compiler). `--tests` builds the test
    # binaries instead, which test_desktop.py runs (mm_tests) and run_scenario.py runs (mm_scenarios).
    targets = ["mm_tests", "mm_scenarios"] if args.tests else ["projectMM"]
    build_cmd = ["cmake", "--build", bdir, "--target", *targets]
    if is_windows:
        build_cmd += ["--config", "Release"]
    r = subprocess.run(build_cmd, cwd=ROOT)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
