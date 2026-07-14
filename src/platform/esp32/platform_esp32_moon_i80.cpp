// Parallel WS2812 output over the ESP32-S3/P4 LCD_CAM i80 peripheral, driven by OUR OWN DMA
// sequencing instead of IDF's esp_lcd component — the peripheral half of MoonI80LedDriver
// (src/light/drivers/MoonI80LedDriver.h), which does all the domain work: applies Correction and
// 3-slot-encodes every light into the DMA frame buffer (ParallelSlots.h). This file owns only the
// peripheral — the LCD_CAM registers, the GDMA channel + descriptor chain, the frame buffer(s),
// transmit + wait, and the loopback test's TX side. No domain logic here.
//
// **Why this exists next to platform_esp32_i80.cpp** (the esp_lcd sibling — read that file first;
// it is the behavioural reference this one matches function for function). esp_lcd re-arms the
// peripheral on EVERY transaction: lcd_start_transaction() does lcd_ll_reset() + lcd_ll_fifo_reset()
// + a hard-coded 4 µs busy-wait before each one (esp_lcd_panel_io_i80.c:772-796). An LCD panel does
// not care; WS2812 is one unbroken self-clocked bit stream, so a mid-frame reset garbles everything
// after it. That makes a frame split across several esp_lcd transactions impossible to send
// gaplessly at any chunk size — which forces the whole frame into ONE transaction, and THAT is what
// caps the driver: the DMA must stream the entire frame from one contiguous DMA-reachable block.
//
// The hardware never demanded this. The LCD peripheral has NO data-length register —
// lcd_ll_set_phase_cycles() takes `data_cycles` as a boolean enable, and IDF's own comment reads
// "Number of data phase cycles are controlled by DMA buffer length" (esp_lcd_panel_io_i80.c:778).
// So the peripheral clocks out exactly what the DMA feeds it and stops when the chain ends: ONE
// gdma_start() over an arbitrarily long descriptor chain + ONE lcd_ll_start() is a single gapless
// stream across as many buffers as we like. This backend takes that, built on IDF's HAL + GDMA
// link-list APIs (one level below esp_lcd — not raw registers; IDF's own drivers use these same
// APIs, which is what keeps this a recognisable construct rather than a bespoke register poke).
//
// Both implementations ship: the esp_lcd one is the reference, this one is the measured
// alternative, and selecting between them is a module swap in the UI. See docs/adr/0014.
//
// Gated on SOC_LCDCAM_I80_LCD_SUPPORTED — the NARROW macro, unlike the esp_lcd sibling's broad
// SOC_LCD_I80_SUPPORTED: this backend pokes LCD_CAM registers through hal/lcd_ll.h, which does not
// exist on the classic ESP32 (whose i80 is the I2S peripheral in LCD mode, a different register
// file entirely). Inert stubs otherwise, since the CMake SRCS list is unconditional.

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_LCDCAM_I80_LCD_SUPPORTED

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_clk_tree.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"     // esp_rom_delay_us — the DMA-to-FIFO settle before lcd_ll_start
#include "esp_timer.h"       // esp_timer_get_time — ISR-safe wire-time stamp
#include "driver/gpio.h"
#include "soc/io_mux_reg.h"  // PIN_FUNC_GPIO — the IO-MUX function the matrix routes through
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hal/gdma_channel.h"          // SOC_GDMA_TRIG_PERIPH_LCD0 / _BUS (the GDMA_MAKE_TRIGGER operands)
#include "hal/lcd_hal.h"               // lcd_hal_context_t / lcd_hal_init
#include "hal/lcd_ll.h"                // the LCD_CAM low-level register API
#include "hal/lcd_periph.h"            // soc_lcd_i80_signals — the GPIO-matrix signal indices
#include "esp_private/gdma.h"          // channel alloc / connect / strategy / transfer / start
#include "esp_private/gdma_link.h"     // the descriptor link list (esp_lcd uses exactly this)
#include "esp_private/esp_dma_utils.h" // esp_dma_calculate_node_count
#include "esp_private/gpio.h"          // gpio_func_sel — the IO-MUX select esp_lcd's GPIO setup uses
#include "esp_private/periph_ctrl.h"   // PERIPH_RCC_ACQUIRE_ATOMIC / PERIPH_RCC_ATOMIC
#include "esp_private/esp_clk_tree_common.h"  // esp_clk_tree_enable_src — power the LCD clock source

#include <cstring>
#include <functional>  // the transmit callback passed to the shared frame loopback
#include <new>         // std::nothrow

namespace mm::platform {

// Defined in platform_esp32_rmt.cpp — the plain-GPIO continuity pre-check the
// RMT loopback uses; the wire question is identical here.
namespace detail { bool loopbackJumperOk(uint8_t txGpio, uint8_t rxGpio); }

namespace {

static const char* MOON_I80_TAG = "mm_moon_i80";

// 3 slots per WS2812 bit (the ParallelSlots.h contract): 2.67 MHz pclk = 375 ns
// slots, "0" = 1 slot HIGH, "1" = 2 slots HIGH. 375 ns and not the lineage's
// usual 416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and on a
// direct 3.3 V data line (no level shifter) a longer "0" pulse gets misread
// as "1" — the strip washes out white. 375 ns sits inside every revision's
// window; the 160 MHz LCD clock divides to it exactly (/60).
constexpr uint32_t kPclkHz = 2'666'666;

// Pixel clock with a 74HCT595 expander fitted. **The constraint that matters is the WS2812 SLOT
// DURATION, not the elegance of the divider.** A '595 shifts each slot out over `clockMultiplier`
// bus words, so:
//
//     slot = clockMultiplier / pclk        and the slot IS the "0" pulse (T0H).
//
// WS2812B spec: T0H 200-380 ns (newer revisions cap ~380 — see lessons.md #5, the max-white
// flicker), T1H 580-1000 ns. The direct path picks 2.67 MHz for a 375 ns slot, right at that edge.
// 20 MHz ("because it divides exactly") gives 8 × 50 ns = 400 ns, OVER the T0H max, and the strands
// wash out white. An exact divider that produces an out-of-spec waveform is worthless.
//
// 26.67 MHz (prescale 3 off the 80 MHz bus resolution — still an exact divide):
//     slot = 8 / 26.67 MHz  = 300 ns   T0H 300 (spec 200-380 ✓)  T1H 600 (spec 580-1000 ✓)
//
// **Do NOT "fix" flicker by lowering this clock** — a lower pclk makes the slot LONGER, pushing T0H
// further past 380 ns. If the waveform needs adjusting, adjust it *up*.
constexpr uint32_t kShiftPclkHz = 26'666'666;   // prescale 3 of 80 MHz -> 300 ns WS2812 slots

// The LCD_CAM group clock divider esp_lcd applies (LCD_PERIPH_CLOCK_PRE_SCALE in
// esp_lcd/priv_include/esp_lcd_common.h:28 — a PRIVATE header, so the constant is restated here
// rather than included). It is the minimum divider the peripheral accepts, and with the default
// PLL160M source it makes the bus resolution 160/2 = 80 MHz, off which every pclk above is an exact
// integer prescale. Kept identical to esp_lcd's so both backends produce the same waveform.
constexpr uint32_t kClockPreScale = 2;

// Max bytes one GDMA descriptor carries (LCD_DMA_DESCRIPTOR_BUFFER_MAX_SIZE, same private header).
// A 144 KB frame therefore needs ~37 nodes ≈ 444 B of descriptor memory — the chain is free.
constexpr size_t kDmaNodeMaxBytes = 4095;

// The LCD_CAM i80 bus index. Both the S3 and the P4 have exactly one (LCD_LL_I80_BUS_NUM == 1), and
// the whole point of this backend is that WE own the peripheral for the frame's duration.
constexpr int kBusId = 0;

// Backstop for a transmit that arrives while the previous frame is still on the wire (the async
// double-buffer's normal case — see moonI80Ws2812Transmit). It bounds a WEDGED peripheral, nothing
// more: a healthy frame clears the wire in single-digit milliseconds, and the driver's own
// frame-derived timeout (ParallelLedDriver::waitBudgetMs) is what actually governs a stalled bus.
// This only exists so a broken DMA cannot hang the render thread forever.
constexpr uint32_t kWireFreeTimeoutMs = 200;

// The frame buffer(s), the peripheral, and the DMA chain that streams one into the other.
//
// Two DMA frame buffers for the async deferred-wait double-buffer: the driver encodes frame N+1
// into buf[1-active] while the GDMA clocks frame N out of buf[active]. buf[1] is null when the
// second allocation didn't fit (single-buffer mode). Each buffer has its OWN done-semaphore so a
// wait targets the right transfer.
//
// The GDMA completes transfers in START order and its EOF event carries no per-transfer token, so a
// 2-slot in-order completion FIFO of started buffer indices, popped in the EOF callback, routes each
// done-signal to the buffer that actually finished. (The textbook completion FIFO for an in-order
// DMA queue; depth 2 because at most two transfers — one per buffer — are ever outstanding.)
//
// Unlike esp_lcd there is no transaction queue: the peripheral is ours, so a transmit programs the
// chain and starts the hardware directly. `busy` is therefore the honest state — a second transmit
// while one is in flight is a caller error (the driver waits before reusing a buffer), and is
// rejected rather than silently queued behind a peripheral reset.
struct MoonI80State {
    lcd_hal_context_t hal = {};
    gdma_channel_handle_t dma = nullptr;
    gdma_link_list_handle_t link = nullptr;
    SemaphoreHandle_t done[2] = {nullptr, nullptr};
    // Signals "the wire is free" — given by the EOF ISR, taken by a transmit that arrives while a
    // transfer is still clocking out. SEPARATE from done[]: the driver owns those (it waits on the
    // buffer it wants to reuse), and a transmit must not consume a signal the driver is still going to
    // wait for. One producer (the ISR), one consumer (the transmit), so a binary semaphore is exactly
    // the right primitive.
    SemaphoreHandle_t wireFree = nullptr;
    uint8_t* buf[2] = {nullptr, nullptr};
    size_t cap = 0;             // shared per-buffer capacity (both buffers equal)
    size_t busWidth = 8;        // 8 or 16 data lines
    uint32_t prescale = 1;      // pixel-clock prescale off the 80 MHz bus resolution
    bool clockAcquired = false; // the PERIPH_RCC bus-clock reference this state holds
    // In-order completion FIFO of started buffer indices (0/1). The transmit pushes at head; the
    // EOF ISR pops at tail. Only ever 0..2 entries (one per buffer).
    volatile uint8_t fifo[2] = {0, 0};
    volatile uint8_t fifoHead = 0;   // next write slot (mod 2)
    volatile uint8_t fifoTail = 0;   // next read slot (mod 2)
    // Wire-time KPI: the hardware-start timestamp of each in-flight transfer, and the last measured
    // duration. Paired with the FIFO, so it tracks the transfer the next EOF completes.
    volatile int64_t txStartUs[2] = {0, 0};
    volatile uint32_t lastTransmitUs = 0;
    volatile bool busy = false;      // a transfer is clocking out right now
};

// GDMA transfer-EOF callback: the descriptor chain hit its EOF node — pop the oldest started buffer
// index, record the wire duration, and release THAT buffer's waiter.
//
// IDF notes that the TX EOF fires when the DMA has pushed the last bytes into the LCD FIFO, which
// can be a few pclk cycles BEFORE the last bits leave the pins (gdma_strategy_config_t's
// `eof_till_data_popped` exists precisely to close that gap). That is fine for this contract: a wait
// gates *reusing a buffer*, not *reading the output* — and the DMA is provably finished reading a
// buffer at EOF, which is exactly the question the caller asks. The residual few-hundred-nanosecond
// error in the wire-time KPI is far below its resolution.
//
// IRAM_ATTR: this runs in the GDMA interrupt, so keeping it out of flash is the correct defensive
// choice (matches the esp_lcd and Parlio siblings). Everything it touches is IRAM-safe
// (esp_timer_get_time reads a hardware counter, xSemaphoreGiveFromISR, plain member stores).
bool IRAM_ATTR moonI80EofCb(gdma_channel_handle_t, gdma_event_data_t*, void* user) {
    auto* st = static_cast<MoonI80State*>(user);
    const uint8_t slot = st->fifoTail;
    const uint8_t b = st->fifo[slot] & 1u;
    const int64_t now = esp_timer_get_time();
    st->lastTransmitUs = static_cast<uint32_t>(now - st->txStartUs[slot]);
    st->fifoTail = (st->fifoTail + 1u) & 1u;
    st->busy = false;
    BaseType_t high = pdFALSE;
    xSemaphoreGiveFromISR(st->done[b], &high);
    BaseType_t highWire = pdFALSE;
    xSemaphoreGiveFromISR(st->wireFree, &highWire);   // release a transmit blocked on the wire
    return (high == pdTRUE) || (highWire == pdTRUE);
}

void destroyState(MoonI80State* st) {
    if (!st) return;
    if (st->dma) {
        gdma_stop(st->dma);
        gdma_disconnect(st->dma);
        gdma_del_channel(st->dma);
    }
    if (st->link) gdma_del_link_list(st->link);
    if (st->hal.dev) {
        lcd_ll_stop(st->hal.dev);
        PERIPH_RCC_ATOMIC() {
            lcd_ll_enable_clock(st->hal.dev, false);
        }
    }
    // Release the bus-clock reference taken in createState (the peripheral powers down when the last
    // holder — us or an esp_lcd bus — lets go). Mirrors esp_lcd_del_i80_bus, esp_lcd_panel_io_i80.c:305.
    if (st->clockAcquired) {
        PERIPH_RCC_ACQUIRE_ATOMIC(soc_lcd_i80_signals[kBusId].module, ref_count) {
            if (ref_count == 0) lcd_ll_enable_bus_clock(kBusId, false);
        }
    }
    for (auto* b : st->buf) if (b) heap_caps_free(b);
    for (auto* s : st->done) if (s) vSemaphoreDelete(s);
    if (st->wireFree) vSemaphoreDelete(st->wireFree);
    delete st;
}

// Bring up the LCD_CAM peripheral in i80 mode for a pure data phase. Replicates
// esp_lcd_new_i80_bus (esp_lcd_panel_io_i80.c:135-245) + lcd_i80_select_periph_clock (:642) +
// lcd_i80_switch_devices' per-device register writes (:800-819), minus everything a WS2812 frame
// does not use: no LCD interrupt (the GDMA EOF is our completion), no transaction queue, no format
// buffer, no PM lock, no sleep retention.
bool initPeripheral(MoonI80State* st, uint32_t pclkHz) {
    // Bus resolution = source clock / the group prescale. Ask the clock tree rather than assuming
    // 160 MHz, so a future default-source change can't silently retune the WS2812 waveform.
    uint32_t srcHz = 0;
    if (esp_clk_tree_enable_src(static_cast<soc_module_clk_t>(LCD_CLK_SRC_DEFAULT), true) != ESP_OK)
        return false;
    if (esp_clk_tree_src_get_freq_hz(static_cast<soc_module_clk_t>(LCD_CLK_SRC_DEFAULT),
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &srcHz) != ESP_OK)
        return false;
    const uint32_t resolutionHz = srcHz / kClockPreScale;   // 80 MHz with the PLL160M default
    const uint32_t prescale = resolutionHz / pclkHz;
    // The prescale is an integer register field: an inexact pclk rounds DOWN into a LONGER slot, and
    // a too-long "0" pulse is read as a "1" (the max-white washout). Both supported rates divide 80
    // MHz exactly, so reject anything that doesn't rather than emit a wrong waveform.
    if (prescale == 0 || prescale > LCD_LL_PCLK_DIV_MAX) return false;
    st->prescale = prescale;

    // Power the peripheral's APB/bus clock. Reference-counted: esp_lcd may hold the same peripheral
    // for the sibling driver, so the reset only fires for the first holder.
    PERIPH_RCC_ACQUIRE_ATOMIC(soc_lcd_i80_signals[kBusId].module, ref_count) {
        if (ref_count == 0) {
            lcd_ll_enable_bus_clock(kBusId, true);
            lcd_ll_reset_register(kBusId);
        }
    }
    st->clockAcquired = true;

    lcd_hal_init(&st->hal, kBusId);
    lcd_cam_dev_t* dev = st->hal.dev;

    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_clock(dev, true);
        lcd_ll_select_clk_src(dev, LCD_CLK_SRC_DEFAULT);
        // Integer division only (0/0 for the fractional part) — a fractional group divider adds
        // clock jitter, which on a self-clocked WS2812 stream is bit error.
        lcd_ll_set_group_clock_coeff(dev, static_cast<int>(kClockPreScale), 0, 0);
    }

    lcd_ll_reset(dev);
    lcd_ll_fifo_reset(dev);

    // No LCD interrupt is installed at all: esp_lcd needs one to dispatch its transaction queue, we
    // do not (the GDMA EOF is the only completion event this driver has). Mask the peripheral's
    // interrupts and clear anything the previous owner left pending, so a stale TRANS_DONE can't
    // fire into an ISR that isn't ours.
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_interrupt(dev, LCD_LL_EVENT_I80, false);
    }
    lcd_ll_clear_interrupt_status(dev, UINT32_MAX);

    lcd_ll_enable_rgb_mode(dev, false);        // i80 (command/data) mode, not RGB timing mode
    lcd_ll_enable_color_convert(dev, false);   // no RGB/YUV conversion — the frame is raw slot words
    lcd_ll_set_dma_read_stride(dev, st->busWidth);   // bytes the FIFO pulls per bus word
    lcd_ll_set_data_wire_width(dev, st->busWidth);   // data lines actually driven
    // "output always on" is what makes the data-phase length come from the DMA chain rather than a
    // cycle count — the property this whole backend is built on (esp_lcd_panel_io_i80.c:226).
    lcd_ll_enable_output_always_on(dev, true);
    lcd_ll_set_swizzle_mode(dev, LCD_LL_SWIZZLE_AB2BA);  // mode select only; the swizzle stays OFF below
    lcd_ll_enable_swizzle(dev, false);              // byte order as encoded — ParallelSlots writes bus words directly
    lcd_ll_reverse_dma_data_bit_order(dev, false);  // bit L of the word IS data line L (the encoder's contract)
    lcd_ll_swap_dma_data_byte_order(dev, false);

    lcd_ll_set_pixel_clock_prescale(dev, prescale);
    lcd_ll_set_clock_idle_level(dev, false);   // WR rests LOW, like the data lines (pclk_idle_low)
    lcd_ll_set_pixel_clock_edge(dev, false);   // data latched on the rising edge
    // DC is a peripheral-mandated line the strands ignore; park it LOW in every phase so it cannot
    // toggle a neighbouring strand if a board wires it to one.
    lcd_ll_set_dc_level(dev, /*idle=*/false, /*cmd=*/false, /*dummy=*/false, /*data=*/false);
    return true;
}

// Route the peripheral's signals onto real pins through the GPIO matrix. Replicates
// lcd_i80_bus_configure_gpio (esp_lcd_panel_io_i80.c:722-746).
void configureGpio(const uint16_t* dataPins, uint8_t laneCount, size_t busWidth,
                   uint16_t wrGpio, uint16_t dcGpio) {
    // Every data line up to busWidth must be driven: the peripheral clocks all of them regardless,
    // so a board with fewer real lanes parks the remainder on the WR "ghost pin" (hpwit's trick) —
    // WR toggles on it harmlessly, and the domain driver clears those lanes' activeMask so they idle.
    for (size_t i = 0; i < busWidth; i++) {
        const uint16_t pin = (i < laneCount) ? dataPins[i] : wrGpio;
        gpio_func_sel(static_cast<gpio_num_t>(pin), PIN_FUNC_GPIO);
        esp_rom_gpio_connect_out_signal(pin, soc_lcd_i80_signals[kBusId].data_sigs[i], false, false);
    }
    gpio_func_sel(static_cast<gpio_num_t>(dcGpio), PIN_FUNC_GPIO);
    esp_rom_gpio_connect_out_signal(dcGpio, soc_lcd_i80_signals[kBusId].dc_sig, false, false);
    gpio_func_sel(static_cast<gpio_num_t>(wrGpio), PIN_FUNC_GPIO);
    esp_rom_gpio_connect_out_signal(wrGpio, soc_lcd_i80_signals[kBusId].wr_sig, false, false);
}

// GDMA channel + descriptor chain. Replicates lcd_i80_init_dma_link (esp_lcd_panel_io_i80.c:670-712)
// with two deliberate differences, both central to this backend:
//
//  1. **check_owner = false.** esp_lcd leaves owner-checking ON, which makes gdma_link_mount_buffers
//     walk from index 0 and refuse at the first descriptor the DMA still owns — the `lli full
//     need=N avail=M` failure that makes the esp_lcd path unusable with a large ('595-expanded)
//     frame. We own the chain outright and rebuild it from scratch on every transmit, so the check
//     protects nothing and only fails. (hpwit's S3 driver disables it for the same reason.)
//  2. The completion callback is the GDMA's own on_trans_eof, not an LCD interrupt.
bool initDma(MoonI80State* st, size_t bufferBytes) {
    gdma_channel_alloc_config_t chanCfg = {};
    // The S3's LCD hangs off the AHB GDMA, the P4's off the AXI one, and the descriptor alignment
    // differs with the bus. Same selection esp_lcd makes (esp_lcd_panel_io_i80.c:19-27), including
    // its `defined(...) &&` guard — on a chip with no AXI GDMA the AXI symbol is simply absent, and
    // an unguarded `== SOC_GDMA_BUS_AXI` would compare against the preprocessor's 0 and match.
#if defined(SOC_GDMA_BUS_AXI) && (SOC_GDMA_TRIG_PERIPH_LCD0_BUS == SOC_GDMA_BUS_AXI)
    constexpr size_t kDescAlign = 8;
    if (gdma_new_axi_channel(&chanCfg, &st->dma, nullptr) != ESP_OK) return false;
#else
    constexpr size_t kDescAlign = 4;
    if (gdma_new_ahb_channel(&chanCfg, &st->dma, nullptr) != ESP_OK) return false;
#endif
    if (gdma_connect(st->dma, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0)) != ESP_OK) return false;

    gdma_strategy_config_t strategy = {};
    strategy.auto_update_desc = true;
    strategy.owner_check = false;   // see (1) above — we own the chain
    if (gdma_apply_strategy(st->dma, &strategy) != ESP_OK) return false;

    gdma_transfer_config_t transfer = {};
    transfer.max_data_burst_size = 64;   // the burst esp_lcd's callers ask for; keeps PSRAM reads efficient
    transfer.access_ext_mem = true;      // the frame usually lives in PSRAM (LCD_CAM GDMA reaches it)
    if (gdma_config_transfer(st->dma, &transfer) != ESP_OK) return false;

    size_t intAlign = 0, extAlign = 0;
    if (gdma_get_alignment_constraints(st->dma, &intAlign, &extAlign) != ESP_OK) return false;
    const size_t bufAlign = intAlign > extAlign ? intAlign : extAlign;

    gdma_link_list_config_t linkCfg = {};
    linkCfg.item_alignment = kDescAlign;
    linkCfg.num_items = esp_dma_calculate_node_count(bufferBytes, bufAlign, kDmaNodeMaxBytes);
    linkCfg.flags.check_owner = false;   // see (1)
    if (gdma_new_link_list(&linkCfg, &st->link) != ESP_OK) return false;

    gdma_tx_event_callbacks_t cbs = {};
    cbs.on_trans_eof = moonI80EofCb;
    return gdma_register_tx_event_callbacks(st->dma, &cbs, st) == ESP_OK;
}

// Allocate one DMA-capable frame buffer, honouring the GDMA's alignment constraints (both the
// address and the length must be a cache-line multiple on the P4 / on PSRAM, or the cache
// write-back before a transfer would touch neighbouring allocations).
uint8_t* allocFrame(MoonI80State* st, size_t bufferBytes, bool psram) {
    size_t intAlign = 0, extAlign = 0;
    gdma_get_alignment_constraints(st->dma, &intAlign, &extAlign);
    const size_t align = psram ? extAlign : intAlign;
    const uint32_t caps = MALLOC_CAP_8BIT | MALLOC_CAP_DMA
                        | (psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    // Round the size up to the alignment too — gdma_link_mount_buffers checks the LENGTH against the
    // same constraint, and a short tail would fail the mount. The pad is zeroed, so it just extends
    // the frame's trailing latch gap (the lines already idle LOW there).
    const size_t size = align > 1 ? ((bufferBytes + align - 1) / align) * align : bufferBytes;
    return static_cast<uint8_t*>(heap_caps_aligned_calloc(align ? align : 4, 1, size, caps));
}

// One peripheral + GDMA chain + frame buffer(s). `wantSecond` allocates the async double-buffer's
// second frame buffer (best-effort — null if it won't fit); false allocates buffer 0 only. Shared by
// the runtime init and the loopback (which passes false — one transfer).
MoonI80State* createState(const uint16_t* dataPins, uint8_t laneCount,
                          uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes, bool wantSecond,
                          uint8_t clockMultiplier) {
    auto* st = new (std::nothrow) MoonI80State();
    if (!st) return nullptr;

    // Bus width is power-of-two only (8 or 16), derived from the lane count: ≤8 → 8, 9..16 → 16.
    st->busWidth = laneCount <= 8 ? 8 : 16;

    // A '595 expander shifts each WS2812 slot out over `clockMultiplier` bus words, so the bus must
    // clock proportionally faster to keep the slot inside the WS2812 bit window. See kShiftPclkHz.
    const uint32_t pclkHz = (clockMultiplier > 1) ? kShiftPclkHz : kPclkHz;
    if (!initPeripheral(st, pclkHz) || !initDma(st, bufferBytes)) {
        destroyState(st);
        return nullptr;
    }
    configureGpio(dataPins, laneCount, st->busWidth, wrGpio, dcGpio);

    st->done[0] = xSemaphoreCreateBinary();
    st->wireFree = xSemaphoreCreateBinary();
    if (!st->done[0] || !st->wireFree) {
        destroyState(st);
        return nullptr;
    }

    // PSRAM first: the LCD_CAM GDMA bursts straight out of PSRAM (access_ext_mem), the 2.67 MHz
    // pixel clock is easy to sustain from it, and keeping a large frame out of the scarce internal
    // DMA heap is the right trade. Internal is the fallback when PSRAM is absent or full.
    //
    // **Where the frame lives is decided by the PIXEL CLOCK, not by its size — measured on this
    // backend, board B, 2026-07-14, and it is the cleanest result of the whole investigation.**
    //
    // Same board, same PSRAM, same descriptor chain, same driver; the ONLY variable is the clock:
    //
    //   direct mode  (2.67 MHz pclk):  a PSRAM frame streams fine   — 2048 lights, 7,712 µs, driving
    //   shift mode   (26.67 MHz pclk): a PSRAM frame NEVER completes — at ANY size, 54 KB or 144 KB
    //
    // So it is not PSRAM, and it is not the frame size: **the S3's GDMA cannot sustain a PSRAM read at
    // the expander's clock.** A '595 is serial-in, so each WS2812 slot is shifted out over 8 bus words
    // and the bus must run 10× faster — which is exactly the rate PSRAM cannot feed.
    //
    // **This backend is what proved it, which is exactly why it was built.** The esp_lcd path failed
    // here with thousands of `lli full` descriptor-mount errors, and those pointed hard at esp_lcd's
    // own descriptor handling. This backend removes that mechanism entirely — we own the chain, mount
    // it once, owner-checking off — and the mount errors are GONE, yet the transfer still never
    // completes. So the `lli full` storm was a symptom, not the cause: the hypothesis is dead, killed
    // by a controlled experiment with a working control condition (direct mode, same PSRAM, drives
    // fine). That is the measurement ADR-0014 phase 1 exists to produce.
    //
    // Hence internal RAM first in shift mode, and PSRAM first otherwise. This is not a workaround
    // inherited from the sibling; it is what the measurement says. It caps the expander at what fits
    // internal DMA RAM (~110 KB → ~96 lights/strand), and lifting that cap is **phase 2, which this
    // backend is the foundation for**: close the chain into a ring (GDMA_FINAL_LINK_TO_HEAD) over
    // small INTERNAL buffers, and refill them from the PSRAM frame in our own EOF callback — a bulk
    // sequential CPU read, so the DMA never reads PSRAM at the expander's clock at all. Every piece of
    // that (our own link list, our own EOF hook, one continuous lcd_ll_start that is never re-armed)
    // exists only because we own the DMA; esp_lcd can express none of it. The ring is an extension of
    // this machinery, not a rewrite — and it is now justified by measurement rather than assumption.
    //
    // PSRAM remains the fallback in shift mode: a frame too big for internal RAM still drives (badly)
    // rather than refusing to start, and the driver's dead-frame guard keeps a stalled bus from
    // starving the device.
    const bool shiftMode = clockMultiplier > 1;
    st->buf[0] = allocFrame(st, bufferBytes, /*psram=*/!shiftMode);
    if (!st->buf[0]) st->buf[0] = allocFrame(st, bufferBytes, /*psram=*/shiftMode);
    if (!st->buf[0]) {
        destroyState(st);
        return nullptr;
    }
    st->cap = bufferBytes;

    // Second buffer for the async double-buffer — ONLY when asked. Allocate-and-degrade (ADR 0002):
    // if it fits, arm double-buffer mode (buf[1] + its semaphore); if it doesn't, leave buf[1] null
    // and the driver runs single-buffer. The internal fallback additionally must leave HEAP_RESERVE
    // intact — the second buffer is a nice-to-have and must never eat the WiFi/HTTP reserve.
    //
    // **The reserve guards the INTERNAL attempt, whichever attempt that is.** The preference order
    // flips with the mode (shift wants internal first, direct wants PSRAM first), so binding the guard
    // to a fixed branch would put it on the wrong one half the time — in shift mode it would leave the
    // internal allocation unguarded (free to eat the WiFi/HTTP reserve) while pointlessly gating the
    // PSRAM retry on internal free space. A small lambda keeps the rule with the thing it guards.
    if (wantSecond) {
        st->done[1] = xSemaphoreCreateBinary();
        if (st->done[1]) {
            auto tryAlloc = [&](bool psram) -> uint8_t* {
                if (!psram
                    && heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
                           < bufferBytes + HEAP_RESERVE) {
                    return nullptr;   // internal, and it would eat the reserve — refuse
                }
                return allocFrame(st, bufferBytes, psram);
            };
            st->buf[1] = tryAlloc(/*psram=*/!shiftMode);
            if (!st->buf[1]) st->buf[1] = tryAlloc(/*psram=*/shiftMode);
            if (!st->buf[1]) {
                vSemaphoreDelete(st->done[1]);
                st->done[1] = nullptr;
            }
        }
    }
    return st;
}

// Program the chain and start the ONE gapless transaction. This is the whole point of the backend:
// esp_lcd's per-transaction reset happens exactly once here, at the START of the single transfer
// that carries the entire frame, never between chunks of it.
bool startTransfer(MoonI80State* st, uint8_t buffer, size_t bytes) {
    lcd_cam_dev_t* dev = st->hal.dev;

    // Write the encoded frame back from cache to physical memory — the GDMA reads DRAM/PSRAM, not
    // the CPU's cache. (esp_lcd does this inside tx_color, esp_lcd_panel_io_i80.c:576.)
    if (esp_cache_get_line_size_by_addr(st->buf[buffer]) > 0) {
        esp_cache_msync(st->buf[buffer], bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    // Mount the WHOLE frame in ONE call: the link list splits it across as many 4095-byte nodes as
    // it needs and chains them, so the DMA walks the frame end to end without CPU involvement.
    // mark_eof fires our completion callback on the last node; mark_final NULLs its next-pointer so
    // the DMA stops there instead of wrapping to the head.
    gdma_buffer_mount_config_t mount = {};
    mount.buffer = st->buf[buffer];
    mount.length = bytes;
    mount.flags.mark_eof = true;
    mount.flags.mark_final = GDMA_FINAL_LINK_TO_NULL;
    if (gdma_link_mount_buffers(st->link, 0, &mount, 1, nullptr) != ESP_OK) return false;

    // Data phase only: no command cycles, no dummy cycles, `data_cycles = 1` is a boolean ENABLE —
    // the peripheral clocks whatever the DMA chain feeds it and stops when the chain ends
    // ("Number of data phase cycles are controlled by DMA buffer length", esp_lcd_panel_io_i80.c:778).
    lcd_ll_set_phase_cycles(dev, /*cmd=*/0, /*dummy=*/0, /*data=*/1);
    lcd_ll_set_blank_cycles(dev, 1, 1);
    lcd_ll_reset(dev);
    lcd_ll_fifo_reset(dev);   // discard any FIFO residue from the previous frame

    // GDMA first: the LCD only starts consuming once data has reached its FIFO. The 1 µs settle is
    // esp_lcd's 4 µs (esp_lcd_panel_io_i80.c:793) shortened — at these pixel clocks (2.67-26.7 MHz,
    // vs the tens of MHz an LCD panel runs) the FIFO fills far faster than one word period, so 1 µs
    // is ample and keeps the inter-frame gap short. Skipping it entirely would risk the first word
    // clocking out of an empty FIFO.
    if (gdma_start(st->dma, gdma_link_get_head_addr(st->link)) != ESP_OK) return false;
    esp_rom_delay_us(1);
    lcd_ll_start(dev);
    return true;
}

} // namespace

bool moonI80Ws2812Init(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                       uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes,
                       bool wantSecondBuffer, uint8_t clockMultiplier) {
    if (!dataPins || laneCount == 0 || bufferBytes == 0 || clockMultiplier == 0) return false;
    // Pre-check that the frame can land SOMEWHERE before touching the peripheral. createState
    // allocates PSRAM-first (direct mode) or internal-first (shift mode) and falls back to the other,
    // so init is fine when EITHER region fits. The HEAP_RESERVE floor guards only INTERNAL RAM (the
    // WiFi/HTTP reserve); a PSRAM buffer doesn't touch it. Degrade (return false → the driver idles
    // with a status) when neither region fits.
    //
    // The PSRAM capacity query uses MALLOC_CAP_SPIRAM ALONE: no registered heap is tagged BOTH
    // SPIRAM and DMA, so the combined query returns 0 even on an S3 whose GDMA reaches PSRAM
    // perfectly well, and gating on it would silently cap the driver at the internal heap. (The
    // *alloc* does pass both caps, which is correct — that's what IDF itself does.)
    const bool fitsInternal =
        heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) >= bufferBytes + HEAP_RESERVE;
    const bool fitsPsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= bufferBytes;
    if (!fitsInternal && !fitsPsram) return false;
    MoonI80State* st = createState(dataPins, laneCount, wrGpio, dcGpio, bufferBytes,
                                   wantSecondBuffer, clockMultiplier);
    if (!st) return false;
    h.impl = st;
    return true;
}

uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle& h, uint8_t buffer) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    return (st && buffer < 2) ? st->buf[buffer] : nullptr;
}

size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    return st ? st->cap : 0;
}

bool moonI80Ws2812Transmit(MoonI80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || buffer >= 2 || !st->buf[buffer] || bytes == 0 || bytes > st->cap) return false;
    // **The peripheral holds exactly one transfer, and the caller is allowed to hand us the next one
    // while it is still clocking out — so wait for the wire here rather than refusing.**
    //
    // Unlike esp_lcd there is no transaction queue to absorb a second transfer, and reprogramming the
    // link list while the DMA is mid-walk would garble the frame. But refusing a busy bus would be
    // wrong: the async double-buffer's whole design is that the driver waits only on the buffer it is
    // about to ENCODE into (`tickAsync` → `busWaitIfBusy(active_)`), then transmits it — at which
    // point the OTHER buffer's transfer is quite legitimately still on the wire. Refusing there would
    // drop every second frame and eventually trip the driver's dead-frame guard.
    //
    // Waiting here costs nothing the double-buffer was buying: its win is that the ENCODE of frame
    // N+1 overlapped the wire time of frame N, and that has already happened by the time we are
    // called. What is left is the wire itself, which is serial on any design — the strand can only
    // receive one frame at a time.
    if (st->busy) {
        // Block on the dedicated wire-free signal, not on the in-flight buffer's done[] — that one
        // belongs to the DRIVER (it waits on the buffer it means to reuse), and consuming it here
        // would make that wait miss. The EOF gives both.
        //
        // The bound is a backstop against a wedged peripheral, not a policy: the driver's own
        // frame-derived timeout (ParallelLedDriver::waitBudgetMs) is what actually governs a stalled
        // bus. This exists only so a broken DMA cannot hang the render thread indefinitely.
        if (xSemaphoreTake(st->wireFree, pdMS_TO_TICKS(kWireFreeTimeoutMs)) != pdTRUE) return false;
    }

    // Push the buffer onto the completion FIFO BEFORE starting the hardware, so a fast EOF (which
    // pops it) can never fire before its slot is populated. The push touches slot `fifoHead`; the ISR
    // only ever reads slot `fifoTail`, and with one transfer in flight those are different slots —
    // that disjointness is what makes the push safe without a lock.
    const uint8_t slot = st->fifoHead;
    st->fifo[slot] = buffer;
    // The transfer starts on the wire immediately (we waited above for any predecessor), so stamp the
    // wire-time KPI right here — no deferred stamping is needed, which is a small simplification
    // over the esp_lcd sibling's queued path.
    st->txStartUs[slot] = esp_timer_get_time();
    st->fifoHead = (st->fifoHead + 1u) & 1u;
    st->busy = true;

    if (!startTransfer(st, buffer, bytes)) {
        // Failed to arm — unwind the FIFO push so the ISR count stays balanced. Safe: no transfer
        // started, so no EOF will pop this slot.
        st->fifoHead = (st->fifoHead + 1u) & 1u;
        st->busy = false;
        return false;
    }
    return true;
}

bool moonI80Ws2812Wait(MoonI80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st || buffer >= 2 || !st->done[buffer]) return true;   // nothing to wait on = not in flight
    // Report whether the transfer actually completed. On a timeout the DMA may still be reading this
    // buffer, so the caller must keep it marked in-flight rather than re-encoding into it — handing a
    // live DMA a half-rewritten buffer is exactly the frame corruption the timeout is meant to avoid.
    return xSemaphoreTake(st->done[buffer], pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    return st ? st->lastTransmitUs : 0;
}

void moonI80Ws2812Deinit(MoonI80Ws2812Handle& h) {
    auto* st = static_cast<MoonI80State*>(h.impl);
    if (!st) return;
    destroyState(st);
    h.impl = nullptr;
}

// ---------------------------------------------------------------------------
// Loopback self-test: a private full-width peripheral setup on the driver's real pins transmits the
// CALLER'S real frame — full size, real descriptor chain, real latch pad — exactly like the render
// loop, while an RMT RX channel captures the whole frame off the jumpered rxGpio and verifies every
// bit. A short synthetic burst would miss exactly the failures a real frame hits (descriptor
// boundaries, sustained-rate stalls), so the test sends the genuine article.
//
// The capture + bit-verify half is shared with the esp_lcd and Parlio loopbacks in
// detail::captureAndVerifyFrame (platform_esp32_rmt.cpp); only the transmit differs.
// ---------------------------------------------------------------------------

namespace detail {
void captureAndVerifyFrame(uint16_t rxGpio, size_t frameBytes, size_t dataBytes,
                           uint8_t rowBits, uint32_t pclkHz, const char* tag,
                           const std::function<void()>& transmitOnce,
                           RmtLoopbackResult& r);
}

RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                        uint16_t wrGpio, uint16_t dcGpio, uint16_t rxGpio,
                                        const uint8_t* frame, size_t frameBytes,
                                        size_t dataBytes, uint8_t rowBits,
                                        uint8_t clockMultiplier) {
    RmtLoopbackResult r;
    r.sent[0] = 0xA5; r.sent[1] = 0x00; r.sent[2] = 0xFF;  // pattern in every row
    if (!dataPins || laneCount == 0 || !frame || frameBytes == 0
        || dataBytes < 3 || dataBytes > frameBytes || rowBits < 8
        || clockMultiplier == 0) return r;
    const uint16_t txGpio = dataPins[0];   // lane 0 carries the pattern
    const bool shiftMode = clockMultiplier > 1;

    if (shiftMode) {
        // SKIP the continuity pre-check. It drives txGpio and expects rxGpio to follow directly,
        // which is true of a bare jumper but FALSE through a 74HCT595: raising the serial input does
        // not raise an output (that takes 8 shift clocks + a latch). Running it here would report
        // "jumper not detected" on perfectly good wiring. The rx pin is fed from a '595 OUTPUT, so
        // the only proof the wire is right is the bit-verify itself — which is the stronger check
        // anyway (it validates the whole chain: encode → bus → shift → latch → output).
        r.jumperDetected = true;
    } else {
        r.jumperDetected = detail::loopbackJumperOk(static_cast<uint8_t>(txGpio),
                                                    static_cast<uint8_t>(rxGpio));
        if (!r.jumperDetected) return r;
    }

    // The continuity check above reset txGpio's GPIO matrix route; createState re-claims it.
    MoonI80State* st = createState(dataPins, laneCount, wrGpio, dcGpio, frameBytes,
                                   /*wantSecond=*/false,    // one transfer — single buffer
                                   clockMultiplier);        // shift mode → the kShiftPclkHz bus clock
    if (!st) {
        ESP_LOGE(MOON_I80_TAG, "loopback: private peripheral setup failed");
        return r;
    }
    std::memcpy(st->buf[0], frame, frameBytes);   // loopback uses buffer 0 only (single transfer)

    // Ship one frame from buffer 0 and wait for its EOF. Everything else (capture, cadence,
    // bit-verify) is the shared helper. This is the runtime path's bookkeeping, minus the handle
    // indirection: surface a failed arm or an EOF timeout instead of letting either show up only as
    // a later capture mismatch (same handling as the esp_lcd and Parlio siblings).
    auto transmitOnce = [st, frameBytes]() {
        st->fifo[st->fifoHead] = 0;
        st->txStartUs[st->fifoHead] = esp_timer_get_time();
        st->fifoHead = (st->fifoHead + 1u) & 1u;
        st->busy = true;
        if (!startTransfer(st, 0, frameBytes)) {
            st->fifoHead = (st->fifoHead + 1u) & 1u;   // unwind the push
            st->busy = false;
            ESP_LOGE(MOON_I80_TAG, "loopback: tx arm failed");
            return;
        }
        if (xSemaphoreTake(st->done[0], pdMS_TO_TICKS(1000)) != pdTRUE)
            ESP_LOGE(MOON_I80_TAG, "loopback: tx EOF timed out");
    };
    // `pclkHz` tells the verifier the WS2812 SLOT RATE the strand sees — it derives both the
    // pulse-width threshold ("0" = one slot, "1" = two) and the expected transmit duration from it,
    // so it must describe the STRAND's waveform, not the bus. In shift mode the strand's slot is NOT
    // the bus period: `clockMultiplier` bus words fill one slot, so slot rate = pclk / multiplier
    // (26.67 MHz ÷ 8 = 3.33 MHz → a 300 ns slot). Passing the bus rate here makes the capture expect
    // the wrong pulse width and size the window for a frame 8× too short — a decode that matches
    // nothing on a strand whose LEDs are visibly lighting.
    const uint32_t slotHz = shiftMode ? (kShiftPclkHz / clockMultiplier) : kPclkHz;
    detail::captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, rowBits, slotHz,
                                  MOON_I80_TAG, transmitOnce, r);
    destroyState(st);
    return r;
}

} // namespace mm::platform

#else  // !SOC_LCDCAM_I80_LCD_SUPPORTED — inert stubs so a chip without LCD_CAM links

namespace mm::platform {

bool moonI80Ws2812Init(MoonI80Ws2812Handle&, const uint16_t*, uint8_t, uint16_t, uint16_t,
                       size_t, bool, uint8_t) {
    return false;
}
uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle&, uint8_t) { return nullptr; }
size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle&) { return 0; }
bool moonI80Ws2812Transmit(MoonI80Ws2812Handle&, uint8_t, size_t) { return false; }
bool moonI80Ws2812Wait(MoonI80Ws2812Handle&, uint8_t, uint32_t) { return true; }
uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle&) { return 0; }
void moonI80Ws2812Deinit(MoonI80Ws2812Handle&) {}
RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t*, uint8_t, uint16_t, uint16_t,
                                        uint16_t, const uint8_t*, size_t, size_t, uint8_t,
                                        uint8_t) {
    return {};
}

} // namespace mm::platform

#endif  // SOC_LCDCAM_I80_LCD_SUPPORTED
