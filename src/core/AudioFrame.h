#pragma once

#include <cstdint>

namespace mm {

// One snapshot of analysed audio, produced by AudioService (src/core/AudioService.h)
// once per render tick and consumed by audio-reactive effects (AudioVolumeEffect,
// AudioSpectrumEffect). The producer/consumer-via-plain-struct model the codebase
// already uses (PreviewDriver writes pixels HttpServer reads); the struct is the
// whole contract between the two, so effects never touch I2S or the FFT.
//
// POD by design: a flat value type the producer fills and the consumer reads, no
// ownership, no methods — copy it or hold a `const AudioFrame*` to the module's
// latest. All fields are pre-scaled to small integers so an effect does integer
// math straight off them (the hot-path rule); the float FFT magnitudes never
// leave the module.
struct AudioFrame {
    uint16_t level = 0;         // RAW overall sound level (RMS), 0..255-ish — the instantaneous VU
                                // value, recomputed each audio block with NO smoothing. Snaps to a
                                // transient (a drum hit spikes immediately) — use this for punchy,
                                // beat-reactive effects. (WLED calls this `volumeRaw`.)
    uint16_t levelSmoothed = 0; // SMOOTHED level: an exponential moving average of `level`, so it
                                // lags and rounds off sudden changes and "breathes" with the music
                                // instead of twitching. Use this for calm/glowing effects that
                                // should swell, not flash. (WLED calls this `volume`/`volumeSmth`.)
    uint16_t peakHz = 0;        // dominant frequency this frame, in Hz (0 = none)
    uint16_t peakMag = 0;       // magnitude of that peak (gates the peakHz update), 0..255.
                                // NOT WLED's scale, and the one audio-sync field that is not
                                // interoperable: WLED sends a raw FFT magnitude reaching ~4096
                                // (its own comment: "Want end result to be scaled linear and
                                // ~4096 max"), and its effects divide by 4 or 16 before use, so
                                // their thresholds sit around 48..144 AFTER a /16. A 0..255 value
                                // arrives below the squelch floor and reads as near-silence.
                                // See services.md (Audio) for the trade and the open decision.
    uint8_t  bands[16] = {};    // 16 log-spaced frequency-band magnitudes, 0..255
                                // (bass = bands[0], treble = bands[15])
};

} // namespace mm
