// MoonBase: the second boot image.
//
// A 4 MB board has room for one application, not two, so its partition table carries this small
// image in the `factory` slot instead of a second copy of the firmware. When the application must
// be replaced, the device reboots here: MoonBase owns the board, writes the new firmware into the
// application slot it is not itself running from, and hands control back.
//
// Everything here is written directly against ESP-IDF. It shares no code with the application on
// purpose: the app's platform layer pulls in RMT, I2S, PSRAM and the JIT, which measured 788 KB
// with an empty entry point. This file plus its sdkconfig measures around a quarter of the flash
// instead. The other half of the budget is in ../sdkconfig.defaults, which is part of the design.
//
// The flow, in order:
//   1. mount the application's filesystem read-only and read the stored WiFi credentials
//   2. bring up the network: Ethernet if the board has it, else WiFi STA, else our own AP
//   3. serve one page: install by upload, or install from a URL
//   4. write the application slot, point the bootloader at it, reboot

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_littlefs.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "nvs_flash.h"

namespace {

// The application writes its config as /.config/<TypeName>.json on a LittleFS volume. Tables
// written from 2026-08 label that partition `littlefs`; older ones label it `spiffs`, so both are
// tried by subtype then label. MoonBase only ever READS it, so a failed install cannot
// corrupt user config.
struct FsCandidate { esp_partition_subtype_t subtype; const char* label; };
constexpr FsCandidate kFsCandidates[] = {
    {ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "littlefs"},
    {ESP_PARTITION_SUBTYPE_DATA_SPIFFS,   "spiffs"},
};
constexpr const char* kFsMountPoint     = "/fs";
constexpr const char* kNetworkConfig    = "/fs/.config/NetworkModule.json";

// The AP fallback address matches the application's (NetworkModule uses 4.3.2.1), so a user who
// has provisioned this device before sees the same address in both firmwares.
constexpr const char* kApAddress = "4.3.2.1";
constexpr const char* kApName    = "MoonBase";

constexpr int kHttpPort = 80;

char ssid_[64] = {};
char password_[64] = {};
char status_[96] = "idle";

EventGroupHandle_t netEvents_;
constexpr int kNetGotIp = BIT0;

// ---------------------------------------------------------------------------------------------
// Credentials
// ---------------------------------------------------------------------------------------------

// Extract one top-level string value from the config JSON. Deliberately not a JSON parser:
// MoonBase reads exactly two known keys out of a file this project itself wrote, and linking a
// parser to do it would cost more than the whole feature. The scan is anchored on `"key":"` at
// the TOP level only, which matters because the same file carries a child module's "0.password"
// (the MQTT broker's) that a naive substring search would find first.
bool jsonFindString(const char* json, const char* key, char* out, size_t outLen) {
    char needle[40];
    const int n = std::snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(needle)) return false;
    const char* p = std::strstr(json, needle);
    if (!p) return false;
    p += n;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outLen) {
        char c = *p++;
        if (c == '\\' && *p) {
            // The app's writer (JsonSink, RFC 8259) escapes with \" \\ \/ \n \r \t, and
            // \uXXXX for other control bytes. All but \u are decoded here; a credential
            // holding a raw control byte fails the join and lands on the access point,
            // visible and recoverable, which is not worth a \u decoder in this image.
            const char e = *p++;
            switch (e) {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': return false;
                default:  c = e;      // \" \\ \/ decode to the char itself
            }
        }
        out[i++] = c;
    }
    out[i] = '\0';
    return i > 0;
}

// Read the stored WiFi credentials, if there are any. Absent, unreadable or empty all mean the
// same thing to the caller: fall through to the access point.
void loadCredentials() {
    const char* label = nullptr;
    for (const auto& c : kFsCandidates) {
        if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, c.subtype, c.label)) continue;
        esp_vfs_littlefs_conf_t conf = {};
        conf.base_path = kFsMountPoint;
        conf.partition_label = c.label;
        conf.format_if_mount_failed = false;   // never format: this volume is the user's config
        if (esp_vfs_littlefs_register(&conf) == ESP_OK) { label = c.label; break; }
    }
    if (!label) return;

    FILE* f = std::fopen(kNetworkConfig, "r");
    if (f) {
        // The credentials are the first keys the module writes, so a bounded prefix read finds
        // them without holding the whole file (which carries every child module's config too).
        // The bound is a cross-image contract with NetworkModule's control order; the app pins
        // it with a unit test (unit_MoonBaseContract).
        char buf[1024];
        const size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = '\0';
        std::fclose(f);
        jsonFindString(buf, "ssid", ssid_, sizeof(ssid_));
        jsonFindString(buf, "password", password_, sizeof(password_));
    }
    esp_vfs_littlefs_unregister(label);
}

// ---------------------------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------------------------

void onGotIp(void*, esp_event_base_t, int32_t, void*) {
    xEventGroupSetBits(netEvents_, kNetGotIp);
}

void onWifiEvent(void*, esp_event_base_t, int32_t id, void*) {
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
}

// Try the stored credentials for a bounded time. Returns whether an address arrived.
bool wifiStation(uint32_t waitMs) {
    if (!ssid_[0]) return false;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;

    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid_, sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), password_, sizeof(cfg.sta.password) - 1);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr, nullptr);
    esp_wifi_start();

    const EventBits_t bits = xEventGroupWaitBits(netEvents_, kNetGotIp, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(waitMs));
    if (bits & kNetGotIp) return true;
    esp_wifi_stop();
    esp_wifi_deinit();
    return false;
}

// The last resort, and the reason SoftAP stays in the size budget: a board whose stored
// credentials no longer work is still reachable without a cable.
bool wifiAccessPoint() {
    esp_netif_t* ap = esp_netif_create_default_wifi_ap();
    if (!ap) return false;
    esp_netif_ip_info_t ip = {};
    ip.ip.addr = esp_ip4addr_aton(kApAddress);
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
    esp_netif_dhcps_stop(ap);
    esp_netif_set_ip_info(ap, &ip);
    esp_netif_dhcps_start(ap);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.ap.ssid), kApName, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(kApName));
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_OPEN;   // an open AP: the user is standing at the device
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    return esp_wifi_start() == ESP_OK;
}

// ---------------------------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------------------------

// The one page MoonBase serves. Inline and tiny: no filesystem read, no compression, no assets.
const char kPage[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>MoonBase</title>"
    "<style>body{font:16px system-ui;margin:2rem;max-width:34rem;line-height:1.5}"
    "h1{font-size:1.3rem;margin-bottom:.2rem}.sub{color:#666;margin-top:0}"
    "input,button{font:inherit;padding:.5rem}"
    "section{margin:1.5rem 0;padding:1rem;border:1px solid #ccc;border-radius:.5rem}"
    "#s{margin-top:1rem;font-variant-numeric:tabular-nums}</style>"
    "<h1>MoonBase</h1><p class=sub>Install firmware to return this device to normal operation.</p>"
    "<section><b>From a file</b><br><input type=file id=f accept=.bin>"
    "<button onclick=up()>Install</button></section>"
    "<section><b>From a URL</b><br><input id=u size=34 placeholder=https://...>"
    "<button onclick=url()>Install</button></section>"
    "<section><b>Back to the app</b><br>Boot the installed firmware without changing it."
    "<br><button onclick=ba()>Boot the app</button></section>"
    "<div id=s></div>"
    "<script>"
    "const S=t=>document.getElementById('s').textContent=t;"
    // Surface the last install status on load: after a failed unattended install the user lands
    // here, and the page should say what went wrong rather than look freshly booted.
    "fetch('/moonbase').then(r=>r.text()).then(t=>{if(t&&t!='idle')S(t)}).catch(()=>{});"
    // The file is sent as the RAW request body, not multipart: the device then writes bytes
    // straight to flash with no boundary parsing, which is a meaningful saving in an image this
    // size and matches how the application's own upload route works.
    "async function up(){const f=document.getElementById('f').files[0];if(!f)return;"
    "S('installing '+(f.size/1024|0)+' KB...');"
    "const r=await fetch('/install',{method:'POST',body:f});"
    "S(await r.text());}"
    "async function url(){const u=document.getElementById('u').value;if(!u)return;"
    "S('downloading...');"
    "const r=await fetch('/install-url',{method:'POST',body:u});"
    "S(await r.text());}"
    "async function ba(){const r=await fetch('/boot-app',{method:'POST'});S(await r.text());"
    "if(r.ok)setTimeout(()=>location.reload(),8000);}"
    "</script>";

// The application slot. From the factory partition esp_ota_get_next_update_partition returns the
// first OTA slot, which is the one we want and is never the one we are running from.
const esp_partition_t* appPartition() {
    return esp_ota_get_next_update_partition(nullptr);
}

// Write a firmware image pulled from `url` straight into the application slot. This is what makes
// an unattended install possible: point MoonBase at a release asset and it fetches it itself.
bool installFromUrl(const char* url) {
    esp_http_client_config_t http = {};
    http.url = url;
    http.timeout_ms = 20000;
    http.keep_alive_enable = true;
    http.crt_bundle_attach = esp_crt_bundle_attach;   // GitHub and friends are HTTPS
    esp_https_ota_config_t ota = {};
    ota.http_config = &http;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t beginErr = esp_https_ota_begin(&ota, &handle);
    if (beginErr != ESP_OK) {
        // Numeric on purpose: the error-name table is compiled out for size
        // (ESP_ERR_TO_NAME_LOOKUP=n), so esp_err_to_name would say "UNKNOWN ERROR".
        std::snprintf(status_, sizeof(status_), "error: cannot start the download (0x%x)",
                      static_cast<unsigned>(beginErr));
        return false;
    }
    esp_err_t err;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        std::snprintf(status_, sizeof(status_), "downloading: %d bytes",
                      esp_https_ota_get_image_len_read(handle));
    }
    if (err != ESP_OK || esp_https_ota_finish(handle) != ESP_OK) {
        std::snprintf(status_, sizeof(status_), "error: the download failed");
        return false;
    }
    std::snprintf(status_, sizeof(status_), "installed, restarting");
    return true;
}


// ---------------------------------------------------------------------------------------------
// The HTTP server
// ---------------------------------------------------------------------------------------------
//
// Hand-written on raw sockets rather than esp_http_server: MoonBase serves one page and receives
// one file, and the component would cost more than the handlers do. One connection at a time is
// the right model here, since installing firmware is exclusive by nature.

constexpr size_t kRecvChunk = 4096;

void sendAll(int sock, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int n = ::send(sock, data + sent, len - sent, 0);
        if (n <= 0) return;   // peer gone: the caller is finishing anyway
        sent += static_cast<size_t>(n);
    }
}

void sendResponse(int sock, const char* status, const char* type, const char* body) {
    char head[160];
    const int n = std::snprintf(head, sizeof(head),
                                "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                                "Connection: close\r\n\r\n",
                                status, type, static_cast<unsigned>(std::strlen(body)));
    if (n > 0) sendAll(sock, head, static_cast<size_t>(n));
    sendAll(sock, body, std::strlen(body));
}

// Write `contentLen` bytes from the socket into the application slot. `prefix` carries whatever
// arrived in the same read as the headers.
bool installFromSocket(int sock, const char* prefix, size_t prefixLen, size_t contentLen) {
    const esp_partition_t* part = appPartition();
    if (!part) { std::snprintf(status_, sizeof(status_), "error: no app partition"); return false; }
    if (contentLen == 0 || contentLen > part->size) {
        std::snprintf(status_, sizeof(status_), "error: image is %u bytes, the slot holds %u",
                      static_cast<unsigned>(contentLen), static_cast<unsigned>(part->size));
        return false;
    }

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(part, contentLen, &handle) != ESP_OK) {
        std::snprintf(status_, sizeof(status_), "error: cannot start the install");
        return false;
    }

    size_t written = 0;
    if (prefixLen) {
        if (esp_ota_write(handle, prefix, prefixLen) != ESP_OK) {
            esp_ota_abort(handle);
            std::snprintf(status_, sizeof(status_), "error: write failed");
            return false;
        }
        written = prefixLen;
    }

    char* buf = static_cast<char*>(std::malloc(kRecvChunk));
    if (!buf) { esp_ota_abort(handle); std::snprintf(status_, sizeof(status_), "error: out of memory"); return false; }
    while (written < contentLen) {
        const size_t want = (contentLen - written) < kRecvChunk ? (contentLen - written) : kRecvChunk;
        const int n = ::recv(sock, buf, want, 0);
        if (n <= 0) break;                       // the upload was cut short
        if (esp_ota_write(handle, buf, static_cast<size_t>(n)) != ESP_OK) {
            std::free(buf);
            esp_ota_abort(handle);
            std::snprintf(status_, sizeof(status_), "error: write failed");
            return false;
        }
        written += static_cast<size_t>(n);
    }
    std::free(buf);

    if (written != contentLen) {
        esp_ota_abort(handle);
        std::snprintf(status_, sizeof(status_), "error: upload ended early (%u of %u bytes)",
                      static_cast<unsigned>(written), static_cast<unsigned>(contentLen));
        return false;
    }
    // esp_ota_end validates the image (magic and checksum) before we ever point the bootloader at
    // it, which is what makes a power cut mid-write safe: otadata still names MoonBase.
    if (esp_ota_end(handle) != ESP_OK) {
        std::snprintf(status_, sizeof(status_), "error: the image is not valid firmware");
        return false;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        std::snprintf(status_, sizeof(status_), "error: cannot set the boot partition");
        return false;
    }
    std::snprintf(status_, sizeof(status_), "installed, restarting");
    return true;
}

// Read the request head, dispatch, and (on a successful install) restart into the application.
void serveOne(int sock) {
    // TCP does not coalesce: the header block (or a small body) can arrive in several
    // segments, so read until the blank line is seen, bounded by the buffer. A request
    // whose headers do not fit 1023 bytes is not one of ours and falls out as 404.
    char head[1024];
    size_t got = 0;
    const char* bodyStart = nullptr;
    while (got < sizeof(head) - 1) {
        const int n = ::recv(sock, head + got, sizeof(head) - 1 - got, 0);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
        head[got] = '\0';
        if ((bodyStart = std::strstr(head, "\r\n\r\n"))) break;
    }
    if (got == 0) return;
    head[got] = '\0';
    const size_t headLen = bodyStart ? static_cast<size_t>(bodyStart + 4 - head) : got;
    size_t prefixLen = got - headLen;

    size_t contentLen = 0;
    if (const char* cl = std::strstr(head, "Content-Length:")) {
        contentLen = static_cast<size_t>(std::strtoul(cl + 15, nullptr, 10));
    }

    bool installed = false;
    if (std::strncmp(head, "POST /install-url", 17) == 0) {
        // The body is the URL itself; small enough to finish reading into the same buffer.
        while (prefixLen < contentLen && headLen + prefixLen < sizeof(head) - 1) {
            const int n = ::recv(sock, head + headLen + prefixLen,
                                 sizeof(head) - 1 - headLen - prefixLen, 0);
            if (n <= 0) break;
            prefixLen += static_cast<size_t>(n);
        }
        char url[256] = {};
        const size_t n = prefixLen < sizeof(url) - 1 ? prefixLen : sizeof(url) - 1;
        std::memcpy(url, head + headLen, n);
        installed = installFromUrl(url);
        sendResponse(sock, installed ? "200 OK" : "500 Internal Server Error", "text/plain", status_);
    } else if (std::strncmp(head, "POST /install", 13) == 0) {
        installed = installFromSocket(sock, head + headLen, prefixLen, contentLen);
        sendResponse(sock, installed ? "200 OK" : "500 Internal Server Error", "text/plain", status_);
    } else if (std::strncmp(head, "POST /boot-app", 14) == 0) {
        // Switch back to the installed application without installing anything.
        // esp_ota_set_boot_partition validates the image first, so a half-written app is
        // refused and the device stays here: only a bootable app can be booted.
        const esp_partition_t* app = appPartition();
        const bool ok = app && esp_ota_set_boot_partition(app) == ESP_OK;
        if (ok) std::snprintf(status_, sizeof(status_), "booting the app");
        else    std::snprintf(status_, sizeof(status_), "error: no valid app image");
        sendResponse(sock, ok ? "200 OK" : "500 Internal Server Error", "text/plain", status_);
        installed = ok;   // reuse the reply-then-restart tail below
    } else if (std::strncmp(head, "GET /moonbase", 13) == 0) {
        // Identity probe: the app UI polls this across the update cycle to tell which image is
        // answering at the shared address (the app 404s it). Body = the live install status, so
        // the poll doubles as a progress read during an unattended install.
        sendResponse(sock, "200 OK", "text/plain", status_);
    } else if (std::strncmp(head, "GET / ", 6) == 0 || std::strncmp(head, "GET /index", 10) == 0) {
        sendResponse(sock, "200 OK", "text/html", kPage);
    } else {
        sendResponse(sock, "404 Not Found", "text/plain", "not found");
    }

    ::shutdown(sock, SHUT_RDWR);
    ::close(sock);
    if (installed) {
        // Let the reply reach the browser before the device goes away.
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

void serveForever() {
    const int listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) return;
    int yes = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(kHttpPort);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { ::close(listener); return; }
    if (::listen(listener, 1) != 0) { ::close(listener); return; }

    while (true) {
        const int sock = ::accept(listener, nullptr, nullptr);
        if (sock < 0) continue;
        // A stalled peer must not hold MoonBase forever: the whole point is that the device stays
        // reachable for the next attempt.
        timeval tv = {};
        tv.tv_sec = 30;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        serveOne(sock);
    }
}

}  // namespace

extern "C" void app_main() {
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    netEvents_ = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &onGotIp, nullptr, nullptr);

    loadCredentials();

    // WiFi STA with the app's stored credentials, else the open access point: the guarantee
    // that a board is never unreachable because its credentials went stale. Ethernet is a
    // follow-up (backlog-core, MoonBase follow-ups): it needs per-board PHY/pin configuration,
    // so today the eth-only esp32-eth variant lands on the access point here.
    // The unattended handoff: the app may have staged an install URL in NVS before rebooting
    // into MoonBase (platform::moonbaseStageInstallUrl). Read AND erase it unconditionally,
    // before anything can fail: a URL that crashes or fails can then never boot-loop the
    // device, and a stale URL can never survive a failed network join to hijack a later,
    // unrelated visit to MoonBase (one try per staging, ever).
    char stagedUrl[256] = {};
    {
        nvs_handle_t h;
        if (nvs_open("moonbase", NVS_READWRITE, &h) == ESP_OK) {
            size_t len = sizeof(stagedUrl);
            if (nvs_get_str(h, "url", stagedUrl, &len) != ESP_OK) stagedUrl[0] = '\0';
            nvs_erase_key(h, "url");
            nvs_commit(h);
            nvs_close(h);
        }
    }

    bool online = wifiStation(20000);

    // STA only: on the fallback AP the URL's network is not reachable, and a user is present.
    if (online && stagedUrl[0]) {
        // A connect attempted straight after GOT_IP can fail (0x7002, ESP_ERR_HTTP_CONNECT)
        // where the same connect succeeds seconds later: the LAN is still warming up around a
        // freshly associated station. A short retry absorbs that; a genuinely unreachable URL
        // still fails through to the page after the last attempt.
        for (int attempt = 0; attempt < 3; attempt++) {
            if (attempt) vTaskDelay(pdMS_TO_TICKS(3000));
            if (installFromUrl(stagedUrl)) esp_restart();   // straight back into the new app
        }
    }

    if (!online) online = wifiAccessPoint();

    // With no network there is nothing MoonBase can do but wait: a user who cannot reach it will
    // reflash over USB, and restarting into an application slot that may be empty helps nobody.
    if (online) serveForever();
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
