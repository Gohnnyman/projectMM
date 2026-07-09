# Plan — Pins increment #4: live-state (level + drive-cap)

Pins top-down [increment #4 / §6](../../backlog/pins-analysis-top-down.md#6-live-state--the-second-axis-separate-later-testing-first): the second axis — *what is GPIO N doing right now*. Builds on #1/#2/#3 (ownership map + severity). PO-directed; scope = **level + drive-cap** (the see-the-wire MVP; ADC/continuity/activity-rate deferred).

## Context

#2/#3 answer *who owns each pin, and is that safe/conflicting* — all **static**. #4 adds the **live** half: is the pin actually toggling (level HIGH/LOW), and how hard is it driving (drive capability). The top-down §6 is emphatic this is **not polish** — it's a HAL-testing tool: "a driver's output pin must toggle when it renders; a mic clock must toggle when the mic runs." `gpio_get_level` reads the pad on *any* pin (even one a peripheral drives), so it's the literal see-the-wire check that separates firmware-idle from wire-fault.

## The UI sidestep check (PO rule)

Unlike #2 (which needed a new `severity`→colour *convention*), live-state is **scalar per-pin values** (`level`, `driveCap`) that refresh each sample. The generic list renderer already shows scalar row fields as " · "-joined text and re-renders on state push. So **level + drive-cap need NO app.js change** — they're just two more fields in the row object, rendered by the existing generic path. (The §6 *board-diagram* view would need a UI sidestep, but that's out of scope here.) Confirmed against the Explore seam-map: the renderer handles scalars natively; the sidestep is only triggered by per-row *styling*, which #2 already generically solved.

## Verified seams

- **GPIO read behind the boundary:** `gpio_get_level` / `gpio_get_drive_capability` — already used by `loopbackJumperOk` ([platform_esp32_rmt.cpp:260](../../../src/platform/esp32/platform_esp32_rmt.cpp#L260)). A new seam generalizes that access. `gpio_get_level` reads the pad (safe on a peripheral-driven pin); `gpio_get_drive_capability` reads the config.
- **Seam precedent:** `gpioCapability` (#2, `platform_esp32_gpio.cpp`) is the exact shape — a per-gpio struct query, desktop-stubbed. `gpioLiveState` is its live sibling, in the same file.
- **Consumption:** `PinsModule::collect` already resolves each claim's gpio; add a live read per claim in `refresh` (on `loop1s`, off the hot path — the existing cadence). New fields on `Claim`, emitted in `writeListRow`.
- **Test seam:** `setTestGpioCapability` (#2) is the model — add `setTestGpioLiveState` so the host can assert the level/drive-cap columns serialize.

## Design

### Part A — `platform::gpioLiveState` seam

```cpp
// platform.h — live electrical state of one GPIO (PinsModule's live-state axis). Domain-neutral.
struct GpioLiveState {
    bool valid = false;   // false when the pin isn't readable (out of range / desktop) → UI omits the columns
    bool level = false;   // current pad level: true = HIGH, false = LOW (gpio_get_level)
    uint8_t driveCap = 0; // output drive strength 0..3 (WEAK..STRONGEST), gpio_get_drive_capability
};
GpioLiveState gpioLiveState(uint8_t gpio);
```
ESP32 impl (in `platform_esp32_gpio.cpp`, beside `gpioCapability`): `gpio_get_level` + `gpio_get_drive_capability`; `valid=false` for an out-of-range pin. Desktop stub: `valid=false` (no real pins) + a `setTestGpioLiveState` override table (mirrors `setTestGpioCapability`). Drive-cap maps the IDF `gpio_drive_cap_t` (0..3) straight through.

### Part B — PinsModule per-claim live columns

In `refresh` (after collecting), read `gpioLiveState(gpio)` per claim; store `level` + `driveCap` when `valid`. Emit in `writeListRow` as `"level":"HIGH"|"LOW"` and `"drive":"WEAK"|"MEDIUM"|"STRONG"|"STRONGEST"` (short labels a bench operator reads), omitted when `!valid` (desktop / bad pin) so the columns simply don't appear there. Keep the label maps small static tables beside `roleFor`.

A subtlety worth a comment: multiple claims can share a GPIO (a conflict) — each reads the same live pad, so they'll show the same level/drive, which is correct (it's one physical pin).

### Part C — UI

**None expected.** The scalar `level`/`drive` fields render via the existing `listSummaryText` join. Verify on hardware that they read well in the row; if the summary gets too long, the fields can move to `writeListRowDetail` (still generic, no pins-specific code) — decide from the live look, not up front.

## Files

- **Edit:** `src/platform/platform.h` (`GpioLiveState` + `gpioLiveState` + test seam); `src/platform/esp32/platform_esp32_gpio.cpp` (impl); `src/platform/desktop/platform_desktop.cpp` (stub + test override); `src/core/PinsModule.h` (live columns in Claim + refresh + writeListRow); `docs/moonmodules/core/system.md` (Pins card: live columns); `docs/reference/gpio-usage.md` (cross-link if useful).
- **Tests:** `unit_PinsModule.cpp` — inject a live state (HIGH, drive STRONG) for a gpio, assert the row emits `"level":"HIGH"` + `"drive":"STRONG"`; a `!valid` pin omits them.

## Verification

1. Build clean; ctest; spec; boundary (the GPIO reads stay in `platform_esp32_gpio.cpp`); scenarios; ESP32 build; JS.
2. Live on S3/P4: a rendering LED driver's lane pin shows a **toggling** level across refreshes (or HIGH/LOW depending on sample phase); an idle/unclaimed pin reads its resting level; drive-cap shows the configured strength. The see-the-wire check: disable the driver → the pin stops being driven (pairs with the disable-releases work).

## Scope guard

Level + drive-cap only. **NO** ADC, **NO** continuity probe (reuse `loopbackJumperOk` later), **NO** activity/toggle-rate (needs edge sampling), **NO** board-diagram UI (the one thing that WOULD need an app.js sidestep). Keep `gpioLiveState` domain-neutral in the platform layer. Mark `(shipped)` when it lands.
