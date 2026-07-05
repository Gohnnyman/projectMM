# RMT LED Driver

Overview, controls, prior art, source, and tests: [drivers.md § LED output](drivers.md#rmtled). This page carries *only* what no single source file can: the WS2812B wire contract, buffer slicing, the on-device loopback self-test, and the LED-flicker troubleshooting playbook.

Output driver for WS2812B-class addressable LEDs over the ESP32 **[RMT (Remote Control Transceiver)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/rmt.html)** peripheral — one GPIO and one RMT TX channel per strand. Reads the [Drivers](../moxygen/Drivers.md) buffer, applies the shared [output correction](../archive/Drivers.md#output-correction) per light, and emits the WS2812 1-wire signal.

## Wire contract — [WS2812B](https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf)

1-wire NRZ at 800 kHz, no clock line. Each data bit is a 1.25 µs cell that starts HIGH then drops LOW; the HIGH duration encodes the bit:

| | HIGH | period | meaning |
|---|---|---|---|
| `0` bit | 350 ns | 1250 ns | short high, long low |
| `1` bit | 700 ns | 1250 ns | long high, short low |

Bits are sent **MSB-first** within each byte; channel order (GRB, GRBW, …) is the light preset applied by `Correction` before the encode, so the encoder itself is order-agnostic. Frames are latched by ≥ 300 µs idle-LOW (current WS2812B/SK6812 silicon — the old 50 µs value is dead). These timings live in `LedDriverConfig` and are converted to RMT ticks from the peripheral's granted resolution (≈ 40 MHz / 25 ns per tick), so they are not hard-coded to one clock.

## Buffer slicing across pins

The source buffer is split into **consecutive slices**, one per pin, in list order: pin 1 takes lights `[0, n₁)`, pin 2 takes `[n₁, n₁+n₂)`, and so on. Slice sizes come from `ledsPerPin`; pins without an explicit count split the unassigned remainder evenly (the last pin takes the rounding remainder). Counts are clamped so the sum never exceeds the buffer; lights beyond the last slice are not emitted. With `ledsPerPin` empty the whole buffer splits evenly over all pins — the zero-config case.

Each pin is also capped at a **WS2812 per-pin ceiling** (2048 lights). A single 1-wire WS2812 line clocks a fixed ~30 µs/light, so 2048 lights is already ~61 ms/frame (~16 FPS) — the floor before output turns to a slideshow. A pin over the ceiling is **clamped to it** (the driver still drives the first 2048, output stays lit — it does not idle) and a Warning status is raised ("some LEDs not driven…"), cleared once the count drops back under. This guards the common misconfig — a whole grid funneled onto one pin — which otherwise pins the tick at ~490 ms (FPS 1). The intended way to drive fewer lights is the start/count window, not this safety cap. The cap is a per-protocol value: a clocked 2-wire type (APA102/SK9822 over SPI) has no such fixed-rate limit and would pass a far higher one. (Clamping accumulates offsets from the clamped counts, so on a multi-pin over-ceiling config the later strips show a shifted window, not just a truncated one — accepted as a degraded, warned state for a config no one wires on purpose; the one-pin headline case is exact.)

## Concurrent show (blocks the render tick for the longest strand)

`loop()` encodes the whole frame once, then starts every pin's transmission (`platform::rmtWs2812Transmit`) before waiting on each (`platform::rmtWs2812Wait`) — the RMT channels clock out concurrently, so the render tick is charged roughly the **longest** strand, not the sum (~3 ms per 100 pixels on the longest slice), plus one shared reset gap. The transmission runs synchronously on the render task, so a large strand or a WiFi-interrupt-sensitive install can show timing artifacts.

## Cross-domain wiring

The driver is added as a child of the `Drivers` container at runtime via the catalog (`POST /api/modules`, a board's [`deviceModels.json`](../../../../web-installer/deviceModels.json) `modules` entry) — not boot-wired, exactly like [NetworkSendDriver](NetworkSendDriver.md). RMT is the default LED driver for classic ESP32 and S3 board entries. The type is registered on every target; on a chip without RMT TX channels it is inert. Once added, it receives `setSourceBuffer` / `setCorrection` / `setLayer` from `Drivers::passBufferToDrivers` (which wires every child, boot- or runtime-added), and applies the same `const Correction*` ArtNet uses. The **symbol encode** (`encodeWs2812Symbols` in `RmtSymbol.h`) is domain code in `src/light/` so it is host-testable; the **peripheral** (`platform::rmtWs2812*` in `src/platform/esp32/platform_esp32_rmt.cpp`) is the only ESP-IDF-touching part. Per-chip channel and memory limits come from the IDF SOC capability macros, so the same code serves classic, S3 and P4.

The peripheral half uses the [**modern RMT driver**](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/rmt.html) (ESP-IDF 5.x+ "RMT v2": `driver/rmt_tx.h` / `rmt_rx.h` / `rmt_encoder.h` — `rmt_new_tx_channel()`, a copy encoder, `rmt_transmit()`), **not** the legacy channel-numbered API (`driver/rmt.h`, `rmt_config_t`, `RMT_CHANNEL_n`, `rmt_write_items()`). This isn't a preference — the legacy driver was **removed entirely in [ESP-IDF v6](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32/migration-guides/release-6.x/6.0/peripherals.html)** (the build IDF), so the modern API is the only one that exists. One payoff is portability: the same v2 code serves every RMT-bearing target with no per-chip branching, including the [**P4**](https://www.espressif.com/en/products/socs/esp32-p4), whose RMT additionally has a DMA backend (`SOC_RMT_SUPPORT_DMA`, used by the whole-frame loopback capture — the classic ESP32 has no RMT DMA).

## Loopback self-test (on device)

The RMT peripheral is a transceiver, so the driver can verify its own output on real silicon — no separate test firmware. Jumper the **first** pin in `pins` (TX) to `loopbackRxPin`, then tick the `loopbackTest` control: the driver transmits a known WS2812 pattern out the data pin, captures it back on the RX pin, decodes, and compares. To test another output, temporarily move it to the front of the list. The outcome goes to the module's **status field** (`setStatus`): `loopback PASS`, `loopback FAIL: sent … got …`, or `loopback: jumper not detected` (a plain-GPIO continuity pre-check runs first, so a wiring fault is reported as such, not mistaken for a code bug). The test releases **all** TX channels first (so the RX capture can always allocate RMT memory, even with every channel in use) and briefly drives the test pattern, so any strips flicker once during the run; normal output resumes after. All hardware lives in `platform::rmtWs2812Loopback`.

The default test sends a 24-bit pattern — enough to prove the GPIO emits correct bytes, but blind to faults that only appear over a sustained transfer (frame-rate DMA corruption, RF interference on a long data line — the *intermittent flicker* class of bug). Tick `loopbackFrame` to switch to the whole-frame variant: it transmits a real frame the size of the first pin's slice, back to back like the render loop, captures the **entire** frame, and bit-verifies every WS2812 bit. A single flipped bit anywhere fails the test and the status reports its position (`loopback FAIL: bad bit N/M (light K)`); a clean run reports the bit count (`loopback PASS (M bits)`). Run it while WiFi is active to reproduce interference that only manifests under radio load. Hardware lives in `platform::rmtWs2812LoopbackFrame`. (On the classic ESP32, which has no RMT DMA, the whole-frame capture is capped to one RMT channel's worth of symbols — ~2 RGB lights — and the frame is still clocked back to back; the S3/P4 capture the full frame via DMA.)

## Troubleshooting: flicker on LEDs that should be off

Random wrong colours on LEDs that the effect leaves black — most often a few stray pixels flickering — is, on a 3.3 V ESP32 driving WS2812 **directly**, almost always a **data-line signal-integrity** problem, not a firmware bug. WS2812 wants a logic-high near 0.7 × VDD (≈ 3.5 V on a 5 V strip), but the ESP32 only drives 3.3 V, so individual bits sit at the margin and noise tips them. Confirm the firmware is innocent before reaching for the soldering iron — these checks were the actual diagnosis path on the bench (recorded in [decisions.md](../../../history/decisions.md)):

1. **Is the data clean?** The preview/source buffer is the logical RGB the effect produced — if it shows no stray colour, the effect is innocent (the corruption is downstream of the buffer).
2. **Is the firmware/peripheral clean?** Run the `loopbackFrame` self-test through a short jumper on the data pin. A `PASS` means the RMT encode + transmit emit bit-perfect WS2812 — the GPIO is fine.
3. **Is it WiFi RF?** Lower `Network.txPowerSetting` from 20 dBm down toward 2 and watch. If the flicker shrinks with TX power, it's radio coupling into the data wire (mitigate with the cap below). If it's **unchanged across the whole sweep, it is not the radio** — it's the physical data path.

When 1–3 all come back clean, the fix is electrical, in rough order of effectiveness:

- **Add a 3.3 → 5 V level shifter** on the data line (e.g. 74HCT125 / 74AHCT125) — the single most effective fix; it restores the logic-high margin the LEDs expect.
- **Add a ~330 Ω series resistor** at the GPIO, close to the board, to damp reflections.
- **Shorten / shield the data wire**, and keep it away from the power leads and the antenna.
- **Share a solid, thick common ground** between the strip's supply and the board.
- If RF coupling was implicated by step 3, set a per-board `Network.txPowerSetting` cap (the same `deviceModels.json` mechanism the ESP32-S3 N16R8 Dev uses).
