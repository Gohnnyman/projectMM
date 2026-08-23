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
    /// Let a binding that owns a particle pool size it from the script's defineControls(). Null for
    /// a binding with no particles, which is every binding but the effect today.
    void setPoolSizer(PoolSizeFn fn, void* ctx) { sizePool_ = fn; poolCtx_ = ctx; }

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
        // prepare sweep, and a file write now triggers one, so it must not cost a compile. One read
        // answers it, against a compile's read plus parse, codegen and exec-block allocation.
        uint32_t fileHash = 0;
        const bool readable = scriptFileHash(name_, fileHash);
        if (readable && engine_.ok() && haveCompiled_ && fileHash == compiledHash_) return false;

        // A failed compile leaves haveCompiled_ false and the engine not ok(), which is
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
            runDefineControls(engine_, sizePool_, poolCtx_);
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
        // the file says". Whether anything IS compiled is a separate flag rather than hash != 0,
        // because 0 is a legitimate hash: a script that happened to hash to it would recompile on
        // every prepare sweep, which is the cost this comparison exists to avoid.
        compiledHash_ = hash;
        haveCompiled_ = engine_.ok();   // a FAILED compile has no program, whatever the file hashed to
        // ADD the engine's heap to whatever else the owner holds, rather than assigning it: a
        // binding may also own ScratchBuffers (a particle pool), and those report themselves
        // through the buffer's own delta hook. Assigning here would erase them, which is the
        // "don't mix addDynamicBytes with setDynamicBytes" contract MoonModule states. Tracking
        // what this script last reported keeps it a delta rather than a running total.
        const size_t nowBytes = engine_.heapBytes();
        owner.setDynamicBytes(owner.dynamicBytes() - reportedBytes_ + nowBytes);
        reportedBytes_ = nowBytes;
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

    /// Hand back everything this script reported to its owner. Called from a binding's release(),
    /// AFTER engine().free(): the exec block is gone, so the owner's card must stop counting it.
    ///
    /// One home for all three bindings rather than two lines each: MoonLive::free() does not touch
    /// the owner's total, so a binding that forgets this leaves a disabled module reporting memory
    /// it no longer holds. Subtracting rather than zeroing is what lets a binding own OTHER memory
    /// too (the effect's particle pool), which a setDynamicBytes(0) would wrongly erase.
    void releaseReporting(MoonModule& owner) {
        const size_t held = owner.dynamicBytes();
        owner.setDynamicBytes(held > reportedBytes_ ? held - reportedBytes_ : 0);
        reportedBytes_ = 0;
    }

    /// Forget what is compiled, so the next sync() rebuilds. For a module coming back from
    /// disabled, where the engine was released but the name was kept.
    void invalidate() {
        compiledHash_ = 0;
        haveCompiled_ = false;
        compileFailed_ = false;
        failedHash_ = 0;
        failedReadable_ = false;
        failedScript_[0] = '\0';
    }

    /// Publish every control the compiled script declared into `controls`, bound by reference to
    /// the engine's live arena slot so a slider write lands where the running native code reads it.
    /// The ONE home for this: all three bindings (effect, layout, modifier) publish identically,
    /// and the type dispatch below is the kind of reasoning that should be stated once.
    void publishDeclaredControls(ControlList& controls) {
        uint8_t n = 0;
        const moonlive::DeclaredControl* decls = engine_.declaredControls(n);
        for (uint8_t i = 0; i < n; i++) {
            uint8_t* slot = engine_.controlSlot(decls[i].offset);
            if (!slot) continue;   // engine not compiled yet — controls appear after prepare
            // Published as the widget the member's TYPE calls for. Every scalar occupies the same
            // 4-byte slot, so this is no longer a width dispatch: it is the semantic one, and the
            // storage underneath is identical in all three cases.
            //
            // Safe to view the slot as its type: the compiler aligns every member to a 4-byte
            // arena offset, and the arena base comes from platform::alloc, which is aligned for
            // any fundamental type.
            //
            // byte and bool point at the slot's LOW BYTE, which is only correct because the two
            // are masked on store: the upper three bytes are always zero, so a 1-byte control
            // reading and writing that byte sees the member's whole value. On a big-endian target
            // the low byte would be at offset+3 — no supported target is one.
            switch (decls[i].type) {
                case moonlive::CtrlType::Bool:
                    controls.addBool(decls[i].name, *reinterpret_cast<bool*>(slot));
                    break;
                case moonlive::CtrlType::Byte:
                    controls.addUint8(decls[i].name, *slot,
                                      static_cast<uint8_t>(decls[i].min),
                                      static_cast<uint8_t>(decls[i].max));
                    break;
                default:   // Int; Fixed and Str never reach here (the compiler refuses to bind one)
                    controls.addInt32(decls[i].name, *reinterpret_cast<int32_t*>(slot),
                                      decls[i].min, decls[i].max);
                    break;
            }
            // The member's initializer (`byte bpm = 60;`) IS the control's default, and it is
            // the only place one exists: /api/types probes a fresh module for defaults, and a
            // scripted module's controls come from the script, so a probe with no script declares
            // none. Carried on the control instead, which is what lights the UI's reset button.
            controls.setDefault(controls.count() - 1, static_cast<int32_t>(decls[i].def));
        }
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
    bool     haveCompiled_ = false;   // 0 is a valid hash, so "is anything compiled" is its own flag
    // The name AND content that failed, so a retry is skipped only while both still match. Content
    // too, because the whole point of this step is that a file's text changes under a fixed name:
    // latching on the name alone would refuse to re-try a script the user just fixed.
    bool     compileFailed_ = false;
    bool     failedReadable_ = false;   // was there a file at all when it failed?
    uint32_t failedHash_ = 0;
    char     failedScript_[kMaxScriptName + 1] = "";

    size_t     reportedBytes_ = 0;   // what this script last added to the owner's total
    PoolSizeFn sizePool_ = nullptr;
    void*      poolCtx_  = nullptr;
};

}  // namespace mm::moonlive
