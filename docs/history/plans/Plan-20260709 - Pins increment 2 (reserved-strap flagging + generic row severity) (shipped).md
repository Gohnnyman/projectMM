# Plan — Pins increment #2: reserved/strap flagging + generic row severity

Pins top-down [increment #2](../../backlog/pins-analysis-top-down.md#8-increments-each-its-own-plan-when-picked): flag a GPIO claim that lands on a reserved or strap pin. Builds on increment #1 (the read-only ownership map, shipped in 37b04ee). PO-directed alongside #3 and #4.

## Context

The pin map ([PinsModule.h](../../../src/core/PinsModule.h)) shows *who owns each GPIO*. Increment #2 adds *is this a safe pin to own?* — the highest-value column per the top-down (§3): "an output role claimed on an input-only pin, or a driven role on a strap, is visible at a glance — the exact class of bug the GPIO-46 loopback corruption was." The reference data already exists: [gpio-usage.md](../../reference/gpio-usage.md) has per-chip tables of Reserved / Role-conflict / Input-only GPIOs. Two problems to solve:

1. **The data is prose** — `gpio-usage.md` is human-readable Markdown tables (`6-11`, `0, 2, 5, 12, 15`), not machine-consumable. The firmware needs a machine form.
2. **The UI can't show a warning** — the generic list renderer maps field-name *conventions* to visuals (`self`→marker class, `*Sec`→age-color dot) but has NO per-row warning/severity affordance. A `flag:"strap"` field would render as plain text.

## The UI sidestep (PO rule, first-class constraint)

**When pins needs richer UI than the generic list gives, extend app.js's list rendering generically — never add pins-specific UI.** The Explore pass confirmed app.js already has the exact pattern: `rowAgeClass` ([app.js:1609](../../../src/ui/app.js#L1609)) reads a `*Sec` field-name convention → returns a CSS class; `buildListEntries` ([app.js:1494](../../../src/ui/app.js#L1494)) applies it as a dot. So increment #2's UI need — "a row can be visually flagged" — is met by adding **one sibling convention**: a `severity` field → a row CSS class, usable by *any* module's list (a task in a bad state, a device with an error, a conflicted pin — all future users). This is not pins UI; it's the generic list gaining a severity affordance the same way it already has an age affordance.

## Verified seams (Explore, file:line)

- **Data source:** [gpio-usage.md](../../reference/gpio-usage.md) — per-chip `| Avoid | GPIOs | Why |` tables, rows tagged Reserved / Role-conflict / Input-only. Chip ceiling `MM_MAX_GPIO` ([Control.h:13](../../../src/core/Control.h#L13), build-injected per target).
- **Flag attaches to:** the `Claim` struct ([PinsModule.h:69](../../../src/core/PinsModule.h#L69)) — `gpio`, `owner[16]`, `role[14]`; add a `severity`. Emitted in `writeListRow` ([PinsModule.h:96](../../../src/core/PinsModule.h#L96)).
- **Lookup precedent:** `roleFor`'s `static constexpr Entry kRoles[]` ([PinsModule.h:178](../../../src/core/PinsModule.h#L178)) — the sibling pattern for a per-chip strap/reserved table.
- **Serialization:** `writeControlValue` List case ([Control.cpp:121](../../../src/core/Control.cpp#L121)) emits `value=[writeListRow…]` — a new field in the row object needs NO core change.
- **UI extension point:** `buildListEntries` ([app.js:1473](../../../src/ui/app.js#L1473)), `rowAgeClass` ([app.js:1609](../../../src/ui/app.js#L1609)), `listSummaryText` ([app.js:1524](../../../src/ui/app.js#L1524)). CSS `/* List control */` ([style.css:571](../../../src/ui/style.css#L571)). Live-patch path `updateModuleControls case "list"` ([app.js:1993](../../../src/ui/app.js#L1993)) reuses `buildListEntries`, so the extension lands once.
- **Test model:** `unit_PinsModule.cpp` — `FakePinModule` fixture + `allRows()` serialize-and-assert. Extend it.

## Design

### Part A — machine-readable GPIO capability data (core, domain-neutral)

The firmware needs, per (chip, gpio): is it Reserved / a strap / input-only / free. Two candidate shapes:

- **A1 — generate a C++ header from `gpio-usage.md`** at build time (a `check`/generate script parses the tables → `gpio_caps_generated.h` with a per-chip `constexpr` bitset/table). Keeps the doc the single source of truth (the check-specs philosophy). Cost: a new generator + parse of prose ranges (`6-11`, PSRAM-conditional notes) — the notes are genuinely hard to parse reliably.
- **A2 — a hand-written `constexpr` table in the platform layer**, with `gpio-usage.md` as its documented source (a comment cross-links them, `check_specs`-style validation optional later). Simpler, no fragile prose parser; the data is small and rarely changes. The strap/reserved sets are ~5-10 GPIOs per chip.
- **A3 — use the IDF's own runtime queries** behind a platform seam: `GPIO_IS_VALID_GPIO` / `GPIO_IS_VALID_OUTPUT_GPIO` / `rtc_gpio_is_valid_gpio` (§3 names these). These give valid/output-capable/RTC for FREE from the SDK — but NOT "strap" or "reserved-for-flash" (those aren't an IDF query; they're board/datasheet knowledge). So A3 covers *capability* but not *strap/reserved*.

**Recommendation: A3 for capability + A2 for strap/reserved.** The IDF queries (A3) are the textbook, always-correct source for valid/output/RTC — behind a new `platform::gpioCapability(gpio)` seam (desktop stubs to "all valid"). The strap/reserved overlay (A2) is a tiny per-chip `constexpr` table the seam ORs in, sourced from `gpio-usage.md`. This avoids the fragile prose-parser (A1) while keeping the SDK as the authority for what it actually knows. Defer A1 (generated-from-doc) unless the table grows enough to warrant it — note that as a follow-up.

New platform seam (follows the `taskSnapshot` fixed-size precedent):
```cpp
// platform.h — capability flags for one GPIO. Domain-neutral; desktop returns "all valid, no straps".
struct GpioCapability { bool validGpio; bool outputCapable; bool rtc; bool strap; bool reserved; };
GpioCapability gpioCapability(uint8_t gpio);
```
ESP32 impl: `GPIO_IS_VALID_GPIO`/`GPIO_IS_VALID_OUTPUT_GPIO`/`rtc_gpio_is_valid_gpio` + the per-chip strap/reserved `constexpr` table (from gpio-usage.md). Lives in `src/platform/esp32/` (a new small file or in platform_esp32.cpp). Desktop stub in `src/platform/desktop/`.

### Part B — PinsModule consumes it → per-claim severity

In `PinsModule::collect`, after resolving each claim's gpio, call `platform::gpioCapability(gpio)` and derive a `severity`:
- **error** — reserved (flash/PSRAM/USB): owning ANY peripheral here corrupts the device.
- **warn** — a driven/output role on a strap or input-only pin (role is output-ish: `pins`/LED lane, loopback Tx, MDC/MDIO/clock — vs. an input role like a mic SD on input-only, which is fine). The role→direction hint comes from the same name-derivation `roleFor` already does.
- **none** — free pin, or an input role on an input-only pin.

Add `char severity[8]` (or an enum serialized to a short string "error"/"warn") to `Claim`; set it in `collect`; emit it in `writeListRow`. Keep the derivation a small static helper beside `roleFor` (the same "small display-logic table, not a vocabulary authority" bar). This stays read-only surfacing — no enforcement (that's #3).

### Part C — generic row severity in app.js (THE SIDESTEP, done right)

Mirror the existing `*Sec`→age-class convention with a `severity`→severity-class convention:
- **`rowSeverityClass(item)`** — sibling of `rowAgeClass` ([app.js:1609](../../../src/ui/app.js#L1609)): reads a `severity` string field ("error"/"warn"), returns `"list-severity-error"` / `"list-severity-warn"` / `""`. Generic over field name; documented as "any list emitting a `severity` field gets the same treatment" (like the age comment).
- **`buildListEntries`** ([app.js:1482](../../../src/ui/app.js#L1482)): apply the class to the `list-entry` (or a badge span), same spot the `self` class and age dot attach.
- **`listSummaryText`** ([app.js:1524](../../../src/ui/app.js#L1524)): add `severity` to the skip-filter (like `self`) so it doesn't also render as a plain-text word — the color IS its rendering.
- **CSS** ([style.css:571](../../../src/ui/style.css#L571)): `.list-severity-error` / `.list-severity-warn` (a left border / text tint / badge — matching the existing age-dot visual language, theme-aware).

No `if (module === "Pins")` anywhere. A DevicesModule row, a TasksModule row, any future list that emits `severity` gets the same visual for free — that's the genericness the rule demands.

## Files

- **New:** `src/platform/esp32/platform_esp32_gpio.cpp` (or fold into platform_esp32.cpp) + desktop stub; `GpioCapability` in `platform.h`.
- **Edit:** `src/core/PinsModule.h` (Claim.severity + collect derivation + writeListRow); `src/ui/app.js` (rowSeverityClass + buildListEntries + listSummaryText); `src/ui/style.css` (severity classes); `docs/reference/gpio-usage.md` (a note that the strap/reserved table is mirrored in the platform layer, cross-link); `docs/moonmodules/core/system.md` (Pins card: the severity column).
- **Tests:** `unit_PinsModule.cpp` — a claim on a reserved pin → severity "error"; an output role on a strap → "warn"; an input role on input-only → none; a free pin → none. `test/js/` — a `rowSeverityClass` unit test (the JS host-test gate) asserting the field-convention mapping.

## Verification

1. Build clean; ctest + scenarios; check_specs (system.md card); platform boundary (the new seam lives in src/platform/); ESP32 build; JS tests.
2. Live on hardware: the P4/S3 map shows a **warn** on any output role sitting on a strap, an **error** on a reserved pin. (The S3's GPIO-46-class bug would now light up.) Re-confirm no false-positive on legit input roles.

## Scope guard

#2 is *surfacing severity*, not enforcing (that's #3) and not live-state (that's #4). Keep the capability seam domain-neutral (it's core infra many things could use). Do NOT parse the prose gpio-usage.md into C++ this increment (A1) — hand-mirror the small strap/reserved table (A2) with the SDK queries (A3) doing the capability heavy-lifting; note the generator as a follow-up if the table grows. The app.js change MUST be the generic severity convention, never pins-specific. Mark `(shipped)` when it lands.
