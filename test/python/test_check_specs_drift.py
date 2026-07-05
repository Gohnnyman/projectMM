"""The spec-drift checks in check_specs.py must catch a control range or an author
URL that changed in the .h but not the .md — and NOT false-alarm on the common
cases (a control whose prose omits the range, a range spelled with a hyphen vs
en-dash). These are the two Phase-3 drift guards; they run inside check_spec_freshness
on every commit (via the spec-check gate), so a wrong range/URL in a doc is caught at
commit instead of shipping. This test pins them against synthetic .h/.md text.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))

from check_specs import _check_range_drift, _check_author_url_drift  # noqa: E402


# ---- range drift ----

def test_range_drift_flags_a_conflicting_range():
    src = 'controls_.addUint8("floor", floor, 0, 255);'
    md = "- `floor` — noise floor (0–128)."     # .md says 0–128, .h says 0–255
    issues = _check_range_drift(src, md)
    assert issues and "floor" in issues[0] and "0–255" in issues[0]


def test_range_drift_silent_when_ranges_match():
    src = 'controls_.addUint8("freq_x", freq_x, 1, 8);'
    md = "- `freq_x` — wave frequency (1–8)."
    assert _check_range_drift(src, md) == []


def test_range_drift_tolerates_hyphen_and_to_spellings():
    src = 'controls_.addUint8("count", count, 1, 255);'
    for prose in ("(1-255)", "1 to 255", "(1–255)"):
        md = f"- `count` — rings {prose}."
        assert _check_range_drift(src, md) == [], prose


def test_range_drift_silent_when_prose_states_no_range():
    # Many controls legitimately don't restate their range (pins, obvious 0–255).
    src = 'controls_.addUint8("gain", gain, 1, 255);'
    md = "- `gain` — microphone gain."
    assert _check_range_drift(src, md) == []


def test_range_drift_ignores_non_range_controls():
    # addBool / addSelect / addPin carry no numeric range → nothing to check.
    src = 'controls_.addBool("enabled", enabled);\ncontrols_.addPin("sdPin", sdPin);'
    assert _check_range_drift(src, "- `enabled` — on/off. - `sdPin` — data pin.") == []


# ---- author-URL drift ----

def test_author_url_drift_flags_missing_url():
    src = "// Author: Someone — https://github.com/acme/thing"
    md = "Origin: Someone · a different link https://github.com/acme/OTHER"
    issues = _check_author_url_drift(src, md)
    assert issues and "acme/thing" in issues[0]


def test_author_url_drift_silent_when_url_present():
    src = "// Author: Someone — https://github.com/acme/thing"
    md = "Origin: Someone · source [thing](https://github.com/acme/thing)"
    assert _check_author_url_drift(src, md) == []


def test_author_url_drift_silent_when_no_url_in_header():
    # An Author line without a URL (a plain credit) has nothing to sync.
    src = "// Author: FastLED inoise field (Mark Kriegsman)"
    assert _check_author_url_drift(src, "Origin: FastLED · inoise field") == []
