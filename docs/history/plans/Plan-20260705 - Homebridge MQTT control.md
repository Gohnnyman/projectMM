# Plan — Homebridge (MQTT) control + shared on/off, HomeKit color-wheel → palette

## Context

A user wants **Homebridge** to control projectMM devices over **MQTT** (the `homebridge-mqttthing`
plugin). Today the device is controllable from the WLED native app and IR, but has no MQTT service.
We build our **own** MQTT 3.1.1 client — no libraries — fully test-guarded with golden-vector frame
tests, the same way `ImprovFrame.h` is pinned. Home Assistant is a **separate later increment**
(via the existing WLED `/json` shim, ~12 lines — HA's WLED integration reuses the same API the WLED
app does); it is *mentioned* here, not designed.

Underpinning this is a **shared `on` control on the Drivers module** — independent of brightness —
that IR, MQTT, the WLED app, and (later) HA all drive through the one apply-core
`Scheduler::setControl(module, control, json)`. Adding it lets us **delete** the WLED shim's on/off
fudge (a genuine subtraction). The user's on/off request thus lands once and is reused everywhere.

For palette control, HomeKit has no "palette" concept but has a native **color wheel** — so the
HomeKit hue drives a **nearest-palette-by-hue** selection (each built-in palette carries a
representative hue; incoming hue picks the closest). The color wheel becomes a natural palette
selector, no non-native control on the HomeKit tile.

**Decisions locked (product owner):** hand-rolled MQTT (no libs); controls = on/off + brightness +
palette; palette via HomeKit color wheel using nearest-palette-by-hue; broker accepts a **hostname**
(add DNS `getaddrinfo` to the platform layer — first DNS use, a reusable primitive); `MqttPacket.h`
lives in **`src/core/`** (a control transport, sibling of `ImprovFrame.h`/`WledPacket.h`; the pixel
formats ArtNet/DDP/E131 live in `light/`). Order: on/off + Homebridge first; HA later.

## Increments (core deliverable = 1–3; palette-by-hue = 4; HA deferred)

1. **`on` control on Drivers** + WLED-shim subtraction + tests — foundation, independently useful
   (improves the existing WLED app immediately).
2. **IR on/off learnable toggle** — small, rides on #1.
3. **Platform `TcpConnection::connect(host,port)` (with DNS) + `MqttPacket.h` + `MqttModule`
   (on/off + brightness)** + Homebridge config — the core deliverable.
4. **Palette over MQTT via HomeKit color wheel** — `nearestPaletteForHue` + HSV topic mapping.
5. *(Deferred, not designed here)* HA via the WLED `/json` shim.

## Files

### New
| File | Why |
|---|---|
| `src/core/MqttPacket.h` | MQTT 3.1.1 wire format: constants + inline `build*`/`parse*` + a byte-at-a-time inbound parser, the `ImprovFrame.h` shape. In `core/` — a control transport (sibling of `ImprovFrame.h`/`WledPacket.h`), not a pixel format. |
| `src/core/MqttModule.h` + `.cpp` | The service module (`.h`+`.cpp` per the core convention; it has real socket-lifecycle logic). |
| `test/unit/core/unit_MqttPacket.cpp` | Golden-vector + round-trip, mirrors `unit_ImprovFrame.cpp`. |
| `test/unit/core/unit_MqttModule.cpp` | Subscribe→apply routing via an injected inbound byte feed + FakeDrivers. |
| `docs/moonmodules/core/MqttModule.md` | The `registerType` doc; carries the Homebridge how-to snippet. |

### Edited
| File | Change |
|---|---|
| `src/light/drivers/Drivers.h` | Add `bool on = true` control; gate the correction LUT on it (§2). |
| `src/light/Palette.h` | Add representative-hue-per-palette + pure `nearestPaletteForHue(hue)→index` (§4). |
| `src/core/IrModule.h` | Add a `Toggle` action kind + an `on/off` action row (§3). |
| `src/core/HttpServerModule.cpp` | **Subtraction**: `applyWledState` writes the real `on`; `writeWledStateBody` reads it; add `driversOn()` beside `driversBrightness()` (§2). |
| `src/platform/platform.h` + desktop + esp32 impls | `TcpConnection::connect(host,port,timeoutMs)` with DNS (§3a). |
| `src/main.cpp` | `registerType<MqttModule>` + construct/inject/`markWiredByCode()`/`networkModule->addChild(mqtt)`. |
| `test/CMakeLists.txt` | Add the two new unit-test sources. |
| `test/unit/core/unit_IrModule.cpp` | Toggle test case (§5). |
| `test/unit/core/unit_HttpServerModule_apply.cpp` | Update WLED on/off expectations to the real-`on` behaviour (§5). |

## 2. The `on` control (shared foundation)

Add `bool on = true;` to `Drivers.h` (beside `brightness` at :239); register
`controls_.addBool("on", on);` first in `onBuildControls()` (:274) so it renders at top. Gate the
LUT via one helper both call sites use (no duplicated ternary):
`uint8_t effectiveBrightness() const { return on ? brightness : 0; }`, then
`correction_.rebuild(effectiveBrightness(), lightPreset)` in `onUpdate` (:291) and `setup` (:303),
and add `"on"` to the `onUpdate` branch that matches `brightness`/`lightPreset` (:289) so it rebuilds
+ propagates via the same `onCorrectionChanged()` loop. `controlChangeTriggersBuildState` stays false
→ on/off is as fluent as brightness. `on=false` scales the whole LUT to black
(`briLut[v]=(v*0)/255`, Correction.h:39) while **preserving** the `brightness` value — `on=true`
restores instantly. Textbook "compute the effective value where it's consumed," no shadow variable,
no hot-path touch.

**WLED subtraction** (HttpServerModule.cpp): add `driversOn()` mirroring `driversBrightness()`
(:991; `ControlType::Bool`, name `"on"`, default `true` if absent). `writeWledStateBody` (:1019):
`driversOn() ? "true" : "false"` instead of `bri>0`. `applyWledState` (:1063-1067): **delete** the
`if(!on)bri=0; else if(bri<0)bri=…128` fudge; replace with a direct
`applySetControl("Drivers","on", on?"{\"value\":true}":"{\"value\":false}")` plus the untouched
`bri` clamp. Net deletion of the restore-128 heuristic + `bri` coupling + `bri<0` sentinel.

## 3. IR on/off toggle

Delta can't toggle a bool. Add `enum class ActionKind : uint8_t { Delta, Toggle };` and a
`ActionKind kind = ActionKind::Delta;` field on `Action` (IrModule.h:107) — existing rows keep their
aggregate form, `kind` defaults. Add one row
`{"on/off","code on/off","Drivers","on",0,ActionKind::Toggle}` and `"on/off"` to `kLearnOptions`
(:122; `kActionCount+1` covers it). In `runAction` (:160) branch before the delta math: for
`Toggle`, read `controlIntValue(c)!=0` (a Bool is a 1-byte object, reads fine through the existing
`uint8_t*` path at :195) and `setControl` the inverse. Everything else (learn select, `codeStr_`
persistence) works unchanged — the toggle is just another data row.

### 3a. Platform `TcpConnection::connect` (with DNS)
MQTT needs a **persistent, non-blocking, outbound** TCP client to a **hostname** — none exists
(all socket paths are inbound-`accept` or IP-only). Add to `platform::TcpConnection`:
`bool connect(const char* host, uint16_t port, uint32_t timeoutMs);` — resolve `host` with
`getaddrinfo` (first DNS use; brokers are named), then the proven non-blocking-connect-with-`select`
block lifted from `httpRequest` (platform_desktop.cpp:597-620 + esp32 equivalent), leaving the
socket **non-blocking** after connect (MQTT uses the existing non-blocking `read()`/`writeSome()`).
Desktop + ESP32 (lwip) impls; caller gates on `networkReady()`. *Follow-up subtraction (not now):*
`httpRequest`'s inline connect could later call this.

### 3b. `MqttPacket.h` (golden-vector tested, `ImprovFrame.h` shape)
`namespace mm`, dependency-free, inline. Packets: type nibbles (CONNECT/CONNACK/PUBLISH/SUBSCRIBE/
SUBACK/PINGREQ/PINGRESP/DISCONNECT), proto `"MQTT"` level `0x04`; **remaining-length varint**
encode/decode (the fiddly bit — textbook 7-bit continuation, tested at 0/127/128/16383/16384);
`buildConnect(clientId,user,pass,keepalive,…)`, `parseConnack`, `buildPublish(topic,payload,…)`
(QoS0), `buildSubscribe(packetId,topic,…)`, `parseSuback`, `buildPingreq`, `buildDisconnect`; and an
`MqttInboundParser` byte-at-a-time state machine (like `ImprovFrameParser`) exposing a completed
PUBLISH's topic/payload — the seam that makes the receive path host-testable with no socket.

### 3c. `MqttModule` (Improv/DevicesModule template)
`MoonModule` subclass, honours `enabled` (a user genuinely disables MQTT), `userEditable()=false`.
Injects `SystemModule` (default prefix = deviceName); reaches Drivers via
`Scheduler::instance()->setControl` (like IR — no HttpServer dependency). **Controls:** `broker`
(Text, hostname/IP), `port` (Uint16, 1883), `username` (Text), `password` (`addPassword` — reuses
the WiFi-password secret serialization, Control.h:320, no new obfuscation), `prefix` (Text, default
`projectMM/<deviceName>`), `mqtt_status` (ReadOnly). **Lifecycle** all on `loop1s()` (off the hot
path): connect lazily gated on `networkReady() && enabled` with reconnect backoff; CONNECT→CONNACK→
SUBSCRIBE; PINGREQ every keepalive/2 (reconnect if no PINGRESP); drain `read()` into the parser and
route `set` PUBLISHes to Drivers via `setControl`; publish `get` topics on change + on connect (so
mqttthing never shows "No Response"). Fixed-size member buffers; `conn_` is a `TcpConnection` member.

**Topics** (mqttthing "lightbulb"):
```
<prefix>/on/set          ← "true"/"false" → Drivers.on
<prefix>/on/get          → publish current on
<prefix>/brightness/set  ← 0..100 → *255/100 → Drivers.brightness
<prefix>/brightness/get  → publish brightness*100/255
<prefix>/hsv/set         ← "h,s,v" → hue → nearestPaletteForHue → Drivers.palette (§4)
<prefix>/hsv/get         → publish "<rep-hue>,100,<bri%>" for the chosen palette
```

## 4. Palette via HomeKit color wheel (nearest-palette-by-hue)

In `src/light/Palette.h`, alongside `palettes::kBuiltins`: a parallel `constexpr uint16_t`
representative hue per built-in (Lava≈15 red-orange, Ocean≈210 blue, Forest≈120 green, Party≈300
magenta, Rainbow = a low-sat/degenerate case → default index 0), and a pure, unit-tested
`uint8_t nearestPaletteForHue(uint16_t hue)` — minimal **circular** hue distance (wrap at 360).
**Boundary:** the MQTT module is core and must not `#include "light/Palette.h"`. Resolve by exposing
the conversion through the already-core-reachable `Palettes::` static API (add
`static uint8_t Palettes::nearestForHue(uint16_t)` delegating to the light-side pure function) — the
same way IR/WLED reach light state without a light include. MQTT converts hue→index, then
`setControl("Drivers","palette",{index})`. `hsv/get` publishes the chosen palette's representative
hue so the HomeKit tile snaps to a sensible color. Saturation ignored for now (very-low-sat may map
to index 0). *This is the palette increment; on/off + brightness ship first.*

## 5. Tests

| Test | Unit/HW | Pins |
|---|---|---|
| `unit_MqttPacket.cpp` | Unit | Golden vectors CONNECT/SUBSCRIBE/PUBLISH/PINGREQ (byte-exact); PUBLISH round-trip; remaining-length varint at 0/127/128/16383/16384; CONNACK/SUBACK/PINGRESP parse; **fragmented-PUBLISH** feed across `read()` boundaries reassembles. |
| `unit_MqttModule.cpp` | Unit | Rig = Scheduler + FakeDrivers (on/brightness/palette) + `feedForTest(bytes,len)` (mirrors `injectCodeForTest`). PUBLISH `on/set`"false"→on=false; `brightness/set`"50"→127; `hsv/set` a blue hue→Ocean index. No socket/broker. |
| Drivers on/off | Unit | `on=false` → `correction_.briLut[255]==0` **and** `brightness` unchanged; `on=true` → LUT restored to `brightness`. |
| IR toggle | Unit | Learn a code to `on/off`; fire → FakeDrivers.on flips; fire again → flips back. |
| `nearestPaletteForHue` | Unit | red→Lava, blue→Ocean, green→Forest, 359 wraps ≈ 0. |
| WLED apply update | Unit | `applyWledState("{\"on\":false}")` → Drivers.on=false, brightness untouched (was: bri→0). |
| Broker end-to-end | **HW** | mosquitto + homebridge-mqttthing driving a real device. Not ctest. |

## 6. Homebridge config (user pastes)

`docs/moonmodules/core/MqttModule.md` carries the minimal `homebridge-mqttthing` "lightbulb" block
(on/off + brightness + the HSV color wheel for palette), prefix `projectMM/<deviceName>`:
```json
{
  "accessory": "mqttthing", "type": "lightbulb", "name": "projectMM",
  "url": "mqtt://<broker>:1883", "username": "<user>", "password": "<pass>",
  "topics": {
    "getOn": "projectMM/MM-3A7F/on/get", "setOn": "projectMM/MM-3A7F/on/set",
    "getBrightness": "projectMM/MM-3A7F/brightness/get", "setBrightness": "projectMM/MM-3A7F/brightness/set",
    "getHSV": "projectMM/MM-3A7F/hsv/get", "setHSV": "projectMM/MM-3A7F/hsv/set"
  },
  "onValue": "true", "offValue": "false"
}
```
HA gets a one-line mention (works via the WLED `/json` shim, a separate increment).

## 7. Verification

1. Desktop build `cmake --build build` (-Wall -Wextra -Werror; the `getaddrinfo`/`connect` path clean
   on desktop + ESP32 toolchains).
2. `ctest` (the six unit tests above) + `uv run moondeck/scenario/run_scenario.py` (no regression).
3. Broker/HW: flash a WiFi board → set broker/port/user/pass/enabled in the UI → run `mosquitto` →
   `mosquitto_sub -t 'projectMM/#'` to watch state → install homebridge-mqttthing (§6) → toggle
   on/off + brightness + color from the Home app; confirm the strip responds, the color wheel snaps
   palettes, and the tile never shows "No Response".
4. Platform boundary: all MQTT socket I/O via `platform::TcpConnection` + `networkReady()` only; the
   one new socket capability (`connect`+DNS) lives in `src/platform/`; `MqttPacket.h` is pure byte
   math, no platform include.

## 8. Subtraction / principle check

**Removes:** the WLED on/off fudge (restore-128 heuristic + `bri` coupling + `bri<0` sentinel,
HttpServerModule.cpp:1063-1067). **Adds:** one `on` control (shared by IR/MQTT/WLED/HA — the first
slice of the backlog LightsControl global state); one IR row + `kind` tag; `TcpConnection::connect`
+ DNS (a reusable primitive — any future named-host client, incl. HA push); `MqttPacket.h` +
`MqttModule` (expected domain growth); `nearestPaletteForHue` (a pure light-domain helper); docs +
tests. Mirrors `ImprovFrame.h`/Improv/DevicesModule (*Common patterns first*); MQTT 3.1.1 written
fresh (*Industry standards, our own code*); no hot-path touch (*Data over objects*); every control
live (*No reboot to apply*). Nothing fights a hard rule.

Save the approved plan to `docs/history/plans/Plan-20260705 - Homebridge MQTT control.md`.
