# Running projectMM on a Linux machine

projectMM runs as an ordinary Linux application: the same effect pipeline, web UI and network drivers as on a board, with a real CPU behind them. A small always-on machine makes a good installation controller, whether a server, a Raspberry Pi or a NanoPi.

Deploying is covered here. Building and developing on Linux is in [building.md](../how-to/building.md).

> Windows, with screenshots: [Installing projectMM on a desktop](installing-to-desktop.md). Flashing a board: [Install & first light](../gettingstarted.md).

## Which route applies to your machine

Check the CPU first:

```sh
uname -m
```

| `uname -m` says | Machine | Route |
|---|---|---|
| `x86_64` | Intel or AMD PC, server or VM | Install the package |
| `aarch64` | arm64 board: Pi, NanoPi, most SBCs | Install the package |

Both architectures get a released binary, so the route below is the same one and only the filename differs. Building from source is still there for a distribution the package does not suit, and for developing. A Pi 4 or 5 has ample headroom. A NanoPi R28S has two Gigabit ports, so it can sit between the house network and the lighting network, and 1 GB of RAM, which runs projectMM comfortably. Everything here assumes a Debian-based system (Debian, Ubuntu, Raspberry Pi OS, Armbian); on another distribution, translate the package names.

> `x64` and `amd64` are two names for the same thing. `arm64` is different machine code.

## Install the package

The [releases page](https://github.com/MoonModules/projectMM/releases/latest) carries one `.deb` per architecture: `_amd64.deb` for `x86_64`, `_arm64.deb` for `aarch64`.

On a machine with a browser, download it and install:

```sh
sudo apt install ./projectmm_X.Y.Z_arm64.deb
projectMM
```

On a headless board, fetch it over ssh instead. This picks the right file for the architecture it runs on, so the same two lines work on a Pi, a NanoPi and a server:

```sh
arch=$(dpkg --print-architecture)
url=$(curl -fsSL https://api.github.com/repos/MoonModules/projectMM/releases/tags/latest \
      | grep -o "https://[^\"]*_${arch}\.deb" | head -1)
curl -fsSL -o projectmm.deb "$url" && sudo apt install -y ./projectmm.deb
projectMM
```

`latest` is the rolling build from `main`, which is what the web installer offers too. For the newest tagged release, replace `tags/latest` with `latest` in that URL.

Open `http://<machine>:8080`. A `.tar.gz` to unpack anywhere is on the same page.

**A package built for the wrong architecture refuses to install**, which is the failure you want: `apt` rejects it by name rather than installing something that cannot run.

The arm64 build targets glibc 2.35, so it installs on Raspberry Pi OS Bookworm, Debian 12 and 13, Ubuntu 22.04 and later. On something older, build from source below.

## Build from source

For a distribution the package does not suit, an older glibc, or to develop on the board. Allow an hour the first time, most of it waiting.

### 1. Write an OS image to the SD card

Take a Debian-based image. For a Raspberry Pi, [Raspberry Pi OS Lite](https://www.raspberrypi.com/software/): the desktop build leaves less memory for the compile. For a NanoPi, the Debian image from the board's [FriendlyELEC wiki page](https://wiki.friendlyelec.com/wiki/index.php/NanoPi_R28S#Flashing_the_OS_to_the_microSD_card), under `01_Official images/01_SD card images`. Skip FriendlyWrt: it is router firmware with a different package manager. [Armbian](https://www.armbian.com/download/) covers many boards from one project, if it lists yours.

Use an 8 GB card or larger. Write it with [Raspberry Pi Imager](https://www.raspberrypi.com/software/) or [balenaEtcher](https://etcher.balena.io/); both take the compressed download directly.

On a Raspberry Pi, open Imager's settings before writing: set the username, hostname and WiFi, and enable SSH. Raspberry Pi OS ships with SSH off and no default user, so a card written without those boots to a machine you cannot reach.

### 2. First boot

Insert the card, connect the network cable, power on. Give it 10 to 20 minutes: the first boot resizes the filesystem and may reboot itself.

Find it on the network:

```sh
ping raspberrypi.local          # or NanoPi-R28S.local
arp -a                          # everything the network has seen
```

Your router's client list is the fallback when mDNS does not resolve. A name that never resolves means the board has no mDNS responder: Raspberry Pi OS ships one, a minimal Debian or FriendlyELEC image often does not, so `.local` fails while the IP answers. Log in by IP and fix it below.

### 3. Log in

```sh
ssh <you>@<hostname>.local      # Raspberry Pi OS: the user you set in Imager
ssh pi@NanoPi-R28S              # FriendlyELEC Debian: user pi, password pi
ssh pi@192.168.1.156            # by address, when neither name resolves
```

**A wall of `setlocale: LC_CTYPE: cannot change locale (UTF-8)` warnings on login is harmless.** macOS sends its own `LC_CTYPE` to the board, which has no locale by that name, and bash repeats the warning per startup file. It fires before anything you run. Silence it on the board:

```sh
sudo apt install -y locales && sudo locale-gen en_US.UTF-8
```

Change a default password at once:

```sh
passwd
```

**If `.local` did not resolve**, install the mDNS responder now and the name works from the next boot:

```sh
sudo apt install -y avahi-daemon
```

Without network access, attach a keyboard and monitor and configure it there: `sudo nmtui` for WiFi and addresses, `ip ad` to see what the board has. On a NanoPi, `sudo nmtui` also configures the second port.

### 4. Update the system

```sh
sudo apt update
sudo apt upgrade -y
```

### 5. Install the prerequisites

```sh
sudo apt install -y python3-pip cmake build-essential git
pip install uv --break-system-packages
```

`--break-system-packages` is routine on Debian 12 and later, where the system Python is marked externally managed; it affects pip's own environment only.

If `uv` is then not found, add its directory to your path:

```sh
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
```

### 6. Build and run

```sh
git clone https://github.com/MoonModules/projectMM.git
cd projectMM
uv run moondeck/build/build_desktop.py
uv run moondeck/run/run_desktop.py
```

The build takes a few minutes on a Pi 4 or a NanoPi, longer on older boards. `run_desktop.py` detaches, so the program outlives the ssh session. Open `http://<board>:8080`.

That is a running system. Everything below is optional.

> A board with 1 GB of RAM or less can run out of memory while compiling; the compiler is killed rather than reporting an error. Add swap: edit `/etc/dphys-swapfile` to raise `CONF_SWAPSIZE`, then `sudo dphys-swapfile swapoff && sudo dphys-swapfile setup && sudo dphys-swapfile swapon`. Editing alone changes nothing; `setup` regenerates the file.

## Keeping it running after a reboot

Give it a systemd unit at `/etc/systemd/system/projectmm.service`:

```ini
[Unit]
Description=projectMM
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/home/pi/projectMM/build/projectMM
Restart=always
RestartSec=5
User=pi

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now projectmm
systemctl status projectmm
```

`Restart=always` covers a crash as well as a reboot. Adjust `User` and the path to where you built.

## Shutting down

Shut down cleanly; an SD card interrupted mid-write can corrupt the filesystem:

```sh
sudo shutdown now     # or: sudo reboot
```

projectMM writes to disk only when settings change, so the card is a fine home for it. The risk is the operating system's own writes.

## Containers

Docker runs a full instance on amd64 and arm64 alike; the command is in the [README](https://github.com/MoonModules/projectMM#readme). The published image carries both, so a board and a server pull the same tag and each gets native instructions.

## Where to go next

- [Install & first light](../gettingstarted.md): the same program on an ESP32.
- [How projectMM works](how-projectmm-works.md): layouts, layers, effects and drivers.
- [building.md](../how-to/building.md): building, testing and packaging in depth.
