// Out-of-line half of JsonUtil.h. Only what must not be inlined lives here.
//
// The rest of the header is deliberately header-only (see its own comment): small first-match
// readers whose callers benefit from inlining. `parseIntStr` is the exception — its three
// validity checks are duplicated into every caller when inline, which measured 1712 bytes of
// flash on the S3 across the ~10 call sites. Every caller is off the hot path, so the call is
// free in practice.

#include "core/JsonUtil.h"

#include <cerrno>
#include <climits>
#include <cstdlib>

namespace mm::json {

int parseIntStr(const char* s, int fallback) {
    if (!s) return fallback;
    char* end = nullptr;
    errno = 0;                                              // strtol only ever SETS it on error
    const long v = std::strtol(s, &end, 10);
    if (end == s) return fallback;                          // no digits — not a number at all
    if (errno == ERANGE) return fallback;                   // saturated: outside `long`
    if (v < INT_MIN || v > INT_MAX) return fallback;        // fits `long`, would not survive `int`
    return static_cast<int>(v);
}

}  // namespace mm::json
