# Plan: Static IP support for WiFi STA + Ethernet (wire the existing controls to the netif)

## Context

The Network module already shows an `addressing` dropdown (**DHCP / Static**) and four IPv4 controls (`ip`, `gateway`, `subnet`, `dns`), stored as `staticIp_/staticGateway_/staticSubnet_/staticDns_` (uint8[4] octets) in `NetworkModule.h` and persisted. But **nothing applies them**: `esp_netif_set_ip_info` is called only for the SoftAP (`platform_esp32.cpp:1122`). Neither `wifiStaInit` nor `ethInitEmac` reads `addressing_` or the static octets, so both the STA and ETH netifs always run their DHCP client regardless of the dropdown. The controls are inert on both interfaces. NetworkModule's own docstring (`:89`) says the `addressing` selector + static-IP controls are meant to "remain" and apply to whichever interface is active, so this closes a gap that was designed for but never wired.

The product owner asked for static support on Ethernet "just like wifi"; since WiFi static is *also* inert, the agreed scope is to make static actually work for **both** STA and ETH.

**Bonus experiment (S31 at 100M).** A static IP bypasses DHCP entirely. The S31 links at 100M but can't complete the DHCP handshake (the open TXC issue, see backlog-core.md). If a static IP makes Ethernet reachable at 100M, that proves the RX/unicast path works and only the DHCP handshake was the 100M blocker: a simpler path than the TXC resync, and possibly enough to call 100M "supported via static." The plan flags this as a bench test.

## Design

Mirror the AP static path (`platform_esp32.cpp:1115-1124`: `dhcps_stop` → build `esp_netif_ip_info_t` → `set_ip_info` → restart) in the **client** form for STA and ETH: `esp_netif_dhcpc_stop(netif)` → `esp_netif_set_ip_info(netif, &info)` → `esp_netif_set_dns_info(netif, ...)`. No dhcpc restart — static means the client stays stopped.

**Stage 1 — platform seam.** Add one narrow platform function, `platform.h`:
```
void netSetStaticIPv4(NetIface iface, const uint8_t ip[4], const uint8_t gw[4],
                      const uint8_t mask[4], const uint8_t dns[4]);
```
`NetIface` is a tiny enum (`NetSta`, `NetEth`) so one function serves both, resolved to `staNetif_` / `ethNetif_` internally. If all-zero `ip` is passed, treat as "no static / use DHCP" (defensive). The ESP32 impl does the dhcpc_stop/set_ip_info/set_dns sequence on the resolved netif; the desktop stub is a no-op (like the other net stubs). This is a domain-neutral, single-purpose primitive — the recognizable "set static addressing on an interface" call, not a bespoke per-interface duplicate. (Naming: `netSetStaticIPv4` matches the existing `wifiStaGetIPv4` / `ethGetIPv4` octet-getter convention in `platform.h`.)

**Stage 2 — apply on bring-up.** In `NetworkModule`, after each interface is initialized and its netif exists, when `addressing_ == 1` (Static) call `platform::netSetStaticIPv4(...)` for that interface:
- **STA:** right after `wifiStaInit` succeeds (the STA netif exists once `esp_wifi_start` ran; apply before/instead of the DHCP client taking a lease). Because the static apply stops the dhcp client, it must run once the netif is created — call it from NetworkModule right after `wifiStaInit()` returns true, guarded by `addressing_ == 1`.
- **ETH:** the eth netif exists after `ethInit()`; apply when Static. The eth DHCP client is started from the IDF CONNECTED handler, so for a clean static setup the apply must also run on link-up. Simplest robust approach: NetworkModule calls `netSetStaticIPv4` for eth when it observes eth link-up in Static mode (in the `WaitingEth` / cascade path), and the platform's eth CONNECTED handler skips `applyHostname`'s dhcpc_start when a static IP is set. Exact wiring settled in implementation; the invariant is "Static mode → dhcpc stopped + ip_info set, on whichever netif is active, without a DHCP round."
- On Static, `ethConnected_` / the STA "got IP" state must be considered connected **without** waiting for a DHCP `GOT_IP` event (there won't be one). NetworkModule treats "Static + netif has the static IP applied" as connected: set the connected state directly after applying, rather than waiting on `ethConnected()` / `wifiStaConnected()` which key off DHCP/association-IP events.

**Stage 3 — apply live on toggle.** Toggling `addressing` (or editing a static field) already triggers `rebuildControls()` (the Select→hidden re-eval). Extend the live path: when `addressing_` flips to Static, apply the static config to the currently-active interface immediately; when it flips back to DHCP, restart the DHCP client (`esp_netif_dhcpc_start`) so the device re-leases without a reboot (the "no reboot to apply" principle). This routes through the existing `onControlChanged` / dirty path NetworkModule already uses for live network reconfig.

**Stage 4 — status.** `updateStatusIP()` already reads the netif IP via `currentIp()` → `ethGetIPv4`/`wifiStaGetIPv4`, which returns the *applied* static IP (set_ip_info makes it the netif address). So the status line shows the static IP with no change. The eth-degraded warning path only fires in DHCP mode (Static won't hit the DHCP-timeout branch), so it composes cleanly.

## Files

- `src/platform/platform.h` — declare `NetIface` enum + `netSetStaticIPv4(...)`.
- `src/platform/esp32/platform_esp32.cpp` — implement it (dhcpc_stop/set_ip_info/set_dns on the resolved netif); teach the eth CONNECTED handler + `applyHostname` to skip dhcpc_start when static is active (a `bool ethStatic_` / STA equivalent set by the setter, or a param).
- `src/platform/desktop/platform_desktop.cpp` — no-op stub.
- `src/core/NetworkModule.h` — call `netSetStaticIPv4` on bring-up + on live toggle for STA and ETH; treat Static as connected without a DHCP event; DHCP-restart on toggle back.

## Reuse (don't reinvent)

- The AP static block (`platform_esp32.cpp:1115-1124`) is the exact server-side shape; the client side is the same minus dhcps→dhcpc and plus DNS.
- `formatDottedQuad` / the `ControlType::IPv4` octet storage already parse/hold the addresses; the octets go straight into `esp_netif_ip_info_t` via `IP4_ADDR` / `esp_netif_set_ip_info`.
- `currentIp()` / `updateStatusIP()` already surface the netif IP — no status rework.

## Tests (regression, per the project rule)

Host-testable seam (`test/unit/core/unit_NetworkModule_ethernet.cpp` or a new `unit_NetworkModule_static.cpp`):
- The `addressing` Select is index 0=DHCP / 1=Static, defaults to DHCP; the four static IPv4 controls exist with the documented defaults (subnet `255.255.255.0`), and are hidden when `addressing_ != 1` (pins the control contract + the visibility rule).
- The desktop `netSetStaticIPv4` stub is a safe no-op (accepts any octets, doesn't bring an interface up) — mirrors the existing "desktop net seam is inert" tests.
The actual dhcpc_stop/set_ip_info is ESP32-only (bench-verified, not host-mockable) — pin the control/seam contract host-side, verify the apply on hardware.

## Verification

1. **Host:** `cmake --build build` (0 warnings) + `ctest` + scenarios green.
2. **S31 build:** `uv run moondeck/build/build_esp32.py --firmware esp32s31 --skip-idf-pin-check` (0 warnings).
3. **Bench (WiFi static):** on a WiFi board, set addressing=Static + a valid static IP/gw/mask/dns on the LAN, confirm the device comes up at that IP (reachable, UI loads), and that toggling back to DHCP re-leases live (no reboot). PO-observed.
4. **Bench (Ethernet static) + the 100M experiment:** on the S31 at 100M, set a static IP in the LAN's range and confirm Ethernet becomes **reachable** (ping + UI at the static IP) — the decisive test: if it works, unicast/RX is fine and DHCP-handshake was the only 100M gap. PO-observed; this is the measurement, serial is supporting evidence.
5. **No-reboot toggle both ways** and **persistence across reboot** (the octets already persist; confirm they re-apply on boot in Static mode).

## Risks / notes

- **Static "connected" without a DHCP event** is the main behavioral change: the cascade's connected-detection keys off DHCP/association IP events today; Static must mark connected right after the apply. Keep this contained in NetworkModule's state machine (don't fake a platform event).
- **DNS optional:** if `dns` is all-zero, skip `set_dns_info` (leave whatever's there) rather than setting 0.0.0.0.
- **Gateway/subnet sanity:** a static IP outside the gateway's subnet silently won't route; out of scope to validate, but the status still shows the applied IP so the user can tell it took.
- **Platform boundary:** all IDF calls stay in `platform_esp32.cpp`; NetworkModule only calls `platform::netSetStaticIPv4` + the existing init functions (no `#ifdef` leaks).
