"""check_catalog.py is the guardrail on the catalog pages, so this is the guardrail on it.

The rules it enforces (documentation-standards.md § Card size) are invisible when they
break: a regex that stops matching makes the check print a clean run, which is
indistinguishable from a tree with nothing wrong in it. Four `## HLS, details` headings
lived in the tree unlinked for exactly that reason, found by adding the rule rather than
by the rule working.

So each rule is pinned twice: it FIRES on a page written to break it, and it stays SILENT
on a page that obeys. A test that only asserted the silence would pass against a check
that had stopped reading anything at all.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

from check_catalog import (MAX_CONTROL, MAX_CONTROLS, MAX_DESC,  # noqa: E402
                           _cards, _structure)


def _card(desc: str = "Short.", controls=("- `a` — one.",)) -> str:
    return "### Thing 💫 · kind\n\n" + desc + "\n\n" + "\n".join(controls) + "\n"


# ---- sizes ----

def test_description_over_the_limit_is_measured():
    card = list(_cards(_card(desc="x" * (MAX_DESC + 50))))[0]
    assert card["desc"] > MAX_DESC


def test_a_card_within_the_limits_measures_under_them():
    card = list(_cards(_card()))[0]
    assert card["desc"] <= MAX_DESC
    assert card["controls"] <= MAX_CONTROLS
    assert card["widest"] <= MAX_CONTROL


def test_controls_are_summed_not_counted_individually():
    # Ten short controls are fine one by one and too much together: the column height is
    # what the reader pays, so the sum is the thing that matters.
    many = [f"- `p{i}` — {'y' * 80}." for i in range(10)]
    card = list(_cards(_card(controls=many)))[0]
    assert card["controls"] > MAX_CONTROLS
    assert card["widest"] < MAX_CONTROL


def test_the_widest_control_is_reported_with_its_text():
    card = list(_cards(_card(controls=("- `a` — ok.", "- `b` — " + "z" * 200)))) [0]
    assert card["widest"] > MAX_CONTROL
    assert "zzz" in card["widest_text"]


def test_links_and_image_lines_are_not_counted_as_description():
    # A card's Detail:/Tests:/Origin: lines and its <img> render in other cells, so
    # counting them as description would fail a card for text the column never holds.
    page = ('### Thing 💫 · kind\n\nShort.\n\n'
            '<img src="../../assets/x.png" alt="x">\n'
            '- `a` — one.\n'
            'Origin: Someone · source [x.h](../../src/x.h)\n'
            '[Tests](../../reference/tests/unit-tests.md#x)\n'
            'Detail: [technical](moxygen/X.md)\n')
    card = list(_cards(page))[0]
    assert card["desc"] == len("Short.")


def test_a_details_section_is_not_counted_into_the_card():
    # `## ` closes the block: the details prose belongs to the section, not the row.
    page = _card() + "\n## Thing — details\n\n" + "w" * 5000 + "\n"
    card = list(_cards(page))[0]
    assert card["desc"] <= MAX_DESC


# ---- structure ----

def test_details_above_a_card_is_flagged():
    page = _card() + "\n## Thing, details\n\nWhy.\n\n### Other 💫 · kind\n\nShort.\n"
    issues = _structure(page, "p.md")
    assert issues and "above the last card" in issues[0][1]


def test_details_below_every_card_is_accepted():
    page = _card() + "\n### Other 💫 · kind\n\nShort.\n\n## Thing, details\n\nWhy.\n"
    assert _structure(page, "p.md") == []


def test_a_details_heading_without_a_comma_is_flagged():
    # The build matches `, details` exactly to link a row to its section, so any other
    # punctuation renders a section nothing points at. The em-dash form is in this list
    # because it is banned repo-wide, not merely unmatched.
    for bad in ("## Thing — details", "## Thing: details", "## Thing details"):
        page = _card() + "\n" + bad + "\n\nWhy.\n"
        issues = _structure(page, "p.md")
        assert issues and "comma" in issues[0][1], bad


def test_details_naming_no_card_is_flagged():
    page = _card() + "\n## Ghost, details\n\nWhy.\n"
    issues = _structure(page, "p.md")
    assert issues and "names no card" in issues[0][1]


def test_two_details_sections_with_one_name_are_flagged():
    """Both slug to the same anchor, so the row's More: link reaches one and the other
    is unreachable. A sweep that adds a section where one already exists does this
    silently, which is how drivers.md ended up with two LED driver sections."""
    page = (_card() + "\n## Thing, details\n\nOne.\n\n## Thing, details\n\nTwo.\n")
    issues = _structure(page, "p.md")
    assert issues and "second details section" in issues[0][1]


def test_one_details_section_per_name_is_accepted():
    page = (_card() + "\n### Other 💫 · kind\n\nShort.\n\n"
            "## Thing, details\n\nOne.\n\n## Other, details\n\nTwo.\n")
    assert _structure(page, "p.md") == []


def test_a_section_heading_that_is_not_details_is_left_alone():
    # `## LED drivers` is a group header, not a details section: it must not be judged
    # by the details rules, or every catalog page would fail on its own structure.
    page = "## LED drivers\n\n" + _card() + "\n## Network drivers\n\n" + _card()
    assert _structure(page, "p.md") == []


# ---- the rendered row: two columns, labelled links ----

def _row(md: str) -> str:
    """One rendered table row, through the build's own renderer."""
    import mkdocs_hooks
    return mkdocs_hooks._render_catalog_table(md)


def test_a_row_renders_as_two_columns():
    """Two, not three or four. Every added column divides the page again, which is what
    turned a long description into a ribbon and made a three-link card taller than its
    own controls."""
    out = _row(_card())
    row = [l for l in out.split("\n") if l.startswith("| ") and "Module |" not in l
           and not l.startswith("|--")][0]
    assert row.count(" |") == 2, row


def test_the_header_names_the_two_columns():
    out = _row(_card())
    assert "| Module | Details |" in out


def test_links_are_labelled_not_only_iconed():
    """An icon alone made the reader decode a glyph, and the tests and API rows both
    render as a list of near-identical module names. The WORD carries the distinction."""
    page = ('### Thing 💫 · kind\n\nShort.\n\n- `a` — one.\n'
            '[Tests](../../reference/tests/unit-tests.md#thing)\n'
            'Detail: [technical](moxygen/Thing.md)\n')
    out = _row(page)
    assert "**Tests:**" in out and "**API:**" in out


def test_attribution_is_not_linked_in_the_second_column():
    """Attribution travels with the code it credits, in the header's `///` where the
    generated page shows it. A `Source:` row under the controls duplicated it and made the
    row length vary per card."""
    page = ('### Thing 💫 · kind\n\nShort.\n\n- `a` — one.\n'
            '[Tests](../../reference/tests/unit-tests.md#thing)\n'
            'Detail: [technical](moxygen/Thing.md)\n'
            'Origin: projectMM, by somebody\n')
    out = _row(page)
    assert "**Source:**" not in out
    assert "by somebody" not in out


def test_both_links_show_even_when_a_target_is_missing():
    """A card with no tests is a gap worth seeing. A row that silently drops the label
    hides it, and the two rows stop being in the same place on every card."""
    out = _row(_card())
    assert "**Tests:**" in out and "**API:**" in out
    assert out.count("none yet") == 2


def test_an_effect_card_needs_a_gif_not_a_png():
    """Effects, modifiers and layouts show MOTION: a still frame of a moving effect says
    almost nothing about it. Everything else is cards and controls, where a png is
    sharper and smaller."""
    import check_catalog
    png = ('### Thing 💫 · 2D\n\n<img src="../../assets/x.png" alt="x">\n\nShort.\n\n- `a` — one.\n')
    issues = check_catalog._structure(png, "moonmodules/light/effects.md")
    # _structure covers placement; the format rule lives with the per-card checks, so
    # exercise it the way _violations does.
    card = list(_cards(png))[0]
    assert card["img_src"].endswith(".png")
    assert "moonmodules/light/effects.md" in check_catalog.ANIMATED_PAGES
    assert "moonmodules/light/drivers.md" not in check_catalog.ANIMATED_PAGES


def test_the_image_source_is_captured_for_the_format_rule():
    gif = ('### Thing 💫 · 2D\n\n<img src="../../assets/x.gif" alt="x">\n\nShort.\n\n- `a` — one.\n')
    assert list(_cards(gif))[0]["img_src"].endswith(".gif")


def test_the_second_column_carries_only_tests_api_and_details():
    """Three doors, always the same three, in the same place. They are followed often
    enough to earn a standing position. A how-to, an explanation or a sibling module is a
    link the DESCRIPTION makes in a sentence, where the reader meets it in context instead
    of as a bare label under the controls."""
    import check_catalog
    assert check_catalog.SECOND_COLUMN_LINKS == ("Tests:", "API:", "Details:")
    page = ('### Thing 💫 · kind\n\n<img src="../../assets/x.png" alt="x">\n\nShort.\n\n'
            '- `a` — one.\n[Tests](../../reference/tests/unit-tests.md#thing)\n'
            'Detail: [technical](moxygen/Thing.md)\n')
    assert check_catalog._rendered_links("p.md", page) == []


def test_a_third_link_in_the_second_column_is_flagged():
    """The rule has to FIRE, or it is indistinguishable from a check that reads nothing.
    Renders a row, then asserts an extra label in cell 2 is caught."""
    import check_catalog, mkdocs_hooks
    real = mkdocs_hooks._emit_row

    def patched(b, names):
        row = real(b, names)
        cells = row.strip("| ").split(" | ")
        cells[1] += '<span class="mm-links">**More:** [x](#x)</span>'
        return "| " + " | ".join(cells) + " |"

    mkdocs_hooks._emit_row = patched
    try:
        page = ('### Thing 💫 · kind\n\nShort.\n\n- `a` — one.\n')
        issues = check_catalog._rendered_links("p.md", page)
    finally:
        mkdocs_hooks._emit_row = real
    assert issues and "More:" in issues[0][1]


# ---- details sections ----

def test_a_wide_details_table_is_flagged():
    """A details section continues the card, so it is for the same reader. Past four
    columns a table stops being scannable and starts being a spec sheet."""
    import check_catalog
    page = (_card() + "\n## Thing, details\n\n"
            "| a | b | c | d | e |\n|---|---|---|---|---|\n| 1 | 2 | 3 | 4 | 5 |\n")
    issues = check_catalog._details_tables("p.md", page)
    assert issues and "5 columns" in issues[0][1]


def test_a_details_table_cell_carrying_an_essay_is_flagged():
    """The widest cell is where implementation prose hides: one Notes cell reached 991
    characters of GDMA chains and ISR refills, which belongs on the API page."""
    import check_catalog
    page = (_card() + "\n## Thing, details\n\n"
            "| a | b |\n|---|---|\n| 1 | " + "z" * 400 + " |\n")
    issues = check_catalog._details_tables("p.md", page)
    assert issues and "prose in a grid" in issues[0][1]


def test_a_narrow_details_table_is_accepted():
    import check_catalog
    page = (_card() + "\n## Thing, details\n\n"
            "| a | b |\n|---|---|\n| 1 | short enough |\n")
    assert check_catalog._details_tables("p.md", page) == []


def test_a_table_outside_a_details_section_is_not_judged():
    """The rule is about details sections. A table in the page intro or in a card is a
    different thing with its own reasons."""
    import check_catalog
    page = "| a | b | c | d | e |\n|---|---|---|---|---|\n| 1 | 2 | 3 | 4 | 5 |\n" + _card()
    assert check_catalog._details_tables("p.md", page) == []


def test_a_card_without_an_image_is_flagged():
    """Every card leads with its picture: a row that starts with a name against blank
    space reads as a gap rather than as a module that happens to be invisible."""
    import check_catalog
    cards = list(_cards(_card()))
    assert cards[0]["img"] is False
    page_with = ('### Thing 💫 · kind\n\n<img src="../../assets/x.png" alt="x">\n\n'
                 'Short.\n\n- `a` — one.\n')
    assert list(_cards(page_with))[0]["img"] is True


def test_links_share_the_controls_cell():
    """Not their own column: stacked under the controls they cost no page width."""
    page = ('### Thing 💫 · kind\n\nShort.\n\n- `a` — one.\n'
            'Detail: [technical](moxygen/Thing.md)\n')
    row = [l for l in _row(page).split("\n") if l.startswith("| ") and "Module |" not in l
           and not l.startswith("|--")][0]
    cells = row.strip("| ").split(" | ")
    assert "mm-param" in cells[1] and "mm-links" in cells[1]


def test_the_preview_image_leads_the_first_cell():
    """Above the name, not beside it: an image in its own column forced every text
    column into a quarter of the page."""
    page = ('### Thing 💫 · kind\n\n<img src="../../assets/x.png" alt="x">\n\nShort.\n\n- `a` — one.\n')
    row = [l for l in _row(page).split("\n") if l.startswith("| ") and "Module |" not in l
           and not l.startswith("|--")][0]
    cells = row.strip("| ").split(" | ")
    assert cells[0].index("mm-preview") < cells[0].index("mm-name")


def test_the_real_pages_obey_the_structure_rules():
    """The tree itself, as the control: the synthetic cases above prove the rules fire,
    and this proves they are satisfied where it counts. Sizes are excluded (a baseline
    grandfathers those); structure has no baseline and must hold everywhere."""
    import check_catalog
    for rel in check_catalog._pages():
        path = ROOT / "docs" / rel
        if path.exists():
            assert _structure(path.read_text(), rel) == [], rel
