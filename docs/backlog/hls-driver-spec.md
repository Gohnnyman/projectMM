# HlsDriver spec (draft, ships with the implementation)

Watch a projectMM installation pixel-exact on a TV, a media player, or a browser: the desktop
build streams its rendered output as H.264 over HLS from its own HTTP server. One general
implementation for every host with ffmpeg (macOS, Windows, Linux, Raspberry Pi); ESP32 is out
of scope (no hardware encoder). Complements NdiDriver: NDI is the pro-tools path, HLS is the
consumer-playback path.

## Decision: pipe to ffmpeg

The driver spawns `ffmpeg` (found on PATH; a runtime dependency like Npcap and the NDI
runtime, never vendored) and writes raw RGB frames to its stdin; ffmpeg encodes with the
user-picked encoder (an `encoder` Select: `libx264` default, hardware entries like
`h264_videotoolbox` offload it) and writes HLS segments plus the `.m3u8` playlist into
`/.hls/` under the fs mount, which HttpServerModule serves. One code path for
all hosts, no codec in the tree, no license baggage. Rejected alternatives: per-OS encoder
integrations (three implementations, fails the generality bar), MJPEG (no interframe
compression: fails 4K throughput), vendored x264/openh264 (GPL / binary-patent baggage).

## Pixel-exact contract

The encoded frame IS the grid: width x height from the layer, no scaling anywhere in the
pipeline. A 512x512 grid arrives at the TV as a 512x512 video the display letterboxes; every
written pixel is one video pixel. Frame pacing follows the render loop capped by `targetFps`;
H.264 carries the actual rate.

## Deviations settled at implementation

- **mDNS advert deferred**: desktop has no mDNS implementation at all today and Apple TV does
  not browse DNS-SD; the read-only `url` control is the discovery. Its own item if wanted.
- **libx264 first**: at practical grid sizes the software encode is trivial CPU; hardware
  encoder selection (VideoToolbox / Media Foundation / VAAPI) becomes worthwhile together with
  the render-scaling item.

## Latency

Encode adds milliseconds; HLS segmentation and player buffering add the seconds. Tuned for
live (1 s segments, short playlist, no B-frames) the expectation is 2-5 s glass-to-glass;
document it on the card so nobody expects preview-grade feedback. Option, not scope: the same
encode can also serve an MPEG-TS endpoint (~1-2 s in VLC) if testing ever needs it.

## Module

Light-domain driver `HlsDriver` under Drivers, desktop builds only (`platform::hasFfmpegPipe`
style gate mirrors NdiDriver's). Controls:

- `targetFps` (uint8, default 30): encode pacing; frames beyond it are dropped before the pipe.
- `bitrate` (uint16 kbit, default 8000): passed to ffmpeg as `-b:v`.
- `encoder` (Select, default `libx264`): the ffmpeg video encoder; hardware entries offload
  the encode. Availability depends on the ffmpeg BUILD (libx264 needs --enable-libx264, which
  practically every distribution ships): an encoder this ffmpeg lacks starts and exits
  immediately, surfacing through the restart path as `encoder exited - check ffmpeg`, since
  encoderStart() can only verify that ffmpeg itself launches.
- read-only `status`: `streaming WxH at F fps` (with a dropped-frame count when any),
  `ffmpeg not found - see the docs`, `encoder restarted`, `encoder exited - check ffmpeg`.
- read-only `url`: the playable address (`http://<host>:<port>/hls/stream.m3u8`, the port the
  server actually serves), shown so the user can copy it into VLC/TV.

## Robustness and the hot path

- The render tick packs the frame (per-pixel correction, O(width x height)) and ENQUEUES it
  whole; a dedicated platform writer thread does the blocking pipe writes on every OS, so the
  tick never touches the pipe. A queue past 3 frames drops-newest with a visible counter, and
  whole-frame handoff makes a torn frame structurally impossible. After a spawn the driver
  waits a short warm-up before the first frame (encoder init reads nothing).
- ffmpeg missing: status says so, nothing crashes, the driver idles until re-enabled.
- ffmpeg exits (crash, kill): status shows the exit, restart with backoff; segments dir is
  recreated per session and cleaned on release().
- Live reconfiguration: grid size or fps change tears down and respawns ffmpeg (a new encode
  geometry needs a new stream); viewers re-buffer, which is inherent to the format.

## GridLayout change (rides along, PO-requested)

`width`/`height` become plain NUMBER inputs (`setNumberField`) with max 3840 x 2160 (4K);
`depth` keeps its slider and 512 bound. `lengthType` (int16_t) holds 3840. The card documents
the framerate expectation honestly: the render loop is single-threaded, so large grids trade
fps (roughly: 512^2 smooth, 1024^2 15-30 fps, 1080p 7-15 fps on a desktop-class host); render
parallelization is a separate backlog item.

## Files

- src/light/drivers/HlsDriver.h — the driver (spawn, pipe, pacing, status).
- src/platform/…: process-spawn + non-blocking pipe seam (desktop implementation; ESP32 stubs
  compile out), ffmpeg discovery.
- src/core/HttpServerModule.cpp — serve the segments directory (`/hls/…`, no-cache playlist).
- src/light/layouts/GridLayout.h — number fields + 4K bounds.
- docs/moonmodules/light/drivers.md — the HlsDriver card (latency + install-ffmpeg note);
  building.md gains the ffmpeg runtime-dependency line.

## Tests

- Unit: ffmpeg argument builder (geometry/fps/bitrate); lifecycle with ffmpeg absent (status,
  no crash); non-blocking drop counter when the sink stalls; restart-with-backoff.
- Host integration: pipe frames into a fake ffmpeg (a script that consumes stdin and writes a
  playlist) and assert the served playlist + segment routes.
- Scenario: driver add/enable/disable/remove live, no reboot.

## Verification

Desktop build zero warnings; live: stream a 512x512 grid to VLC and a TV, confirm pixel-exact
(test pattern with single-pixel features), measure glass-to-glass latency, kill ffmpeg
mid-stream and watch the status + recovery. The PO's eyes on the TV are the measurement.
