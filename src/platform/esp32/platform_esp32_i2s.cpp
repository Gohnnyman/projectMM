// I2S microphone input + FFT kernel — the peripheral half of the audio feature
// (AudioService, src/core/AudioService.h). The module does all the domain work: the
// level math (AudioLevel.h), windowing and the magnitude->band mapping
// (AudioBands.h). This file owns only the two seams: reading samples off an I2S
// RX channel, and the FFT itself (esp-dsp's float radix-2).
//
// The microphone is a digital I2S MEMS part (e.g. INMP441): standard/Philips
// framing, 24-bit data left-justified in a 32-bit slot, mono — the channel reads
// the one slot the part's L/R pin selects. It is self-clocked from the bit clock
// (no master clock pin). We configure a 32-bit slot and hand the raw int32
// samples to the domain code, which shifts the 24-bit value down itself.
//
// Compiles on every ESP32 chip: everything is under SOC_I2S_SUPPORTED with inert
// stubs otherwise; the module never calls in (platform::hasI2sMic == false).

#include "platform/platform.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_I2S_SUPPORTED

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"   // heap_caps_malloc — the FFT scratch, internal RAM only
#include "esp_log.h"
#include "dsps_fft2r.h"

#include <cmath>
#include <cstring>
#include <new>      // std::nothrow

namespace mm::platform {

namespace {

const char* I2S_TAG = "mm_i2s";

struct MicState {
    i2s_chan_handle_t rx = nullptr;
};

// esp-dsp's float FFT works in place on an interleaved complex array (re, im,
// re, im, …). One scratch sized to the largest block we'll see; the twiddle
// tables are initialised once, lazily, on first use.
//
// ALLOCATED on first FFT, not held in .bss. As a plain array this was 8 KB of
// INTERNAL RAM reserved from boot on every board — 2.5% of the ~320 KB pool that
// WiFi, the HTTP stack and every task stack share — including the many boards
// with no microphone fitted, where nothing ever reads it. A module that is not
// used should cost nothing (surfaced by check_footprint's STATIC column, where
// this file read 561 B of code against 8193 B of static). Now a board without
// audio pays 4 bytes for the pointer, and one with audio allocates on its first
// analysed frame and keeps it for the process — no per-frame allocation on the
// audio path, which runs at block rate.
constexpr size_t kMaxFftN = 1024;
float* g_fftBuf = nullptr;
bool   g_fftReady = false;

bool ensureFftInit() {
    if (g_fftReady) return true;
    // dsps_fft2r_init_fc32(NULL, …) uses the library's built-in max-size twiddle
    // table — no caller allocation, initialised once for the process.
    if (dsps_fft2r_init_fc32(nullptr, CONFIG_DSP_MAX_FFT_SIZE) != ESP_OK) {
        ESP_LOGE(I2S_TAG, "esp-dsp FFT init failed");
        return false;
    }
    // Internal RAM, not PSRAM: esp-dsp's assembly kernels run per audio block and
    // a PSRAM scratch would put a cache miss in the middle of every butterfly.
    g_fftBuf = static_cast<float*>(
        heap_caps_malloc(kMaxFftN * 2 * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!g_fftBuf) {
        ESP_LOGE(I2S_TAG, "FFT scratch alloc failed (%u B internal)",
                 static_cast<unsigned>(kMaxFftN * 2 * sizeof(float)));
        return false;   // audioFft zero-fills its output, so the caller degrades to silence
    }
    g_fftReady = true;
    return true;
}

}  // namespace

bool audioMicInit(AudioMicHandle& h, uint16_t wsPin, uint16_t sdPin,
                  uint16_t sckPin, int16_t mclkPin, uint32_t sampleRate) {
    auto* st = new (std::nothrow) MicState();
    if (!st) return false;

    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chanCfg, nullptr, &st->rx) != ESP_OK) {
        delete st;
        return false;
    }

    // Standard (Philips) mode, 32-bit slot / 24-bit data, mono. The INMP441 puts
    // its data in ONE slot, chosen by its L/R pin (GND=left, VDD/floating=right),
    // and leaves the other empty. The bench mic is wired L/R-to-GND, so we read the
    // LEFT slot — the populated one for that wiring. If a mic reads silence with
    // sound present it's filling the other slot; flip this to I2S_STD_SLOT_RIGHT.
    i2s_std_slot_config_t slotCfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
    slotCfg.slot_mask = I2S_STD_SLOT_LEFT;
    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
        .slot_cfg = slotCfg,
        .gpio_cfg = {
            // MCLK: unused for a self-clocked MEMS mic (INMP441); driven on the
            // given pin for a codec that needs a master clock (the ES8311). −1 = none.
            .mclk = mclkPin < 0 ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(mclkPin),
            .bclk = static_cast<gpio_num_t>(sckPin),
            .ws   = static_cast<gpio_num_t>(wsPin),
            .dout = I2S_GPIO_UNUSED,         // input only
            .din  = static_cast<gpio_num_t>(sdPin),
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(st->rx, &stdCfg) != ESP_OK
        || i2s_channel_enable(st->rx) != ESP_OK) {
        if (st->rx) i2s_del_channel(st->rx);
        delete st;
        return false;
    }

    h.impl = st;
    return true;
}

size_t audioMicRead(AudioMicHandle& h, int32_t* out, size_t maxSamples) {
    auto* st = static_cast<MicState*>(h.impl);
    if (!st || !st->rx || !out || maxSamples == 0) return 0;
    size_t bytesRead = 0;
    // Non-blocking (0 ms): this runs in the render tick, so it must NOT wait for
    // the DMA to fill (the hot-path no-blocking rule). It drains whatever the DMA
    // already holds and returns immediately. A full 512-sample block takes ~23 ms
    // at 22 kHz — longer than one render tick — so a single read returns only a
    // partial block; AudioService accumulates partials across ticks and runs the FFT
    // when it has a whole block. On timeout the call still copies whatever was
    // ready into `out` and reports it in `bytesRead`, so we use that count
    // regardless of the return code (a timeout with bytesRead>0 is a partial read,
    // not a failure).
    i2s_channel_read(st->rx, out, maxSamples * sizeof(int32_t), &bytesRead,
                     0 /* ms — non-blocking */);
    return bytesRead / sizeof(int32_t);
}

void audioMicDeinit(AudioMicHandle& h) {
    auto* st = static_cast<MicState*>(h.impl);
    if (!st) return;
    if (st->rx) {
        i2s_channel_disable(st->rx);
        i2s_del_channel(st->rx);
    }
    delete st;
    h.impl = nullptr;
}

void audioFft(const float* windowed, size_t n, float* outMag) {
    if (!windowed || !outMag || n == 0 || n > kMaxFftN) return;
    if (!ensureFftInit()) {
        for (size_t k = 0; k < n / 2; k++) outMag[k] = 0.0f;
        return;
    }
    // Pack the real input into the interleaved complex scratch (imag = 0).
    for (size_t i = 0; i < n; i++) {
        g_fftBuf[2 * i] = windowed[i];
        g_fftBuf[2 * i + 1] = 0.0f;
    }
    dsps_fft2r_fc32(g_fftBuf, static_cast<int>(n));
    dsps_bit_rev_fc32(g_fftBuf, static_cast<int>(n));
    // Magnitudes of the first n/2 bins: sqrt(re^2 + im^2).
    for (size_t k = 0; k < n / 2; k++) {
        const float re = g_fftBuf[2 * k];
        const float im = g_fftBuf[2 * k + 1];
        outMag[k] = std::sqrt(re * re + im * im);
    }
}

}  // namespace mm::platform

#else  // !SOC_I2S_SUPPORTED — inert stubs so any I2S-less target links

namespace mm::platform {

bool audioMicInit(AudioMicHandle&, uint16_t, uint16_t, uint16_t, int16_t, uint32_t) {
    return false;
}
size_t audioMicRead(AudioMicHandle&, int32_t*, size_t) { return 0; }
void audioMicDeinit(AudioMicHandle&) {}
void audioFft(const float*, size_t, float*) {}


// OS capture devices are a desktop concept (hasAudioCapture == false here); the stubs exist
// because the discarded if-constexpr branches in shared code still compile them.
size_t audioCaptureDevices(const char* const** optionsOut) {
    if (optionsOut) *optionsOut = nullptr;
    return 0;
}
bool audioCaptureInit(AudioMicHandle& /*h*/, uint8_t /*deviceIndex*/, uint32_t /*sampleRate*/) {
    return false;
}

}  // namespace mm::platform

#endif  // SOC_I2S_SUPPORTED
