# Plan — ESP32-S31 RGMII Ethernet (1 Gb) with Ethernet-preferred cascade

## Context

The bench ESP32-S31 board (Espressif Function-CoreBoard-1) has an on-chip 1 Gb EMAC wired
through an **RGMII** interface to a **YT8531** PHY → RJ45. The product owner connected an
Ethernet cable to it and it isn't used yet: the S31 firmware only brings up WiFi. The goal is
Ethernet-preferred networking — use Ethernet when the cable is up at boot, fall back to WiFi
otherwise — matching how the classic ESP32 (Olimex) and P4 boards already behave.

**Why S31-only:** among projectMM's targets, the S31 is the *only* chip whose EMAC advertises
`SOC_EMAC_SUPPORT_1000M` + RGMII. The classic ESP32 and P4 EMACs are RMII (100 Mb); S2/S3/C3/C6
have no EMAC at all (Ethernet only via an external W5500 SPI chip). RGMII/1000M is intrinsic to
the SoC — no extension board can add it to a non-S31 — so the RGMII path is S31-only by nature,
not a selectable per-board option. (Product owner confirmed: "otherwise S31 only".)

**Failover is already built:** `NetworkModule` runs `ethInit()` first and only starts WiFi if it
returns false (no PHY/cable). So "use Ethernet when available, WiFi otherwise" needs **no new
failover code** — only a new RGMII init path that the existing cascade calls. (Product owner
confirmed: Ethernet-preferred cascade at boot, not live hot-swap.)

## Design (mirrors the existing RMII path, adds an RGMII sibling)

The Ethernet layer already dispatches on `ethConfig_.phyType`: `ethInitRmii()` (on-chip EMAC,
RMII) and `ethInitSpi()` (W5500). This adds a third sibling, `ethInitRgmii()`, selected by a new
`ethYt8531` phyType — same shape, same cascade, same on-chip-EMAC compile guard.

**RGMII data pins are hardwired, not runtime config.** Exactly like RMII (whose TX/RX data lines
live in the IDF EMAC macro, not `EthPinConfig` — see the comment at
[platform_config.h:196-198](src/platform/esp32/platform_config.h#L196)), the S31 CoreBoard's RGMII
data pins are fixed by the board schematic. So they go straight into `ethInitRgmii()` as literals
from the schematic — **no new `EthPinConfig` fields, no NetworkModule controls, no deviceModels.json
eth block.** This keeps the struct and the UI untouched; the whole feature is one board's wiring.

**Pins (from `docs/reference/esp32-s31-coreboard.md`, sourced from the official schematic):**
MDC 4, MDIO 5, PHY reset 6, PHY int 2; TX_CTL 11, TXD0-3 = 7/8/9/10; RX_CTL 15, RXD0-3 = 19/18/17/16;
clock_tx 13, clock_rx 14. PHY = YT8531 via `esp_eth_phy_new_generic` (IEEE-standard registers).

## Files to change

1. **`src/platform/esp32/platform_config.h`**
   - Add `isEsp32S31` constexpr flag (keyed on `CONFIG_IDF_TARGET_ESP32S31`), following the
     `isEsp32P4`/`isEsp32S3` pattern at [L33-46](src/platform/esp32/platform_config.h#L33).
   - Add `ethYt8531 = 4` to the `EthPhyType` enum ([L175](src/platform/esp32/platform_config.h#L175)),
     with a one-line "RGMII, YT8531 PHY, S31 on-chip 1 Gb EMAC" comment.
   - Add an `isEsp32S31` branch to the `ethConfigDefault` ternary
     ([L216](src/platform/esp32/platform_config.h#L216)): `phyType ethYt8531`, `phyAddr` (from the
     YT8531 strap — default 0, confirm on bench), `rstGpio 6`, MDC/MDIO 4/5. RGMII data + clock
     pins are NOT struct fields (hardwired in `ethInitRgmii`); pass -1 for the unused RMII/SPI
     fields. **No struct change.**

2. **`src/platform/esp32/platform_esp32.cpp`**
   - Add `static bool ethInitRgmii()` mirroring `ethInitRmii()`
     ([L457-543](src/platform/esp32/platform_esp32.cpp#L457)) under the same
     `#ifdef CONFIG_ETH_USE_ESP32_EMAC` guard. Differences from the RMII version:
     - `emac_config.interface = EMAC_DATA_INTERFACE_RGMII`
     - set `emac_config.clock_config.rgmii.clock_tx_gpio/clock_rx_gpio` (13/14) and the
       `emac_config.emac_dataif_gpio.rgmii` struct (tx_ctl/txd0-3/rx_ctl/rxd0-3 = the schematic
       pins) — the RGMII fields IDF exposes in `esp_eth_mac_esp.h`.
     - PHY: `esp_eth_phy_new_generic(&phy_config)` (YT8531 is standard-register; same generic ctor
       LAN8720 uses — no managed component needed).
     - reuse the identical `fail()` cleanup lambda, driver-install, netif-attach, event-register,
       non-blocking `esp_eth_start`, and the link-up hostname handling. Log "Ethernet init done
       (RGMII, S31)".
   - Add the dispatch case to `ethInit()`
     ([the switch, ~L661](src/platform/esp32/platform_esp32.cpp#L661)):
     `#ifdef CONFIG_ETH_USE_ESP32_EMAC` → `case ethYt8531: return ethInitRgmii();` (alongside the
     existing `ethLan8720`/`ethIp101` RMII cases).

3. **`esp32/sdkconfig.defaults.esp32s31`** — `CONFIG_ETH_USE_ESP32_EMAC=y` + DMA buffers are
   already present ([L26-32](esp32/sdkconfig.defaults.esp32s31)). RGMII is selected at runtime via
   the struct `interface` field (not a sdkconfig symbol), so likely **no change** — but verify at
   configure time that no `CONFIG_ETH_*RGMII*`/1000M symbol is required; add it only if the build
   demands it.

4. **`web-installer/deviceModels.json`** — move `"Ethernet"` from the S31's `planned` list to
   `supported` (the S31 entry). No eth `NetworkModule` control block needed (pins are the
   compile-time default). `check_devices.py` allows `Ethernet` in `supported` (it's in
   `SUPPORTED_VOCAB`).

5. **`docs/reference/esp32-s31-coreboard.md`** — update the Ethernet section's "wiring the S31 eth
   needs an RGMII branch — not a drop-in" note to present-tense "driven by `ethInitRgmii`", since
   it now ships. (Small doc sync, per the present-tense rule.)

## Not doing (deliberately, keeps it minimal)

- No `EthPinConfig` struct fields for RGMII data pins (hardwired, like RMII).
- No NetworkModule UI controls, no `syncEthConfig` change (nothing new to sync).
- No deviceModels.json eth-pin block for the S31 (compile-time default covers it).
- No failover/route-switching code (the eth→WiFi cascade already exists).
- No new managed component (generic PHY driver covers YT8531).

## Verification

- **Build:** `esp32s31` builds clean on 6.1 with `-Werror` (the RGMII branch is behind the
  already-set `CONFIG_ETH_USE_ESP32_EMAC`; other targets unaffected — the case is chip-guarded).
- **Non-regression:** classic/P4/S3 eth paths untouched (RMII/SPI code unchanged); a quick
  `esp32p4-eth` + `esp32` build stays green. `check_devices.py` green with `Ethernet` under S31
  `supported`. `ctest` + scenarios unaffected (platform-only change).
- **Bench (the real test), on the connected S31 (`/dev/cu.usbserial-20213420`), cable plugged in:**
  1. Flash `esp32s31`, capture boot log → expect `Ethernet init done (RGMII, S31)` then an
     **Ethernet DHCP lease** (an `MM_IP=` from the wired subnet, like the P4 eth test showed
     `192.168.1.133`), and mDNS `MM-S31.local`.
  2. Confirm the render loop still runs (FPS line present) and heap is healthy.
  3. **Failover check:** unplug the cable, reboot → it should fall back to WiFi (the existing
     cascade). Plug back in, reboot → Ethernet again. (Boot-time cascade, per the chosen model.)
- Save the approved plan to `docs/history/plans/Plan-20260703 - S31 RGMII Ethernet.md` as the first
  implementation step.

## Open items to confirm on the bench during implementation

- **YT8531 PHY address** — default strap is usually 0; if `esp_eth` can't find the PHY at addr 0,
  scan/try 1 (the reference doc doesn't pin the strap). One-line fix in `ethConfigDefault`.
- **RGMII clock direction / delay** — YT8531 boards sometimes need RX/TX clock delay config; if the
  link comes up but no packets flow, revisit the RGMII clock config. (Bench will show.)
