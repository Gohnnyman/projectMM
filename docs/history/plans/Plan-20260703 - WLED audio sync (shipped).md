# Plan — WLED-compatible audio sync (send + receive) in AudioModule

## Context

projectMM's AudioModule analyses local audio (line-in / mic) into an `AudioFrame` (16 GEQ
bands + level + peak). The product owner wants **WLED audio-sync over UDP** so projectMM can
interoperate with the WLED ecosystem: a projectMM device can **broadcast** its analysed audio
for WLED/MoonLight receivers, and can **receive** a peer's audio to drive its own effects when
it has no local source. MoonLight already *receives* this (`D_WLEDAudio.h`) but nothing in the
family *sends* it — this closes that loop. The wire format is a fixed compatibility contract
(netmindz/WLED-sync), so the packet must be byte-exact.

The design (product-owner-confirmed): a single **`sync` control: Off / Send / Receive**.
- **Off** — local audio only (today's behaviour). No socket bound → zero overhead.
- **Send** — broadcast the local `AudioFrame` as a WLED v2 packet on UDP 11988.
- **Receive** — bind 11988, and when packets arrive, write the peer's audio *into* `frame_`
  (so effects react to it transparently); **auto-blend**: fall back to the local mic/simulate
  when no packet for ~1 s. Socket bound only in Receive mode.

## The wire format (authoritative — netmindz/WLED-sync, header "00002")

`__attribute__((packed))`, **44 bytes**, UDP port **11988**, broadcast:

| offset | field | type | projectMM source (AudioFrame) |
|---|---|---|---|
| 0  | `header[6]` | char | `"00002"` (+ NUL) |
| 6  | `gap1[2]` | u8×2 | zero (part of the wire layout) |
| 8  | `sampleRaw` | f32 | `level` |
| 12 | `sampleSmth` | f32 | `levelSmoothed` |
| 16 | `samplePeak` | u8 | derived: 1 when a beat/peak this frame, else 0 |
| 17 | `frameCounter` | u8 | incrementing send counter |
| 18 | `fftResult[16]` | u8×16 | `bands[16]` — direct 1:1 |
| 34 | `gap2[2]` | u8×2 | zero |
| 36 | `FFT_Magnitude` | f32 | `peakMag` |
| 40 | `FFT_MajorPeak` | f32 | `peakHz` |

The mapping is near-1:1 — `AudioFrame`'s comments already cite the WLED field names
(`volumeRaw`/`volumeSmth`), the struct was built with this in mind. (There's also an 83-byte
v1 packet; we **send v2 only**, and **parse v2 only** — v1 is legacy, out of scope unless a
received v1 shows up, in which case ignore it, don't crash.)

## Files

1. **New: `src/light/WLEDAudioSyncPacket.h`** — the format in one place (the `ArtNetPacket.h` /
   `DdpPacket.h` convention: constants + inline `build` + inline `parse`, round-trip unit-tested).
   - `constexpr uint16_t WLED_SYNC_PORT = 11988;`
   - `constexpr char WLED_SYNC_HEADER[6] = "00002";`
   - The 44-byte `#pragma pack`ed struct (or a hand-serialised builder writing exact offsets —
     hand-serialise is safer than relying on struct packing across compilers, matching how
     ArtNet/DDP builders write bytes explicitly; **decide at impl time**, but the *test* pins 44
     bytes + the offsets regardless).
   - `size_t buildWledAudioSync(uint8_t out[44], const AudioFrame&, uint8_t frameCounter, bool peak)`
   - `bool parseWledAudioSync(const uint8_t* buf, size_t len, AudioFrame& out)` — validates length
     (44) + header ("00002"); fills an AudioFrame from the packet (inverse of build); returns false
     on a v1/short/foreign packet so the caller ignores it.

2. **`src/core/AudioModule.h`** — the send/receive plumbing (all guarded `if constexpr (platform::hasWiFi)`
   so a `MM_NO_WIFI` build compiles the paths out):
   - **Members:** `uint8_t sync = 0;` (0=Off/1=Send/2=Receive), `platform::UdpSocket syncSock_;`
     `uint32_t lastSyncSend_ = 0;`, `uint32_t lastSyncRecv_ = 0;` (millis of last received packet,
     for the auto-blend fallback), `uint8_t syncFrameCounter_ = 0;`, a `uint8_t syncPkt_[64]` scratch.
   - **Control:** in `onBuildControls()`, `controls_.addSelect("sync", sync, {"Off","Send","Receive"}, 3)`
     — placed after `simulate`. A read-only `"sync status"` line (e.g. "receiving from 1.2.3.4" /
     "sending 33/s" / off) via the existing `addReadOnly` + loop1s idiom.
   - **Mode transitions:** `sync` changes must re-bind/unbind the socket, so add it to
     `controlChangeTriggersBuildState()` → `onBuildState()` → a small `syncReinit()`: close the
     socket; if Send → `open()` + `connect("255.255.255.255", 11988)`; if Receive → `open()` +
     `bind(11988)`; if Off → leave closed. (Mirrors NetworkSendDriver's `connectIfDestChanged` +
     NetworkReceiveEffect's bind.)
   - **Send** (in `loop()`, after `frame_` is refreshed): throttle by an interval (WLED sends
     ~real-time; cap ~30–40/s to match, a `syncSendIntervalMs` const — reuse the
     `now - lastSyncSend_ < interval` pattern from NetworkSendDriver:106-114). Build the packet
     from `frame_` + `syncFrameCounter_++`, `syncSock_.sendTo(...)`.
   - **Receive** (in `loop()`): bounded non-blocking drain (mirror NetworkReceiveEffect:103-143,
     e.g. ≤8 packets/tick — sync is low-rate). For each, `parseWledAudioSync` into a temp frame;
     on success, copy it into `frame_` and stamp `lastSyncRecv_ = millis()`. **Auto-blend:** the
     existing local-analysis block in `loop()` should only overwrite `frame_` when NOT in Receive
     mode, OR when in Receive mode but `millis() - lastSyncRecv_ > kSyncFallbackMs` (~1000 ms) —
     i.e. received audio wins while fresh, local mic resumes when the peer goes quiet.
   - **`samplePeak` derivation** for the send: a simple beat flag — set when `level` exceeds a
     short-running average by a margin (or reuse whatever peak signal the FFT block already has at
     AudioModule.h:264-266). Keep it cheap; it's a hint field.

3. **New: `test/*` round-trip test** — `test/` C++ unit (ctest) for `WLEDAudioSyncPacket.h`:
   build→parse round-trips an AudioFrame; pins the **44-byte size**, the **"00002" header**, the
   **exact field offsets** (a golden byte vector — the compatibility contract, same rigor as the
   Improv frame golden vector), and that `parse` rejects a wrong-length / wrong-header / v1 packet.
   (Follows the ArtNet/DDP packet-test precedent + the Improv golden-vector precedent.)

4. **`docs/moonmodules/core/AudioModule.md`** (+ the `///` header docs check_specs validates) —
   document the `sync` control (Off/Send/Receive, the auto-blend behaviour, port 11988, WLED v2
   compatibility). Keep control-name ↔ doc in sync (check_specs gate).

## Not doing (scope guards)

- **No platform change** — `UdpSocket` already has open/connect/sendTo/bind/recvFrom + SO_BROADCAST.
- **No v1 (83-byte) send or parse** — v2 only; a received v1/foreign packet is ignored, not crashed.
- **No new module** — this lives in AudioModule (it owns the AudioFrame, both ends need it).
- **No separate send+receive toggles** — one tri-state `sync` (PO-confirmed), simplest coherent UX.
- **Not a `src/light/` driver/effect** — audio sync is an AudioModule capability, not a light node;
  the packet header goes in `src/light/` only because that's the established home for wire formats.

## Verification

- **Build:** desktop (`cmake --build build`, -Werror) + `esp32p4-eth` (the MHC shield) + a mic-less
  variant (e.g. `esp32-eth`) all clean — the `hasWiFi` guard keeps it compiling everywhere.
- **Unit:** `ctest` — the new round-trip/golden-vector test passes; existing tests unaffected.
  `check_specs` green (control ↔ doc).
- **Bench — SEND (the headline interop):** on the MHC-WLED P4 shield (line-in working, at
  `192.168.1.139`), set `sync=Send`. Capture the UDP on the Mac (`nc -ul 11988` / a tiny Python
  `recvfrom` on 11988) and assert: 44-byte packets, header "00002", `fftResult` matches the live
  bands, arriving continuously. **Cross-check with MoonLight** if a MoonLight receiver is available:
  its `D_WLEDAudio` should light up from the projectMM broadcast — the real compatibility proof.
- **Bench — RECEIVE + auto-blend:** point a second device (or a Python sender emitting the golden
  v2 packet) at the shield with `sync=Receive`; confirm the shield's effects react to the injected
  audio (its `level`/`peakHz` readouts track the sent values), then **stop** the sender and confirm
  it falls back to the local line-in within ~1 s.
- Save the approved plan to `docs/history/plans/Plan-20260703 - WLED audio sync.md`.

## Open items (settle during impl, not blockers)

- **Send rate** — WLED transmits ~per-frame; pick a cap (30–40/s) that's WLED-friendly without
  flooding. A `sync fps` control could be added but Off/Send/Receive + a sensible fixed rate is
  leaner; add the control only if the PO wants tunability.
- **`samplePeak`** — exact beat-flag source (reuse the FFT peak block vs a small level-vs-average
  check). It's a hint field; a simple, cheap derivation is fine.
- **Struct packing vs hand-serialise** for the 44-byte layout — hand-serialised byte writes are
  the safer, portable choice (no cross-compiler packing surprises); the golden-vector test pins it
  either way.
