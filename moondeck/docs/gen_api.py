"""Generate per-module technical Markdown from source, at MkDocs build time.

The source `.h` is the single home of technical content; these pages are generated
*views* of the `///` comments in it — nothing is hand-restated. Pipeline: Doxygen
(the de-facto-standard parser — robust on the C++20 that a Tree-sitter tool choked
on) emits XML; moxygen renders it to Markdown with our custom template
(moondeck/docs/moxygen-templates/). Called by mkdocs_hooks.py's on_files, the output
injected into the virtual tree under moonmodules/{core,light}/moxygen/<Module>.md —
the domain-nested layout the § Documentation model standard defines.

**Every** `.h` under src/{core,light} gets a page — core and light, module and
utility alike. Discovery is exhaustive and automatic (no hand-maintained list): a
richly-`///`-commented header yields a full page, a sparsely-commented one a thin
page. Curation is a *separate* layer: only MoonModule subclasses (the things with
controls) appear in the end-user summary tables; a non-module header (a wire-format
struct, a math utility) has no table row but is still reachable as a generated page
and cross-linked from the pages that use it. Catalog effects/modifiers/layouts/
drivers get a page from their `///` too; their *controls* surface stays in the
summary-page cards, since those come from runtime `controls_.add(...)` calls no
static tool sees.

Doxygen (a brew/apt binary) and moxygen (via npx) are NOT uv-installable — the one
justified non-uv dependency (like the ESP-IDF Python exception). If either is absent
the generator returns nothing and the site builds without these pages (they appear
in CI, where both are provisioned); a contributor without doxygen still gets the rest.
"""

import os
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TEMPLATES = Path(__file__).resolve().parent / "moxygen-templates" / "cpp"

# A floor on how many pages a healthy run produces. Below this, something broke
# (doxygen parsed nothing, moxygen emitted nothing, the class→header map is empty) —
# raise rather than write a near-empty API set. Set well under the real count (~114
# today) so it only trips on genuine breakage, not on adding/removing a few headers.
_MIN_EXPECTED_PAGES = 50


class GenApiError(RuntimeError):
    """The Doxygen/moxygen toolchain was present but failed or produced too few pages.
    Distinct from the toolchain being *absent* (which is a graceful {} skip): this is a
    real failure the caller should surface, so CI doesn't ship a degraded docs site."""

# The two domains whose headers are offered to Doxygen. The output URI nests under
# the matching domain dir (moonmodules/core/moxygen/, moonmodules/light/moxygen/),
# mirroring src/. Discovery walks these; no per-header list to maintain.
DOMAINS = ("core", "light")


def domain_of(header_rel: str) -> str | None:
    """The doc domain ('core'/'light') for a repo-relative header path, or None if
    the header isn't under src/core or src/light (so it gets no generated page)."""
    parts = Path(header_rel).parts
    if len(parts) >= 2 and parts[0] == "src" and parts[1] in DOMAINS:
        return parts[1]
    return None


def _discover_headers() -> list[str]:
    """Every `.h` under src/core and src/light, repo-relative, sorted. Every header
    gets a generated technical page — exhaustive, no gating. Curation (which modules
    appear in the end-user summary tables) is the summary pages' job, not the
    generator's: only MoonModule subclasses are tabled, but every header is reachable
    as a generated page. A sparsely-commented header just yields a thin page."""
    found: list[str] = []
    for d in DOMAINS:
        for h in sorted((ROOT / "src" / d).rglob("*.h")):
            found.append(str(h.relative_to(ROOT)))
    return found


def available() -> bool:
    """Both tools present? (doxygen binary + npx for moxygen)."""
    return shutil.which("doxygen") is not None and shutil.which("npx") is not None


def _doxyfile(headers: list[str], xml_out: str) -> str:
    # Quote each path so a ROOT (or any parent) containing spaces doesn't get split
    # into separate INPUT entries — Doxygen treats a quoted path as one argument.
    inputs = " ".join(f'"{ROOT / h}"' for h in headers)
    return (
        f'PROJECT_NAME="projectMM API"\n'
        f"INPUT = {inputs}\n"
        # XML only — moxygen's input. Doxygen defaults GENERATE_HTML *and*
        # GENERATE_LATEX to YES; leaving LaTeX on drops a stray latex/ dir of
        # .tex/.sty files in the cwd every build. We want neither, just the XML.
        f"GENERATE_HTML = NO\nGENERATE_LATEX = NO\nGENERATE_XML = YES\nXML_OUTPUT = {xml_out}\n"
        # documented-only, no privates/statics, hide undoc → the compact public surface.
        "EXTRACT_ALL = NO\nEXTRACT_PRIVATE = NO\nEXTRACT_STATIC = NO\n"
        "HIDE_UNDOC_MEMBERS = YES\nHIDE_UNDOC_CLASSES = YES\n"
        "JAVADOC_AUTOBRIEF = YES\n"          # a leading `///`/`//` line is the brief
        f"STRIP_FROM_PATH = {ROOT}\n"        # relative "Defined in src/…", never an abs path
        "QUIET = YES\nWARN_IF_UNDOCUMENTED = NO\n"
    )


# Where the generated pages are written under docs/ (gitignored). Writing them to
# disk — rather than injecting them as in-memory virtual pages — puts them through
# the standard MkDocs flow (MkDocs discovers real files) and lets a human open/preview
# the .md directly, the same as any other doc source.
DOCS_MOONMODULES = ROOT / "docs" / "moonmodules"

_BLOB_BASE = "https://github.com/MoonModules/projectMM/blob/main"


def _migration_crosscheck_header(header_rel: str, domain: str, stem: str) -> str:
    """A TEMPORARY banner prepended to each generated page during the docs-v2
    migration: a link to the source `.h` (GitHub blob — `src/` isn't published to the
    site) and, if one still exists, the original hand-written `<stem>.md` as an
    IN-SITE relative link (that page still builds during the migration, so the link
    resolves to its rendered `.html`), so a reviewer can cross-check that the `.md`'s
    content was absorbed into the `.h`'s `///` comments. Removed at Stage 5."""
    parts = [f"[source `{Path(header_rel).name}`]({_BLOB_BASE}/{header_rel})"]
    # The old per-module .md now lives under docs/moonmodules/<domain>/archive/. Find
    # it by name (excluding the generated moxygen/ dirs); SORT so rglob's unspecified
    # order can't make the chosen match (and thus the emitted relative path) vary build
    # to build. Link RELATIVE to this generated page so MkDocs resolves it in-site.
    this_dir = DOCS_MOONMODULES / domain / "moxygen"
    for md in sorted(DOCS_MOONMODULES.rglob(f"{stem}.md")):
        if "moxygen" in md.parts:
            continue
        rel = os.path.relpath(md, this_dir).replace(os.sep, "/")
        parts.append(f"[original `{md.name}`]({rel})")
        break
    return f"> _Migration cross-check (temporary):_ {' · '.join(parts)}\n\n"


# A moxygen inter-class link: `](cls_mm-<Class>.md#<anchor>)`, plus the namespace file
# `](cls_mm.md#<anchor>)` (namespace-level free functions). moxygen names these by its
# OWN per-file output names, which don't exist after we recombine into per-header pages
# — so every such link must be repointed (or dropped for the namespace file, which has
# no single per-header home).
_CLS_LINK_RE = re.compile(r'\]\(cls_(?P<key>mm(?:-[\w-]+)?)\.md(?P<frag>#[\w-]+)?\)')


def _rewrite_cls_links(md: str, from_domain: str, cls_to_page: dict) -> str:
    """Repoint moxygen's `cls_mm-<Class>.md#anchor` cross-links at the per-header page
    the class actually lands on. Same domain → a sibling `<stem>.md`; cross-domain →
    `../../<domain>/moxygen/<stem>.md`. A class with no generated page (in a class-less
    util header) → drop the link, leaving its label as plain text so nothing dangles.

    The `#anchor` fragment is DROPPED: moxygen numbered anchors per its own per-class
    file (`#onbuildstate-13`), so after recombining several classes into one page those
    numbers no longer match the rendered heading ids — keeping them would emit thousands
    of dead-anchor warnings. Linking to the page (no fragment) lands the reader on the
    right module; the intra-page jump is a fair trade for a clean build."""
    def _sub(m: re.Match) -> str:
        page = cls_to_page.get(m.group("key"))
        if page is None:
            return "]"          # unknown class → strip target, keep the `[label]` text
        domain, stem = page
        rel = f"{stem}.md" if domain == from_domain else f"../../{domain}/moxygen/{stem}.md"
        return f"]({rel})"
    return _CLS_LINK_RE.sub(_sub, md)


# moxygen in-page `](#anchor)` self-links that don't survive recombination:
#   - `#_..._8h_source`  — Doxygen's per-header source-file anchor (never rendered here)
#   - `#name-<n>`         — moxygen's numbered member anchor (`#onbuildstate-13`); the
#                          number is a per-original-file dedup counter that no longer
#                          matches the heading id once classes are combined onto one page.
# Both point at nothing on the recombined page, so drop the link, keeping the `[label]`
# text. A *bare* self-link (`#modifylive`, `#drivers`) with no numeric suffix and no
# `_8h_source` shape is left alone — those match a real rendered heading id.
_BAD_ANCHOR_RE = re.compile(r'\]\(#(?:_\w+_8h_source|[\w-]+-\d+)\)')


def _strip_bad_anchor_links(md: str) -> str:
    """Drop moxygen self-links whose anchor doesn't exist on the recombined page
    (source-file anchors + numbered member anchors), leaving the link label as text."""
    return _BAD_ANCHOR_RE.sub("]", md)


# A `@card <file>` directive in a class `///` comment — the module's UI-card screenshot.
# Doxygen with GENERATE_HTML=NO drops `\image`/`@htmlonly`/raw `<img>` from the XML, but
# preserves plain text, so `@card foo.png` survives Doxygen → moxygen as-is and we render
# it to an `<img>` here (post-process). The asset lives at docs/assets/<domain>[/<sub>]/<file>;
# from a moxygen page (moonmodules/<domain>/moxygen/) that resolves to ../../../assets/… .
# `@card <file>` can land mid-line: Doxygen flows consecutive `///` lines into one paragraph,
# so in a richly-commented class the directive trails the last text run (`… esp_event.h. @card x.png`)
# rather than sitting on its own line. Match it ANYWHERE, with optional surrounding whitespace, and
# render to an <img> on its own block (a leading newline lifts it out of the trailing paragraph).
_CARD_RE = re.compile(r'[ \t]*@card\s+(?P<file>\S+\.(?:png|jpe?g|gif))[ \t]*')
_ASSETS = ROOT / "docs" / "assets"


def _render_card_directives(md: str, domain: str, stem: str) -> str:
    """Replace each `@card <file>` directive with an <img> pointing at the resolved asset.
    A file that doesn't exist on disk drops the directive (no broken image) — the same
    fail-soft as a missing generated page."""
    def repl(m: re.Match) -> str:
        fname = m.group("file")
        # Search under docs/assets/<domain>/ for the file (handles the light/{drivers,effects,…} subdir).
        hits = list((_ASSETS / domain).rglob(fname))
        if not hits:
            return ""   # asset absent → emit nothing rather than a broken link
        rel = os.path.relpath(hits[0], DOCS_MOONMODULES / domain / "moxygen")
        return f'\n\n<img src="{rel}" alt="{stem} card" width="300">\n'
    return _CARD_RE.sub(repl, md)


# A member signature line moxygen emits as its own paragraph: a backtick-delimited
# code span alone on a line — an attribute (`uint8_t protocol = 0`) or a method
# (`virtual inline void onBuildControls() override`). The template wraps each in
# backticks; nothing else on the site opens a line with a bare code span, so this
# anchors the match to member signatures only.
_SIG_LINE_RE = re.compile(r'^`(?P<sig>[^`\n]+)`[ \t]*$', re.MULTILINE)
# A METHOD name is the identifier immediately before the FIRST argument-list `(`
# (`… onBuildControls(…) override` → `onBuildControls`) — trailing `const`/`override`
# come after and must NOT win. An ATTRIBUTE name is the identifier before ` =` or at
# the end of the declarator (`uint8_t protocol = 0` → `protocol`; `char pins[24] = ""`
# → `pins`, skipping the `[N]` array bound). Two anchored patterns, method tried first.
_SIG_METHOD_NAME_RE = re.compile(r'(?P<name>[A-Za-z_]\w*)\s*\(')
_SIG_ATTR_NAME_RE = re.compile(r'(?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=|$)')


def _highlight_signature_names(md: str) -> str:
    """Wrap the declared member NAME in each generated signature so the theme can
    highlight it while the type/args stay muted (CSS: `.mm-sig-name`). moxygen emits
    a flat `<code>` string with no internal markup, so 'colour only the name' can't
    be done in CSS alone — we split the code span here into
    `<code>…<span class="mm-sig-name">name</span>…</code>` (raw HTML the markdown
    passes through). The signature reads as one code chip; only the identifier pops."""
    def repl(m: re.Match) -> str:
        sig = m.group("sig")
        # Method (has an arg list) → the id before the FIRST `(`; else attribute →
        # the id before `=` / end. Falls through to the whole span if neither matches.
        h = _SIG_METHOD_NAME_RE.search(sig) if "(" in sig else _SIG_ATTR_NAME_RE.search(sig)
        if not h:
            return m.group(0)
        start, end = h.start("name"), h.end("name")
        wrapped = (_html_escape(sig[:start])
                   + f'<span class="mm-sig-name">{_html_escape(sig[start:end])}</span>'
                   + _html_escape(sig[end:]))
        return f'<code class="mm-sig">{wrapped}</code>'
    return _SIG_LINE_RE.sub(repl, md)


def _html_escape(s: str) -> str:
    # Signatures carry `<`, `>`, `&` (templates, refs) — escape so the raw-HTML
    # <code> we emit renders them as text, not markup.
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _class_to_header(xml_dir: Path) -> dict[str, str]:
    """Map each moxygen class-file key → its source header (repo-relative), read from
    the Doxygen XML `<location file=...>` of every class/struct compound. The key is
    moxygen's `--classes` filename stem: the fully-qualified name with `::` → `-`
    (e.g. `mm::ControlList` → `mm-ControlList`), matching moxygen's `%s` substitution."""
    mapping: dict[str, str] = {}
    for cx in list(xml_dir.glob("class*.xml")) + list(xml_dir.glob("struct*.xml")):
        try:
            root = ET.parse(cx).getroot()
        except ET.ParseError:
            continue
        cd = root.find("compounddef")
        if cd is None:
            continue
        name = cd.findtext("compoundname") or ""     # e.g. "mm::ControlList"
        loc = cd.find("location")
        if not name or loc is None:
            continue
        header = loc.get("file")                      # e.g. "src/core/Control.h"
        if header:
            mapping[name.replace("::", "-")] = header
    return mapping


def generate() -> dict[str, str]:
    """Write a generated technical page for every documented module under
    src/{core,light} into docs/moonmodules/{domain}/moxygen/<Module>.md (gitignored),
    and return {doc_uri: markdown} for the pages written (empty if the toolchain is
    unavailable). doc_uri nests by domain, e.g. 'moonmodules/core/moxygen/Control.md'.

    ONE Doxygen pass over all headers + ONE moxygen `--classes` call — not per-header.
    Per-header (132×) meant 132 npx cold-starts (~0.95s each ≈ 150s); the single pass
    is ~5s. moxygen `--classes` emits one file per class, so a header's several classes
    (Control.h → Control, ControlList, ControlDescriptor) are recombined here into one
    per-header page via the class→header map from the XML `<location>`.

    Failure model: `available()` false → return {} (a contributor without the tools
    still builds the rest of the site — a *graceful* skip). But if the tools ARE present
    and then fail (npx registry fetch error, doxygen crash, empty output), raise
    GenApiError — silently returning {} there would ship a docs site with ZERO API pages
    and no red X. The caller (mkdocs_hooks) degrades gracefully on absent tools but lets
    the error propagate so CI, where the tools are provisioned, fails loudly."""
    if not available():
        return {}

    headers = [h for h in _discover_headers() if domain_of(h)]
    if not headers:
        return {}

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        xml_dir = tdp / "xml"
        (tdp / "Doxyfile").write_text(_doxyfile(headers, str(xml_dir)))
        r = subprocess.run(["doxygen", str(tdp / "Doxyfile")],
                           cwd=tdp, capture_output=True, text=True)
        if r.returncode != 0 or not xml_dir.exists():
            raise GenApiError(f"doxygen failed (rc={r.returncode}): {r.stderr[-500:]}")

        # One moxygen call, class-per-file (output name = fully-qualified class, ::→-).
        m = subprocess.run(
            ["npx", "--yes", "moxygen@2.1.10",
             "--templates", str(TEMPLATES), "--classes", "--noindex",
             "--output", str(tdp / "cls_%s.md"), str(xml_dir)],
            cwd=tdp, capture_output=True, text=True,
        )
        if m.returncode != 0:
            # npx couldn't fetch/run moxygen (registry outage, yanked version, no net).
            raise GenApiError(f"npx moxygen failed (rc={m.returncode}): {m.stderr[-500:]}")

        cls_to_header = _class_to_header(xml_dir)

        # moxygen's `--classes` cross-references link to its OWN per-class filenames
        # (`cls_mm-<Class>.md#anchor`). We recombine classes into per-header pages, so
        # those targets don't exist — rewrite each to the header page the class lands
        # on. A class in a non-generated header (e.g. a struct in a class-less util)
        # maps to nothing → strip the link to plain text so it can't dangle.
        # cls-key ("mm-Layer") → (domain, header-stem) of the page it ends up in.
        cls_to_page = {
            key: (domain_of(h), Path(h).stem)
            for key, h in cls_to_header.items() if domain_of(h)
        }

        # Group the per-class markdown by owning header (in header order, so a page's
        # classes appear top-down as declared).
        by_header: dict[str, list[str]] = {}
        for cls_md in sorted(tdp.glob("cls_*.md")):
            key = cls_md.name[len("cls_"):-len(".md")]   # "mm-ControlList"
            header = cls_to_header.get(key)
            if header is None or domain_of(header) is None:
                continue
            by_header.setdefault(header, []).append(cls_md.read_text(encoding="utf-8"))

        pages: dict[str, str] = {}
        for header, blocks in by_header.items():
            domain = domain_of(header)
            stem = Path(header).stem
            body = _rewrite_cls_links("".join(blocks), domain, cls_to_page)
            body = _strip_bad_anchor_links(body)
            body = _render_card_directives(body, domain, stem)
            body = _highlight_signature_names(body)
            md = _migration_crosscheck_header(header, domain, stem) + body
            uri = f"moonmodules/{domain}/moxygen/{stem}.md"
            dst = DOCS_MOONMODULES / domain / "moxygen" / f"{stem}.md"
            dst.parent.mkdir(parents=True, exist_ok=True)
            # Write ONLY when the content changed. These files live under docs_dir,
            # which `mkdocs serve` watches — an unconditional write bumps the mtime
            # every build, which the watcher reads as a change and rebuilds, which
            # regenerates, which writes again: an endless rebuild loop that pins the
            # serve at ~7s/request. Skipping an identical write leaves mtime untouched,
            # so the watcher stays quiet.
            if not dst.exists() or dst.read_text(encoding="utf-8") != md:
                dst.write_text(md, encoding="utf-8")
            pages[uri] = md

        # Tools ran but produced far too few pages → something broke upstream (empty
        # XML, an unmatched class→header map). Fail loudly rather than ship a gutted set.
        if len(pages) < _MIN_EXPECTED_PAGES:
            raise GenApiError(
                f"only {len(pages)} API pages generated (expected ≥ {_MIN_EXPECTED_PAGES}) "
                f"— doxygen/moxygen ran but produced almost nothing")
        return pages
