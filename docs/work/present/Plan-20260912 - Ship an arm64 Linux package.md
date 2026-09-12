# Plan: ship an arm64 Linux package

## Context

projectMM releases Linux binaries for x86-64 only. An arm64 board (Raspberry Pi, NanoPi, most SBCs) has to clone the repo and compile, which takes an hour on first run and needs swap on a 1 GB board or the compiler is OOM-killed with no error. [installing-on-linux.md](docs/tutorials/installing-on-linux.md) documents that route, and the PO walked it on a NanoPi R28S: `.local` did not resolve, the routing table was skipped by a deep link, and the build step was still ahead.

The blocker was never the code. `package_desktop.py` hand-rolls the `.deb` (an `ar` archive of two tarballs plus a control file), so nothing about it is x86-specific; it was that no arm64 Linux machine was available to build on. GitHub's arm64 runners went generally available for public repositories on 2025-08-07, which removes that. The release workflow already anticipates this at [release.yml:629](.github/workflows/release.yml): *"An arm64 image needs an arm64 Linux build in `build-linux` first, at which point this job gains a `platforms:` line and nothing else changes."*

Outcome: a user on a Pi or a NanoPi runs `sudo apt install ./projectmm_X.Y.Z_arm64.deb` and is done, and the container image runs on arm64 hosts.

## What the exploration settled

- **The glibc floor is the whole decision.** The amd64 binary is built on `ubuntu-24.04` (glibc 2.39) and requires ≥ 2.38. Raspberry Pi OS Bookworm ships **2.36**, so an arm64 build on `ubuntu-24.04-arm` would install cleanly and then die with `GLIBC_2.38 not found` — the exact failure [Dockerfile:68](Dockerfile) records hitting on debian12. Building on **`ubuntu-22.04-arm` (glibc 2.35)** covers Bookworm (2.36), Debian 13 trixie (2.41) and Ubuntu 24.04 (2.39), because glibc symbol versioning is backward compatible.
- **GCC 12.3.0 is preinstalled on `ubuntu-22.04-arm`**, so the low floor costs no compiler setup (no PPA, no sysroot, no static-link tradeoff). [CMakeLists.txt:73](CMakeLists.txt) already demotes four false-positive warnings for `GNU AND VERSION_LESS 16`, so GCC 12 is inside the range the project already supports.
- **The PO's NanoPi is not the constraint**: measured over ssh as Debian 13 trixie, **glibc 2.41**, GCC 14.2.0, aarch64, 964 MB RAM with **swap 0**, cmake not installed. It installs a package built against any of these floors. Its value here is *verification on real arm64 hardware*, which CI cannot do.
- **Three hardcoded strings block arm64** in [moondeck/ci/package_desktop.py](moondeck/ci/package_desktop.py): the arch gate at line 612, `"Architecture: amd64"` in the control file at line 164, and the `_amd64.deb` / `-linux-x64-` names at lines 170 and 100.
- **The container job needs one line plus a glob fix.** [release.yml:700](.github/workflows/release.yml) pins `platforms: linux/amd64`, and its Dockerfile step selects `dist/projectmm_*_amd64.deb` at line 674.

## Steps

### 1. Teach `package_desktop.py` about arm64

In [moondeck/ci/package_desktop.py](moondeck/ci/package_desktop.py), derive the Debian arch rather than hardcoding it:

- `main()` line 612: accept `aarch64`/`arm64` alongside `x86_64`/`amd64`, and pass the resolved Debian arch (`amd64` or `arm64`) down.
- `package_deb()` line 164: `f"Architecture: {deb_arch}\n"`; line 170: `projectmm_{version}_{deb_arch}.deb`.
- `package_linux()` line 100: `projectMM-linux-{label}-v{version}.tar.gz` where label is `x64` or `arm64`; line 102 passes the matching `platform_label` to `readme_text()`.
- Update the module docstring at lines 7-8, which lists the produced asset names.

One new helper, `_linux_arch()`, returning `(deb_arch, tarball_label, readme_label)`. No behaviour change on x86-64: same names, same bytes.

### 2. Add the `build-linux-arm64` job

In [.github/workflows/release.yml](.github/workflows/release.yml), copy `build-linux` (line 392) to `build-linux-arm64` with `runs-on: ubuntu-22.04-arm`, uploading artifact name `desktop-linux-arm64`. The tag/version resolution blocks are identical by design across the build jobs, so keep them identical here too.

Add the job to `release`'s `needs:` list (line 626). The "Flatten artifacts into dist/" step already globs every artifact, so the new assets ride along with no change.

**Carry a comment explaining the runner choice**, because `ubuntu-22.04-arm` next to `ubuntu-latest` looks like an oversight: 22.04 is deliberate for glibc 2.35, the floor that keeps Raspberry Pi OS Bookworm supported.

### 3. Publish a multi-arch container image

In the `publish-container` job: download both Linux artifacts, select the `.deb` per platform, and set `platforms: linux/amd64,linux/arm64`. Delete the now-stale "amd64 only" comment at line 629.

The base image is already multi-arch (`gcr.io/distroless/cc-debian13`), but it is **pinned by digest**, and a digest names one architecture. Switch that pin to the multi-arch index digest, keeping the pin-not-tag discipline and its comment.

### 4. Documentation

- [docs/tutorials/installing-on-linux.md](docs/tutorials/installing-on-linux.md): the routing table at line 19 sends `aarch64` to "Build from source"; it becomes "Install the package". Rewrite the `x86-64:` section as one **Install the package** section covering both arches, keep build-from-source as the fallback for other distributions, and update the line-22 paragraph that states the binaries are x86-64 only. Keep the swap note: it still applies to anyone building from source on a 1 GB board.
- [README.md:171](README.md): the asset list gains the arm64 line.
- [Dockerfile:32](Dockerfile) and line 170 of the tutorial: both say the image is amd64-only.
- A [MIGRATING](docs/reference/MIGRATING.md) entry is **not** needed: this adds assets and breaks nothing.

## Verification

1. `uv run moondeck/ci/package_desktop.py --version 0.0.0-test` on macOS still produces the unchanged `.dmg` path (the refactor must not touch non-Linux branches).
2. Push the branch and let the release workflow run on a dispatch: `build-linux` and `build-linux-arm64` both green, and the release carries `projectmm_X.Y.Z_amd64.deb`, `projectmm_X.Y.Z_arm64.deb` and both tarballs.
3. `dpkg-deb -I projectmm_X.Y.Z_arm64.deb` reports `Architecture: arm64`.
4. **On the NanoPi** (the gate that matters, PO's call): `sudo apt install ./projectmm_*_arm64.deb`, then `projectMM`, then open `http://192.168.1.156:8080` and see the UI render. This is the only step that proves the binary runs on real arm64 hardware.
5. On the NanoPi: `ldd $(which projectMM)` resolves every library, and `strings` confirms no `GLIBC_2.3[89]` requirement above 2.35.
6. `docker buildx imagetools inspect ghcr.io/moonmodules/projectmm:latest` lists both `linux/amd64` and `linux/arm64`.
7. Ideally on a Raspberry Pi OS Bookworm board if one is available, since 2.36 is the floor this whole design protects. Absent a Pi, step 5's symbol check is the proxy.

## Risks

- **GCC 12 vs the CI's 13.** The four demoted warnings are already handled for GCC < 16, but GCC 12 is a version nothing currently compiles with. A new warning there fails the job; the fix is the same demotion block, and the job failing loudly in CI is the right place to find out.
- **22.04 leaves GitHub-hosted support in 2027.** The fallback is a sysroot build on a newer runner, which is more setup; worth a backlog note rather than building now.
- **Multi-arch digest pinning.** Repinning the distroless base to an index digest is the one step where a wrong value silently produces an amd64-only image; step 6 is what catches it.
