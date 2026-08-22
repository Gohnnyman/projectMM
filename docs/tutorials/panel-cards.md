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

**Install [Npcap](https://npcap.com/)** (free; it is also installed if you already have Wireshark). Legacy WinPcap 4.1.3 also works, and is what this driver was developed and measured against; Npcap offers the same API and is the maintained choice on a new machine.

projectMM loads it *at run time*, so the application installs and runs fine without either; you simply cannot bind an interface until one is present, and the driver says so.

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

## 5. Card firmware, and the flicker

A card's firmware has a version of its own, separate from the hardware revision printed on the board. It matters twice: once because one generation is defective, and once because projectMM has to know which generation it is talking to.

### The v13 flicker

Cards running **firmware v13** on **v8.x hardware** flicker in time with network activity. This is a defect in the card, not in the sender: it shows up identically under projectMM, [FPP](https://github.com/FalconChristmas/fpp) and ColorLight's own LEDVision, and nothing about how the frames are sent avoids it. The fix is to put older firmware on the card.

To be clear about what is and is not wrong: projectMM drives a v13 card perfectly well. It binds, sends at the full frame rate, and the picture is correct. The flicker is the *only* reason to move off v13, and it is the card doing it. Since the trigger is network activity and driving a wall means constant network activity, there is no sending-side setting that avoids it. Batching, frame rate and packet count have all been tried; the defect is downstream of all of them.

Two different faults look like "flicker", and only one of them is this. Tell them apart before spending an evening on the wrong one:

| What you see | What it is |
|---|---|
| Flicker follows the Ethernet activity LED, and is there even on a still, dim image | The v13 defect. Downgrade the card. |
| Flicker grows with brightness and with how much of the wall is lit | Power. The panels draw more than the supply holds. A downgrade changes nothing. |

The second row is worth taking seriously, because projectMM sends a full frame every tick no matter what the effect is doing. The packet rate is identical for a black wall and a busy one, so flicker that tracks *content* is not coming from the network.

### Reading and changing the version

**[LEDUpgrade](https://en.colorlightinside.com/product/download/383)** reads and writes card firmware. Use **version 4.0**. Version 5.0 ships no pre-v12 firmware at all, so it cannot do this downgrade from its preset list however long you fight it.

**Why 11.09 rather than something older.** Anything before v12 clears the flicker, but older is not automatically safer: cards on 11.08 were reported strobing white, which 11.09 fixes. 11.09 is the newest build on the safe side of the defect, so it carries the most fixes while carrying none of the flicker.

1. Connect the card **directly** to the machine, no switch in between.
2. **Close everything else that talks to the card**, projectMM included. A card being streamed at will not answer, and two ColorLight tools at once (LEDVision and LEDUpgrade) interfere.
3. `Send Mode` set to the network-card mode, then choose your adapter. Restart LEDUpgrade afterwards: it binds the adapter at startup.
4. **Detect Receiver Cards.** It reports something like `5A 13.17 (v8.0)`, meaning firmware 13.17 on v8.0 hardware.
5. **Readback Firmware** to back up what is on the card before you replace it.
6. `Upgrade Firmware`, preset, `4in1`, `normal`, then **`normal-11.09`**. Stay in the `normal` series: `PWM` and `shixin` are for different panel driver ICs.
7. **Power-cycle the card.** It goes on running the old firmware until you do, which is the step most often missed.

### Then tell projectMM what it is talking to

Set the driver's `firmware` control to match the card:

| Setting | For |
|---|---|
| `v13 and newer` | A stock card. The brightness and sync frames go out twice, which is the copy this firmware acts on. |
| `v12 and older` | A downgraded card. Both go out once. |

The mismatch is not subtle in one direction: leave a downgraded card on `v13 and newer` and it receives a second sync, treats it as another latch, aborts the refresh already running, and the wall updates once every few seconds.

---

## 6. When it does not light up

| Symptom | Look at |
|---|---|
| `no ethernet link` | On a desktop, check `interface` first: a string that matches no adapter fails the bind and reports exactly this, with nothing to distinguish it from an unplugged cable. Then cable seated, card powered, and plugged into the card's **input** port. On a 100 Mbit controller, try a gigabit switch in between. |
| Status healthy, panels dark | The Layout, not the driver. If the wall is 0 lights, or the driver's window (`start`/`count`) selects none, there is nothing to send. |
| Image tears or rolls | Link speed (§2). Check the status line says 1000 Mbit. |
| A new frame only every few seconds | `firmware` (§5) is set to `v13 and newer` on a card running v12 or older. The duplicate sync latches twice and aborts the refresh. |
| Flicker in time with network activity | The card's own v13 firmware defect (§5), not the sender. Downgrade the card. |
| Every other row or column mirrored | `snake` for within-panel, `snakeP` for panel order. |
| Panels in the wrong places | `wiringOrderP`, `X++P`, `Y++P`: the panel-grid ordering. |
| Right image, wrong colours | `lightPreset` on the driver, which is where channel order and RGBW synthesis live for every driver. There is no separate colour-order control here. |
| Works on ESP32, not on desktop | Permission (§4). The driver will be showing a warning that says so. |

---

## Where to go next

- **[Drivers](../moonmodules/light/drivers.md#panelcard)**: the Panel Card control reference.
- **[Layouts](../moonmodules/light/layouts.md#panels)**: the Panels layout in full.
- **[Effects](../moonmodules/light/effects.md)** and **[live scripting](../moonmodules/light/MoonLiveEffect.md)**: a wall is a big canvas, and scripted effects are the fastest way to fill it.
