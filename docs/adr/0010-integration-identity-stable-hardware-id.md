# 10. Integration identity is a stable hardware id, never the editable name

Status: Accepted

## Context

The MQTT module first derived its topic prefix from the user-editable `deviceName` (`projectMM/<deviceName>`). On the bench a rename (ShellyOne → ShellyTwo) instantly repointed every topic and orphaned the Homebridge config: the hub kept publishing to the old topics while the device listened on new ones, showing "not responding." WLED, Tasmota, ESPHome, and HA MQTT discovery are unanimous, the machine-facing identity anchors to a stable hardware id (WLED's `wled/<last6-of-MAC>` is rename-stable; HA discovery *requires* a stable `unique_id` and forbids the device name / hostname as identity).

## Decision

Any control-plane identity an external system binds to (an MQTT topic prefix, an HA discovery `unique_id`, an API key path) derives from an immutable hardware id, never the editable `deviceName`. MQTT topics derive from `projectMM/<last6-of-MAC>`; the friendly `deviceName` rides a separate retained `<prefix>/name` topic as a published-but-non-identifying field.

## Consequences

- A device rename updates only the display label; every external integration stays bound to the stable id.
- The rule applies to any future integration, not just MQTT: derive the identity from something immutable, keep the human name a separate non-identifying field. Restated as an invariant in [architecture.md § Device name](../architecture.md#device-name-one-identity-every-network-name-derives-from-it).
