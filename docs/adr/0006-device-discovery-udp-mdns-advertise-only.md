# 6. UDP presence for discovery, mDNS advertise-only

Status: Accepted

## Context

The DevicesModule refactor reached for a UDP beacon, swung to "mDNS is the standard, use it for discovery too," then, after bench measurement, landed elsewhere. A blocking mDNS PTR query for a service the device *also advertises* exhausts the IDF mDNS pool (`Cannot allocate memory` with megabytes free) and the device's own advertisement vanishes from peers. The "use the standard" instinct was right about announce-to-foreign-apps and wrong about discover-peers; only measuring the wire separated them.

## Decision

Use **UDP presence for discovery** (where we control both ends, or a foreign system already broadcasts, e.g. WLED's 44-byte packet on UDP 65506), and **mDNS advertise-only** (only where a foreign app requires it, e.g. the native WLED app finds us solely via mDNS `_wled._tcp`). The `DevicePlugin` seam is transport-agnostic (`discoveryPort()` / `classifyPacket(...)`), not mDNS-shaped.

## Consequences

- The mDNS-pool exhaustion is structurally impossible: discovery no longer queries, mDNS shrinks to advertise-only.
- Two directions of one interop can use different transports: we discover WLED over UDP 65506; WLED's app discovers us over mDNS.
- The lesson that generalised: read the prior art's *actual* behaviour rather than assuming it; a self-inflicted bug misattributed to "the network" or "the chip" wastes hours (the WLED app discovering WLED on classic ESP32s proves classic mDNS propagates, so an invisible projectMM device is our bug).
