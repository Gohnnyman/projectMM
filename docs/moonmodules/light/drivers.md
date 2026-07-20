# Drivers

A driver sends lights somewhere. It reads its slice of the [Drivers](moxygen/Drivers.md) container's shared buffer, applies its own [output correction](moxygen/DriverBase.md), and outputs — over a wire (WS2812), the network (Art-Net / E1.31 / DDP), to a smart-light hub (Hue), or to the web UI (Preview).

Several drivers can share one buffer, each driving its own slice. Every driver starts with the same [shared controls](#shared-driver-controls), then adds its own. Drivers are added per board through the catalog ([`deviceModels.json`](../../../web-installer/deviceModels.json)); `PreviewDriver` is the one boot-wired driver.

**Jump to:** [shared controls](#shared-driver-controls) · [LED](#led-drivers) · [Network](#network-drivers) · [Smart light](#smart-light-drivers) · [Preview](#preview-drivers)

## Shared driver controls

### Shared 💫 · every driver

Added once by [`DriverBase`](moxygen/DriverBase.md) so no driver re-implements it: a per-driver **output correction** (how this driver's slice looks) and a **source window** (which slice of the shared buffer it reads). Every driver card leads with this block; its own controls follow.

<img src="../../assets/light/drivers/RmtLedDriver.png" width="300" alt="Shared driver controls: localBrightness, preset, whiteMode, start, count">

- `localBrightness` — this driver's dim (0–255), multiplied with the global brightness into one LUT; both sliders reach the output.
- `preset` — the [light preset](supporting.md) this driver applies per light (channel order / RGBW synthesis), referenced by its stable id (not its name), so renaming or reordering presets never breaks a driver's reference and it survives a reboot.
- `whiteMode` — how the white channel is derived for an RGBW strip, applied only when the referenced preset carries a W channel.
- `start` — first light of the shared buffer this driver reads (default `0`).
- `count` — how many lights from `start` this driver drives. **Blank / default drives all lights**; set a number to output only that slice — the way multiple drivers each own a section of one buffer (an onboard status LED at `0`, the main strip from `1`).

Detail: [technical](moxygen/DriverBase.md)

## LED drivers

<a id="rmtled"></a>
<a id="multipinled"></a>
<a id="moonled"></a>
<a id="parlioled"></a>

### LED driver 💫 · wire

Addressable WS2812B-class LEDs over a wire. Four drivers, same controls and same wire contract; they differ in how many strands clock out at once and on which chip. **Start with RMT** for a few strands, **MultiPin** for many (up to 16), **Parlio** on a P4 — and **Moon** when you need more lights than one DMA buffer holds, or more strands than you have GPIOs (its 74HCT595 pin expander turns 6 pins into 48 strands). Which to pick, and why: [details](#led-driver-details).

<img src="../../assets/light/drivers/RmtLedDriver.png" width="300" alt="LED output driver controls">

Plus the [shared controls](#shared-driver-controls) above:

- `pins` — data GPIO list, e.g. `18,17,16`. One strand each — or, with Moon's pin expander, one *group of 8*. Empty idles until set; changing it re-inits live.
- `ledsPerPin` — lights per **strand**, following the broadcasting idiom (cf. NumPy / CSS shorthand): **empty** = even split of the window; **one number** = that many on *every* strand (`64` → 64 each); **a list** `3,4,5` = one per strand by position (a short list even-splits the remainder). Shorter strands go dark early while the longest finishes. Through an expander an entry is one strand, not one pin, so two strands on one '595 can differ.
- **Expert-only** (🔧, shown when `System.expertMode` is on): `loopbackTest` — a TX→RX loopback self-test (jumper the first pin to `loopbackRxPin`), verdict in the status field, with `loopbackTxPin`/`loopbackRxPin` its wiring. Moon adds `shiftOverclock` and the manual `ring*` geometry knobs (below).

Origin: WS2812B on FastLED / hpwit / WLED prior art ([analysis](../../history/leddriver-analysis-top-down.md))

Tests: [RMT](../../tests/unit-tests.md#rmtleddriver) · [MultiPin](../../tests/unit-tests.md#multipinleddriver) · [Moon](../../tests/unit-tests.md#moonleddriver) · [Parlio](../../tests/unit-tests.md#parlioleddriver) · [shared](../../tests/unit-tests.md#parallelleddriver)

Detail: [RMT](moxygen/RmtLedDriver.md) · [MultiPin](moxygen/MultiPinLedDriver.md) · [Moon](moxygen/MoonLedDriver.md) · [Parlio](moxygen/ParlioLedDriver.md)

## Network drivers

<a id="networksend"></a>

### Network Send 💫 · UDP

<img src="../../assets/light/drivers/NetworkSendDriver.png" width="300" alt="NetworkSend controls">

Streams the buffer over UDP as **Art-Net**, **E1.31 / sACN**, or **DDP** — one burst per frame, compatible with Falcon/Advatek controllers, xLights, and LedFx. Feeds **one or more receivers** from a single driver: each gets its own slice of the window, unicast to its own address.

- `protocol` — Art-Net / E1.31 / DDP (default Art-Net); the destination port follows automatically.
- `ips` — the receivers. **Blank by default — the driver idles until set**, so it never sends uninvited traffic. Type the full address once, then a range or a list: `192.168.1.70-74` (five tubes, ends inclusive) or `192.168.1.60,61,62,65`; both mix, and a further full address switches subnet.
- `lightsPerIp` — lights per receiver, same idiom as an LED driver's `ledsPerPin`: **blank** = split the window evenly; **one number** = that many each; **a list** `150,100,50` = one per receiver by position.
- `universe_start` — first universe for Art-Net / E1.31 (DDP ignores it). Restarts per receiver — each is an independent node addressing its own strip.
- `fps` — frame-rate limit (default 50, 1–120).

Unicast is the default because Art-Net 4 requires it and because broadcast makes *every* host on the LAN parse *every* packet; a broadcast address still works if you type one. The full addressing rationale (and the one case where broadcast is the better tool) is on the [detail page](moxygen/NetworkSendDriver.md).

Origin: MoonLight D_NetworkOut; Art-Net 4 / E1.31 / DDP specs

[Tests](../../tests/unit-tests.md#networksenddriver)

Detail: [technical](moxygen/NetworkSendDriver.md)

## Smart light drivers

<a id="hue"></a>

### Hue 💫 · bridge

<img src="../../assets/light/drivers/HueDriver.png" width="300" alt="A HueDriver in the UI">

Drives **Philips Hue bulbs as pixels**: each color bulb in the driver's window becomes one pixel, pushed to the bridge over its HTTP API. Paced to the bridge's ~10 cmd/s limit — smooth ambient color, not strobing.

- `bridgeIp` — the bridge's LAN IPv4.
- `appKey` — the Hue app key; filled by `pair`, persisted.
- `pair` — button: press it, then the bridge's physical link button within ~30 s to claim a key.
- `room` / `light` — dropdowns narrowing which color lights are driven (both default `All`).

Origin: projectMM, on the [Hue v1 CLIP API](https://developers.meethue.com/develop/hue-api/)

[Tests](../../tests/unit-tests.md#huedriver)

Detail: [technical](moxygen/HueDriver.md)

## Preview drivers

<a id="preview"></a>

### Preview 💫 · web UI

<img src="../../assets/light/drivers/PreviewDriver.png" width="300" alt="PreviewDriver controls">

Streams a true-shape 3D preview to the web UI over WebSocket as a **point list** — only the real lights at their real positions, so a sphere/ring/arbitrary map shows in its true shape. The one boot-wired driver.

- `fps` — preview stream rate (default 24, 1–60; independent of the render loop).

Origin: projectMM, on [MoonLight](https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/PhysicalLayer.h)'s PhysicalLayer model

[Tests](../../tests/unit-tests.md#previewdriver)

Detail: [technical](moxygen/PreviewDriver.md)

## LED driver — details

**Which driver?**

| Want | Use | Why |
|---|---|---|
| A few strands, any ESP32 | **RMT** | The default. Simple, no bus width to think about, one channel per strand. |
| Many strands (up to 16) | **MultiPin** | The scale path where RMT runs out of channels. One DMA transfer drives every strand at once. |
| Up to 16 strands on a **P4** | **Parlio** | The P4's own parallel peripheral — it generates its pixel clock, so there is no clock pin to spend. |
| **More lights than fit one DMA buffer**, or **more strands than you have GPIOs** | **Moon** | The same LCD_CAM output as MultiPin, on our own DMA: it *streams* the frame, and it drives a 74HCT595 **pin expander** — 6 pins → 48 strands. |

**Moon and MultiPin drive the same pins the same way; only the DMA underneath differs.** Start with MultiPin — it is the proven path. Choose Moon when you hit one of its two limits. Both are registered module types, so you can swap them in the UI on one board with no reflash.

**The four, compared.** All drive WS2812B-class strips with the same `pins` / `ledsPerPin` / `loopback*` controls and the same wire contract; they differ in parallelism, chip, and — for the two i80-bus entries (**MultiPin** and **Moon**) — in who programs the DMA.

**Lane, pin, strand.** A **lane** is one bus data line; a **strand** is one chain of LEDs. The i80 **bus** is 8 or 16 lanes wide (a hardware fact — `lcd_ll_set_data_wire_width` takes nothing else), but you configure only the **pins** that drive something, at any count from 1: the driver rounds the bus up around them and parks the spare lanes on a pin the peripheral already drives, where nothing reads them.

- **Direct:** one pin = one lane = one strand. 1–16 strands.
- **Through an expander:** each data pin feeds one '595 and fans out to 8 strands, so **1–8 data pins → up to 64 strands** (the driver's ceiling). The **latch** also costs a lane — the peripheral has only one clock output, so it has to ride a data line — but the strand ceiling binds first. hpwit's board populates 6 pins → **48 strands**.

| Driver | Chip | Strands | Extra controls | Notes |
|---|---|---|---|---|
| **RMT** ([detail](moxygen/RmtLedDriver.md)) | any ESP32 (classic 8 ch, S3 4, P4 4 DMA) | one per RMT TX channel | `loopbackFrame` | The general single-/few-strand output; default for classic + S3 board entries. `loopbackFrame` bit-verifies a *whole frame*, catching frame-rate / RF corruption a 24-bit burst misses. |
| **MultiPin** ([detail](moxygen/MultiPinLedDriver.md)) | S3 / P4 (LCD_CAM) · classic (I2S) | **1–16** | `clockPin` `dcPin` | One driver over IDF's `esp_lcd` i80 bus. The **bus** is 8 or 16 bits wide (≤8 pins → 8-bit, 9–16 → 16-bit) — but the **pin count is free**: configure only the pins that drive something and the driver rounds the bus up around them, parking the spare lanes on a pin the peripheral already drives. `clockPin`/`dcPin` are i80 bus lines the LEDs ignore. **Capped by one contiguous DMA buffer**: the classic backend is internal-RAM only (I2S can't reach PSRAM) → **2048 lights**; LCD_CAM draws from PSRAM → **16384**. Over the cap it idles with a status rather than crashing. |
| **Moon** ([detail](moxygen/MoonLedDriver.md)) | S3 / P4 (LCD_CAM only) | **1–16**; ×8 per pin with an expander (**6 pins → 48 strands**) | `clockPin` `pinExpander` `latchPin` `useRing` `ringAuto`; 🔧 `shiftOverclock` `ringRows` `ringBufs` `ringPadUs` | The same LCD_CAM output as MultiPin on **our own GDMA chain**, which buys two things `esp_lcd` cannot: a frame **streamed** through a small buffer pool instead of held whole (so length stops being a memory question), and a **74HCT595 pin expander** — one GPIO fans out to 8 strands. `ringAuto` (default on) derives the streaming geometry per config, so the manual `ring*` knobs and `shiftOverclock` (a faster '595 clock for short-wired rigs) are expert-only tuning — the full guide is on the technical page. No `dcPin` at all, and WR is routed only when a '595 needs it as its shift clock. Not on the classic ESP32 (its i80 is the I2S peripheral). The prime-only ring (frame fits the buffer pool) and the pin expander are wall-solid; the **lapping** ring (very long strands, where the ISR refills from a PSRAM source) has a known last-row sparkle on the largest configs, tracked in [the backlog](../../backlog/backlog-light.md). Why + what it costs: [ADR-0014](../../adr/0014-own-i80-dma-driver-below-esp-lcd.md). |
| **Parlio** ([detail](moxygen/ParlioLedDriver.md)) | ESP32-P4 | **1–16** | — | The P4's parallel path; Parlio generates its own pixel clock, so no clock/dc pins to spend. Bus width follows the pin count. On P4-NANO a known-good 8-set is `20,21,22,23,24,25,26,27`. |

The detail pages carry each driver's wire contract, buffer slicing, memory sizing, and the loopback self-test.

**What the parallel drivers share.** MultiPin, Moon and Parlio are thin peripheral shells over two common pieces — worth reading if you care how a frame is actually built:

- **[Parallel LED driver base](moxygen/ParallelLedDriver.md)** — the shared body: strand slicing, the encode loop, the async double-buffer, the latch pad, the loopback self-test. A derived driver adds only its peripheral's DMA calls.
- **[Slot encoder](moxygen/ParallelSlots.md)** — the wire format itself. Each WS2812 bit becomes three bus slots (pulse start / data / tail), and the data slot is an **8×8 bit transpose**: lanes in, bit-planes out, so one bus word carries the same bit of every strand. It is the render loop's measured hot spot.
