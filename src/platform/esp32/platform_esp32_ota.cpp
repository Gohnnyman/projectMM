// OTA — fetch firmware from a URL and flash it to the next OTA partition.
//
// Cut out of platform_esp32.cpp (plan-23) for size + readability. The
// file owns the OtaTaskParams + otaTask shape in an anonymous namespace;
// the rest of the platform layer talks to it only through the public
// mm::platform::http_fetch_to_ota symbol declared in platform.h. Move
// was a code-organisation change with no API delta.

#include "platform/platform.h"

#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"   // esp_partition_find_first: locating MoonBase
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_heap_caps.h"   // heap_caps_malloc/free — the upload chunk buffer
#include "esp_log.h"
#include "nvs.h"           // moonbaseStageInstallUrl: the URL handoff to MoonBase

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>        // unique_ptr — frees the upload buffer on every exit path
#include <new>           // std::nothrow for the OtaTaskParams alloc below

namespace mm::platform {

// One upload chunk. 4 KB matches the flash page granularity esp_ota_write prefers and is
// the size the HTTP path already streams in.
constexpr size_t kOtaChunkBytes = 4096;

namespace {

// Heap-allocated task parameters. Task owns this and frees it on exit.
struct OtaTaskParams {
    char url[512];
    char* statusBuf;
    size_t statusBufLen;
    uint32_t* bytesReadOut;   // current bytes downloaded
    uint32_t* bytesTotalOut;  // image size; 0 until esp_https_ota reports it
};

// Write to the status buffer with bounded length. snprintf truncates safely.
void otaSetStatus(OtaTaskParams* p, const char* fmt, ...) {
    if (!p->statusBuf || p->statusBufLen == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(p->statusBuf, p->statusBufLen, fmt, args);
    va_end(args);
}

void otaTask(void* arg) {
    auto* p = static_cast<OtaTaskParams*>(arg);

    otaSetStatus(p, "downloading");
    *p->bytesReadOut = 0;
    *p->bytesTotalOut = 0;   // unknown until esp_https_ota reports it

    // `esp_crt_bundle_attach` enables the bundled-trust-anchor mode for TLS verification — the same
    // mechanism Chrome/curl use for general HTTPS (api.github.com, objects.githubusercontent.com, …).
    // No baked cert. It's attached unconditionally: for an https URL it verifies the server; for a
    // plain-http LAN OTA (MoonDeck serving a local build) it goes unused, but its presence satisfies
    // esp_https_ota_begin's "server verification enabled" check, so the fetch proceeds over plain TCP.
    esp_http_client_config_t http_config = {};
    http_config.url = p->url;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.timeout_ms = 10000;
    // GitHub release-asset URLs 302-redirect to objects.githubusercontent.com.
    // Default redirect handling is off in esp_http_client; force-follow.
    http_config.disable_auto_redirect = false;
    http_config.max_redirection_count = 10;
    // ESP-IDF's default HTTP header buffer is 512 bytes per direction. GitHub's
    // 302 redirect response includes a multi-KB `content-security-policy`
    // header that overflows it ("HTTP_CLIENT: Out of buffer") and the OTA
    // fails before the .bin download even starts. Raising both sides to 4 KB
    // covers GitHub's longest headers with room to spare; the cost is ~7 KB
    // of heap during the OTA fetch, freed when the OTA task exits.
    http_config.buffer_size = 4096;
    http_config.buffer_size_tx = 4096;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;
    // Performs partial-image-write + commit + boot-pointer flip internally.

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        // esp_https_ota_begin collapses ~6 distinct failures (DNS, TLS,
        // HTTP, partition init, header-buffer overflow) into one ESP_FAIL,
        // so the only useful detail is in the IDF log on the serial console.
        // We surface the IDF error name plus a pointer to the log.
        otaSetStatus(p, "error: ota begin %s (see serial log)",
                     esp_err_to_name(err));
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    if (total > 0) {
        // Publish the real total so the UI can render "X KB / Y KB".
        // FirmwareUpdateModule's tick1s() rebuildControls picks this up on
        // the next 1 Hz poll (re-binds the progress descriptor with the new
        // total snapshot).
        *p->bytesTotalOut = static_cast<uint32_t>(total);
    }
    otaSetStatus(p, "flashing");

    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int got = esp_https_ota_get_image_len_read(handle);
        if (got >= 0) *p->bytesReadOut = static_cast<uint32_t>(got);
    }
    if (err != ESP_OK) {
        otaSetStatus(p, "error: ota perform %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        otaSetStatus(p, "error: incomplete download");
        esp_https_ota_abort(handle);
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        // After finish, abort isn't valid — handle is consumed. Surface and exit.
        otaSetStatus(p, "error: ota finish %s", esp_err_to_name(err));
        delete p;
        vTaskDelete(nullptr);
        return;
    }

    // Final byte count match — pull from the OTA handle one last time so the
    // UI's last frame before reboot shows a clean "Y KB / Y KB".
    if (*p->bytesTotalOut > 0) *p->bytesReadOut = *p->bytesTotalOut;
    otaSetStatus(p, "rebooting");
    delete p;
    // 600 ms delay gives the HTTP response time to make it back to the browser
    // before the device drops the socket on restart.
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

}  // anonymous namespace

bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    if (!url || !statusBuf || statusBufLen == 0 || !bytesReadOut || !bytesTotalOut) {
        return false;
    }

    // Reject oversize URLs explicitly rather than silently truncating with
    // strncpy — a truncated URL almost always 404s or fetches the wrong
    // file, with no clue in the status surface.
    size_t urlLen = std::strlen(url);
    constexpr size_t kUrlMax = sizeof(OtaTaskParams::url) - 1;
    if (urlLen > kUrlMax) {
        std::snprintf(statusBuf, statusBufLen,
                      "error: url too long (%zu > %zu)", urlLen, kUrlMax);
        return false;
    }

    // std::nothrow so OOM doesn't abort the process. Status string carries
    // the failure back to the route, which returns 500 to the browser.
    auto* p = new (std::nothrow) OtaTaskParams{};
    if (!p) {
        std::snprintf(statusBuf, statusBufLen, "error: out of memory");
        return false;
    }
    std::memcpy(p->url, url, urlLen + 1);   // includes NUL; size already verified
    p->statusBuf = statusBuf;
    p->statusBufLen = statusBufLen;
    p->bytesReadOut = bytesReadOut;
    p->bytesTotalOut = bytesTotalOut;

    // 12 KB stack matches v1's working number (TLS handshake + HTTPS body
    // buffering inside esp_https_ota). Priority 5 = above idle, below
    // FreeRTOS critical drivers.
    BaseType_t ok = xTaskCreate(&otaTask, "urlOta", 12288, p, 5, nullptr);
    if (ok != pdPASS) {
        otaSetStatus(p, "error: task create failed");
        delete p;
        return false;
    }
    return true;
}


bool otaWriteStream(FsWriteSrc src, void* user, size_t contentLen,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut) {
    if (!src || !statusBuf || statusBufLen == 0 || !bytesReadOut) return false;
    auto setStatus = [&](const char* fmt, auto... a) {
        std::snprintf(statusBuf, statusBufLen, fmt, a...);
    };

    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) { setStatus("error: no OTA partition"); return false; }

    // SINGLE-SLOT GUARD. esp_ota_get_next_update_partition iterates only OTA subtypes and falls
    // back to the FIRST ota slot it finds, so on a table with one ota_0 (the MoonBase layout) it
    // hands back the partition we are executing from. Erasing that is a brick mid-flash. IDF also
    // refuses it inside esp_ota_begin (ESP_ERR_OTA_PARTITION_CONFLICT), but failing here says WHY
    // and names the fix. On a dual-OTA table this never fires.
    if (part == esp_ota_get_running_partition()) {
        setStatus("error: one app slot, reboot to MoonBase first");
        return false;
    }
    // SIZE GUARD. Without it an oversized image fails partway through esp_ota_write, leaving the
    // target slot half-written and the user staring at a generic write error. Content-Length is
    // advisory, so this only fires when the caller knows the size.
    if (contentLen && contentLen > part->size) {
        setStatus("error: image too large (%u > %u)",
                  static_cast<unsigned>(contentLen), static_cast<unsigned>(part->size));
        return false;
    }

    setStatus("flashing");
    esp_ota_handle_t handle = 0;
    // OTA_SIZE_UNKNOWN: the upload streams, so we don't pre-declare the exact size (Content-Length
    // is advisory for the UI); esp_ota_begin erases lazily as writes arrive.
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) { setStatus("error: ota begin %s", esp_err_to_name(err)); return false; }

    // Pull the upload body chunk-by-chunk and write each into the partition — the same producer
    // callback fsWriteStream drives, here feeding esp_ota_write instead of a file. `abort` from the
    // caller (an incomplete/timed-out upload) fails the OTA, and esp_ota_abort discards the partial.
    // Heap, not static, and not the stack either. 4 KB is far too large for a task frame, but as a
    // `static` it held 4 KB of INTERNAL RAM from boot to power-off for a buffer used only while a
    // firmware image is uploading — minutes of the device's life at most, and never at all on a
    // device that is never updated. An OTA is the one moment when spare RAM is least scarce (the
    // render path is the only other big consumer), so allocating here and freeing at every exit
    // costs nothing and gives the 4 KB back to WiFi and the HTTP stack for the other 99.9% of
    // uptime. Surfaced by check_footprint's STATIC column: this file read 1016 B of code against
    // 4096 B of static.
    //
    // Failing the alloc aborts the OTA cleanly rather than proceeding — a firmware write with no
    // buffer is not something to degrade around.
    // unique_ptr, not a raw malloc: there are six exit paths below (abort, write error, truncated
    // upload, three esp_ota failures) and a leak on any of them would be a 4 KB hole per attempt.
    const std::unique_ptr<uint8_t, decltype(&heap_caps_free)> owned(
        static_cast<uint8_t*>(heap_caps_malloc(kOtaChunkBytes, MALLOC_CAP_8BIT)), &heap_caps_free);
    uint8_t* const buf = owned.get();
    if (!buf) {
        setStatus("error: out of memory for the upload buffer");
        esp_ota_abort(handle);
        return false;
    }
    uint32_t written = 0;
    for (;;) {
        bool abort = false;
        const size_t n = src(reinterpret_cast<char*>(buf), kOtaChunkBytes, user, &abort);
        if (abort) {
            setStatus("error: upload aborted");
            esp_ota_abort(handle);
            return false;
        }
        if (n == 0) break;   // clean EOF — whole body delivered
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            setStatus("error: ota write %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            return false;
        }
        written += static_cast<uint32_t>(n);
        *bytesReadOut = written;
    }
    // Guard a truncated upload: if the client sent fewer bytes than Content-Length, the image is
    // incomplete — don't commit a half-image. (contentLen 0 = unknown; skip the check then.)
    if (contentLen && written < contentLen) {
        setStatus("error: incomplete upload (%u/%u)",
                  static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
        esp_ota_abort(handle);
        return false;
    }

    err = esp_ota_end(handle);   // validates the image (magic/checksum); consumes the handle
    if (err != ESP_OK) { setStatus("error: ota end %s", esp_err_to_name(err)); return false; }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) { setStatus("error: set boot %s", esp_err_to_name(err)); return false; }

    setStatus("rebooting");
    // Image committed + boot pointer flipped. Return to the caller so it can send its HTTP 200
    // BEFORE the reboot (the caller closes the socket + reboots, same sequence as /api/reboot) —
    // that's what lets the browser see a clean "flashed" response instead of an aborted socket.
    return true;
}

// Does this device's partition table carry a factory app? True on the MoonBase layout used by
// the 4 MB boards, false on the dual-OTA tables. The caller uses it to decide whether an update
// needs the reboot-into-MoonBase hop, and the UI to say which kind of device this is.
bool otaHasMoonBase() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr) != nullptr;
}

// Point the bootloader at MoonBase and report whether it took. Returns false when the table has
// no factory partition, which tells the caller this device updates in place.
// NB esp_ota_set_boot_partition on a factory partition ERASES otadata rather than writing a
// sequence number: that is what makes a power cut mid-update land back in MoonBase rather than in
// a half-written app.
bool otaBootMoonBase() {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (!part) return false;
    return esp_ota_set_boot_partition(part) == ESP_OK;
}

// Is the device currently executing FROM MoonBase?
bool otaRunningMoonBase() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    return run && run->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
}

// Stage the install URL in NVS for MoonBase to consume on its next boot (see platform.h).
bool moonbaseStageInstallUrl(const char* url) {
    if (!url || !url[0]) return false;
    nvs_handle_t h;
    if (nvs_open("moonbase", NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_str(h, "url", url) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

} // namespace mm::platform
