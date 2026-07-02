# Preview Driver

Overview, controls, prior art, source, and tests: [drivers.md § Preview](drivers.md#preview). This page carries *only* what no single source file can: the wire contract and the cross-module data flow. (Implementation mechanics live in `PreviewDriver.h`.)

## Protocol

PreviewDriver owns both wire formats and **streams** the bytes to a `BinaryBroadcaster` — the core [HttpServerModule](../../core/moxygen/HttpServerModule.md) — via `beginBinaryFrame`/`pushBinaryFrame`/`endBinaryFrame`, never building a copy of a frame. The HTTP server only writes the bytes to its WebSocket clients: no knowledge of the preview, the light domain, or the formats. `main.cpp` wires the driver's broadcaster to the HTTP server instance.

The wire layout is the device source itself — edit the header, this follows:

```cpp
--8<-- "src/light/drivers/PreviewDriver.h:wire-format"
```

- **`0x03` coordinate table** — sent on geometry change (LUT rebuild), new-client connect, or a downscale-factor change; not per-frame. `stride` carries the downscale factor (1 = full res).
- **`0x02` per-frame channels** — RGB by coordinate-table order. The browser **skips a `0x02` whose `count` ≠ the current `0x03` count** (a rebuild is mid-flight); the device likewise withholds colour frames until the matching `0x03` is accepted, so the two never desync.

## Cross-module data flow

PreviewDriver reads the **sparse driver buffer** — the one the LED/ArtNet drivers also consume — which the `Layer`'s [MappingLUT](../moxygen/MappingLUT.md) fills with exactly the real lights (a radius-4 sphere → 210 entries, not its 9×9×9 = 729 box). Both messages stream straight from that buffer and the layout's coordinate iterator; neither holds a preview-side copy. The coordinate table is built in the **same driver order** as the buffer, so RGB index `i` and coordinate `i` always refer to the same light. See [Layer](../moxygen/Layer.md) / [MappingLUT](../moxygen/MappingLUT.md) for the box→driver mapping.

**Buffer-lifecycle coupling (the non-obvious one).** A large frame rides HttpServerModule's resumable `sendBufferedFrame`, drained a chunk per tick from the *stable* driver buffer — so a 196² frame (~115 KB) never stalls the loop, and the frame rate self-limits to what the link sustains. But a geometry rebuild frees + reallocs that buffer, so `onBuildState()` must call `cancelBufferedSend()` first, or a half-sent frame reads freed memory — a use-after-free guard pinned by a test. This coupling spans PreviewDriver ↔ HttpServerModule ↔ the Layer buffer, and is why the driver streams without its own frame copy.

## Large layouts — design rationale

The preview never freezes or tears at any grid size: it always delivers a **complete** frame, shedding in order — frame rate first, then spatial resolution.

- **Point cap = min(display, memory).** A browser canvas is a few hundred px wide, so beyond ~4096 points the lights are sub-pixel and more points only cost link bandwidth (a 16K-point full-res frame streams at <1 fps even on Ethernet) — the bottleneck at large grids is *throughput, not memory*. A second cap from `maxAllocBlock()` only bites on a board too tight to stream even the display cap. Above the cap the driver keeps lights on a **spatial lattice** (position ≡ 0 mod `s`), sampling *positions* not indices so there's no diagonal moiré in 2D/3D.
- **Adaptive downscale** — the deeper fallback after frame rate. The struggle signal is **latency** (a frame still draining after several `fps` slots), catching the slow-but-complete case a pure all-sent signal misses. Sustained struggle coarsens the lattice (`stride`++); sustained prompt sends refine back, with hysteresis to stop oscillation.

Positions are 1 byte/axis; a box exceeding 255 on any axis is scaled (largest edge → 255, aspect preserved), so large grids preview at true proportions.
