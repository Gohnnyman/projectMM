#pragma once

#include "core/MoonModule.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/drivers/Correction.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

/// Base class for one driver — a consumer that reads the shared source buffer and
/// emits it to a destination (a physical LED output, a network sink, or the preview).
///
/// A driver optionally reads dimensions from an active Layer, optionally applies the
/// shared output correction, and optionally restricts its output to a contiguous
/// *window* of the source buffer (see `addWindowControls`). It plays the same
/// zero-state role for drivers that EffectBase does for effects.
class DriverBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Driver; }
    virtual void setSourceBuffer(Buffer* buf) = 0;
    /// Set the active Layer this driver reads dimensions from. Optional: drivers that
    /// need dimensions (such as PreviewDriver describing the LED grid in the WebSocket
    /// frame) call `layer_` for current physical width/height/depth; ArtNet doesn't need
    /// it — it just streams bytes.
    ///
    /// This is the *active* layer for dimension queries, not a 1:1 wiring constraint:
    /// each driver outputs to a single physical fixture, and the Drivers container hands
    /// it one Layer for dimensions regardless of how many layers feed the output buffer.
    void setLayer(Layer* layer) { layer_ = layer; }

    /// First light of the configured window. Public for tests pinning the slice
    /// arithmetic; production reads via `windowSlice()`. See `start_`/`count_` below.
    uint16_t windowStart() const { return start_; }
    /// Number of lights in the configured window (0 = to end of buffer).
    uint16_t windowCount() const { return count_; }
    /// Set the window directly (the UI sets it via the start/count controls; this is
    /// for code-wiring a driver's slice and for tests). Takes effect on the next config
    /// parse / loop, like a control edit.
    void setWindow(uint16_t start, uint16_t count) { start_ = start; count_ = count; }
    /// The active Layer this driver reads dimensions from — null when no Layer is wired
    /// (such as after the last Layer was deleted). Drivers must tolerate null here.
    Layer* layer() const { return layer_; }

    /// Hand this driver the shared output correction (brightness LUT + channel order +
    /// white) owned by the Drivers container. Default no-op so Preview (which shows the
    /// raw logical buffer) and any preview-style driver opt out for free; only physical
    /// drivers (ArtNet, LED) override to apply it.
    virtual void setCorrection(const Correction* /*c*/) {}

    /// Notified by Drivers when the shared Correction's outChannels may have changed (a
    /// lightPreset switch RGB↔RGBW). Default no-op; physical drivers that own an
    /// intermediate correction-applied buffer override to resize it OFF the hot path.
    /// Topology changes (light count, channels per light) already flow through
    /// onBuildState — this hook is just for the preset-driven channel-count change that
    /// doesn't trigger a structural rebuild.
    virtual void onCorrectionChanged() {}

    /// Clear every shared status string on teardown — fail buffer, config error, and config
    /// warning — so a stopped driver leaves nothing behind (frees the owned `failBuf_`; retracts
    /// the warning the same "clear only MY status" way as the error). A driver that overrides
    /// teardown() for its own peripheral cleanup chains to this afterwards:
    /// `deinit(); DriverBase::teardown();`.
    void teardown() override { clearFailBuf(); clearConfigErr(); setConfigWarn(nullptr); }

    /// Release-on-disable helper for the drivers that hold a scarce resource (a GPIO peripheral or a
    /// socket): disabling frees it so another module can claim the pins, re-enabling re-acquires. The
    /// driver's own setup()/teardown() ARE its acquire/release, so this routes to them virtually — no
    /// duplicated logic; the freed pins then show released in the pin map. NOT wired into onEnabled at
    /// the base, because a driver whose teardown() has extra lifecycle (HueDriver frees name buffers
    /// its setup() doesn't re-fetch) would half-tear-down on a disable; each resource-holding driver
    /// opts in by overriding onEnabled to call this. Preview/Hue (no GPIO/peripheral held) don't.
    void releaseOnDisable(bool on) { if (on) setup(); else teardown(); }

protected:
    Layer* layer_ = nullptr;

    // --- Shared source-buffer window (start, count) ---------------------------
    /// Every driver reads the SAME shared source buffer (Drivers hands the one
    /// `Buffer*` to each child) and outputs a contiguous slice of it: lights
    /// `[start_, start_+count_)`. This is how light distribution is made *explicit*
    /// and order-independent — a second driver on a different slice (such as an
    /// onboard status LED at index 0, the main strip from index 1) just sets its
    /// own window, rather than the buffer being split by driver order. `count_`==0
    /// means "the rest of the buffer from start_" (the common whole-buffer case).
    /// NetworkSendDriver's universe maps onto the same window; the LED drivers'
    /// pins/ledsPerPin distribute lights *within* the window. It is the alternative
    /// to a "split the buffer by sibling order" model some controllers use — here the
    /// user (or catalog) says which slice goes where.
    uint16_t start_ = 0;   ///< First source-buffer light this driver outputs (default 0).
    uint16_t count_ = 0;   ///< Lights to output from `start_`; 0 = to end of buffer.

    /// Add the two window controls — call from a driver's onBuildControls(). Kept a
    /// helper (not auto-added) so a driver opts in by calling it where its other
    /// controls go, keeping control *order* in the driver's hands.
    void addWindowControls() {
        controls_.addUint16("start", start_);
        controls_.addUint16("count", count_);
    }

    /// True if `name` is one of the window controls — a driver folds this into its
    /// controlChangeTriggersBuildState() so editing the slice re-runs its config.
    static bool isWindowControl(const char* name) {
        return std::strcmp(name, "start") == 0 || std::strcmp(name, "count") == 0;
    }

    /// Resolve the window against a buffer of `bufN` lights: writes the clamped first
    /// light to `outStart` and the slice length to `outLen` (0 if the window starts
    /// past the end — the driver then idles, no out-of-bounds read). The textbook
    /// `[start, start+count)` clamp — every driver calls this instead of reading from
    /// light 0.
    void windowSlice(nrOfLightsType bufN, nrOfLightsType& outStart,
                     nrOfLightsType& outLen) const {
        outStart = start_ < bufN ? start_ : bufN;
        const nrOfLightsType avail = static_cast<nrOfLightsType>(bufN - outStart);
        outLen = (count_ == 0 || count_ > avail) ? avail
                                                 : static_cast<nrOfLightsType>(count_);
    }

    // --- Shared status-string lifecycle for the physical LED drivers (RMT / LCD /
    // Parlio). They report two kinds of transient status that must clear cleanly
    // without stomping an unrelated status set by something else:
    //   configErr_ — a borrowed string literal (a parse-error message);
    //   failBuf_   — an owned, on-demand char buffer (a formatted loopback/init
    //                failure with numbers in it).
    // Both follow the same "clear only MY status" rule: only call clearStatus() if
    // the status currently shown is the one this driver set. This was triplicated
    // verbatim across the three drivers; it lives here once (the No-duplication
    // rule). Preview-style drivers never touch these, so the cost is a couple of
    // null pointers they ignore.
    const char* configErr_ = nullptr;
    const char* configWarn_ = nullptr;
    char* failBuf_ = nullptr;
    static constexpr size_t kFailBufLen = 48;

    // Record a parse/config error: set the status and remember it so clearConfigErr
    // can later retract exactly this one.
    void setConfigErr(const char* err) {
        configErr_ = err;
        setStatus(err, Severity::Error);
    }
    void clearConfigErr() {
        if (configErr_) {
            if (status() == configErr_) clearStatus();
            configErr_ = nullptr;
        }
    }

    // A non-fatal config warning (a borrowed literal, like configErr_): the driver keeps
    // running but flags something the user should see (e.g. a per-pin count clamped to the
    // WS2812 ceiling). `warn` non-null sets it; null retracts a stale one — so a driver calls
    // setConfigWarn(warn) unconditionally each parse and the warning tracks the live state.
    // Same "clear only MY status" rule as configErr_. Factored here so the WS2812 drivers
    // (Rmt / LCD / Parlio) don't each inline it — the No-duplication rule this block records.
    void setConfigWarn(const char* warn) {
        if (warn) {
            configWarn_ = warn;
            setStatus(warn, Severity::Warning);
        } else if (configWarn_) {
            if (status() == configWarn_) clearStatus();
            configWarn_ = nullptr;
        }
    }

    // Lazily allocate the owned fail-message buffer (caller snprintf's into it then
    // setStatus(failBuf_)). Returns null if the allocation fails, in which case the
    // caller falls back to a literal status.
    char* failBufEnsure() {
        if (!failBuf_) failBuf_ = static_cast<char*>(platform::alloc(kFailBufLen));
        return failBuf_;
    }
    void clearFailBuf() {
        if (failBuf_) {
            if (status() == failBuf_) clearStatus();
            platform::free(failBuf_);
            failBuf_ = nullptr;
        }
    }
};

} // namespace mm
