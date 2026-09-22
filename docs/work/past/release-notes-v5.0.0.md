# projectMM v5.0.0

**166 commits across 27 PRs**, and the last release under this name: the next one is MoonLight. Highlights: the wall leaves the device as video over NDI, HLS and RTSP; HUB75 panels drive straight from the board's pins; a second boot image gives 4 MB boards their flash back; configuration travels by backup and restore; and audio-reactive effects work on the desktop.

If you like projectMM, give it a ⭐️, fork it, or open an issue or pull request. It helps the project grow, improve, and get noticed.

### 🌗 About the name

This is the final projectMM release. The project becomes **MoonLight** at v6.0.0, which is a rename rather than a rewrite: the same code, the same modules, the same configuration. Your settings carry across with Backup and Restore, and a device running v5.0.0 finds the v6.0.0 release on its own, because this firmware already knows both addresses.

### ✨ Highlights

**The wall as video (new)**

- **NDI** publishes the layer as a source OBS, Resolume and TouchDesigner pick up by name.
- **HLS** serves an H.264 stream from the device's own HTTP server, playable in VLC, a browser or an Apple TV. Desktop encodes through your ffmpeg; the **ESP32-P4 encodes in hardware** and serves segments from a PSRAM ring, never touching flash.
- **RTSP** streams the same H.264 as a pull stream a player opens directly. It reaches a viewer far sooner than HLS, which buffers whole segments before showing one, so this is the remote view to reach for. Point `ffplay` or VLC at the card's URL.

**HUB75 panels, driven directly (new)**

A HUB75 panel now lights from the board's own pins, with no ColorLight receiving card in between. Board presets for MoonHub75, the Adafruit MatrixPortal S3 and the Waveshare RGB Matrix; selectable scan rate, bit depth and clock edge; and two backends, LCD_CAM and Parlio, to choose between.

**MoonBase: a second boot image (new)**

A 4 MB board stops spending half its flash on a second copy of the firmware. The app partition grows from **1856 to 2496 KB** and the filesystem from **256 to 548 KB**. One click covers reboot, install and reboot, and a power cut mid-update lands back in MoonBase rather than a half-written app. Installs measure about three times faster than the path they replace.

**Backup and restore (new)**

One button in the File Manager downloads WiFi, modules, presets and MoonLive scripts as a single bundle. Restore converts settings written by an older firmware and applies them live, with no reboot. It works from a freshly erased device's own access point, which is what makes a rebuild from nothing a two-minute job.

**Audio on the desktop (new)**

Audio-reactive effects now run on macOS, Windows and Linux, reading a microphone or a loopback device from a dropdown. A desktop with audio sending enabled becomes a WLED audio-sync source for a whole fleet, so one machine hears the room and every device reacts.

**Control surfaces and scripted inputs**

Open Stage Control is now a two-way surface: move a fader on the device and the surface follows. Infrared and buttons became **lists of rows** you add, learn and point at any target the REST API can set, rather than fixed firmware actions. Analog inputs are readable, and sensor scripts run on the board.

**MoonLive grows a language**

Functions take arguments and return values, local variables are real, and a script sees the whole rig, including aiming moving heads. **Scripted palettes** (`.mlp`) recompute their sixteen entries every frame and sit in the same picker as the sixty built-ins. The whole shipped script library is browsable and downloadable in the UI.

**Moving heads**

Moving heads work in 2D and 3D with per-fixture placement, effects steer pan and tilt through the same buffer that carries color, and the preview draws each head's beam as a colored cone.

**New effects**

Fluid (Navier-Stokes dye jets), Nebula, Trails, ColorTrails, Aurora, BeatRipples, RadialSpectrum, VuMeters, FishTank, Pacman, Pong, SpaceInvaders, FlyingToasters, SpriteFountain and FixedPoint. Underneath them, Perlin gradient noise replaces value noise, joined by polar and oscillator kernels, all with 3D forms.

**New boards and distribution**

ESP32-S3-Zero and the QuinLED Dig-Next-2 join the shipped boards, with Ethernet presets for Classic RMII, the P4-NANO and the S31 CoreBoard. The web installer flashes every chip projectMM ships, the S31 included. Linux gains an **arm64 package**, Windows a zip install when Defender blocks the installer, and every release publishes a **container image** whose instances generate their own MAC and name so a fleet is not a row of identical devices.

**MoonCloud, strictly opt-in**

Off by default. Report hardware and configuration statistics, and post to a public board. The installation id is a SHA-256 of a salt and the MAC, so the raw MAC never leaves the device.

### 📐 Measured improvements

- **RMT LED driver** rewritten to ship wire bytes: **96 bytes per light down to 3**, and the ceiling where a long strand silently stopped updating is gone.
- **Parallel LEDs on a classic ESP32**, alongside the microphone, where any pin change used to reset the board: **256 lights at 114 fps** on a Dig-Next-2, **64 lights at 414 fps** on an Olimex Gateway with Ethernet up.
- **Fluid** on a 64x64 panel costs **134 us**, and a 20x20x20 cube **232 us**. The pressure solve is the knob, near-linear from 20 us at one iteration to 69 at twenty.
- **Polar effects** read angle and radius from a table built once per geometry, which is **34% of a PolarNoise frame**.
- **MoonLive on a classic ESP32**: transient heap for the largest script falls from about **110 KB to 40 KB**.
- **Per-band audio conditioning** measures each band against its own noise floor, so a quiet band is audible without a loud one clipping, and beats are detected rather than inferred from volume.
- **Brightness follows the CIE 1931 lightness curve**, so a slider at half reads as half.
- **Every one of the 61 effects has a preview**, where twelve did.

### 🐛 Notable fixes

- **A silent configuration wipe on upload**: HTTP header matching was case-sensitive and missed a lowercase `content-length`, committing an empty file with a 200. Found on the bench when it zeroed a test device's configs and scripts.
- **Flicker under WiFi on classic ESP32**, fixed by moving the RMT refill to a level-5 interrupt. Two boards that flickered all evening are clean.
- **Scripts painted into a corner**: a script read width as 255 on any larger grid, so every 2D effect drew a complete picture into one corner.
- **A remote overread in the OSC parser**, which is a security fix.
- **HUB75 row addressing off by one**, and a DMA wrap that double-lit row 0.
- **Frame-rate coupling in sprite effects**, where a motion took two seconds at 60 fps and half a second at 240.
- **The PDM microphone on the Dig-Next-2**, whose two-wire mode the I2S seam did not support.
- **The preview dropped lights** whose coordinates shared a large factor.

### ⚠️ Breaking changes

Seventeen entries, each with the action it asks of you, are in [MIGRATING](../../reference/MIGRATING.md). Most need nothing. The ones most likely to affect a running device:

- `expertMode` became `mode`, with three levels, so a device comes up in `user` mode.
- Infrared is a list of learned rows, and the remote must be re-learned.
- 4 MB boards and `esp32-16mb` move to the MoonBase partition table, which is an erase-and-reflash.
- AudioVolume is gone, and Noise2D folded into Noise.
- `soundReactive` is now `audioReactive`, and PreviewDriver's `fps` is now `targetFps`.

**Backup before upgrading.** The File Manager's Backup carries configuration across every one of these, converting what it can and reporting what it cannot.
