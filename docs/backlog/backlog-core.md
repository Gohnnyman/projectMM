# Backlog — core

Forward-looking to-build items for the **core / infrastructure** domain (`src/core/`, `src/platform/`, build, CI, network, persistence, UI). The light-domain counterpart is [backlog-light.md](backlog-light.md); items that genuinely span both are in [backlog-mixed.md](backlog-mixed.md). Index + overview: [README.md](README.md). Completed items are removed.

## Distribution

### Release 2.0 — distribution catches up to the source tree

1.0 ships ESP32 firmware (4 variants) + macOS arm64 + Windows x64. Still to add:

- **ESP32-P4** firmware variant — **`esp32p4-eth` (Ethernet-only) shipped**: in `build_esp32.py`'s `FIRMWARES`, the `deviceModels.json` catalog (Waveshare P4-NANO), and CI builds + publishes it to the web installer + releases. **Still to ship: `esp32p4-eth-wifi`** (the C6-WiFi variant) — it doesn't build reproducibly in CI yet (the `CONFIG_WIFI_RMT_*` Kconfig defaults don't survive a plain build without a fresh `set-target`), so it's held out of the release matrix until that's fixed; see § ESP32-P4 round 3.
- **ESP32-S31 web-flash (waiting on esptool-js)** — the `esp32s31` firmware ships (build, catalog, CI matrix, web installer listing), and CLI flashing works (`flash_esp32.py` → esptool.py, which has S31 support since v5.2.0). **Browser flashing does not**: the web installer's `esptool-js` (pinned 0.5.7) has no S31 chip class. Worse than a missing entry — the S31's ROM magic (`15736195`) *collides* with the classic ESP32's; esptool.py disambiguates with secondary register detection (S31 `USES_MAGIC_VALUE=False`), but esptool-js has only the magic table, so it would mis-identify the RISC-V S31 as a classic Xtensa ESP32 and flash the wrong stub/params. `install.js`'s `WEB_FLASH_UNSUPPORTED_CHIPS` guard catches an S31 connect-flash failure and points the user at the CLI. **No upstream timeline**: as of 2026-06 the esptool-js repo has zero S31 issues/PRs/commits and its last release was 2026-03 (it lags esptool.py on new chips by months). **Removal trigger**: when esptool-js ships S31 support *with* the secondary detection (not just a magic-table entry — re-check the chip-detect switch, not the version number), bump the esptool-js pin in `install-orchestrator.js` and drop `ESP32-S31` from `WEB_FLASH_UNSUPPORTED_CHIPS`.
- **ESP32-P4 v3.x silicon variant (backlog)** — `esp32p4-eth` is built for pre-v3 P4 (`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` + `REV_MIN_0`), because the v6.1 IDF default (v3.1) refused to boot on the bench/field v1.x P4 and rev <3.0 vs >=3.0 are "huge hardware difference" (one binary can't cover both). The field is pre-v3 P4 today, so the single image is correct for now. When v3.x P4 silicon arrives, add an `esp32p4-eth-v3` firmware variant (REV_MIN_300+) + the catalog/device-model mapping so each P4 board points at the matching image, rather than the pre-v3 `esp32p4-eth` for everything. (CodeRabbit flagged the single-variant exposure; deferred until v3.x P4 is actually in play.)
- **Linux desktop binary** — third desktop job in `release.yml`, static-linked libstdc++.
- **Teensy 4.1** — toolchain-file build, `.hex` for Teensy Loader.
- **Raspberry Pi** — ARM64, cross-built or native.
- **macOS code-signing** — drops the Gatekeeper "downloaded from internet" prompt.
- **Windows code-signing** — drops the SmartScreen warning on first run of `projectMM.exe`. Same shape as macOS signing; needs an EV / OV code-signing certificate (Microsoft Trusted Signing is the cheapest current option). Until then, the README notes the SmartScreen prompt.
- **Live RMII Ethernet reconfigure** — runtime PHY/pin config shipped (`ethType` + pin controls in NetworkModule, per-board defaults in `deviceModels.json`, `platform::setEthConfig`/`ethInit` dispatch). W5500 (SPI) on S3 applies **live** — `ethStop()` tears down the SPI bus and `ethInit()` re-runs on the next `loop1s()` with no reboot. RMII (classic/P4 internal EMAC) still saves config and asks for a restart to apply, because the EMAC bring-up is fiddlier to hot-cycle cleanly. Make RMII live too: a hot `esp_eth_stop` + EMAC/netif teardown + re-init on config change, matching the W5500 path, so every interface honours the no-reboot principle.
- **Installer UX polish** — clear "Pre-release (beta)" warning on RC/latest picks, yank-by-asset-tag instead of yank-by-release-deletion.
- **Offer projectMM/MoonLight as a library** — a downstream sketch where another firmware/app consumes the light pipeline (or a subset) as an embeddable dependency rather than running the whole binary. `library.json` is already a PlatformIO *library* manifest, so the seed exists. When this is designed, give it a small public **identity surface**: one runtime constant the consumer reads (a `kProjectName`, likely a `ProjectInfo` bundle of name + version + url) that the network wire-strings (ArtNet/E1.31 source-name + CID), the UI banner, and any "About" string all *derive from* — the one place a consumer queries "what am I embedding." This is the genuine home for the name-centralisation that the rename ([rename-to-moonlight.md § Phase 1.3](rename-to-moonlight.md)) deliberately *didn't* do: the rename is a one-time sweep (a constant would just split it), but a library consumer references the identity ongoing and widely, which is the test a constant must pass. Build it *then*, against the real library API (per *Concrete first, abstract later*), not speculatively now.
- **ESP32-P4 DHCP hostname not shown by the router (recheck later)** — the device sets its DHCP hostname (option 12 = `deviceName`, default `MM-XXXX`) in the `ETHERNET_EVENT_CONNECTED` handler, verified working on two boards: the S3 over WiFi (router shows `MM-70BC`) and the Olimex over RMII Ethernet (`MM-BD3C`) — the *same* `ethEventHandler` code path the P4 uses. Yet the bench P4 (Waveshare P4-NANO, RMII) still shows as blank/"Unknown" in the GL.iNet client list, while serial confirms `set_hostname` succeeds with no error. Two unconfirmed suspects, neither our logic: (1) the router holds a **sticky lease** for the P4's MAC and won't relearn the hostname until it fully expires (the per-client "forget" isn't exposed in this GL.iNet UI, and a plain reboot didn't clear it); (2) a P4-specific IDF netif quirk serializing option 12 differently on the newer P4 Ethernet path. Since the shared code path is proven on two other boards, this is not treated as a code bug. Recheck after the P4's lease naturally expires, or on a different router, before spending more on it.

### DevicesModule — interop plugins + the command half (discovery shipped)

DevicesModule discovers via **passive UDP presence** (UDP 65506) feeding a [`DevicePlugin`](../../src/core/DevicePlugin.h) seam (shipped: projectMM + WLED plugins). mDNS is advertise-only so projectMM appears in the native WLED apps + Home Assistant; the WLED-app interop (list + live colour + brightness control) is shipped too. What remains is *growth on the seam*, each piece additive (one plugin file, no core change):

- **More discovery plugins** — ESPHome, Tasmota, Hue (*hub-shaped*: a bridge whose Zigbee bulbs are children behind it, with link-button auth). Each is a new `DevicePlugin` declaring its `discoveryPort()` + classifying the datagram (or, for a system that only does mDNS, a re-introduced advertise-side browse scoped to *foreign* services only — never the ones we advertise). Hue is the canonical "more than a flat device" case the seam is shaped for. (Note: Hue *control* already ships as an **output driver**, see [HueDriver](../moonmodules/light/moxygen/HueDriver.md) — bulbs as effect pixels; the driver also *lists* its bridge in DevicesModule with the colour-light count. Two complementary follow-ups remain: (a) auto-fill the driver's bridge IP from discovery so the user doesn't type it (the mDNS-browse plugin above); (b) **pair once, not per driver** — pairing + the app key currently live on each HueDriver, so two drivers on one bridge pair twice. The clean end-state moves the bridge identity (IP + key + Pair button + light list) into DevicesModule and makes HueDriver a pure output that reads the paired bridge by IP — do this together with the discovery plugin, since both hinge on DevicesModule owning the bridge.)
- **The command half** — `DevicePlugin::command()` (+ per-plugin capability/auth), so projectMM can *control* a discovered foreign device, not just list it: set WLED brightness via its JSON API, a Hue resource via the bridge's authenticated CLIP API, a Tasmota via `cmnd`. Built when a control consumer exists; the discovery seam is already shaped to accept it (incl. hub plugins). This is the **multi-ecosystem selling point** — one UI controlling WLED + ESPHome + Hue. Commands split by need (the rule, not "all REST"): must-arrive config over REST; latency-critical sync over UDP (~0.5–1 ms vs REST's 10–50 ms — REST would visibly de-sync).
- **Live peer state** — a discovered peer's brightness / on-off shown in our list, refreshed by polling its REST `/json` after discovery gives the IP (discovery = UDP/mDNS, state = REST). The read-side complement to the command half.
- **Non-IP transports (board-gated, far future)** — Tasmota-MQTT / zigbee2mqtt need an MQTT client; **direct Zigbee/Thread** (S31/C6/H2 802.15.4 radio) makes projectMM the *hub itself*, driving bulbs over the mesh with no gateway — the standout differentiator, the biggest lift. Same plugin philosophy, a transport addition + board gate.

Full design + the reasoned transport split: [Plan-20260629 — UDP device discovery + mDNS advertise-only (shipped)](../history/plans/Plan-20260629%20-%20UDP%20device%20discovery%20%2B%20mDNS%20advertise-only%20%28shipped%29.md).

## ESP32 performance and memory

### Flash budget — the 4 MB classic ESP32 is the ceiling (investigation)

The binary has grown ~1.4 → ~1.48 MB as effects, audio sync, IR, and Ethernet landed. Per-board headroom against the app (OTA) partition slot:

| Board | app slot | binary | used |
|---|---|---|---|
| **classic esp32 (4 MB)** | 1.75 MB | ~1.48 MB | **~84 %** ⚠️ |
| esp32s3-n8r8 (8 MB) | 3.00 MB | ~1.48 MB | ~49 % |
| 16 MB boards | 4.00 MB | ~1.48 MB | ~37 % |

Only the **4 MB classic** is tight (the partition comment itself notes "~200 KB headroom"); the 8/16 MB boards have years of room. So this is a *classic-ESP32-only* constraint, not a global one — the fix should shrink the small-flash build without touching the flagship boards.

Levers, roughly by payoff-per-effort:

1. **`-Os` for the size-bound builds.** The ESP32 build currently runs the IDF default optimization (`CONFIG_COMPILER_OPTIMIZATION_DEBUG`, ~`-Og`), **not** `CONFIG_COMPILER_OPTIMIZATION_SIZE` (`-Os`). Setting size-opt on the classic (and any small-flash) firmware typically buys 5–15 % flash for free — measure the tick/FPS delta, since `-Os` can cost a little hot-path speed; if it does, gate it to the flash-bound firmwares only, not the S3/P4 where headroom is fine.
2. **`MM_MINIMAL` feature profile for the 4 MB target.** The classic board doesn't need every effect/driver compiled in. A build profile that compiles out heavy optional modules (the same `firmware_cmake_args()` seam the eth/wifi gating already uses) keeps the flagship boards full-featured while the small board ships a curated subset — the standard "small board, smaller build" pattern.
3. **Repartition the 4 MB classic.** If A/B OTA isn't required on the classic, a single-app-slot layout nearly doubles the app ceiling (1.75 → ~3.5 MB). Trade-off: OTA loses its rollback slot. Decide per-board, not globally.

Start with (1) — it's a one-line sdkconfig change with a measurable payoff and no code churn; (2)/(3) only if (1) plus normal growth still crowds the classic. The UI embed is already gzipped (`app.js` ~140 KB → ~40 KB), so UI growth is cheap in flash; the pressure is C++ `.text`.

### E1.31 multicast receive (IGMP join)

NetworkReceiveEffect accepts E1.31 via unicast only — the same scope MoonLight ships. Multicast senders address the per-universe group `239.255.{universe_hi}.{universe_lo}`, which a receiver must join via IGMP; the platform `UdpSocket` has no `IP_ADD_MEMBERSHIP` support yet (lwIP `setsockopt` on ESP32, plain `setsockopt` on desktop, plus a join-per-accepted-universe bookkeeping question). Add when a multicast-only sender actually shows up on a bench; until then the spec documents "point sACN senders at the device's IP".

### WiFi ArtNet performance (pending investigation)

128×128 WiFi ArtNet measurements exist (see [performance.md](../performance.md) "ArtNet over WiFi" and "Build-variant WiFi comparison"). Remaining matrix:

- WiFi STA 64×64 (4K LEDs, 24 universes)
- WiFi STA 32×32 (1K LEDs, 6 universes)

This determines the practical LED limit for WiFi-only boards. Until the `sdkconfig.defaults` TX-buffer fix lands (identified in the build-variant table), **prefer wired Ethernet for any ArtNet workload on classic ESP32** — the default `esp32` build carries both stacks, so Ethernet is available even when the original measurements were taken on the old `esp32-eth-wifi` variant.

### Network round-trip test — drop/reorder measurement (deferred)

`moondeck/scenario/run_network_roundtrip.py` measures desktop→device→desktop **latency and jitter** per protocol (ArtNet/E1.31/DDP) by timing how long a sent colour takes to appear in the device's preview stream. It deliberately does **not** measure per-frame **drops or reorder**, because the path can't track individual frames cleanly: `NetworkSendDriver` re-clocks at its own fps (decoupled from receive) and `NetworkReceiveEffect` holds-last-frame, so frames don't pass through 1:1 — a sequence number embedded in frame N may be re-sent 0, 1, or several times downstream. The min/median/max spread the test already reports *is* the jitter signal (it surfaced multi-second outliers on the classic ESP32). To measure true drop/reorder, the firmware would need a sequence-faithful echo path (e.g. a `NetworkReceiveEffect` echo mode that re-emits each received frame 1:1 back to the sender, bypassing the fps re-clock), then the desktop could match sent↔received sequence numbers. The test's docstring lists this under "extend later" alongside per-frame sequence matching and the device→device chain.

### Async ArtNet send — decouple the wire from the render tick (PSRAM-only)

The ArtNet send is synchronous: `NetworkSendDriver::loop()` blasts ~97 universes (a 48 KB frame at 128×128) inline, and the per-universe `send()` blocks on lwIP TX backpressure — the netif/EMAC (or WiFi) drivers throttle to wire throughput. Measured on hardware: **~35 ms over Ethernet, ~90 ms over WiFi**, charged straight to the render tick, so ArtNet alone caps the Olimex at ~15 FPS and the S3 (WiFi) at ~7 FPS at 128×128. This is a transport throughput limit, **not** something a non-blocking socket can shed — verified that neither `O_NONBLOCK` nor `MSG_DONTWAIT` makes lwIP return early for UDP (the block is below the socket API; both flags drop zero packets and cost the same ~35 ms). The earlier "non-blocking recovered it to ~2 ms" reading was a transient external condition (the receiver/switch draining the burst freely in one window), unreproducible under steady load with the exact firmware.

The real fix is a **dedicated send task**: `loop()` snapshots the corrected frame into a handoff buffer and signals the task; the send task drains it to the wire at its own pace while the render task continues. The tick stops paying the ~35–90 ms — render runs at its own rate (~30 FPS on the Olimex), ArtNet streams independently at whatever the link sustains.

**Gate this on `platform::hasPsram`. It does not fit non-PSRAM boards at the grid sizes where it would help** — the math is hard:
- The handoff buffer must hold one full frame: **48 KB at 128×128** (16384 × 3), plus a ~4 KB task stack.
- The Olimex (no-PSRAM) at 128×128 runs at **~46 KB free heap, ~18 KB largest contiguous block** (measured). The 48 KB buffer exceeds *both* the total free heap and (by far) the largest block — it can't allocate at all, the same fragmentation cliff the paged MappingLUT just had to work around.
- A double-buffer (so the task reads frame N while render writes N+1) doubles it to ~96 KB — even more out of reach.
- At 64×64 the frame is only 12 KB and *might* fit, but at 64×64 the synchronous send is already fast enough that ArtNet isn't the bottleneck — so the task buys nothing where it's affordable on no-PSRAM.

So the PSRAM gate isn't conservative; it's a hard requirement. PSRAM boards (S3/S2, Olimex-with-PSRAM variants) have megabytes for the handoff buffer via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`; non-PSRAM boards keep the synchronous send and the documented "use Ethernet / smaller grid for high FPS at large grids" guidance ([NetworkSendDriver.md](../moonmodules/light/moxygen/NetworkSendDriver.md)).

When implemented: `if constexpr (platform::hasPsram)` (or a runtime `hasPsram()` check) selects the async path; the buffer lives in PSRAM; the send task pins to the core opposite the render task (see [Task core-pinning](#task-core-pinning-backlog)). Non-PSRAM keeps `loop()`'s inline send unchanged. One handoff buffer + a binary semaphore/notification is the minimal shape — don't build a ring of frames until a second consumer needs it.

### `esp32-eth` slow Ethernet bring-up vs `esp32-eth-wifi` (investigation)

On Olimex ESP32-Gateway flashed with `esp32-eth`, Ethernet sometimes takes **a minute or more** to acquire a DHCP lease at boot. The same hardware flashed with `esp32-eth-wifi` brings Ethernet up in seconds. The B1 Idle-recovery fix in `src/core/NetworkModule.h` masks the symptom (status correctly transitions to "Eth: <ip>" once the lease arrives), but the underlying slow bring-up is a real performance regression on the eth-only build.

What we know:
- `build/esp32-esp32-eth/sdkconfig` and `build/esp32-esp32-eth-wifi/sdkconfig` are **byte-identical** (3,617 lines each, `cmp -s` confirms). So lwIP buffer pools, DHCP timeouts, and Ethernet driver settings are the same.
- Same hardware (Olimex ESP32-Gateway Rev G), same RMII pin/clock config (`EMAC_CLK_OUT` on GPIO17), same `ethInit()` code in `src/platform/esp32/platform_esp32.cpp`.
- The only difference at link time: `esp32-eth` passes `EXCLUDE_COMPONENTS=esp_wifi;wpa_supplicant;esp_coex` to ESP-IDF (see `moondeck/build/build_esp32.py:31`).
- `esp_coex` (WiFi/Bluetooth coexistence) was an early hypothesis: even though Ethernet doesn't share the radio, `esp_coex`'s init might warm a shared clock path that helps Ethernet auto-negotiation, and the eth-only build excludes it. **Disproven — see below.**

**Firmware is ruled out (the evidence is contradictory across reboots with the *same* build, which by itself proves build-independence).** Over one session, on the same Olimex + cable:
- `esp32-eth-wifi` (keeps `esp_coex`) → flapped: `Ethernet link up` → `link down` repeating, never reached DHCP.
- `esp32-eth` (excludes it) → on one flash **came up immediately and worked**; on a later flash **flapped the same way**.

So the *same* eth-only build both works and flaps at different times, and the eth-wifi build flaps too. The instability does **not** track the firmware build — it's intermittent. That kills the `esp_coex` theory and any WiFi-interference theory. It also confirms our code isn't the cause: `mm_net: Ethernet link up/down` is logged straight from the ESP-IDF `ETHERNET_EVENT_CONNECTED/DISCONNECTED` events (`platform_esp32.cpp:238-243`) — the **PHY hardware reports the drops**; `NetworkModule` only reacts, never stops/restarts the link. Memory is ruled out (boot heap 286 KB, steady 133 KB free / 110 KB block — abundant).

**Signature = physical layer.** When flapping, the link holds for ~2 s, drops, comes back ~10 s later — a repeating cycle, not random. That fingerprints the PHY auto-negotiating, holding briefly, then losing sync: marginal **PHY power, clock, cable, or connector**, not firmware.

**Correlates with board reset.** The flapping tends to *start* right after a flash/soft-reset (this session: a reset preceded each flapping window; a clean power-up tended to link cleanly). Fits the documented slow/flaky PHY re-link on the Olimex after a reset — on some resets the PHY settles, on others it cycles for a long time before (or instead of) holding.

What we still don't know (all **physical** tests — no code change is warranted):
- Does a **clean power-cycle** (vs soft reset) reliably link? (Tests the reset-relink correlation.)
- Does **barrel-jack / stronger 5V** power stop it? (Tests PHY brown-out under USB-only supply, a known Olimex-Gateway weakness.)
- Does **swapping cable / switch port** stop it? (Rules out cable/connector.)

Bottom line: intermittent, build-independent, reset-correlated → a hardware/PHY issue, not a firmware bug. The earlier "slow DHCP at boot" is likely the same root cause (the PHY cycling many times before one window holds long enough to complete DHCP). Pick this up with the physical tests above before touching any code.

### MoonDeck doc-asset endpoint hardening (backlog)

`moondeck/moondeck.py::_serve_doc_asset` accepts any ROOT-relative path and serves the file. Path traversal *is* blocked (`asset_path.relative_to(ROOT.resolve())`), but inside the repo any file is served — including local-only artefacts like `moondeck/build/wifi_credentials.json` if present. MoonDeck binds to all interfaces by design (the existing comment in `main()` explicitly enables LAN reach), so anyone on the LAN can hit the endpoint.

Two improvements when this matters:
- **Subdirectory whitelist** — only serve under `docs/` (and image asset paths the markdown renderer needs). Reject `moondeck/build/wifi_credentials.json` etc. with 403.
- **Extension whitelist** — only image / CSS / JS mime types via a small allowlist.
- **Optional bind-to-localhost flag** — `--bind 127.0.0.1` for users who don't want LAN reachability. Default stays "" (all interfaces) since the LAN-reach is the documented design.

Not blocking — MoonDeck is a developer tool, not a production server. Pick this up when MoonDeck is in scope for hardening.

### CI: pin GitHub Actions to commit SHAs (supply-chain hardening)

`.github/workflows/release.yml` references all 9 action types by mutable `@vN` tag (`actions/checkout@v4`, `astral-sh/setup-uv@v3`, `softprops/action-gh-release@v2`, `espressif/esp-idf-ci-action@v1`, …). A mutable tag can be force-moved to malicious code by a compromised publisher; pinning each `uses:` to a full commit SHA (with a `# vN` trailing comment) removes that vector. **Done already (cheaper half):** `persist-credentials: false` on every checkout that doesn't push, so the `GITHUB_TOKEN` isn't left in `.git/config` for later steps to read (the `release` job keeps it — it force-pushes the `latest` tag). **Not done (this item):** SHA-pinning, because it carries an ongoing cost — pinned SHAs go stale and miss security patches, so it only pays for itself **alongside Dependabot** (or a Renovate config) to auto-bump them. Pick this up as a deliberate "CI hardening + Dependabot" pass, not piecemeal. Low risk today: every action pinned is a first-party `actions/*` or a well-known publisher (astral, espressif, softprops), not an obscure third-party action.

### Static IP on WiFi STA — wire the existing fields to the network (backlog)

NetworkModule exposes `addressing` (DHCP / Static) plus `ip` / `gateway` / `subnet` / `dns` fields, and they persist — but they are **not applied to the WiFi STA interface**. `wifiStaInit(ssid, password)` takes only credentials; the STA always runs DHCP (there is no `esp_netif_dhcpc_stop` + `esp_netif_set_ip_info` on `staNetif_` — that pattern exists only for the AP). So selecting Static and entering an IP currently does nothing: the device keeps its DHCP lease. The fields are display-only scaffolding ahead of the functionality.

Implementing it needs to answer three UX/safety questions (these *are* the spec):

- **When is it applied?** NOT per-keystroke — editing the fields must only update the stored values, never reconfigure the live interface mid-entry (a valid `ip` with a still-zero `gateway` would otherwise be applied and break routing). Apply on an explicit commit — safest is **on next connect / reboot**, not a live switch, because changing the STA IP drops the very connection the browser UI is talking to.
- **Validation before apply.** Require all of ip/gateway/subnet present and self-consistent; reject `0.0.0.0` gateway/ip. If invalid, stay on DHCP rather than half-apply.
- **Warn before a live change.** If applied live (not reboot-deferred), the UI must confirm ("about to change this device's IP to X — you'll need to reconnect at the new address") and surface the new URL, since the current socket dies the instant the IP changes.

Platform work: extend `wifiStaInit` (or add `wifiStaSetStatic`) to take optional ip/gateway/subnet/dns and call `esp_netif_dhcpc_stop` + `esp_netif_set_ip_info` on `staNetif_` when addressing is Static and the config validates. Needs careful hardware testing — a wrong static config locks the device off-network (recovery is the AP-fallback path or a flash erase). Until landed, consider hiding the Static option so it doesn't read as functional.

### Memory ceiling on non-PSRAM ESP32 with eth-wifi (backlog)

On `esp32-eth-wifi`, default 128×128 grid, free heap at boot is ~28 KB — not enough for `esp_wifi_init` (needs ~16 KB RX buffers) after the light pipeline allocates ~210 KB. The device stays running but WiFi init fails silently.

Fix options in increasing scope:
- **Cap the default grid** — drop to 64×64 on `esp32-eth-wifi` (Layer ~32 KB + LUT ~16 KB = 48 KB, comfortably under). Simplest.
- **PSRAM for Layer buffer + LUT** — ESP32-Gateway has 4 MB PSRAM unused on non-S3 builds. Moving the 49 KB pixel buffer + 64 KB LUT out of DRAM frees ~110 KB for radios. Cost: ~25% FPS hit (PSRAM bandwidth ~12 MB/s vs DRAM ~80 MB/s); needs measurement. See [lessons.md](../history/lessons.md) "Adaptive memory allocation design" for the allocation rules.
- **Lazy WiFi init** — skip `esp_wifi_init` when `ssid_` is empty and no AP-fallback is pending. Helps only when credentials exist but the network is unreachable — niche.

### Boot-time buffer degradation on non-PSRAM at 128×128 (investigation)

On the Olimex (no-PSRAM) at 128×128 with a modifier, the Layer sometimes comes up **degraded** at boot — status `"buffer reduced — not enough memory"`, with a visibly wrong render (the reduced render buffer overflows what the LUT/extrude expects). **Toggling any layout control (forcing a fresh `onBuildState`) fixes it** — the rebuild allocates the full buffer and the display is correct. So this is a boot-time allocation *race*, not a code bug: the same rebuild path that fails at boot succeeds moments later.

Measured: the full pipeline needs two ~49 KB contiguous buffers (Layer render buffer + Drivers output buffer, both 128×128×3), plus the LUT. At boot the largest contiguous block is only ~14–20 KB while the network stack / mDNS / HTTP-server buffers are still settling into the heap — so the Layer can't claim a contiguous 49 KB and degrades (halves its dimensions). A rebuild after the heap settles wins the contiguous block and allocates full. The device is at the fragmentation edge either way (~42 KB free / ~14 KB largest block at 128×128 with the full pipeline up).

The annoyance is purely that the device boots degraded and needs a poke to recover — it should come up working. Fix options in increasing scope:
- **Allocation order** — claim the big Layer/Drivers buffers *before* the network/mDNS/HTTP buffers fragment DRAM (i.e. wire the light pipeline's `onBuildState` ahead of network bring-up in `main.cpp`). Cheapest if the ordering is safe.
- **Boot retry** — if `onBuildState` degrades, schedule one more `buildState()` after boot settles (a one-shot, e.g. after the first `loop1s` or once the network reports up). Self-healing without reordering init.
- **Cap the default grid** on no-PSRAM to a size whose two buffers fit the post-boot largest block (same lever as the eth-wifi memory ceiling above).
- **PSRAM for the buffers** on PSRAM-equipped variants — sidesteps DRAM fragmentation entirely (related: the Async ArtNet and [Memory ceiling](#memory-ceiling-on-non-psram-esp32-with-eth-wifi-backlog) PSRAM notes).

Related: this is the render/output-buffer face of the same non-PSRAM fragmentation cliff the paged `MappingLUT` already addressed for the *LUT*. The buffers themselves still allocate as single contiguous blocks.

### Task core-pinning (backlog)

No FreeRTOS tasks are pinned today. At 16K LEDs the render task takes ~52 ms/tick; if OTA download or Improv scan causes tick-variance spikes, pin render → core 1, OTA/Improv → core 0 (where WiFi already lives via `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`). Defer until contention is observed — neither OTA nor Improv runs during normal operation.

## Architecture

### WiFi runtime disable — open design question (undesigned)

Today the eth-only build profile compiles WiFi out (`MM_NO_WIFI`). Turning WiFi off *at runtime* instead is undesigned: whether the gate should key off detected hardware presence, an explicit control, or a deviceModel-catalog field isn't decided. The eth-only build covers the need until a concrete case forces the choice. (Moved from architecture.md § What we leave undesigned; it's a deferred design decision, not a settled 🚧 one.)

### Consolidate the two module-by-name tree-walkers (backlog)

`HttpServerModule::findModuleByName` (`findInTree` recursion) and `Scheduler::firstByName` (`firstInTree`) are two implementations of the same "find the first module in tree-walk order with this name" operation. The duplication predates the `setControl`-to-Scheduler extraction, but that extraction made `Scheduler::firstByName` public *and* added `Scheduler::instance()`, so HttpServer no longer needs its own copy: its ~9 remaining call sites (identify, addModule, removeModule, clearChildren, the WLED/system shims) can call `scheduler_->firstByName()` and the private `findModuleByName`/`findInTree` pair deletes. Purely a *No duplication* cleanup — no behaviour change — worth doing so the walk order/semantics live in exactly one place. (Reviewer note, IrService/setControl branch.)


### Pin-uniqueness check across modules (prevents conflicts; replaces a singleton hack)

**Problem it solves.** Two modules must not drive the same physical GPIO. Today nothing stops it: add two `RmtLedDriver`s with `pins="18"`, or two `AudioService`s with the same `wsPin/sdPin/sckPin`, and they fight over the pin — at best garbage output, at worst (for I2S) endless `i2s_new_channel` driver-error spam every tick. This surfaced when a repeated catalog inject stacked duplicate AudioModules and the device spammed I2S failures (a clean install is fine; the duplicates were the artifact).

**Why pin-uniqueness, not a per-type singleton.** The first instinct was "make AudioService single-instance" — but that's a crude proxy. The *real* invariant is pin non-overlap: a board legitimately can have **two LED drivers on different GPIOs** (multi-output rigs do exactly this), or even two mics on distinct pin sets. "One mic" isn't fundamentally true; "no two modules on the same pin" is. So check pin conflicts, which both prevents the breakage **and** allows legitimate multi-instance setups. (A per-type singleInstance flag was prototyped and rejected in favour of this.)

**The clean mechanism — reuse `ControlType::Pin`.** Pins are already their own control type (the `addPin` work). So the check is domain-neutral and needs no per-module declaration: enumerate every `Pin`-typed control's value across the whole tree; a value of `-1` is "unused" (ignored); any other value appearing on two controls is a conflict. Handle the list case: `RmtLedDriver.pins` is a comma list (`"18,19,20"`), so the enumerator expands list-of-pins controls too.

**Where it runs.** Two sites, because a pin can be introduced at add *or* edit:
- `POST /api/modules` (add): if the new module's catalog/default pins collide, reject.
- `POST /api/control` (pin write): if setting a `Pin` control to a value already used elsewhere, reject (or soft-flag — see below).

**Open decision (UX).** Conflict on add → reject with a clear message (`"GPIO 18 already used by RmtLed"`). Conflict on a live pin edit → reject is safest but blocks mid-reassignment (you can't swap two drivers' pins without a free intermediate); a **soft-flag** (accept, set a status warning) is friendlier for live editing. Leaning: reject on add, soft-flag on live edit. Product-owner call.

**Hardware-limit tail (not covered by the pin check).** Pin-uniqueness rejects the common case but not the controller-count limit: the S3 has **2 I2S controllers** regardless of pins, so a 3rd mic on distinct pins passes the pin check yet fails `i2s_new_channel` at runtime. That tail is already handled — the platform I2S init returns false on failure (no panic, module stays `inited_=false`); verified live (4 pinned AudioModules → error spam, no crash). So scope = pin-uniqueness check + the existing graceful-degrade; don't try to make the pin check also model controller counts.

**Related:** [§ Disabling a module should release its resources](#disabling-a-module-should-release-its-resources-not-just-stop-its-loop-backlog) — a disabled module freeing its pins is what lets the same GPIO be reassigned live without a conflict-reject.

### PinsModule — strict reject-on-add mode (the one remaining increment)

[PinsModule](../moonmodules/core/system.md) is shipped: the read-only ownership map, reserved/strap severity grading, the conflict **soft-flag** (a GPIO claimed by two controls shows both owners, flagged red, never rejected), live-state dir/level/drive, and the disable-frees-pins cascade all landed — see [pins-analysis-top-down.md](pins-analysis-top-down.md) for the shipped design record. The soft-flag choice obviated the reassignment broker (a live pin swap already works: set A→B's pin, then B→A's, the transient conflict clears).

The one still-open item is an **optional strict reject-on-add mode** for the installer/catalog path: a "clean tree" that *refuses* a module add whose pins collide, rather than accepting it soft-flagged. This is add-path *validation* (a different feature than the live soft-flag reassignment), wanted only if a stricter installer UX is desired. Spec + `/plan` if picked; the soft-flag default stays for live editing.

### Runtime board presets (multi-commit, partially landed)

The firmware-vs-board separation is now in place across the codebase (see [architecture.md § Firmware vs deviceModel vs board](../architecture.md#firmware-vs-devicemodel-vs-board)). `build_esp32.py --firmware <variant>` picks the compiled binary; MoonDeck deduces the physical board where the firmware uniquely identifies hardware (`esp32-eth*` ⇒ `olimex-esp32-gateway-rev-g`) and lets the user pick from a short hardcoded list otherwise. Firmware variants stay separate — `esp32-eth` saves ~670 KB flash + ~30 KB DRAM vs the default `esp32` (WiFi+Ethernet, measured); merging would erase that win.

What still needs separation: the eth variants hardcode Olimex Gateway RMII pins in `src/platform/esp32/platform_esp32.cpp::ethInit()`, so they only work on that one PCB. As we add boards with different pins (LOLIN D32 tested 2026-06-02, QuinLED variants planned), runtime pin configuration becomes the next step.

Pin config moves to runtime (next, separate commit):
- Drop hardcoded `GPIO_NUM_17` from `ethInit()`. NetworkModule reads `Network.eth_rmii_clock_gpio` (new control) and similar pin values, defaulting to current Olimex hardcodes so behaviour is unchanged.
- Same for any other hardware-pin literal in the firmware.

Board preset catalog + upload (later, when the runtime config has real consumers):
- Add structured per-board files (location TBD — not `docs/` since they're config not docs; `boards/` at repo root is the strong candidate, matches the PlatformIO convention contributors will recognise).
- Each file declares chip, flash, PSRAM, Ethernet PHY + pins, default module config.
- New `/api/board-preset` endpoint accepts the JSON; device persists to LittleFS; bootstrap applies pins + defaults on next boot.
- MoonDeck "Set board" picker reads the catalog to populate the dropdown.
- Pin reassignment requires reboot (ESP-IDF can't hot-reconfigure EMAC pins after `esp_eth_driver_install`); document the constraint.
- A first attempt at this catalog landed and was rolled back during the firmware-vs-board separation work — the catalog only earns its keep once the device reads it, otherwise it's a docs-shaped file in the wrong place.

**Prior art — MoonLight's per-board pin database** ([ModuleIO.h](https://github.com/ewowi/MoonLight/blob/main/src/MoonBase/Modules/ModuleIO.h)). MoonLight (our own project) already models exactly this for ~25 boards across ESP32-D0 / S3 / P4: a `pins[]` array of `{GPIO, usage, index}` plus board-level `maxPower`, `ethernetType`, `ethPhyAddr`, `ethClkMode`. Don't copy the file or paste its tables here — read it when building the catalog and write our own. Its `usage` enum enumerates the hardware functionalities a projectMM board preset *could* drive once the device-side consumers exist (each needs its own module/control before the corresponding `deviceModels.json` / catalog field earns its keep — none exist today beyond `System.deviceModel` + `Network.txPowerSetting`):

- **LED output pins** — per-strip data GPIOs (1–16 outputs/board); the first real consumer (a Driver pin control) unblocks multi-output boards (QuinLED Dig-Quad/Octa, SE16, LightCrafter).
- **Ethernet PHY config** — LAN8720/RMII (MDC/MDIO/CLK/power-pin/PHY-addr/clock-mode) vs W5500/SPI (MISO/MOSI/SCK/CS/IRQ); the consumer is the runtime `Network.eth_*` controls listed above, replacing the hardcoded Olimex pins.
- **Power budget** — `maxPower` (Watts) per board, for a future current-limit / brightness-cap control.
- **Audio / I2S** — SD/WS/SCK/MCLK pins, the input side of audio-reactive effects (Pi-5 sensor note is the desktop counterpart).
- **Buttons & inputs** — push/toggle/lights-on, PIR, digital-input; needs an input-event concept the firmware doesn't have yet.
- **Relays & power control** — relay / lights-on / high-low pins.
- **Infrared** — IR receive pin (remote control).
- **RS485 / DMX** — TX/RX/DE pins (wired DMX-512 output beyond the current Art-Net path). The driver that consumes this is the [RS-485 / DMX-512 wired output item](backlog-light.md#rs-485--dmx-512-wired-output-future--the-physical-dmx-driver) in the light backlog.
- **Sensing** — voltage / current / battery / temperature ADC pins.
- **Onboard LED / key, exposed / reserved pins** — board-housekeeping and conflict-avoidance metadata.

Sequencing rule (unchanged): each functionality lands a device-side control first, then its preset field; the catalog grows one earned consumer at a time, never as a speculative pin dump.

**Module variant + PSRAM within the classic-ESP32 family.** `getChipDescription()` and MoonLight's `ModuleIO.h` both report only the *core* family ("ESP32"), not the *module* (WROOM / WROVER / PICO) — so neither distinguishes whether a classic-ESP32 board has PSRAM. This matters for projectMM (whose large-LED story leans on PSRAM) in a way it doesn't for MoonLight: e.g. the **QuinLED Dig-Next-2 is built on an ESP32-PICO with 2 MB PSRAM**, but projectMM's `esp32` build has no `CONFIG_SPIRAM` (see the `#ifdef CONFIG_SPIRAM` gate in `platform_esp32.cpp::psramAlloc`), so it flashes and runs as a no-PSRAM device and hits the non-PSRAM fragmentation ceiling at large grids that the 2 MB would otherwise relieve. A PSRAM-enabled classic-ESP32 firmware variant (e.g. `esp32-psram`) would unlock it; `deviceModels.json` could then carry a `psram` hint per board to steer the picker — but only once that variant exists (no consumer today). `deviceModels.json` currently maps every classic board to the WiFi-only `esp32` variant, which is correct-but-unoptimised for PSRAM-bearing PICO boards.

### Per-layout coordinate offset for independent placement (backlog)

`Layouts` stitches multiple child layouts into one physical light space, but only their *indices* are stitched (offset sequentially in `forEachCoord`) — their *coordinates* are not translated. Two layouts therefore overlap in the same coordinate box: two 64×64 grids both occupy x,y ∈ 0..63, so the Layer's dense bounding-box buffer is 64×64 (4096 voxels) even though the container reports 8192 lights, and the second layout's lights land on the first's positions. `scenario_Layouts_mutation` documents this (its steps assert pipeline liveness, not buffer-size arithmetic).

When picked up: add `offsetX/Y/Z` (lengthType) controls to `LayoutBase`; `Layouts::forEachCoord` translates each child's emitted coords by its offset so layouts occupy disjoint regions of the physical extent (a 64-wide grid at offsetX=64 sits beside another at offsetX=0 → a 128×64 combined extent). `Layer::onBuildState` already derives physical dims from the max emitted coordinate, so it would pick up the wider extent automatically. Until then, "multiple layouts" means "multiple layouts sharing a coordinate box", which is only useful when they genuinely overlap (e.g. a sphere inscribed in a grid).

### Improv as a child of NetworkModule (deferred — needs scheduler work first)

Architecturally the right shape; attempted in plan-21, reverted. Blocker: `Scheduler::tick()` only walks top-level modules for `loop20ms`/`loop1s` — children silently miss those callbacks. See [lessons.md](../history/lessons.md) "Trying to add a child module to NetworkModule".

Minimum-scope fix before the move:
1. `MoonModule::loop20ms`/`loop1s` propagate to children (or Scheduler walks them) — pick whichever costs less at runtime.
2. Audit every existing override of `setup`/`loop1s`/`loop20ms`/`onBuildControls`/`teardown` in NetworkModule, SystemModule, FilesystemModule, HttpServerModule to confirm base-chaining.
3. Then the actual move is a one-line `networkModule->addChild(improvModule)` swap. Estimate ~2 h total.

### Platform API: `std::span` migration (backlog)

Several `platform.h` APIs still use `(buf, len)` pairs where `std::span` would catch length/pointer mismatches at compile time. Concrete sites: `http_fetch_to_ota`, `improvProvisioningInit`, and friends. ~2 h including ripple updates to callers. Do alongside the next platform-API expansion (Windows socket port or POST /api/firmware streaming).

### Improv-as-REST follow-ups

Device-model injection over Improv shipped as **"Improv = REST over serial"** (the `APPLY_OP` vendor RPC pushes the whole `deviceModels.json` entry over serial during install; the device runs the same apply-core the HTTP REST API does, on WiFi *and* eth-only firmware). That subsumed the earlier multi-step "board injection + Improv as a general data injector" plan — the general injector *is* APPLY_OP. What remains:

**Open follow-up: closed-loop APPLY_OP pacing (read-back ack + retry).** The installer paces APPLY_OP frames open-loop (`sendApplyOpFrame` waits a fixed ~120 ms between ops) rather than reading the device's ack back, because a Web Serial duplex read while the writer lock is held is awkward. The delay covers the worst-case single-buffer consume window with headroom, and each op is idempotent (a lost op re-applies cleanly on a re-flash), so this is robust today. The closed-loop upgrade — read the RPC response, retry once on error `0x82` (buffer busy) — removes the fixed delay (faster install) and makes op-loss impossible rather than improbable. Worth doing if a real install is ever observed dropping an op, or when the config push grows large enough that the cumulative fixed delay is noticeable. Touches only `install-orchestrator.js`.

**Open follow-up: shared JS helpers across device-UI and web-installer.** `safeLocalGet` / `safeLocalSet` (3-line hostile-storage guards) are duplicated in `src/ui/install-picker.js` (device firmware, embedded as a C string via `embed_ui.cmake`) and `docs/install/devices.js` (web installer page, served from Pages). The two live in different build contexts so the shared extract isn't trivial — it'd need a new `src/ui/safe-storage.js` plus updates to: `embed_ui.cmake` (embed the new file), `ui_embedded.h` generator (new C array), HTTP server file routing (new path served), `release.yml` workflow staging, `preview_installer.py` staging. Five files for one 3-line helper is too much pre-merge. Worth doing when the next shared helper arrives — `relativeTime` and `formatBytes` are candidates. Two helpers earn the build-glue cost; one doesn't.

### Live scripting — author effects/layouts/modifiers/drivers/sensor logic on-device (multi-commit, design phase)

Run user-authored scripts on a running device — a scripted effect, layout, modifier, driver, or core sensor rule, pushed as text and live on the next tick with no reflash/reboot — the leap WLED took with ARTI-FX and the heart of the PixelBlaze product. A scripted module **is** a MoonModule (controls, `loop()`, role, generic UI). The engine lives in core (domain-neutral: also "transform sensor data") and serves the light domain specifically. Targets in order: ESP32 classic + S3 first, then P4/other ESP32, then Teensy, then desktop. Must be blazingly fast (runs in the render hot path at 16K+ lights × 50 FPS), memory-smart (IRAM/PSRAM via `platform::alloc`, compile-once), and synced (Scheduler tick, tick-atomic hot-swap, live reconfig).

The **bottom-up landscape survey** is done — [livescripts-analysis-bottom-up.md](livescripts-analysis-bottom-up.md): deep-reads the [ESPLiveScript fork](https://github.com/ewowi/ESPLiveScript/tree/fix-warnings) (a from-scratch C-like JIT that emits **native Xtensa** machine code — blazingly fast but **Xtensa-only**, so it covers classic+S3 and *not* P4/Teensy/desktop), surveys the field (PixelBlaze bytecode VM + web editor, WLED ARTI-FX AST-walking interpreter, embedded VMs / WASM / lightweight multi-ISA JITs), and extracts the load-bearing decisions (execution strategy, the IR seam ESPLiveScript lacks, the MoonModule binding, the per-pixel contract, memory placement, sync, sandboxing). Its thesis to validate: a **portable bytecode-VM baseline that runs on every target on day one + an optional native back-end for the hot ISAs behind a shared IR**. **Next: the top-down redesign** — the prompt that generates `livescripts-analysis-top-down.md` is at the bottom of the bottom-up doc; it produces the reference architecture + staged spike plan. Implementation is multi-commit, spike-ordered, after the top-down lands. Credits: [history/hpwit-ESPLiveScript.md](../history/hpwit-ESPLiveScript.md).

## HTTP and OTA

### HTTP file serving blocks the render tick (backlog)

`HttpServerModule::handleConnection()` serves large embedded files (`app.js`, `style.css`) with the blocking `TcpConnection::write` — a page load can briefly stall `loop20ms`. One-shot per load (lower priority than the per-tick preview issue, which is fixed). Fix: serve large HTTP responses with `writeChunks` (the same non-blocking path used for preview frames).

### Generic control + state topics over MQTT — the automation escape hatch (backlog)

The MQTT bridge ships a **semantic** topic surface (`<prefix>/on/set`, `brightness/set`, `hsv/set` → the `Drivers` controls) sized for Homebridge/HomeKit, which need fixed, typed topics per accessory. That leaves everything else the web UI can reach — effect choice, layout, modifier params, palette-by-index — unreachable from a home hub. The ask that surfaced this: *"expose the whole REST API over MQTT."*

**It's compatible with Homebridge/HA, because it's a second topic namespace, not a replacement.** Homebridge only ever subscribes to the semantic topics it's configured for; a generic topic family sits beside them and a broker carries both. The two surfaces already share one apply-core — `Scheduler::setControl(module, control, json)`, the same seam `/api/control` and IR funnel through — so a generic MQTT surface is that core under a different topic shape, honouring the module's own "MQTT is a transport, not new control logic" contract.

But "the whole REST API" over-scopes: of the routes (`/api/control`, `/api/state`, `/api/system`, `/api/modules[/…]`, `/api/dir`, `/api/file`, `/api/firmware/*`, `/api/reboot`, `/api/types`, the `/json/*` shim, `/ws`), only two are genuinely pub/sub-shaped. The rest are request/response or bulk transfer and belong on REST:

- **`<prefix>/api/control/set`** ← `{"module","control","value"}` (the exact JSON `/api/control` takes) → `applySetControl`. The real win: any control on any module — the effect/layout/modifier/palette reach HomeKit can't express. The apply-core already validates (range / read-only / module-not-found via `SetControlResult`), so safety is free.
- **`<prefix>/api/state/get`** → publishes the `/api/state` JSON (retained). For dashboards (fps / heap / tick / current effect across a fleet), offline detection, and alerting — the monitoring half.

Explicitly **out** (no practical MQTT case, and wrong for the transport):
- **`/api/file`, `/api/dir`, `/api/firmware/upload`** — kilobytes-to-megabytes of binary; MQTT is a small-message bus, REST already streams these correctly.
- **`/api/modules` add/delete/move/replace** — stateful, order-sensitive pipeline surgery a human does once in the UI; an automation reshaping the pipeline is an anti-pattern (the *Robust to any input* guarantee makes it *safe*, not *advisable*).
- **`/api/reboot`** — one narrow action; a single fleet-reboot topic is a mild nice-to-have, not "the API."
- **`/api/types`, `/json/*`, `/`, `/ws`, `/api/firmware/url`** — discovery metadata, the WLED shim, the UI itself, the socket: static, already-served-elsewhere, or a different protocol.

**Shape:** ~15 lines routing into the existing `applySetControl` + a state publish; gate behind a `generic API` bool defaulting **off**, so a home user's broker isn't a remote-config backdoor and the curated HomeKit surface stays the clean default. **Don't** auto-generate a semantic topic per control — HomeKit/HA need stable typed topics; a generic `palette/set` whose meaning shifts per firmware breaks their discovery. Keep semantic topics hand-curated; let the generic pair be the escape hatch.

**HA MQTT Discovery — SHIPPED.** The device announces a retained JSON-schema light config to
`homeassistant/light/<id>/config` (the Tasmota/ESPHome/Zigbee2MQTT pattern), so HA auto-creates a
wired entity — gated on the `haDiscovery` control, with a Last-Will availability topic. This covered
the "HA can't reach the device over MQTT" incidents. The *generic-topics escape hatch* above (the
whole REST API over MQTT, for power-user scripting) is the remaining unbuilt piece; keep it coherent
with the [DevicesModule command half](#devicesmodule-interop-plugins-the-command-half-discovery-shipped)
(Tasmota-MQTT / zigbee2mqtt as *outbound* control — the mirror of this *inbound* surface) and the
[LightsControl integration point](backlog-mixed.md).

**Open question — Homebridge example maps both `setBrightness` and `setHSV`.** In `homebridge-mqttthing`'s `lightbulb`, HSV's *value* (V) already carries HomeKit's Brightness characteristic, so mapping both topic pairs may double-drive brightness (mqttthing's docs lean toward using one or the other). The [MQTT § Homebridge example](../moonmodules/core/system.md#mqtt) currently lists both, to show the device's full topic surface. Resolve on hardware: flash a board, run Homebridge + mqttthing with that config, and check whether the Home-app brightness slider misbehaves — if it does, drop the two `brightness` topics from the example; if not, it's a non-issue. (Flagged by CodeRabbit; parked here rather than changing the doc on an untested hunch.)

### HA update entity via MQTT discovery — release check (open follow-up)

**Discovery config + install command wiring — shipped.** [`MqttModule`](../../src/core/MqttModule.cpp) now publishes a second HA-discovery component at `homeassistant/update/projectMM_<mac6>/config` (same haDiscovery gate, same MAC-stable id, same device card as the light), state on `<prefix>/update/state`, and subscribes to `<prefix>/update/set`. HA renders a *"Firmware: <version>"* card in the diagnostic section of the device panel. An install command routes to `platform::http_fetch_to_ota` with the release-artifact URL built from the payload version + `kFirmwareName` (`https://github.com/MoonModules/projectMM/releases/download/v<version>/firmware-<key>-v<version>.bin`), reusing the same OTA task the `/api/firmware/url` route drives.

**Still to build — device-side release check.** `installed_version` and `latest_version` are equal today (both `MM_VERSION`), so HA renders the entity as up-to-date and the Install button is disabled. What's missing is a periodic poll of `https://api.github.com/repos/MoonModules/projectMM/releases/latest`: at boot (~30 s post-network-up) and every 24 h thereafter, fetch the release JSON, extract `tag_name`, and if newer than `MM_VERSION` call `MqttModule::publishUpdateState()` with the updated `latest_version` (the publish path is already there — only the caller is missing). ~2 KB per check.

Two blockers, both platform-layer:
- **`platform::http_fetch_to_ota` is OTA-shaped** — it writes bytes into the next OTA partition; there's no general "HTTPS GET into a buffer" seam. A small `platform::http_fetch_to_buffer(url, buf, cap, statusBuf, size_out)` next to it — ~30 lines wrapping `esp_http_client` on ESP32, a libcurl / URLSession stub on desktop — is the right home for this.
- **JSON parsing on-device** — the existing `mm::json` scalar helpers are flat and key-order-independent; a `releases/latest` payload uses a nested `assets[]` array which the current helpers don't walk. Either extend them (~30 lines to walk one level of array), or scan for the `tag_name` string directly with `strstr` for this one caller.

Once both land, add a `ReleaseCheckModule` (or a small extension inside NetworkModule / FirmwareUpdateModule — decide when building) that owns the poll timer + last-seen version cache and calls into `MqttModule::publishUpdateState()`. ~50 additional lines.

## Testing

### Additional test coverage (pending)

- **Memory degradation cascade** — the output-buffer *allocation* decision (no buffer for a lone identity layer; a buffer for ≥2 layers or any LUT layer) is unit-pinned (`unit_Layers_container` "Drivers allocates the output buffer only when…"), and LUT-vs-identity is pinned by `unit_Layer_sparse_mapping`. What's **not** pinned is the *low-heap* half of [architecture.md § Degradation cascade](../architecture.md#degradation-cascade): under heap pressure the LUT + driver buffer are skipped *together* (`lutSkipped()` true, forced 1:1), and below that the layer buffer *reduces dimensions* (halving to a 8×8 floor) rather than failing. The hook exists — `unit_BlendMap` already uses `platform::setTestMaxAllocBlock` to force allocation failure for the paging test — so a test could cap the block size and assert: (1) LUT+output buffer both skip and `lutSkipped()` flips, (2) the layer buffer shrinks to fit and never goes null. Pre-existing gap (predates multi-layer); the *happy-path* allocation contract is covered, only the OOM-degrade branch isn't.
- **UI page load time** — scenario step measuring HTTP response time for `/`, `/api/state`, `/api/system` via the live runner. Verifies acceptable load time on ESP32.
- **Module teardown memory** — scenario that tears down all modules and verifies heap returns to pre-setup baseline. Confirms no lifecycle leaks.
- **JavaScript test harness** — `vitest` + `jsdom` for the browser UI: pure helpers in `install-picker.js` (`isCompatible`, `parseFirmwaresFromAssets`, `relativeTime`) **and `app.js`'s conditional-control DOM logic** (`syncVisibleControls` — reconciles which control rows are rendered when a `hidden` flag flips). The C++/backend half of conditional controls IS unit-tested (`conditional_controls.h` + per-module tests pin the binding + `hidden` flag), but the **UI re-render half is not** — `syncVisibleControls` was the source of a real re-render-loop freeze (Network static-IP toggle) caught only on hardware. A `jsdom` test that builds a card, flips a control's `hidden`, runs the reconcile, and asserts the right rows appear/disappear (and that it converges — the unchanged→no-op fast path) would have caught it. **Attempted and reverted (2026-06-17):** stood up vitest + 13 passing tests for the install-picker pure helpers, but the high-value half (`syncVisibleControls`) needs either an `app.js` module seam or extracting its reconcile logic into a separate served `.js` (6 embed/route wiring edits for a firmware-served file). Judged not worth adding a whole Node/npm toolchain to a C++/Python repo to test ~3 small pure functions; the toolchain earns its place only once the `syncVisibleControls` DOM test (and a real body of JS logic) lands with it. **Do it as its own focused branch**, deciding the app.js seam first (it's already `type="module"`, so extracting `reconcileControlRows` into a served file — wired through `embed_ui.cmake` + the two HttpServerModule routes like the other UI .js — is the clean shape). Pure-helper `_test` exports + the reconcile extraction are the two pieces; both were prototyped in that reverted attempt.
- **Browser-level Improv automation** (deferred) — `moondeck/build/improv_smoke_test.py` (added 2026-06-03) exercises the device-side Improv listener over plain serial; what's missing is the browser-side equivalent — Playwright driving Chrome's Web Serial, clicking through ESP Web Tools' install modal, filling the WiFi creds form, asserting `PROVISIONED`. Catches "ESP Web Tools changed its Improv handling in a way that broke our manifest format" failures the serial-only smoke test can't see. Hard to set up reliably (headless Chrome with Web Serial is finicky, needs a wired ESP32 in CI). Pick this up if a regression in the browser flow ever escapes the manual dev-environment test (preview_installer flash-ready mode at <http://localhost:8000/>).

### Live full-suite run leaks state between scenarios (test infra)

`run_live_scenario.py --module all` runs scenarios in sequence against one device, and they share the live tree. Two scenario styles don't compose:
- **Canvas-preparing scenarios** (`scenario_modifier_swap`, `scenario_perf_light`, `scenario_perf_full`) `clear_children` the containers and rebuild, then their cleanup leaves the tree **bare**.
- **Canonical-tree-assuming scenarios** (`scenario_GridLayout_resize`, `scenario_MoonModule_control_change`, `scenario_NetworkModule_mdns_toggle`) are `mutate` scenarios that expect the boot tree (Grid / Noise / Multiply) to already exist and only tweak it.

Run a bare-leaving scenario before a tree-assuming one and the latter fails pre-flight ("references ids neither on the device nor added by an earlier step"). Each passes **in-process** (fresh tree per scenario — the authoritative gate) and **live individually** (after a clean boot); only the chained live run trips. Not a product bug — a consequence of the "scenarios own their state, no restore" model the canvas-preparing scenarios follow, which the older ones predate.

Fix options: (a) make every live mutate scenario clear+rebuild its own canvas (consistent with the newer ones) so order never matters; or (b) have the live runner reboot / restore the canonical tree between scenarios. (a) is the cleaner long-term shape. Until then, the in-process suite is the gate; live full-suite runs need a clean boot per scenario, or run scenarios individually.

## Housekeeping

### Socket-pair fixture for HttpServerModule WS-send tests (test infra)

`HttpServerModule`'s resumable preview send (`sendBufferedFrame` / `drainPreviewSend` / `cancelBufferedSend`, the newest-wins drop, the per-client cursor over `[hdr ++ body]`, the memory-adaptive chunk) has no direct unit test because driving it needs real `TcpConnection` clients whose `writeSome` returns partial / WouldBlock under control — and there's no socket-pair test fixture today. The send *contract* is covered indirectly: `unit_PreviewDriver` drives a `CaptureBroadcaster` mock for route-to-buffered / gate-on-idle / cancel-on-rebuild, and the live device sweep exercises the real drain across ticks. A loopback `socketpair()` fixture on the desktop platform (a `TcpConnection` pair where the test reads the bytes the server pushed, and can simulate a stalled receiver by not draining) would let the drain/drop/cancel/over-push paths be pinned host-side. Build it when the next core transport change lands (it'd also serve future WS tests).

### ESP-IDF version pinning (pending)

The build IDF is `v6.1-dev-399-gd1b91b79b5`, a dev-branch snapshot (2025-11-05) ahead of the v6.0 stable but on the unreleased v6.1 line. The version facts (what v6.0 vs v6.1 changed, the release schedule, the 30-month support policy, how to check for a newer tag) live in [building.md § ESP-IDF version](../building.md#esp-idf-version); this entry tracks only the **open decisions** the doc doesn't make. Being on a dev branch already cost us once — the missing `ESP_ROM_ELF_DIR` in the post-build gdbinit step (fixed in `build_esp32.py`). **Partly landed:** `setup_esp_idf.py` carries `PINNED_IDF_COMMIT`/`PINNED_IDF_VERSION` and **warns on drift** (installed HEAD vs pinned) — it can't `checkout` for you (it doesn't own the clone), but a silent `git pull` or a stray shallow clone is now visible. **Still to do:** (a) a MoonDeck UI banner / status dot surfacing the same drift (the CLI warning only shows during Setup), and (b) the migrate-or-stay call — stay on the pinned commit (chosen for now: it's what all targets incl. P4 were validated against), or move to `v6.1` stable (skipping v6.0, since v6.1 is close); migration is a full re-validation pass across classic/S3/P4, a deliberate task, not a pull. Until then: don't `git pull` the IDF. **Schedule note:** the v6.1-stable target of 2026-07-31 is unlikely to hold — v6.0 slipped ~1 month (planned 2026-02-27, shipped late March), and Espressif minors historically slip 2-6 weeks on the *final* even when betas land on time. So migrate **to the event** (v6.1 stable actually tagging on the releases page), not to the calendar date. `v6.0` stable is the lower-risk fallback if the dev-branch warts (`ESP_ROM_ELF_DIR`, API-churn risk) get worse before v6.1 lands.

### Three-level device model: MCU → Board → Device (config provenance)

The model itself is now a shipped design — see architecture.md § Config provenance (the three levels + the `txPowerSetting` example + "default only at the level that fixes it"). The catalog that carries it is `web-installer/deviceModels.json` (schema). The remaining forward-looking pieces — a `devices.json`/MCU-layer split and annotated-pin images — stay gated by the sequencing rule (no catalog field ahead of a consumer).

### Persistence overlay: partial-save / schema-change audit (backlog)

The absent-key fix (`json::hasKey` guard in `applyControlValue`, so a saved file omitting a key no longer zeroes the control's default — the P4 `ethType` no-DHCP root cause) closed the acute hole. A broader audit would harden the overlay against the rest of the schema-drift surface, now that controls carry meaningful non-zero defaults:
- **Type-change migration.** `ethType` changed `int16` → `uint8` (Select) this branch. A persisted file written under the old type still loads (flat parser reads the number either way), but confirm every type transition is safe — especially Text/Password/IPv4 buffer-size changes and a numeric control narrowing its range (Clamp handles range, but a renamed control silently drops its old value with no warning, unlike `migrateRenamedConfigs` for whole files).
- **Per-control rename path.** `FilesystemModule::migrateRenamedConfigs` only renames whole *files* (by type). A *control* rename within a module has no migration — the old key is silently ignored (now preserved-as-default rather than zeroed, which is better, but the user's saved value is still lost). Decide whether control-level renames need a migration map or are rare enough to accept the loss with a logged warning.
- **Test coverage.** `unit_Control_apply_absent_key` pins the absent-key contract; extend with a type-change round-trip (save int16, load into uint8 Select) and a narrowed-range clamp case so a future schema change can't regress silently.
This is hardening, not a known bug — the shipped fix is correct for the cases that occur today.

### ESP32-P4 support — rounds 3-4 (in progress)

Rounds 1 (board + Ethernet-only) and 2 (Parlio LED driver) have landed. Remaining rounds, each its own plan + commit:
- **Round 3 — WiFi via the C6 co-processor. PARTIALLY PROVEN — C6 link up, STA failover not yet working.** The P4 has no native radio (`SOC_WIFI_SUPPORTED` absent); WiFi comes from the on-board ESP32-C6 over SDIO via `esp_wifi_remote` / esp_hosted. Landed as the `esp32p4-eth-wifi` firmware variant: components pulled P4-only (`rules:` gate in `idf_component.yml`), and `ensureWifiInit()` adds an `esp_hosted_init` + `connect_to_slave` prelude before `esp_wifi_init` (gated on `platform::usesRemoteWifi`). The rest of the WiFi seam is unchanged because `esp_wifi_remote` is API-compatible. A deliberate, documented [v6.0-floor exception](../building.md#esp-idf-version); C6 config via `CONFIG_SLAVE_IDF_TARGET_ESP32C6` + `CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6` + the `CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD` SDIO-pin preset.

  **Hardware results (bench, P4-NANO, 2026-06-12):**
  - ✅ **esp_hosted / C6 SDIO comes up at boot.** `host_init: ESP Hosted`, `H_API: ESP-Hosted starting`, `add_esp_wifi_remote_channels`, `H_SDIO_DRV: sdio_data_to_rx_buf_task started`. No NVS error / assert / panic / hang. Device boots fully (~57-60 FPS), `hasWiFi` true, WiFi controls present. esp_hosted **self-initialises at boot via a constructor** (`ESP_SYSTEM_INIT_FN` → `esp_hosted_init`), so no bring-up code is needed in our platform layer — an earlier explicit `esp_hosted_init` + `esp_hosted_connect_to_slave` prelude was *removed*: init was a redundant no-op and `connect_to_slave` is actually a transport *reconfigure* (slave GPIO-54 reset + SDIO re-init). SDIO config confirmed correct on the wire: `CLK[18] CMD[19] D0[14] D1[15] D2[16] D3[17] Slave_Reset[54]`, 4-bit 40 MHz.
  - ❌ **WiFi STA connect fails on the SDIO re-init.** The cascade DOES fire correctly (`Ethernet no link, cascading` → STA path), but `esp_wifi_init()` (forwarded to esp_wifi_remote) internally triggers `esp_hosted_reconfigure` → `Reset slave using GPIO[54]` → **`sdmmc_card_init failed` (×15) → `card init failed` → `esp_wifi_init failed: ESP_FAIL`**. So: the boot-time SDIO init succeeds, but a **runtime slave reset can't re-establish the SDIO link**. The C6 doesn't come back after the GPIO-54 reset during operation. This is an esp_hosted/SDIO/C6-slave-firmware level issue (reset timing or slave image), below our application code — the pins and Kconfig are correct.

  **Open issues before this is done:**
  1. **Runtime SDIO re-init of the C6 fails — CONFIRMED a C6 slave-firmware problem (not a guess).** SystemModule now exposes a `wifiCoproc` read-only control (via `platform::coprocessorWifi()` → `esp_hosted_get_coprocessor_fwversion()`), and on the bench it reads **`not detected`** — the C6 returns no valid firmware version (0.0.0 / handshake never completes), which is exactly the documented signature of absent / incompatible C6 slave firmware. So this is proven off the device, not inferred. Likely a version mismatch on top of that: The host pulled esp_hosted **2.12.9**; Espressif's P4-Function-EV-Board ships its C6 pre-flashed with esp_hosted slave **v0.0.6**, and the **Waveshare NANO is a different board that may carry a different / absent C6 slave image**. The symptom fits: boot inits the host SDIO master fine, but resetting the C6 (GPIO 54) and re-enumerating it as a slave fails (`sdmmc_card_init failed`) because the C6 has no compatible slave firmware responding. **Primary next step: build + flash the version-matched esp_hosted slave firmware onto the NANO's C6.** The slave project is already vendored at `esp32/managed_components/espressif__esp_hosted/slave/` (`sdkconfig.defaults.esp32c6`, `partitions.esp32c6.csv`); `idf.py create-project-from-example "espressif/esp_hosted:slave"` → `set-target esp32c6` → flash. **Caveat / needs PO + bench hardware:** flashing the C6 on the EV board uses an **ESP-Prog wired to the `PROG_C6` header** with the P4 held in bootloader mode (esp_hosted `docs/esp32_p4_function_ev_board.md` §5.2) — the NANO's C6-flash path must be confirmed (separate USB? equivalent header? ESP-Prog?), and an ESP-Prog may be needed. An OTA slave-update path exists but needs a *working* link first (chicken-and-egg here). This is a hardware-provisioning task, not application code. Secondary fallbacks if firmware-match doesn't fix it: an esp_hosted option to skip the reconfigure/slave-reset when the transport is already up at boot; a slower SDIO freq or 1-bit mode; verify GPIO 54 reset polarity/timing for the NANO. **(Note: EIM — the building.md v6.0-adoption item — does NOT help here; it's a host-machine installer, unrelated to device-side C6 firmware.)**

  **User lead (2026-07-09) — avoid the WiFi teardown/re-init on hosted targets entirely; it may sidestep the slave-reset failure.** A user hit the mirror symptom on a *different* codebase (ESP32-Sveltekit / WLED-MM, not projectMM — its `lib/framework/WiFiSettingsService.cpp` doesn't exist here): after a clean C6-flash the P4-NANO WiFi worked, but their app's boot-time `WiFi.disconnect(true)` fully tore down the WiFi stack, and the later AP bring-up then failed with `esp_hosted_transport_config: Transport already initialized` / `esp_hosted_init failed!` / `AP enable failed!`. Their working fix: on `CONFIG_ESP_WIFI_REMOTE_ENABLED` (= hosted) targets, **don't do a full stack reset** — keep STA enabled, reconnect *without* a full teardown/re-init, and never call `WiFi.disconnect(true)` in the disconnect callback. **Why this is relevant to us even though the file differs:** our own comment at `platform_esp32.cpp:796` already documents that the esp_hosted transport is set up **once at boot** and is fragile to re-init (`connect_to_slave` = a transport reconfigure that resets the slave + re-inits SDIO and fails on a live link). Our disconnect *callback* is already safe (`wifiEventHandler` on `STA_DISCONNECTED` only sets a flag — no teardown), BUT our **failover path is not**: `wifiStaStop()` (`platform_esp32.cpp:915`) calls `esp_wifi_deinit()`, and the STA-retry / AP-fallback then re-runs `ensureWifiInit()` → `esp_wifi_init()` — the exact deinit→reinit cycle that on a hosted target triggers the GPIO-54 slave reset (round-3 open-issue #2's `sdmmc_card_init failed`). So the round-3 failure ("runtime SDIO re-init of the C6 fails") and this user's report may be **the same root cause**: the re-init shouldn't happen at all on a hosted target. **Concrete next step to try on the bench:** guard the teardown/re-init on `platform::hasWifiCoprocessor` (already defined = `isEsp32P4 && hasWiFi`) — on hosted targets, do NOT `esp_wifi_deinit()` in `wifiStaStop()` and do NOT re-`esp_wifi_init()` in `ensureWifiInit()` once the boot-time init is up; instead just `esp_wifi_disconnect()` + `esp_wifi_set_config()` + `esp_wifi_connect()` (STA retry) or `esp_wifi_set_mode(APSTA)` for the fallback, reusing the live transport. This is cheaper than the C6 reflash and independent of #18759, so it's worth trying first once the board boots. If it works, it also removes the slave-reset from the normal failover, not just the AP case. Blocked behind the #18759 boot crash like everything else P4-WiFi, but this is the first thing to try when the board boots again.
  2. **Co-processor components no longer compile into `esp32p4-eth` — FIXED.** The gate is now `rules: if CONFIG_MM_P4_WIFI == True` (a Kconfig option declared in `esp32/main/Kconfig.projbuild`, set only by `sdkconfig.defaults.esp32p4-eth-wifi`), so `esp_hosted` / `esp_wifi_remote` are pulled **only** by the WiFi build, never by eth-only. The old `target == esp32p4` gate pulled them into `esp32p4-eth` too; that wasn't merely build-time waste — esp_hosted self-inits its SDIO master at boot, which on the eth-only build interfered with the EMAC bring-up (a red herring chased during the P4 no-DHCP hunt). The eth-only image dropped 1.36→1.12 MB once gated out. The `wifiCoproc` read-out stays compile-gated on `platform::hasWifiCoprocessor` (`isEsp32P4 && hasWiFi`).
  3. **Build reproducibility.** `build_esp32.py` does not yet build this variant reliably: the C6 slave-target Kconfig `default ... if IDF_TARGET_ESP32P4` only fires on `set-target`, and the reconfigure a plain `build` triggers drops it back to ESP32-H2 (no WiFi) → fails on missing `CONFIG_WIFI_RMT_*`. A clean manual sequence works (`rm -rf <build dir>` → `set-target esp32p4` → `build`, all with the same `-DSDKCONFIG`/`-DSDKCONFIG_DEFAULTS`); the wrapper needs a fix so the auto-default sticks across reconfigures (see the KNOWN ISSUE comment in `build_esp32.py`).

  **Round 4 — the IDF-update regression (2026-07-03).** After the IDF bump to `v6.1-dev-5215-g0d928780081`, the variant stopped building entirely, then stopped booting. Two distinct causes, both IDF/component-manager side (all our config was correct):
  - ❌→✅ **Build: `esp_hosted.h` not found — the manifest `if` syntax changed.** The component manager (now 3.0.3) silently skipped `esp_hosted`/`esp_wifi_remote` (`NOTICE: Skipping optional dependency`) because our `idf_component.yml` rule used the bare `CONFIG_MM_P4_WIFI == True`. The current manager only recognises a Kconfig variable in the **`$CONFIG{...}`** form (its `KCONFIG_VAR_REGEX = \$CONFIG\{([^}]+)}`); the bare `CONFIG_X` falls through to plain string-eval → false → skipped. **Fixed:** `if: "$CONFIG{MM_P4_WIFI} == True"` (note: NO `CONFIG_` prefix inside the braces). Confirmed by [idf-component-manager #104](https://github.com/espressif/idf-component-manager/issues/104) + the official manifest docs. This also supersedes open-issue #3 above — with the correct syntax the pull is reliable (no `set-target` dance needed for the *dependency*).
  - ❌ **Boot: `sleep_clock_icg_startup_init` aborts with `ESP_ERR_NO_MEM` (0x101) → reboot loop.** A KNOWN, OPEN ESP-IDF bug: **[esp-idf #18759 (IDFGH-17859)](https://github.com/espressif/esp-idf/issues/18759)** — on ESP32-P4 + PSRAM, this sleep-clock retention init runs unconditionally at a SECONDARY boot phase (before `app_main`, NOT gated by `CONFIG_PM_ENABLE`) and fails to allocate its REGDMA retention links when early internal DRAM is tight, which it is once esp_hosted's SDIO stack is pulled in (WiFi build only; the eth-only P4 has DRAM to spare). Espressif's guidance on the issue: reduce early internal-DRAM static usage. Bench findings (2026-07-03): `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` does NOT help (those buffers allocate after the boot init); `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP=y` drops the ICG file but only moves the failure to the next retention alloc (PCR / int_wdt) then a `sleep_retention.c:914` assert; `.bss`/`.noinit` → PSRAM (`CONFIG_SPIRAM_ALLOW_{BSS,NOINIT}_SEG_EXTERNAL_MEMORY`) still fails — because `MALLOC_CAP_RETENTION` is a *specific reserved memory region*, not general DRAM, so freeing general DRAM doesn't reach it. This is the current blocker; MoonLight ran the same board on IDF 5.5 without it, so it's a 6.1-era regression.

    **Resolution paths (each independent):**
    1. **Upstream fix — the clean one, but no ETA.** Espressif's engineer on #18759 proposed *"add new configuration options to control whether the sleep clock ICG feature is enabled."* That fixes us directly: ICG retention only exists to save/restore clock config across a *light-sleep* cycle, and a 236-FPS LED controller never light-sleeps — so a `disable-ICG` Kconfig would let us skip the crashing init entirely, losing nothing, via one sdkconfig line. **Status (checked 2026-07-03):** the issue is `Status: Reviewing`, `Type: Bug`, **no milestone / no assignee / no target version** — the config-option approach was *offered by Espressif and seconded by another 24/7-app user (matti122), but not committed to a release*. So there is **no expected date**; it could land soon or sit. Watch [esp-idf #18759](https://github.com/espressif/esp-idf/issues/18759) for a linked PR / milestone.
    2. **IDF 5.5 fallback — investigated 2026-07-03, NOT a cheap escape.** MoonLight ran this exact board on IDF **5.5** without the crash, so a 5.5 build of just this variant (everything else on 6.1) looked like a timeline-independent path. A bench attempt against IDF 5.5.4 found `src/platform/esp32/` has drifted to genuinely require IDF **6.x**: four distinct 5.5↔6.1 API breaks in `platform_esp32.cpp` / `platform_config.h` — (a) `RMT_LL_TX_CANDIDATES_PER_INST` renamed (5.5: `SOC_RMT_TX_CANDIDATES_PER_GROUP`); (b) `esp_eth_phy_ip101.h` moved (5.5: ctor lives in core `esp_eth_phy.h`, no standalone header); (c) `CHIP_ESP32S31` enum is 6.1-only; (d) `ETH_ESP32_EMAC_DEFAULT_CONFIG()` in 5.5 has out-of-declaration-order designated initializers — a C++ **hard error no compiler flag suppresses** (`-fpermissive` doesn't touch it). So a working 5.5 binary needs permanent `#if`-IDF-version compat branches in the platform layer (an EMAC-init back-port + a second IDF 5.5.4 in the CI matrix) — a real feature, not a throwaway. Only worth it if #18759 stalls long enough that a shippable P4-WiFi is needed sooner.
    3. **Version-matched C6 slave reflash** (see round-3 item 1) — may change the boot memory picture, but is blocked behind the boot crash (host must boot to test the C6 handshake), so it only matters once 1 or 2 gets us to `app_main` stably.
- **Round 4 — Parlio loopback self-test FIXED (2026-07-09), real long strip still to prove.** The Parlio loopback self-test now passes on P4 hardware at any grid size (verified on MM-P4, jumper GPIO 32↔33, at both 8×8 and 128×128). A real *long* WS2812 strip (not just the bench panel) is the remaining hardware proof.

  **The bug it exposed (fixed):** on IDF `v6.1-beta1` the self-test failed `bad bit 0/0` at large grids — it worked at 8×8 but not 128×128. Two stacked hardware limits, both hit only because the test built a frame sized to the *whole operational grid*: (1) the P4 Parlio has no DMA-EOF, so one non-loop transfer is hard-capped at `PARLIO_LL_TX_MAX_BITS_PER_FRAME` (~0.5 Mbit) — a 128×128 grid (~1.2 Mbit) is rejected outright with `ESP_ERR_INVALID_ARG`, so nothing transmits; and (2) even once the transfer fits, the shared RMT-RX capture can't hold a large symbol count (a big frame → `rx captured 0 symbols`). Both surfaced as the same misleading `bad bit 0/0`. **A wrong first guess** (an IDF `sample_edge`→`shift_edge` clock-edge rename) was tried on hardware and rejected — both edge values fail, ruling the edge out; the serial log (`loopback: N bytes in …`, `rx captured 0 symbols`) is what pinned the real cause. **The fix:** the self-test now caps its frame to a small fixed `ParallelLedDriver::kLoopbackTestLights` (256) instead of the operational grid — the test only needs to prove the peripheral emits correct WS2812 bits, which a few hundred lights exercise fully (encode → fused correct+transpose → single-shot DMA → latch pad), and 256 fits under both the transfer cap and the capture buffer on every parallel driver, so it runs identically at any grid size. **Lesson:** an on-device self-test must size its own probe to the peripheral's real limits, not to the user's configured output size. (This whole diagnosis is a good "verify against the working reference before concluding" case — the PO's memory that it *had* worked at fd6c1d8e is what redirected the hunt from "capability gap" to "regression".)

  **Loopback on the MHC-WLED P4 shield — RESOLVED: the header is not pure GPIO (shield builder confirmed, 2026-07-09).** The shield's 4x-In/Out header (pins **46, 47, 2, 48**; 46 & 2 are P4 boot straps per gpio-usage) never passed the continuity precheck: **46↔47** captured partial/corrupt (206 of 1536 symbols → `bad bit 0/0`); **47↔48** read `jumper not detected` even with the jumper seated. The `mm_loopback continuity …: hi=%d lo=%d` diagnostic read `hi=0 lo=0` (no direct signal path). **Shield builder (Wladi) confirmed the cause:** the header has **no pure GPIOs** — outputs go through a **level shifter**, inputs through **diode protection + a ~16 kHz low-pass filter** (by design, for robust button-style inputs), so the raw drive-and-read continuity probe can't bridge it. Not a firmware bug and not fixable in software; the self-test needs a genuinely direct GPIO pair. **Workaround for a real loopback test on the shield (Wladi's suggestion):** drive the LED signal out one **RS485 output**, configure a 4th channel as an **RS485 input**, and jumper the two with cables — the signal is then readable on the respective GPIO. The frame-size fix itself is **proven independent of this** (MM-P4 NANO, direct 32↔33, PASSes at every grid). The `loopbackJumperOk` continuity read is kept as a permanent bench HAL diagnostic (it's what pinned this) — a `hi=0 lo=0` result now has a documented meaning: a buffered/level-shifted header, not a direct GPIO.

  **Dev-loop note — reading the P4's runtime log over USB.** The P4-NANO's primary console is **UART on GPIO 37/38** (`CONFIG_ESP_CONSOLE_UART_DEFAULT`), not the USB port, so `ESP_LOGI` / `mm_net` lines are *not* visible over `/dev/cu.usbmodem*` by default — only the ROM boot banner and `std::printf`-to-stdout (which routes to the **secondary** USB-Serial-JTAG console) come through. Two workarounds when you need the runtime log over USB: (a) temporarily set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (note the JTAG endpoint re-enumerates when the app starts, so a reader must reconnect across the drop — `idf.py monitor` handles it; a plain fixed `pyserial` handle dies); or (b) hang a USB-UART adapter on GPIO 37/38. This cost real time during the P4 no-DHCP hunt; the fastest signal there turned out to be a `printf` of the runtime struct (stdout → secondary JTAG console) plus a `git worktree` bisect (build an old commit, flash, check LAN reachability) to prove code-vs-hardware without needing the log at all.

### WiFi runtime disable (backlog)

Compile-time answer already ships: `--firmware esp32-eth` excludes the WiFi stack. The default `esp32` already *cascades* — `ethInit()` runs first, WiFi only comes up if no PHY responds — so a wired board never associates over WiFi. What's still missing is reclaiming WiFi's **heap**: even when Ethernet wins the cascade, `esp_wifi_init`'s RX buffers stay allocated. This item skips that init entirely once Ethernet is up, freeing ~16 KB. Defer until the heap saving is worth the teardown-ordering risk.


## UI

Forward-looking companion to the shipped UI spec, [moonmodules/core/services.md](../moonmodules/core/services.md). The live spec describes the UI as shipped; this file holds what is **not** in it yet: deferred items, open design questions for 1.0, and the gap analysis against projectMM v1. The backward-looking half (how v1/v2 actually worked, patterns consciously rejected, recorded quirks) lives in [history/v1-inventory.md](../history/v1-inventory.md).

### Deferred to 1.x

- Side nav with drag-reorder of root modules (root order is fixed in `main.cpp` today; not painful — and arguably correct, see the gap-analysis note below)
- Health panel (`<details>` + `GET /api/test`)
- Log panel (`<details>` + WS `{t:"log",m:"…"}`)
- Core affinity badge (C0/C1) — only meaningful when core pinning lands
- Module `category()` field — taxonomy beyond `role()` for the picker (decision: derive from `role()` for now)

### File Manager follow-ups

The shipped File Manager (see [system.md](../moonmodules/core/system.md#file-manager)) is a lazy expand/collapse tree over `/api/dir` + a size-capped text editor over `/api/file`, with drag-drop upload (tier 1) + per-file download + a filesystem-usage bar. Deferred capabilities, each self-contained:

- **Upload — recursive folder tier.** Single-file upload of **any size** streams to the file (`fsWriteStream`, binary-safe, `kUploadMax` + free-space guarded) and downloads stream back (`fsReadAt`), so text/config/binary all work. Remaining: **multi-file / recursive folder drops** — client-side `mkdir` + walk via `dataTransfer.items` `webkitGetAsEntry()`.
- **Download — folder as `.zip`.** Per-file download streams (`<a download>` on `/api/file`). A folder download needs the browser to walk `/api/dir` recursively, fetch each file, and build a `.zip` client-side — which means bundling a zip library into `app.js`. That's a permanent app.js size bump, and app.js is embedded in firmware, so it weighs on the flash budget (see the flash-budget item above). Gate on real demand; symmetric with the folder-upload tier.
- **Last-modified dates.** Needs a time source (NTP) + LittleFS mtime storage; the tree is name + size until both land. Backlogged with the NTP work.
- **`.ml` syntax highlighting in the editor.** MoonLive source wants *highlighting* (a colour layer over the textarea), not the JSON-style reformat — a bigger editor change (a highlight overlay or a small tokenizer), added when MoonLive `.ml` files land on disk. The editor already has an extension seam (`fmPrettify`) for the reformat case; highlighting is the separate, larger tier.
- **Keyboard + screen-reader access for the tree.** The `fm-row` tree entries are mouse-only today (click to select/expand/open). Making them keyboard-operable (focusable, Enter/Space activation) with ARIA tree semantics (`role="treeitem"`, `aria-expanded` from `isOpen`, `aria-level`) is a real accessibility win but adds JS that lands in the firmware-embedded `app.js` (flash cost). Do it when a11y is a stated goal, together across the whole generic UI, not just this panel.

### Open design questions

These don't block the shipped baseline but should be answered before 1.0:

- **Multi-layer UI** — [architecture.md](../architecture.md) plans for N layers blended into one Drivers. The current card layout shows one Layer. Likely needs a tab/accordion to switch layers, or a per-layer column.
- **Modifier chain visualization** — show the modifier order visually. They're a flat list today, but the `children[]` order **is** the apply order now (modifiers compose as a chain, M₁∘M₂∘…), so a visual that conveys the stacking (and that order matters) would help users reason about a multi-modifier layer.
- **Presets** — save/load named bundles of control values. Persistence already stores them; needs a UI surface.
- **Canvas/node-graph view** — v2 attempted this. Powerful for complex setups but doubles the UI surface. A reasonable v3 follow-up gated on user demand.

### Gap analysis — v1 features not yet in v3

Inventory of v1 frontend behaviours v3 lacks, with a recommendation each. Items already shipped (control types, dragTs, two-timescale inputs, type picker, theme, scroll-shrink preview, status bar, reset-to-default, fps/ms toggle, drag reorder, side nav + drawer + footer) are not repeated.

Legend: **Adopt-1.0** (small, high value) · **Defer-1.x** (needs engine work or a feature we lack) · **Drop** (not needed).

### Per-card features

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Header: setup-dot before name | name only | **Defer-1.x** — needs `setupOk()` + `health()` on MoonModule with a real failure mode. Today both would always be `true` / `""`. |
| Module ID shown separately from name | name only | **Defer-1.x** — add when instances need disambiguating (e.g. two effects of the same type under one Layer). |
| Category emoji badge on the card header | role emoji in the picker, not on the card | **Defer-1.x** — `ROLE_EMOJI` already exists in `app.js`; showing it per-card is a small step if card scannability needs it. |
| Core affinity badge (C0/C1) | core pinning not implemented | **Drop** until core pinning is a real engine feature. |
| Memory split heap vs PSRAM | `static+dynamic` shown on the card | **Defer-1.x** — splitting `dynamicBytes` further needs `platform::isPsramPointer(p)` or per-alloc tracking, neither exists yet. |

### WebSocket / panels

| v1 feature | v3 today | Recommendation |
|---|---|---|
| Drag-to-reorder *root* modules (`POST /api/modules/reorder`) | not supported | **Drop** — root order is fixed in `main.cpp` and that's correct: Layouts/Layers/Drivers + system modules are mandatory and ordered. Children reorder via drag already. |
| Log channel `{t:"log",m:"…"}` pushed by server | no server log push | **Defer-1.x** — needs an engine-side log producer. Gate: when boot/network/persistence logs become interesting to non-developers. |
| Schema channel `{t:"schema",modules:[…]}` for tree-shape changes | full `/api/state` push every update | **Drop** — keep the full-tree push; re-evaluate only if WS bandwidth becomes a problem with large trees. |
| System health panel (polls `GET /api/test`, pass/fail table) | none | **Defer-1.x** — needs a runtime `/api/test` that runs the doctest suite; `ctest` covers this for now. |
| Log panel (ring buffer, severity colouring, stick-to-bottom, `GET /api/log` backfill) | none | **Defer-1.x** — pairs with the log WS channel; both arrive together. |

### Cost / decision table

| Cost class | Items |
|---|---|
| Tiny (< 30 lines, no backend) | category emoji badge on the card header |
| Medium (minor backend change) | help-link mapping (needs docs site); richer `category()` than role()-derived |
| Large (separate plan) | health panel + `/api/test`; log panel + WS log channel; OTA + GitHub-update badge; full multi-layer UI; presets UI |
