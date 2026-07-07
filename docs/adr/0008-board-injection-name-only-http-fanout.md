# 8. Board injection: SET_BOARD name-only, controls over HTTP

Status: Accepted

## Context

The web installer's catalog ships per-board control values the orchestrator pushes to a fresh device during provisioning. Every per-board control shipped today applies post-association (`Network.txPowerSetting`, Ethernet pin maps), so the ~1 s window between WiFi association and HTTP fan-out running at a wrong setting is acceptable. It would not be acceptable for a pre-association control: country code (governs which channels are scanned), antenna selector (wrong RF path at init makes the device deaf), or pre-association TX-power (some chips need the cap before the first probe).

## Decision

`SET_BOARD` over Improv-Serial carries only the board name (one vendor RPC, one Text control); every other field ships via HTTP after WiFi association. If a pre-association control is ever added, **do not extend SET_BOARD's wire format** (that couples unrelated controls to the board-name lifecycle and hides the timing constraint). Use one of two explicit escape hatches instead: a second vendor Improv RPC (`SET_<control>`) dispatched before `SEND_WIFI_CREDENTIALS` for user-configurable pre-association controls, or a board-specific sdkconfig fragment baking the value into firmware for truly board-static values.

## Consequences

- The board-name lifecycle stays decoupled from control values; the timing contract is explicit rather than buried in a growing wire format.
- The implicit option "just push it earlier in SET_BOARD" is ruled out, it tangles the lifecycle.
