# Plan: MoonLight, from v5.0.0 to the rename

projectMM becomes MoonLight. **v5.0.0 is the last release under the old name and v6.0.0 is the first under the new one.** This file is the whole record: what ships before the switch, what happens at it, what follows, and the decisions already taken along the way. It replaces the five files that held pieces of it.

## The two decisions that shape everything

**Migration from the predecessor is finished.** No further effects, layouts or modifiers are ported from the old MoonLight. What exists today is what ships, and the gap tables in the old plans are closed rather than outstanding. New effects are written on merit from here, not to match a list.

**A user's configuration survives both releases.** v5.0.0 upgrades in place, subject only to the breaks [MIGRATING](../../reference/MIGRATING.md) already records. v6.0.0 carries configuration across too, by Backup on v5 and Restore on v6, because **nothing persisted carries the product name**: config files are named after module types (`Effects.json`, `Drivers.json`) and the sweep leaves every type and `namespace mm::` untouched. The migration engine in [migrate.js](../../../src/ui/migrate.js) therefore has no rename to apply for the rename itself, which is the easiest case it can be handed.

What does break at v6.0.0 is **live interoperation**: a projectMM device and a MoonLight device on one network stop recognising each other, because discovery compares a literal name. That is a same-day nuisance rather than lost work, and the fix is to update both.

## Where we stand

Verified against the tree on 2026-09-22 rather than read from the plans, because several of their status lines had gone stale.

| | Count | Note |
|---|---|---|
| Effects | 67 headers, ~64 registered | Migration complete by decision |
| Modifiers | 12 | All 9 predecessor ones plus three of ours |
| Layouts | 18, 17 registered | Three install-specific ones absent, and staying so |
| MoonLive | 33 `.mle` scripts, 5 `.mlp` palettes | Palettes shipped |
| HUB75 | Ships | `Hub75Driver` is registered |
| Fidelity | 4 of 6 settled | Two open, both bench questions rather than code |

## What the rename touches

393 occurrences of `projectMM` across `src/`, `moondeck/` and `mooninstaller/`; the sweep script measures 542 across 113 tracked files tree-wide. The categories matter more than the count.

[rename_to_moonlight.py](../../../moondeck/rename/rename_to_moonlight.py) handles almost all of it and runs dry by default. It replaces two tokens, `ProjectMM` then `projectMM`, which is correct for every form because `projectMM` is never a substring of another token. Its file list comes from `git ls-files`, so build output is excluded without a blocklist. `MoonLive`, the predecessor's own name, and `namespace mm` are provably never touched.

### The device builds its own OTA URL, and that turns out to be safe

[MqttModule.cpp:324](../../../src/core/system/MqttModule.cpp) formats `github.com/MoonModules/projectMM/releases/download/v<version>/firmware-<name>-v<version>.bin` in firmware, so a device flashed today asks the old repository for its updates forever. Three things make that work anyway, and all three were verified in the code rather than assumed:

- GitHub issues a permanent redirect for a transferred repository, for the API and for release assets.
- The OTA client follows redirects deliberately: [platform_esp32_ota.cpp:117](../../../src/platform/esp32/platform_esp32_ota.cpp) sets `disable_auto_redirect = false` with a redirect count of 10, and raises the header buffer specifically because GitHub's asset redirect overflows the default. It was hardened for this shape of URL already.
- The asset filename is `firmware-<variant>-v<version>.bin`, which carries no product name and so does not change at the rename.

**A v5 device therefore finds and installs v6 with no code change.** Only recreating `MoonModules/projectMM` would break the redirect, and the name sits inside an organisation we control, so nothing to do beyond leaving it alone.

This was worth checking rather than believing: reading the URL construction alone suggests a hard break, and only the fetch path shows there is none.

### What a clean break still costs

Two things the sweep changes that a user feels, neither needing code:

- **Peer discovery** compares a literal `"projectMM"` at [DevicesModule.h:109](../../../src/core/system/DevicesModule.h), and [E131Packet.h:55](../../../src/light/util/E131Packet.h) writes a fixed nine-byte source name. A projectMM device and a MoonLight device will not see each other, so a mixed network is a transitional state to move through rather than live in. Accepted rather than bridged: carrying both tokens forever is the debt this project exists to avoid.
- **The `MM-` device prefix** in [SystemModule.h:57](../../../src/core/system/SystemModule.h) is every device's mDNS name and Home Assistant entity id. Changing it to `ML-` renames all of that, and unlike the configuration it is not something Restore carries back. Keeping `MM-` costs an odd prefix forever; changing it costs every user their bookmarks and automations once. Product owner's call, in the same sweep commit either way.

## v5.0.0, the last projectMM release

1. **An in-place upgrade from any earlier version.** Whatever a tester has configured keeps working across the upgrade, with [MIGRATING](../../reference/MIGRATING.md) as the only exception list. v5.0.0 is where the installed base gathers before the switch, so it cannot be the release that asks them to start over.
2. **The two open fidelity questions**, both bench work rather than code: the audio `volume` scale (0..1 float against our 0..255 `level`), and a cross-check of effects whose predecessor source was incomplete.
3. **Windows day's findings**, since the Sept 29 pass is the first time that platform has ever been tested and anything it turns up is cheaper to fix under the old name.

The release is therefore small by design. Its value is being a known-good, widely-installed baseline that the rename can be measured against, rather than a feature drop.

Everything else is optional polish under the old name.

## v6.0.0, the rename itself

One repository transfer, one sweep commit, one release. The installer manifest, the release asset names and the in-firmware URL builder are a lockstep set, so a half-applied rename leaves devices unable to update.

1. **Dry-run the sweep on a throwaway branch** and run the full gate set on it: every ESP32 variant, the tests, the scenarios, `check_devices`, `check_specs`. Fix what the sweep gets wrong, including the fixed-length wire-protocol fields and their golden-vector tests. Throw the branch away; the point is to harden the script.
2. **Transfer the repository**, which redirects the old URLs.
3. **Run the sweep with `--apply`** on a branch off the renamed repo, as one auditable commit, and read the diff in full.
4. **Flip the identity set together** in that same commit: binary name, asset names, the manifest `name` and `home_assistant_domain`, the docs domain, and the device prefix if it changes.
5. **Cut v6.0.0**, verify OTA from a v5 device on the bench, and document the upgrade as Backup on v5 then Restore on v6.
6. **Hand-edit `moondeck/moondeck.json`**, which is gitignored and so outside the sweep.

`namespace mm::` stays. It is not the product name, and renaming it would touch every file for nothing.

## The cutover

Dates are the product owner's. The two fixed points are Windows day and release day, and the rest hangs off them.

### Before: v5.0.0 ships (date to set)

v5.0.0 has to exist before anything downstream matters, because it is the release the installed base updates *from* and the only one that can carry them across the move. Its three items are listed above. **Cut it far enough ahead that a tester can run it for a few days**, since a defect found after the rename is a defect in two releases at once.

### Sept 29: Windows day

The desktop build runs on Windows and is packaged by CI, but **no test has ever run there**. [release.yml:318](../../../.github/workflows/release.yml) has the only Windows runner in the repository and it compiles and packages without invoking `mm_tests`, the scenarios, or anything else. Every item below is therefore unverified rather than lightly verified.

Four features are a genuinely different program on Windows rather than a thin shim, and they come first:

1. **Raw Ethernet output** (the L2 panel driver). Loads Npcap's `wpcap.dll` at runtime and batches through `pcap_sendqueue`, where POSIX opens a raw socket and sends per packet. Needs Npcap installed. The struct layouts must match the installed Npcap exactly.
2. **The Ethernet interface picker.** Enumerates through `GetIfTable2` and matches GUIDs against pcap names. A Hyper-V switch or a VPN adapter can empty the list or report another adapter's link speed.
3. **RTSP and HLS video out.** `CreateProcessA` takes a hand-built command string where POSIX passes an argv array, and the stop path is `TerminateProcess` plus `CancelIoEx` rather than a signal and a pipe EOF. Both were written this week and neither has run. Test that the stream plays, and that changing the layout while it plays does not hang the app.
4. **NDI.** Resolves `Processing.NDI.Lib.x64.dll` off the PATH the NDI installer sets, so a missing runtime reports "not installed" rather than failing loudly.

Then the JIT, which fails as a crash or as wrong pixels rather than an error:

5. **MoonLive scripts.** The x86-64 backend emits for the **Win64 ABI**, a different register assignment and a 32-byte shadow space, with its own hand-assembled blobs and patch offsets. Open a script and watch it render. Executable memory comes from `VirtualAlloc` with `PAGE_EXECUTE_READWRITE`, which antivirus or a hardened policy can refuse outright, taking all scripting with it.

Then the things a user meets on day one:

6. **The installer.** Install, launch from the Start menu, upgrade over a running instance (NSIS runs `taskkill` first), and uninstall.
7. **Saving configuration.** Windows gets a plain `fopen` with no owner-only ACL, and `std::filesystem::rename` over an open file fails where POSIX replaces it, so a save can fail silently. Save a preset twice.
8. **Port binding.** `SO_REUSEADDR` is deliberately omitted because on Windows it means "steal the port", so a second instance behaves the opposite way from macOS. Start two and bind DDP twice.
9. **Audio input.** miniaudio switches to WASAPI, so the device list, the default entry and whether loopback works are all Windows-specific.
10. **Serial ports and flashing.** The dropdown reads `SERIALCOMM` from the registry, and [_idf_win_shim.py](../../../moondeck/build/_idf_win_shim.py) forces a UTF-8 locale because idf.py refuses to start under cp1252, which only a non-English Windows reproduces.
11. **The web installer.** Its documented DTR/RTS reset bug is worse on Windows 11, and the Windows-only hint rows are user-agent gated, so confirm they appear.

Smaller checks to make while the above runs: the browser opens on first launch, HTTPS reaches the cloud through WinHTTP and the Windows certificate store, and the crash log's timestamp is not garbled, since `localtime_s` takes its arguments in the opposite order to `localtime_r`.

**Anything found here is fixed before the rename**, not after: a Windows defect discovered in v6.0.0 costs a patch release under a brand-new name.

### Sept 30: MoonLight v6.0.0

One day, in order, with a stop at each gate:

1. **Dry-run the sweep on a throwaway branch** and run the full gate set: every ESP32 variant, the tests, the scenarios, `check_devices`, `check_specs`. Fix what it gets wrong, particularly the fixed-length wire-protocol fields and their golden vectors. Discard the branch.
2. **Back up a configured v5.0.0 device** and keep the bundle. This is the evidence for the migration claim, and it has to be taken before anything moves.
3. **Transfer the repository**, which leaves the old URLs redirecting.
4. **Run the sweep with `--apply`** on a branch off the renamed repo, read the diff in full, commit it as one change.
5. **Flip the identity set in that same commit**: binary name, release asset names, the manifest `name` and `home_assistant_domain`, the docs domain, and the `MM-` prefix if it changes.
6. **Run the full gate set again** on the swept tree, then tag and release v6.0.0.
7. **Verify the two claims**: a v5.0.0 device finds and installs v6.0.0 over OTA, and the backup from step 2 restores onto it with layouts, effects and scripts intact.
8. **Hand-edit `moondeck/moondeck.json`**, which is gitignored and outside the sweep.

If step 7 fails, the release stays and the fix is a v6.0.1: the repository has already moved by then, so rolling back is not on the table. That is why steps 1 and 2 happen first.

### After: the week following

Watch for what only real users hit: OTA from versions older than v5.0.0, the documentation redirect, and a mixed network where someone has not updated both devices.

## After v6.0.0

**Wired DMX-512, in and out.** Absent from `src/light/drivers/` entirely, and the largest remaining capability gap. It needs a transceiver board, so it carries the longest lead time of anything here, and it gates nothing about the rename: a driver added under the new name costs exactly what it would have cost under the old one. Deferring it is what keeps v5.0.0 small enough to cut.

Also here: Ants, Spiral Fire, LightsControl, the IMU, and the per-band onset and BPM work. None of it blocks the rename, and none is easier before it.

## Decisions already on record

### Improvements over the predecessor

The migration mandate was fidelity, so every deliberate divergence was registered rather than left as drift. Product-owner ruling, 2026-07-01: improvements that increase user satisfaction are allowed.

| Effect or primitive | Predecessor behavior | Ours | Why it is better |
|---|---|---|---|
| `math8::map8` | `lo + scale8(in, hi-lo)`, so the input top never reaches `hi` and a one-step span collapses to 0 | `lo + in*(hi-lo)/255`, reaching `hi` exactly | Audio bars reach full height, and a 1-row bar becomes possible. Matches FastLED's documented `map8` |
| FreqSaws | Each band's physics advanced once per **column**, so a band spanning K columns ran K times too fast | Each of the 16 bands integrates once per frame | Speed no longer depends on panel width: identical on a 32-wide and a 256-wide grid |
| SphereMove | An integer divide meant the shell only advanced on whole ticks, about 20 updates a second at 60 fps | The expression stays in float | Smooth motion at all speeds. The predecessor intended float here, so this is also more faithful |
| Lissajous | A 1-wide or 1-tall grid mapped every sample to coordinate 1, which clips, so nothing drew | The size-1 axis maps to coordinate 0 | Visible output on thin grids; normal grids unchanged |
| PaintBrush | Oscillator endpoints truncated into `uint8_t`, so grids past 256 per axis swept only a low corner | Oscillators generate 0..255 then scale to the grid | Strokes span any grid and use the full palette range. Grids up to 256 per axis are pixel-identical |
| FixedRectangle | On RGBW the W channel was written on every box cell, tinting colored tiles and leaving W stale | W follows the checker, cleared to 0 on colored tiles | Colored tiles render as pure RGB, and the checker actually alternates |
| GEQ3D sweep | A per-frame counter, so the sweep tracked frame rate and ran faster on a quicker board | A time-based triangle wave | `speed` means the same on every device, which is the projectMM convention |
| GEQ3D bars | Bar width `cols / NUM_BANDS` truncates to 0 when columns are fewer than bands, piling every bar at x=0 | The drawn band count is clamped to the column count | Bars render on narrow grids; a no-op on normal ones |
| AudioFrame | One level value, where WLED exposes both instant and smoothed | Added `levelSmoothed`, an EMA beside the raw `level` | Effects that should glide no longer jitter per audio block, and beat-reactive ones stay snappy |

Invisible fixes, listed for the record rather than as behavior: overflow guards on huge grids in GEQ, StarSky and PaintBrush; the Tetrix 49-day `millis` wrap; a GoL 3D out-of-bounds read; RubiksCube float to int, which is pixel-identical. StarField's blur control was flagged as inverted and turned out to match the predecessor, so only its comment changed.

### Fidelity tensions

Four of six are settled. The two open ones are listed under v5.0.0 above, and both are bench questions rather than code:

- **The audio level scale.** The predecessor normalizes `volume` to 0..1 where ours is a 0..255 `level`. A real INMP441 cross-check against the synthetic reference settles whether any effect reads differently.
- **Reconstructed logic.** Where the predecessor's source was incomplete, the behavior was reconstructed: the Tetrix fall cadence, the FreqMatrix scroll, Blurz dot placement, the FreqSaws band response, the GEQ peak fall, the NoiseMeter drift. A bench pass confirms each looks right. Only three `RECONSTRUCTED` markers survive in the tree, all in layouts, so the effect-side markers are gone and this is a visual check rather than a grep.

The accepted-as-is entry: `scale8` against integer `*bri/255` rounding in SolidEffect and elsewhere, kept faithful, no change wanted.

## Verification

1. A device running the current release upgrades to v5.0.0 and keeps its configuration, layouts and scripts, with only the [MIGRATING](../../reference/MIGRATING.md) entries behaving differently.
2. A v5.0.0 device on the bench updates itself to a v6.0.0 release after the repository has moved. This is the gate the v5 release exists for, and it can only be tested once both exist.
3. The sweep's diff is read in full before it is committed, since a blanket replace is how a symbol gets renamed by accident.
4. The full gate set passes on the swept tree: every ESP32 variant, the tests, the scenarios, `check_devices`, `check_specs`.
5. `moonmodules.org/projectMM` redirects to the new documentation site.
6. A v5.0.0 backup restores onto a v6.0.0 device with its layouts, effects and scripts intact, which is the claim that replaces the erase.
