# Migrating

The log of **breaking changes** — what changed between versions, and the action to take.

projectMM ships **no migration code**: the persistence layer is robust by default (an absent key keeps the control's default, a stale value clamps to the new bounds, an unknown key is ignored), which absorbs almost all schema drift with zero migration-specific code. The rare change that a robust reader *cannot* absorb is **documented here instead of migrated** — see [ADR-0013](adr/0013-no-migration-code-robust-persistence-plus-documented-breaks.md) for the decision and its rationale.

**Read this when upgrading a device that already holds persisted state.** Entries are newest first. Each says what changed and what to do; most need nothing at all, because the lost value re-populates on next use.

**Action legend** — how much work an entry costs you:

| Action | Meaning |
|---|---|
| *nothing* | Self-heals. The value re-populates on next use, or the default is correct. |
| *re-set a control* | One value resets to its default; set it again in the UI if you had changed it. |
| *re-add a module* | The module vanishes from the tree on boot; add it again and re-enter its controls. |
| *update a file* | An on-device file must be edited or replaced. |
| *erase flash* | A full flash erase is required (the heaviest — a full reconfigure follows). |

---

## Unreleased (`next-iteration`)

### `MoonLedDriver`: `forceRing` → `useRing`, and the ring's geometry is now settable (2026-07-17)

The pin-expander path selector was a three-option Select (`auto` / `ring` / `wholeFrame`) named for a *diagnostic override*. The auto-router is gone — at the size the expander exists for (48 strands × 256 lights) a whole frame never fits internal DMA RAM, so "auto" had exactly one right answer while presenting itself as a choice, and its silent fallback hid which path was actually running. What remains is the honest question, as a switch:

| Old | New |
|---|---|
| control `forceRing` (Select: `auto`/`ring`/`wholeFrame`) | `useRing` (a switch: on = ring, off = whole frame) |
| — | `ringRows` (new: lights per DMA buffer, 1..64) |
| — | `ringBufs` (new: buffers the DMA circulates, 2..32) |

**Action: re-set `useRing` if you had `forceRing` on `wholeFrame`.**

`forceRing` reads as absent → ignored, and `useRing` takes its default (**on**, the ring). A device that had explicitly selected whole-frame therefore comes up on the ring; flip `useRing` off to get it back. `ringRows`/`ringBufs` default to 16 and 12 — the geometry the driver effectively ran. (It shipped with a pool of 16, but 16 buffers never fit the S3's internal DMA heap, so the ring build failed its own fit check and the driver quietly fell back to whole-frame; 12 is what actually held. A config on the old defaults may therefore start *ringing* where it used to fall back.) They exist so the RAM / encode-overhead / interrupt-rate / lap-time trade-off can be swept on a live board rather than fixed at compile time.

### LED driver + control rename — a human-readable UI (2026-07-16)

The LED driver module types and several controls were renamed so the UI reads in plain language rather than peripheral jargon (the UI shows a control's name verbatim, so the name *is* the label).

| Old | New |
|---|---|
| module type `I80LedDriver` | `MultiPinLedDriver` |
| module type `MoonI80LedDriver` | `MoonLedDriver` |
| control `shiftRegister` | `pinExpander` |
| control `asyncTransmit` | `doubleBuffer` |
| read-only `wireUs` | `frameTime` |
| read-only `stall` (Drivers) | `renderWait` |

**Action: re-add the module, then re-set `pinExpander` / `doubleBuffer` if you had changed them.**

A device whose persisted config names the old module type loads a module type that no longer exists — the unknown type is ignored, so **the driver is absent from the tree on boot**. Re-add it (`MultiPinLedDriver` or `MoonLedDriver`) and re-enter its controls. Within a re-added driver, the two renamed *settable* controls (`pinExpander`, `doubleBuffer`) read as absent → they take their defaults (`pinExpander` off, `doubleBuffer` on); set them again if your board needs otherwise. `frameTime` and `renderWait` are read-only KPIs — nothing to restore.

`RmtLedDriver` and `ParlioLedDriver` are unchanged. The `pins` / `ledsPerPin` / `clockPin` / `latchPin` / `loopback*` controls are unchanged.

---

## Earlier

These pre-date this log and were recorded in ADR-0013's Consequences list. A device that persisted state on an older build and loads a newer one loses only the noted value, which re-populates on next use.

### Container config filenames (`LayoutGroup.json` → `Layouts.json`)

`.config/LayoutGroup.json` → `Layouts.json`, `.config/DriverGroup.json` → `Drivers.json` (container type rename). The stale files are ignored (unknown-type config isn't loaded); they linger harmlessly on disk until a flash erase.

**Action: nothing.** Lost: the container's `enabled` flag (defaults back on).

### UI last-selected module (`mm.selectedModule` → `mm_selected`)

The browser localStorage key for the UI's last-selected module.

**Action: nothing.** Lost: the remembered selection resets to the first module.

### Device-list `colour` → `color` (US-spelling rename)

The DevicesModule persisted-list key for a Hue bridge's color-capable light count (`DevicesModule::restoreList()`). A device list persisted under the old key reads the count as absent → 0.

**Action: nothing.** The cached bridge count resets to 0 until the bridge is re-heard live and re-populates it.
