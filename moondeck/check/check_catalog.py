#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["markdown"]
# ///
# `markdown` because this check renders a row through the BUILD's own renderer rather than
# reimplementing it, and that renderer slugs a heading with Python-Markdown's own function.
# MkDocs supplies it at build time; a bare `uv run` of this script does not.
"""Catalog card size, enforced against the pages that render as tables.

A card is one table row, and a row is read across. Text that outgrows its cell turns the
column into a ribbon: at the worst, 3,416 characters of description in a cell 44% of the
page wide. The rule and its limits are in documentation-standards.md § Card size; this
script owns the measuring.

Where the overflow goes is the point, and there are exactly two homes:

  * Behavior of the module itself -> the `///` comments in its .h, which reach the reader
    through the generated technical page the card already links. Free, and it cannot drift
    from the code it sits beside.
  * Rationale spanning modules, or guidance a user needs before choosing -> a
    `## <Name>, details` section under the same page, which the build links as
    `⌄ details`. documentation-standards.md forbids a per-module detail page, and this
    section is what it offers instead.

Parsing is the BUILD's parser (mkdocs_hooks._render_catalog_table's block loop), imported
rather than reimplemented: a second parser of the same format is the drift this check exists
to prevent.

Pinned by test/python/test_check_catalog.py. A check like this fails silently when it
breaks: a regex that stops matching prints a clean run, which reads exactly like a tree
with nothing wrong in it. So every rule here is tested firing on a page built to break it,
not only staying quiet on one that obeys.

    uv run moondeck/check/check_catalog.py
    uv run moondeck/check/check_catalog.py --baseline    # rewrite the grandfather list
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

# The limits. Description and controls share one number deliberately: two cells side by side
# with different caps makes one column reliably longer than the other, which is the shape
# this check exists to remove.
MAX_DESC = 600       # characters of prose under the name
MAX_CONTROLS = 600   # characters of control lines, all together
MAX_CONTROL = 120    # characters of any ONE control line

# A details section continues the CARD, so it is written for the same reader: someone
# choosing and using the module, not someone reading its implementation. Two limits keep
# it that way. A table wider than this stops being scannable and starts being a spec
# sheet, and its widest cell is where implementation prose hides (one Notes cell reached
# 991 characters of GDMA chains and ISR refills, which is API-page material).
MAX_DETAILS_TABLE_COLS = 4
MAX_DETAILS_CELL = 300
# Every card carries an image, no threshold: it either leads with one or it does not.
# Effects, modifiers and layouts show MOTION, so theirs is a .gif: a still frame of a
# moving effect says almost nothing about it. The rest are cards and controls, where a
# .png is sharper and smaller. The tree already follows this (66 gifs on effects.md, png
# throughout drivers and system); the rule writes down what it does.
ANIMATED_PAGES = {
    "moonmodules/light/effects.md",
    "moonmodules/light/modifiers.md",
    "moonmodules/light/layouts.md",
}

BASELINE = ROOT / "moondeck" / "check" / "catalog_baseline.txt"


def _pages():
    """The pages the build renders as card tables: the hook's own list, so a page added
    there is checked here without a second edit."""
    import mkdocs_hooks
    return sorted(mkdocs_hooks._CATALOG_PAGES)


def _cards(text: str):
    """Every `### ` block on a page, measured the way the build splits it: a line starting
    `- ` is a control, an <img> is the preview, Detail:/Tests:/Origin: are links, and the
    rest is description. A `## ` closes the block, so a details section is never counted.

    Takes the page TEXT rather than a path, so the rules can be exercised against a page
    written to break them (test/python/test_check_catalog.py) rather than only against the
    docs tree, where a rule that silently stopped matching would look like a clean run."""
    import mkdocs_hooks as h
    cur = None
    for ln in text.split("\n"):
        if h._H3_RE.match(ln):
            if cur:
                yield cur
            cur = {"title": h._H3_RE.match(ln).group("title"), "desc": 0,
                   "controls": 0, "widest": 0, "widest_text": "", "img": False,
                   "img_src": ""}
            continue
        if h._H2_RE.match(ln):
            if cur:
                yield cur
            cur = None
            continue
        if cur is None:
            continue
        if h._IMG_RE.match(ln):
            cur["img"] = True
            src = re.search(r'src="([^"]+)"', ln)
            cur["img_src"] = src.group(1) if src else ""
            continue
        if h._ORIGIN_RE.match(ln) or h._TESTS_RE.match(ln) \
                or h._TESTS_MULTI_RE.match(ln) or h._DETAIL_RE.match(ln):
            continue
        if h._PARAM_RE.match(ln):
            text = re.sub(r"^-\s+", "", ln.strip())
            cur["controls"] += len(text)
            if len(text) > cur["widest"]:
                cur["widest"], cur["widest_text"] = len(text), text
        elif ln.strip():
            cur["desc"] += len(ln.strip())
    if cur:
        yield cur


def _structure(text: str, rel: str):
    """The two rules that keep a page readable as a whole rather than card by card.

    A `## <Name>, details` section is a CONTINUATION of a card, so it belongs after every
    card on the page. Placed between two cards it splits the table in half, and a reader
    scanning the rows meets a wall of prose where the next row should be.

    And the heading form is load-bearing: the build links a card to its details by matching
    `, details` exactly, so `Infrared: details` renders a section that nothing links to. A
    COMMA rather than an em-dash, because em-dashes are banned repo-wide and a form the
    build requires must not be the one character the prose rules forbid.
    """
    import mkdocs_hooks as h
    out = []
    lines = text.split("\n")

    last_card = max((i for i, l in enumerate(lines) if h._H3_RE.match(l)), default=-1)
    card_names = {re.split(r"\s+[^\w(]", h._H3_RE.match(l).group("title"))[0].strip()
                  for l in lines if h._H3_RE.match(l)}

    # One section per card. Two headings with the same name both slug to the same anchor,
    # so the row's `More:` link resolves to whichever MkDocs emits first and the other is
    # unreachable. A sweep that adds a section where one already exists is exactly how
    # that happens, silently.
    seen = set()

    for i, ln in enumerate(lines):
        if not ln.startswith("## ") or ln.startswith("###"):
            continue
        # A details section by intent: any `## ` heading whose last word is "details".
        if not re.search(r"\bdetails\s*$", ln, re.I):
            continue
        m = h._DETAILS_RE.match(ln)
        if not m:
            out.append((f"{rel}::{ln[3:].strip()}",
                        "details heading must read `## <Name>, details` "
                        "(a comma, never an em-dash), or the build links nothing to it"))
            continue
        if i < last_card:
            out.append((f"{rel}::{m.group('name')}",
                        f"details section sits at line {i + 1}, above the last card "
                        f"(line {last_card + 1}): move it below every card"))
        if m.group("name") not in card_names:
            out.append((f"{rel}::{m.group('name')}",
                        "details heading names no card on the page, so no row links to it"))
        if m.group("name") in seen:
            out.append((f"{rel}::{m.group('name')}",
                        "a second details section with this name: both slug to one anchor, "
                        "so one of them is unreachable"))
        seen.add(m.group("name"))
    return out


# The second column links to exactly three places: the test inventory, the generated API
# page, and the card's details section. Three because all three are followed often enough
# to earn a standing position, so they sit identically on every card and the eye learns
# them once. Everything else a card points at (a how-to, an explanation, a sibling module)
# is a link the DESCRIPTION makes in a sentence, where the reader meets it in context
# rather than as a bare label under the controls.
SECOND_COLUMN_LINKS = ("Tests:", "API:", "Details:")
_LINK_LABEL_RE = re.compile(r"\*\*([A-Z][A-Za-z ]{1,12}):\*\*")


def _rendered_links(rel: str, text: str):
    """Which labelled links the build puts in the second column, per card."""
    import mkdocs_hooks as h
    out = []
    for row in h._render_catalog_table(text).split("\n"):
        if not row.startswith("| ") or row.startswith("|--") or "| Module |" in row:
            continue
        cells = row.strip("| ").split(" | ")
        if len(cells) < 2:
            continue
        for label in _LINK_LABEL_RE.findall(cells[1]):
            if f"{label}:" not in SECOND_COLUMN_LINKS:
                name = re.search(r'class="mm-name">([^<]*)', cells[0])
                out.append((f"{rel}::{name.group(1) if name else '?'}",
                            f"second column links to `{label}:`, but it carries only "
                            f"Tests and API: put the rest in the description"))
    return out


def _details_tables(rel: str, text: str):
    """Tables inside a details section: narrow enough to read, and no cell carrying an
    essay. A wide table with one huge column is the shape prose takes when it is put in a
    grid to look organized."""
    out = []
    lines = text.split("\n")
    sec = None
    for i, ln in enumerate(lines):
        m = re.match(r"^## (.+?), details$", ln)
        if m:
            sec = m.group(1)
            continue
        if ln.startswith("## "):
            sec = None
            continue
        if sec is None or not ln.startswith("|"):
            continue
        if set(ln.replace("|", "").strip()) > set("-: "):
            continue                       # not the separator row
        cols = lines[i - 1].count("|") - 1
        widest = 0
        for body in lines[i + 1:]:
            if not body.startswith("|"):
                break
            for cell in body.strip("|").split("|"):
                widest = max(widest, len(cell.strip()))
        if cols > MAX_DETAILS_TABLE_COLS:
            out.append((f"{rel}::{sec}",
                        f"details table has {cols} columns (max {MAX_DETAILS_TABLE_COLS})"))
        if widest > MAX_DETAILS_CELL:
            out.append((f"{rel}::{sec}",
                        f"details table cell is {widest} characters "
                        f"(max {MAX_DETAILS_CELL}): prose in a grid"))
    # One entry per table+rule, however many rows it has: a table reported once per body
    # row turned a single wide table into five identical lines of output.
    return list(dict.fromkeys(out))


def _violations():
    out = []
    for rel in _pages():
        path = ROOT / "docs" / rel
        if not path.exists():
            continue
        text = path.read_text()
        out.extend(_structure(text, rel))
        out.extend(_rendered_links(rel, text))
        out.extend(_details_tables(rel, text))
        for c in _cards(text):
            key = f"{rel}::{c['title']}"
            if c["desc"] > MAX_DESC:
                out.append((key, f"description {c['desc']} > {MAX_DESC}"))
            if c["controls"] > MAX_CONTROLS:
                out.append((key, f"controls {c['controls']} > {MAX_CONTROLS}"))
            if c["widest"] > MAX_CONTROL:
                out.append((key, f"one control {c['widest']} > {MAX_CONTROL}: "
                                 f"{c['widest_text'][:60]}..."))
            # A card leads with its picture. The image is the first thing the eye reaches
            # on a row, and a card without one starts with a name against blank space,
            # which reads as a gap rather than as a module that happens to be invisible.
            if not c["img"]:
                out.append((key, "no image: every card leads with one"))
            elif rel in ANIMATED_PAGES and not c["img_src"].lower().endswith(".gif"):
                out.append((key, f"image is {c['img_src'].rsplit('.', 1)[-1]}, not a gif: "
                                 f"an effect, modifier or layout shows motion"))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", action="store_true",
                    help="rewrite the grandfather list from what the tree holds now")
    args = ap.parse_args()

    found = _violations()

    if args.baseline:
        BASELINE.write_text(
            "# Cards over the size limits when the check was introduced. Each line is one\n"
            "# card+rule that check_catalog.py tolerates. The list only SHRINKS: a card\n"
            "# edited back under the limit loses its line, and nothing is ever added.\n"
            "# Empty this file and the check is enforced everywhere. Regenerate: --baseline\n"
            + "".join(f"{k}\t{v.split(':')[0]}\n" for k, v in sorted(found)))
        print(f"Catalog baseline: {len(found)} tolerated violation(s) written to "
              f"{BASELINE.relative_to(ROOT)}")
        return 0

    # A baseline line is card + rule NAME, not the number: a card already over the limit
    # must not be allowed to grow further unnoticed, so the tolerated entry still fails if
    # its rule changes kind. The number is deliberately excluded so an EDIT that shortens a
    # card does not need a baseline refresh.
    tolerated = set()
    if BASELINE.exists():
        for ln in BASELINE.read_text().split("\n"):
            if ln.strip() and not ln.startswith("#"):
                tolerated.add(tuple(ln.split("\t")))

    fresh = [(k, v) for k, v in found if (k, v.split(":")[0].split(" ")[0]) not in
             {(a, b.split(" ")[0]) for a, b in tolerated}]

    if not fresh:
        print(f"Catalog check: {len(found)} card(s) over the limits, all in the baseline. "
              f"Limits: description {MAX_DESC}, controls {MAX_CONTROLS}, "
              f"one control {MAX_CONTROL}.")
        return 0

    print(f"Catalog check: {len(fresh)} card(s) over the limits.\n")
    for key, why in fresh:
        page, title = key.split("::", 1)
        print(f"  {page}\n    {title}: {why}")
    print("\nMove the overflow, do not trim it: module behavior into the header's ///"
          "\n(the technical page the card links), cross-module rationale into a"
          "\n`## <Name>, details` section on the same page."
          "\nRules: docs/contributing/documentation-standards.md § Card size.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
