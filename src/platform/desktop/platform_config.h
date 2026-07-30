#pragma once

// Desktop platform configuration — always uses larger types (PSRAM-like)

#include <cstdint>

// MM_RAMFUNC — RAM-resident code attribute for deadline-bound ISR paths (see the ESP32
// platform_config.h for the rationale). Desktop code always executes from RAM; empty.
#define MM_RAMFUNC

namespace mm::platform {

constexpr bool hasPsram = true;

// Not an ESP32-P4, so the P4-specific seams (Ethernet pin map, co-processor WiFi)
// compile out on desktop. Mirrors the esp32 config, which keeps only isEsp32P4
// (the general isEsp32/isEsp32S3 family flags had no users and were removed).
constexpr bool isEsp32P4 = false;

// RMT channels the host reports. Non-zero for the same reason as the parallel lane counts: the
// desktop build emulates the peripheral so RmtLedDriver actually RUNS here, rather than guarding
// itself off and leaving its encode + channel-assignment logic testable only on hardware. 4 matches
// the S3 / P4 / S31 TX channel count (the classic ESP32 has 8), so the host exercises the tighter
// of the two real constraints.
constexpr uint8_t rmtTxChannels = 4;

// Lane counts the parallel backends report on desktop. NOT zero, deliberately: everything in the
// repo runs on the desktop build — the platform layer just has no hardware behind the call. A
// zero here makes every backend's lanesAvailable() report "not my silicon", so ParallelLedDriver
// idles and its ~2500-line body never executes off-device: not runnable, not unit-testable, and
// invisible to every AST-based check.
//
// 16 is the widest real rig (LightCrafter 16), so the host exercises the same lane-splitting and
// bus-rounding arithmetic the hardware does rather than a degenerate 1-lane path. The bus behind
// them is a heap buffer (platform_desktop.cpp § Parallel-WS2812 buses): the driver encodes real
// WS2812 bit patterns into real memory, and only the DMA hand-off is absent.
constexpr uint8_t lcdLanes = 16;

// hasLcdCam — TRUE on the host, like the lane counts above: the desktop build emulates the
// peripheral rather than declaring itself incapable. Saying false here would leave the pin-expander
// path (a real feature with real config validation) unreachable off-device, which is the same gap
// the zero lane counts used to create. There is no LCD_CAM silicon; there is a memory bus that
// behaves like one.
constexpr bool hasLcdCam = true;
constexpr uint8_t parlioLanes = 16;

// MultiPinLedDriver's lanesAvailable() reads lcdLanes + i2sLanes, so this stays 0 — otherwise the
// i80 backend would claim 32 lanes, which no real chip offers.
constexpr uint8_t i2sLanes = 0;

// No I2S microphone — AudioService guards on this and is inert on desktop. The
// audioFft seam still has a (naive-DFT) desktop implementation so the audio
// band math runs end-to-end in host tests; only live capture is absent.
constexpr bool hasI2sMic = false;

// Audio-codec config type — desktop has no codec (audioCodecInit stubs to true),
// but platform.h declares audioCodecInit(CodecType, const AudioCodecPins&, …) for
// every platform, so the types must exist here too. Mirror the esp32 definitions;
// desktop is always CodecType::None.
enum class CodecType : uint8_t { None = 0, Es8311 = 1 };
struct AudioCodecPins { uint16_t i2cSda; uint16_t i2cScl; uint16_t mclk; uint8_t i2cAddr; };
constexpr CodecType audioCodecType = CodecType::None;
constexpr AudioCodecPins audioCodecPins = { 0, 0, 0, 0 };

// Desktop is not a target of the Ethernet-only firmware profile; it ships
// WiFi stubs and exercises the hasWiFi==true code path for compile coverage.
constexpr bool hasWiFi = true;

// Ethernet PHY config type — desktop has no Ethernet (ethInit() stubs to false),
// but platform.h declares setEthConfig(const EthPinConfig&) for every platform, so
// the type must exist here too. Mirror the esp32 struct; the desktop stub ignores it.
enum EthPhyType { ethNone = 0, ethLan8720 = 1, ethIp101 = 2, ethW5500 = 3 };
struct EthPinConfig {
    int phyType; int phyAddr;
    int mdcGpio; int mdioGpio; int rstGpio; int rmiiClockGpio; bool rmiiClockExtIn;
    int spiMiso; int spiMosi; int spiSck; int spiCs; int spiIrq;
};
// Desktop has no Ethernet — hasEthernet false, the default is ethNone. NetworkModule
// (shared code) `if constexpr (hasEthernet)`s the eth controls off, and seeds its
// members from this default; both must exist here for that shared code to compile.
constexpr bool hasEthernet = false;

// hasNamedNetInterfaces — the host has several NICs and a raw sender must name one ("eth0", "en0").
// True on desktop, false on a microcontroller with a single MAC, where the name would be a control
// that does nothing. Drivers use it to hide the field rather than to choose behaviour.
constexpr bool hasNamedNetInterfaces = true;
// Some-IP-stack flag (WiFi OR Ethernet) — mirrors the esp32 config so shared code
// (WLED audio sync, UDP interop) gates on "has network" uniformly. True on desktop
// via the WiFi stubs (UdpSocket has a desktop implementation).
constexpr bool hasNetwork = hasWiFi || hasEthernet;
// No SPI-Ethernet (W5500) driver on desktop either — NetworkModule's live-reconfigure
// path gates on this, so it must exist on every platform (mirrors the esp32 flag).
constexpr bool hasEthW5500 = false;
constexpr EthPinConfig ethConfigDefault{ ethNone, 0, -1, -1, -1, -1, false, -1, -1, -1, -1, -1 };

// Desktop has no separate WiFi co-processor (the ESP32-P4 + C6 case); the
// coprocessorWifi() read-out and its SystemModule control compile out here.
constexpr bool hasWifiCoprocessor = false;

// OTA writes to an ESP-IDF OTA partition; desktop has none. FirmwareUpdateModule
// + the /api/firmware/url route `if constexpr (hasOta)` to a 501 stub instead.
constexpr bool hasOta = false;

// Improv WiFi reads from UART0; desktop has neither a UART nor a WiFi stack.
// ImprovProvisioningModule's setup() `if constexpr (hasImprov)` skips the
// listener-install on desktop.
constexpr bool hasImprov = false;

} // namespace mm::platform

// MM_MOONLIVE_HAS_HOST_JIT — 1 when this desktop host has BOTH the emit blob AND the general
// assembler (moonlive_asm_host.cpp) available. Both are arm64-only today; x86_64 Windows/Linux/
// Intel-macOS desktops ship without a backend and MoonLive::compile / compileSource fail
// cleanly (scripted modules render dark). Tests and scenarios that presuppose a working JIT
// gate on this macro. Kept in platform_config.h (not core) per the platform-boundary rule:
// no `#if defined(__aarch64__)` outside src/platform/. A #define (not constexpr) so #include-
// side test files can use it in `#if` — CLAUDE.md's `if constexpr` preference is for runtime
// branches inside code, not preprocessor gating around whole TEST_CASEs.
#if defined(__aarch64__)
    #define MM_MOONLIVE_HAS_HOST_JIT 1
#else
    #define MM_MOONLIVE_HAS_HOST_JIT 0
#endif

// MM_LINKS_ALL_LED_DRIVERS — 1 where the build links every LED driver regardless of silicon.
// The desktop host does: the repo's rule is that everything runs there, with the platform layer
// simply having no hardware behind the call. A driver excluded from the host binary cannot be
// unit-tested, cannot be seen by any AST-based check, and only ever runs where it is hardest to
// debug. A #define (not constexpr) because it gates `#include`s in main.cpp, which `if constexpr`
// cannot do — and it lives here, not in core, per the platform-boundary rule.
#define MM_LINKS_ALL_LED_DRIVERS 1
