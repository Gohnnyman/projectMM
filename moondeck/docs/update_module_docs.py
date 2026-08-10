#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Insert screenshot references into docs/moonmodules/**/*.md files.

For each .md file, if a matching screenshot exists at
docs/assets/<type-folder>/<TypeName>.png and the file doesn't already
contain a screenshot reference, insert one line after the first heading:

    ![<TypeName> controls](../../../assets/<type-folder>/<TypeName>.png)

The relative path is computed from the .md file's location so links
work both on GitHub and in the MoonDeck /api/docs/ renderer.

Also reports any screenshots (PNG or GIF) under docs/assets/ that
are not referenced anywhere in docs/.

Usage:
    uv run moondeck/docs/update_module_docs.py [--dry-run]
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DOCS_DIR = ROOT / "docs" / "moonmodules"
ASSETS = ROOT / "docs" / "assets"
UI_DIR = ASSETS / "ui"   # tooling/installer/full-page shots (not per-module)

# A module's asset subfolder (domain/type), mirroring src — see folder-structure-proposal.md.
def asset_dir_for(type_name: str):
    if type_name.endswith("Effect"):
        return ASSETS / "light" / "effects"
    if type_name.endswith("Modifier"):
        return ASSETS / "light" / "modifiers"
    if type_name.endswith("Layout"):
        return ASSETS / "light" / "layouts"
    if type_name.endswith("Driver"):
        return ASSETS / "light" / "drivers"
    if type_name in ("Layouts", "Effects", "Drivers"):
        return ASSETS / "light"
    return ASSETS / "core"

SCREENSHOT_RE = re.compile(r'!\[.*?\]\(.*?assets/.*?\.(?:png|jpe?g)\)')
GIF_RE        = re.compile(r'!\[.*?\]\(.*?assets/.*?\.gif.*?\)')

# Extra screenshots with fixed placement in specific doc files.
# Each entry: (filename, doc_rel_path, anchor_text)
#   filename:     file in docs/assets/ui/
#   doc_rel_path: repo-relative path to the doc file
#   anchor_text:  heading/line after which to insert (exact prefix match)
EXTRA_SHOTS = [
    ("moondeck_desktop.png",    "README.md",           "![MoonDeck]"),
    ("moondeck_desktop.png",    "moondeck/MoonDeck.md", "## Desktop Tab"),
    ("moondeck_esp32.png", "moondeck/MoonDeck.md", "## ESP32 Tab"),
    ("moondeck_live.png",  "moondeck/MoonDeck.md", "## Live Tab"),
    ("installer.png",      "README.md",           "**ESP32 — flash from your browser.**"),
    ("installer.png",      "moondeck/MoonDeck.md", "### preview_installer"),
]


def relative_path(md_file: Path, asset: Path) -> str:
    """Return a relative path from the .md file's directory to the asset."""
    return "../" * (len(md_file.parent.relative_to(ROOT).parts) - 1) + \
           str(asset.relative_to(ROOT / "docs"))


def type_name_from_md(md_file: Path) -> str:
    """Derive the expected TypeName from the filename (e.g. RainbowEffect.md → RainbowEffect)."""
    return md_file.stem


def resolve_asset(path: Path) -> Path | None:
    """`path` as it is ACTUALLY spelled on disk, or None when no such file exists.

    `Path.exists()` answers through the filesystem, and macOS's is case-insensitive: it reports
    True for `assets/core/layouts.png` when the file is `Layouts.png`. The link written from that
    path then resolves locally and 404s on the case-sensitive Linux runner that builds the
    published site — a failure that cannot reproduce on the machine that produced it.

    So the parent directory is listed and matched case-insensitively, and the REAL name is
    returned. Catalog pages are the case that needs it: `layouts.md` has a lowercase stem while
    the screenshot follows the `<TypeName>.png` convention.
    """
    if path.exists() and path.name in {p.name for p in path.parent.iterdir()}:
        return path            # exact match, including case
    if not path.parent.is_dir():
        return None
    lowered = path.name.lower()
    return next((p for p in sorted(path.parent.iterdir()) if p.name.lower() == lowered), None)


def insert_extra_shot(doc_path: Path, filename: str, anchor: str,
                      dry_run: bool) -> bool:
    """Insert an image reference for filename into doc_path after anchor.

    Skips if filename is already mentioned anywhere in the file.
    Returns True if a change was made.
    """
    text = doc_path.read_text(encoding="utf-8")
    # Skip only if the file already has an image reference for this filename,
    # not just a prose mention of the filename.
    if re.search(r'!\[.*?\]\(.*?' + re.escape(filename) + r'.*?\)', text):
        return False

    # Relative path from the doc file's directory to the screenshot.
    depth = len(doc_path.parent.relative_to(ROOT).parts)
    rel = "../" * depth + f"docs/assets/ui/{filename}"
    label = filename.replace(".png", "").replace("_", " ").title()
    img_line = f"\n![{label}]({rel})\n\n"

    lines = text.splitlines(keepends=True)
    insert_at = None
    for i, line in enumerate(lines):
        if line.startswith(anchor):
            insert_at = i + 1
            break
    if insert_at is None:
        # Fall back to after the first heading.
        for i, line in enumerate(lines):
            if line.startswith("#"):
                insert_at = i + 1
                break
    if insert_at is None:
        insert_at = len(lines)

    # Skip blank lines already after the anchor.
    while insert_at < len(lines) and lines[insert_at].strip() == "":
        insert_at += 1

    lines.insert(insert_at, img_line)
    if not dry_run:
        doc_path.write_text("".join(lines), encoding="utf-8")
    return True


def insert_assets(md_file: Path, png: Path, gif: Path | None, dry_run: bool) -> bool:
    """Insert PNG (and optional GIF) after the first heading if not already present.

    If the file already has a PNG reference but no GIF reference, appends the
    GIF on the line immediately after the existing PNG reference.
    Returns True if any change was made.
    """
    text = md_file.read_text(encoding="utf-8")
    changed = False

    has_png = bool(SCREENSHOT_RE.search(text))
    has_gif = bool(GIF_RE.search(text)) if gif else True

    if has_png and has_gif:
        return False  # nothing to do

    type_name = type_name_from_md(md_file)
    lines = list(text.splitlines(keepends=True))

    if not has_png:
        # Find insertion point: after first heading, skipping blank lines.
        insert_at = 0
        for i, line in enumerate(lines):
            if line.startswith("#"):
                insert_at = i + 1
                break
        while insert_at < len(lines) and lines[insert_at].strip() == "":
            insert_at += 1

        rel_png = relative_path(md_file, png)
        img_lines = [f"![{type_name} controls]({rel_png})\n"]
        if gif and not has_gif:
            rel_gif = relative_path(md_file, gif)
            img_lines.append(f"![{type_name} preview]({rel_gif})\n")
            has_gif = True  # mark inserted
        img_lines.append("\n")  # blank line after image block
        lines[insert_at:insert_at] = img_lines
        changed = True

    if not has_gif and gif:
        # PNG reference exists — insert GIF on the line after it.
        text_so_far = "".join(lines)
        m = SCREENSHOT_RE.search(text_so_far)
        if m:
            # Find which line contains the match and insert after it.
            pos = text_so_far[:m.end()].count("\n")
            rel_gif = relative_path(md_file, gif)
            lines.insert(pos + 1, f"![{type_name} preview]({rel_gif})\n")
            changed = True

    if changed and not dry_run:
        md_file.write_text("".join(lines), encoding="utf-8")
    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dry-run", action="store_true",
                        help="Print what would change without writing files")
    args = parser.parse_args()

    updated, skipped_no_screenshot, skipped_has_ref = [], [], []

    # Fix spacing around image blocks: ensure one blank line after the last
    # image in a block, and collapse any triple+ blank lines to one blank.
    img_line_re = re.compile(r'^!\[.*?\]\(.*?assets/.*?\)\n', re.MULTILINE)
    for md_file in sorted(DOCS_DIR.rglob("*.md")):
        original = md_file.read_text(encoding="utf-8")
        lines = original.splitlines(keepends=True)
        i = 0
        while i < len(lines):
            if img_line_re.match(lines[i]):
                next_i = i + 1
                if next_i < len(lines) and lines[next_i].strip() != "" \
                        and not img_line_re.match(lines[next_i]):
                    lines.insert(next_i, "\n")
            i += 1
        fixed = re.sub(r'\n{3,}', '\n\n', "".join(lines))
        if fixed != original:
            if not args.dry_run:
                md_file.write_text(fixed, encoding="utf-8")
            verb = "would fix spacing" if args.dry_run else "fixed spacing"
            print(f"  {verb}: {md_file.relative_to(ROOT)}")

    for md_file in sorted(DOCS_DIR.rglob("*.md")):
        type_name = type_name_from_md(md_file)
        # Resolved through the directory listing, not Path.exists(), so the link carries the name
        # as it is really spelled — see resolve_asset.
        png = resolve_asset(asset_dir_for(type_name) / f"{type_name}.png")
        gif = resolve_asset(asset_dir_for(type_name) / f"{type_name}.gif")

        if png is None:
            skipped_no_screenshot.append(md_file.relative_to(ROOT))
            continue

        changed = insert_assets(md_file, png, gif, args.dry_run)
        if changed:
            rel = md_file.relative_to(ROOT)
            verb = "would update" if args.dry_run else "updated"
            print(f"  {verb}: {rel}")
            updated.append(rel)
        else:
            skipped_has_ref.append(md_file.relative_to(ROOT))

    # --- Extra shots: insert MoonDeck tab + installer images into fixed doc files ---
    extra_updated = []
    for filename, doc_rel, anchor in EXTRA_SHOTS:
        screenshot = UI_DIR / filename
        if not screenshot.exists():
            continue
        doc_path = ROOT / doc_rel
        if not doc_path.exists():
            continue
        changed = insert_extra_shot(doc_path, filename, anchor, args.dry_run)
        if changed:
            verb = "would insert" if args.dry_run else "inserted"
            print(f"  {verb} {filename} → {doc_rel}")
            extra_updated.append(f"{filename} → {doc_rel}")

    print(f"\n{'─'*50}")
    print(f"Updated  : {len(updated)}")
    print(f"Extra inserted: {len(extra_updated)}")
    print(f"No screenshot : {len(skipped_no_screenshot)}")
    print(f"Already has ref: {len(skipped_has_ref)}")

    if skipped_no_screenshot:
        print("\nNo screenshot found for:")
        for p in skipped_no_screenshot:
            print(f"  {p}")

    # --- Unreferenced screenshots check ---
    # Collect all *.md text across docs/ and moondeck/ (MoonDeck.md lives there).
    all_docs_text = ""
    for md in sorted((ROOT / "docs").rglob("*.md")):
        all_docs_text += md.read_text(encoding="utf-8")
    for md in sorted((ROOT / "moondeck").rglob("*.md")):
        all_docs_text += md.read_text(encoding="utf-8")
    all_docs_text += (ROOT / "README.md").read_text(encoding="utf-8")

    unreferenced = []
    for f in sorted(ASSETS.rglob("*")):
        if f.suffix.lower() not in (".png", ".gif"):
            continue
        if f.name not in all_docs_text:
            unreferenced.append(f.name)

    print(f"\nUnreferenced screenshots: {len(unreferenced)}")
    for name in unreferenced:
        print(f"  {name}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
