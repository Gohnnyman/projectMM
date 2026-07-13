#pragma once

#include <cstdint>
#include <cstdlib>  // std::strtol
#include <cstring>  // std::memcpy

namespace mm {

// Domain-neutral core primitive: parse a destination list of IPv4 addresses from one text control.
// Sits beside parsePinList (the GPIO-CSV parser) as the same shape of thing — a human-typed list of
// hardware endpoints — so a driver that fans one buffer out to N receivers reads its destinations
// the same way a driver that fans one buffer out to N GPIO lanes reads its pins.
//
// Why a LIST rather than one IP per driver instance: Art-Net 4 requires ArtDmx to be unicast to the
// node that owns each universe ("There are no conditions in which broadcast is allowed"), so driving
// N receivers means N destinations. Expressing that as one control keeps the common case (a row of
// identical tubes on consecutive addresses) to a single field instead of N driver instances.
//
// Two syntaxes, both accepted, and mixable:
//   "192.168.1.60-70"        → a RANGE: .60 .61 … .70 (last octet only; the first three must match)
//   "192.168.1.60,61,62,65"  → a LIST: after a full dotted quad, a bare number is the next host on
//                              the SAME /24 (the shorthand people actually type), and a further full
//                              quad switches subnet.
//   "192.168.1.60, 10.0.0.5" → explicit full quads, any subnets.
//
// Returns nullptr on success, or a static error literal the caller feeds straight to setStatus().
// Host-tested by unit_IpList.cpp.

// Parse one dotted quad (or a bare last-octet shorthand continuing `prev`) starting at *p.
// Advances *p past the token. Returns false on a malformed token.
namespace detail {
inline bool parseOneIp(const char*& p, const uint8_t prev[4], bool havePrev, uint8_t out[4]) {
    long o[4];
    char* end = nullptr;
    o[0] = std::strtol(p, &end, 10);
    if (end == p || o[0] < 0 || o[0] > 255) return false;
    const char* q = end;
    uint8_t n = 1;
    while (n < 4 && *q == '.') {
        const char* after = q + 1;
        char* e2 = nullptr;
        o[n] = std::strtol(after, &e2, 10);
        if (e2 == after || o[n] < 0 || o[n] > 255) return false;
        q = e2;
        n++;
    }
    if (n == 4) {                       // a full dotted quad
        for (uint8_t i = 0; i < 4; i++) out[i] = static_cast<uint8_t>(o[i]);
    } else if (n == 1 && havePrev) {    // a bare host number → same /24 as the previous address
        std::memcpy(out, prev, 3);
        out[3] = static_cast<uint8_t>(o[0]);
    } else {
        return false;                   // a bare number with no previous address to extend
    }
    p = q;
    return true;
}
}  // namespace detail

// Parse `s` into out[0..maxIps) as 4-byte quads. `nOut` receives the count.
// An empty/blank string is NOT an error — it yields nOut == 0, the "unconfigured, idle" state
// (a driver with no destination must do nothing, never fall back to broadcasting).
inline const char* parseIpList(const char* s, uint8_t (*out)[4], uint8_t maxIps, uint8_t& nOut) {
    nOut = 0;
    if (!s) return nullptr;
    while (*s == ' ') s++;
    if (!*s) return nullptr;            // blank → no destinations, caller idles

    const char* p = s;
    uint8_t prev[4] = {};
    bool havePrev = false;

    while (true) {
        uint8_t ip[4];
        if (!detail::parseOneIp(p, prev, havePrev, ip)) return "invalid ip list";

        while (*p == ' ') p++;
        if (*p == '-') {                // a RANGE: expand low..high over the last octet
            p++;
            while (*p == ' ') p++;
            char* end = nullptr;
            const long hi = std::strtol(p, &end, 10);
            if (end == p || hi < 0 || hi > 255) return "invalid ip range";
            if (hi < ip[3]) return "ip range runs backwards";
            for (long last = ip[3]; last <= hi; last++) {
                if (nOut >= maxIps) return "too many destinations";
                out[nOut][0] = ip[0]; out[nOut][1] = ip[1];
                out[nOut][2] = ip[2]; out[nOut][3] = static_cast<uint8_t>(last);
                nOut++;
            }
            std::memcpy(prev, ip, 4);
            havePrev = true;
            p = end;
        } else {
            if (nOut >= maxIps) return "too many destinations";
            std::memcpy(out[nOut], ip, 4);
            nOut++;
            std::memcpy(prev, ip, 4);
            havePrev = true;
        }

        while (*p == ' ') p++;
        if (*p == '\0') return nullptr;
        if (*p != ',') return "invalid ip list";
        p++;
        while (*p == ' ') p++;
    }
}

}  // namespace mm
