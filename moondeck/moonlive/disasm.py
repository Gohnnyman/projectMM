#!/usr/bin/env python3
"""Disassemble the machine code MoonLive emits for a script, on any backend, without a device.

Why this exists: an emitted-code bug on a device shows up as "the script did nothing" — it
compiles, reports no error, and places no lights. Reasoning about hand-written encoders from that
symptom is guesswork; five hypotheses in a row were wrong before this tool read the instructions and
answered it in one run. The bug was `Mov` lowering to add-immediate-zero, which Xtensa cannot encode
(the ISA reuses that slot for -1), so a loop counter started at -1 and every loop exited immediately.

The per-ISA assemblers are ordinary C++ behind a target guard, so they build and run on the host:
this compiles a script through a REAL backend and pipes the bytes to that ISA's objdump.

    uv run moondeck/moonlive/disasm.py "for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"
    uv run moondeck/moonlive/disasm.py --isa riscv moonlive/effects/plasma.mle effect
    uv run moondeck/moonlive/disasm.py --isa all moonlive/layouts/grid.mll layout

Note there is no x86_64 backend to disassemble: the desktop assembler is arm64-only
(`moonlive_asm_host.cpp`), and on an x86 host a compile fails cleanly and the module renders dark
(backlogged). The three backends below are therefore every backend that exists.
"""

import argparse
import binascii
import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL_SRC = os.path.join(ROOT, "moondeck", "moonlive", "emit_isa.cpp")

# Per ISA: the emitter's -D flag, the objdump to disassemble with, and objdump's -m architecture.
# `toolchain` is a glob because the ESP-IDF toolchains are installed per version; `None` means the
# host's own objdump already understands this ISA (arm64 on an Apple Silicon machine).
ISAS = {
    "xtensa": {
        "define": "MM_EMIT_XTENSA",
        "toolchain": "~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump",
        "machine": "xtensa",
    },
    "riscv": {
        "define": "MM_EMIT_RISCV",
        "toolchain": "~/.espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-objdump",
        "machine": "riscv:rv32",
    },
    "arm64": {
        "define": "MM_EMIT_ARM64",
        "toolchain": None,
        "machine": "aarch64",
    },
}


def objdump(isa: str) -> str:
    """The objdump that understands `isa`, wherever its toolchain was installed."""
    pattern = ISAS[isa]["toolchain"]
    if pattern is None:
        # llvm-objdump ships with the Xcode command line tools and knows aarch64. It is REQUIRED
        # rather than merely preferred on macOS: the `objdump` on PATH there is Apple's, which
        # rejects the raw-binary flags (`-b binary`) this tool depends on. An ESP-IDF toolchain
        # brings its own llvm-objdump, so check those too before giving up.
        found = subprocess.run(["which", "llvm-objdump"], capture_output=True, text=True)
        if found.returncode == 0:
            return found.stdout.strip()
        # On macOS llvm-objdump ships inside the Xcode command line tools but is NOT on PATH;
        # xcrun is how the platform expects you to find it.
        found = subprocess.run(["xcrun", "-f", "llvm-objdump"], capture_output=True, text=True)
        if found.returncode == 0 and found.stdout.strip():
            return found.stdout.strip()
        sys.exit("llvm-objdump not found — needed to disassemble arm64")
    hits = glob.glob(os.path.expanduser(pattern))
    if not hits:
        sys.exit(f"objdump for {isa} not found — install the matching ESP-IDF toolchain")
    return sorted(hits)[-1]


def disassemble(isa: str, script: str, binding: str) -> int:
    """Compile `script` through the `isa` backend and print its disassembly."""
    with tempfile.TemporaryDirectory() as tmp:
        emitter = os.path.join(tmp, "emit")
        build = subprocess.run(
            ["c++", "-std=c++20", "-O0", "-I", os.path.join(ROOT, "src"),
             "-I", os.path.join(ROOT, "src", "platform", "desktop"),
             "-D", ISAS[isa]["define"],
             TOOL_SRC, os.path.join(ROOT, "src", "core", "moonlive", "MoonLiveCompiler.cpp"),
             # Every backend runs the register allocator before lowering, so the pass comes along
             # too — without it the tool fails to link on spillToBudget.
             os.path.join(ROOT, "src", "core", "moonlive", "MoonLiveSpill.cpp"),
             # The IR sizes its op array with platform::alloc, so the platform implementation has
             # to come along — the compiler is no longer self-contained.
             os.path.join(ROOT, "src", "platform", "desktop", "platform_desktop.cpp"),
             "-o", emitter],
            capture_output=True, text=True)
        if build.returncode != 0:
            print(build.stderr[:2000])
            return 1

        run = subprocess.run([emitter, script, binding], capture_output=True, text=True)
        print(run.stdout.split("\n")[0])          # the script
        if run.returncode != 0:
            print(run.stdout.strip() or run.stderr.strip())
            return 1

        # Keep only lines that ARE hex. The emitter echoes the script as a `# …` comment, but a
        # MULTI-LINE script only gets `#` on its first line, so a "not a comment" filter fed the
        # remaining source lines into unhexlify and died on an odd-length string.
        def is_hex(line: str) -> bool:
            s = line.replace(" ", "")
            return bool(s) and all(c in "0123456789abcdefABCDEF" for c in s)

        hexbytes = "".join(line for line in run.stdout.splitlines() if is_hex(line))
        raw = binascii.unhexlify(hexbytes.replace(" ", ""))
        print(f"# {len(raw)} bytes\n")

        binpath = os.path.join(tmp, "code.bin")
        with open(binpath, "wb") as f:
            f.write(raw)
        tool = objdump(isa)
        if os.path.basename(tool).startswith("llvm-"):
            # llvm-objdump has NO raw-binary mode (no `-b binary`), so the bytes are assembled into
            # a real object file first — `.incbin` is the one-line way to put a blob in a .text
            # section the disassembler will then walk.
            asm = os.path.join(tmp, "blob.s")
            with open(asm, "w") as f:
                f.write(f'.text\n.incbin "{binpath}"\n')
            obj = os.path.join(tmp, "blob.o")
            wrap = subprocess.run(["cc", "-c", asm, "-o", obj], capture_output=True, text=True)
            if wrap.returncode != 0:
                print(wrap.stderr.strip()[:600])
                return 1
            argv = [tool, "-d", obj]
        else:
            argv = [tool, "-D", "-b", "binary", "-m", ISAS[isa]["machine"], binpath]
        dis = subprocess.run(argv, capture_output=True, text=True)
        if dis.returncode != 0:
            print(dis.stderr.strip()[:600])
            return 1
        # Skip objdump's file header; the instructions start after the section line.
        lines = dis.stdout.splitlines()
        start = next((i for i, line in enumerate(lines) if line.startswith("00000000")), 0)
        print("\n".join(lines[start:]))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--isa", default="xtensa", choices=[*ISAS, "all"],
                    help="which backend to emit through (default: xtensa; 'all' runs every one)")
    ap.add_argument("script", nargs="?",
                    default="for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }",
                    help="script text, or a path to a .mlv file")
    # Which binding's system variables to compile against: layout (default), effect or modifier.
    # They are different vocabularies, not nested ones — a layout may use `x`/`y` as loop counters
    # precisely because it is NOT handed them, so the binding has to be stated.
    ap.add_argument("binding", nargs="?", default="layout",
                    choices=["layout", "effect", "modifier"],
                    help="which system-variable vocabulary to compile against (default: layout)")
    args = ap.parse_args()

    script = args.script
    # A path is read as a file; anything else is the script text itself.
    if os.path.isfile(script):
        with open(script) as f:
            script = f.read()

    isas = list(ISAS) if args.isa == "all" else [args.isa]
    rc = 0
    for isa in isas:
        if len(isas) > 1:
            print(f"\n{'=' * 60}\n== {isa}\n{'=' * 60}")
        rc |= disassemble(isa, script, args.binding)
    return rc


if __name__ == "__main__":
    sys.exit(main())
