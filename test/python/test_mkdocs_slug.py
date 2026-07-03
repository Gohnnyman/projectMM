"""The catalog-table hook builds a "⌄ details" anchor link, `#{_slug(name + ' — details')}`,
that must land on the `## <Name> — details` heading MkDocs renders on the catalog pages.
Because `validation.links.anchors` is `warn` (not `fail`), a slug that diverges from the
real heading id produces a SILENT dead link, not a build failure — so this test pins
`_slug()` to the actual anchor Python-Markdown generates.

`_slug()` delegates to `markdown.extensions.toc.slugify` (the exact function MkDocs' toc
uses), so this both guards that delegation and documents the contract. If `_slug` is ever
re-hand-rolled, the unicode / consecutive-separator cases below catch the drift.
"""

import sys
from pathlib import Path

import pytest

# `markdown` ships with MkDocs but isn't in the base test env — skip cleanly rather
# than error collection where it's absent. CI installs it (--with markdown) so the
# test actually runs there; a contributor without it just skips this one file.
markdown = pytest.importorskip("markdown")
from markdown.extensions.toc import slugify  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "scripts" / "docs"))

from mkdocs_hooks import _slug  # noqa: E402


def test_slug_matches_markdown_toc_slugify():
    """_slug is exactly Python-Markdown's toc slugify — including the edge cases a
    hand-rolled mimic gets wrong (accent-stripping, decoration, repeated separators)."""
    for text in [
        "LED output — details",
        "Fire2012 — details",
        "GEQ · 3D — details",
        "a  b   c",
        "Émoji 💫 test",
        "Multiple---hyphens",
    ]:
        assert _slug(text) == slugify(text, "-"), text


def test_slug_resolves_to_the_real_rendered_heading_id():
    """A `## <heading>` rendered by the same toc extension MkDocs uses gets an
    `id=` equal to _slug(heading) — so the hand-built anchor actually resolves."""
    for heading in ["LED output — details", "GEQ · 3D — details"]:
        html = markdown.markdown(f"## {heading}", extensions=["toc"])
        assert f'id="{_slug(heading)}"' in html, heading
