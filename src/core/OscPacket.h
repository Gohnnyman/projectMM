#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>     // std::lround: correct float-to-byte rounding
#include <cstring>

// OSC 1.0 wire format: the one place the layout lives (the ArtNetPacket.h /
// WLEDAudioSyncPacket.h convention: constants plus an inline parse, pinned by a golden byte
// vector so we cannot drift from the spec). OscModule receives with it; nothing sends yet.
//
// A message is three parts, each padded to a 4-byte boundary, everything BIG-endian:
//
//   address pattern   OSC-string starting '/', NUL-terminated, then 0-3 NULs to reach a multiple of 4
//   type tag string   OSC-string starting ',', one character per argument
//   arguments         in tag order, each 32-bit aligned
//
// Type tags we read: 'i' int32, 'f' float32. The spec also has 's' string and 'b' blob, plus the
// no-payload 'T'/'F'/'N'/'I'; a control value is a number, so the rest are skipped rather than
// rejected (a controller that appends a label must not make the whole message unusable).
//
// PARSING IS THE SECURITY SURFACE: this reads an unauthenticated datagram off the LAN, so every
// read is length-checked against the buffer end and nothing is allocated. A malformed packet
// returns false; it never reads past `len`.
//
// Bundles ("#bundle" instead of an address) are recognized and rejected: controllers send plain
// messages, and a bundle is a timed batch we have no use for yet (see the plan's Not doing).

namespace mm::osc {

inline constexpr uint16_t kDefaultPort = 9000;   // the de-facto OSC receive port (TouchOSC's default)

/// One parsed message: the address, and the first numeric argument as both forms.
struct Message {
    const char* address = nullptr;   ///< points INTO the caller's buffer, NUL-terminated there
    float    f = 0.0f;               ///< first numeric argument as a float ('i' converted)
    int32_t  i = 0;                  ///< first numeric argument as an int ('f' truncated)
    bool     hasValue = false;       ///< false when the message carried no numeric argument
    bool     wasFloat = false;       ///< which form arrived, so a caller can scale 0..1 correctly
};

/// Round `n` up to the next multiple of 4, the padding every OSC element uses.
inline constexpr size_t pad4(size_t n) { return (n + 3u) & ~static_cast<size_t>(3u); }

/// Big-endian loads: OSC is network byte order, which is not the host's on any target we build.
inline int32_t beInt32(const uint8_t* p) {
    return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                                (static_cast<uint32_t>(p[1]) << 16) |
                                (static_cast<uint32_t>(p[2]) << 8) |
                                 static_cast<uint32_t>(p[3]));
}
inline float beFloat32(const uint8_t* p) {
    const int32_t bits = beInt32(p);
    float f;
    std::memcpy(&f, &bits, 4);   // the standard type-pun; a reinterpret_cast here is UB
    return f;
}

/// Length of the NUL-terminated string at `p`, including its padding, or 0 if it is not
/// terminated within the buffer. 0 is the failure signal: a valid OSC-string is never empty,
/// since it carries at least the leading '/' or ','.
inline size_t stringLen(const uint8_t* p, size_t avail) {
    for (size_t k = 0; k < avail; k++)
        if (p[k] == '\0') return pad4(k + 1);
    return 0;   // ran off the end without a terminator
}

/// Parse one OSC message. Returns false for anything we cannot use, having read nothing past
/// `len`: a bundle, a truncated packet, a missing or malformed type tag, or an address that is
/// not NUL-terminated inside the buffer.
///
/// `out.address` points into `pkt`, so it is valid only while that buffer is.
inline bool parse(const uint8_t* pkt, size_t len, Message& out) {
    if (!pkt || len < 8) return false;                    // shorter than the smallest valid message
    if (pkt[0] == '#') return false;                      // "#bundle": not handled, see the header
    if (pkt[0] != '/') return false;                      // an address pattern always starts with '/'

    const size_t addrLen = stringLen(pkt, len);
    if (addrLen == 0 || addrLen >= len) return false;     // unterminated, or nothing after it
    out.address = reinterpret_cast<const char*>(pkt);

    const uint8_t* tags = pkt + addrLen;
    const size_t tagsAvail = len - addrLen;
    if (tags[0] != ',') return false;                     // the type tag string always starts with ','
    const size_t tagsLen = stringLen(tags, tagsAvail);
    if (tagsLen == 0) return false;

    const uint8_t* arg = tags + tagsLen;
    size_t argAvail = (tagsLen < tagsAvail) ? tagsAvail - tagsLen : 0;

    // Walk the tags to the first numeric one, stepping over the sizes of those we skip. A message
    // with no numeric argument is still valid (a bare /mm/pad/3 press), so hasValue stays false.
    for (size_t t = 1; tags[t] != '\0' && t < tagsLen; t++) {
        switch (tags[t]) {
            case 'i':
                if (argAvail < 4) return false;
                out.i = beInt32(arg);
                out.f = static_cast<float>(out.i);
                out.hasValue = true; out.wasFloat = false;
                return true;
            case 'f':
                if (argAvail < 4) return false;
                out.f = beFloat32(arg);
                out.i = static_cast<int32_t>(out.f);
                out.hasValue = true; out.wasFloat = true;
                return true;
            // Skipped types: step over their payload and keep looking for a number.
            case 's': {
                const size_t n = stringLen(arg, argAvail);
                if (n == 0) return false;
                arg += n; argAvail -= n;
                break;
            }
            case 'b': {
                if (argAvail < 4) return false;
                const int32_t blob = beInt32(arg);
                if (blob < 0) return false;
                const size_t n = 4 + pad4(static_cast<size_t>(blob));
                if (n > argAvail) return false;
                arg += n; argAvail -= n;
                break;
            }
            case 'h': case 'd': case 't':                 // 64-bit types: skip the payload
                if (argAvail < 8) return false;
                arg += 8; argAvail -= 8;
                break;
            case 'T': case 'F': case 'N': case 'I':       // no payload
                break;
            default:
                return false;                             // an unknown tag: we cannot find the args
        }
    }
    return true;   // a valid message that simply carries no number
}

/// The 0..255 control value a message means.
///
/// OSC apps overwhelmingly send a float in 0..1 (TouchOSC, Resolume); hardware bridges usually
/// send an int in the target's own range. Both are accepted, and CLAMPED rather than rejected: a
/// controller sending 0..127 must not appear dead, and a float slightly past 1.0 is a rounding
/// artifact, not an error.
inline uint8_t toByte(const Message& m) {
    if (!m.hasValue) return 0;
    if (m.wasFloat) {
        const float v = m.f <= 0.0f ? 0.0f : (m.f >= 1.0f ? 1.0f : m.f);
        return static_cast<uint8_t>(std::lround(v * 255.0f));
    }
    return static_cast<uint8_t>(m.i <= 0 ? 0 : (m.i >= 255 ? 255 : m.i));
}

} // namespace mm::osc
