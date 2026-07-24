"""Shared moondeck.json helpers used across the check/ and run/ scripts.

Kept dependency-free (stdlib only) so a PEP-723 script can import it after adding
moondeck/ to sys.path, without threading extra `--with` deps. Mirrors the shared
`_net_probe.py` pattern in scenario/, one level up so both check/ and run/ reach it.
"""

import json
import urllib.request
from contextlib import contextmanager
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


def _get_log_level(ip):
    """Read one device's current System.logLevel index from /api/state, or None if unreachable
    or unparseable. Used to snapshot a device before a temporary change so it can be restored."""
    try:
        with urllib.request.urlopen(f"http://{ip}/api/state", timeout=3) as r:
            state = json.loads(r.read().decode("utf-8", errors="replace"))
    except Exception:
        return None

    def walk(node):
        if isinstance(node, dict):
            if node.get("type") == "SystemModule":
                for c in node.get("controls", []):
                    if c.get("name") == "logLevel":
                        v = c.get("value")
                        return v if isinstance(v, int) else None
            for v in node.values():
                found = walk(v)
                if found is not None:
                    return found
        elif isinstance(node, list):
            for v in node:
                found = walk(v)
                if found is not None:
                    return found
        return None

    return walk(state)


def set_log_level(ips, index):
    """POST System.logLevel=<index> to each device IP. The value is a numeric JSON index
    (a quoted value parses to 0). Best-effort per device: a stale/absent entry is skipped,
    never raised, so it can't fail the caller's real work (a KPI capture, a monitor session)."""
    body = json.dumps({"module": "System", "control": "logLevel", "value": index}).encode("utf-8")
    for ip in ips:
        try:
            # Request() is inside the try too: a malformed IP makes its construction raise, and that
            # must skip only this device — not abort the loop and strand the rest (which, under
            # raised_log_level, would also skip the restore of every device after it).
            req = urllib.request.Request(f"http://{ip}/api/control", data=body,
                                         headers={"Content-Type": "application/json"}, method="POST")
            urllib.request.urlopen(req, timeout=3).read()
        except Exception:
            pass


@contextmanager
def raised_log_level(ips, index=LOG_INFO):
    """Temporarily raise each device to `index` (default Info, so the serial tick line prints) and
    restore each device's ORIGINAL level on exit — not a hardcoded Warn, so a device the user had set
    to Debug/Error keeps its choice. A device whose level can't be read is still raised, and restored
    to Warn (the resting default) as the honest fallback. Best-effort throughout; never raises."""
    saved = {ip: _get_log_level(ip) for ip in ips}
    set_log_level(ips, index)
    try:
        yield
    finally:
        for ip in ips:
            prev = saved.get(ip)
            set_log_level([ip], prev if prev is not None else LOG_WARN)
