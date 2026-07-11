#pragma once

#include "core/MoonModule.h"
#include "core/BinaryBroadcaster.h"
#include "platform/platform.h"

#include <cstdint>

namespace mm {

// Forward declarations — bodies in HttpServerModule.cpp include the real headers.
class JsonSink;
class Scheduler;

/// Embedded HTTP server plus WebSocket — serves the web UI and the REST API that backs it.
/// Core infrastructure held to a **light-include-free** contract with one PO-accepted
/// exception: the WLED-compatibility shim's colour path uses `light/Palette.h`'s pure
/// hue/RGB↔palette-index conversions (`Palettes::nearestForRgb`, `Palettes::representativeRgb`),
/// the same sanctioned exception `MqttModule` documents at its top-of-file — routing a HomeKit /
/// HA WLED colour to a projectMM palette needs the palette set, which is inherently light-domain,
/// and a format conversion is the least-coupling way to bridge it (this module still drives the
/// palette through `Scheduler::setControl`, not a light object). No other light-domain include
/// is permitted here. Implementation lives in HttpServerModule.cpp; this header is the interface
/// only. The `port` control defaults to 8080 on desktop, 80 on ESP32.
///
/// **REST API:** `GET /` serves index.html and the UI assets (`/app.js`, `/style.css`,
/// `/moonlight-logo.png`). `GET /api/state` returns the full module-tree JSON (each entry
/// carries name, type, role, enabled, tickTimeUs, classSize, dynamicBytes, `controls[]`,
/// status + severity when set, and `userEditable:false` only when the module opts out of
/// UI delete/replace). `GET /api/system` returns fps, tickTimeUs, freeHeap, freeInternal,
/// maxBlock, uptime. `GET /api/types` returns the type catalog (stable factory `name`,
/// role-suffix-stripped `displayName`, `acceptsChildRoles`, and per-type `defaults` captured
/// from a fresh probe instance). Mutations: `POST /api/control` `{module,control,value}`,
/// `POST /api/modules` create, `POST /api/modules/{name}/move` reorder, `.../replace` swap,
/// `POST /api/reboot`, `DELETE /api/modules/{name}`. File Manager: `GET /api/dir?path=` lists a
/// directory, `POST /api/dir?path=` creates a folder, `DELETE /api/dir?path=` removes a file or
/// empty folder, `GET|POST /api/file?path=` reads / writes a file body (the path rides the query,
/// so a filesystem op carries its target in the request, not a stored control). All JSON responses
/// stream through a `JsonSink` — no fixed-buffer ceiling, so a tree of any size serialises correctly.
///
/// **WebSocket:** `GET /ws` with `Upgrade: websocket` does the RFC 6455 handshake (SHA-1 +
/// base64), up to 4 concurrent clients. Server pushes full state JSON as text frames from
/// `tick1s()`. Binary frames take two paths, both without a frame-sized buffer: a synchronous
/// stream (`beginBinaryFrame` / `pushBinaryFrame` / `endBinaryFrame`) for a forward-only
/// producer, and a resumable buffered send (`sendBufferedFrame`) that drains a memory-adaptive
/// chunk per client per `tick20ms` from a stable caller-owned buffer — so a large frame is
/// delivered over wall-clock ticks without spinning any loop, yet stays one atomic WS message.
/// One buffered send is in flight at a time (newest-wins backpressure: a new offer while one
/// is active is dropped). Clients send nothing back over WS; mutations go through REST.
///
/// **Hot-path split:** the resumable drain runs on `tick20ms` (the 20 ms transport poll),
/// deliberately NOT the per-render-tick `tick()`, so pushing preview bytes to the socket is
/// never charged to the LED render hot path. The LED output is never delayed by the preview;
/// the preview frame rate is instead bounded by the 20 ms drain cadence, which is the right
/// trade since the preview is a view and the LEDs are not.
///
/// **WLED-compatibility shim:** a small set of WLED-shaped messages make a projectMM device
/// appear in — and be controlled from — the native WLED apps (iOS / Android) and Home
/// Assistant's WLED integration. Discovery is over mDNS `_wled._tcp`; validation is a minimal
/// `GET /json/info` `{name, mac, leds{}, wifi{}, brand:"WLED", product:"MoonModules"}` (the
/// app keys on `brand:"WLED"` to accept it — we interoperate, not impersonate; this is NOT a
/// full WLED emulation). Live state is pushed over `/ws` as a `{state, info}` frame; `state`
/// mirrors the Drivers `brightness` control and the live first-LED RGB (falling back to
/// projectMM purple `[128,0,255]` when the first LED is off). Control is bidirectional over the
/// same `/ws`: the app's slider/toggle send a `{on?, bri?}` frame, read by
/// `pollWledStateFromWebSockets()` and applied to Drivers brightness through the shared
/// apply-core (the same `applySetControl` path REST and Improv use). The colour read is the
/// one place this core module reaches output state — `MoonModule::firstOutputRgb()` is a
/// domain-neutral virtual the light-domain Drivers overrides — keeping this module free of any
/// light-domain include.
///
/// **Cross-domain wiring:** this module exposes the `BinaryBroadcaster` interface; the
/// light-domain PreviewDriver holds a `BinaryBroadcaster*` and streams each frame's bytes
/// through it. `main.cpp` wires PreviewDriver's broadcaster to the HttpServerModule instance —
/// the only file that knows both. The preview's point budget and wire format are PreviewDriver's
/// concern.
///
/// The five `JsonSink&` helpers below are private members rather than free functions because
/// they all read `this->wsClients_`, `this->scheduler_`, or other module state, or call other
/// HttpServerModule members. Three pieces of this module's helpers live in their own headers:
/// JsonSink + jsonEscape() in core/JsonSink.h, sha1() (RFC 3174, WS handshake) in core/Sha1.h,
/// base64Encode() (WS handshake + Password obfuscation) in core/Base64.h — all in `namespace mm`
/// so the call sites are unchanged.
///
/// **Prior art:** the WLED-compatibility shim's exact field requirements were reverse-engineered
/// from the WLED-Android client by Christophe Gagnier (@Moustachauve,
/// https://github.com/Moustachauve/WLED-Android) — `DeviceDiscovery.kt` (mDNS browse),
/// `DeviceFirstContactService.kt` (the `/json/info` validation + non-empty `mac` check), the
/// Info/State Moshi models, and `WebsocketClient.kt` (live state over `/ws`, the `sendState`
/// control direction). Knowing precisely what the app reads is why the shim is the minimal
/// accepted object rather than a guessed full WLED emulation.
class HttpServerModule : public MoonModule, public BinaryBroadcaster {
public:
    uint16_t port = 8080;

    void setScheduler(Scheduler* s) { scheduler_ = s; }
    void setUiPath(const char* path) { uiPath_ = path; }

    /// BinaryBroadcaster — stream one binary WS frame to every connected client, pushed
    /// incrementally so no frame-sized buffer is held. Producers (PreviewDriver) push the
    /// payload bytes; this prepends the WS header. Domain-neutral: no knowledge of the content.
    void beginBinaryFrame(size_t totalLen) override;
    void pushBinaryFrame(const uint8_t* data, size_t len) override;
    bool endBinaryFrame() override;

    /// Resumable one-frame send from a stable caller-owned buffer (no copy), drained a bounded chunk
    /// per client per tick20ms (drainPreviewSend) so a large frame stays off this module's hot path;
    /// a would-block socket resumes next tick. See BinaryBroadcaster.
    bool sendBufferedFrame(const uint8_t* header, size_t headerLen,
                           const uint8_t* body, size_t bodyLen) override;
    bool bufferedSendIdle() const override { return !previewSend_.active; }
    void cancelBufferedSend() override { previewSend_.active = false; }
    /// Bumped on each new WS client (see handleWebSocketUpgrade). PreviewDriver watches it to
    /// re-stream its coordinate table the moment a fresh page connects, so a refresh shows the
    /// preview immediately.
    uint32_t clientGeneration() const override { return wsClientGeneration_; }

    /// Keep running even when "disabled" via the UI — otherwise the user has no way
    /// to re-enable themselves through the same UI.
    bool respectsEnabled() const override { return false; }

    /// Non-UI: this IS the server that renders /api/state — it doesn't list itself as a card.
    /// The "not a UI module" opt-out (shared with FilesystemModule), read by the state serializer's
    /// module loop to skip this module.
    bool appearsInUi() const override { return false; }

    void defineControls() override;
    void setup() override;
    void release() override;
    void tick20ms() override;
    void tick1s() override;

    // -----------------------------------------------------------------------
    // Transport-free apply-core — "the REST API, callable in-process"
    // -----------------------------------------------------------------------
    /// The add/set/clear-children operations the HTTP handlers do, factored out of
    /// the TcpConnection so any transport can drive them. Two callers today: the
    /// HTTP handlers (thin wrappers that map OpResult → status code) and the Improv
    /// serial path (ImprovProvisioningModule applies a pushed op on the main loop —
    /// "Improv = REST over serial"). One home for the apply logic; transports differ
    /// only in how they frame the request and report the result.
    enum class OpResult : uint8_t {
        Ok,
        AlreadyExists,   ///< add is a no-op: a module with this id is already in the tree (still success)
        ModuleNotFound,  ///< module / parent name not in the tree
        ControlNotFound, ///< module exists but has no such control (a distinct 404)
        UnknownType,     ///< factory doesn't know the type
        BadRequest,      ///< missing field, top-level add, parent rejected child
        OutOfRange,      ///< numeric value outside bounds
        Malformed,       ///< value didn't parse (such as an IPv4)
        ReadOnly,        ///< tried to write a display-only control
    };
    /// body is a small JSON object: `{"type","id","parent_id"}` / `{"module","control","value"}`.
    OpResult applyAddModule(const char* typeName, const char* id, const char* parentId);
    OpResult applySetControl(const char* moduleName, const char* controlName, const char* valueJson);
    /// Enumerate-then-DELETE every child of `parentName` (the catalog inject's
    /// replaceChildren). Returns NotFound if the parent doesn't exist, else Ok.
    OpResult applyClearChildren(const char* parentName);
    /// Parse a single REST op object (`{"op":"add|set|clearChildren", …}`) and dispatch
    /// to the three above. The wire shape the Improv APPLY_OP frame carries.
    OpResult applyOp(const char* opJson);

    /// Decode a `path=<rel>` query value into `out` (%XX + '+' decoding), rooted at the mount.
    /// Returns false on a missing/empty path, a `..` traversal, or an overlong (buffer-filling)
    /// value. The single filesystem-path guard shared by every fs HTTP entry (read/write/dir/
    /// mkdir/delete). Public + static so it's unit-testable without a socket fixture.
    static bool parseFilePath(const char* query, char* out, size_t cap);

    /// Apply a WLED `{on?, bri?}` state body onto the Drivers `on` / `brightness` controls through
    /// the shared apply-core (`on` and `bri` independent — off preserves the level). The transport-
    /// free entry the HTTP `POST /json/state`, the inbound-`/ws` path, and the unit tests all drive.
    void applyWledState(const char* body);

private:
    platform::TcpServer server_;
    Scheduler* scheduler_ = nullptr;
    const char* uiPath_ = "src/ui";

    static constexpr int MAX_WS_CLIENTS = 4;
    platform::TcpConnection wsClients_[MAX_WS_CLIENTS];
    uint32_t wsClientGeneration_ = 0;   // ++ on each new WS client; see clientGeneration()

    // begin/push/endBinaryFrame stream a binary WS frame straight to every client with NO
    // frame-sized buffer: the header goes out on begin, each pushed slice is fanned to all
    // clients, and end reports whether every client got the whole frame. A producer (PreviewDriver
    // streaming the producer buffer / forEachCoord) holds no copy. wsFrameAllSent_ tracks the
    // current frame's all-sent result across the push calls.
    bool wsFrameAllSent_ = true;
    // Max TOTAL WouldBlock spins for one span in sendAllOrClose before a stuck client is closed.
    // Used by the begin/push/end stream (coord table + downsampled colour frame); the full-res
    // colour frame goes through the resumable sendBufferedFrame instead, which never spins.
    static constexpr int kDirectSendSpins = 2000;

    // Resumable full-frame send (BinaryBroadcaster::sendBufferedFrame). One WS message = a copied
    // header + a pointer into the caller's STABLE body buffer (the PreviewDriver producer buffer),
    // drained a bounded chunk per client per tick20ms via writeSome — so a large frame is delivered
    // over wall-clock ticks without spinning any loop, yet stays ONE atomic WS message to the
    // browser. One in flight at a time (drop-new: a frame offered while one is active is rejected,
    // the in-flight one kept). The caller calls cancelBufferedSend() before freeing/reallocating the
    // body (a geometry rebuild), so a cursor never reads freed memory.
    struct PreviewSend {
        uint8_t hdr[16] = {};                 // WS + app header, copied (caller's may be a stack local)
        size_t hdrLen = 0;
        const uint8_t* body = nullptr;        // caller-owned, stable until done/cancelled — NOT copied
        size_t bodyLen = 0;
        size_t sent[MAX_WS_CLIENTS] = {};     // per-client cursor over [hdr ++ body]; a slow client lags
        bool active = false;
    };
    PreviewSend previewSend_;
    // Drain one memory-adaptive chunk per client of the in-flight resumable send; mark it done when
    // every live client has the whole frame. Called from tick20ms. No-op when none is active.
    void drainPreviewSend();
    // Largest chunk to push per client per drain tick, derived from free contiguous memory so a
    // tight board takes small bites (bounded tick occupancy) and a roomy board drains fast.
    size_t previewChunkBytes() const;

    // All JSON API responses (/api/state, /api/types, /api/system) and the WS
    // state push stream through a JsonSink — no shared fixed-size buffer.

    // XOR key for Password-control obfuscation in /api/state. NOT a secret — the
    // same value lives in src/ui/app.js (PW_XOR_KEY). This only stops the
    // password being plainly readable in a raw API response; it is trivially
    // reversible by design (see the ControlType::Password serialization).
    static constexpr uint8_t PASSWORD_XOR_KEY = 0x5A;

    // -----------------------------------------------------------------------
    // HTTP handling
    // -----------------------------------------------------------------------
    void handleConnection(platform::TcpConnection& conn);
    void sendResponse(platform::TcpConnection& conn, int status, const char* contentType, const char* body);
    void sendPreflightResponse(platform::TcpConnection& conn);
    void serveFile(platform::TcpConnection& conn, const char* filename, const char* contentType);

    // File Manager: read/write an arbitrary filesystem path (the /api/file endpoints). `query` is
    // the request's query string, read for `path=<rel>`; the path is vetted (no traversal, rooted
    // at the mount) and size-capped. A file body isn't a control value, so these are their own
    // endpoints rather than /api/control.
    void serveFileContents(platform::TcpConnection& conn, const char* query);
    // Streamed atomic upload: `initialBody`/`initialLen` are the body bytes already in the request
    // buffer; `contentLen` is the declared total. Pulls any remainder off the socket → fsWriteStream,
    // so an upload of any size streams to the file (rejected if it exceeds kUploadMax or free space).
    void handleWriteFile(platform::TcpConnection& conn, const char* query,
                         const char* initialBody, size_t initialLen, size_t contentLen);
    // File Manager: one directory's children as JSON (the /api/dir endpoint) — the source the lazy
    // tree loads a node's children from. Single-level; `hidden=1` in the query includes dotfiles.
    void serveDirListing(platform::TcpConnection& conn, const char* query);
    void handleMakeDir(platform::TcpConnection& conn, const char* query);      // POST /api/dir?path=
    void handleRemoveEntry(platform::TcpConnection& conn, const char* query);  // DELETE /api/dir?path=

    // -----------------------------------------------------------------------
    // JSON state
    // -----------------------------------------------------------------------
    void serveState(platform::TcpConnection& conn);
    void buildStateJson(JsonSink& sink);
    void writeModuleJson(JsonSink& sink, MoonModule* mod);
    void writeControls(JsonSink& sink, MoonModule* mod);
    // Emit `,"status":"…","severity":"…"` for a module that has a status set;
    // no-op when status is null. Shared by writeModuleJson (/api/state) and
    // writeModuleMetricsJson (/api/system) so the two endpoints stay in sync.
    static void writeStatus(JsonSink& sink, MoonModule* mod);

    // -----------------------------------------------------------------------
    // Control setter
    // -----------------------------------------------------------------------
    void handleSetControl(platform::TcpConnection& conn, const char* body);

    // Find a module anywhere in the scheduler's tree by its name — null-guards scheduler_
    // then delegates to Scheduler::firstByName (the one canonical tree-walk).
    MoonModule* findModuleByName(const char* name);

    // -----------------------------------------------------------------------
    // System metrics
    // -----------------------------------------------------------------------
    void serveSystem(platform::TcpConnection& conn);
    /// WLED-compatibility shim — see the class comment + the /json/info route. /json/info
    /// lists the device; /json/state + /json/si carry on/brightness/colour for the card;
    /// POST /json/state maps the app's toggle + slider onto Drivers brightness.
    void serveWledInfo(platform::TcpConnection& conn);
    void serveWledState(platform::TcpConnection& conn);
    void serveWledStateInfo(platform::TcpConnection& conn);
    void serveWledDeviceJson(platform::TcpConnection& conn);   ///< /json — HA WLED integration surface
    void handleWledState(platform::TcpConnection& conn, const char* body);
    void pollWledStateFromWebSockets();             ///< read app's slider/toggle sent over /ws
    void writeWledInfoBody(JsonSink& sink, const char* name, const uint8_t mac[6]);
    void writeWledName(JsonSink& sink, const char* name);   // 💫-prefixed WLED name (HA marker)
    void writeWledStateBody(JsonSink& sink);
    /// Resolve device identity for the WLED shim: `deviceName` (from SystemModule) → `name`,
    /// live IPv4 (Ethernet first, WiFi fallback) → `ip`, MAC → `mac`. Extracted so the
    /// /json/info, /json/si and /json handlers share one lookup instead of hand-copying the
    /// same four platform calls. `nameFallback` defaults to `"projectMM"` (the value the
    /// handlers used before extraction). Both `ip` and `mac` are written in place; `name`
    /// points at the SystemModule string when present (which outlives the request).
    void resolveWledIdentity(const char*& name, uint8_t mac[6], uint8_t ip[4],
                             const char* nameFallback = "projectMM");
    void writeModuleMetricsJson(JsonSink& sink, MoonModule* mod, bool& first);

    // -----------------------------------------------------------------------
    // Module CRUD
    // -----------------------------------------------------------------------
    void handleAddModule(platform::TcpConnection& conn, const char* body);
    void handleDeleteModule(platform::TcpConnection& conn, const char* moduleName);
    void handleReplaceModule(platform::TcpConnection& conn, const char* moduleName, const char* body);
    void serveTypes(platform::TcpConnection& conn);
    void writeTypeDefaults(JsonSink& sink, const char* typeName);
    void handleMoveModule(platform::TcpConnection& conn, const char* moduleName, const char* body);
    void handleReboot(platform::TcpConnection& conn);
    /// OTA: `POST /api/firmware/url` body=`{"url":"..."}`. Body parsed; URL handed
    /// to platform::http_fetch_to_ota which spawns a task and returns. Caller
    /// gets 202 immediately; progress streams via FirmwareUpdateModule controls.
    void handleFirmwareUrl(platform::TcpConnection& conn, const char* body);
    void handleFirmwareUpload(platform::TcpConnection& conn, const char* initialBody,
                              size_t initialLen, size_t contentLen);   // POST /api/firmware/upload

    // -----------------------------------------------------------------------
    // WebSocket
    // -----------------------------------------------------------------------
    void handleWebSocketUpgrade(platform::TcpConnection& conn, const char* req);
    void pushStateToWebSockets();
    void pushWledStateToWebSockets();   // WLED-app {state,info} frame on /ws (see impl)
    static bool sendWsTextFrame(platform::TcpConnection& conn, const char* data, int len);
    // Write the whole span to one client via repeated non-blocking writeSome; close it + return
    // false if it can't all go (a stuck/too-slow client). The push primitive behind begin/push/end.
    static bool sendAllOrClose(platform::TcpConnection& ws, const uint8_t* data, size_t len);
};

} // namespace mm
