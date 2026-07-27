# 12. Home Assistant discovery: WLED by default, MQTT discovery opt-in

Status: Accepted

## Context

A projectMM device can announce itself to Home Assistant over two independent auto-discovery mechanisms, and it implements both:

- **WLED integration** — HA's built-in WLED integration discovers the device over mDNS (`_wled._tcp`) and validates it by fetching `/json`; no broker. The `HttpServerModule` WLED-compat shim serves the `/json` shape `frenck/python-wled` requires, so HA adopts the device as a light with color, palette, and diagnostic sensors.
- **MQTT discovery** — the `MqttModule` publishes a retained `homeassistant/light/<id>/config`, the Tasmota/ESPHome/Zigbee2MQTT convention; HA (and any Discovery-aware hub) auto-creates a wired light. This needs an MQTT broker.

Both were on by default. The result on the bench: HA created a light entity from *each* path, so every device appeared **twice** — one `platform=wled` card (rich: color, palette, sensors) and one `platform=mqtt` card (on/off + brightness only, per the discovery config). The duplication reads as a bug and confuses which card to use.

The two paths are not redundant. WLED needs no broker and carries a richer entity, so it is the better default. MQTT discovery reaches where mDNS cannot — a broker-only network, or a device on a different subnet/VLAN from HA — so it earns its place as a fallback, not as a second simultaneous announcement.

HomeKit does not enter the decision: neither path exposes HomeKit directly. Apple Home is reached by HA's HomeKit Bridge re-exposing whatever HA entity exists, so once the device is a light in HA (via either path) HomeKit works with no extra device-side protocol.

## Decision

Default to the **WLED** path; make **MQTT discovery opt-in**.

The `haDiscovery` control defaults **off**. A device on defaults appears in HA exactly once, via the WLED `/json` shim over mDNS, with the full color/palette/sensor entity and no broker required. Turning `haDiscovery` on publishes the retained MQTT discovery config for setups where mDNS does not reach HA (broker-only, cross-subnet). The two never announce the same device to the same hub by default, so HA never double-lists it.

Toggling `haDiscovery` re-announces / retracts live (an empty retained config removes the MQTT entity), no reconnect.

## Consequences

- One HA card per device out of the box, from the richer WLED path, with zero broker setup — the common case is clean by default.
- MQTT discovery remains available for the networks where mDNS fails; the capability isn't lost, only its default.
- A device that wants *both* (rare) still can — turn `haDiscovery` on and accept the two cards deliberately.
- The WLED shim becomes the load-bearing HA surface, so its `/json` correctness matters for every default install (the `seglc` capability-code and complete-segment fixes recorded in [lessons.md](../history/lessons.md) were found because of this).
- This is [*Common patterns first*](../../CLAUDE.md#principles) applied to discovery: when a device speaks two auto-discovery protocols to one hub, exactly one is the default or the hub double-lists it.
