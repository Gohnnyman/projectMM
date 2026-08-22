cmake_minimum_required(VERSION 3.20)
project(projectMM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit build/<host>/compile_commands.json — the compilation database clangd reads to give
# real diagnostics in the editor (it needs the actual include paths and flags, not guesses).
# On by default because it costs nothing, is ignored by every consumer that doesn't want it,
# and a missing database is the single most common reason clangd reports phantom errors.
# `.clangd` at the repo root points at it; see docs/coding-standards.md § Editor setup.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warning flags differ between GCC/Clang and MSVC. Gate by compiler so the
# desktop build runs cleanly on Windows runners (no -W flags on MSVC) and on
# macOS/Linux (no /W flags on Clang/GCC).
if(MSVC)
    # /W4 /WX matches the GCC/Clang `-Wall -Wextra -Werror` discipline. A few
    # MSVC-only warnings have no GCC equivalent at -Wall -Wextra and are
    # suppressed below rather than fought at every call site:
    #   C4702 — "unreachable code" inside `if constexpr` early-return branches
    #           (a well-known MSVC quirk: it analyses the discarded branch as
    #           live and flags everything after `return;`). GCC understands the
    #           constexpr discard and doesn't warn.
    #   C4244 / C4267 — implicit narrowing (int→smaller, size_t→smaller). GCC
    #           only warns under -Wconversion, which is not part of -Wall
    #           -Wextra. The codebase uses explicit static_cast at the sites
    #           where loss-of-data is the real concern; suppressing here brings
    #           MSVC noise level back in line with GCC.
    #   C5285 — vendored doctest.h (v2.4.11) forward-declares `std::tuple` as a
    #           libstdc++/libc++ portability workaround; the VS 2026 MSVC STL
    #           newly flags that as "specializing a standard template". It's in
    #           third-party header code we don't own and GCC/Clang never warn.
    add_compile_options(/W4 /WX /wd4702 /wd4244 /wd4267 /wd5285)
    # MSVC marks the standard C library (strcpy, sprintf, fopen, localtime, …)
    # as deprecated and emits C4996; /WX makes that fatal. Silence the family —
    # the code uses these standard functions deliberately and consistently with
    # the POSIX side, and the "secure" `_s` variants are a Microsoft extension.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
                            _WINSOCK_DEPRECATED_NO_WARNINGS)
    # Static MSVC runtime so the Windows binary doesn't need vcredist.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # Tier zero on top of -Wall -Wextra: five warnings that catch real defects the base set
    # misses. -Wdouble-promotion is the one that earns its place here specifically — the Xtensa
    # has no FPU, so an accidental float->double promotion is a silent softfloat call in the
    # render path, and it enforces the integer-math rule the coding standards ask for.
    # (GCC/Clang only; the MSVC branch above uses /W4 /WX, which has no direct equivalents.)
    add_compile_options(-Wall -Wextra -Werror
                        -Wshadow -Wnon-virtual-dtor -Wdouble-promotion
                        -Wimplicit-fallthrough -Wnull-dereference)

    # Hot-path discipline, enforced by the compiler. MM_NONBLOCKING marks tick/tick20ms/tick1s
    # (platform.h); -Wfunction-effects then checks TRANSITIVELY that nothing they reach
    # allocates or blocks, through the whole call graph. Clang 20+ only; the flag does not
    # exist on GCC, so the ESP32 build gets no hot-path check — src/platform/esp32/ is
    # analysed only where the desktop build reaches it (docs/testing.md § Static analysis).
    # Probe for the flag instead of inferring it from a version: AppleClang reports its own
    # version line (the CI macos-14 runner reads >= 20 while predating the warning), so a version
    # comparison enables -Wfunction-effects on toolchains that reject it — and it is -Werror.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wfunction-effects MM_HAS_WFUNCTION_EFFECTS)
    if(MM_HAS_WFUNCTION_EFFECTS)
        # -Wno-error is PERMANENT, not a stepping stone: this is a report, not a gate. The
        # findings are real and frozen in docs/metrics/hotpath-baseline.txt as architecture
        # work; failing the build on them would block every other gate, and a new one may be
        # legitimate (a driver that must wait on hardware). The clang-hotpath card reports
        # what is NEW; a human judges it.
        add_compile_options(-Wfunction-effects -Wno-error=function-effects)
    endif()
endif()

# MM_MOONLIVE_NO_HOST_JIT builds this machine the way a desktop with NO MoonLive backend sees the
# code. Every x86-64 desktop is one (Windows, Linux, Intel macOS ship no backend), so a test that
# wrongly presumes a compile succeeds passes on the arm64 bench and fails only once CI runs it.
# A compile DEFINITION rather than a CMAKE_CXX_FLAGS entry: this file configures warnings through
# add_compile_options, which does not consume that variable, so a flag passed there would reach the
# cache and never the compiler.
option(MM_MOONLIVE_NO_HOST_JIT "Build as a desktop with no MoonLive backend" OFF)
if(MM_MOONLIVE_NO_HOST_JIT)
    add_compile_definitions(MM_MOONLIVE_FORCE_NO_HOST_JIT=1)
endif()

# `uv` is the project's Python launcher (see CLAUDE.md / moondeck/MoonDeck.md).
# The build invokes Python helpers for build_info.h generation and UI embedding;
# resolving them through `uv run python …` keeps Windows (where `python3` isn't
# on PATH) on the same path as macOS / Linux without a `find_package(Python3)`
# detour and without per-host scaffolding.
find_program(UV_EXECUTABLE NAMES uv REQUIRED
             HINTS "$ENV{USERPROFILE}/.local/bin" "$ENV{HOME}/.local/bin")

# Core library. Most modules ship header-only (single .h file with implementation
# inline). Core service modules that bridge to the platform (HTTP server,
# filesystem, network, …) ship as .h + .cpp per CLAUDE.md — list those .cpps
# here so both the application and the test binary link them.
add_library(mm_core STATIC
    src/core/Control.cpp
    src/core/JsonUtil.cpp
    src/core/ScratchBuffer.cpp
    src/core/HttpServerModule.cpp
    src/core/FilesystemModule.cpp
    src/core/FileManagerModule.cpp
    src/core/MqttModule.cpp
    src/core/Scheduler.cpp
    src/core/moonlive/MoonLive.cpp
    src/core/moonlive/MoonLiveCompiler.cpp
    src/core/moonlive/MoonLiveSpill.cpp
)
target_include_directories(mm_core PUBLIC src/)
target_link_libraries(mm_core PUBLIC mm_platform)
# `add_dependencies(mm_core ui_embed)` is below, after the ui_embed target is defined.

# Platform library (desktop). moonlive_emit.cpp is the desktop MoonLive backend (host-ISA
# codegen) — it lives here because emitted machine code is platform/ISA-specific.
add_library(mm_platform
    src/platform/desktop/platform_desktop.cpp
    src/platform/desktop/moonlive_emit_noarch.cpp
    src/platform/desktop/moonlive_asm_noarch.cpp
    src/platform/desktop/moonlive_emit_arm64.cpp
    src/platform/desktop/moonlive_emit_x86_64.cpp
    src/platform/desktop/moonlive_asm_arm64.cpp
    src/platform/desktop/moonlive_asm_x86_64.cpp
)
target_include_directories(mm_platform PUBLIC src/ src/platform/desktop/)
# Winsock for the desktop socket surface; IPHLPAPI for real Ethernet link state and negotiated
# speed (PanelCardDriver warns below gigabit, and a ColorLight card tears rather than erroring).
# Npcap is deliberately NOT linked: platform_desktop.cpp resolves wpcap.dll at run time so the
# build needs no SDK and the binary runs without it.
target_link_libraries(mm_platform PUBLIC $<$<PLATFORM_ID:Windows>:ws2_32> $<$<PLATFORM_ID:Windows>:iphlpapi>)

# Generate build_info.h from library.json + git (carries version, build id, build date, board name).
# ALWAYS out-of-date on purpose — see the same rule in esp32/main/CMakeLists.txt for why: MM_BUILD_ID
# must track the git hash on every build, and pinning this to library.json's mtime is what made the
# reported build stamp go stale, which reads as a failed flash and sends debugging at the wrong binary. The generator
# rewrites the header only when its content changes, so an unchanged tree triggers no rebuild.
add_custom_target(build_info_gen ALL
    COMMAND ${UV_EXECUTABLE} run python ${CMAKE_SOURCE_DIR}/moondeck/build/generate_build_info.py
    BYPRODUCTS ${CMAKE_SOURCE_DIR}/src/core/build_info.h
    COMMENT "Generating build_info.h (git build id)"
    VERBATIM
)

# Embed UI files as C arrays. Pass UV_EXECUTABLE through as a single value
# (no semicolons on the command line — those would either get split by `make`
# on POSIX hosts or land as literals in the value on MSBuild) and let
# embed_ui.cmake assemble the `<uv> run python` list internally.
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h
    COMMAND ${CMAKE_COMMAND} -DUI_DIR=${CMAKE_SOURCE_DIR}/src/ui -DOUT=${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h -DUV_EXECUTABLE=${UV_EXECUTABLE} -P ${CMAKE_SOURCE_DIR}/src/ui/embed_ui.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/index.html ${CMAKE_SOURCE_DIR}/src/ui/app.js ${CMAKE_SOURCE_DIR}/src/ui/style.css ${CMAKE_SOURCE_DIR}/src/ui/install-picker.js ${CMAKE_SOURCE_DIR}/src/ui/preview3d.js ${CMAKE_SOURCE_DIR}/src/ui/moonlight-logo.png ${CMAKE_SOURCE_DIR}/src/ui/embed_ui.cmake
    COMMENT "Embedding UI files"
)
add_custom_target(ui_embed DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h)

# mm_core's HttpServerModule.cpp consumes ui_embedded.h and SystemModule.h
# (a mm_core public header) consumes build_info.h. Anything that links mm_core
# transitively includes those headers, so both generated files must exist
# before any mm_core compile unit starts. CI's parallel clean build exposes
# the race (test/ compiles SystemModule.h while build_info_gen is still
# running); wiring the dep here makes the ordering explicit on every consumer.
add_dependencies(mm_core ui_embed build_info_gen)

# Application
add_executable(projectMM src/main.cpp src/platform/desktop/main_desktop.cpp)
target_link_libraries(projectMM PRIVATE mm_core mm_platform)
add_dependencies(projectMM ui_embed build_info_gen)

# Tests
enable_testing()
add_subdirectory(test)
