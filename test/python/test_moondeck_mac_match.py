"""MoonDeck breadcrumb MAC matching (_mac_matches).

After a flash, flash_esp32.py records esptool's raw efuse MAC in the breadcrumb, and
discover links the just-flashed serial port to the probed device whose reported MAC
matches. On most chips the device reports that same efuse MAC, so an exact compare
works. But the ESP32-S31's `esp_efuse_mac_get_default()` returns the EUI-64 encoding
of the efuse MAC (FF:FE inserted after the OUI) truncated to 6 bytes — so the device
reports 30:ED:A0:FF:FE:F3 for an efuse MAC of 30:ED:A0:F3:D4:68, and a raw compare
never links the port. _mac_matches accepts both forms.

Regression: without the EUI-64 tolerance the S31's port silently never linked in
moondeck.json after a flash. Run: `uv run --with pytest pytest test/python -q`.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from moondeck import _mac_matches  # noqa: E402


def test_exact_match_case_insensitive():
    assert _mac_matches("AA:BB:CC:DD:EE:FF", "aa:bb:cc:dd:ee:ff")
    assert _mac_matches("aa:bb:cc:dd:ee:ff", "AA:BB:CC:DD:EE:FF")


def test_s31_eui64_truncated_derivation_matches():
    # The real S31 pair seen on the bench: esptool efuse MAC vs the device's
    # esp_efuse_mac_get_default() report. OUI (30:ED:A0) kept, FF:FE inserted,
    # then truncated to 6 bytes — so only the first post-OUI efuse byte (F3) survives.
    assert _mac_matches("30:ED:A0:F3:D4:68", "30:ED:A0:FF:FE:F3")


def test_unrelated_macs_do_not_match():
    assert not _mac_matches("30:ED:A0:F3:D4:68", "11:22:33:44:55:66")
    # Same OUI but a different chip — OUI overlap must NOT be treated as a match.
    assert not _mac_matches("30:ED:A0:F3:D4:68", "30:ED:A0:AA:BB:CC")


def test_derivation_is_directional_efuse_to_device():
    # The breadcrumb always holds the efuse MAC; the device holds the derived one.
    # Matching is computed forward (efuse -> EUI-64-trunc). The reverse string in the
    # breadcrumb slot (the already-derived MAC) has no efuse to reconstruct, so it only
    # matches itself exactly — pinning that we don't over-accept.
    assert _mac_matches("30:ED:A0:FF:FE:F3", "30:ED:A0:FF:FE:F3")   # exact still fine
    assert not _mac_matches("30:ED:A0:FF:FE:F3", "30:ED:A0:F3:D4:68")  # can't go backward


def test_empty_never_matches():
    assert not _mac_matches("", "30:ED:A0:FF:FE:F3")
    assert not _mac_matches("30:ED:A0:F3:D4:68", "")
    assert not _mac_matches(None, None)


def test_malformed_only_matches_itself_never_derives():
    # A too-short breadcrumb can't drive the EUI-64 derivation (needs 6 octets), so it
    # only ever matches an identical string — never a derived one. Exact-match still
    # holds for identical inputs (harmless: same source), but no derivation is attempted.
    assert _mac_matches("30:ED:A0", "30:ED:A0")             # identical → exact branch
    assert not _mac_matches("30:ED:A0", "30:ED:A0:FF:FE")   # can't derive from 3 octets
