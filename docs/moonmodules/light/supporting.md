# Light supporting modules

The light-domain machinery the catalog modules (effects, modifiers, layouts, drivers) lean on — not directly user-facing. Every row links to its generated technical page (the full API, from the `.h`) and its tests. Cross-cutting rationale that no single `.h` owns lives in the prose sections below the table.

<a id="layer"></a>

### Layer

One rendering layer — an effect writes into its buffer, modifiers transform the coordinate mapping, and the layer composites onto the shared output. The unit the render loop iterates.

<img src="../../assets/light/Layer.png" width="300" alt="Layer container with a child effect">

- `blendMode` — how this layer composites onto the ones below (overwrite / alpha / additive).

Detail: [technical](moxygen/Layer.md)

[Tests](../../tests/unit-tests.md#layer)

<a id="layers"></a>

### Layers

The container of layers — composites them (blend mode + opacity per layer) into the final light buffer.

<img src="../../assets/light/Layers.png" width="300" alt="Layers container">

Detail: [technical](moxygen/Layers.md)

[Tests](../../tests/unit-tests.md#layers)

<a id="layouts"></a>

### Layouts

The container of layout modules — walks each layout's coordinates to build the physical light set the mapping consumes.

<img src="../../assets/light/Layouts.png" width="300" alt="Layouts container">

Detail: [technical](moxygen/Layouts.md)

[Tests](../../tests/unit-tests.md#layouts)

<a id="drivers"></a>

### Drivers

The container of driver modules — owns the shared driver buffer and the per-light output correction every driver applies before sending.

<img src="../../assets/light/Drivers.png" width="300" alt="Drivers container with the on/off + brightness controls">

- `on` — master power (default on). Turning it off scales the whole output to black while preserving `brightness`, so on restores the exact level. The single power control every consumer drives (the UI toggle, IR's on/off, the WLED app / Home Assistant, MQTT/Homebridge) through `Scheduler::setControl`.
- `brightness` — global output brightness (0–255).
- `lightPreset` — the output-correction preset (colour order, gamma, brightness) every driver applies.
- `palette` — the active palette effects sample from.

Detail: [technical](moxygen/Drivers.md)

[Tests](../../tests/unit-tests.md#drivers)

<a id="lightpresets"></a>

### LightPresets

The reusable light-preset library — a Drivers submodule that owns the named channel-role wirings drivers reference. A light preset is a channel layout (which channel carries Red, Green, Blue, White, WarmWhite, Yellow, UV, or a fixture role like Pan/Tilt); a curated set of real fixtures is seeded as read-only entries — the colour orders (RGB, GRB, BGR, RGBW, GRBW, WRGB), multi-channel LED/par fixtures (Curtain GRB6, Lightbar RGBWYP, RGBCCT, IRGB), and moving heads (MH BeeEyes 15, MH BeTopper 32, MH 19x15W-24) — and a user adds custom named wirings alongside them. A driver stores only a preset's stable id and resolves it here at rebuild time, so one wiring is reusable across every driver and reordering/deleting other presets never disturbs a reference. Built on the editable-list control primitive (add/delete/reorder/edit rows), which custom palettes reuse later.

- `presets` — the editable list of preset definitions. Each row: a name, a channel count, and one role picker per channel. Built-in rows are read-only; custom rows are fully editable and persist across reboot.

Detail: [technical](moxygen/LightPresetsModule.md)

### Buffer

Contiguous light-data buffer, shared between the layers that write it (effects) and the driver groups that read it. A raw `uint8_t*` so any channel layout fits — RGB, RGBW, multi-channel DMX.

Detail: [technical](moxygen/Buffer.md)

[Tests](../../tests/unit-tests.md#buffer)

### MappingLUT

Maps the virtual grid to the physical sparse light set — a radius-4 sphere becomes its 210 real lights, not the 729-cell box. The lookup effects and the preview both consume.

Detail: [technical](moxygen/MappingLUT.md)

[Tests](../../tests/unit-tests.md#mappinglut)

### Effect base

The `EffectBase` class every effect derives from — the shared surface (buffer access, dimensions, the palette) an effect renders against.

Detail: [technical](moxygen/EffectBase.md)

### Modifier base

The `ModifierBase` class every modifier derives from — transforms the coordinate mapping (mirror, rotate, multiply, …) a layer applies before rendering.

Detail: [technical](moxygen/ModifierBase.md)

### Driver base

The `DriverBase` class every driver derives from — the shared surface (the driver window, the source buffer, the output correction) a driver reads before sending its slice. It plays the same zero-state role for drivers that Effect base does for effects.

Detail: [technical](moxygen/DriverBase.md)

### Layout base

The `LayoutBase` class every layout derives from — the shared surface a layout implements to walk its coordinates into the physical light set the mapping consumes.

Detail: [technical](moxygen/LayoutBase.md)

### Parallel LED driver base

The `ParallelLedDriver` CRTP base shared by the two parallel WS2812 drivers (the S3's LCD_CAM and the P4's Parlio) — the one copy of the common body they were ~250 lines of byte-for-byte identical over: pin slicing, the fused correct+encode, the latch pad, and the single-shot autonomous-DMA transfer. Each derived driver supplies only its peripheral-specific pieces.

Detail: [technical](moxygen/ParallelLedDriver.md)
