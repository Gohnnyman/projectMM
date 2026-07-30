# Plan: driving LED panel cards over raw Ethernet (`PanelCardDriver`)

## Context

[Issue #58](https://github.com/MoonModules/projectMM/issues/58) asks to drive ColorLight receiver
cards from one of our boards, replacing the Linux host that drives them today.

**The board renders and sends.** Effects, layers, MoonLive and the preset system are already on the
device, so the primary case is a self-contained panel controller: our own render output goes
straight out to the cards. Taking ArtNet or E1.31 in and forwarding it is one application of the
same driver, and it comes last.

The two facts that shape the driver: the cards need a **1 Gbit link** (a wire-time constraint,
rather than bandwidth), and the transport is **raw Ethernet frames** rather than UDP.

**Target: ESP32-S31.** It is the only board we ship with RGMII gigabit. The P4 is RMII/100 Mbit
(`platform_esp32.cpp:562`).

### The open S31 Ethernet issue, and why this feature sits below it

S31 Ethernet currently negotiates 1000M full duplex and transmits (activity LED, frames on the
wire), while DHCP does not complete: the signature points at the RGMII **RX** path needing
board-specific delay tuning (`Plan-20260726`).

**This driver is TX-only over raw L2**, which is the half that already works, and it skips the two
layers the bug lives in: no DHCP, no lwIP, no IP address. `esp_eth_transmit` takes a frame with our
own destination MAC and puts it on the wire.

So it is plausible this works on an S31 that still cannot get a DHCP lease, which also makes it a
useful bisect: **panels lighting up confirms TX and the 1 Gbit link independently of the IP stack.**
Two caveats keep this from being a free pass. Sustained gigabit TX is a heavier exercise of the
RGMII timing than DHCP's few frames, so a marginal link may show up here as corrupt pixels rather
than as no output. And step 4 needs RX, so the bug is a hard prerequisite there.

**Step 1 therefore starts with a TX-only spike on the current firmware:** send a hand-built frame,
confirm it on the wire with a capture. That answers the risk before any driver code is written.

## How this is written

The wire format is documented by open implementations. We write our own code against the
documented byte layout, the same way our ArtNet, DDP and E1.31 support is built: the layout is a
fact about the format, and the code that acts on it is ours. Where a byte's purpose is
undocumented, the code marks it unknown.

The driver is `PanelCardDriver` with a format selector, because the category is panel cards and
ColorLight 5A-75 is the first format it speaks.

## Design

One driver, one format per card family: the shape `NetworkSendDriver` already uses for
ArtNet/E1.31/DDP.

```
src/light/drivers/PanelCardDriver.h        window, correction, chunking, sync
src/light/ColorLight5A75Packet.h           the first wire format
src/platform/platform.h                    + ethSendRaw declaration
src/platform/esp32/platform_esp32.cpp      + ethSendRaw implementation
src/platform/desktop/platform_desktop.cpp  + ethSendRaw stub (host build runs everything)
```

**Reuse, do not reinvent:** `DriverBase` (window, preset, correction), `windowSlice`, the catalog
registration, the `Correction` pipeline, and the `*Packet.h` convention. The new code is the packet
builder and one platform seam.

Linsn, Novastar and DBstar are candidates for later formats. Whether their layouts fit this
window/chunk model is unverified, so the architecture leaves room and the plan commits to one
format.

## Steps

### Step 1: the platform seam

`platform::ethSendRaw(const uint8_t* frame, size_t len)`, ~20 lines, wrapping `esp_eth_transmit`
on the `esp_eth_handle_t` the Ethernet init already holds. Desktop gets a stub that records frames
so the host build and its tests still run everything (the desktop-runs-everything rule).

**Exit:** a unit test that the desktop stub receives what the driver emits; `check_platform_boundary`
clean, with `esp_eth_*` confined to the platform layer.

### Step 2: the packet builder

`ColorLight5A75Packet.h`, written from the documented layout:

- row data (per row, chunked at 497 px)
- sync/latch
- brightness

Pure functions over a caller-supplied buffer, host-testable and platform-independent: the same
shape as `ArtNetPacket.h`.

**Exit:** unit tests pinning each packet's bytes against the documented layout, including the
chunk boundary at 497 px and a row that needs two packets.

### Step 3: the driver

`PanelCardDriver.h`, sibling of `NetworkSendDriver`. Window from `DriverBase`, correction from the
shared pipeline, one row packet per row, then the sync.

At this point the feature is complete for the primary case: **effects rendering on the board light
a real panel.** Send-only, with a manual layout. Card discovery (which needs raw L2 *receive*, a
separate seam) and multi-receiver layout are follow-ups.

**Exit:** a scenario test driving a known pattern through the desktop stub; a real card showing the
correct image on the bench (**PO judgement, the gate that matters**).

### Step 4: ArtNet in over the same link (application)

With the driver working, a board on one network segment takes ArtNet or E1.31 in and forwards it to
the panels.

Before building it, **measure**: feed an S31 ArtNet at increasing universe counts and find where it
breaks. We have no measured ArtNet-receive numbers on any board. The adjacent measurement that
makes this worth doing first: a ~19 ms encode on core 0 **starved the network stack** on an LC16,
with the link dropping and HTTP timing out while the render loop kept ticking (`lessons.md`). A
bridge is network-in *and* network-out, so both halves want the same core.

That measurement decides two things: whether the input protocol wants to be DDP (480 px/packet)
rather than ArtNet (170 px/packet), and whether the dual-core split is required.

**Exit:** a table of universes/s vs tick budget on real S31 hardware, and a decision on input
protocol and core placement.

### Step 5: a separate input link, so the gigabit carries panels only

**WiFi is already on the board, and it is the cheap version of this.** ArtNet in over WiFi, panels
out over RGMII: two interfaces, no new transport code, available the day step 4 works.

```
  ArtNet/E1.31 in ──► [WiFi]  S31  [RGMII 1 Gbit] ──► raw L2 ──► cards
```

This suits a controller running its own effects with occasional live input, and the UI stays
reachable over WiFi while the wire is busy. The limit is WiFi itself: shared airtime and jitter,
which matter for a full-rate ArtNet feed at high universe counts. Step 4's measurement says whether
the intended pixel count fits.

**USB-Ethernet is the wired version, for when it does not.** The S31 has `SOC_USB_OTG_SUPPORTED`
with a UTMI PHY, so USB high-speed host is possible. The CDC-ECM/NCM host class driver comes from a
managed component or from writing one, which makes it the biggest unknown in the plan. **Spike
before committing**, and only once a measurement shows WiFi is the constraint.

Steps 1-3 stand alone, and step 4 works over a single shared link.

## Verification

1. Host tests: packet bytes, chunk boundaries, the desktop stub round-trip.
2. `check_platform_boundary` clean; `clang-hotpath` shows no new blocking call on the render path.
3. `check_footprint`: the driver holds **zero static RAM** when not enabled.
4. **A real ColorLight card renders a correct image from an effect running on the board, judged by
   the PO on the bench.** This is the gate.
5. For step 4: the throughput table exists and the input-protocol decision is recorded.

## Risks

| risk | mitigation |
|---|---|
| S31 RX path unresolved (`Plan-20260726`) | this driver is TX-only; step 1 spikes a raw frame first |
| Sustained gigabit TX stresses RGMII timing harder than DHCP does | capture the wire in step 1; corrupt pixels read as a timing fault |
| USB-Ethernet host driver comes from outside IDF | WiFi covers the input link first; USB only if measured short |
| Card firmware variance (brightness, repeated sync) | calibrate against a real card; keep those out of the first cut |
| ArtNet packet rate exceeds the S31 | step 4 measures it; DDP is the lever |

## Follow-ups

- **Card discovery**, once the raw L2 receive seam exists; a manual layout works until then.
- **Other vendor formats**, as the architecture allows.
