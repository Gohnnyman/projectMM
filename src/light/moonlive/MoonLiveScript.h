#pragma once

#include "core/MoonModule.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "light/moonlive/MoonLiveScriptFile.h"

#include <cstring>

namespace mm::moonlive {

/// One scripted module's script: the file it points at, the engine holding the compiled program,
/// and the 4-byte content hash that answers "is what is compiled still what the file says".
///
/// Held BY VALUE in each binding rather than inherited from. The three scripted modules derive from
/// three sibling bases under MoonModule, so a shared base would need virtual inheritance and would
/// change the object layout of every module in the system to serve three of them.
///
/// The rule this exists to state once: **if the file changed, recompile**. That is all `sync()` is.
/// The three bindings each grew their own bookkeeping around that one rule and drifted apart:
/// an effect had no content hash at all (so editing a script's text changed nothing until it was
/// renamed), a layout cleared its hash only on a name change (same gap), and only a modifier
/// re-read the file each time. Same question, one answer.
class MoonLiveScript {
public:
    /// Re-read the file and recompile IFF its content hash moved.
    ///
    /// Returns true when a NEW program was installed. That return value is load-bearing rather than
    /// informational: a modifier turns it into "ask the Layer to rebuild its mapping", and the Layer
    /// rebuild IS applyState(), which calls prepare() again. Returning true unconditionally makes
    /// the two call each other forever, the mapping rebuilds every frame and the fixture renders
    /// nothing at all. So this is idempotent by contract: called twice with an unchanged file, the
    /// second call returns false and touches nothing.
    ///
    /// `owner` receives the status line and the dynamic-byte figure, so a binding does not repeat
    /// the reporting. `sysvars` is the one thing that genuinely differs per role (an effect is told
    /// its grid, a layout is not), which is why it is a parameter rather than a member.
    bool sync(const SysVarTable& sysvars, MoonModule& owner) {
        // Cheapest question first: does the file still hash to what is loaded? This runs on every
        // prepare sweep, and a file write now triggers one, so it must not cost a compile. It reads
        // through a small stack buffer and allocates nothing.
        uint32_t fileHash = 0;
        const bool readable = scriptFileHash(name_, fileHash);
        if (readable && engine_.ok() && compiledHash_ != 0 && fileHash == compiledHash_) return false;

        // A failed compile leaves compiledHash_ at 0 and the engine not ok(), which is
        // indistinguishable from "not compiled yet", so without this latch a layout re-reads and
        // re-compiles the file on every lightCount()/placeLights(). Each attempt is two LittleFS
        // operations (~5 ms on an S3), the pipeline asks repeatedly while sizing and walking the
        // fixture, and the retries starve the task until the 12 s watchdog RESETS THE DEVICE.
        //
        // Keyed on the name AND the content, not the name alone. As a bare flag it latched on the
        // empty script every device boots with and skipped the compile forever; keyed on the name
        // only, it would refuse to re-try a script the user just fixed in place, which is exactly
        // what editing a file on its card does.
        //
        // An UNREADABLE file (missing, empty, oversized) hashes to nothing, so it is latched on the
        // name alone: there is no content to have changed, and a hash comparison would compare 0
        // against the hash of whatever failed last and release the latch on every ask. That is the
        // watchdog case above, reached by deleting the script a card points at.
        if (compileFailed_ && std::strcmp(failedScript_, name_) == 0 &&
            (!readable ? !failedReadable_ : (failedReadable_ && fileHash == failedHash_)))
            return false;

        resetPrintBudget();
        const char* err = nullptr;
        uint32_t hash = 0;
        if (compileScriptFile(engine_, name_, lightBuiltins(), sysvars, err, &hash)) {
            // Declare the controls the script asks for, the way a compiled module does: by RUNNING
            // defineControls(). Before the binding's rebuildControls(), which turns the declared
            // list into UI cards.
            runDefineControls(engine_);
            // A compiled script is not an error, but it has something to say: how big it is, and
            // the one budget it is closest to using up. The card's memory figure is the ALLOCATION,
            // word-rounded, which says nothing about the program itself.
            engine_.describe(statusBuf_, sizeof(statusBuf_));
            owner.setStatus(statusBuf_, MoonModule::Severity::Status);
            compileFailed_ = false;
        } else {
            owner.setStatus(err, MoonModule::Severity::Error);
            compileFailed_ = true;
            failedHash_ = fileHash;
            failedReadable_ = readable;   // distinguishes "this text is broken" from "no file"
            std::snprintf(failedScript_, sizeof(failedScript_), "%s", name_);
        }
        // The CONTENT hash, not a copy of the text: 4 bytes to answer "is what I compiled still what
        // the file says". 0 means nothing is compiled.
        compiledHash_ = hash;
        owner.setDynamicBytes(engine_.heapBytes());
        return true;
    }

    /// The name buffer the `script` control binds to. A control write lands here DIRECTLY, without
    /// passing through setName(), which is why sync() re-derives from the file rather than trusting
    /// a flag someone remembered to clear.
    char*  buffer() { return name_; }
    size_t bufferSize() const { return sizeof(name_); }
    const char* name() const { return name_; }

    /// Point at a different script. The next sync() compiles it.
    void setName(const char* n) {
        if (!n) return;
        std::snprintf(name_, sizeof(name_), "%s", n);
        invalidate();
    }

    /// Forget what is compiled, so the next sync() rebuilds. For a module coming back from
    /// disabled, where the engine was released but the name was kept.
    void invalidate() {
        compiledHash_ = 0;
        compileFailed_ = false;
        failedHash_ = 0;
        failedReadable_ = false;
        failedScript_[0] = '\0';
    }

    MoonLive&       engine()       { return engine_; }
    const MoonLive& engine() const { return engine_; }
    bool ok() const { return engine_.ok(); }

private:
    MoonLive engine_;
    // Backing store for the status line: MoonModule::setStatus keeps a POINTER, so the text has to
    // outlive the call. The same module-owned pattern NetworkModule uses.
    char     statusBuf_[48] = {};
    // The script's FILE NAME, inside the shared script directory. Empty on a fresh card: it reports
    // "no script" until one is named, rather than every new module compiling the same default.
    char     name_[kMaxScriptName + 1] = "";
    uint32_t compiledHash_ = 0;
    // The name AND content that failed, so a retry is skipped only while both still match. Content
    // too, because the whole point of this step is that a file's text changes under a fixed name:
    // latching on the name alone would refuse to re-try a script the user just fixed.
    bool     compileFailed_ = false;
    bool     failedReadable_ = false;   // was there a file at all when it failed?
    uint32_t failedHash_ = 0;
    char     failedScript_[kMaxScriptName + 1] = "";
};

}  // namespace mm::moonlive
