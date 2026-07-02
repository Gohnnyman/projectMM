"""Generate per-module technical Markdown from source, at MkDocs build time.

The source `.h` is the single home of technical content; these pages are generated
*views* of the `///` comments in it — nothing is hand-restated. Pipeline: Doxygen
(the de-facto-standard parser — robust on the C++20 that a Tree-sitter tool choked
on) emits XML; moxygen renders it to Markdown with our custom template
(scripts/docs/moxygen-templates/). Called by mkdocs_hooks.py's on_files, the output
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
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TEMPLATES = Path(__file__).resolve().parent / "moxygen-templates" / "cpp"

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
    inputs = " ".join(str(ROOT / h) for h in headers)
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
    # The old per-module .md lives somewhere under docs/moonmodules — find it by name
    # (excluding the generated moxygen/ dirs). Link RELATIVE to this generated page
    # (docs/moonmodules/<domain>/moxygen/<stem>.md) so MkDocs resolves it in-site.
    this_dir = DOCS_MOONMODULES / domain / "moxygen"
    for md in DOCS_MOONMODULES.rglob(f"{stem}.md"):
        if "moxygen" in md.parts:
            continue
        rel = os.path.relpath(md, this_dir).replace(os.sep, "/")
        parts.append(f"[original `{md.name}`]({rel})")
        break
    return f"> _Migration cross-check (temporary):_ {' · '.join(parts)}\n\n"


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
    per-header page via the class→header map from the XML `<location>`."""
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
            return {}

        # One moxygen call, class-per-file (output name = fully-qualified class, ::→-).
        m = subprocess.run(
            ["npx", "--yes", "moxygen@2.1.10",
             "--templates", str(TEMPLATES), "--classes", "--noindex",
             "--output", str(tdp / "cls_%s.md"), str(xml_dir)],
            cwd=tdp, capture_output=True, text=True,
        )
        if m.returncode != 0:
            return {}

        cls_to_header = _class_to_header(xml_dir)

        # Group the per-class markdown by owning header (in header order, so a page's
        # classes appear top-down as declared). moxygen's output is used verbatim.
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
            md = _migration_crosscheck_header(header, domain, stem) + "".join(blocks)
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
        return pages
