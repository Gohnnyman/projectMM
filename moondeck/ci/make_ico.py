# /// script
# requires-python = ">=3.9"
# dependencies = ["pillow"]
# ///
"""Turn a PNG into a Windows .ico.

Run: uv run moondeck/ci/make_ico.py <source.png> <out.ico>

Pillow is declared inline (PEP 723) rather than added to the project, because this is the only
thing in the tree that needs it and it runs at most once per build. uv fetches it on demand.

The source is the same web-installer/favicon.png the macOS .icns is derived from, so the mark has
one home. A resize is unavoidable: ICO stores each image's dimensions in a SINGLE byte (with 0
meaning 256), so the 320x320 source cannot be embedded as-is at any size.
"""
import sys
from pathlib import Path

# Every size Windows picks between: the small ones appear in Explorer lists and the title bar, 256
# in large-icon views and the installer. Shipping all of them beats letting Windows downscale one,
# which it does badly at small sizes.
SIZES = [16, 24, 32, 48, 64, 128, 256]


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_ico.py <source.png> <out.ico>", file=sys.stderr)
        return 2

    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    if not src.is_file():
        print(f"make_ico: no such file: {src}", file=sys.stderr)
        return 1

    from PIL import Image

    # RGBA throughout: the source is palette-mode, and a palette image saved into an .ico loses
    # its transparency to a black square behind the mark.
    img = Image.open(src).convert("RGBA")
    dst.parent.mkdir(parents=True, exist_ok=True)
    img.save(dst, format="ICO", sizes=[(s, s) for s in SIZES])
    print(f"make_ico: {src.name} {img.size[0]}x{img.size[1]} -> {dst} ({dst.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
