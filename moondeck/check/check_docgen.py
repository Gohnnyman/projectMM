#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["markdown"]
# ///
# `markdown` because this check renders a row through the BUILD's own renderer rather than
# reimplementing it, and that renderer slugs a heading with Python-Markdown's own function.
# MkDocs supplies it at build time; a bare `uv run` of this script does not.
"""The documentation the build GENERATES, held to the standards that describe it.

Two surfaces, one check, because they are one pipeline: the hand-written catalog pages
that render as card tables, and the `///` comments in the headers that become the
technical pages beside them. A rule about the first is a rule about a cell width; a rule
about the second is a rule doxygen enforces by silently dropping what it cannot read.
Prose is NOT here: em-dashes, spelling and weasel words are Vale's, and a second prose
checker is the thing the documentation sweep exists to delete.

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

Pinned by test/python/test_check_docgen.py. A check like this fails silently when it
breaks: a regex that stops matching prints a clean run, which reads exactly like a tree
with nothing wrong in it. So every rule here is tested firing on a page built to break it,
not only staying quiet on one that obeys.

    uv run moondeck/check/check_docgen.py
    uv run moondeck/check/check_docgen.py --baseline    # rewrite the grandfather list
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
# From the hook, the one home: the build renders these pages and screenshot_modules
# captures for them, so a third copy here is a third thing to forget.
import mkdocs_hooks as _hooks  # noqa: E402
ANIMATED_PAGES = _hooks.ANIMATED_PAGES

# ---------------------------------------------------------------------------
# The OTHER generated surface: the `///` comments that become the technical pages.
#
# A card is read across a row; a member comment is read beside the thing it describes,
# and the generated page shows its first sentence as the summary. So the budget is one
# line, and a deep dive goes after `@moreinfo` where a post-process moves it below the
# member lists. The numbers come from the tree: a member line is 14 words at the median
# and 19 at p95, so 20 words bites the outliers and leaves the normal case alone.
HEADER_DIRS = ("src/light/drivers",)
MAX_CLASS_DOC = 10      # lines of `///` directly above `class X`
MAX_MOREINFO = 20       # lines of the `@moreinfo` appendix
MAX_MEMBER_DOC = 1      # lines of a `///` run that is NOT a class comment
MAX_DOC_WORDS = 20      # words on one `///` line

BASELINE = ROOT / "moondeck" / "check" / "docgen_baseline.txt"


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
    written to break them (test/python/test_check_docgen.py) rather than only against the
    docs tree, where a rule that silently stopped matching would look like a clean run."""
    import mkdocs_hooks as h
    lines = text.split("\n")
    # WHERE a block begins and ends is the build's rule, shared with check_specs. What the
    # lines inside it MEAN is this check's own business, and stays here.
    for title, start, end in h.split_blocks(text):
        cur = {"title": title, "desc": 0, "controls": 0, "widest": 0,
               "widest_text": "", "img": False, "img_src": ""}
        for ln in lines[start + 1:end]:
            if h._IMG_RE.match(ln):
                cur["img"] = True
                src = re.search(r'src="([^"]+)"', ln)
                cur["img_src"] = src.group(1) if src else ""
            elif (h._ORIGIN_RE.match(ln) or h._TESTS_RE.match(ln)
                  or h._TESTS_MULTI_RE.match(ln) or h._DETAIL_RE.match(ln)):
                continue                      # renders in another cell, not the description
            elif h._PARAM_RE.match(ln):
                control = re.sub(r"^-\s+", "", ln.strip())
                cur["controls"] += len(control)
                if len(control) > cur["widest"]:
                    cur["widest"], cur["widest_text"] = len(control), control
            elif ln.strip():
                cur["desc"] += len(ln.strip())
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


def _measure(reason: str):
    """The rule word and its number, or None where a rule carries no number.

    A baseline entry is tolerated at the number it recorded, so both halves are needed:
    the word says which rule, the number says how far over it was allowed to be.
    """
    word = reason.split(":")[0].split(" ")[0]
    m = re.match(r"\D*(\d+)", reason)
    return word, (int(m.group(1)) if m else None)


def _headers():
    """Every header the rules cover, repo-relative."""
    for d in HEADER_DIRS:
        yield from sorted(p.relative_to(ROOT) for p in (ROOT / d).glob("*.h"))


def _doc_runs(lines):
    """Each run of consecutive `///` lines, as (start, end, next_code_line)."""
    i = 0
    while i < len(lines):
        if lines[i].lstrip().startswith("///"):
            j = i
            while j < len(lines) and lines[j].lstrip().startswith("///"):
                j += 1
            nxt = lines[j].lstrip() if j < len(lines) else ""
            yield i, j, nxt
            i = j
        else:
            i += 1


def _declared_name(decl: str, start: int) -> str:
    """The NAME a declaration introduces, for a stable baseline key.

    Splitting on "(" and taking the last token yields `true;` from `bool ok = true;` and
    `Slot>` from a template line, so several members collapse onto one key: the baseline
    then records one member's size and reads every other as having grown. The identifier
    before `(`, `=` or `;` is the name, and a line number is the fallback where no
    declaration follows (a file-level comment block).
    """
    # The name is the identifier that PRECEDES the initializer, so `bool ok = true;` is
    # `ok` rather than `true`. Searching for the first identifier before any of `( = ;`
    # matched the initializer's value and collapsed nine members onto `::true`.
    m = re.match(r"^(?:class|struct)\s+(\w+)", decl)
    if m:
        return m.group(1)
    # The identifier immediately BEFORE the initializer, parameter list or terminator.
    # A greedy prefix swallows it and captures the initializer instead, so anchor on the
    # last identifier that precedes one of `( = ; {`.
    m = re.match(r"^[\w:<>,\s\*&\[\]]*?\b(\w+)\s*(?:\(|=|;|\{)", decl)
    if m:
        return m.group(1)
    return f"line {start + 1}"


_CLASS_RE = re.compile(r"^(class|struct)\s+\w+")
_FUNC_RE = re.compile(r"^[\w:<>,\s\*&]+\s+(\w+)\s*\([^)]*\)\s*(const)?\s*(override)?\s*[;{]")
_VAR_RE = re.compile(r"^[\w:<>,\s\*&]+\s+(\w+)\s*(=[^;]+)?;")


def _header_rules(rel: str, text: str):
    """The `///` budget, on one header.

    Five rules, all counted rather than judged: a class comment of at most ten lines, an
    `@moreinfo` appendix of at most twenty, ONE line for any other `///`, twenty words on
    a line, and a `///` on every public member. The first four cut; the last adds, and
    they are meant to pull against each other: the result is a short line on everything
    rather than an essay on a few things.
    """
    out = []
    lines = text.split("\n")

    for start, end, nxt in _doc_runs(lines):
        n = end - start
        if _CLASS_RE.match(nxt):
            if n > MAX_CLASS_DOC:
                out.append((f"{rel}::{nxt.split()[1].rstrip('{:')}",
                            f"class comment {n} lines > {MAX_CLASS_DOC}"))
        elif n > MAX_MEMBER_DOC:
            out.append((f"{rel}::{_declared_name(nxt, start)}",
                        f"member comment {n} lines > {MAX_MEMBER_DOC}: "
                        f"a deep dive goes after @moreinfo"))
        for k in range(start, end):
            words = len(re.sub(r"^\s*///\s*", "", lines[k]).split())
            if words > MAX_DOC_WORDS:
                out.append((f"{rel}::line {k + 1}",
                            f"doc line {words} words > {MAX_DOC_WORDS}"))

    for i, ln in enumerate(lines):
        if "@moreinfo" in ln:
            n = 0
            for m in lines[i + 1:]:
                if not m.lstrip().startswith("///"):
                    break
                n += 1
            if n > MAX_MOREINFO:
                out.append((f"{rel}::@moreinfo", f"appendix {n} lines > {MAX_MOREINFO}"))

    # Every public member carries one. A generated page shows a member with no `///` as a
    # bare signature, which tells a reader nothing the declaration did not.
    # Only declarations in the CLASS BODY itself. A `{` opens a function body, and
    # `return true;` or `Preset& p = presets_[count_];` inside one parses as a
    # declaration, so 251 statements were reported as undocumented public members.
    #
    # The depth of a class body is NOT a constant: every header opens `namespace mm`, but
    # a nested struct adds another level (Hub75Slots puts its class at 3 where the others
    # are at 2). So remember WHICH depth the class opened at, rather than counting to a
    # number that is right for most files and wrong for the rest.
    public = False
    depth = 0
    class_depth = None
    for i, ln in enumerate(lines):
        st = ln.strip()
        opens, closes = ln.count("{"), ln.count("}")
        before = depth
        depth += opens - closes
        if _CLASS_RE.match(st) and opens:
            class_depth = before + 1
            public = st.startswith("struct")     # a struct is public by default
            continue
        if class_depth is not None and depth < class_depth:
            class_depth = None                   # the class body closed
            public = False
        if st.startswith("public:"):
            public = True
            continue
        if st.startswith(("private:", "protected:")):
            public = False
            continue
        if not public or not st or st.startswith(("//", "/*", "*", "#")):
            continue
        # `before` is the depth the line STARTS at: a member sits exactly in the body.
        if before != class_depth or st.startswith(("return", "if", "for", "while", "}")):
            continue
        prev = lines[i - 1].lstrip() if i else ""
        if prev.startswith("///") or "///" in ln:
            continue
        if _FUNC_RE.match(st):
            out.append((f"{rel}::{_declared_name(st, i)}", "public function has no ///"))
        elif "(" not in st and _VAR_RE.match(st):
            out.append((f"{rel}::{_declared_name(st, i)}", "public variable has no ///"))
    return out


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
            else:
                want = ".gif" if rel in ANIMATED_PAGES else ".png"
                if not c["img_src"].lower().endswith(want):
                    why = ("an effect, modifier or layout shows motion" if want == ".gif"
                           else "a card or control is sharper and smaller as a png")
                    out.append((key, f"image is {c['img_src'].rsplit('.', 1)[-1]}, "
                                     f"not a {want.lstrip('.')}: {why}"))

    for rel in _headers():
        out.extend(_header_rules(str(rel), (ROOT / rel).read_text()))
    return out


REPORT = ROOT / "docs" / "reference" / "metrics" / "docgen.md"


def _write_report(found) -> None:
    """The current state as a tracked page, so its git history is the trend.

    The baseline records WHICH cards are tolerated. This records HOW MUCH is left, per page
    and per rule, so `git log -p docs/reference/metrics/docgen.md` answers whether the
    documentation is getting better. Same shape as repo-health.md, for the same reason.
    """
    from collections import Counter, defaultdict
    by_page = defaultdict(list)
    for key, why in found:
        page, _, title = key.partition("::")
        by_page[page].append((title, why))
    # The RULE, not its first word: "no image" and "one control" both truncate to a word
    # that names nothing. A reader of the report needs to know which rule, so map to it.
    def _rule(why: str) -> str:
        head = why.split(":")[0]
        for name in ("no image", "image is", "one control", "description", "controls",
                     "details table", "details section", "details heading", "second column",
                     "class comment", "member comment", "doc line", "appendix",
                     "public function has no", "public variable has no"):
            if head.startswith(name):
                return name
        return head.split(" ")[0]

    rules = Counter(_rule(why) for _, why in found)

    out = ["# Docgen", "",
           "Generated by [`moondeck/check/check_docgen.py`](../../../moondeck/check/check_docgen.py) "
           "with `--report`. **Do not edit by hand.**", "",
           "Every place the generated documentation breaks the shape [the standards]"
           "(../../contributing/documentation-standards.md#the-card) define. Current state only: "
           "the trend is this file's git history. The list only shrinks.", "",
           f"**{len(found)} finding(s)** across {len(by_page)} page(s).", "",
           "## By rule", "", "| Rule | Count |", "|---|---:|"]
    for rule, n in rules.most_common():
        out.append(f"| {rule} | {n} |")
    cards = {k: v for k, v in by_page.items() if k.endswith(".md")}
    headers = {k: v for k, v in by_page.items() if not k.endswith(".md")}
    for title, group in (("Catalog pages", cards), ("Headers", headers)):
        if not group:
            continue
        out += ["", f"## {title}", "", "| File | Findings |", "|---|---:|"]
        for page in sorted(group, key=lambda k: (-len(group[k]), k)):
            out.append(f"| `{page}` | {len(group[page])} |")
    out += ["", "## Every finding", ""]
    for page in sorted(by_page):
        out += [f"### {page}", ""]
        for title, why in sorted(by_page[page]):
            out.append(f"- **{title}**: {why}")
        out.append("")
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text("\n".join(out) + "\n")
    print(f"Docgen report: {len(found)} finding(s) written to {REPORT.relative_to(ROOT)}")


def _report(entries, heading: str) -> None:
    """Every entry, grouped by page, with a per-page count.

    A check that prints only a total tells a reader nothing they can act on, and this one
    runs as a MoonDeck card whose whole output is its log. So the detail is the report.
    """
    from collections import defaultdict
    by_page = defaultdict(list)
    for key, why in entries:
        page, _, title = key.partition("::")
        by_page[page].append((title, why))
    print(heading)
    for page in sorted(by_page):
        rows = by_page[page]
        print(f"\n  {page}  ({len(rows)})")
        for title, why in sorted(rows):
            print(f"    {title}: {why}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--baseline", action="store_true",
                    help="rewrite the grandfather list from what the tree holds now")
    ap.add_argument("--report", action="store_true",
                    help="write docs/reference/metrics/docgen.md, the tracked state of the sweep")
    args = ap.parse_args()

    found = _violations()

    if args.report:
        _write_report(found)
        return 0

    if args.baseline:
        BASELINE.write_text(
            "# Cards over the size limits when the check was introduced. Each line is one\n"
            "# card+rule that check_docgen.py tolerates. The list only SHRINKS: a card\n"
            "# edited back under the limit loses its line, and nothing is ever added.\n"
            "# Empty this file and the check is enforced everywhere. Regenerate: --baseline\n"
            + "".join(f"{k}\t{v.split(':')[0]}\n" for k, v in sorted(found)))
        print(f"Docgen baseline: {len(found)} tolerated violation(s) written to "
              f"{BASELINE.relative_to(ROOT)}")
        return 0

    # A baseline entry tolerates a card at the size it WAS, not at any size. Matching the
    # rule word alone let a 678-character control block grow to 5,000 and stay quiet,
    # which is the opposite of what a baseline is for. The stored number is the ceiling:
    # an edit that shortens a card needs no refresh, one that lengthens it fails.
    tolerated = {}
    if BASELINE.exists():
        for ln in BASELINE.read_text().split("\n"):
            if ln.strip() and not ln.startswith("#"):
                key, _, reason = ln.partition("\t")
                word, n = _measure(reason)
                # The LARGEST value wins where a key repeats: keeping the last one
                # lets a colliding sibling read as growth on the next run, which is a
                # false failure the tree cannot clear.
                prev = tolerated.get((key, word))
                if prev is None or (n is not None and n > prev):
                    tolerated[(key, word)] = n

    fresh = []
    for k, v in found:
        word, now = _measure(v)
        if (k, word) not in tolerated:
            fresh.append((k, v))
            continue
        was = tolerated[(k, word)]
        if now is not None and was is not None and now > was:
            fresh.append((k, f"{v} (was {was} in the baseline: it grew)"))

    if not fresh:
        print(f"Docgen check: {len(found)} finding(s), all in the baseline. "
              f"Limits: description {MAX_DESC}, controls {MAX_CONTROLS}, "
              f"one control {MAX_CONTROL}.\n")
        _report(found, "Tolerated, from the baseline. The list only shrinks:")
        print("\nNothing new. An entry already here may not grow: the baseline holds each at "
              "the size it recorded.")
        return 0

    print(f"Docgen check: {len(fresh)} finding(s).\n")
    _report(fresh, "New, not in the baseline:")
    print("\nMove the overflow, do not trim it: module behavior into the header's ///"
          "\n(the technical page the card links), cross-module rationale into a"
          "\n`## <Name>, details` section on the same page."
          "\nRules: docs/contributing/documentation-standards.md § Card size.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
