// @module MoonLiveLayout
#include "doctest.h"
#include "light/moonlive/MoonLiveLayout.h"
#include <cstring>
using namespace mm;
TEST_CASE("probeW: changing width repeatedly") {
    MoonLiveLayout l;
    l.defineControls();
    l.setSource("uint8_t width = 8; // @control 1..64\n"
                "uint8_t height = 8; // @control 1..64\n"
                "for (i = 0; i < width; i = i + 1) {\n"
                "for (j = 0; j < height; j = j + 1) {\n addLight(i, j, 0); \n}\n}");
    l.prepare();
    std::printf("  initial lights=%u\n", (unsigned)l.lightCount());
    // Does an unchanged source recompile? codeCap changes address each time if so.

    const auto& cs = l.controls();
    for (uint8_t v : {10, 20, 32, 64, 1, 16}) {
        for (uint8_t i = 0; i < cs.count(); i++)
            if (cs[i].name && std::strcmp(cs[i].name, "width") == 0)
                *static_cast<uint8_t*>(cs[i].ptr) = v;
        l.prepare();
        // Ask several times, as the pipeline does (count, then walk, then count).
        auto a = l.lightCount(); auto b = l.lightCount(); auto c = l.lightCount();
        std::printf("  width=%3u -> %u/%u/%u  dynamicBytes=%u\n", v,
                    (unsigned)a,(unsigned)b,(unsigned)c,(unsigned)l.dynamicBytes());
    }
}
