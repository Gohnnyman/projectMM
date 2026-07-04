// @module FileManagerModule

// Drives the file-manager create/delete ops against the real platform::fs* seam, isolated to a temp
// dir via fsSetRoot (the FilesystemModule-test pattern). Browsing is UI-side over the /api/dir HTTP
// endpoint (covered in the HttpServerModule tests) — this pins the module's own surface: the two
// path-targeted ops and their robustness (bad path / non-empty-dir / '..' traversal never crash).

#include "doctest.h"
#include "core/FileManagerModule.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace mm;

namespace {

// A file-manager on a fresh temp filesystem root. Seeds a known layout the ops act against.
struct Rig {
    char root[256];
    FileManagerModule fm;
    Rig() {
        // A monotonic per-process counter, not millis() — several Rigs construct within the same
        // millisecond in one test run, so a time-based name could collide.
        static unsigned counter = 0;
        std::snprintf(root, sizeof(root), "/tmp/mm_fm_test_%u", counter++);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(std::string(root) + "/.config");
        { FILE* f = std::fopen((std::string(root) + "/.config/Drivers.json").c_str(), "w");
          std::fputs("{\"brightness\":20}", f); std::fclose(f); }
        { FILE* f = std::fopen((std::string(root) + "/readme.txt").c_str(), "w");
          std::fputs("hello", f); std::fclose(f); }
        platform::fsSetRoot(root);
        fm.setTypeName("FileManagerModule");
        fm.onBuildControls();
        fm.setup();
    }
    ~Rig() { platform::fsSetRoot("."); std::filesystem::remove_all(root); }

    // Set the `path` op-target control, then press a button (new folder / delete).
    void op(const char* button, const char* path) {
        auto& ctrls = fm.controls();
        for (uint8_t i = 0; i < ctrls.count(); i++)
            if (std::strcmp(ctrls[i].name, "path") == 0) {
                std::strncpy(static_cast<char*>(ctrls[i].ptr), path, 127);
                static_cast<char*>(ctrls[i].ptr)[127] = 0; break;
            }
        fm.onUpdate(button);
    }
    bool onDisk(const char* rel) const {
        return std::filesystem::exists(std::string(root) + rel);
    }
};

}  // namespace

TEST_CASE("FileManager: new folder creates a dir at the target path; delete removes it") {
    Rig r;
    r.op("new folder", "/newdir");
    CHECK(r.onDisk("/newdir"));
    CHECK(std::strstr(r.fm.status(), "created") != nullptr);
    r.op("delete", "/newdir");
    CHECK(!r.onDisk("/newdir"));
    CHECK(std::strstr(r.fm.status(), "deleted") != nullptr);
}

TEST_CASE("FileManager: new folder nested under an existing dir") {
    Rig r;
    r.op("new folder", "/.config/sub");
    CHECK(r.onDisk("/.config/sub"));
}

TEST_CASE("FileManager: delete of a non-empty folder is rejected, not a crash") {
    Rig r;
    r.op("delete", "/.config");        // .config holds Drivers.json → not empty
    CHECK(r.onDisk("/.config"));       // still there
    CHECK(std::strstr(r.fm.status(), "not empty") != nullptr);
}

TEST_CASE("FileManager: delete removes a file too") {
    Rig r;
    r.op("delete", "/readme.txt");
    CHECK(!r.onDisk("/readme.txt"));
}

TEST_CASE("FileManager: a '..' traversal in the path is refused") {
    Rig r;
    r.op("new folder", "/../escape");
    CHECK(std::strstr(r.fm.status(), "invalid") != nullptr);
    CHECK(!std::filesystem::exists(std::string(r.root) + "/../escape"));   // nothing outside root
}

TEST_CASE("FileManager: an empty target path is refused, not a crash") {
    Rig r;
    r.op("delete", "");
    CHECK(std::strstr(r.fm.status(), "invalid") != nullptr);
}

// The /api/file upload streams the body to fsWriteStream (any size, binary-safe) and downloads via
// fsReadAt (positional, chunked). These pin the seam primitives that path relies on. (The HTTP
// framing itself needs a socket fixture — backlogged; here we exercise the platform contracts.)

// A multi-chunk source (larger than the src callback's cap) with a NUL writes in full and reads back
// byte-for-byte — the streamed-upload contract.
namespace {
struct SpanSrc { const char* p; size_t left; };
size_t spanPull(char* out, size_t cap, void* user) {
    auto* s = static_cast<SpanSrc*>(user);
    const size_t n = s->left < cap ? s->left : cap;
    std::memcpy(out, s->p, n);
    s->p += n; s->left -= n;
    return n;   // 0 when exhausted
}
}  // namespace

TEST_CASE("FileManager: fsWriteStream writes a multi-chunk NUL-containing payload in full") {
    Rig r;
    // 3000 bytes (spans several 1KB internal chunks) with a NUL planted mid-stream.
    std::string payload(3000, 'X');
    payload[1500] = '\0';
    payload[2999] = 'Z';
    SpanSrc src{payload.data(), payload.size()};
    REQUIRE(platform::fsWriteStream("/blob.bin", &spanPull, &src));
    CHECK(platform::fsSize("/blob.bin") == static_cast<long>(payload.size()));

    // Read it back positionally across a chunk boundary — the streamed-download contract.
    char win[64] = {};
    const int got = platform::fsReadAt("/blob.bin", 1480, win, 40);   // straddles the NUL at 1500
    CHECK(got == 40);
    CHECK(win[20] == '\0');                                           // the planted NUL, offset 1500
    CHECK(std::memcmp(win, payload.data() + 1480, 40) == 0);
}

// A source that reports a short read (mid-stream failure) still commits atomically — here a clean
// end just yields the bytes delivered; there's no partial/torn file (temp → rename).
TEST_CASE("FileManager: fsWriteStream commits atomically (no torn file)") {
    Rig r;
    const char data[] = {'a', 'b', '\0', 'c', 'd'};
    SpanSrc src{data, sizeof(data)};
    REQUIRE(platform::fsWriteStream("/atomic.bin", &spanPull, &src));
    char back[16] = {};
    const int n = platform::fsRead("/atomic.bin", back, sizeof(back));
    CHECK(n == static_cast<int>(sizeof(data)));
    CHECK(std::memcmp(back, data, sizeof(data)) == 0);
    // No leftover temp file beside it.
    CHECK(!std::filesystem::exists(std::string(r.root) + "/atomic.bin.tmp"));
}
