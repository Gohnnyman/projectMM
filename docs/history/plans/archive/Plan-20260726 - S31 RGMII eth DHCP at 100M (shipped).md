# Plan: ESP32-S31 RGMII Ethernet DHCP at 100 Mbps (fix the TXC-speed mismatch)

## Context

The ESP32-S31's on-chip EMAC is 1 Gb RGMII (YT8531 PHY). On a **gigabit** switch it leases fine (link at 1000M, MAC Tx clock TXC = 125 MHz, the driver's install default). On a **10/100** switch — like the bench GL-AR300M — the link negotiates to **100M**, where RGMII requires **TXC = 25 MHz**. The MAC never gets reconfigured to 25 MHz, so every Tx frame clocks out garbled and the switch drops it. Symptom: link up, DHCP DISCOVER sent, **no OFFER ever** → `NetworkModule: Ethernet no IP (DHCP timeout), cascading` → falls back to WiFi.

**This is not a regression.** Verified on hardware at commit `d5ee07c` built against its exact pinned IDF (`0d928780081` / v6.1-dev-5215, confirmed via both `setup_esp_idf.py` and `release.yml:169`): eth DHCP times out on the 10/100 router there too. Eth has **never** worked at 100M on this firmware; the earlier "DHCP confirmed" success was on a 1 Gb switch. So the goal is a genuine new capability: **eth DHCP at 100M**, without regressing 1000M.

**Root cause (confirmed by IDF source trace).** The MAC's `emac_esp32_set_speed` (`esp_eth_mac_esp.c:422`) reprograms TXC per link rate. It runs **only** via `on_state_changed(ETH_STATE_SPEED)`, emitted from the generic 802.3 PHY driver's `updt_link_dup_spd` (`esp_eth_phy_802_3.c:217`) — and that is gated on a link_status **transition** (`if (phy_802_3->link_status != link)`, `:235`). Our firmware manually re-enables the YT8531's auto-negotiation (a documented YT8531-reset quirk) *before* `esp_eth_start`; negotiation can complete and latch link-UP **before** the PHY poll task's first read, so the poll sees no transition, so `set_speed(100M)` never runs, so TXC stays at its 125 MHz install default. RX still works (it rides the PHY-recovered RXC), which is why DHCP OFFERs to the *WiFi* interface kept masking this.

**Why the current WIP is not the final fix.** The stashed WIP corrects TXC with an `esp_eth_stop()` + `esp_eth_start()` bounce placed after the netif glue is attached. It *did* produce the first-ever eth lease (`.125`) — proving the mechanism — but `esp_eth_stop` unconditionally tears the netif down: it releases the DHCP lease, resets `dhcpc_status` to INIT, and sets the netif IP to `0.0.0.0` (`esp_netif_lwip.c:1308-1360, 1899-1948`). That re-acquisition races the NetworkModule cascade's 15 s eth-DHCP window and the double CONNECTED event (which re-runs `applyHostname`'s dhcp stop/restart), giving the non-deterministic "sometimes eth `.125` with a half-applied netif, sometimes WiFi `.212`" behaviour observed on the bench.

## The fix: force one PHY link-state transition, without touching the netif

IDF exposes exactly the public surface to re-drive the speed push without a driver stop/start (all confirmed present in the pinned IDF):
- `esp_eth_get_phy_instance(handle, &phy)` — `esp_eth_driver.h:403`
- `esp_eth_phy_into_phy_802_3(phy)` — public inline cast, `esp_eth_phy_802_3.h:372`
- `phy_802_3_t::link_status` — public member, `esp_eth_phy_802_3.h:27`
- `phy->get_link(phy)` — public fn-ptr, `esp_eth_phy.h:130`

**Mechanism:** after `esp_eth_start` and once the link has settled UP, obtain the PHY instance, set `link_status = ETH_LINK_DOWN` **directly** (NOT via `esp_eth_phy_802_3_set_link`, which would post DISCONNECTED and disturb the netif), then call `phy->get_link(phy)`. The poll now sees a fresh DOWN→UP transition, re-reads the negotiated 100M, emits `ETH_STATE_SPEED`, and `emac_esp32_set_speed` lands TXC at 25 MHz — **the netif, DHCP client, and any lease are never touched.** This removes both remaining problems (netif teardown race + IP-not-applied) by construction, because there is no teardown.

A no-op at 1000M (the re-detected speed is 1000M, TXC already 125 MHz). S31-only (`#ifdef CONFIG_IDF_TARGET_ESP32S31`); the classic/P4 RMII path derives Tx clock from the fixed 50 MHz REF_CLK and has no per-speed TXC.

## Design

**File: `src/platform/esp32/platform_esp32.cpp`** (the S31 eth init `ethInitEmac`, ~line 703-744)

1. **Remove** the stashed `esp_eth_stop()` + `esp_eth_start()` bounce block (lines ~723-740).
2. **Add** a new S31-only helper `ethYt8531ForceSpeedResync(esp_eth_handle_t)` that:
   - `esp_eth_get_phy_instance(handle, &phy)`; on error, log-warn and return (non-fatal, as with `ethYt8531BoardInit`).
   - `phy_802_3_t* p = esp_eth_phy_into_phy_802_3(phy);`
   - `p->link_status = ETH_LINK_DOWN;`
   - `phy->get_link(phy);` — re-detects and pushes speed→MAC.
   - Keep the existing `ETH-DIAG`/diagnostic prints controlled by the temp-debug flags until the PO signs off end-to-end.
3. **Call site:** the link settles UP a few hundred ms after `esp_eth_start`. Rather than a fixed long `vTaskDelay` in `ethInitEmac` (which eats the cascade's eth-DHCP budget and blocks boot), invoke the resync from the **`ETHERNET_EVENT_CONNECTED` handler** (`ethEventHandler`, ~line 485) on the S31 — that fires exactly when the link first comes UP, i.e. the first (speed-less) transition. Forcing `link_status = DOWN` + `get_link` there triggers the immediate second transition that carries the speed. Guard it to run **once** per link-up (a static/So-far flag reset on DISCONNECTED) so it doesn't recurse. This also means it self-heals on a live cable re-plug, not just at boot — consistent with the "no reboot to apply" principle.
   - Confirm ordering against `applyHostname(ethNetif_)` already in that handler: run the speed resync **before** `applyHostname`, so the DHCP client that `applyHostname` (re)starts runs on a MAC whose TXC is already correct.

**Keep the confirmed-good WIP fixes** (independent of this change, already validated):
- `ethYt8531BoardInit` (autoneg re-enable + RGMII delays) — unchanged; still needed.
- `ethPhyAddr` `int16_t` + `addInt16(-1,31)` + `numberField` — unchanged.
- The `/api/types` probe-freeze guard in `ParallelLedDriver.h` + its regression test — unrelated, keep.

**Temp debug (remove only on PO end-to-end sign-off, per standing instruction):** the ARP/netif/IP/UDP/ETH/DHCP debug flags in `sdkconfig.defaults.esp32s31` + `sdkconfig.defaults`, and the `ETH-DIAG`/`ETH-MAC` printfs. Leave in place through bench verification.

## Regression tests (per PO directive: every fix pinned in a test)

The TXC-resync is platform/hardware logic (esp_eth calls) that can't run on the desktop host. Pin what *can* be pinned at the seam, host-side:
1. **`ethPhyAddr` sentinel** (`test/unit/core/` NetworkModule control test): assert the control is `int16` with range `[-1, 31]` and default `-1` (so a `uint8` cast can never again mangle `-1`→31), and that `numberField` is set on it. This is pure control-metadata, host-testable.
2. **Speed-resync contract** (documented + asserted where possible): the platform desktop stub already returns `ethConnected()/ethLinkUp() == false`; add a focused unit or a documented invariant that the S31 eth CONNECTED path calls the resync before `applyHostname`. Since the esp_eth calls are ESP32-only, pin the *ordering/contract* via a small seam (e.g. a testable free function or a documented sequence asserted by a comment + a platform-boundary check) rather than mocking IDF. Exact seam chosen during implementation; the bar is "a future edit that drops the resync or reorders it fails a check," not "mock the whole IDF."
3. Keep the existing render-split probe-freeze regression test (`test/unit/light/unit_Drivers_rendersplit.cpp`) — already green.

Document the root cause + fix in `docs/history/lessons.md` (S31 RGMII 100M TXC), and update the memory note `s31-ethernet-dhcp-rx-open` outcome once bench-verified.

## Verification

1. **Host gates:** `cmake --build build` (zero warnings) + `ctest` + `uv run moondeck/scenario/run_scenario.py` green.
2. **S31 build:** `uv run moondeck/build/build_esp32.py --firmware esp32s31 --skip-idf-pin-check` (current IDF release/v6.1) — zero warnings.
3. **Bench (the real test), on the 10/100 GL-AR300M:** flash the S31, capture serial. Success criteria:
   - Serial shows the speed resync running and (with debug on) `working in 100Mbps` from the EMAC driver, i.e. TXC reconfigured.
   - `NetworkModule: Connected via Ethernet — Eth: <ip>` and an `IP_EVENT_ETH_GOT_IP` — a **deterministic** eth lease across repeated reboots (not sometimes-WiFi).
   - **PO-observed:** the S31 is reachable over the Ethernet cable at its leased IP (`http://<ip>` / `MM-S31.local`) — ARP resolves, ping replies, UI loads. This is the measurement; agent serial logs are supporting evidence only.
   - Re-plug the cable live → eth re-leases without reboot (self-heal).
4. **No 1000M regression:** when a gigabit switch is available (PO has one, not on hand now), confirm eth still leases at 1000M (the resync is a no-op there).

## Notes / risks

- **Direct `link_status` write** touches a public struct member but bypasses the `set_link` helper deliberately (to avoid the DISCONNECTED post that would disturb the netif). This is a bespoke touch of IDF internals — carry a one-line comment at the site naming *why* (netif-preserving) per the "bespoke choices carry their reason" principle. It uses only public headers (`esp_eth_phy_802_3.h`), not private ones.
- If `phy->get_link(phy)` inside the event handler proves to re-enter awkwardly (it runs on the event-loop task, same as the poll), fall back to just `link_status = DOWN` and let the **next scheduled poll** (`check_link_period_ms`) do the re-detect — one poll interval later, still well inside the 15 s cascade window. Decide by bench observation.
- The stashed WIP's `esp_eth_stop/start` bounce is **removed**, not kept as a fallback — it's the racy path this replaces (no-hacks floor).
