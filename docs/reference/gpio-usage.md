# GPIO usage per MCU — hardware reference

Which GPIOs are safe to wire a mic, an LED strand, or an Ethernet PHY to on each MCU projectMM supports, so a bench build picks a pin from here instead of re-scraping the datasheet. This is **chip**-level (which GPIOs the silicon reserves or role-assigns); a specific product's *board* wiring is its catalog entry in [`web-installer/deviceModels.json`](../../web-installer/deviceModels.json), and the one fully-mapped board is the [ESP32-S31 coreboard](esp32-s31-coreboard.md).

Each chip lists two kinds of pin to avoid:

- **Reserved** — wired to flash / PSRAM / native USB on-silicon. Using one crashes or corrupts the device. **Never** route peripheral I/O here.
- **Role-conflict** — a usable GPIO that also carries a default role (JTAG, the UART0 console, a boot strap). Usable for I/O, but you give up that role: no hardware JTAG debug, no serial console, or a boot-mode hazard if the pin is driven during reset. Fine for a peripheral you're willing to trade the role for.

Everything else is free for user I/O. Since the ESP32 I²S / RMT / LED peripherals route through the GPIO matrix, any free GPIO works for a mic or an LED lane; there is no fixed pinout to match.

**Sources:** the per-chip Espressif datasheet (linked in each section). When picking defaults for a new chip, read its strapping-pin and flash/PSRAM tables from the datasheet first, not from a tutorial.

## ESP32 (classic)

[Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf). 34 GPIOs, but several are input-only or strapping.

| Avoid | GPIOs | Why |
|-------|-------|-----|
| Reserved | **6-11** | SPI flash (the on-package flash bus). Always off-limits. |
| Reserved | **16, 17** | Extra flash/PSRAM bus on WROVER (PSRAM) modules; free on plain WROOM. |
| Role-conflict | **0, 2, 5, 12, 15** | Boot straps. Usable, but a wrong level at reset changes boot mode (GPIO 12 = flash voltage is the dangerous one). Don't drive during boot. |
| Input-only | **34, 35, 36, 39** | No output driver and no internal pull-ups. Fine for a mic **SD/SCK/WS input**, useless for an LED output. |

**Clear for output I/O:** 4, 13, 14, 18, 19, 21-23, 25-27, 32, 33 (plus 16/17 on non-PSRAM). Input-only 34-39 suit mic data lines.

## ESP32-S3

[Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf). 45 GPIOs. The reserved set **depends on the module's PSRAM**, which is the trap.

| Avoid | GPIOs | Why |
|-------|-------|-----|
| Reserved | **26-32** | SPI flash bus. Always off-limits. |
| Reserved (**octal-PSRAM only, the N16R8 / R8 modules**) | **33-37** | Octal PSRAM's SPIIO4-7 + DQS. On an R8 module these are **not** GPIOs; using one corrupts PSRAM. On quad-PSRAM (N8R2) or no-PSRAM parts they are free. Check the module suffix. |
| Role-conflict | **39, 40, 41, 42** | JTAG (MTCK/MTDO/MTDI/MTMS). Usable as GPIO, but you lose hardware JTAG debug. Keep free if you plan to add JTAG. |
| Role-conflict | **19, 20** | Native USB D-/D+ (the USB-C port on DevKitC boards). Usable, but breaks the native-USB console/DFU. |
| Role-conflict | **43, 44** | UART0 TX/RX — the default serial console. Take them and you lose `printf`-over-serial. |
| Role-conflict | **0, 45, 46** | Boot straps. Don't drive at reset. |
| Absent | **22, 23, 24, 25** | These GPIO numbers **do not exist** on the S3 (a numbering gap in the silicon). Setting a pin control to one is invalid — `GPIO_IS_VALID_GPIO` rejects it, and the pin map flags it as an error. |

**Clear for I/O on the N16R8:** 1-18, 21, 47, 48 (mind GPIO 3 is a soft strap; the on-board WS2812 LED sits on **48** on DevKitC-1 rev v1.1 but on **38** on rev v1.0, so avoid whichever your board uses — the bench board is a rev v1.0, LED on 38, leaving 48 free). Example, the bench mic on the N16R8 is **SCK=47 / WS=48 / SD=21** — clear of octal PSRAM, JTAG (39-42 stay free for a future JTAG probe), the boot straps, and (on this rev) the LED, with the mic wires kept away from the Wi-Fi antenna end.
## ESP32-P4

[Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf). 55 GPIOs, RISC-V. The P4 has **no native radio**; WiFi rides an on-board ESP32-C6 over SDIO, so several pins carry that link.

| Avoid | GPIOs | Why |
|-------|-------|-----|
| Reserved | **flash/PSRAM pins per module** | The HP SPI flash + PSRAM bus (module-specific; the Waveshare P4-NANO wires them internally). Off-limits. |
| Role-conflict | **34-38** | Strapping pins (boot mode). Don't drive at reset; the first LED-driver default wrongly landed here (see [lessons.md](../history/lessons.md)). |
| Role-conflict | **37, 38** | UART0 console on the P4-NANO (`CONFIG_ESP_CONSOLE_UART_DEFAULT`) — the runtime `ESP_LOGI` lines come out here, not over USB. |
| Board-wired (P4-NANO) | **Ethernet RMII** 28-31 / 49-52, **C6 SDIO** 14-19 / 54, **I2C** 7-8 | Consumed by the NANO's on-board Ethernet PHY, the C6 WiFi co-processor, and the I2C bus. |

**Clear on the P4-NANO:** 20-27, 32-33, 39-48 (the LED-driver default is `pins="20,21,22,23,24,25,26,27"`). Exact free set is board-specific; the NANO's is the reference. A carrier board changes what's exposed and how — the [MHC-WLED ESP32-P4 shield](mhc-wled-esp32-p4-shield.md) routes every terminal through level shifters / RS-485 transceivers / protected inputs (no bare GPIO), which is why a direct loopback jumper fails on it; its full terminal map is on that page.

## ESP32-S31

[Datasheet](https://documentation.espressif.com/esp32-s31_datasheet_en.pdf). RISC-V, Wi-Fi 6 + on-chip 1 Gbps EMAC; shares the MoonLive RISC-V backend with the P4. The bench board's full schematic pin map (Ethernet RGMII, ES8311 audio codec, SD, USB-host) is on its own page: [ESP32-S31 coreboard](esp32-s31-coreboard.md).

| Avoid | GPIOs | Why |
|-------|-------|-----|
| Reserved | flash/PSRAM per module | On-package flash + PSRAM bus. Off-limits. |
| Board-wired (coreboard) | **RGMII Ethernet** + **ES8311 I2S/I2C** + **SD** + **USB-host** | The Function-CoreBoard-1 uses a large set for its on-board peripherals — see the [coreboard reference](esp32-s31-coreboard.md) for the exact map, since the audio codec (not a bare I²S mic) drives its own pins. |

For the S31 coreboard, take pins from the coreboard reference's "free" set rather than guessing, because so much of the header is committed to on-board peripherals.

## The rule behind the defaults

projectMM leaves peripheral pins **unset** by default and lets the deviceModel catalog fix them only where the *product* wires them (the [Defaults rule](../coding-standards.md#defaults)): a board-soldered PHY or codec defaults its pins; a user-soldered mic or LED strand stays unset so a guess can't drive a pin the user committed elsewhere. This table is what to consult when choosing that user pin, or when writing a new board's catalog entry.
