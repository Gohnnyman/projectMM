#!/usr/bin/env python3
"""Build + package a release-ready desktop binary for the host platform.

Runs under CI on macOS, Windows and Linux runners. The output lands in `dist/`:

  macOS arm64:  dist/projectMM-macos-arm64-vX.Y.Z.tar.gz + .dmg
  Windows x64:  dist/projectMM-windows-x64-vX.Y.Z.zip
  Linux x64:    dist/projectMM-linux-x64-vX.Y.Z.tar.gz + dist/projectmm_X.Y.Z_amd64.deb

Each archive carries the executable + a short README.txt with run instructions.

The macOS build is ad-hoc signed, which gets it as far as Gatekeeper's "could not
verify" dialog; clearing the quarantine flag is still required to open it. Windows
is unsigned, so SmartScreen warns. Documented in the README and each README.txt.
"""

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
# Per-host build dirs to match the ESP32 ``build/esp32-<board>/`` shape.
# Each host gets its own; CI runners use exactly one, but on a developer
# machine the layout means an experimental Linux build wouldn't clobber a
# macOS package job.
BUILD_DIR_MACOS = ROOT / "build" / "macos"
BUILD_DIR_WIN   = ROOT / "build" / "windows"
BUILD_DIR_LINUX = ROOT / "build" / "linux"
DIST_DIR = ROOT / "dist"


def read_version() -> str:
    meta = json.loads((ROOT / "library.json").read_text())
    return meta["version"]


def run(cmd: list[str]) -> None:
    print("+ " + " ".join(cmd))
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        sys.exit(r.returncode)


def version_args(version: str) -> list[str]:
    """The -DMM_VERSION override, or nothing: the exact contract build_esp32.py has. Empty means
    a local/dev build and build_info.h's library.json default; the release pipeline passes the
    computed semver so the binary, the asset names and the update badge all carry one version.
    The inner quotes make the macro a string literal, same as the ESP32 build."""
    return [f'-DMM_VERSION="{version}"'] if version else []


def configure_and_build_macos(version: str = "") -> Path:
    """Configure + build for macOS arm64. Returns the built binary path."""
    bdir = str(BUILD_DIR_MACOS.relative_to(ROOT))
    run([
        "cmake", "-B", bdir,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_OSX_ARCHITECTURES=arm64",
    ] + version_args(version))
    run(["cmake", "--build", bdir, "--config", "Release", "-j"])
    binary = BUILD_DIR_MACOS / "projectMM"
    if not binary.exists():
        print(f"package_desktop: expected binary not found at {binary}")
        sys.exit(1)
    return binary


def configure_and_build_linux(version: str = "") -> Path:
    """Configure + build for Linux x86-64. Returns the built binary path."""
    bdir = str(BUILD_DIR_LINUX.relative_to(ROOT))
    run(["cmake", "-B", bdir, "-DCMAKE_BUILD_TYPE=Release"] + version_args(version))
    run(["cmake", "--build", bdir, "--config", "Release", "-j"])
    binary = BUILD_DIR_LINUX / "projectMM"
    if not binary.exists():
        print(f"package_desktop: expected binary not found at {binary}")
        sys.exit(1)
    return binary


def package_linux(binary: Path, version: str) -> Path:
    """A .tar.gz and a .deb. No signing: Linux has no Gatekeeper equivalent to satisfy.

    The tarball is the universal form (any distro, unpack and run). The .deb is for the machines
    this actually runs on: Debian, Ubuntu, and Raspberry Pi OS, where `apt install ./file.deb` puts
    it on PATH and a user never thinks about where it landed.

    Deliberately no AppImage yet: it needs a runtime downloaded at package time, which makes the
    release job depend on a third-party host. Worth adding once someone asks.
    """
    DIST_DIR.mkdir(exist_ok=True)
    out = DIST_DIR / f"projectMM-linux-x64-v{version}.tar.gz"
    readme = DIST_DIR / "_README.txt"
    readme.write_text(readme_text(version, "Linux x64"), encoding="utf-8")
    try:
        with tarfile.open(out, "w:gz") as tar:
            tar.add(binary, arcname="projectMM")
            tar.add(readme, arcname="README.txt")
    finally:
        readme.unlink(missing_ok=True)
    print(f"package_desktop: wrote {out}")
    package_deb(binary, version)
    return out


def package_deb(binary: Path, version: str) -> Path | None:
    """A .deb, built with dpkg-deb when the host has it (every Debian-family CI runner does).

    Hand-rolled rather than via a helper library: a .deb is an ar archive of two tarballs and a
    control file, and the packaging tools that wrap that would be a build dependency for something
    this project needs exactly once.
    """
    if shutil.which("dpkg-deb") is None:
        # Under CI this is fatal: the release uploads dist/projectmm_*.deb with
        # fail_on_unmatched_files, so skipping here would fail the entire release (ESP32
        # firmware and all) with an error naming the glob rather than the missing tool.
        # On a dev machine it stays a skip, which is what the tarball beside it is for.
        if os.environ.get("CI"):
            sys.exit("package_desktop: no dpkg-deb on this CI runner, cannot build the .deb")
        print("package_desktop: no dpkg-deb on this host, skipping the .deb")
        return None
    # A Debian version cannot carry a leading 'v' and must start with a digit. A hyphen is also
    # out: dpkg reads it as the upstream/revision separator, so the computed 3.0.0-dev.N becomes
    # 3.0.0~dev.N here. Deliberately a tilde: dpkg sorts ~ BEFORE the bare version, so a dev build
    # upgrades to the 3.0.0 release exactly as semver intends the prerelease to.
    version = version.replace("-", "~")
    stage = DIST_DIR / f"deb-{version}"
    shutil.rmtree(stage, ignore_errors=True)
    (stage / "DEBIAN").mkdir(parents=True)
    (stage / "usr" / "bin").mkdir(parents=True)
    shutil.copy2(binary, stage / "usr" / "bin" / "projectMM")
    # A menu entry, so a Linux user launches it the way a macOS user opens the .app: Terminal=true
    # gives the same visible, closeable window that shows the log and stops the server when closed.
    apps = stage / "usr" / "share" / "applications"
    apps.mkdir(parents=True)
    (apps / "projectmm.desktop").write_text(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=projectMM\n"
        "Comment=Drive large LED installations and DMX fixtures\n"
        "Exec=projectMM\n"
        "Icon=projectmm\n"
        "Terminal=true\n"
        "Categories=Graphics;Utility;\n", encoding="utf-8")
    icons = stage / "usr" / "share" / "icons" / "hicolor" / "256x256" / "apps"
    icons.mkdir(parents=True)
    fav = ROOT / "web-installer" / "favicon.png"
    if fav.exists():
        shutil.copy2(fav, icons / "projectmm.png")

    (stage / "DEBIAN" / "control").write_text(
        "Package: projectmm\n"
        f"Version: {version}\n"
        "Section: misc\n"
        "Priority: optional\n"
        "Architecture: amd64\n"
        "Maintainer: MoonModules <https://github.com/MoonModules/projectMM>\n"
        "Description: Drive large LED installations and DMX fixtures\n"
        " projectMM renders effects to LED fixtures and DMX, controlled from a\n"
        " browser. This is the desktop build; run projectMM and open\n"
        " http://localhost:8080/.\n", encoding="utf-8")
    out = DIST_DIR / f"projectmm_{version}_amd64.deb"
    run(["dpkg-deb", "--build", "--root-owner-group", str(stage), str(out)])
    shutil.rmtree(stage, ignore_errors=True)
    print(f"package_desktop: wrote {out}")
    return out


def configure_and_build_windows(version: str = "") -> Path:
    """Configure + build for Windows x64. Returns the built binary path."""
    bdir = str(BUILD_DIR_WIN.relative_to(ROOT))
    # No -G: let CMake auto-detect the installed Visual Studio. Pinning a
    # specific generator (e.g. "Visual Studio 17 2022") breaks whenever the
    # windows-latest runner image migrates its bundled VS version.
    run([
        "cmake", "-B", bdir,
        "-DCMAKE_BUILD_TYPE=Release",
    ] + version_args(version))
    run(["cmake", "--build", bdir, "--config", "Release"])
    # MSVC multi-config places binaries under <build-dir>/Release/.
    binary = BUILD_DIR_WIN / "Release" / "projectMM.exe"
    if not binary.exists():
        # Some generators drop it directly under the build dir.
        fallback = BUILD_DIR_WIN / "projectMM.exe"
        if fallback.exists():
            return fallback
        print(f"package_desktop: expected binary not found at {binary}")
        sys.exit(1)
    return binary


def readme_text(version: str, platform_label: str) -> str:
    return (
        f"projectMM v{version} ({platform_label})\n"
        f"\n"
        f"Run: ./projectMM (macOS) or projectMM.exe (Windows)\n"
        f"Open: http://localhost:8080/\n"
        f"\n"
        f"macOS first run: the app is ad-hoc signed, not notarized, so macOS\n"
        f"refuses it with 'Apple could not verify projectMM is free of malware'.\n"
        f"That dialog has no way through on macOS 15 and later, so clear the\n"
        f"download flag in Terminal and open it again:\n"
        f"\n"
        f"  xattr -dr com.apple.quarantine /Applications/projectMM.app\n"
        f"\n"
        f"(for the tarball, point it at ./projectMM instead). One time only.\n"
        f"\n"
        f"Source: https://github.com/MoonModules/projectMM\n"
    )


def adhoc_sign(binary: Path) -> None:
    """Sign the macOS binary with an ad-hoc signature. Free, and it changes what a user sees.

    An UNSIGNED binary is refused by recent macOS before it even reaches Gatekeeper's usual
    prompt. Ad-hoc signing gets it as far as the standard "could not verify" dialog, which is the
    free half of the distance to notarization (that needs a paid Developer ID).

    It is NOT enough to make the app openable: macOS 15 dropped the right-click -> Open bypass for
    ad-hoc signed apps, so that dialog now has only "Move to Trash" and "Done". The user clears the
    quarantine flag instead, which every README this script writes explains. Verified on macOS
    26.6: the app launches normally once the flag is gone.

    Best effort: a failure prints and continues, because an unsigned build is still shippable and
    a release that stops for this would be worse than one that warns.
    """
    r = subprocess.run(["codesign", "--force", "--sign", "-", str(binary)],
                       capture_output=True, text=True)
    if r.returncode == 0:
        print("package_desktop: ad-hoc signed (Gatekeeper shows the standard unverified-developer prompt)")
    else:
        print(f"package_desktop: ad-hoc signing failed, shipping unsigned: {r.stderr.strip()}")


def make_icns(dest: Path) -> Path | None:
    """Build an .icns from the favicon the browser tab already shows, so the Dock icon and the tab
    icon are the same mark. sips and iconutil ship with macOS: no dependency, no download.

    The source is 320x320, which covers every size a Dock or Finder list shows. The 512pt slots an
    .icns can carry are left out rather than upscaled, since a soft icon reads worse than a smaller
    crisp one. Swap in a 1024 master and they can be added.
    """
    src = ROOT / "web-installer" / "favicon.png"
    if not src.exists() or shutil.which("iconutil") is None:
        print("package_desktop: no favicon or no iconutil, the app will use the default icon")
        return None
    iconset = dest / "projectMM.iconset"
    shutil.rmtree(iconset, ignore_errors=True)
    iconset.mkdir(parents=True)
    # (size, filename) pairs iconutil expects; @2x is the Retina variant of the size below it.
    for px, name in ((16, "icon_16x16.png"), (32, "icon_16x16@2x.png"), (32, "icon_32x32.png"),
                     (64, "icon_32x32@2x.png"), (128, "icon_128x128.png"),
                     (256, "icon_128x128@2x.png"), (256, "icon_256x256.png")):
        run(["sips", "-z", str(px), str(px), str(src), "--out", str(iconset / name)])
    out = dest / "projectMM.icns"
    run(["iconutil", "-c", "icns", str(iconset), "-o", str(out)])
    shutil.rmtree(iconset, ignore_errors=True)
    return out


def make_app_bundle(binary: Path, version: str, dest: Path) -> Path:
    """A double-clickable .app around the console binary.

    projectMM is a terminal program that serves a web UI, and that IS the shape a user wants: the
    window shows it is alive, the log is right there, and closing it stops the server. What Finder
    will not do is open a terminal for a bare binary, so the bundle's executable is a stub that asks
    Terminal to run the real one.

    The CLI lives INSIDE the bundle (Contents/MacOS/projectMM), so the disk image holds exactly one
    draggable item while `--port` and `--no-browser` stay reachable at
    /Applications/projectMM.app/Contents/MacOS/projectMM.
    """
    app = dest / "projectMM.app"
    shutil.rmtree(app, ignore_errors=True)
    macos = app / "Contents" / "MacOS"
    res = app / "Contents" / "Resources"
    macos.mkdir(parents=True)
    res.mkdir(parents=True)

    shutil.copy2(binary, macos / "projectMM")
    icns = make_icns(dest)
    if icns:
        shutil.move(str(icns), res / "projectMM.icns")

    # The stub Finder launches. `open -a Terminal` gives the user the window the app lives in.
    launcher = macos / "projectMM-launcher"
    launcher.write_text(
        "#!/bin/sh\n"
        "# Finder runs this; it opens a Terminal window on the real binary beside it. The window is\n"
        "# the app's presence: it shows the log, and closing it stops the server.\n"
        'exec open -a Terminal "$(dirname "$0")/projectMM"\n', encoding="utf-8")
    launcher.chmod(0o755)

    (app / "Contents" / "Info.plist").write_text(f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>projectMM</string>
  <key>CFBundleDisplayName</key><string>projectMM</string>
  <key>CFBundleIdentifier</key><string>org.moonmodules.projectmm</string>
  <key>CFBundleVersion</key><string>{version}</string>
  <key>CFBundleShortVersionString</key><string>{version}</string>
  <key>CFBundleExecutable</key><string>projectMM-launcher</string>
  <key>CFBundleIconFile</key><string>projectMM</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
</dict>
</plist>
""", encoding="utf-8")
    adhoc_sign(app)          # sign the BUNDLE, which covers the binary inside it
    return app


def package_dmg(binary: Path, version: str) -> Path | None:
    """A disk image holding one item: drag projectMM to Applications and it is installed.

    hdiutil ships with macOS. A .tar.gz still ships beside this for anyone scripting a deploy.
    """
    if shutil.which("hdiutil") is None:
        # Fatal under CI for the same reason as the .deb above.
        if os.environ.get("CI"):
            sys.exit("package_desktop: no hdiutil on this CI runner, cannot build the .dmg")
        print("package_desktop: no hdiutil, skipping the .dmg")
        return None
    stage = DIST_DIR / "dmg"
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    make_app_bundle(binary, version, stage)
    # The Applications symlink is what makes the window a drag-and-drop target.
    (stage / "Applications").symlink_to("/Applications")
    out = DIST_DIR / f"projectMM-macos-arm64-v{version}.dmg"
    out.unlink(missing_ok=True)
    run(["hdiutil", "create", "-volname", f"projectMM {version}",
         "-srcfolder", str(stage), "-ov", "-format", "UDZO", str(out)])
    shutil.rmtree(stage, ignore_errors=True)
    print(f"package_desktop: wrote {out}")
    return out


def package_macos(binary: Path, version: str) -> Path:
    adhoc_sign(binary)
    DIST_DIR.mkdir(exist_ok=True)
    out = DIST_DIR / f"projectMM-macos-arm64-v{version}.tar.gz"
    readme = DIST_DIR / "_README.txt"
    # encoding="utf-8" explicitly: Windows' default write_text encoding is cp1252, so a
    # non-ASCII character added to readme_text later would raise there and nowhere else.
    # The text is plain ASCII today; naming the encoding keeps that from being load-bearing.
    readme.write_text(readme_text(version, "macOS arm64"), encoding="utf-8")
    try:
        with tarfile.open(out, "w:gz") as tar:
            tar.add(binary, arcname="projectMM")
            tar.add(readme, arcname="README.txt")
    finally:
        readme.unlink(missing_ok=True)
    print(f"package_desktop: wrote {out}")
    package_dmg(binary, version)
    return out


NSI_TEMPLATE = r'''Unicode true
!define APPNAME "projectMM"
!define PUBLISHER "MoonModules"
!define HOMEPAGE "https://github.com/MoonModules/projectMM"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\projectMM"

Name "${APPNAME} @VERSION@"
OutFile "@OUT@"
; Per-user, under Programs: no elevation prompt, and no chance of landing in a directory the user
; cannot write to. The SETTINGS live in $LOCALAPPDATA\projectMM, deliberately NOT under here, so an
; upgrade replaces the program and leaves the configuration untouched.
InstallDir "$LOCALAPPDATA\Programs\projectMM"
InstallDirRegKey HKCU "${UNINSTKEY}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
!include "MUI2.nsh"
!define MUI_ICON "@ICON@"
!define MUI_UNICON "@ICON@"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "install"
  ; A running projectMM.exe holds a lock on the file and the install would fail with a
  ; file-in-use error. taskkill is a Windows built-in, so this needs no NSIS plugin; it is
  ; harmless when nothing is running. projectMM is a local server the user restarts from the
  ; Start menu, so stopping it costs a reconnect, not work.
  DetailPrint "Stopping any running ${APPNAME}..."
  nsExec::Exec 'taskkill /F /IM projectMM.exe'
  Pop $0

  SetOutPath "$INSTDIR"
  File "@BINARY@"
  File /oname=projectMM.ico "@ICON@"
  File /oname=README.txt "@README@"

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\projectMM.exe" "" "$INSTDIR\projectMM.ico"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\uninstall.exe"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  ; Add/Remove Programs. HKCU to match the per-user install: an HKLM entry would need elevation
  ; and would advertise the app to users who do not have it.
  WriteRegStr   HKCU "${UNINSTKEY}" "DisplayName"     "${APPNAME}"
  WriteRegStr   HKCU "${UNINSTKEY}" "DisplayVersion"  "@VERSION@"
  WriteRegStr   HKCU "${UNINSTKEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr   HKCU "${UNINSTKEY}" "URLInfoAbout"    "${HOMEPAGE}"
  WriteRegStr   HKCU "${UNINSTKEY}" "DisplayIcon"     "$INSTDIR\projectMM.ico"
  WriteRegStr   HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${UNINSTKEY}" "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
SectionEnd

Section "uninstall"
  nsExec::Exec 'taskkill /F /IM projectMM.exe'
  Pop $0
  Delete "$INSTDIR\projectMM.exe"
  Delete "$INSTDIR\projectMM.ico"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"
  DeleteRegKey HKCU "${UNINSTKEY}"
  ; $LOCALAPPDATA\projectMM is NOT removed. Settings outlive the program on purpose: an uninstall
  ; is often a step in a reinstall, and silently discarding a user's configuration is the kind of
  ; thing they only discover afterwards. Removing that folder by hand is the documented way out.
SectionEnd
'''


def windows_icon(version: str) -> Path | None:
    """The .ico for the installer, its shortcut and Add/Remove Programs.

    Prefers the one CMake already generated beside the binary, so the installer and the exe carry
    the identical icon; regenerates it only when packaging a build that did not produce one.
    """
    built = BUILD_DIR_WIN / "projectMM.ico"
    if built.exists():
        return built
    out = DIST_DIR / "projectMM.ico"
    src = ROOT / "web-installer" / "favicon.png"
    if not src.exists():
        print("package_desktop: no favicon to generate an icon from")
        return None
    run(["uv", "run", str(ROOT / "moondeck" / "ci" / "make_ico.py"), str(src), str(out)])
    return out if out.exists() else None


def find_makensis() -> str | None:
    """NSIS's compiler, on PATH or in its default install location.

    The Windows NSIS installer does NOT put itself on PATH, so `which` alone reports it missing on
    a developer machine that has it installed the ordinary way. The CI runner image does not ship
    NSIS at all (the first release run proved it), so the workflow installs it via choco, which
    lands in Program Files (x86)\NSIS, one of the two locations searched below.
    """
    exe = shutil.which("makensis")
    if exe:
        return exe
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
        if not base:
            continue
        candidate = Path(base) / "NSIS" / "makensis.exe"
        if candidate.is_file():
            return str(candidate)
    return None


def package_windows_installer(binary: Path, version: str) -> Path | None:
    """A setup.exe, built with NSIS when the host has makensis (the windows-latest runner does).

    The zip beside it stays the portable form: unpack anywhere, run, no install. This is for the
    user who wants what macOS and Linux already get, an entry in the Start menu with an icon and a
    working uninstall, rather than an executable loose in their Downloads folder.
    """
    makensis = find_makensis()
    if makensis is None:
        # Same reasoning as the .deb: on CI this is fatal, because the release uploads the
        # installer with fail_on_unmatched_files and a skip would fail the whole release with an
        # error naming a glob rather than the missing tool. On a dev machine it is a skip, and the
        # zip beside it is what that developer uses.
        if os.environ.get("CI"):
            sys.exit("package_desktop: no makensis on this CI runner, cannot build the installer")
        print("package_desktop: no makensis on this host, skipping the installer")
        return None

    icon = windows_icon(version)
    if icon is None:
        # CI-fatal for the same reason as the missing makensis above: the release uploads the
        # installer with fail_on_unmatched_files, so skipping here would fail the whole release
        # with an error naming a glob rather than the missing icon source.
        if os.environ.get("CI"):
            sys.exit("package_desktop: no icon for the installer, and no favicon to build one from")
        print("package_desktop: no icon, skipping the installer")
        return None

    out = DIST_DIR / f"projectMM-windows-x64-v{version}-setup.exe"
    readme = DIST_DIR / "_README.txt"
    readme.write_text(readme_text(version, "Windows x64"), encoding="utf-8")
    script = DIST_DIR / "projectMM.nsi"
    script.write_text(
        NSI_TEMPLATE
        .replace("@VERSION@", version)
        .replace("@OUT@", str(out))
        .replace("@BINARY@", str(binary))
        .replace("@ICON@", str(icon))
        .replace("@README@", str(readme)),
        encoding="utf-8")
    try:
        run([makensis, str(script)])
    finally:
        script.unlink(missing_ok=True)
        readme.unlink(missing_ok=True)
    print(f"package_desktop: wrote {out}")
    return out


def package_windows(binary: Path, version: str) -> Path:
    DIST_DIR.mkdir(exist_ok=True)
    out = DIST_DIR / f"projectMM-windows-x64-v{version}.zip"
    readme = DIST_DIR / "_README.txt"
    readme.write_text(readme_text(version, "Windows x64"), encoding="utf-8")
    try:
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.write(binary, arcname="projectMM.exe")
            zf.write(readme, arcname="README.txt")
    finally:
        readme.unlink(missing_ok=True)
    print(f"package_desktop: wrote {out}")
    package_windows_installer(binary, version)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", default="",
                    help="Override the library.json version with the pipeline-computed semver "
                         "(compute_version.py), so the binary, the asset names and the update "
                         "badge all carry the same 3.0.0-dev.N. Empty = a local/dev build.")
    args = ap.parse_args()
    version = args.version or read_version()
    system = platform.system()
    machine = platform.machine().lower()

    # Clean only THIS host's build dir so a configure-flag change picked
    # up by this run gets a fresh CMakeCache. We don't touch the other
    # host's dir; on CI each runner only ever sees its own anyway.
    host_build = {"Darwin": BUILD_DIR_MACOS, "Windows": BUILD_DIR_WIN}.get(system, BUILD_DIR_LINUX)
    if host_build.exists():
        shutil.rmtree(host_build, ignore_errors=True)

    if system == "Darwin":
        if machine not in ("arm64", "aarch64"):
            print(f"package_desktop: unsupported macOS arch '{machine}'. "
                  f"projectMM 1.0 ships macOS arm64 only.")
            return 2
        binary = configure_and_build_macos(args.version)
        package_macos(binary, version)
        return 0

    if system == "Windows":
        binary = configure_and_build_windows(args.version)
        package_windows(binary, version)
        return 0

    if system == "Linux":
        if machine not in ("x86_64", "amd64"):
            print(f"package_desktop: unsupported Linux arch '{machine}'. "
                  f"Only x86-64 is packaged; other arches build from source.")
            return 2
        binary = configure_and_build_linux(args.version)
        package_linux(binary, version)
        return 0

    print(f"package_desktop: host '{system}' not supported. "
          f"projectMM ships macOS arm64, Windows x64 and Linux x64.")
    return 2


if __name__ == "__main__":
    sys.exit(main())
