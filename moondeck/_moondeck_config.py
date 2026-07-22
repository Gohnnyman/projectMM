"""Shared moondeck.json helpers used across the check/ and run/ scripts.

Kept dependency-free (stdlib only) so a PEP-723 script can import it after adding
moondeck/ to sys.path, without threading extra `--with` deps. Mirrors the shared
`_net_probe.py` pattern in scenario/, one level up so both check/ and run/ reach it.
"""

import json
import urllib.request
from pathlib import Path

_ROOT = Path(__file__).resolve().parent.parent
_STATE = _ROOT / "moondeck" / "moondeck.json"

# logLevel option indices, matching SystemModule's addSelect order
# (None, Error, Warn, Info, Debug, Verbose).
LOG_NONE, LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG, LOG_VERBOSE = range(6)


def active_device_ips():
    """ESP32 device IPs in the active network (from moondeck.json). Skips the desktop
    entry (its ip carries a :port) and anything without an ip. [] if unresolvable."""
    if not _STATE.exists():
        return []
    try:
        state = json.loads(_STATE.read_text(encoding="utf-8"))
        active = next((n for n in (state.get("networks") or [])
                       if n.get("name") == state.get("active_network")), None)
        return [d["ip"] for d in (active or {}).get("devices", [])
                if d.get("ip") and ":" not in d["ip"]]
    except Exception:
        return []


def set_log_level(ips, index):
    """POST System.logLevel=<index> to each device IP. The value is a numeric JSON index
    (a quoted value parses to 0). Best-effort per device: a stale/absent entry is skipped,
    never raised, so it can't fail the caller's real work (a KPI capture, a monitor session)."""
    body = json.dumps({"module": "System", "control": "logLevel", "value": index}).encode("utf-8")
    for ip in ips:
        req = urllib.request.Request(f"http://{ip}/api/control", data=body,
                                     headers={"Content-Type": "application/json"}, method="POST")
        try:
            urllib.request.urlopen(req, timeout=3).read()
        except Exception:
            pass
