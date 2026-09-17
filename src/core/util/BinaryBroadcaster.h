#pragma once

#include <cstdint>
#include <cstddef>  // size_t

namespace mm {

// A sink that broadcasts a binary WebSocket message to all connected clients.
// HttpServerModule implements it; producers (e.g. PreviewDriver) hold a pointer
// to this interface rather than to the concrete server, so a light-domain
// producer depends only on "something I can send bytes to" — not on the HTTP
// server's full surface. Domain-neutral: the bytes' meaning is the caller's.
struct BinaryBroadcaster {
    // RESUMABLE one-frame send for a payload that lives in a STABLE caller-owned buffer (no copy):
    // one WS message = `header` (copied — small, may be a stack local) followed by `body` (a pointer
    // the caller keeps stable until the send completes or is cancelled). The implementation drains it
    // across transport-poll ticks (a bounded chunk per tick, returning on a would-block socket), so a
    // large frame stays off the caller's hot path. The frame is still ONE atomic WS message to the
    // browser — "resumable" means delivered over wall-clock, not split into multiple messages.
    //   sendBufferedFrame(...) — begin a send; while one is in flight a new call is DROPPED
    //                            (drop-new backpressure — the in-flight frame is kept, the new one
    //                            rejected), and the caller reads that as "link busy".
    //   bufferedSendIdle()     — true when no send is in flight (the previous frame fully drained
    //                            or was cancelled). The caller gates the next frame on this, so the
    //                            effective frame rate self-limits to what the link sustains.
    //   cancelBufferedSend()   — abandon the in-flight send NOW. The caller calls this before it
    //                            frees/reallocates the `body` buffer (a geometry rebuild), keeping a
    //                            cursor reading only live memory. A client caught mid-message is
    //                            closed by the implementation (the only honest exit once bytes are
    //                            out); it reconnects and the generation bump primes it fresh.
    // Only PreviewDriver uses this today (the color frames: full-res hands the producer buffer,
    // downsampled and the coord table hand their gathered staging buffers), so every /wsp message
    // rides the one paced path.
    virtual bool sendBufferedFrame(const uint8_t* header, size_t headerLen,
                                   const uint8_t* body, size_t bodyLen) = 0;
    virtual bool bufferedSendIdle() const = 0;
    virtual void cancelBufferedSend() = 0;



    // How many subscribers are listening right now. Purely observational (a status line, a log);
    // producers must not branch per subscriber through this, the channel stays broadcast-only.
    virtual int subscriberCount() const { return 0; }

    // Inbound client messages, delivered OPAQUELY: the transport unmasks a client's WS frame
    // (framing is its job) and hands the payload bytes to the registered sink; only the producer
    // knows what they mean. onClientGone fires when a client's slot closes or turns over, so a
    // producer keeping per-slot standing state (the preview's [stride][fps] request) can drop it
    // with the client. Both fire on the transport's own thread (core 0 under the split); a
    // producer ticking elsewhere stores single-byte fields the reader tolerates racing on, the
    // lossy-channel rule.
    struct ClientMessageSink {
        virtual void onClientMessage(int slot, const uint8_t* payload, int len) = 0;
        virtual void onClientGone(int slot) = 0;
    protected:
        ~ClientMessageSink() = default;
    };
    virtual void setClientMessageSink(ClientMessageSink* sink) { (void)sink; }

    // Exclusive access to the sender, for a producer that does NOT run on the transport's own thread.
    // The multicore split (Drivers `multicore`) ticks the offloaded PreviewDriver on core 1 while
    // this transport drains, reaps and admits on core 0: two producers, two cores, one preview
    // socket set and one resumable send slot (the control channel stays core-0-only and outside
    // this lease). A producer therefore brackets its whole message in tryAcquire/releaseSend:
    // a frame arm must not race the drain that is reading the slot.
    //
    // TRY-acquire, never block: the caller may be on the render or encode thread, where blocking is a
    // hot-path violation (CLAUDE.md § Hot path). false = the transport is busy this instant → SKIP the
    // message, don't wait. Skipping is already the producer's back-off path (PreviewDriver's adaptive
    // frame rate drops a slot whenever the link is behind), so a lost race costs one frame at most.
    //
    // Single-threaded transports may return true unconditionally: with one producer thread there is no
    // race to prevent, and the pair is then free.
    virtual bool tryAcquireSend() = 0;
    virtual void releaseSend() = 0;

protected:
    ~BinaryBroadcaster() = default;  // not owned through this interface
};

/// RAII bracket for the pair above: `if (SendLease s{bc}; s) { …one whole message… }`.
/// Releases on scope exit; no-ops when the transport was busy. Same shape as mm::LockGuard.
class SendLease {
public:
    explicit SendLease(BinaryBroadcaster* bc)
        : bc_(bc), held_(bc && bc->tryAcquireSend()) {}
    ~SendLease() { if (held_) bc_->releaseSend(); }
    explicit operator bool() const { return held_; }
    SendLease(const SendLease&) = delete;
    SendLease& operator=(const SendLease&) = delete;
private:
    BinaryBroadcaster* bc_;
    bool held_;
};

} // namespace mm
