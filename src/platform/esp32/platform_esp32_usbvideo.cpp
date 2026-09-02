// USB video capture: the peripheral half of VideoService (src/core/VideoService.h). An HDMI
// grabber presents itself as a UVC webcam; this file owns the UVC stream and the JPEG decode.
//
// MJPEG, because uncompressed does not fit: 640x480 YUY2 at 60 fps is 37 MB/s against a USB 2.0
// host's ~24.6 MB/s.
//
// The USB host library is a per-application singleton: installed on first use and left running,
// with its event loop on a task of its own. uvc_host_stream_open() blocks waiting for an
// enumeration that loop drives, so it cannot share a thread with init.
//
// Decoding runs on a task of its own too. jpeg_decoder_process() blocks, and the render tick is
// MM_NONBLOCKING, so videoCaptureFrame only reads an index, and the frame it names was decoded
// earlier by decoderTask. A frame arriving while one is still pending is dropped: the newest is
// the only one worth having.

#include "platform/platform.h"

#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)

#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>

namespace mm::platform {
namespace {

constexpr char kTag[] = "usbvideo";

constexpr int kSlots = 3;
static_assert(kSlots >= 3, "freeSlot() needs a spare beyond the published and the in-use slot");

struct Capture {
    uvc_host_stream_hdl_t stream = nullptr;
    jpeg_decoder_handle_t jpeg = nullptr;
    bool uvcInstalled = false;

    // What to ask for again after a disconnect, and the flag that asks.
    uint16_t reqWidth = 0;
    uint16_t reqHeight = 0;
    uint8_t reqFps = 0;
    std::atomic<bool> lost{false};

    // The frame the UVC callback handed over, or null. Exchanged rather than assigned so the
    // callback never blocks and never overwrites one the decoder is already reading.
    std::atomic<uvc_host_frame_t*> pending{nullptr};

    TaskHandle_t decoder = nullptr;
    SemaphoreHandle_t wake = nullptr;    // callback -> decoder
    SemaphoreHandle_t stopped = nullptr; // decoder -> deinit
    std::atomic<bool> running{false};

    // Decoded RGB888, from jpeg_alloc_decoder_mem for its cache-line and 2D-DMA alignment: a
    // plain malloc shows up as intermittent corruption, not an error.
    uint8_t* rgb[kSlots] = {};
    size_t rgbCap = 0;
    uint16_t width[kSlots] = {};
    uint16_t height[kSlots] = {};
    std::atomic<int> published{-1}; // decoder -> renderer: newest complete slot
    std::atomic<int> inUse{-1};     // renderer -> decoder: slot handed out last call
};

// What the attached device advertises. File scope rather than inside Capture because it is learned
// from the driver event, which fires before a stream exists and outlives a failed open.
constexpr size_t kMaxFormats = 24;
VideoCaptureFormat advertised[kMaxFormats];
uvc_host_frame_info_t frameList[kMaxFormats]; // file scope: too big for the driver task's stack
// Written by the UVC driver task, read by prepare(). Zeroed before the rewrite and released after,
// so a reader sees either an empty list or a complete one, never a half-written one.
std::atomic<size_t> advertisedCount{0};

bool hostReady = false;

// usb_host_lib_handle_events() is where enumeration and the port state machine actually run, and
// it blocks. Nothing else may drive it, so this task owns it for the life of the application.
void pumpTask(void*) {
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// Installed once and never uninstalled: the library is a singleton the whole application shares,
// and tearing it down on a source switch only risks leaving it un-reinstallable.
bool ensureUsbHost() {
    if (hostReady) return true;
    usb_host_config_t hostCfg = {};
    hostCfg.intr_flags = ESP_INTR_FLAG_LOWMED;
    if (usb_host_install(&hostCfg) != ESP_OK) {
        ESP_LOGE(kTag, "usb_host_install failed");
        return false;
    }
    // Priority 4 is below the UVC driver task, which consumes what this one produces. Unpinned
    // because the render loop is fixed to core 0 by CONFIG_ESP_MAIN_TASK_AFFINITY, so leaving
    // placement to the scheduler keeps USB off it whenever core 1 is free.
    if (xTaskCreatePinnedToCore(pumpTask, "usbpump", 4 * 1024, nullptr, 4, nullptr, tskNO_AFFINITY) !=
        pdPASS) {
        ESP_LOGE(kTag, "no USB event task");
        usb_host_uninstall();
        return false;
    }
    hostReady = true;
    return true;
}

// Runs on the UVC driver task (uvc_client_task -> usb_host_client_handle_events -> here), so the
// ordinary FreeRTOS API is safe. It still only hands the frame over: decoding here would stall the
// task that collects isochronous packets, and a missed packet is gone for good.
bool onFrame(const uvc_host_frame_t* frame, void* ctx) {
    auto* cap = static_cast<Capture*>(ctx);
    uvc_host_frame_t* expected = nullptr;
    // Take the slot only if it is free. Returning false keeps the frame, so the loser of this
    // race must return true to hand it straight back or the driver runs out of buffers.
    if (!cap->pending.compare_exchange_strong(expected, const_cast<uvc_host_frame_t*>(frame))) return true;
    xSemaphoreGive(cap->wake);
    return false;
}

// UVC states a rate as dwFrameInterval, a period in 100 ns ticks, so one second is 10 million of
// them. Rounded rather than truncated: 59.94 fps is a real rate and reads better as 60 than 59.
uint8_t fpsFrom(uint32_t interval) {
    constexpr uint32_t kTicksPerSecond = 10000000;
    return static_cast<uint8_t>((kTicksPerSecond + interval / 2) / interval);
}

// One dropdown row per (resolution, rate) pair. A device that does 320x240 at both 30 and 60 lists
// the resolution ONCE with several intervals, so without this expansion only its default is
// reachable from the UI.
void addAdvertised(const uvc_host_frame_info_t& info, uint32_t interval, size_t& n) {
    if (n >= kMaxFormats || interval == 0) return;
    VideoCaptureFormat& f = advertised[n++];
    f.width = static_cast<uint16_t>(info.h_res);
    f.height = static_cast<uint16_t>(info.v_res);
    f.fps = fpsFrom(interval);
    ESP_LOGI(kTag, "offers MJPEG %ux%u @ %u fps", f.width, f.height, f.fps);
}

// Runs on the UVC driver task when a device enumerates: before any stream is opened, which is what
// makes the list available even when the open then fails on an unsupported resolution.
void onDriverEvent(const uvc_host_driver_event_data_t* event, void*) {
    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) return;

    size_t count = kMaxFormats;
    if (uvc_host_get_frame_list(event->device_connected.dev_addr,
                                event->device_connected.uvc_stream_index,
                                reinterpret_cast<uvc_host_frame_info_t(*)[]>(frameList),
                                &count) != ESP_OK) {
        ESP_LOGW(kTag, "device connected but its frame list could not be read");
        return;
    }
    if (count > kMaxFormats) count = kMaxFormats; // it reports what it NEEDS, not what it wrote

    advertisedCount.store(0, std::memory_order_release); // hide the list while it is rewritten
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        const uvc_host_frame_info_t& info = frameList[i];
        if (info.format != UVC_VS_FORMAT_MJPEG) continue; // nothing else is decodable here
        if (info.interval_type == 0) { // a continuous range: offer both ends, fastest first
            addAdvertised(info, info.interval_min, n);
            if (info.interval_max != info.interval_min) addAdvertised(info, info.interval_max, n);
            continue;
        }
        const uint8_t rates = info.interval_type < CONFIG_UVC_INTERVAL_ARRAY_SIZE
                                  ? info.interval_type
                                  : CONFIG_UVC_INTERVAL_ARRAY_SIZE;
        for (uint8_t j = 0; j < rates; j++) addAdvertised(info, info.interval[j], n);
    }
    advertisedCount.store(n, std::memory_order_release);
}

void onEvent(const uvc_host_stream_event_data_t* event, void* ctx) {
    auto* cap = static_cast<Capture*>(ctx);
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
        cap->published.store(-1);
        cap->lost.store(true); // decoderTask reopens; stream_open blocks, so not from here
        ESP_LOGW(kTag, "capture device disconnected");
    }
}

// Allocated once from the negotiated format, so no reallocation ever races the render thread.
bool allocSlots(Capture& cap, uint16_t w, uint16_t h) {
    const size_t need = static_cast<size_t>(w) * h * 3;
    jpeg_decode_memory_alloc_cfg_t memCfg = {};
    memCfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    for (int i = 0; i < kSlots; i++) {
        size_t got = 0;
        cap.rgb[i] = static_cast<uint8_t*>(jpeg_alloc_decoder_mem(need, &memCfg, &got));
        if (!cap.rgb[i]) return false;
        cap.rgbCap = got; // every slot gets the same request, so the same rounded-up size
    }
    return true;
}

// -1 when every slot is spoken for: unreachable while kSlots is 3, but returning a real index
// anyway would hand the decoder a buffer the render thread is reading. Corruption with no error is
// worse than a dropped frame.
int freeSlot(const Capture& cap) {
    const int pub = cap.published.load(std::memory_order_relaxed);
    const int use = cap.inUse.load(std::memory_order_relaxed);
    for (int i = 0; i < kSlots; i++)
        if (i != pub && i != use) return i;
    return -1;
}

void decode(Capture& cap, uvc_host_frame_t* frame) {
    // Dimensions from the bitstream, not from the request: a device may negotiate something else.
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(frame->data, frame->data_len, &info) != ESP_OK) return;
    if (static_cast<size_t>(info.width) * info.height * 3 > cap.rgbCap) {
        ESP_LOGW(kTag, "frame %ux%u exceeds the buffers sized at open", info.width, info.height);
        return;
    }

    const int slot = freeSlot(cap);
    if (slot < 0) return; // drop the frame rather than write over one being read

    jpeg_decode_cfg_t decodeCfg = {};
    decodeCfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
    decodeCfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
    decodeCfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t outSize = 0;
    if (jpeg_decoder_process(cap.jpeg, &decodeCfg, frame->data, frame->data_len, cap.rgb[slot], cap.rgbCap,
                             &outSize) != ESP_OK)
        return;

    cap.width[slot] = static_cast<uint16_t>(info.width);
    cap.height[slot] = static_cast<uint16_t>(info.height);
    cap.published.store(slot, std::memory_order_release); // dimensions first, then the slot
}

bool openStream(Capture& cap, uint16_t width, uint16_t height, uint8_t fps); // defined below

// Replug recovery. uvc_host_stream_open blocks for up to its timeout, so this runs on the decode
// task rather than in the event callback or the render tick. A failed attempt costs that timeout,
// which is its own retry pacing.
void reopen(Capture& cap) {
    if (cap.stream) {
        uvc_host_stream_close(cap.stream);
        cap.stream = nullptr;
    }
    if (!openStream(cap, cap.reqWidth, cap.reqHeight, cap.reqFps)) return;
    uvc_host_stream_start(cap.stream);
    cap.lost.store(false);
    ESP_LOGI(kTag, "capture device back");
}

// The blocking half, kept off the render tick. Waits on a finite timeout rather than forever so
// `running` and `lost` are seen without the callback having to signal.
void decoderTask(void* arg) {
    auto* cap = static_cast<Capture*>(arg);
    while (cap->running.load()) {
        if (xSemaphoreTake(cap->wake, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (cap->lost.load()) reopen(*cap);
            continue;
        }
        if (uvc_host_frame_t* frame = cap->pending.exchange(nullptr)) {
            decode(*cap, frame);
            uvc_host_frame_return(cap->stream, frame);
        }
    }
    xSemaphoreGive(cap->stopped);
    vTaskDelete(nullptr);
}

// Priority 6 puts it above the UVC driver task: a decode that runs late holds the only free frame
// buffer, which is what starves the driver.
bool startDecoder(Capture& cap) {
    cap.running = true;
    if (xTaskCreatePinnedToCore(decoderTask, "usbjpeg", 4 * 1024, &cap, 6, &cap.decoder, tskNO_AFFINITY) ==
        pdPASS)
        return true;
    cap.running = false;
    ESP_LOGE(kTag, "no decode task");
    return false;
}

bool installUvc(Capture& cap) {
    uvc_host_driver_config_t driverCfg = {};
    driverCfg.driver_task_stack_size = 4 * 1024;
    driverCfg.driver_task_priority = 5;
    driverCfg.xCoreID = tskNO_AFFINITY;
    driverCfg.create_background_task = true;
    driverCfg.event_cb = onDriverEvent; // fills `advertised` as soon as a device enumerates
    if (uvc_host_install(&driverCfg) != ESP_OK) {
        ESP_LOGE(kTag, "uvc_host_install failed");
        return false;
    }
    cap.uvcInstalled = true;
    return true;
}

bool createJpeg(Capture& cap) {
    jpeg_decode_engine_cfg_t jpegCfg = {};
    jpegCfg.timeout_ms = 40;
    if (jpeg_new_decoder_engine(&jpegCfg, &cap.jpeg) == ESP_OK) return true;
    ESP_LOGE(kTag, "no JPEG decoder engine");
    return false;
}

bool createSignals(Capture& cap) {
    cap.wake = xSemaphoreCreateBinary();
    cap.stopped = xSemaphoreCreateBinary();
    if (cap.wake && cap.stopped) return true;
    ESP_LOGE(kTag, "no semaphores");
    return false;
}

bool openStream(Capture& cap, uint16_t width, uint16_t height, uint8_t fps) {
    cap.reqWidth = width;
    cap.reqHeight = height;
    cap.reqFps = fps;
    uvc_host_stream_config_t streamCfg = {};
    streamCfg.event_cb = onEvent;
    streamCfg.frame_cb = onFrame;
    streamCfg.user_ctx = &cap;
    streamCfg.usb.dev_addr = UVC_HOST_ANY_DEV_ADDR;
    streamCfg.usb.vid = UVC_HOST_ANY_VID;
    streamCfg.usb.pid = UVC_HOST_ANY_PID;
    streamCfg.usb.uvc_stream_index = 0;
    streamCfg.vs_format.h_res = width;
    streamCfg.vs_format.v_res = height;
    streamCfg.vs_format.fps = fps; // negotiated down to what the device offers
    streamCfg.vs_format.format = UVC_VS_FORMAT_MJPEG;
    // urb_size and frame_size left at 0: the driver then derives them from what this device
    // actually negotiated, which beats any constant here.
    streamCfg.advanced.number_of_frame_buffers = 3;

    // Wait rather than fail: the host enumerates asynchronously, so a device plugged in at boot
    // is usually not ready when this runs.
    if (uvc_host_stream_open(&streamCfg, 3000, &cap.stream) != ESP_OK) {
        ESP_LOGW(kTag, "no UVC device offering MJPEG %ux%u", width, height);
        return false;
    }
    uvc_host_desc_print(cap.stream); // the format list, for the bench
    return true;
}

// Sized from what the device agreed to, not from what we asked for, so no frame can arrive
// needing more room than the slots have.
bool sizeBuffers(Capture& cap) {
    uvc_host_stream_format_t got = {};
    if (uvc_host_stream_format_get(cap.stream, &got) != ESP_OK) {
        ESP_LOGE(kTag, "cannot read the negotiated format");
        return false;
    }
    ESP_LOGI(kTag, "streaming MJPEG %ux%u @ %.1f fps", got.h_res, got.v_res, got.fps);
    if (allocSlots(cap, static_cast<uint16_t>(got.h_res), static_cast<uint16_t>(got.v_res))) return true;
    ESP_LOGE(kTag, "no memory for %d decode buffers", kSlots);
    return false;
}

} // namespace

bool videoCaptureInit(VideoCaptureHandle& handle, uint16_t width, uint16_t height, uint8_t fps) {
    if (handle.impl) return true;
    if (!ensureUsbHost()) return false;
    auto* cap = new Capture();
    handle.impl = cap; // every failure below unwinds through videoCaptureDeinit

    if (!installUvc(*cap) || !createJpeg(*cap) || !createSignals(*cap) ||
        !openStream(*cap, width, height, fps) || !sizeBuffers(*cap) || !startDecoder(*cap)) {
        videoCaptureDeinit(handle);
        return false;
    }
    uvc_host_stream_start(cap->stream);
    return true;
}

// Hot path: an index load and two field reads. Everything expensive already happened on
// decoderTask, which is what lets the render tick stay MM_NONBLOCKING.
const uint8_t* videoCaptureFrame(VideoCaptureHandle& handle, uint16_t& width,
                                 uint16_t& height) MM_NONBLOCKING {
    auto* cap = static_cast<Capture*>(handle.impl);
    if (!cap) return nullptr;
    const int slot = cap->published.load(std::memory_order_acquire);
    // Consecutive publishes always land in different slots, so an unchanged one means no new frame.
    if (slot < 0 || slot == cap->inUse.load(std::memory_order_relaxed)) return nullptr;
    cap->inUse.store(slot, std::memory_order_relaxed); // claim it before the caller reads it
    width = cap->width[slot];
    height = cap->height[slot];
    return cap->rgb[slot];
}

size_t videoCaptureFormats(VideoCaptureFormat* out, size_t max) {
    const size_t have = advertisedCount.load(std::memory_order_acquire);
    const size_t n = have < max ? have : max;
    for (size_t i = 0; i < n; i++) out[i] = advertised[i];
    return n;
}

void videoCaptureDeinit(VideoCaptureHandle& handle) {
    auto* cap = static_cast<Capture*>(handle.impl);
    if (!cap) return;
    if (cap->stream) uvc_host_stream_stop(cap->stream); // no new frames while we tear down
    if (cap->decoder) {                                 // stop the decoder before what it uses
        cap->running = false;
        xSemaphoreTake(cap->stopped, pdMS_TO_TICKS(500));
    }
    if (uvc_host_frame_t* frame = cap->pending.exchange(nullptr)) uvc_host_frame_return(cap->stream, frame);
    if (cap->stream) uvc_host_stream_close(cap->stream);
    if (cap->jpeg) jpeg_del_decoder_engine(cap->jpeg);
    if (cap->uvcInstalled) uvc_host_uninstall();
    if (cap->wake) vSemaphoreDelete(cap->wake);
    if (cap->stopped) vSemaphoreDelete(cap->stopped);
    for (uint8_t* buf : cap->rgb) free(buf);
    delete cap;
    handle.impl = nullptr;
}

} // namespace mm::platform

#else // every other target: no High-Speed USB host, no JPEG decoder

namespace mm::platform {

bool videoCaptureInit(VideoCaptureHandle&, uint16_t, uint16_t, uint8_t) { return false; }
size_t videoCaptureFormats(VideoCaptureFormat*, size_t) { return 0; }
const uint8_t* videoCaptureFrame(VideoCaptureHandle&, uint16_t&, uint16_t&) MM_NONBLOCKING { return nullptr; }
void videoCaptureDeinit(VideoCaptureHandle&) {}

} // namespace mm::platform

#endif
