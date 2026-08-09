#!/usr/bin/env python3
"""Disassemble the machine code MoonLive emits for a script, without a device.

Why this exists: an emitted-code bug on a device shows up as "the script did nothing" — it
compiles, reports no error, and places no lights. Reasoning about hand-written encoders from that
symptom is guesswork; five hypotheses in a row were wrong before this tool read the instructions and
answered it in one run. The bug was `Mov` lowering to add-immediate-zero, which Xtensa cannot encode
(the ISA reuses that slot for -1), so a loop counter started at -1 and every loop exited immediately.

The per-ISA assemblers are ordinary C++ behind a target guard, so they build and run on the host:
this compiles a script through the real Xtensa backend and pipes the bytes to the ESP-IDF objdump.

    uv run moondeck/moonlive/disasm.py "for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"
"""

import binascii
import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL_SRC = os.path.join(ROOT, "moondeck", "moonlive", "emit_xtensa.cpp")


def objdump() -> str:
    """The ESP-IDF Xtensa objdump, wherever the toolchain was installed."""
    hits = glob.glob(os.path.expanduser(
        "~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump"))
    if not hits:
        sys.exit("xtensa-esp32s3-elf-objdump not found — install the ESP-IDF Xtensa toolchain")
    return sorted(hits)[-1]


def main() -> int:
    script = sys.argv[1] if len(sys.argv) > 1 else \
        "for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"

    with tempfile.TemporaryDirectory() as tmp:
        emitter = os.path.join(tmp, "emit")
        build = subprocess.run(
            ["c++", "-std=c++20", "-O0", "-I", os.path.join(ROOT, "src"),
             "-I", os.path.join(ROOT, "src", "platform", "desktop"),
             TOOL_SRC, os.path.join(ROOT, "src", "core", "moonlive", "MoonLiveCompiler.cpp"),
             "-o", emitter],
            capture_output=True, text=True)
        if build.returncode != 0:
            print(build.stderr[:2000])
            return 1

        run = subprocess.run([emitter, script], capture_output=True, text=True)
        print(run.stdout.split("\n")[0])          # the script
        if run.returncode != 0:
            print(run.stdout.strip() or run.stderr.strip())
            return 1

        hexbytes = "".join(l for l in run.stdout.splitlines() if not l.startswith("#"))
        raw = binascii.unhexlify(hexbytes.replace(" ", ""))
        print(f"# {len(raw)} bytes\n")

        binpath = os.path.join(tmp, "code.bin")
        with open(binpath, "wb") as f:
            f.write(raw)
        dis = subprocess.run([objdump(), "-D", "-b", "binary", "-m", "xtensa", binpath],
                             capture_output=True, text=True)
        # Skip objdump's file header; the instructions start after the section line.
        lines = dis.stdout.splitlines()
        start = next((i for i, l in enumerate(lines) if l.startswith("00000000")), 0)
        print("\n".join(lines[start:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
