# Plan — Rename the LED driver surface for a human-readable UI (+ MIGRATING.md)

## Context

**The problem:** the UI reads like a datasheet. `src/ui/app.js:1176` does `label.textContent = ctrl.name` — the **raw control name IS the visible UI label**, with no prettifier and no separate label field. So the control identifiers are the UX, and today they expose peripheral jargon (`I80LedDriver`, `wireUs`, `shiftRegister`) or are actively misleading (`stall` implies a fault; it is really spare capacity).

**The outcome:** user-facing names say *what the thing is/does*; hardware names stay where hardware names belong (the platform layer). Consistent across code, UI, docs, catalog, and tests — no half-renamed surface.

**Why now:** we are on `next-iteration` with commits to spare, so a clean 100% sweep is cheap. Per ADR-0013 this is a **clean break, documented, not migrated**.

## Decisions (settled with the PO)

| Now | New | Why |
|---|---|---|
| `I80LedDriver` | `MultiPinLedDriver` | "i80" is IDF's bus name; users pick a *multi-pin* driver. (Avoids colliding with the `ParallelLedDriver` base class.) |
| `MoonI80LedDriver` | `MoonLedDriver` | "Moon" already signals *our own DMA* vs IDF's — keeps the deliberate A/B distinction. |
| `RmtLedDriver`, `ParlioLedDriver` | *(keep)* | Named after peripherals users actually see in chip docs. |
| control `shiftRegister` | `pinExpander` | Says what it does (1 pin → 8 strands), not the chip part-family. |
| control `asyncTransmit` | `doubleBuffer` | The textbook name for the mechanism. Test file already named `..._doublebuffer.cpp`. |
| read-only `wireUs` | `frameTime` | Unambiguous ("time to clock one frame"); `refresh` was rejected — it collides with ~30 existing `refresh*` identifiers and reads as "re-fetch". |
| read-only `stall` (Drivers) | `renderWait` | It is the render core's worst wait on the output core — always ≥ 0, one-way, and a LARGE value means **recoverable headroom, not a fault**. |
| status `"output stalled — the bus is not delivering frames"` | `"No LED output — the driver isn't sending frames (check pins / LED count)"` | Says what was lost + what to check. |

**Scope calls:**
- **Platform layer KEEPS `i80`** — `i80Ws2812*`, `MoonI80State`, `platform_esp32_i80.cpp` name IDF's real `esp_lcd_new_i80_bus` / `SOC_LCD_I80_SUPPORTED`. The platform boundary is exactly where hardware names belong; renaming would *hide* which IDF API is wrapped. Only comment references to the renamed driver classes change. (Revisit after the sweep if it still grates.)
- **Shift internals DO rename** (PO call): `shiftMode()`, `kShiftOutputs`, `encodeWs2812Shift*`, `prefillShiftRows`, the `unit_ParallelLedDriver_shiftregister.cpp` filename → pinExpander-consistent naming.
- **Keep** `ringSnapshot` / `forceRing` / `ringDbg` — still needed while the ring work is open.
- **Do NOT touch**: `docs/history/**` and `docs/backlog/*-analysis.md` (dated records/verbatim PO quotes — rewriting them falsifies the record); `docs/adr/**` (immutable; ADR-0014's filename encodes "i80" legitimately); `docs/moonmodules/**/moxygen/*` and `docs/tests/*.md` (gitignored, regenerated from the `.h`/test filenames).

## The four traps (why this is not a find-replace)

1. **Order matters**: `MoonI80LedDriver` *contains* `I80LedDriver`. Replace `MoonI80LedDriver` → `MoonLedDriver` **first**, then `I80LedDriver` → `MultiPinLedDriver`. Reverse order yields `MoonMultiPinLedDriver`.
2. **`stall` is the deadliest token**: the English word appears ~25× in unrelated prose, and **`install` contains `stall`** — a substring sweep wrecks `src/ui/install-picker.js`, `web-installer/`, `test/js/installer-*.test.mjs`, `test/python/test_installer_manifests.py`. Only ~12 sites are real: `Drivers.h:195,210,231,237-240,373,378-379,484-486,507,550,555,559`, `main.cpp:554,560`, `unit_Drivers_rendersplit.cpp:243`.
3. **`I80` must never be blind-replaced**: `CONFIG_SOC_LCD_I80_SUPPORTED`, `CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED`, and every platform symbol must survive. Replace whole-word `I80LedDriver` / `MoonI80LedDriver` only.
4. **`check_devices.py` fails SILENTLY**: `moondeck/check/check_devices.py:204,206,212-213` reads `controls.get("shiftRegister")`. Rename the control without updating it and all four 74HCT595 wiring invariants (latchPin presence, 1..15 data pins, latch/clock/dc collisions) **quietly stop validating** — no error, exactly the "dark LEDs on a bench" case it exists to prevent.

## Implementation

Method: rename **members** and let the **compiler** find the code (compiler-enforced); the **string literals** need eyes (a missed `strcmp` silently stops a bus rebuild — behavioural, no compile error).

1. **Save this plan** to `docs/history/plans/Plan-20260716 - LED driver rename for a human-readable UI.md`.
2. **Classes + files** (order per trap 1): rename `src/light/drivers/MoonI80LedDriver.h` → `MoonLedDriver.h`, `I80LedDriver.h` → `MultiPinLedDriver.h`, and the two unit tests; update the 4 `#include` sites (`src/main.cpp:93,96` + the two tests) and `test/CMakeLists.txt:109,110,114`. There is **no umbrella header enumerating drivers** — `src/light/drivers/Driver.h` is what drivers *include*, not a list; `src/main.cpp` is the enumeration point.
3. **Registration + docPaths**: `src/main.cpp:218,224` — class, type string, and docPath (`#i80led` → `#multipinled`, `#mooni80led` → `#moonled`) in lockstep with step 5. Comments at `:210,215,222-223`.
4. **Controls**: in `ParallelLedDriver.h` rename members `shiftRegister`→`pinExpander` (`:216`), `asyncTransmit`→`doubleBuffer` (`:131`) and their registrations (`:238,248,251`); private buffers `wireStr_`/`stallStr_` are free to rename. **Then hand-check every string site**: `affectsPrepare` (`:285,286`), the loopback gate (`:302`), `MoonI80LedDriver.h:127` `busControlTriggersBuild`, and `Drivers.h:195` + `main.cpp:560`'s `"  stall: %uus"` printf. Rename the shift internals (`shiftMode()`, `kShiftOutputs`, `encodeWs2812Shift*`, `prefillShift*`) and `unit_ParallelLedDriver_shiftregister.cpp`.
5. **Docs (present-tense only)**: `docs/moonmodules/light/drivers.md` — the anchors are **explicit `<a id>` tags at `:22,23`** (not heading-derived; all four drivers share the one `### LED output 💫 · wire` heading at `:26`), plus prose/links at `:28,30,45,112,117,118` (two `moxygen/*.md` link targets follow the renamed `.h` stems). `docs/performance.md` (~11 lines incl. an inline `drivers.md#i80led` link at `:247`), `docs/coding-standards.md:17`, `docs/reference/gpio-usage.md:44`.
6. **Catalog + checker (trap 4)**: `web-installer/deviceModels.json` — `"type"` at `:870,928,1253,1295` (boards: LightCrafter 16, SE 16 V1, hpwit shift-register, hpwit shift-register 15), `shiftRegister` at `:1258,1300`, `asyncTransmit` at `:1263,1305`. **And** `moondeck/check/check_devices.py:204,206,212-213`. (No `MoonI80LedDriver` entries exist.)
7. **Tests/scenarios**: `test/scenarios/light/scenario_perf_full.json:12,839,842` (type strings — load-bearing) and the control uses in `unit_MoonI80LedDriver.cpp:90,104,117`, `unit_ParallelLedDriver_shiftregister.cpp:96,281,288`, `unit_ParallelLedDriver_ring.cpp:311`, `unit_ParallelLedDriver_doublebuffer.cpp` (~12 sites).
8. **`docs/MIGRATING.md`** (new — industry standard, cf. Rails/Django/Webpack/Ember; beats "migrations.md" which reads as *database* migrations): a reverse-chronological log, newest first, each entry = **what changed + action required** (erase flash / re-add module / re-set control / nothing—self-heals). **Move** ADR-0013's "Known breaking changes" list (`docs/adr/0013-*.md:23-26`) into it verbatim — that list is an append-only log living inside an *immutable* ADR, which is a real tension. ADR-0013 keeps its decision + rationale and **links** to the log. Append this rename's entry: driver `type` + control keys changed → a board with a persisted old-name config **loses its driver / the renamed control values** on next boot; action: re-add the driver module and re-set `pinExpander`/`doubleBuffer`. Link `MIGRATING.md` from `README.md` + `docs/index.md`.

## Verification

- **`check_specs.py`** — the anchor guard (`:334-374`) proves every `registerType` docPath resolves to a real `#anchor`; `check_source_links` (`:238-326`) proves the `moxygen/*.md` links match the renamed `.h` stems. **Known gap to state, not fix here**: `ParallelLedDriver.h` is a CRTP template skipped at `:70-73`, so `pinExpander`/`doubleBuffer`/`frameTime` are *invisible* to the spec check (drivers.md documents none of the three today) — only `Drivers.h`'s `renderWait` is checked, against `light/supporting.md#drivers`.
- **`check_devices.py`** — must pass with the renamed types AND still fire its 74HCT595 rules (trap 4). Sanity-check by temporarily breaking a `pinExpander` board entry and confirming it errors.
- **`ctest`** (822 cases) + **scenarios** + **desktop build** (`-Werror`, zero warnings) + **platform boundary**.
- **ESP32**: build all 3 variants (classic / S3 / P4).
- **Hardware (the real gate)**: flash **shiffy** (S3, pinExpander path) and **SE16** (S3, direct 16-lane) — confirm both still drive, and that the UI now reads `pinExpander` / `doubleBuffer` / `frameTime` / `renderWait`. **PO's eyes are the measurement — stop and hand over; do not self-certify.**
- **Grep audit**: zero remaining whole-word `I80LedDriver` / `MoonI80LedDriver` / `shiftRegister` / `asyncTransmit` / `wireUs` outside `docs/history/**`, `docs/backlog/*-analysis.md`, `docs/adr/**`, and the platform layer's IDF-facing symbols.
