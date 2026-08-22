# Driving LED panels with a receiving card

You bought a ColorLight receiving card. This page takes you from a box of parts to a lit wall: first on an ESP32, then from a desktop.

> New here? Start with **[Install & first light](../gettingstarted.md)**, then **[How projectMM works](how-projectmm-works.md)**. This page assumes you can find a card and change a control.

---

## 1. What you actually bought

An LED wall is normally driven by two boxes. A **sending card** (a PCI-E board in a PC, or a standalone unit) takes video in and puts a specialised signal on Ethernet. A **receiving card** sits in each cabinet, decodes that signal, and drives the panels over HUB75 ribbon cables. The card you bought is a receiving card.

**projectMM takes the sending card's place.** The board renders the effect and emits the frames itself, so there is no PC in the installation and no sending card to buy.

That has one consequence worth understanding before you wire anything: these frames are **raw Ethernet**, below IP. No address, no port, no DHCP. The cable between your controller and the card is not a network in the usual sense; it is a private link carrying pixel data, and nothing else should be on it.

Currently supported: **ColorLight 5A-75** (5A-75B and 5A-75E use the same wire format).

---

## 2. What you need

| Part | Notes |
|---|---|
| ColorLight 5A-75 receiving card | The one this guide is about |
| HUB75 panels | Any size; you tell projectMM the geometry later |
| 5 V power supply | Sized for the panels: a 64×64 panel at full white can pull 20 A+ |
| Cat5e or Cat6 cable | Controller → the card's **input** port |
| A controller | An ESP32 board (Part 3) or a desktop/Pi (Part 4) |

### The one hardware fact that decides everything

**These cards want a 1 Gbit link.** Not for bandwidth, since a 256×256 wall at 40 fps is only about 65 Mbit/s, which 100 Mbit would carry comfortably. It is about **wire time**.

The cards have no buffering and no flow control. They latch the image when the sync frame arrives, so an entire frame has to land inside the gap between frames. At gigabit a 256×256 frame is ~1.6 ms on the wire; at 100 Mbit the identical bytes take ~16 ms, which overruns the budget and breaks the timing the latch depends on.

The failure mode is the confusing part: **nothing errors**. The link is up, frames go out, and the panels tear, show wrong rows, or never latch. That is why projectMM reads the *negotiated* speed and warns you, rather than letting a slow link look like a format bug. It still sends, since a small wall on 100 Mbit is often fine, but if your picture is unstable, check this first.

The cards can also be picky about detecting a gigabit link. A gigabit switch between the controller and the card is reported to help.

---

## 3. Part one: on an ESP32

Do this first even if a desktop is your goal. It is fewer moving parts: no drivers to install, no permissions, one MAC and therefore nothing to choose.

### 3.1 Pick a board

| Board | Link | Verdict |
|---|---|---|
| **ESP32-S31** (Function-CoreBoard-1) | **1 Gbit** (YT8531) | The right board. On-chip EMAC at gigabit is exactly what these cards want. |
| **ESP32-P4** (Waveshare P4-NANO) | 100 Mbit (IP101) | Works for a small wall. A gigabit switch in between is reported to help. |

Other ESP32 variants do not ship panel-card support: their Ethernet is 100 Mbit at best, and most reach it over an SPI module that is slower still.

### 3.2 Flash it

Use the [web installer](https://moonmodules.org/projectMM/install/), or from a checkout:

```sh
uv run moondeck/build/flash_esp32.py --firmware esp32s31 --port <port>
```

Panel-card support is compiled in per firmware, and it is already on for the boards in the table above.

### 3.3 Wire it

1. **Controller → card.** Ethernet cable from the board to the card's *input* (some cards have two RJ45 ports; the second is for daisy-chaining to the next cabinet).
2. **Card → panels.** HUB75 ribbons from the card's outputs to the panels, in the order you intend to address them.
3. **Power.** 5 V to the panels *and* to the card. Do not power panels from the board.

Nothing needs an IP address. If your board also has WiFi, leave it configured as normal; it is unrelated to the panel link.

### 3.4 Describe the wall

The driver has **no geometry controls**. The wall's shape lives in the Layout, once, so that everything else (effects, modifiers, the preview) sees the same picture.

Add a **Panels** layout and set:

| Control | Meaning |
|---|---|
| `horizontalPanels` / `verticalPanels` | How many panels across and down (1–32 each) |
| `panelWidth` / `panelHeight` | One panel's pixels (default 16×16; a common HUB75 panel is 64×64) |
| `wiringOrder`, `X++`, `Y++`, `snake` | How pixels run *within* a panel |
| `wiringOrderP`, `X++P`, `Y++P`, `snakeP` | How the panels themselves are ordered |

The snake settings are what to reach for when the image is right but every other row or column is reversed.

### 3.5 Add the driver

Add a **Panel Card** driver under Drivers, and set:

| Control | Set it to |
|---|---|
| `format` | `ColorLight 5A-75` |
| `interface` | **Leave blank**: an ESP32 has one MAC, so this is ignored |
| `fps` | 40 is a good start (1–120) |
| `start` / `count` | Leave at defaults unless you are splitting one picture across several controllers |

### 3.6 What you should see

The driver's status line tells you what the wire is doing, and the three states are worth recognising:

| Status | Meaning |
|---|---|
| `1000 Mbit - 5280 packets/s` | Working. The rate is (rows + 4) × fps: one packet per row, plus two brightness and two sync packets per frame. A row wider than 497 pixels splits into several packets, so a wide wall sends more than one per row. |
| `100 Mbit - …` (warning) | Sending, but see §2: fine for a small wall, tearing on a large one. |
| `no ethernet link` | Cable, card power, or the wrong port on the card. |

If the status is healthy and the panels are dark, jump to §5.

---

## 4. Part two: from a desktop

A PC, a Mac or a Raspberry Pi can drive the same card. Reasons to want this: far more compute for heavy effects, and a machine you already own. This is the setup [FPP](https://github.com/FalconChristmas/fpp) popularised on the Pi.

The steps are the same as Part 3, same Layout and same driver, with **two differences**:

- **`interface` matters.** A desktop has several NICs and the frames must leave the right one. Which spelling to use is per-OS, below.
- **Raw Ethernet needs permission.** Sending below IP is privileged on every desktop OS. Without it, projectMM does not fail silently: the driver warns and *records* frames instead of sending them, which is also how the tests run with no hardware.

Pick your OS.

### 4.1 Windows: needs Npcap

Windows has **no** built-in way for an application to put a raw Ethernet frame on the wire. That is an OS restriction, not a projectMM limitation, and it is why Wireshark bundles a driver and why ColorLight's own LEDVision needs one.

**Install [Npcap](https://npcap.com/)** (free; it is also installed if you already have Wireshark). Legacy WinPcap works too.

projectMM loads it *at run time*, so the application installs and runs fine without Npcap; you simply cannot bind an interface until it is present, and the driver says so.

For `interface`, type **any distinctive part of the adapter's name**, case-insensitive: `Realtek`, `Intel`, `Ethernet`. Windows names its capture devices `\Device\NPF_{…GUID…}`, which is neither memorable nor short enough for the field, so projectMM matches your text against the adapter description instead. The full device name also works if you have it.

> If binding fails with Npcap installed, re-run its installer and check whether *"Restrict Npcap driver's access to Administrators only"* was selected. If so, run projectMM as Administrator.

### 4.2 Linux, including Raspberry Pi

Raw frames go out over `AF_PACKET`, which needs `CAP_NET_RAW`.

Either run as root, or grant the capability once so it does not need root again:

```sh
sudo setcap cap_net_raw+ep ./projectMM
```

For `interface`, use the kernel's name exactly: `eth0`, `enp3s0`. `ip link` lists them.

### 4.3 macOS

Raw frames go out over BPF (`/dev/bpf*`), which is root-only by default.

Run projectMM with `sudo`, or install Wireshark's **ChmodBPF** helper, which grants your user access to the BPF devices at boot and is the tidier option if you do this regularly.

For `interface`, use the BSD name exactly: `en0`, `en7`. `ifconfig` lists them.

> On an Apple Silicon or Intel Mac the built-in port is usually gigabit; a USB-C dongle may not be. Check the driver's status line rather than assuming.

### 4.4 Then, on any desktop

Set up the **Panels** layout and the **Panel Card** driver exactly as in §3.4 and §3.5, with `interface` filled in per your OS. The status line means the same things.

---

## 5. When it does not light up

| Symptom | Look at |
|---|---|
| `no ethernet link` | Cable seated, card powered, and plugged into the card's **input** port. On a 100 Mbit controller, try a gigabit switch in between. |
| Status healthy, panels dark | The Layout, not the driver. If the wall is 0 lights, or the driver's window (`start`/`count`) selects none, there is nothing to send. |
| Image tears or rolls | Link speed (§2). Check the status line says 1000 Mbit. |
| Every other row or column mirrored | `snake` for within-panel, `snakeP` for panel order. |
| Panels in the wrong places | `wiringOrderP`, `X++P`, `Y++P`: the panel-grid ordering. |
| Right image, wrong colours | `lightPreset` on the driver, which is where channel order and RGBW synthesis live for every driver. There is no separate colour-order control here. |
| Works on ESP32, not on desktop | Permission (§4). The driver will be showing a warning that says so. |

---

## Where to go next

- **[Drivers](../moonmodules/light/drivers.md#panelcard)**: the Panel Card control reference.
- **[Layouts](../moonmodules/light/layouts.md#panels)**: the Panels layout in full.
- **[Effects](../moonmodules/light/effects.md)** and **[live scripting](../moonmodules/light/MoonLiveEffect.md)**: a wall is a big canvas, and scripted effects are the fastest way to fill it.
