// Parallel WS2812 output over the ESP-IDF esp_lcd i80 bus — the peripheral half of I80LedDriver
// (src/light/drivers/I80LedDriver.h), which does all the domain work: applies Correction and
// 3-slot-encodes every light into the DMA frame buffer (ParallelSlots.h). This file owns only the
// peripheral — the esp_lcd i80 bus, the IO device, the DMA-capable frame buffer, transmit + wait,
// and the loopback test's TX side. No domain logic here.
//
// Design: the whole frame is pre-encoded into ONE buffer and sent as ONE gapless GDMA stream
// (tx_color with lcd_cmd = -1 → pure data phase). Once started, no CPU work remains until the done
// callback — there is no refill deadline for WiFi to miss, the deliberate difference from the
// ISR-refilled rings in the hpwit/FastLED I2S lineage this design studied.
//
// **One seam, two silicon backends.** IDF's esp_lcd component exposes ONE public i80 API
// (esp_lcd_new_i80_bus / esp_lcd_panel_io_tx_color / esp_lcd_i80_alloc_draw_buffer) and routes it,
// via its own CMake, to whichever peripheral the chip has: **LCD_CAM** on the S3/P4
// (esp_lcd_panel_io_i80.c) or the **I2S peripheral in i80/LCD mode** on the classic ESP32
// (esp_lcd_panel_io_i2s.c). Both do WHOLE-FRAME chained DMA with WR/DC + 8/16 bus width, so this
// file's body is 100% generic i80 and serves both — the single I80LedDriver runs on all three.
//
// Gated on SOC_LCD_I80_SUPPORTED (true on classic + S3 + P4) with inert stubs otherwise. This is the
// BROAD macro on purpose (not the narrower SOC_LCDCAM_I80_LCD_SUPPORTED), precisely so the classic
// I2S backend compiles here too. (An earlier note warned against the broad macro — that was before
// an i2s-backed driver existed, so compiling this onto classic init'd a bus with no consumer; now
// I80LedDriver is that consumer on every i80 chip.)

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

// SOC_LCD_I80_SUPPORTED: the generic esp_lcd i80 API, backed by LCD_CAM on the S3/P4 and by
// the I2S peripheral on the classic ESP32 — both selected by IDF's own CMake. The body below
// is backend-agnostic. (esp_lcd i80 headers exist wherever SOC_LCD_I80_SUPPORTED is set.)
#if SOC_LCD_I80_SUPPORTED

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

static const char* I80_TAG = "mm_i80";

// The lcd_cmd passed to esp_lcd_panel_io_tx_color. LCD_CAM (S3/P4) takes -1 = "no command phase"
// (pure data). The classic I2S backend has an UNCONDITIONAL command phase (see the lcd_cmd_bits note
// in createState), so it needs a real 8-bit command: 0 is a benign no-op byte that completes the
// backend's command poll before the WS2812 data frame.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
constexpr int kI80Cmd = -1;
#else
constexpr int kI80Cmd = 0;
#endif

// 3 slots per WS2812 bit (the ParallelSlots.h contract): 2.67 MHz pclk = 375 ns
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
struct I80State {
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
//
// IRAM_ATTR: the DMA-done ISR calls this from interrupt context, so keeping it out of flash is the
// correct, defensive choice (matches the sibling Parlio done-cb) — everything it touches is IRAM-safe
// (esp_timer_get_time reads a hardware counter, xSemaphoreGiveFromISR, plain member stores). The
// classic I2S backend's extra quirk (an unconditional command phase that busy-waits) is handled by
// the kI80Cmd / lcd_cmd_bits split above, not here.
bool IRAM_ATTR i80DoneCb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* user) {
    auto* st = static_cast<I80State*>(user);
    const uint8_t slot = st->fifoTail;
    const uint8_t b = st->fifo[slot] & 1u;
    const int64_t now = esp_timer_get_time();
    st->lastTransmitUs = static_cast<uint32_t>(now - st->txStartUs[slot]);
    st->fifoTail = (st->fifoTail + 1u) & 1u;
    // In-order queue: a buffer already queued behind this one starts the instant this transfer ends —
    // stamp its true start here, since the transmit call deliberately skipped stamping it (the wire was
    // busy). Without this the second buffer's wireUs would include this one's remaining wire time.
    if (st->fifoTail != st->fifoHead) st->txStartUs[st->fifoTail] = now;
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done[b], &high);
    return high == pdTRUE;
}

void destroyState(I80State* st) {
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
I80State* createState(const uint16_t* dataPins, uint8_t laneCount,
                      uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes, bool wantSecond) {
    auto* st = new (std::nothrow) I80State();
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
    ioCfg.on_color_trans_done = i80DoneCb;
    ioCfg.user_ctx = st;
    // LCD_CAM backend (S3/P4): no command phase — tx_color(cmd = -1) skips it, so cmd_bits = 0.
    // Classic I2S backend: its tx_color has an UNCONDITIONAL command phase that busy-waits for the
    // command DMA's TX_EOF. With cmd_bits = 0 that command buffer is zero-length, the DMA never
    // asserts EOF, and the poll loop hangs → interrupt-watchdog reset (WROVER bench, 2026-07-13). So
    // give the i2s backend a real 8-bit command phase: one benign byte clocks out (completing the
    // poll) before the WS2812 data frame. It rides in the inter-frame idle-LOW gap, within the strand
    // latch window, so the strands ignore it. (See the classic-only cmd value at each tx_color call.)
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    ioCfg.lcd_cmd_bits = 0;
#else
    ioCfg.lcd_cmd_bits = 8;
#endif
    ioCfg.lcd_param_bits = 0;
    ioCfg.flags.pclk_idle_low = 1;      // WR rests LOW like the data lines
    if (esp_lcd_new_panel_io_i80(st->bus, &ioCfg, &st->io) != ESP_OK) {
        destroyState(st);
        return nullptr;
    }

    // DMA-capable draw buffer. On the LCD_CAM backend (S3/P4) the i80 GDMA can burst straight from
    // PSRAM (access_ext_mem), so allocate PSRAM-first to keep the large 16-bit frame off scarce
    // internal DRAM. But the classic ESP32's I2S-i80 backend CANNOT DMA from PSRAM —
    // esp_lcd_i80_alloc_draw_buffer rejects MALLOC_CAP_SPIRAM there with "external memory is not
    // supported" (bench-confirmed on the WROVER, 2026-07-13). So the PSRAM attempt is compiled in
    // ONLY on the LCD_CAM chips; the classic backend goes straight to internal DMA RAM. IDF also does
    // its own DMA/ext-mem cache alignment for whichever region it lands in. Zeroed so the trailing
    // latch pad holds the lines LOW.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
    st->buf[0] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
        st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
    if (!st->buf[0])
#endif
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
            // PSRAM first (LCD_CAM only — the classic I2S backend can't DMA from PSRAM, see buf[0]).
            // PSRAM doesn't touch the scarce internal DMA heap, so no reserve check needed there.
#if SOC_LCDCAM_I80_LCD_SUPPORTED
            st->buf[1] = static_cast<uint8_t*>(esp_lcd_i80_alloc_draw_buffer(
                st->io, bufferBytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
#endif
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

bool i80Ws2812Init(I80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
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
    I80State* st = createState(dataPins, laneCount, wrGpio, dcGpio, bufferBytes, wantSecondBuffer);
    if (!st) return false;
    h.impl = st;
    return true;
}

uint8_t* i80Ws2812Buffer(const I80Ws2812Handle& h, uint8_t buffer) {
    auto* st = static_cast<I80State*>(h.impl);
    return (st && buffer < 2) ? st->buf[buffer] : nullptr;
}

size_t i80Ws2812BufferCapacity(const I80Ws2812Handle& h) {
    auto* st = static_cast<I80State*>(h.impl);
    return st ? st->cap : 0;
}

bool i80Ws2812Transmit(I80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    auto* st = static_cast<I80State*>(h.impl);
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
    const bool wireIdle = (st->fifoHead == st->fifoTail);   // nothing in flight → this one starts NOW
    st->fifo[slot] = buffer;
    // Stamp the wire-time start only when the wire is IDLE (enqueue == hardware-start). If a transfer is
    // already clocking out, this one does not begin until that one ends, so stamping here would fold the
    // predecessor's remaining wire time into this buffer's measured duration. The done-callback stamps it
    // instead, at the moment the hardware actually starts it.
    if (wireIdle) st->txStartUs[slot] = esp_timer_get_time();
    st->fifoHead = (st->fifoHead + 1u) & 1u;
    // lcd_cmd = -1: no command phase — the transfer is one continuous GDMA data stream, gapless
    // at the pclk rate.
    const esp_err_t err = esp_lcd_panel_io_tx_color(st->io, kI80Cmd, st->buf[buffer], bytes);
    if (err != ESP_OK) {
        // Enqueue failed — unwind the FIFO push so the ISR count stays balanced. Safe: a failed
        // enqueue produced no transfer, so no done-callback will pop this slot.
        st->fifoHead = (st->fifoHead + 1u) & 1u;
    }
    return err == ESP_OK;
}

bool i80Ws2812Wait(I80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs) {
    auto* st = static_cast<I80State*>(h.impl);
    if (!st || buffer >= 2 || !st->done[buffer]) return true;   // nothing to wait on = not in flight
    // Report whether the transfer actually completed. On a timeout the DMA may still be reading this
    // buffer, so the caller must keep it marked in-flight rather than re-encoding into it — handing a
    // live DMA a half-rewritten buffer is exactly the frame corruption the timeout is meant to avoid.
    return xSemaphoreTake(st->done[buffer], pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle& h) {
    auto* st = static_cast<I80State*>(h.impl);
    return st ? st->lastTransmitUs : 0;
}

void i80Ws2812Deinit(I80Ws2812Handle& h) {
    auto* st = static_cast<I80State*>(h.impl);
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

RmtLoopbackResult i80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
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
    I80State* st = createState(dataPins, laneCount, wrGpio, dcGpio, frameBytes,
                               /*wantSecond=*/false);   // one transfer — single buffer
    if (!st) {
        ESP_LOGE(I80_TAG, "loopback: private bus creation failed");
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
        const esp_err_t err = esp_lcd_panel_io_tx_color(st->io, kI80Cmd, st->buf[0], frameBytes);
        if (err != ESP_OK) {
            st->fifoHead = (st->fifoHead + 1u) & 1u;   // unwind the push
            ESP_LOGE(I80_TAG, "loopback: tx enqueue failed (%s)", esp_err_to_name(err));
            return;
        }
        if (xSemaphoreTake(st->done[0], pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(I80_TAG, "loopback: tx done-callback timed out");
    };
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, kPclkHz,
                                  I80_TAG, transmitOnce, r);
    destroyState(st);
    return r;
}

} // namespace mm::platform

#else  // !SOC_LCD_I80_SUPPORTED — inert stubs so a chip with no i80 (LCD_CAM or I2S) links

namespace mm::platform {

bool i80Ws2812Init(I80Ws2812Handle&, const uint16_t*, uint8_t, uint16_t, uint16_t,
                   size_t, bool) {
    return false;
}
uint8_t* i80Ws2812Buffer(const I80Ws2812Handle&, uint8_t) { return nullptr; }
size_t i80Ws2812BufferCapacity(const I80Ws2812Handle&) { return 0; }
bool i80Ws2812Transmit(I80Ws2812Handle&, uint8_t, size_t) { return false; }
bool i80Ws2812Wait(I80Ws2812Handle&, uint8_t, uint32_t) { return true; }
uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle&) { return 0; }
void i80Ws2812Deinit(I80Ws2812Handle&) {}
RmtLoopbackResult i80Ws2812Loopback(const uint16_t*, uint8_t, uint16_t, uint16_t,
                                    uint16_t, const uint8_t*, size_t, size_t, uint8_t) {
    return {};
}

} // namespace mm::platform

#endif  // SOC_LCD_I80_SUPPORTED
