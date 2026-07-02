# Core UI modules

The user-facing core services — the machinery that runs a show, each configurable in the web UI. Every row links to its generated technical page (the full API, from the `.h`) and its tests. Cross-cutting rationale that no single `.h` owns lives in the prose sections below the table.

### System

The device's identity and vitals — name (behind mDNS `<name>.local`, the SoftAP SSID, the DHCP hostname), uptime, heap, and per-module footprint reporting. Hosts the Audio and I2C-scan peripherals.

- `deviceName` — the device identity behind mDNS `<name>.local`, the SoftAP SSID, and the DHCP hostname.
- `deviceModel` — the board model (drives the installer catalog entry).
- read-only vitals — `uptime`, `fps`, `heap`, `psram`, `flash`, `chip`, and per-module footprint.

Detail: [technical](../moxygen/SystemModule.md)

[Tests](../../../tests/unit-tests.md#systemmodule)

### Network

WiFi / Ethernet connectivity, static-IP configuration, RSSI and TX-power reporting. Brings the device onto the LAN before the HTTP and WebSocket servers start.

- `mode` — WiFi / Ethernet / off.
- `ssid` / `password` — WiFi credentials.
- `mDNS` — the `<name>.local` hostname.
- `addressing` — DHCP or static; static exposes IP / gateway / subnet / DNS fields.
- `ethType` / `ethPhyAddr` / `ethRstGpio` / … — Ethernet PHY configuration.
- read-only — `rssi` (dBm), `txPower` (dBm).

Detail: [technical](../moxygen/NetworkModule.md)

[Tests](../../../tests/unit-tests.md#networkmodule)

### Improv provisioning

Serial/BLE Improv Wi-Fi provisioning — the web installer hands credentials to a fresh device over this protocol during the flash-and-connect flow.

- `provision_status` — read-only provisioning state.

Detail: [technical](../moxygen/ImprovProvisioningModule.md)

### Devices

Discovers and lists other projectMM devices on the LAN (the `devices` List control), each row expanding to a detail panel; persists the last-known list across reboot.

- `devices` — a List control of discovered devices; each row expands to a detail panel. Persistable.

Detail: [technical](../moxygen/DevicesModule.md)

[Tests](../../../tests/unit-tests.md#devicesmodule)

### Firmware update

Over-the-air firmware flashing — the one operation that swaps the binary and needs a power cycle (every *config* change applies live; a firmware OTA does not).

- `firmware` — the OTA image to flash.
- read-only — `version`, `build`, `firmwarePartition`, `update_pct` (progress).

Detail: [technical](../moxygen/FirmwareUpdateModule.md)

[Tests](../../../tests/unit-tests.md#firmwareupdatemodule)

### Filesystem

Persists control values as JSON and restores them on boot, overlaying loaded values through each control's pointer during `onBuildControls()`. The home of the no-reboot live-reconfiguration behaviour.

- read-only — `lastSaved`, `filesystem` (usage).

Detail: [technical](../moxygen/FilesystemModule.md)

[Tests](../../../tests/unit-tests.md#filesystemmodule)

### Audio

A System peripheral (added by the user, not auto-wired): an I²S microphone feeding the FFT that audio-reactive effects consume via `AudioModule::latestFrame()`. Idle until real GPIOs are entered.

- `wsPin` / `sdPin` / `sckPin` — the I²S microphone GPIOs (unset until entered).
- `sampleRate` — mic sample rate.
- `simulate` — feed a synthetic signal instead of the mic (for testing without hardware).
- read-only — `level` (RMS), `peakHz`.

Detail: [technical](../moxygen/AudioModule.md)

[Tests](../../../tests/unit-tests.md#audiomodule)

### I2C scan

A System peripheral that probes the I²C bus (default GPIO21/22) on a button press and reports the addresses found.

- `sda` / `scl` — the bus GPIOs (default GPIO21/22).
- `scan` — a button; press to probe the bus now.
- read-only — `result` (addresses found).

Detail: [technical](../moxygen/I2cScanModule.md)
