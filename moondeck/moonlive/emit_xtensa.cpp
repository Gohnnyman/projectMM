#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
// The Xtensa backend is `#if defined(__XTENSA__)`, so on this host it compiles to nothing — there
// is no library to link against, which is the whole point: this tool runs the REAL Xtensa emitter on
// the development machine so an encoding can be read without flashing a board. Defining the macro
// and including the sources is what makes that possible; the target build never sees this file, so
// there is still exactly one backend definition in the firmware.
#define __XTENSA__ 1
#include "platform/esp32/moonlive_asm_xtensa.h"
#include "platform/esp32/moonlive_asm_xtensa.cpp"
// The lowerer body, with the emit seam it expects.
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/MoonLiveBuiltins.h"
namespace mm::moonlive {
size_t lowerToBytes(const IrProgram& ir, uint8_t* out, size_t cap);
}
#include "platform/esp32/moonlive_lower_xtensa.cpp"
#undef __XTENSA__

#include "core/moonlive/MoonLiveCompiler.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
using namespace mm;
int main(int argc, char** argv) {
    const char* src = argc > 1 ? argv[1] : "for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }";
    uint8_t buf[4096];
    auto r = moonlive::compileSource(src, moonlive::lightBuiltins(), buf, sizeof(buf));
    if (!r.ok) { printf("compile failed: %s\n", r.error); return 1; }
    printf("# %s\n# %zu bytes\n", src, r.len);
    for (size_t i = 0; i < r.len; i++) printf("%02x%s", buf[i], (i % 16 == 15) ? "\n" : " ");
    printf("\n");
    return 0;
}
