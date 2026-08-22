# Plan — streamed file upload (any size, fixed small buffer)

## The bottleneck (measured)
Uploads are limited by `HttpServerModule`'s `uint8_t buf[2048]` — it holds the WHOLE request
(headers + body), so a body can't exceed ~1.8 KB (the UI's 8 KB guard is optimistic; the real
cap is smaller). `kFileApiCap = 8192` bounds the *buffered* read/write helpers but never actually
binds for uploads because the 2 KB buffer bites first. `fsWriteAtomic` already writes via a temp
file (fopen/fwrite/fsync/rename) — so the DEVICE never needs the whole file in RAM; only the HTTP
layer's whole-request buffering forces it. Fix: stream the body to the file, never buffering it.

## Design — a pull-based streaming atomic write (fits the existing seam)

Rather than leak a stateful `FILE*` across the platform seam (open/write/close/abort — 4 calls,
error-prone), add ONE seam function that keeps the atomic temp-file dance in core and pulls data
from a caller source until exhausted. Recognizable "sink pulls from source" streaming shape.

**platform.h (new):**
```
// Streamed atomic write: open a temp file, pull chunks from `src` until it returns 0, then
// atomically rename into place. `src(buf, cap, user)` fills up to cap bytes, returns the count
// (0 = end). Returns false (and discards the temp file) on any write/short-read/rename failure.
using FsWriteSrc = size_t(*)(char* buf, size_t cap, void* user);
bool fsWriteStream(const char* path, FsWriteSrc src, void* user);
```
**platform_esp32_fs.cpp / platform_desktop.cpp:** same fopen(tmp,"wb") → loop { n = src(chunk,
sizeof chunk, user); if !n break; fwrite } → fflush/fsync/fclose → rename. A fixed local
`char chunk[1024]` — bounded RAM regardless of file size. `fsWriteAtomic` stays (small callers use
it); `fsWriteStream` is the large/streamed path. (Could re-express fsWriteAtomic on top of
fsWriteStream later; not now — concrete-first, don't churn the working small-write path.)

## HTTP layer — stream the /api/file POST body to the file

The trick: route EARLY (on headers) for this one endpoint, before the "buffer whole body" step.
In the request loop, once `\r\n\r\n` is seen and the method+path parse to `POST /api/file`:
- Parse Content-Length. Reject > a sane ceiling (`kUploadMax`, e.g. 256 KB — a guard against a
  runaway/hostile upload filling LittleFS; the write still fails cleanly if the FS is full, but a
  ceiling keeps a single request bounded). NOT 8 KB — that cap goes away for uploads.
- Any body bytes already in `buf` after the header terminator are the FIRST chunk; the source
  callback yields those, then reads the rest straight off the socket (`conn.read`) in ≤1 KB chunks,
  tracking remaining = Content-Length, with the same bounded-wait patience as today.
- `fsWriteStream(path, srcFromSocket, &ctx)` → 200 `{"ok":true}` / 400 / 500.
- Everything else (control/modules/wled JSON POSTs — all small) keeps the existing buffered path
  untouched. Only `/api/file` POST gets the streaming branch.

`handleWriteFile` (the buffered version) can stay for the editor Save (small text) OR both routes
funnel through the streamed path. Simplest: the streamed path handles ALL `/api/file` POSTs (editor
Save included — it's just a small stream), and the old `strlen`/buffered `handleWriteFile` is
removed. Net: one write path, no size cap besides `kUploadMax`. (Subtraction — deletes the buffered
special-case + the kFileApiCap-on-write.)

## UI
- Raise/remove `FM_UPLOAD_CAP` — set it to the new `kUploadMax` (256 KB) or drop the client guard
  and let the device 413/400 a too-big file with a message. Keep reading as text for now (tier 1
  was text); a follow-up reads binary via ArrayBuffer. Actually: `file.text()` handles up to any
  size fine client-side — the cap was the device's. So just raise FM_UPLOAD_CAP to kUploadMax.
- The editor's binary read-only guard stays (textarea can't round-trip binary).

## kFileApiCap (the READ/serve side)
- Downloads + editor-load still use `static char fileBuf[kFileApiCap+1]` (8 KB) — a file larger
  than 8 KB currently serves truncated. Since uploads can now exceed 8 KB, a >8 KB file could exist
  and would DOWNLOAD truncated. So the read/serve path needs the same streaming treatment OR a
  bigger cap. Cleanest symmetric fix: stream the file → socket on GET too (read chunk → conn.write),
  no `fileBuf`. Do this in the same change so up/down are symmetric and neither truncates.

## Tests
- `fsWriteStream` round-trip (a multi-chunk source writes the full content, incl a NUL); a source
  that errors mid-stream leaves no partial file (atomic). Desktop unit.
- Byte-exact NUL test already covers content integrity; extend for >1 chunk.

## Gates + verify
- Build/ctest/scenarios/spec/esp32/kpi. Bench: upload the 19.6 KB deviceModels.json to the S3,
  re-download it, diff — must be byte-identical. Confirm a large file both up- and downloads whole.

## Open question for PO
- `kUploadMax` ceiling: 256 KB? (LittleFS state partition is ~384 KB–2 MB depending on board; a
  single config file is realistically < 64 KB. 256 KB is generous but bounded.)
