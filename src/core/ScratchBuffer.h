#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

class MoonModule;  // forward — the full definition is only needed in ScratchBuffer.cpp

/// Type-erased base for `ScratchBuffer<T>`: owns the one `platform::alloc`, the raw
/// `void*`, the owner tie (dynamic-bytes delta accounting + free-on-disable
/// registration), and the sole heavy body (`resizeBytes`). Compiled ONCE in
/// ScratchBuffer.cpp — no per-`T` duplication, so a device with several element
/// types pays for this logic once, not per type. Not used directly; `ScratchBuffer<T>`
/// is the public face.
///
/// This is the "type-erased base + thin typed façade" split (the same trick `std::vector`
/// implementations use to share code across element types), applied so a memory-holding
/// effect writes `heat_.resize(n)` and nothing else — the primitive is the free-helper,
/// the destructor, the `setDynamicBytes` call, and the null-guard, all at once.
class ScratchBufferBase {
protected:
    explicit ScratchBufferBase(MoonModule& owner);   // registers with owner (.cpp)
    ~ScratchBufferBase();                            // frees + deregisters (.cpp)

    ScratchBufferBase(const ScratchBufferBase&) = delete;
    ScratchBufferBase& operator=(const ScratchBufferBase&) = delete;
    // Move is NOT provided: a ScratchBuffer is a fixed member of its module, tied by
    // reference to that specific module and threaded into that module's free list.
    // Moving would dangle owner_ and corrupt the list. (Buffer.h is movable because it
    // is a standalone value; ScratchBuffer is deliberately not — it is an owned member.)
    ScratchBufferBase(ScratchBufferBase&&) = delete;
    ScratchBufferBase& operator=(ScratchBufferBase&&) = delete;

    /// Size to exactly `bytes` (0 frees), reallocating only when the byte count
    /// changes. Zero-fills on (re)alloc. Adjusts owner_'s dynamic-bytes total by
    /// (new - old). Returns true iff the buffer holds `bytes` afterwards (bytes==0
    /// returns true; an alloc failure returns false and leaves the buffer empty).
    bool resizeBytes(std::size_t bytes);

    void*       raw_   = nullptr;
    std::size_t bytes_ = 0;

private:
    friend class MoonModule;             // walks next_ / calls resizeBytes(0) on release
    MoonModule&        owner_;
    ScratchBufferBase* next_ = nullptr;  // intrusive singly-linked free-list node
};

/// Typed façade — pure inline sugar over ScratchBufferBase. Every method folds into its
/// call site, so a new `T` adds ~0 flash. The ONE `static_cast` in the whole design lives
/// in `data()`. Effects declare `ScratchBuffer<T> name_{*this};` and use
/// resize()/count()/operator[]/operator bool — no `void*`, no `sizeof`, no cast.
template <class T>
class ScratchBuffer : private ScratchBufferBase {
public:
    explicit ScratchBuffer(MoonModule& owner) : ScratchBufferBase(owner) {}

    /// Size to hold `count` elements (0 frees). Returns true on success.
    bool resize(std::size_t count) { return resizeBytes(count * sizeof(T)); }

    T*          data()        { return static_cast<T*>(raw_); }         // the one cast
    const T*    data()  const { return static_cast<const T*>(raw_); }
    std::size_t count() const { return bytes_ / sizeof(T); }
    std::size_t bytes() const { return bytes_; }
    explicit operator bool() const { return raw_ != nullptr; }

    T&       operator[](std::size_t i)       { return data()[i]; }
    const T& operator[](std::size_t i) const { return data()[i]; }
};

} // namespace mm
