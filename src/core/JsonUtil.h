#pragma once

// JSON helpers for projectMM. Two layers, both header-only, both off the hot path
// (persistence load at boot, control writes) so bounded stack use is fine:
//
//   1. Flat helpers (parseString/hasKey/parseInt/parseBool): first-match key lookup
//      over the subset we emit — flat key/value pairs, optional whitespace after the
//      colon (Python's json.dumps inserts it), string/integer/boolean values. They do
//      not descend into nested objects or arrays; many callers rely on this cheap
//      strstr-based scan (HttpServerModule, FilesystemModule, scenario_runner, Control).
//
//   2. Recursive reader (JsonDoc/parse + the read/get accessors below): a standard
//      recursive-descent parser, the recognizable shape for walking nested structure —
//      needed for the persisted device / preset lists, arrays of small objects. The text
//      arena and node pool are HEAP-allocated per parse, sized to the input and grown as
//      needed (nodes are referenced by index, so a realloc never dangles), then freed when
//      the JsonDoc is destroyed — so there is no node-count or length cap and no large
//      standing buffer; only recursion is bounded (kMaxDepth, for the ESP32 task stack).
//      Any malformed / truncated input fails cleanly (parse() returns false, accessors
//      return safe defaults) and never reads OOB. Off the hot path (boot load, control writes).

#include <cerrno>                   // ERANGE — parseIntStr's overflow signal on 32-bit `long`
#include <climits>                  // INT_MIN/INT_MAX — parseIntStr's range check
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm::json {

// The longest `"<key>"<sep>` search pattern these readers build. Sized from the parts rather than a
// round number, so the bound is provable: 2 quotes + a colon + an optional space + NUL = 5 bytes of
// fixture around the key.
inline constexpr size_t kMaxKeyLen = 58;
inline constexpr size_t kSearchLen = kMaxKeyLen + 5;

// Build `"<key>"<sep>` into `buf` — the pattern the readers below strstr for. Returns false when the
// key is too long to fit, which the caller MUST treat as "not found".
//
// Why it returns a bool rather than truncating: a truncated pattern still strstr's, and it matches
// the WRONG thing (or nothing) — so an over-long key would silently read as absent, and an absent key
// takes the default. That is the same silent-default failure mode that once reset a control to 0 on
// every reboot. Failing the lookup loudly-in-code (and leaving the value untouched) beats guessing.
// The single helper also removes the four copies of this snprintf that GCC flagged as truncating.
inline bool buildKeyPattern(char (&buf)[kSearchLen], const char* key, const char* sep) {
    const int n = std::snprintf(buf, sizeof(buf), "\"%s\"%s", key, sep);
    return n > 0 && static_cast<size_t>(n) < sizeof(buf);
}

inline void parseString(const char* json, const char* key, char* out, size_t maxLen) {
    if (!json || !key || !out || maxLen == 0) return;
    char search[kSearchLen];
    if (!buildKeyPattern(search, key, ":\"")) return;      // key too long → treat as absent
    const char* start = std::strstr(json, search);
    if (!start) {
        if (!buildKeyPattern(search, key, ": \"")) return;
        start = std::strstr(json, search);
    }
    if (!start) return;
    start += std::strlen(search);
    // Copy until the real closing quote, decoding the JSON string escapes our own writer emits
    // (JsonSink::appendEscaped / writeJsonString): \" \\ \n \r \t \b \f and `\u00XX` for control
    // bytes < 0x20. A bare strchr for '"' would stop at an escaped quote inside the value, and a
    // multi-line value (a script with a `\n`) would arrive with a literal backslash-n unless \n is
    // decoded — so reader and writer stay symmetric.
    auto hexNibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    size_t oi = 0;
    for (const char* p = start; *p && oi + 1 < maxLen; p++) {
        if (*p == '\\' && p[1]) {
            p++;                 // consume the backslash; map the escape
            switch (*p) {
                case 'n': out[oi++] = '\n'; break;
                case 'r': out[oi++] = '\r'; break;
                case 't': out[oi++] = '\t'; break;
                case 'b': out[oi++] = '\b'; break;
                case 'f': out[oi++] = '\f'; break;
                case 'u': {     // \uXXXX — decode the low byte (the writer only emits \u00XX)
                    int h1 = p[1] ? hexNibble(p[1]) : -1, h2 = (p[1] && p[2]) ? hexNibble(p[2]) : -1;
                    int h3 = (p[1] && p[2] && p[3]) ? hexNibble(p[3]) : -1;
                    int h4 = (p[1] && p[2] && p[3] && p[4]) ? hexNibble(p[4]) : -1;
                    if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0) {
                        out[oi++] = static_cast<char>((h3 << 4) | h4);   // low byte (high byte is 0x00)
                        p += 4;
                    } else { out[oi++] = 'u'; }   // malformed \u — copy literally, don't run off
                    break;
                }
                default:  out[oi++] = *p;   break;   // \" \\ / and anything else: copy literally
            }
        } else if (*p == '"') {
            break;               // unescaped quote — end of string
        } else {
            out[oi++] = *p;
        }
    }
    out[oi] = 0;
}

// True when `key` is present in the JSON object. Lets callers distinguish a
// genuinely-absent key from one whose value happens to be 0/false — parseInt and
// parseBool can't, so applying their result for an absent key would clobber a
// control's non-zero default (e.g. eth phyType=2) with 0 on a partial/older save.
inline bool hasKey(const char* json, const char* key) {
    if (!json || !key) return false;
    char search[kSearchLen];
    if (!buildKeyPattern(search, key, ":")) return false;   // key too long → treat as absent
    return std::strstr(json, search) != nullptr;
}

/// The integer `s` starts with, or `fallback` when it does not start with one or the value does
/// not fit in `int`. The one string→int conversion these readers use.
///
/// `strtol`, not `atoi`: atoi cannot report a failure, so "no digits at all" and a genuine `"0"`
/// are indistinguishable, and a value too large for `long` is undefined behavior rather than a
/// detectable error. Callers that then narrow the result (a `uint16_t` id, a `uint8_t` percent)
/// would silently store a DIFFERENT valid number — `"65537"` becoming 1. Out-of-range is reported
/// as the fallback here, which is the only place it can still be seen.
///
/// Overflow needs BOTH checks, because they cover different targets. `long` is 64-bit on the
/// desktop, so a huge value lands inside `long` and only the INT_MAX compare rejects it; `long` is
/// 32-bit on ESP32 and Windows, where `LONG_MAX == INT_MAX` makes that compare dead code and
/// strtol's own saturation-to-LONG_MAX plus `ERANGE` is the only signal. Testing one alone passes
/// on the desktop and silently returns INT_MAX on the target this exists to protect.
///
/// Trailing text is deliberately allowed: these values are read out of a JSON body, so digits are
/// followed by `,` or `}`. Only the LEADING characters decide.
inline int parseIntStr(const char* s, int fallback = 0) {
    if (!s) return fallback;
    char* end = nullptr;
    errno = 0;                                              // strtol only ever SETS it on error
    const long v = std::strtol(s, &end, 10);
    if (end == s) return fallback;                          // no digits — not a number at all
    if (errno == ERANGE) return fallback;                   // saturated: outside `long`
    if (v < INT_MIN || v > INT_MAX) return fallback;        // fits `long`, would not survive `int`
    return static_cast<int>(v);
}

inline int parseInt(const char* json, const char* key) {
    if (!json || !key) return 0;
    char search[kSearchLen];
    if (!buildKeyPattern(search, key, ":")) return 0;       // key too long → treat as absent
    const char* start = std::strstr(json, search);
    if (!start) {
        if (!buildKeyPattern(search, key, ": ")) return 0;
        start = std::strstr(json, search);
    }
    if (!start) return 0;
    return parseIntStr(start + std::strlen(search));
}

inline bool parseBool(const char* json, const char* key) {
    if (!json || !key) return false;
    char search[kSearchLen];
    if (!buildKeyPattern(search, key, ":")) return false;   // key too long → treat as absent
    const char* start = std::strstr(json, search);
    if (!start) {
        if (!buildKeyPattern(search, key, ": ")) return false;
        start = std::strstr(json, search);
    }
    if (!start) return false;
    const char* val = start + std::strlen(search);
    while (*val == ' ') val++;
    // Accept both the JSON literal `true` and a numeric `1` — deviceModels.json / the
    // catalog fan-out historically wrote 0/1 for flags that are now Bool controls
    // (e.g. ethClockExtIn), and some HTTP clients send 1/0; treat either as true.
    return std::strncmp(val, "true", 4) == 0 || *val == '1';
}

// --- Recursive reader -------------------------------------------------------
//
// A standard recursive-descent parser into a fixed node arena. parse() copies the
// input into the document's own buffer (so strings can be NUL-terminated in place,
// un-escaped) and links nodes by index — no pointers into caller memory, no heap.

// kMaxDepth bounds recursion for the ESP32 task stack (~3.5-8 KB); a deeply-nested document can't
// blow the stack. 64 is far deeper than anything we emit (array -> object -> value is depth 3) yet
// still a hard guard against a pathological input. There is NO node-count or text-length cap: the
// text arena and node pool are heap-allocated per parse, sized to the input, and freed when the
// JsonDoc goes out of scope — so a config of any size (many light presets, a wide fixture) parses.
inline constexpr int kMaxDepth = 64;

enum class JsonType : uint8_t { Null, Bool, Int, String, Object, Array };

// One value in the arena. Children form a singly-linked list by index (firstChild ->
// next -> next -> ...), which keeps each node fixed-size with no per-node child array.
// For an object member, `key` points into the doc buffer (the member name); the member's
// value is the node itself.
struct JsonNode {
    JsonType type = JsonType::Null;
    const char* key = nullptr;    // member name when this node is an object member, else nullptr
    const char* str = nullptr;    // string value (points into doc buffer) when type == String
    long intValue = 0;            // numeric value when type == Int; 0/1 mirror for Bool
    int firstChild = -1;          // index of first child node, or -1
    int next = -1;                // index of next sibling, or -1
};

// The parsed document: owns the text buffer and node arena, both HEAP-allocated by parse() and
// freed here. `buf` is sized to the input; `nodes` grows (realloc-doubling) as the parser allocates
// — nodes are addressed by INDEX (firstChild/next are ints), so a realloc that moves the block never
// dangles. No node-count or length cap. Non-copyable (it owns two heap blocks); a caller keeps it
// alive while walking. Off the hot path (boot load / control writes), so a transient alloc is fine.
struct JsonDoc {
    char*     buf = nullptr;     // heap copy of the input (mutable — un-escaping rewrites in place)
    JsonNode* nodes = nullptr;   // heap node pool, grown by ensureNode()
    int       cap = 0;           // allocated node slots
    int       count = 0;         // used node slots
    int       root = -1;

    JsonDoc() = default;
    ~JsonDoc() { std::free(buf); std::free(nodes); }
    JsonDoc(const JsonDoc&) = delete;
    JsonDoc& operator=(const JsonDoc&) = delete;

    bool valid() const { return root >= 0; }
    const JsonNode* node(int i) const { return (i >= 0 && i < count) ? &nodes[i] : nullptr; }
    const JsonNode* rootNode() const { return node(root); }

    // Grow the node pool if full; returns false on allocation failure. Called by the parser's
    // alloc(). Doubling keeps total reallocations logarithmic. Indices stay valid across the move.
    bool ensureNode() {
        if (count < cap) return true;
        int newCap = cap ? cap * 2 : 32;
        auto* grown = static_cast<JsonNode*>(std::realloc(nodes, static_cast<size_t>(newCap) * sizeof(JsonNode)));
        if (!grown) return false;
        nodes = grown;
        cap = newCap;
        return true;
    }
};

namespace detail {

// Parser cursor over the doc's own (mutable) buffer. Un-escaping rewrites string bytes in
// place, so the buffer doubles as scratch — a parsed string is NUL-terminated where its
// closing quote was, and `str` points at its first byte.
struct JsonParser {
    JsonDoc& doc;
    char* p;        // current position in doc.buf
    bool ok = true;

    explicit JsonParser(JsonDoc& d) : doc(d), p(d.buf) {}

    void skipWs() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }

    int alloc() {
        if (!doc.ensureNode()) { ok = false; return -1; }   // grows the heap pool; false = OOM
        const int i = doc.count++;
        doc.nodes[i] = JsonNode{};   // realloc doesn't construct — reset the freshly-used slot
        return i;
    }

    // Parse a JSON string literal: assumes *p == '"'. Un-escapes in place and NUL-terminates,
    // returning a pointer to the first byte. Handles \" \\ \/ \n \r \t \b \f; \uXXXX is passed
    // through as raw bytes (no decode — we never emit it). Returns nullptr on unterminated input.
    char* parseStringLiteral() {
        p++;                       // opening quote
        char* out = p;             // write cursor (<= read cursor, so in-place is safe)
        char* start = out;
        while (*p) {
            char c = *p++;
            if (c == '"') { *out = 0; return start; }
            if (c == '\\') {
                char e = *p++;
                switch (e) {
                    case '"':  c = '"';  break;
                    case '\\': c = '\\'; break;
                    case '/':  c = '/';  break;
                    case 'n':  c = '\n'; break;
                    case 'r':  c = '\r'; break;
                    case 't':  c = '\t'; break;
                    case 'b':  c = '\b'; break;
                    case 'f':  c = '\f'; break;
                    case 0:    ok = false; return nullptr;  // trailing backslash, truncated
                    default:   c = e;    break;             // unknown escape (incl. \u) — keep byte
                }
            }
            *out++ = c;
        }
        ok = false;                // ran off the end without a closing quote
        return nullptr;
    }

    int parseValue(int depth) {
        if (!ok) return -1;
        if (depth >= kMaxDepth) { ok = false; return -1; }
        skipWs();
        switch (*p) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': {
                int idx = alloc();
                char* s = parseStringLiteral();
                if (!ok || idx < 0) return -1;
                doc.nodes[idx].type = JsonType::String;
                doc.nodes[idx].str = s;
                return idx;
            }
            case 't':
            case 'f': {
                bool isTrue = (*p == 't');
                const char* lit = isTrue ? "true" : "false";
                size_t n = std::strlen(lit);
                if (std::strncmp(p, lit, n) != 0) { ok = false; return -1; }
                p += n;
                int idx = alloc();
                if (idx < 0) return -1;
                doc.nodes[idx].type = JsonType::Bool;
                doc.nodes[idx].intValue = isTrue ? 1 : 0;
                return idx;
            }
            case 'n': {
                if (std::strncmp(p, "null", 4) != 0) { ok = false; return -1; }
                p += 4;
                int idx = alloc();
                if (idx < 0) return -1;
                doc.nodes[idx].type = JsonType::Null;
                return idx;
            }
            default: {
                // Number. Integer-only model: read the optional sign + digits, then
                // discard any fractional/exponent tail (truncate) — we never persist floats.
                if (*p != '-' && (*p < '0' || *p > '9')) { ok = false; return -1; }
                char* endp = nullptr;
                long v = std::strtol(p, &endp, 10);
                if (endp == p) { ok = false; return -1; }
                p = endp;
                // Skip a well-formed fractional/exponent tail (we keep only the integer
                // part — never persist floats). Precise so we stop at the number's real
                // end and don't swallow a following token: `1-2` reads `1` then leaves
                // `-2`, not one run. Fraction: '.' then digits. Exponent: e/E, optional
                // sign, then digits.
                auto digits = [&] { while (*p >= '0' && *p <= '9') p++; };
                if (*p == '.') { p++; digits(); }
                if (*p == 'e' || *p == 'E') {
                    char* e = p++;
                    if (*p == '+' || *p == '-') p++;
                    if (*p >= '0' && *p <= '9') digits();
                    else p = e;     // bare 'e' with no exponent digits — not part of the number
                }
                int idx = alloc();
                if (idx < 0) return -1;
                doc.nodes[idx].type = JsonType::Int;
                doc.nodes[idx].intValue = v;
                return idx;
            }
        }
    }

    int parseObject(int depth) {
        int self = alloc();
        if (self < 0) return -1;
        doc.nodes[self].type = JsonType::Object;
        p++;                       // '{'
        skipWs();
        int prev = -1;
        if (*p == '}') { p++; return self; }
        while (ok) {
            skipWs();
            if (*p != '"') { ok = false; return -1; }
            char* key = parseStringLiteral();
            if (!ok) return -1;
            skipWs();
            if (*p != ':') { ok = false; return -1; }
            p++;
            int child = parseValue(depth + 1);
            if (!ok || child < 0) return -1;
            doc.nodes[child].key = key;
            if (prev < 0) doc.nodes[self].firstChild = child;
            else          doc.nodes[prev].next = child;
            prev = child;
            skipWs();
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; return self; }
            ok = false; return -1;
        }
        return -1;
    }

    int parseArray(int depth) {
        int self = alloc();
        if (self < 0) return -1;
        doc.nodes[self].type = JsonType::Array;
        p++;                       // '['
        skipWs();
        int prev = -1;
        if (*p == ']') { p++; return self; }
        while (ok) {
            int child = parseValue(depth + 1);
            if (!ok || child < 0) return -1;
            if (prev < 0) doc.nodes[self].firstChild = child;
            else          doc.nodes[prev].next = child;
            prev = child;
            skipWs();
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return self; }
            ok = false; return -1;
        }
        return -1;
    }
};

}  // namespace detail

// Parse `json` into `out`. Returns true on success (out.root is the top value), false on
// any malformed, truncated, oversized, or too-deep input — out is left invalid (root == -1).
// Safe on a null pointer and the empty string. Trailing whitespace is allowed; trailing
// non-whitespace garbage (e.g. "}{][") fails.
inline bool parse(const char* json, JsonDoc& out) {
    out.count = 0;
    out.root = -1;
    if (!json) return false;
    size_t len = std::strlen(json);
    if (len == 0) return false;
    // Heap-copy the input, sized exactly to it — no length cap. The parser un-escapes strings in
    // place, so this mutable copy doubles as the string arena; freed by ~JsonDoc.
    std::free(out.buf);
    out.buf = static_cast<char*>(std::malloc(len + 1));
    if (!out.buf) return false;
    std::memcpy(out.buf, json, len + 1);

    detail::JsonParser parser(out);
    int root = parser.parseValue(0);
    if (!parser.ok || root < 0) return false;
    parser.skipWs();
    if (*parser.p != 0) return false;   // trailing garbage after the top-level value
    out.root = root;
    return true;
}

// --- Navigation -------------------------------------------------------------
// All accessors are null-safe and bounds-safe: pass a node from doc.node(...) (or nullptr),
// get back a child node / safe default. They never crash on a wrong-typed or missing node.

// Member of an object by key, or nullptr if `obj` is not an object / has no such member.
inline const JsonNode* member(const JsonDoc& doc, const JsonNode* obj, const char* key) {
    if (!obj || obj->type != JsonType::Object || !key) return nullptr;
    for (int i = obj->firstChild; i >= 0;) {
        const JsonNode* n = doc.node(i);
        if (!n) break;
        if (n->key && std::strcmp(n->key, key) == 0) return n;
        i = n->next;
    }
    return nullptr;
}

// Number of elements in an array (0 if `arr` is not an array).
inline int arraySize(const JsonDoc& doc, const JsonNode* arr) {
    if (!arr || arr->type != JsonType::Array) return 0;
    int count = 0;
    for (int i = arr->firstChild; i >= 0;) {
        const JsonNode* n = doc.node(i);
        if (!n) break;
        count++;
        i = n->next;
    }
    return count;
}

// Element `index` of an array, or nullptr if out of range / not an array.
inline const JsonNode* element(const JsonDoc& doc, const JsonNode* arr, int index) {
    if (!arr || arr->type != JsonType::Array || index < 0) return nullptr;
    int at = 0;
    for (int i = arr->firstChild; i >= 0;) {
        const JsonNode* n = doc.node(i);
        if (!n) break;
        if (at == index) return n;
        at++;
        i = n->next;
    }
    return nullptr;
}

// Read a node as a string into `out` (always NUL-terminated). Empty string for a non-string
// node or null node. Returns true when a string value was copied.
inline bool readString(const JsonNode* n, char* out, size_t maxLen) {
    if (!out || maxLen == 0) return false;
    out[0] = 0;
    if (!n || n->type != JsonType::String || !n->str) return false;
    std::strncpy(out, n->str, maxLen - 1);
    out[maxLen - 1] = 0;
    return true;
}

// Read a node as an int. A Bool reads as 0/1; anything else returns `fallback`.
inline long readInt(const JsonNode* n, long fallback = 0) {
    if (!n) return fallback;
    if (n->type == JsonType::Int || n->type == JsonType::Bool) return n->intValue;
    return fallback;
}

// Read a node as a bool. A non-zero Int reads as true; anything non-bool/non-int is `fallback`.
inline bool readBool(const JsonNode* n, bool fallback = false) {
    if (!n) return fallback;
    if (n->type == JsonType::Bool || n->type == JsonType::Int) return n->intValue != 0;
    return fallback;
}

// Parse `json`, find the array at `key`, and call `fn(doc, element)` for each OBJECT
// element. This is the boilerplate every persisted-list restore shares — parse,
// navigate, type-check, iterate, malformed-safety — so it lives here in core; a caller
// (a ListSource's restoreList) supplies only the per-element "read my fields" body and
// stays a few plain lines. `fn` is a template callback (zero-overhead, no std::function
// / heap). Non-object elements are skipped. Returns false on malformed/missing/non-array
// (the caller's list is simply not restored). The JsonDoc lives on this call's stack —
// boot-time load, not the hot path. Recognizable callback-iteration shape.
// The non-template core: parse `json` and navigate to the array under `key`, returning
// the array node (or nullptr on malformed/missing/non-array) along with the shared doc.
// Lives in a .cpp-less inline but in its OWN non-template function so the heavy static
// JsonDoc below is ONE copy in .bss regardless of how many callback types instantiate
// forEachListElement — a per-instantiation static would multiply the ~8 KB doc by the
// number of distinct lists (and "we'll add more lists"). The doc is a function-local
// static (not a stack local: ~8 KB overflows the ESP32 task stack → boot-loop) and is
// safe to share because JSON parsing is strictly serial — boot-time load or a single
// control write, never concurrent (same single-owner-buffer reasoning as
// FilesystemModule::fileBuf_). Returns the doc by out-param so the caller can read fields.
inline const JsonNode* parseListArray(const char* json, const char* key, JsonDoc*& docOut) {
    static JsonDoc doc;
    docOut = &doc;
    if (!parse(json, doc)) return nullptr;
    const JsonNode* arr = member(doc, doc.rootNode(), key);
    if (!arr || arr->type != JsonType::Array) return nullptr;
    return arr;
}

template <typename Fn>
inline bool forEachListElement(const char* json, const char* key, Fn&& fn) {
    JsonDoc* doc = nullptr;
    const JsonNode* arr = parseListArray(json, key, doc);
    if (!arr) return false;
    const int n = arraySize(*doc, arr);
    for (int i = 0; i < n; i++) {
        const JsonNode* el = element(*doc, arr, i);
        if (el && el->type == JsonType::Object) fn(*doc, el);
    }
    return true;
}

} // namespace mm::json
