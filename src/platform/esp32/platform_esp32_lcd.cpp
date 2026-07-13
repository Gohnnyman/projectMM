// LCD_CAM parallel WS2812 output — the peripheral half of the LCD LED driver
// (ESP32-S3). The driver (src/light/drivers/LcdLedDriver.h) does all the
// domain work: applies Correction and 3-slot-encodes every light into the DMA
// frame buffer (LcdSlots.h). This file owns only the peripheral — the esp_lcd
// i80 bus, the IO device, the DMA-capable frame buffer, transmit + wait, and
// the loopback test's TX side. No domain logic here.
//
// Design: the whole frame is pre-encoded into ONE buffer and sent as ONE
// gapless GDMA stream (tx_color with lcd_cmd = -1 → pure data phase). Once
// started, no CPU work remains until the done callback — there is no refill
// deadline for WiFi to miss, which is the deliberate difference from the
// ISR-refilled rings in the hpwit/FastLED LCD drivers this design studied.
//
// The file compiles on every ESP32 chip: everything is under
// SOC_LCDCAM_I80_LCD_SUPPORTED with inert stubs otherwise (classic ESP32 builds
// it too; the driver never calls in thanks to platform::lcdLanes == 0). Gate on
// SOC_LCDCAM_I80_LCD_SUPPORTED, NOT SOC_LCD_I80_SUPPORTED: the classic ESP32 sets
// the latter for its unrelated I2S-LCD peripheral, so keying on it wires this driver
// onto a chip with no LCD_CAM and hangs its boot (see platform_config.h + lessons.md).

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

// SOC_LCDCAM_I80_LCD_SUPPORTED, not SOC_LCD_I80_SUPPORTED: the classic ESP32
// sets the latter for its I2S-LCD peripheral, which is NOT the LCD_CAM i80 bus
// esp_lcd drives here — compiling this body for the classic chip wired the
// driver onto it and hung its boot. Mirror the lcdLanes gate in
// platform_config.h. (esp_lcd headers below only exist where LCD_CAM does.)
#if SOC_LCDCAM_I80_LCD_SUPPORTED

#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_log.h"
#include "esp_timer.h"   // esp_timer_get_time — ISR-safe wire-time stamp
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstring>
#include <functional>  // the transmit callback passed to the shared frame loopback
#include <new>      // std::nothrow

namespace mm::platform {

// Defined in platform_esp32_rmt.cpp — the plain-GPIO continuity pre-check the
// RMT loopback uses; the wire question is identical here.
namespace detail { bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio); }

namespace {

static const char* LCD_TAG = "mm_lcd";

// 3 slots per WS2812 bit (the LcdSlots.h contract): 2.67 MHz pclk = 375 ns
// slots, "0" = 1 slot HIGH, "1" = 2 slots HIGH. 375 ns and not the lineage's
// usual 416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and on a
// direct 3.3 V data line (no level shifter) a longer "0" pulse gets misread
// as "1" — the strip washes out white. 375 ns sits inside every revision's
// window; the 160 MHz LCD clock divides to it exactly (/60).
constexpr uint32_t kPclkHz = 2'666'666;

// Two DMA frame buffers for the async deferred-wait double-buffer: the driver encodes frame N+1
// into buf[1-active] while the GDMA clocks frame N out of buf[active]. buf[1] is null when the
// second allocation didn't fit (single-buffer mode — the driver runs the old wait-every-frame path
// on buf[0]). Each buffer has its OWN done-semaphore so a wait targets the right transfer.
//
// With trans_queue_depth 2 both buffers can be in flight at once (N draining while N+1 is queued),
// and the empty esp_lcd done-event carries no per-transfer token — but the i80 GDMA completes
// transfers in ENQUEUE order, so a 2-slot FIFO of enqueued buffer indices, popped in the callback,
// routes each done-signal to the buffer that actually finished. (This is the textbook completion-
// FIFO for an in-order DMA queue; the depth is 2 because at most two transfers — one per buffer —
// are ever outstanding.)
struct LcdState {
    esp_lcd_i80_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    SemaphoreHandle_t done[2] = {nullptr, nullptr};
    uint8_t* buf[2] = {nullptr, nullptr};
    size_t cap = 0;   // shared per-buffer capacity (both buffers equal)
    // In-order completion FIFO of enqueued buffer indices (0/1). enqueue at head under a critical
    // section around tx_color; the ISR pops at tail. Only ever 0..2 entries (one per buffer).
    volatile uint8_t fifo[2] = {0, 0};
    volatile uint8_t fifoHead = 0;   // next write slot (mod 2)
    volatile uint8_t fifoTail = 0;   // next read slot (mod 2)
    // Wire-time KPI: the start timestamp of the oldest in-flight transfer (paired with the FIFO,
    // so it tracks the transfer the next done-callback completes), and the last measured duration.
    volatile int64_t txStartUs[2] = {0, 0};
    volatile uint32_t lastTransmitUs = 0;
};

// Done-callback: the GDMA stream finished — pop the oldest enqueued buffer index (transfers
// complete in enqueue order), record the wire duration (now − that transfer's start), and release
// THAT buffer's waiter. esp_timer_get_time() is ISR-safe (it reads a hardware counter).
bool lcdDoneCb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user) {
    auto* st = static_cast<LcdState*>(user);
    const uint8_t slot = st->fifoTail;
    const uint8_t b = st->fifo[slot] & 1u;
    st->lastTransmitUs = static_cast<uint32_t>(esp_timer_get_time() - st->txStartUs[slot]);
    st->fifoTail = (st->fifoTail + 1u) & 1u;
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done[b], &high);
    return high == pdTRUE;
}

void destroyState(LcdState* st) {
    if (!st) return;
    if (st->io) esp_lcd_panel_io_del(st->io);
    if (st->bus) esp_lcd_del_i80_bus(st->bus);
    for (auto* b : st->buf) if (b) heap_caps_free(b);
    for (auto* s : st->done) if (s) vSemaphoreDelete(s);
    delete st;
}

// One bus + IO device + DMA buffer(s). `wantSecond` allocates the async double-buffer's second
// frame buffer (best-effort — null if it won't fit); false allocates buffer 0 only. Shared by the
// runtime init and the loopback's private bus (which passes false — one transfer).
LcdState* createState(const uint16_t* dataPins, uint8_t laneCount,
                      uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes, bool wantSecond) {
    auto* st = new (std::nothrow) LcdState();
    if (!st) return nullptr;

    esp_lcd_i80_bus_config_t busCfg = {};
    busCfg.dc_gpio_num = static_cast<gpio_num_t>(dcGpio);
    busCfg.wr_gpio_num = static_cast<gpio_num_t>(wrGpio);
    busCfg.clk_src = LCD_CLK_SRC_DEFAULT;
    // Bus width is power-of-two only (8 or 16), derived from the lane count: ≤8 → 8,
    // 9..16 → 16. The domain driver already guarantees exactly 8 or 16 real data pins.
    const size_t busWidth = laneCount <= 8 ? 8 : 16;
    busCfg.bus_width = busWidth;
    // The i80 layer REJECTS an NC data pin (unlike Parlio), so every data line up to
    // bus_width must be a real GPIO. A board that drives fewer than bus_width lanes
    // parks the unused ones on the WR "ghost pin" (hpwit's trick) — WR toggles on it
    // harmlessly, and the domain driver clears those lanes' activeMask so they idle.
    for (size_t i = 0; i < ESP_LCD_I80_BUS_WIDTH_MAX; i++) {
        busCfg.data_gpio_nums[i] = (i < busWidth) ? static_cast<gpio_num_t>(wrGpio)
                                                  : GPIO_NUM_NC;
    }
    for (uint8_t i = 0; i < laneCount && i < busWidth; i++) {
        busCfg.data_gpio_nums[i] = static_cast<gpio_num_t>(dataPins[i]);
    }
    busCfg.max_transfer_bytes = bufferBytes;
    busCfg.dma_burst_size = 64;
    if (esp_lcd_new_i80_bus(&busCfg, &st->bus) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    // One done-semaphore per buffer (buf[1]'s is created only when its buffer allocates).
    st->done[0] = xSemaphoreCreateBinary();
    if (!st->done[0]) {
        destroyState(st);
        return nullptr;
    }

    esp_lcd_panel_io_i80_config_t ioCfg = {};
    ioCfg.cs_gpio_num = GPIO_NUM_NC;    // no chip select — we own the bus
    ioCfg.pclk_hz = kPclkHz;
    // Queue depth 2 so the deferred-wait tick can hand the IO device the next frame's
    // transfer while the current one is still draining (double-buffer). The driver still
    // waits before REUSING a buffer, so at most two transfers (one per buffer) are ever
    // outstanding. Single-buffer boards allocate only buf[0] but the depth is harmless.
    ioCfg.trans_queue_depth = 2;
    ioCfg.on_color_trans_done = lcdDoneCb;
    ioCfg.user_ctx = st;
    ioCfg.lcd_cmd_bits = 0;             // no command phase ever (tx_color cmd = -1)
    ioCfg.lcd_param_bits = 0;
    ioCfg.flags.pclk_idle_low = 1;      // WR rests LOW like the data lines
    if (esp_lcd_new_panel_io_i80(st->bus, &ioCfg, &st->io) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    // DMA-capable draw buffer. The i80 GDMA can burst straight from PSRAM
    // (access_ext_mem), so allocate PSRAM-first to keep the large frame off scarce
    // internal DRAM — a 16-lane frame is 16-bit-wide and doubles the footprint, so
    // an S3 grid that overran internal SRAM at 8 lanes fits here. Fall back to
    // internal if PSRAM is absent or full (allocate-and-degrade, like platform::alloc).
    // esp_lcd_i80_alloc_draw_buffer applies the bus's DMA + ext-mem cache alignment
    // for whichever region it lands in. Zeroed so the trailing latch pad holds the
    // lines LOW.
    st->buf[0] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
        st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
    if (!st->buf[0])
        st->buf[0] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
            st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!st->buf[0]) {
        destroyState(st);
        return nullptr;
    }
    std::memset(st->buf[0], 0, bufferBytes);
    st->cap = bufferBytes;

    // Second buffer for the async double-buffer — ONLY when asked (wantSecond). Off by default, so
    // the common path allocates exactly one frame buffer and pays no async memory. When wanted, same
    // PSRAM-first-else-internal allocate-and-degrade: if it fits, arm double-buffer mode (buf[1] + its
    // semaphore); if it doesn't (memory-tight board), leave buf[1] null and run single-buffer. The
    // double-buffer is never *required* (allocate-and-degrade, ADR 0002).
    if (wantSecond) {
        st->done[1] = xSemaphoreCreateBinary();
        if (st->done[1]) {
            // PSRAM first (doesn't touch the scarce internal DMA heap, so no reserve check needed).
            st->buf[1] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
                st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
            // Internal fallback ONLY if it leaves HEAP_RESERVE intact — the second buffer is a nice-to-
            // have (allocate-and-degrade), so it must never eat the WiFi/HTTP reserve. Without this guard
            // a default-ON async board whose frame lands internal (e.g. a big moving-head frame, or the
            // P4 where PSRAM DMA degrades) would drop internal RAM below the reserve → WiFi/HTTP alloc
            // failures. Degrade to single-buffer instead.
            if (!st->buf[1]
                && heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
                       >= bufferBytes + HEAP_RESERVE) {
                st->buf[1] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
                    st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            }
            if (st->buf[1]) {
                std::memset(st->buf[1], 0, bufferBytes);
            } else {
                // No room for the second buffer — drop its unused semaphore, stay single-buffer.
                vSemaphoreDelete(st->done[1]);
                st->done[1] = nullptr;
            }
        }
    }
    return st;
}

} // namespace

bool lcdWs2812Init(LcdWs2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                   uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes,
                   bool wantSecondBuffer) {
    if (!dataPins || laneCount == 0 || bufferBytes == 0) return false;
    // Pre-check that the draw buffer can land SOMEWHERE before building the bus. createState
    // allocates PSRAM-first (the 16-lane frame is meant to live there), then falls back to internal.
    // So init is fine when EITHER the internal DMA heap has room past HEAP_RESERVE, OR PSRAM can hold
    // it. Gating only on internal would reject a board whose frame fits solely in PSRAM — exactly the
    // PSRAM-first case (an S3/16-lane grid). The HEAP_RESERVE floor only guards INTERNAL RAM (the
    // WiFi/HTTP reserve); a PSRAM buffer doesn't touch it. Degrade (return false → driver idles with a
    // status) when neither region fits.
    const bool fitsInternal =
        heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) >= bufferBytes + HEAP_RESERVE;
    const bool fitsPsram =
        heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM) >= bufferBytes;
    if (!fitsInternal && !fitsPsram) return false;
    LcdState* st = createState(dataPins, laneCount, wrGpio, dcGpio, bufferBytes, wantSecondBuffer);
    if (!st) return false;
    h.impl = st;
    return true;
}

uint8_t* lcdWs2812Buffer(const LcdWs2812Handle& h, uint8_t buffer) {
    auto* st = static_cast<LcdState*>(h.impl);
    return (st && buffer < 2) ? st->buf[buffer] : nullptr;
}

size_t lcdWs2812BufferCapacity(const LcdWs2812Handle& h) {
    auto* st = static_cast<LcdState*>(h.impl);
    return st ? st->cap : 0;
}

bool lcdWs2812Transmit(LcdWs2812Handle& h, uint8_t buffer, size_t bytes) {
    auto* st = static_cast<LcdState*>(h.impl);
    if (!st || !st->io || buffer >= 2 || !st->buf[buffer] || bytes == 0 || bytes > st->cap) return false;
    // Push this buffer onto the completion FIFO BEFORE enqueuing the transfer, so a fast done-callback
    // (which pops the FIFO) can never fire before its slot is populated. The push is the only thing the
    // ISR races, and it is a couple of plain stores into slot `fifoHead` — the ISR only ever reads slot
    // `fifoTail`, and head != tail while a transfer is in flight (the driver waits before reusing a
    // buffer), so the push and the pop touch DIFFERENT slots. That slot-disjointness is what makes the
    // push safe without a lock. **Do NOT wrap tx_color in a critical section:** esp_lcd_panel_io_tx_color
    // blocks on an internal FreeRTOS queue (xQueueSend/Receive), and calling a blocking RTOS API from
    // inside taskENTER_CRITICAL panics (spinlock held + interrupts off). Push, then enqueue outside any CS.
    const uint8_t slot = st->fifoHead;
    st->fifo[slot] = buffer;
    st->txStartUs[slot] = esp_timer_get_time();   // wire-time start stamp for this transfer
    st->fifoHead = (st->fifoHead + 1u) & 1u;
    // lcd_cmd = -1: no command phase — the transfer is one continuous GDMA data stream, gapless
    // at the pclk rate.
    const esp_err_t err = esp_lcd_panel_io_tx_color(st->io, -1, st->buf[buffer], bytes);
    if (err != ESP_OK) {
        // Enqueue failed — unwind the FIFO push so the ISR count stays balanced. Safe: a failed
        // enqueue produced no transfer, so no done-callback will pop this slot.
        st->fifoHead = (st->fifoHead + 1u) & 1u;
    }
    return err == ESP_OK;
}

void lcdWs2812Wait(LcdWs2812Handle& h, uint8_t buffer, uint32_t timeoutMs) {
    auto* st = static_cast<LcdState*>(h.impl);
    if (!st || buffer >= 2 || !st->done[buffer]) return;
    // Finite timeout, same self-healing stance as rmtWs2812Wait: a timed-out
    // frame is dropped and the driver re-encodes the whole frame next tick.
    xSemaphoreTake(st->done[buffer], pdMS_TO_TICKS(timeoutMs));
}

uint32_t lcdWs2812LastTransmitUs(const LcdWs2812Handle& h) {
    auto* st = static_cast<LcdState*>(h.impl);
    return st ? st->lastTransmitUs : 0;
}

void lcdWs2812Deinit(LcdWs2812Handle& h) {
    auto* st = static_cast<LcdState*>(h.impl);
    if (!st) return;
    destroyState(st);
    h.impl = nullptr;
}

// ---------------------------------------------------------------------------
// Loopback self-test: a private FULL-width bus (the i80 layer rejects NC data
// pins, so the driver's complete pin set is rebuilt) transmits the CALLER'S
// real frame — full size, real DMA descriptor chain, real latch pad — back to
// back like the render loop, while an RMT RX channel (rmtWs2812RxCapture with
// the DMA backend — transmitter-agnostic, reused from the RMT rig) captures
// the WHOLE frame off the jumpered rxGpio and verifies every bit. A short
// synthetic burst would miss exactly the failures a real frame hits
// (descriptor boundaries, sustained-rate stalls), so the test sends the
// genuine article.
// ---------------------------------------------------------------------------

// The capture + bit-verify half is shared with the Parlio loopback in
// detail::captureAndVerifyFrame (platform_esp32_rmt.cpp); only the i80 transmit
// differs. Declared here so this TU can call it (same pattern as loopbackJumperOk).
namespace detail {
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r);
}

RmtLoopbackResult lcdWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                    uint16_t wrGpio, uint16_t dcGpio, uint16_t rxGpio,
                                    const uint8_t* frame, size_t frameBytes,
                                    size_t dataBytes, uint8_t rowBits) {
    RmtLoopbackResult r;
    r.sent[0] = 0xA5; r.sent[1] = 0x00; r.sent[2] = 0xFF;  // pattern in every row
    if (!dataPins || laneCount == 0 || !frame || frameBytes == 0
        || dataBytes < 3 || dataBytes > frameBytes || rowBits < 8) return r;
    const uint16_t txGpio = dataPins[0];   // lane 0 carries the pattern

    r.jumperDetected = detail::loopbackJumperOk(static_cast<uint8_t>(txGpio),
                                                static_cast<uint8_t>(rxGpio));
    if (!r.jumperDetected) return r;

    // The continuity check above reset txGpio's GPIO matrix route; bus
    // creation re-claims it.
    LcdState* st = createState(dataPins, laneCount, wrGpio, dcGpio, frameBytes,
                               /*wantSecond=*/false);   // one transfer — single buffer
    if (!st) {
        ESP_LOGE(LCD_TAG, "loopback: private bus creation failed");
        return r;
    }
    std::memcpy(st->buf[0], frame, frameBytes);   // loopback uses buffer 0 only (single transfer)

    // The i80-specific transmit: ship one frame from buffer 0 and wait for its done-callback.
    // Everything else (capture, cadence, bit-verify) is the shared helper. The FIFO/semaphore
    // bookkeeping matches the runtime path: push buffer 0, enqueue, the ISR pops and gives done[0].
    auto transmitOnce = [st, frameBytes]() {
        // Loopback self-test path (not the render hot path): surface a failed
        // enqueue or a done-callback timeout instead of letting it show up only as
        // a later capture mismatch (same handling as the Parlio sibling).
        st->fifo[st->fifoHead] = 0;
        st->fifoHead = (st->fifoHead + 1u) & 1u;
        const esp_err_t err = esp_lcd_panel_io_tx_color(st->io, -1, st->buf[0], frameBytes);
        if (err != ESP_OK) {
            st->fifoHead = (st->fifoHead + 1u) & 1u;   // unwind the push
            ESP_LOGE(LCD_TAG, "loopback: tx enqueue failed (%s)", esp_err_to_name(err));
            return;
        }
        if (xSemaphoreTake(st->done[0], pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(LCD_TAG, "loopback: tx done-callback timed out");
    };
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, kPclkHz,
                                  LCD_TAG, transmitOnce, r);
    destroyState(st);
    return r;
}

} // namespace mm::platform

#else  // !SOC_LCDCAM_I80_LCD_SUPPORTED — inert stubs so classic ESP32 links

namespace mm::platform {

bool lcdWs2812Init(LcdWs2812Handle&, const uint16_t*, uint8_t, uint16_t, uint16_t,
                   size_t, bool) {
    return false;
}
uint8_t* lcdWs2812Buffer(const LcdWs2812Handle&, uint8_t) { return nullptr; }
size_t lcdWs2812BufferCapacity(const LcdWs2812Handle&) { return 0; }
bool lcdWs2812Transmit(LcdWs2812Handle&, uint8_t, size_t) { return false; }
void lcdWs2812Wait(LcdWs2812Handle&, uint8_t, uint32_t) {}
uint32_t lcdWs2812LastTransmitUs(const LcdWs2812Handle&) { return 0; }
void lcdWs2812Deinit(LcdWs2812Handle&) {}
RmtLoopbackResult lcdWs2812Loopback(const uint16_t*, uint8_t, uint16_t, uint16_t,
                                    uint16_t, const uint8_t*, size_t, size_t, uint8_t) {
    return {};
}

} // namespace mm::platform

#endif  // SOC_LCDCAM_I80_LCD_SUPPORTED
