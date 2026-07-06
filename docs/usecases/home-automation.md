# Home automation

Bring a projectMM device into your smart home — controlled alongside your lights, scenes, and automations — using the [MQTT module](../moonmodules/core/services/services.md#mqtt) (or, for some hubs, the built-in WLED compatibility). The device exposes on/off, brightness, and colour; a home-automation platform adopts it and drives those from its own app, voice assistant, and automations.

Integration goes **both directions**, and this page covers both:

- **Your smart home controlling the device** — a hub (Homebridge, Home Assistant, …) adopts the projectMM device and drives its on/off, brightness, and colour from its own app, voice assistant, and automations.
- **The device controlling smart-home lights** — projectMM drives **Philips Hue** bulbs as effect pixels, so your existing smart bulbs become part of a show.

The recipes below are each self-contained:

- **[Homebridge (Apple Home / HomeKit)](#homebridge-apple-home-homekit)** — bridge the device over MQTT so it appears as a HomeKit accessory. Full walkthrough (the worked example today).
- **[Drive Hue lights](#drive-hue-lights)** — point an effect at your Hue bulbs.
- **[Other platforms](#other-platforms)** — Home Assistant and beyond (an overview of the paths; detailed recipes as they land).

The control surface these integrations drive — the MQTT controls, topics, and accessory config — is documented once in [Core › Services › MQTT](../moonmodules/core/services/services.md#mqtt); this page is the setup, and links there for the reference.

## Prerequisites

- A projectMM device on your WiFi/Ethernet with the [MQTT module](../moonmodules/core/services/services.md#mqtt) present (it ships on boards whose catalog entry includes it; the Shelly model does).
- The device's **MAC suffix** — the last 6 hex of its MAC, which is its stable topic id (`projectMM/<suffix>`). Read it from the MQTT module's `mqtt_status`, or once the device is talking to the broker, `mosquitto_sub -t 'projectMM/#'`. The examples below use `563cfe`; substitute yours.

## Homebridge (Apple Home / HomeKit)

[Homebridge](https://homebridge.io) exposes non-HomeKit devices to Apple Home. The chain is the same however you host it: an **MQTT broker** sits in the middle, because Homebridge's `homebridge-mqttthing` plugin is an MQTT *client*, not a broker. The device publishes/subscribes to the broker; Homebridge talks to the same broker; HomeKit talks to Homebridge.

```
Apple Home ──HAP──▶ Homebridge ──MQTT──▶ broker ◀──MQTT── projectMM device
```

So there are two things to do: **install** a broker + Homebridge (the only part that differs by host — a Pi for a permanent setup, a Mac/PC for a quick test), then **configure** the device and the accessory (identical everywhere). If you already run Homebridge and a broker, skip straight to [Set up Homebridge](#set-up-homebridge).

### Install a broker + Homebridge

Only this part is host-specific — pick your platform. Each installs **Mosquitto** (the broker) and **Homebridge**; the next section wires them up the same way regardless. A **Raspberry Pi** is the natural permanent home (services survive a reboot); a **Mac or PC** is fine for a throwaway test. Already have both running? Jump to [Set up Homebridge](#set-up-homebridge).

Wherever the broker runs, the device reaches it over the LAN, so the broker must listen on all interfaces, not just loopback — noted per platform below. Confirm the broker sees the device at any point with:

```
mosquitto_sub -t 'projectMM/#' -v
```

#### Raspberry Pi

On Raspberry Pi OS (or any Debian-based distro):

```bash
# broker
sudo apt update && sudo apt install -y mosquitto mosquitto-clients
```

The package starts a `mosquitto` **systemd service**. Mosquitto 2.x listens only on `localhost` by default, so open the LAN listener with a drop-in config — create `/etc/mosquitto/conf.d/projectmm.conf` containing:

```
listener 1883 0.0.0.0
allow_anonymous true
```

then `sudo systemctl restart mosquitto`. (`allow_anonymous true` is fine for a home LAN; add a password file if the broker is exposed more widely.)

Install Homebridge via the official Debian repository ([the apt-package guide](https://github.com/homebridge/homebridge/wiki/Install-Homebridge-on-Raspbian)) — it runs as a systemd service with the Config UI at `http://<pi-ip>:8581`. Both services persist across reboots by default (`sudo systemctl enable mosquitto homebridge` to be explicit), which is what makes a Pi the right permanent host.

#### macOS

```bash
# broker — run in the foreground so you see every packet (Ctrl-C to stop)
brew install mosquitto
/opt/homebrew/sbin/mosquitto -v
```

The foreground broker listens on `1883` on all interfaces, so the device reaches it at your Mac's LAN IP while Homebridge reaches it on loopback.

Install Homebridge ([homebridge.io](https://homebridge.io) has the macOS installer; `npm i -g homebridge homebridge-config-ui-x` is the CLI route). Start it with `homebridge` in a terminal, or via the menu-bar app.

**Tearing it down:** Ctrl-C the foreground `mosquitto` and quit Homebridge — nothing persists as a service.

#### Windows

```powershell
# broker
winget install EclipseFoundation.Mosquitto
```

The installer registers a Windows service and installs the tools under `C:\Program Files\mosquitto\`. For a test, stop the service and run it in the foreground — Mosquitto 2.x binds only to `localhost` unless told otherwise, so pass a one-line config that opens the LAN listener (create `test.conf` with `listener 1883 0.0.0.0` and `allow_anonymous true`):

```powershell
net stop mosquitto
& 'C:\Program Files\mosquitto\mosquitto.exe' -v -c test.conf
```

Allow `mosquitto.exe` through Windows Defender Firewall on `1883` the first time (accept the prompt, or add an inbound rule for TCP 1883).

Install Homebridge for Windows ([the official installer](https://github.com/homebridge/homebridge/wiki/Install-Homebridge-on-Windows-10-11)) — it runs as a service with the Config UI at `http://localhost:8581`.

**Tearing it down:** Ctrl-C the foreground `mosquitto` (leave the broker service stopped, or disable it in **Services**); stop the Homebridge service from the Config UI or **Services**.

### Set up Homebridge

With a broker and Homebridge running — anywhere — the rest is host-independent: point the device at the broker, then add one accessory to Homebridge. Do this whether you just installed both above or already had them.

1. **Point the device at the broker.** In the device's web UI, open the **MQTT** module and set:
   - `broker` — the broker's hostname or IP. Use the broker host's **LAN** IP (or `.local` name) so the device can route to it — not `127.0.0.1`, which is loopback on the *host*, unreachable from the device. Find it with `hostname -I` (Pi/Linux), `ipconfig getifaddr en0` (macOS), or `ipconfig` (Windows).
   - `port` — usually `1883`.
   - `username` / `password` — only if your broker requires auth (the test brokers above don't).

   `mqtt_status` turns to `connected` once it reaches the broker, and `mosquitto_sub -t 'projectMM/#' -v` shows the device's `.../on/get`, `.../brightness/get`, `.../hsv/get`, and retained `.../name`.

2. **Add the accessory to Homebridge.** Install the [`homebridge-mqttthing`](https://github.com/arachnetech/homebridge-mqttthing) plugin (Config UI **Plugins** screen, or `npm i -g homebridge-mqttthing`), then add the `lightbulb` accessory block from [Core › Services › MQTT § Homebridge](../moonmodules/core/services/services.md#mqtt) to your Homebridge config — via the Config UI **Config** editor, or `~/.homebridge/config.json` for a CLI install. Set:
   - `url` — `mqtt://<broker>:1883`. When Homebridge and the broker share a host (the usual case), `mqtt://127.0.0.1:1883` works, since here loopback *is* the broker.
   - `563cfe` — replaced by your device's MAC suffix throughout the `topics`.
   - `username` / `password` — matching the broker, or removed if none.

   Restart Homebridge.

### Pair it in Apple Home

Once the broker, device, and Homebridge are all talking (the `mosquitto_sub` window shows the device, and Homebridge lists the accessory), add it to HomeKit: in the **Home** app, **Add Accessory → More options →** the Homebridge bridge, using the PIN Homebridge prints on start (or shown in the Config UI). The device shows up as a light — on/off, brightness, and the colour wheel that steps through palettes — the same control surface a real hub gives, verified locally.

## Drive Hue lights

The other direction: instead of a hub controlling the device, the **device controls your Philips Hue bulbs**, treating each colour bulb as a pixel of an effect. Your existing smart lights join the show — an effect's colours glide across them alongside (or instead of) an LED strip. This is a projectMM **output driver**, not a hub integration, so there's no broker and no Homebridge — the device talks straight to the Hue bridge over its LAN HTTP API.

Because Hue is a rate-limited HTTP hub (~10 commands/s), this is **smooth ambient colour**, not fast strobing — the driver paces itself to the bridge and lets it fade between colours. Full behaviour, controls, and the wire contract are in the driver reference: [Drivers › Hue](../moonmodules/light/drivers/drivers.md#hue).

**Recommended layout:** set up a **one-dimensional grid — width 1, height = the number of Hue lights** you want to control. Each pixel of that column maps to one bulb (the driver assigns window pixels to bulbs in order), so a 1×N grid gives you exactly N discrete lights with no wasted pixels, and 1D effects (rainbow, chase, …) read naturally across the bulbs. Sizing the grid to your bulb count keeps the effect and the driver in step.

To set it up:

1. **Add a Hue driver.** In the device's web UI pipeline (**Layers → a Layer → its Drivers**), add a **Hue** driver. Enter your bridge's IP in `bridgeIp` (find it in the Hue app, or at [discovery.meethue.com](https://discovery.meethue.com)).
2. **Pair with the bridge.** Press the physical **link button** on the Hue bridge, then click the driver's **`pair`** button within ~30 seconds. The device claims an app key (stored on the driver as `appKey`) — a one-time step; the status line reports `paired, N lights`.
3. **Pick what it drives.** The driver lists the bridge's colour-capable, reachable bulbs and its rooms; use the `room` / `light` controls to aim the effect at all bulbs, one room, or a single light. Each selected bulb becomes one pixel of the driver's window.
4. **Run an effect.** Any effect on the layer now drives the bulbs — the global brightness slider and colour-order correction apply to them just like a physical strip (brightness 0 turns a bulb off).

*Note:* a bulb is only driven if it's an "Extended color light" and currently reachable — a white-only bulb, a plug, or a powered-off light is skipped. For true real-time (fast) Hue, the [Hue Entertainment API](https://developers.meethue.com/develop/hue-entertainment/) (DTLS streaming) is a separate future path; today's driver targets the standard API's ambient-colour sweet spot.

## Other platforms

Homebridge above is the worked example; other home-automation platforms adopt the same device through the same primitives. The building blocks are shared — the [MQTT control surface](../moonmodules/core/services/services.md#mqtt) (broker + the `on`/`brightness`/`hsv` topics) and, for hubs that speak it, the device's WLED compatibility — so a new integration is a new front-end on top, not new device work.

- **Home Assistant** — HA adopts the device **without MQTT**: its built-in WLED integration discovers it over the existing WLED `/json` API the device already serves (the same interface the WLED apps use), so on/off + brightness work with no broker in the middle. An MQTT path (HA's MQTT Discovery, for the full control surface) is the natural next step. *Detailed recipe to be added.*
- **Others** (Node-RED, openHAB, voice assistants via a hub, …) — anything that speaks MQTT can drive the device through the [control topics](../moonmodules/core/services/services.md#mqtt); the broker setup is the same [install step](#install-a-broker-homebridge) as above, only the consumer differs. *Recipes added as they're worked through.*

When a platform gets a full walkthrough, it becomes its own `##` section beside Homebridge, listed in the intro — the per-integration recipe is self-contained, while the shared reference stays in [Core › Services › MQTT](../moonmodules/core/services/services.md#mqtt).

## Troubleshooting

- **`mqtt_status` stuck on `connecting`** — the device can't reach the broker. Check the broker IP is your computer's *LAN* IP (not `127.0.0.1`, which the device can't route to), that the broker is actually listening on all interfaces (on Windows, the `listener 1883 0.0.0.0` line above), and that the firewall isn't blocking `1883`.
- **Homebridge shows "No Response"** — the accessory's topics don't match the device's MAC suffix, or the `url` points at the wrong broker. Confirm the suffix with `mosquitto_sub -t 'projectMM/#'` and that the same broker appears in both the device's `broker` control and the accessory `url`.
- **Colour wheel doesn't match a specific colour** — expected: HomeKit sends a full-precision hue, and the device snaps it to the *nearest* built-in palette (there's no arbitrary-colour mode). See the palette note in the [MQTT reference](../moonmodules/core/services/services.md#mqtt).
