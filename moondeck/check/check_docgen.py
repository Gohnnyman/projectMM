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
page wide. The rule and its limits are in documentation-standards.md § The card; this
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
MAX_DOC_WORDS = 20      # words on one comment line, `///` or `//`
MAX_CODE_COMMENT = 1    # lines of a `//` run inside a class body




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
    card_names = {h.card_name(h._H3_RE.match(l).group("title"))
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
                            f"second column links to `{label}:`, which is not one of "
                            f"{', '.join(sorted(SECOND_COLUMN_LINKS))}: put it in the description"))
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


# Every rule this check emits, longest first so "details table cell" wins over
# "details table". The FULL name, not a first word: "no image" and "one control" each
# truncate to a word that names nothing, and the two details-table rules share theirs.
_RULE_NAMES = (
    "details table cell", "details table has", "details section", "details heading",
    "public function has no", "public variable has no", "second column",
    "class comment", "member comment", "no image", "image is", "one control",
    "description", "controls", "doc line", "appendix",
)


def _rule_name(reason: str) -> str:
    """Which rule a finding reports, as a stable key for the baseline and the report."""
    head = reason.split(":")[0]
    for name in _RULE_NAMES:
        if head.startswith(name):
            return name
    return head.split(" ")[0]


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


def _declared_key(decl: str, start: int) -> str:
    """The baseline key for a declaration: its name, plus its arity where it takes one.

    Two overloads share a name, so keying on the name alone put both under one entry and
    the baseline then tolerated whichever the check happened to measure first (the tree
    has `PreviewDriver::emit` twice today). The parameter COUNT separates them without
    depending on how a signature is spelled, which a normalized type list would.
    """
    name = _declared_name(decl, start)
    m = re.search(r"\(([^)]*)\)", decl)
    if not m or "(" not in decl:
        return name
    args = m.group(1).strip()
    n = 0 if not args or args == "void" else args.count(",") + 1
    return f"{name}/{n}"


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
# A constructor and a destructor have no return type, and a pure virtual ends `= 0;`, so the
# form above matches none of them: three public declarations the "needs a ///" rule never saw.
_SPECIAL_FUNC_RE = re.compile(
    r"^(?:explicit\s+|virtual\s+)*~?(\w+)\s*\([^)]*\)\s*"
    r"(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:=\s*(?:0|default|delete)\s*)?[;{:]")
_VAR_RE = re.compile(r"^[\w:<>,\s\*&]+\s+(\w+)\s*(=[^;]+)?;")


def _header_rules(rel: str, text: str):
    """The comment budget, on one header.

    Six rules, all counted rather than judged: a class comment of at most ten lines, an
    `@moreinfo` appendix of at most twenty, ONE line for any other `///`, twenty words on
    a comment line, ONE line for a `//` run, and a `///` on every public member. The first five cut;
    the last adds, and they are meant to pull against each other: the result is a short line
    on everything rather than an essay on a few things.

    `//` and `///` carry the same one-line budget, because without that the `///` cap moves
    text rather than removing it: a fifty-line member comment re-spelled as `//` satisfies
    every other rule and leaves the file exactly as long. Past one line the reasoning belongs
    after `@moreinfo` or on the module's page.
    """
    out = []
    lines = text.split("\n")

    for start, end, nxt in _doc_runs(lines):
        n = end - start
        # A run that opens a @defgroup documents the FILE, not a member: it precedes an include,
        # a constant or nothing at all, so the one-line member budget measured the whole block and
        # reported a 45-line finding on every such header. It is a lead comment, so it is held to
        # the class budget and splits at @moreinfo the same way.
        if any("@defgroup" in lines[k] for k in range(start, end)):
            head = next((k for k in range(start, end) if "@moreinfo" in lines[k]), end)
            n = head - start
            if n > MAX_CLASS_DOC:
                out.append((f"{rel}::line {start + 1}",
                            f"class comment {n} lines > {MAX_CLASS_DOC}"))
            continue
        if _CLASS_RE.match(nxt):
            # The LEAD only: the run ends at @moreinfo, whose own lines are the appendix and
            # are measured against MAX_MOREINFO below. Counting both here would put the
            # documented budget (10 lead + 20 appendix) out of reach of any header.
            head = next((k for k in range(start, end) if "@moreinfo" in lines[k]), end)
            n = head - start
            if n > MAX_CLASS_DOC:
                out.append((f"{rel}::{nxt.split()[1].rstrip('{:')}",
                            f"class comment {n} lines > {MAX_CLASS_DOC}"))
        elif n > MAX_MEMBER_DOC:
            out.append((f"{rel}::{_declared_key(nxt, start)}",
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

    # `//` carries the SAME one-line budget as `///`, or the `///` cap only MOVES text:
    # re-spelling a fifty-line member comment as `//` satisfies every other rule and leaves the
    # file the same length, which is what a first pass through these headers produced.
    # File-level `//` (above the first class, explaining the compilation unit) is exempt: it is
    # the non-Doxygen sibling of the class comment, and has no member to sit beside.
    first_class = next((i for i, ln in enumerate(lines) if _CLASS_RE.match(ln.strip())), len(lines))
    i = 0
    while i < len(lines):
        st = lines[i].strip()
        if st.startswith("//") and not st.startswith("///"):
            j = i
            while j < len(lines) and lines[j].strip().startswith("//") \
                    and not lines[j].strip().startswith("///"):
                j += 1
            if j - i > MAX_CODE_COMMENT and i > first_class:
                nxt = next((lines[k].strip() for k in range(j, len(lines)) if lines[k].strip()), "")
                out.append((f"{rel}::{_declared_key(nxt, i) if nxt else f'line {i + 1}'}",
                            f"code comment {j - i} lines > {MAX_CODE_COMMENT}"))
            # The same word budget as a `///` line, and for the same reason: one line is a
            # sentence, not a paragraph that happens to lack line breaks.
            for k in range(i, j):
                words = len(re.sub(r"^\s*//+\s*", "", lines[k]).split())
                if words > MAX_DOC_WORDS:
                    out.append((f"{rel}::line {k + 1}",
                                f"comment line {words} words > {MAX_DOC_WORDS}"))
            i = j
        else:
            i += 1

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
            # A NESTED struct inherits the enclosing access: one declared after `private:` is
            # private however it is spelled, and demanding a `///` on its fields asked a file to
            # document what no reader of the generated page can see. Only a top-level struct
            # (nothing open above it) starts public.
            nested = class_depth is not None or before > 1
            class_depth = before + 1
            public = st.startswith("struct") and not (nested and not public)
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
        if _FUNC_RE.match(st) or _SPECIAL_FUNC_RE.match(st):
            out.append((f"{rel}::{_declared_key(st, i)}", "public function has no ///"))
        elif "(" not in st and _VAR_RE.match(st):
            out.append((f"{rel}::{_declared_name(st, i)}", "public variable has no ///"))
    return out


def _card_rules(rel: str, c: dict):
    """Every rule that reads ONE card: its sizes and its image.

    A function rather than a loop body inside _violations, so a test can hand it a card
    and assert the rule fires. Read through the page's rendered form, the rules stayed
    untested against anything but the real tree, which is where a rule that had stopped
    matching would look like a clean run.
    """
    out = []
    key = f"{rel}::{c['title']}"
    if c["desc"] > MAX_DESC:
        out.append((key, f"description {c['desc']} > {MAX_DESC}"))
    if c["controls"] > MAX_CONTROLS:
        out.append((key, f"controls {c['controls']} > {MAX_CONTROLS}"))
    if c["widest"] > MAX_CONTROL:
        out.append((key, f"one control {c['widest']} > {MAX_CONTROL}: "
                         f"{c['widest_text'][:60]}..."))
    # A card leads with its picture. The image is the first thing the eye reaches on a
    # row, and a card without one starts with a name against blank space, which reads as
    # a gap rather than as a module that happens to be invisible.
    if not c["img"]:
        out.append((key, "no image: every card leads with one"))
    else:
        want = ".gif" if rel in ANIMATED_PAGES else ".png"
        if not c["img_src"].lower().endswith(want):
            why = ("an effect, modifier or layout shows motion" if want == ".gif"
                   else "a card or control is sharper and smaller as a png")
            out.append((key, f"image is {c['img_src'].rsplit('.', 1)[-1]}, "
                             f"not a {want.lstrip('.')}: {why}"))
    return out


def _orphan_pages():
    """Generated pages nothing links to: a technical page a reader cannot reach.

    Every `.h` under src/{core,light} gets a page, so a header nobody references from a
    catalog card, another page, or another header's `///` is documentation that exists and
    is unreachable. The link may come from anywhere: a card's Detail line, a prose page, or
    a sibling header naming the file (the hook retargets a `.h` mention at its page).
    """
    pages = {p.stem for p in (ROOT / "docs" / "moonmodules").rglob("moxygen/*.md")}
    linked = set()
    for md in (ROOT / "docs").rglob("*.md"):
        if "moxygen" in md.parts:
            continue
        for m in re.finditer(r"moxygen/(\w+)\.md", md.read_text(errors="ignore")):
            linked.add(m.group(1))
    for d in ("core", "light"):
        for h in (ROOT / "src" / d).rglob("*.h"):
            for m in re.finditer(r"\b(\w+)\.h\b", h.read_text(errors="ignore")):
                if m.group(1) != h.stem:
                    linked.add(m.group(1))
    return [(f"moonmodules::{name}", "generated page nothing links to: unreachable")
            for name in sorted(pages - linked)]


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
        for card in _cards(text):
            out.extend(_card_rules(rel, card))

    for rel in _headers():
        out.extend(_header_rules(str(rel), (ROOT / rel).read_text()))
    out.extend(_orphan_pages())
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
    rules = Counter(_rule_name(why) for _, why in found)

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
    ap.add_argument("--report", action="store_true",
                    help="write docs/reference/metrics/docgen.md, the tracked state of the sweep")
    args = ap.parse_args()

    found = _violations()

    if args.report:
        _write_report(found)
        return 0

    if not found:
        print(f"Docgen check: clean. Limits: description {MAX_DESC}, controls "
              f"{MAX_CONTROLS}, one control {MAX_CONTROL}, one comment line, "
              f"{MAX_DOC_WORDS} words.")
        return 0

    print(f"Docgen check: {len(found)} finding(s).\n")
    _report(found, "Every finding. There is no tolerated list: these are the limits.")
    print("\nMove the overflow, do not trim it: module behavior into the header's ///"
          "\n(the technical page the card links), cross-module rationale into a"
          "\n`## <Name>, details` section on the same page."
          "\nRules: docs/contributing/documentation-standards.md § The card.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
