// HUB75 panel output over the ESP-IDF esp_lcd i80 bus: the peripheral half of Hub75Driver
// (src/light/drivers/Hub75Driver.h), which does the domain work: applies Correction and encodes the
// rendered frame into bit planes (light/drivers/Hub75Slots.h). This file owns only the peripheral:
// the bus, the DMA frame buffer, and the continuous scan. No domain logic here.
//
// **HUB75 output is CONTINUOUS, not one-shot, and that is the design difference from every other
// output seam in this tree.** A WS2812 strand latches a frame and holds it; a HUB75 panel holds
// nothing. It displays only while it is being clocked, so the controller re-scans the same buffer
// forever and the driver writes the next frame into it between scans. There is no per-frame
// transmit call: `hub75Start` arms the loop once, and from then on the panel is lit by the DMA.
//
// esp_lcd's i80 transaction model is built for discrete frames, so the scan is kept continuous the
// producer/consumer way: a refill task queues the same buffer again the moment the previous scan
// completes, and the peripheral's own ISR starts it. Nothing re-arms from interrupt context, because
// esp_lcd_panel_io_tx_color blocks when the transaction pool is exhausted, and the done callback
// runs BEFORE the finished slot is recycled: called from there it always blocks, and a blocking
// queue wait in an ISR is an interrupt watchdog panic (bench-verified, CPU0, every board preset).
//
// Gated on SOC_LCDCAM_I80_LCD_SUPPORTED (S3/P4/S31) rather than the broader SOC_LCD_I80_SUPPORTED:
// the classic ESP32's I2S-backed i80 cannot DMA from PSRAM, and § GPIO requirements in the plan
// rules the classic out on pin count before memory is even considered. So there is no classic
// backend to write, and the broad macro would only compile dead code onto it.

#include "platform/platform.h"
// The wire format, for the frame size: one home for it, shared with the driver that encodes.
#include "light/drivers/Hub75Slots.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#define MM_HUB75_LCDCAM  SOC_LCDCAM_I80_LCD_SUPPORTED
#define MM_HUB75_PARLIO   (SOC_PARLIO_SUPPORTED && SOC_PARLIO_TX_UNIT_MAX_DATA_WIDTH >= 16)

#if MM_HUB75_LCDCAM || MM_HUB75_PARLIO

#if MM_HUB75_LCDCAM
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#endif
#if MM_HUB75_PARLIO
#include "driver/parlio_tx.h"
#endif
#include "esp_heap_caps.h"
#include "esp_cache.h"   // writeback: the CPU fills the frame, the DMA reads it
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstring>
#include <new>

namespace mm::platform {

namespace {

// The shift clock. 20 MHz is the rate the parallel LED path already runs on this silicon, and the
// shift-register analysis records hpwit driving a '595 chain at 19.2 MHz, so 20 MHz is a proven
// working rate for the same class of load rather than a number picked from a datasheet maximum.
//
// It must be an EXACT divide of the 80 MHz bus resolution: esp_lcd silently rounds an inexact pclk
// DOWN into a wrong waveform rather than reporting it, which the i80 driver learned the hard way.
constexpr uint32_t kPclkHz = 20'000'000;

// Parlio's hardware ceiling: PER_FRAME is 0x7FFFF bits on every Parlio-capable target, which is
// 65,535 BYTES whatever the bus width. The same constant platform_esp32_parlio.cpp records, and it
// is what decides that four 8-bit panels (65,792 bytes) are an LCD_CAM job.
constexpr size_t kParlioMaxTransferBytes = 0x7FFFF / 8;

const char* g_lastError = nullptr;

struct Hub75State {
    Hub75Backend backend = Hub75Backend::LcdCam;
#if MM_HUB75_LCDCAM
    esp_lcd_i80_bus_handle_t bus = nullptr;
    esp_lcd_panel_io_handle_t io = nullptr;
    TaskHandle_t refill = nullptr;         // keeps the transaction queue full, see hub75RefillTask
    std::atomic<bool> refillParked{false}; // the refill task has stopped touching this state
#endif
#if MM_HUB75_PARLIO
    parlio_tx_unit_handle_t parlio = nullptr;
#endif
    uint8_t* frame = nullptr;
    size_t   frameBytes = 0;
    size_t   frameAlloc = 0;      // what was allocated: rounded up to whole cache lines
    bool     framePsram = false;  // cached external memory, so writes need a flush
    // ATOMIC: the ISR callbacks read this to decide whether to touch `frame`, and teardown clears
    // it from a task. A plain bool orders nothing between the two, so a callback could pass the
    // check and then read a freed frame.
    std::atomic<bool> running{false};
    // Refresh measurement. The done callback counts scans, so dividing that by elapsed time is the
    // panel's ACTUAL refresh rather than a calculation: it is what a tester
    // reports back, and the docs' predicted table is what they compare it against.
    // ATOMIC, not volatile. The counter is written in the DMA completion ISR and read from the
    // 1 Hz status tick, which is exactly the case volatile does NOT cover: it prevents the compiler
    // caching the value but orders nothing between the two contexts. Relaxed ordering is enough:
    // nothing else is published alongside it, and a refresh figure one scan stale is meaningless.
    std::atomic<uint32_t> scans{0};
    uint32_t windowStartUs = 0;
    std::atomic<uint16_t> refreshHz{0};
};

// Keeping the panel clocked, which the two peripherals solve differently.
//
// The panel is lit only while it is being clocked, so any gap between frames is a visible dim.
// PARLIO re-sends by itself: `loop_transmission` repeats the buffer until the unit is disabled, so
// its callback only counts. LCD_CAM has no such flag, so a task queues the next scan as each one
// ends (hub75RefillTask). The callback here only counts as well: an ISR never calls
// esp_lcd_panel_io_tx_color (file header).
#if MM_HUB75_LCDCAM
bool IRAM_ATTR hub75DoneCb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
    auto* st = static_cast<Hub75State*>(ctx);
    if (!st || !st->running.load(std::memory_order_acquire)) return false;
    st->scans.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// One scan is in flight at a time (kLcdQueueDepth). Each esp_lcd_panel_io_tx_color call blocks,
// legitimately in task context, until that scan finishes and recycles its slot, then queues the
// same buffer again; the ISR starts it at once because the done status is still pending. The gap
// is one task wake, microseconds against a scan of a millisecond or more, and it is uniform, so it
// is a fraction of a percent of brightness rather than a flicker. Depth 1 is not a choice: the bus
// owns ONE DMA link list, and queueing a second scan mounts it while the first still owns the
// descriptors, which fails with "gdma-link: lli full" (bench-measured at depth 2). The same line
// can appear once or twice right after start while the DMA hands the first scan's descriptors
// back; the ISR then restarts the link list as it stands, which already holds this frame, so the
// panel loses nothing (platform_esp32_i80.cpp records the same behavior and its investigation).
// Priority 6 sits above the encode task (5): a refill that lost the CPU for a whole scan would
// leave the panel dark for that long. The task never deletes itself, so teardown can always delete
// it by handle without racing a self-delete.
constexpr size_t kLcdQueueDepth = 1;

void hub75RefillTask(void* arg) {
    auto* st = static_cast<Hub75State*>(arg);
    while (st->running.load(std::memory_order_acquire)) {
        if (esp_lcd_panel_io_tx_color(st->io, -1, st->frame, st->frameBytes) != ESP_OK) break;
    }
    st->refillParked.store(true, std::memory_order_release);
    vTaskSuspend(nullptr);
}
#endif

#if MM_HUB75_PARLIO
// Parlio counts only: the unit loops the buffer on its own, so re-arming here would queue a second
// transmission against a transfer that never ends.
bool IRAM_ATTR hub75ParlioDoneCb(parlio_tx_unit_handle_t,
                                 const parlio_tx_done_event_data_t*, void* ctx) {
    auto* st = static_cast<Hub75State*>(ctx);
    if (!st || !st->running.load(std::memory_order_acquire)) return false;
    st->scans.fetch_add(1, std::memory_order_relaxed);
    return false;
}
#endif

void destroyState(Hub75State* st) {
    if (!st) return;
    // Order matters: clear the flag, stop the peripheral (which disables its callback and waits for
    // an in-flight one), and only then free the frame the callback reads.
    st->running.store(false, std::memory_order_release);
#if MM_HUB75_LCDCAM
    if (st->refill) {
        // The refill task is inside tx_color until the current scan ends (about a millisecond),
        // then reads the flag and parks. It must be gone before io_del, which drains the same done
        // queue. The wait is bounded so a peripheral that never completes cannot hang teardown.
        for (int i = 0; i < 100 && !st->refillParked.load(std::memory_order_acquire); i++) {
            vTaskDelay(1);
        }
        vTaskDelete(st->refill);
        st->refill = nullptr;
    }
    if (st->io) esp_lcd_panel_io_del(st->io);
    if (st->bus) esp_lcd_del_i80_bus(st->bus);
#endif
#if MM_HUB75_PARLIO
    if (st->parlio) {
        parlio_tx_unit_disable(st->parlio);
        parlio_del_tx_unit(st->parlio);
    }
#endif
    if (st->frame) heap_caps_free(st->frame);
    delete st;
}

/// Every HUB75 line, in the bit order Hub75Layout declares. The encoder writes bit N of each bus
/// byte for line N, so the bus data pins must be wired in that same order: index N of this array is
/// the GPIO that carries bit N.
///
/// Returns false when a required line is unset. A HUB75 port with a missing line is not a degraded
/// port, it is a dark one, so this refuses rather than initializing something that cannot work.
bool buildPinOrder(const Hub75Pins& p, uint8_t scanRate, gpio_num_t* out, size_t& count) {
    constexpr uint16_t kUnset = 0xFFFF;
    // Bits 0-5: color. Bits 8-12: address. 13: latch. 14: OE. The gap at 6-7 is deliberate, because it
    // matches Hub75Layout's defaults, so encoder and bus agree without either knowing the other.
    const uint16_t order[15] = {
        p.r1, p.g1, p.b1, p.r2, p.g2, p.b2,
        kUnset, kUnset,               // 6, 7: unused by the default layout
        p.a, p.b, p.c, p.d, p.e,
        p.lat, p.oe,
    };
    // How many address lines this panel's scan rate actually uses: a 1/8 panel leaves D and E
    // unwired, and demanding them would refuse a perfectly good port.
    const uint8_t addrBits = (scanRate > 16) ? 5 : (scanRate > 8) ? 4 : 3;

    count = 0;
    for (size_t bit = 0; bit < 15; bit++) {
        const bool isAddr = (bit >= 8 && bit <= 12);
        const bool needed = !isAddr || (bit - 8) < addrBits;
        if (!needed || order[bit] == kUnset) {
            // An unneeded or unwired line still occupies its bus position: the encoder puts data at
            // fixed bit offsets, so a hole cannot shift the lines above it. NC parks it.
            out[bit] = GPIO_NUM_NC;
            count = bit + 1;
            continue;
        }
        out[bit] = static_cast<gpio_num_t>(order[bit]);
        count = bit + 1;
    }

    // The lines without which nothing lights.
    const uint16_t required[] = {p.r1, p.g1, p.b1, p.r2, p.g2, p.b2,
                                 p.a, p.b, p.c, p.clk, p.lat, p.oe};
    for (uint16_t pin : required) {
        if (pin == kUnset) return false;
    }
    if (addrBits >= 4 && p.d == kUnset) return false;
    if (addrBits >= 5 && p.e == kUnset) return false;
    return true;
}

}  // namespace

const char* hub75LastError() { return g_lastError; }

const char* hub75BackendLabel(Hub75Backend backend) {
    return backend == Hub75Backend::Parlio ? "Parlio" : "LCD_CAM";
}

bool hub75BackendAvailable(Hub75Backend backend, size_t frameBytes) {
    if (backend == Hub75Backend::LcdCam) {
#if MM_HUB75_LCDCAM
        // LCD_CAM has no transfer-size cap of its own: the frame size is a memory question
        // (internal DRAM first, see the allocation in hub75Init).
        (void)frameBytes;
        return true;
#else
        return false;
#endif
    }
#if MM_HUB75_PARLIO
    // Parlio's single-shot transfer caps at 65,535 bytes, width-invariant. Offering it for a frame
    // that cannot fit would be a choice that fails at init, so the ceiling is part of "available".
    return frameBytes <= kParlioMaxTransferBytes;
#else
    (void)frameBytes;
    return false;
#endif
}

bool hub75Init(Hub75Handle& h, Hub75Backend backend, const Hub75Pins& pins,
               uint16_t width, uint16_t height, uint8_t scanRate, uint8_t bitDepth) {
    hub75Deinit(h);
    g_lastError = nullptr;

    if (width == 0 || height == 0 || scanRate == 0) {
        g_lastError = "set the panel size and scan rate";
        return false;
    }
    // A REAL panel is 1/8, 1/16 or 1/32. Checked here rather than in the encoder, which only asks
    // whether a geometry can be encoded: buildPinOrder derives the address line count from this,
    // so a value like 3 divides some heights cleanly and then asks for an address width no panel
    // has. The driver's select offers only these three; this catches a config file that does not.
    if (scanRate != 8 && scanRate != 16 && scanRate != 32) {
        g_lastError = "scan rate must be 1/8, 1/16 or 1/32";
        return false;
    }
    // Same cap as the encoder's Hub75Geometry::valid: unweighted planes above 4 cost a scan
    // pass and a share of the frame for nothing the eye can see. Lifts when planes are weighted.
    if (bitDepth < 2 || bitDepth > 4) {
        g_lastError = "bit depth must be 2 to 4";
        return false;
    }
    if (height % scanRate != 0 || ((height / scanRate) % 2) != 0) {
        g_lastError = "the scan rate does not divide the panel height into row pairs";
        return false;
    }

    gpio_num_t busPins[16] = {};
    size_t busCount = 0;
    if (!buildPinOrder(pins, scanRate, busPins, busCount)) {
        g_lastError = "a HUB75 line is unset: every color, address and control pin is required";
        return false;
    }

    // The size comes from the encoder, which is the one home for the wire format. Recomputing
    // it here is how the two silently disagreed: this must be the exact buffer hub75Encode
    // writes, or a correct encode overruns the DMA frame.
    mm::Hub75Geometry geo;
    geo.width = width; geo.height = height; geo.scanRate = scanRate; geo.bitDepth = bitDepth;
    if (!geo.valid()) {
        g_lastError = "the panel geometry does not divide into scan row pairs";
        return false;
    }
    const size_t frameBytesPre = geo.frameBytes();
    if (!hub75BackendAvailable(backend, frameBytesPre)) {
        // Named rather than silently substituted. A user who picked Parlio for a reason (the LCD_CAM
        // is driving their strips) must hear that this panel will not fit on it, not find themselves
        // moved onto the peripheral they were keeping free.
        g_lastError = (backend == Hub75Backend::Parlio)
            ? "this panel is too big for Parlio (65,535 byte limit): use LCD_CAM or lower the depth"
            : "this chip has no LCD_CAM";
        return false;
    }

    auto* st = new (std::nothrow) Hub75State();
    if (!st) {
        g_lastError = "out of memory";
        return false;
    }
    st->backend = backend;

    if (backend == Hub75Backend::Parlio) {
#if MM_HUB75_PARLIO
        parlio_tx_unit_config_t cfg = {};
        cfg.clk_src = PARLIO_CLK_SRC_DEFAULT;
        cfg.data_width = 16;              // the layout puts OE at bit 14
        cfg.clk_in_gpio_num = GPIO_NUM_NC;
        cfg.valid_gpio_num = GPIO_NUM_NC;
        cfg.clk_out_gpio_num = static_cast<gpio_num_t>(pins.clk);
        // Parlio ACCEPTS an NC data lane where the i80 layer rejects one, so the layout's
        // holes (bits 6, 7 and 15) stay unparked here. The i80 branch below has to park
        // them on WR instead.
        for (size_t i = 0; i < 16; i++) {
            cfg.data_gpio_nums[i] = (i < busCount) ? busPins[i] : GPIO_NUM_NC;
        }
        cfg.output_clk_freq_hz = kPclkHz;
        cfg.trans_queue_depth = 1;
        cfg.max_transfer_size = frameBytesPre;
        cfg.shift_edge = PARLIO_SHIFT_EDGE_POS;   // shift on the rising edge, as the LED path does
        if (parlio_new_tx_unit(&cfg, &st->parlio) != ESP_OK) {
            g_lastError = "could not claim the Parlio unit: is another driver using it?";
            destroyState(st);
            return false;
        }
        parlio_tx_event_callbacks_t cbs = {};
        cbs.on_trans_done = hub75ParlioDoneCb;
        parlio_tx_unit_register_event_callbacks(st->parlio, &cbs, st);
        parlio_tx_unit_enable(st->parlio);
#else
        g_lastError = "this chip has no Parlio";
        destroyState(st);
        return false;
#endif
    } else {
#if MM_HUB75_LCDCAM
    // The bus is 16 bits wide because the layout puts OE at bit 14: the color and control lines do
    // not fit in 8. A board is free to re-map them into the low byte (Hub75Layout allows it), but
    // the default wiring is the one this must support.
    esp_lcd_i80_bus_config_t busCfg = {};
    // DC rides WR, and it must be a REAL gpio. A HUB75 panel has no command phase, so there is
    // nothing for DC to signal, but the i80 layer validates `dc_gpio_num >= 0` before it
    // configures anything (esp_lcd_panel_io_i80.c, lcd_i80_bus_configure_gpio) and rejects the
    // whole bus otherwise. NC here failed every board preset identically with "configure GPIO
    // failed", which reads like a pin conflict and is not one. Pointing it at WR costs no extra
    // GPIO: tx_color(-1) never asserts the command phase, so DC never toggles.
    busCfg.dc_gpio_num = static_cast<gpio_num_t>(pins.clk);
    busCfg.wr_gpio_num = static_cast<gpio_num_t>(pins.clk);   // WR IS the panel's shift clock
    busCfg.clk_src = LCD_CLK_SRC_DEFAULT;
    busCfg.bus_width = 16;
    // The i80 layer REJECTS an NC data pin, unlike Parlio, so every lane up to bus_width must
    // be a real GPIO. HUB75 uses 14 of 16 (bits 6, 7 and 15 are holes in the layout), so the
    // unused lanes park on WR, the same ghost-pin trick platform_esp32_i80.cpp uses: WR toggles
    // on them harmlessly and no panel line is attached. Leaving them NC is what made
    // esp_lcd_new_i80_bus return "configure GPIO failed" on every board preset.
    for (size_t i = 0; i < ESP_LCD_I80_BUS_WIDTH_MAX; i++) {
        busCfg.data_gpio_nums[i] = (i < 16) ? static_cast<gpio_num_t>(pins.clk) : GPIO_NUM_NC;
    }
    for (size_t i = 0; i < busCount && i < 16; i++) {
        if (busPins[i] != GPIO_NUM_NC) {
            busCfg.data_gpio_nums[i] = busPins[i];
        }
    }

    busCfg.max_transfer_bytes = frameBytesPre;
    busCfg.dma_burst_size = 64;
    const esp_err_t busErr = esp_lcd_new_i80_bus(&busCfg, &st->bus);
    if (busErr != ESP_OK) {
        // Name the REASON rather than guessing at one. "another driver using it" sent a user
        // hunting a conflict that did not exist: the real fault was ESP_ERR_INVALID_ARG from a
        // GPIO the bus would not configure, and only the IDF log said so.
        g_lastError = (busErr == ESP_ERR_INVALID_ARG)
            ? "the LCD bus refused a pin: check the board's GPIO map"
            : (busErr == ESP_ERR_NOT_FOUND)
                ? "no free LCD bus: another driver is using it"
                : "the LCD bus would not start";
        destroyState(st);
        return false;
    }

    esp_lcd_panel_io_i80_config_t ioCfg = {};
    ioCfg.cs_gpio_num = GPIO_NUM_NC;
    ioCfg.pclk_hz = kPclkHz;
    ioCfg.trans_queue_depth = kLcdQueueDepth;   // why 1: see hub75RefillTask
    ioCfg.on_color_trans_done = hub75DoneCb;
    ioCfg.user_ctx = st;
    ioCfg.lcd_cmd_bits = 0;     // LCD_CAM: tx_color(-1) skips the command phase entirely
    ioCfg.lcd_param_bits = 0;
    ioCfg.flags.pclk_idle_low = 1;
    if (esp_lcd_new_panel_io_i80(st->bus, &ioCfg, &st->io) != ESP_OK) {
        g_lastError = "could not configure the LCD device";
        destroyState(st);
        return false;
    }
#else
        g_lastError = "this chip has no LCD_CAM";
        destroyState(st);
        return false;
#endif
    }

    // Internal DRAM first. The S3's LCD_CAM cannot be fed from PSRAM at this clock: the LCD keeps
    // clocking at 40 MB/s while the GDMA falls behind on PSRAM reads, signals "done" with
    // descriptors still owned by the DMA, and every following mount fails with "gdma-link: lli
    // full" (bench-measured here with a 16.5 KB frame, and platform_esp32_i80.cpp records the same
    // cliff for LED frames). A single 64x64 panel at 4 bits is 16.5 KB and four are 66 KB, both
    // internal-sized. PSRAM is the fallback for a frame that does not fit, so a large wall still
    // starts and degrades visibly rather than refusing.
    st->frame = static_cast<uint8_t*>(heap_caps_calloc(1, frameBytesPre, MALLOC_CAP_DMA));
    st->frameAlloc = frameBytesPre;
    if (!st->frame) {
        // ALIGNED, and the size rounded up to match: a PSRAM DMA buffer must start on a cache-line
        // boundary and span whole lines, or the writeback flushes a partial line and the burst
        // reads a neighbour's bytes. heap_caps_calloc gives neither guarantee.
        // 64 bytes is the external-memory cache line on every chip with PSRAM (S3, P4, S31). The
        // IDF query for it lives in esp_private/, and reaching into a private header to learn a
        // constant the silicon fixes is a worse dependency than naming the constant.
        constexpr size_t kCacheLine = 64;
        const size_t frameAlloc = ((frameBytesPre + kCacheLine - 1) / kCacheLine) * kCacheLine;
        st->frame = static_cast<uint8_t*>(
            heap_caps_aligned_calloc(kCacheLine, 1, frameAlloc, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
        st->frameAlloc = frameAlloc;
        st->framePsram = st->frame != nullptr;
    }
    if (!st->frame) {
        g_lastError = "the panel frame does not fit in memory: lower the bit depth";
        destroyState(st);
        return false;
    }
    st->frameBytes = frameBytesPre;
    st->windowStartUs = static_cast<uint32_t>(esp_timer_get_time());

    h.impl = st;
    return true;
}

uint8_t* hub75Buffer(const Hub75Handle& h) MM_NONBLOCKING {
    return h.impl ? static_cast<Hub75State*>(h.impl)->frame : nullptr;
}

size_t hub75BufferCapacity(const Hub75Handle& h) MM_NONBLOCKING {
    return h.impl ? static_cast<Hub75State*>(h.impl)->frameBytes : 0;
}

bool hub75Start(Hub75Handle& h) {
    auto* st = static_cast<Hub75State*>(h.impl);
    if (!st || !st->frame) return false;
    if (st->running.load(std::memory_order_acquire)) return true;   // starting twice would queue two
    st->running.store(true, std::memory_order_release);
    bool ok = false;
    if (st->backend == Hub75Backend::Parlio) {
#if MM_HUB75_PARLIO
        parlio_transmit_config_t xcfg = {};
        // Repeat forever until the unit is disabled: this is what keeps the panel clocked without
        // an ISR re-arming it, and it is why the Parlio done callback only counts.
        xcfg.flags.loop_transmission = 1;
        // Parlio counts in BITS, the one API difference from the LCD_CAM path.
        ok = st->parlio && parlio_tx_unit_transmit(st->parlio, st->frame,
                                                   st->frameBytes * 8, &xcfg) == ESP_OK;
#endif
    } else {
#if MM_HUB75_LCDCAM
        // The first scan is queued here, so the refill task starts by blocking on it rather than
        // finding the bus idle.
        ok = st->io && esp_lcd_panel_io_tx_color(st->io, -1, st->frame, st->frameBytes) == ESP_OK;
        if (ok) {
            st->refillParked.store(false, std::memory_order_release);
            ok = xTaskCreate(&hub75RefillTask, "mmHub75", 4096, st, 6, &st->refill) == pdPASS;
            if (!ok) st->refill = nullptr;
        }
#endif
    }
    if (!ok) {
        st->running.store(false, std::memory_order_release);
        g_lastError = "the panel scan would not start";
    }
    return ok;
}

uint16_t hub75RefreshHz(const Hub75Handle& h) MM_NONBLOCKING {
    auto* st = static_cast<Hub75State*>(h.impl);
    if (!st || !st->running.load(std::memory_order_acquire)) return 0;
    // Averaged over a ~1 s window, recomputed on read. Cheap enough for a 1 Hz status poll, and it
    // keeps the ISR down to one increment.
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t elapsed = now - st->windowStartUs;
    if (elapsed >= 1'000'000u) {
        const uint32_t scans = st->scans.exchange(0, std::memory_order_relaxed);
        st->refreshHz.store(static_cast<uint16_t>(
            (static_cast<uint64_t>(scans) * 1'000'000u) / elapsed),
            std::memory_order_relaxed);
        st->windowStartUs = now;
    }
    return st->refreshHz.load(std::memory_order_relaxed);
}

const char* hub75Backend(const Hub75Handle& h) {
    auto* st = static_cast<Hub75State*>(h.impl);
    return st ? hub75BackendLabel(st->backend) : nullptr;
}

void hub75Deinit(Hub75Handle& h) {
    destroyState(static_cast<Hub75State*>(h.impl));
    h.impl = nullptr;
}

}  // namespace mm::platform

#else   // no LCD_CAM i80 on this chip

namespace mm::platform {

// Inert on silicon without the peripheral (the classic ESP32). The driver reports the cause, the
// same allocate-and-degrade rule every other output seam follows.
const char* hub75LastError() { return "HUB75 needs an ESP32-S3, P4 or S31"; }
bool hub75BackendAvailable(Hub75Backend, size_t) { return false; }
const char* hub75BackendLabel(Hub75Backend backend) {
    return backend == Hub75Backend::Parlio ? "Parlio" : "LCD_CAM";
}
bool hub75Init(Hub75Handle&, Hub75Backend, const Hub75Pins&, uint16_t, uint16_t,
               uint8_t, uint8_t) {
    return false;
}
uint8_t* hub75Buffer(const Hub75Handle&) MM_NONBLOCKING { return nullptr; }
size_t hub75BufferCapacity(const Hub75Handle&) MM_NONBLOCKING { return 0; }
bool hub75Start(Hub75Handle&) { return false; }
uint16_t hub75RefreshHz(const Hub75Handle&) MM_NONBLOCKING { return 0; }
const char* hub75Backend(const Hub75Handle&) { return nullptr; }
void hub75Deinit(Hub75Handle&) {}

}  // namespace mm::platform

#endif  // SOC_LCDCAM_I80_LCD_SUPPORTED
