// Parlio (Parallel IO) WS2812 output — the peripheral half of the Parlio LED
// driver (ESP32-P4). The driver (src/light/drivers/ParlioLedDriver.h) does all
// the domain work: applies Correction and 3-slot-encodes every light into the
// DMA frame buffer (LcdSlots.h, the SAME encoder the LCD_CAM driver uses — one
// bus byte per slot, bit L = data line L). This file owns only the peripheral:
// the Parlio TX unit, the DMA frame buffer, transmit + wait. No domain logic.
//
// Design mirrors the LCD_CAM driver: the whole frame is pre-encoded into ONE
// buffer and sent as ONE autonomous DMA transfer (single-shot, NOT Parlio's
// loop-transmission mode) — once started no CPU work remains until the done
// callback, so there is no refill deadline for WiFi to miss.
//
// Simpler than i80: Parlio takes the data GPIOs directly (no sacrificial WR/DC
// lines — it generates the pixel clock internally) and the bus is always 8
// lanes wide to match the encoder's 8-bit bus byte; lanes the driver doesn't
// use get GPIO -1 so they're simply not driven (no all-pins-required rule).
//
// Compiles on every ESP32 chip: everything is under SOC_PARLIO_SUPPORTED with
// inert stubs otherwise; the driver never calls in (platform::parlioLanes == 0).

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_PARLIO_SUPPORTED

#include "driver/parlio_tx.h"
#include "driver/gpio.h"        // gpio_num_t / GPIO_NUM_NC for the unit's pin map
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstring>
#include <functional>  // the transmit callback passed to the shared frame loopback
#include <new>      // std::nothrow

namespace mm::platform {

namespace {

static const char* PAR_TAG = "mm_parlio";

// Parlio data_width is power-of-two only (≤ SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH),
// derived from the lane count: ≤8 lanes → an 8-bit bus (uint8 encoder slots, bit
// L = lane L), 9..16 → a 16-bit bus (uint16 slots). Unlike i80, Parlio accepts an
// NC unused lane, so a sub-width lane count just leaves the extra data lines NC.
constexpr size_t kMaxBusWidth = 16;   // both peripherals' physical ceiling

// WS2812 slot rate (375 ns @ 2.67 MHz), same value the driver passes at init —
// the loopback creates its own private unit and needs the constant directly.
constexpr uint32_t kPclkHz = 2'666'666;

struct ParlioState {
    parlio_tx_unit_handle_t unit = nullptr;
    SemaphoreHandle_t done = nullptr;
    uint8_t* buf = nullptr;
    size_t cap = 0;
};

// Done-callback: the DMA transfer finished — release the waiter.
bool IRAM_ATTR parlioDoneCb(parlio_tx_unit_handle_t, const parlio_tx_done_event_data_t*,
                            void* user) {
    auto* st = static_cast<ParlioState*>(user);
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done, &high);
    return high == pdTRUE;
}

void destroyState(ParlioState* st) {
    if (!st) return;
    if (st->unit) {
        parlio_tx_unit_disable(st->unit);
        parlio_del_tx_unit(st->unit);
    }
    if (st->buf) heap_caps_free(st->buf);
    if (st->done) vSemaphoreDelete(st->done);
    delete st;
}

// One TX unit + zeroed DMA buffer. pclkHz is the WS2812 slot rate (2.67 MHz).
ParlioState* createState(const uint16_t* dataPins, uint8_t laneCount,
                         uint32_t pclkHz, size_t bufferBytes) {
    auto* st = new (std::nothrow) ParlioState();
    if (!st) return nullptr;

    parlio_tx_unit_config_t cfg = {};
    cfg.clk_src = PARLIO_CLK_SRC_DEFAULT;     // PLL_F160M → /60 = 2.67 MHz
    cfg.clk_in_gpio_num = GPIO_NUM_NC;        // internal clock, not external
    cfg.output_clk_freq_hz = pclkHz;
    const size_t busWidth = laneCount <= 8 ? 8 : 16;   // power-of-two, derived
    cfg.data_width = busWidth;
    for (size_t i = 0; i < kMaxBusWidth; i++) cfg.data_gpio_nums[i] = GPIO_NUM_NC;
    for (uint8_t i = 0; i < laneCount && i < busWidth; i++)
        cfg.data_gpio_nums[i] = static_cast<gpio_num_t>(dataPins[i]);
    cfg.clk_out_gpio_num = GPIO_NUM_NC;       // WS2812 ignores the clock line
    cfg.valid_gpio_num = GPIO_NUM_NC;
    cfg.trans_queue_depth = 1;                // single full-frame transfer
    cfg.max_transfer_size = bufferBytes;
    cfg.dma_burst_size = 64;
    cfg.shift_edge = PARLIO_SHIFT_EDGE_POS;   // shift data on the clock's rising edge
    cfg.bit_pack_order = PARLIO_BIT_PACK_ORDER_MSB;
    if (parlio_new_tx_unit(&cfg, &st->unit) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    st->done = xSemaphoreCreateBinary();
    if (!st->done) { destroyState(st); return nullptr; }
    parlio_tx_event_callbacks_t cbs = {};
    cbs.on_trans_done = parlioDoneCb;
    if (parlio_tx_unit_register_event_callbacks(st->unit, &cbs, st) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }
    if (parlio_tx_unit_enable(st->unit) != ESP_OK) { destroyState(st); return nullptr; }

    // DMA-capable draw buffer, PSRAM-first (with an internal fallback). The intent is to keep the
    // large 16-bit-wide 16-lane frame off scarce internal DRAM. MALLOC_CAP_CACHE_ALIGNED lets the
    // allocator apply the PSRAM cache-line alignment the ext-mem GDMA needs (wider than the fixed 64
    // the internal path uses). **Measured reality (performance.md § Multi-pin):** on the P4 this
    // SPIRAM request evidently doesn't satisfy — the fallback governs, so the frame lands in internal
    // SRAM and the ~368 KB largest-contiguous-internal-block caps 16-lane at ~4096 lights (unlike the
    // S3 LCD path, whose esp_lcd_i80 PSRAM buffer is proven to 16384). So this is allocate-and-degrade
    // that currently degrades on P4; lifting it to a real PSRAM frame is the chunked-transfer backlog
    // item. Zeroed so the trailing latch pad holds lines LOW.
    st->buf = static_cast<uint8_t*>(heap_caps_malloc(
        bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
    if (!st->buf)
        st->buf = static_cast<uint8_t*>(heap_caps_aligned_alloc(
            64, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!st->buf) { destroyState(st); return nullptr; }
    std::memset(st->buf, 0, bufferBytes);
    st->cap = bufferBytes;
    return st;
}

}  // namespace

// Parlio clocks the WHOLE buffer out in ONE transaction (unlike RMT's streaming ping-pong or i80's
// chained DMA), so a frame must fit the peripheral's single-transfer register: PARLIO_LL_TX_MAX_BITS_
// PER_FRAME = 0x7FFFF (524287) bits = 65535 bytes on every current Parlio-capable target (P4/C6/H2/…).
// The IDF rejects an over-limit unit with ESP_ERR_INVALID_ARG, and — the trap this guards — a unit
// created oversized then fails EVERY transmit (the check is on the unit's configured max, not the
// payload), so output goes silently dark. We reject up front with a clear status instead.
//
// The limit is a total-BUFFER byte limit (0x7FFFF bits / 8 = 65535 bytes — buffer bits = bytes × 8
// regardless of data_width, so this ceiling is the same at 8 or 16 lanes). A light costs FAR more
// than its channel count in the DMA buffer: the buffer holds the WS2812 *waveform*, not the color
// bytes. One light = channels × 8 bits × 3 slots (the encoder shapes each bit into 3 bus-word slots
// for the 800 kHz NRZ pulse). On the 8-bit bus a slot is 1 byte → 24 bytes/channel: RGB = 72 B/light,
// so max ≈ (65535 − 864 pad)/(3×24) = 897 RGB lights/lane. On the 16-BIT bus (>8 lanes) a slot is 2
// bytes → 48 bytes/channel, so the byte cost per light DOUBLES and the lights/lane HALVES: ~448 RGB,
// ~336 RGBW. Reaching the higher per-lane totals needs the chunked-transfer enhancement (backlog).
// Not a UI input guard — the driver surfaces it as a status.
inline constexpr size_t kParlioMaxTransferBytes = 0x7FFFF / 8;   // 65535 (buffer bytes, width-invariant)

bool parlioWs2812Init(ParlioWs2812Handle& h, const uint16_t* dataPins,
                      uint8_t laneCount, uint32_t pclkHz, size_t bufferBytes) {
    if (!dataPins || laneCount == 0 || bufferBytes == 0) return false;
    // Reject a frame larger than the peripheral can clock out in one transaction — else the created
    // unit would fail every transmit silently (see kParlioMaxTransferBytes). The driver reports the
    // init failure as a status; the fix for the user is fewer lights/lane or the start/count window.
    if (bufferBytes > kParlioMaxTransferBytes) return false;
    // Keep the platform memory reserve intact — degrade (init failure → driver
    // idles with a status error) rather than starve the system of internal RAM.
    if (heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
        < bufferBytes + HEAP_RESERVE) {
        return false;
    }
    ParlioState* st = createState(dataPins, laneCount, pclkHz, bufferBytes);
    if (!st) return false;
    h.impl = st;
    return true;
}

uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& h) {
    auto* st = static_cast<ParlioState*>(h.impl);
    return st ? st->buf : nullptr;
}

size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& h) {
    auto* st = static_cast<ParlioState*>(h.impl);
    return st ? st->cap : 0;
}

bool parlioWs2812Transmit(ParlioWs2812Handle& h, size_t bytes) {
    auto* st = static_cast<ParlioState*>(h.impl);
    if (!st || bytes == 0 || bytes > st->cap) return false;
    parlio_transmit_config_t xcfg = {};
    xcfg.idle_value = 0;   // lines rest LOW between/after the frame (the latch)
    // payload length is in BITS; the buffer is bytes × 8 lanes-worth of slots.
    return parlio_tx_unit_transmit(st->unit, st->buf, bytes * 8, &xcfg) == ESP_OK;
}

void parlioWs2812Wait(ParlioWs2812Handle& h, uint32_t timeoutMs) {
    auto* st = static_cast<ParlioState*>(h.impl);
    if (!st) return;
    // Finite timeout, self-healing: a timed-out frame is dropped and the whole
    // frame re-encoded next tick (same stance as rmt/lcd Wait).
    parlio_tx_unit_wait_all_done(st->unit, static_cast<int>(timeoutMs));
}

void parlioWs2812Deinit(ParlioWs2812Handle& h) {
    auto* st = static_cast<ParlioState*>(h.impl);
    if (!st) return;
    destroyState(st);
    h.impl = nullptr;
}

// ---------------------------------------------------------------------------
// Loopback self-test: a private Parlio TX unit on the driver's data pins
// transmits the CALLER'S real frame — full size, real DMA transfer, real latch
// pad — back to back like the render loop, while an RMT RX channel
// (rmtWs2812RxCapture with the DMA backend — transmitter-agnostic, reused from
// the RMT/LCD rigs) captures the WHOLE frame off the jumpered rxGpio and
// verifies every bit. This is the LCD loopback (platform_esp32_lcd.cpp) with
// the i80 transmit swapped for Parlio's: no WR/DC pins, and the payload goes
// out via parlio_tx_unit_transmit (length in BITS) instead of
// esp_lcd_panel_io_tx_color. The RX capture half is byte-for-byte identical —
// the wire signal is the same WS2812 the encoder produced for either bus.
// ---------------------------------------------------------------------------

// loopbackJumperOk + captureAndVerifyFrame live in platform_esp32_rmt.cpp (the
// shared continuity check and the shared capture+bit-verify all three loopback
// rigs reuse); declared here so this TU can call them.
namespace detail {
bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio);
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r);
}

RmtLoopbackResult parlioWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                       uint16_t rxGpio, const uint8_t* frame,
                                       size_t frameBytes, size_t dataBytes,
                                       uint8_t rowBits) {
    RmtLoopbackResult r;
    r.sent[0] = 0xA5; r.sent[1] = 0x00; r.sent[2] = 0xFF;  // pattern in every row
    if (!dataPins || laneCount == 0 || !frame || frameBytes == 0
        || dataBytes < 3 || dataBytes > frameBytes || rowBits < 8) return r;
    const uint16_t txGpio = dataPins[0];   // lane 0 carries the pattern

    r.jumperDetected = detail::loopbackJumperOk(static_cast<uint8_t>(txGpio),
                                                static_cast<uint8_t>(rxGpio));
    if (!r.jumperDetected) return r;

    // The continuity check above reset txGpio's GPIO matrix route; the TX unit
    // creation below re-claims it.
    ParlioState* st = createState(dataPins, laneCount, kPclkHz, frameBytes);
    if (!st) {
        ESP_LOGE(PAR_TAG, "loopback: private TX unit creation failed");
        return r;
    }
    std::memcpy(st->buf, frame, frameBytes);

    // The Parlio-specific transmit: ship one frame (length in BITS, not bytes)
    // and wait for its done-callback. Everything else (capture, cadence, bit-
    // verify) is the shared helper.
    parlio_transmit_config_t xcfg = {};
    xcfg.idle_value = 0;   // lines rest LOW between frames (the latch)
    auto transmitOnce = [st, frameBytes, &xcfg]() {
        // Loopback self-test path (not the render hot path): a failed enqueue or a
        // done-callback timeout would otherwise be silent and just surface later as
        // a capture mismatch — log it so the real cause is visible in the verdict.
        const esp_err_t err = parlio_tx_unit_transmit(st->unit, st->buf,
                                                      frameBytes * 8, &xcfg);
        if (err != ESP_OK) {
            ESP_LOGE(PAR_TAG, "loopback: tx enqueue failed (%s)", esp_err_to_name(err));
            return;   // nothing to wait for
        }
        if (xSemaphoreTake(st->done, pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(PAR_TAG, "loopback: tx done-callback timed out");
    };
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, kPclkHz,
                                  PAR_TAG, transmitOnce, r);
    destroyState(st);
    return r;
}

}  // namespace mm::platform

#else  // !SOC_PARLIO_SUPPORTED — inert stubs so classic ESP32 / S3 link

namespace mm::platform {

bool parlioWs2812Init(ParlioWs2812Handle&, const uint16_t*, uint8_t, uint32_t, size_t) {
    return false;
}
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle&) { return nullptr; }
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle&) { return 0; }
bool parlioWs2812Transmit(ParlioWs2812Handle&, size_t) { return false; }
void parlioWs2812Wait(ParlioWs2812Handle&, uint32_t) {}
void parlioWs2812Deinit(ParlioWs2812Handle&) {}
RmtLoopbackResult parlioWs2812Loopback(const uint16_t*, uint8_t, uint16_t,
                                       const uint8_t*, size_t, size_t, uint8_t) {
    return {};
}

}  // namespace mm::platform

#endif  // SOC_PARLIO_SUPPORTED
