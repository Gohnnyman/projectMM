# Privacy policy

**Last updated: 2026-09-01**

This policy covers the projectMM software (the firmware, the desktop application and the web interface they serve), the [web installer](https://moonmodules.org/projectMM/install/), and this documentation site.

## The short version

**projectMM collects nothing.** It has no accounts, no analytics, no telemetry and no usage reporting. It does not phone home. Your configuration lives on your own device or computer and is never sent anywhere by the software.

The rest of this page is the detail behind that sentence, because "we collect nothing" is worth being able to check rather than being asked to take on trust.

## What projectMM stores, and where

Everything projectMM keeps is **on the machine you run it on**:

- **On an ESP32 or similar device**: your configuration in the device's own flash storage.
- **On a desktop**: a folder belonging to your user account. `%LOCALAPPDATA%\projectMM` on Windows, `~/Library/Application Support/projectMM` on macOS, `$XDG_DATA_HOME/projectMM` (or `~/.local/share/projectMM`) on Linux.

That configuration is what you would expect it to contain: your layouts, effects, drivers, pin assignments, device name, and any network credentials you entered so the device can reconnect. **Credentials are stored so the device can use them and are never transmitted to us**, because there is no "us" for them to be transmitted to.

Nothing in that folder is uploaded, synchronized or backed up by projectMM. You can read it, edit it, copy it and delete it.

## What reaches the internet, and when

Four things, all of them either something you asked for or something your browser does:

**Checking for a newer release.** The web interface asks GitHub's public release API whether a newer version exists, so it can show the update badge. **Your browser makes this request, not your device and not the desktop application.** GitHub sees it as an ordinary anonymous API call from your browser, subject to [GitHub's privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement).

**Downloading a MoonLive script.** When you pick a factory script, your browser fetches it from `raw.githubusercontent.com`. Again the browser, not the device: projectMM serves your browser the name of the script and the browser downloads it.

**Updating firmware over the air.** If you start a firmware update, the device downloads the new firmware from GitHub. This only happens when you ask for it.

**Using the web installer.** The installer page is served from GitHub Pages and talks to GitHub's API to list releases. Flashing itself happens over USB, directly between your browser and the board, and the firmware never passes through a server of ours.

In every case the request goes to GitHub, not to MoonModules. We operate no server that receives data from projectMM, and we keep no logs of your use of it, because there is nothing to log.

## What we do not collect

To be explicit, projectMM does not collect or transmit: your name, email address, postal address or location; your IP or MAC address; your Wi-Fi credentials or MQTT passwords; which effects you use or how you use them; crash reports; identifiers of any kind; or anything that would let one installation be told from another.

There is no opt-out because there is nothing to opt out of.

## Your browser's local storage

The web interface stores a few conveniences in your browser's own local storage, for example the cached result of the update check and the release you last selected in the installer. This never leaves your browser, is readable and clearable by you, and holds nothing personal.

## Things you connect projectMM to

projectMM speaks to other systems on your network when you configure it to: an MQTT broker, Home Assistant, Art-Net or E1.31 consoles, and similar. Those connections carry lighting data and go where you point them, usually to hardware on your own network.

**When you connect projectMM to another service, that service's own privacy policy governs whatever it does with the data.** This is worth knowing if you point it at a cloud-hosted broker or a smart-home platform, because that is a decision to send data somewhere, and it is yours rather than ours.

## Documentation and downloads

This documentation site and the release downloads are hosted by **GitHub** (GitHub Pages and GitHub Releases). Like any web host, GitHub receives the requests your browser makes, including your IP address, and processes them under [its own privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement). We add no analytics, no cookies of our own and no tracking to these pages.

## If usage statistics are ever added

We may one day want to know which boards and configurations are actually in use, so that development effort goes where the users are. If that is ever added, we commit in advance that it will:

- be **opt-in**, with a clear prompt and a working decline,
- be a **one-time report** rather than continuous reporting,
- carry **no identifying information in the report itself**: no device name, no network addresses, no credentials, no free-text fields you typed, and nothing that would let one installation be recognized again,
- **not retain the IP address** the report arrives from, and
- be documented on this page, in detail, before it ships.

The fourth point deserves saying out loud rather than leaving implied. Any server that receives an HTTP request necessarily sees the IP address it came from, and a country breakdown, which is the sort of thing such a dashboard usually shows, can only be derived from that address. So "we do not collect your IP" would be too strong a claim for any such service to make honestly. What we can promise is the narrower and truthful version: the address is used to answer the request, and to derive a country if we show one, and is not stored.

**Nothing of the kind exists today.** This section is a commitment about how it would be done, not a description of something running. This page is updated when the software changes, not in advance of it.

## Changes to this policy

Changes are made in the open: this page lives in the [project repository](https://github.com/MoonModules/projectMM/blob/main/docs/privacy-policy.md), so every revision is a commit you can read, with its date and its reason.

## Contact

Questions about this policy, or about anything on this page you would like to verify, are welcome as an [issue on the repository](https://github.com/MoonModules/projectMM/issues) or on the [Discord](https://discord.gg/TC8NSUSCdV).

projectMM is free and open-source software. If you want to check any statement here rather than trust it, the source is public and the network calls described above are the only ones in it.
