# POMPPC — PowerPC Macs (Mac OS X 10.4 Tiger, Mac OS 9.2.2) on QEMU, with host-GPU 3D

POMPPC is a Power Mac G4 emulated by **QEMU/TCG** (`mac99`), patched so that Mac OS X Tiger runs
well on a modern machine (Apple Silicon or Linux x86-64): two CPU cores, sound, fast floating
point, networking, and above all a **paravirtual GPU** that renders Tiger's OpenGL applications
on the *host's* GPU.

Nothing of Apple's is modified inside the guest: the 3D path is one QEMU device, one kext and
one OpenGL driver plugin that `OpenGL.framework` loads like the driver of a real graphics card.

> Documentation under `docs/`, the dashboard (`TODO.md`), the changelog and code comments are
> written in **French**. This README and the release notes are in English. The original French
> README, with the full developer notes, is [README.fr.md](README.fr.md).

## What runs

Each game below is checked by an automated matrix (`tools/matrice/`, `docs/matrice-jeux.md`):
the frames the guest displayed are dumped, **replayed natively on the host and compared
pixel by pixel** with the VM's screen and with a reference image validated by eye; the plugin
must not fall back to Apple's software renderer. Run of 2026-10-01, Apple Silicon host, Tiger
10.4.6 guest, all 15 automated cells green:

| Game | OpenGL path | ms/frame (≈ fps) |
|---|---|---|
| Zenerchi | fixed pipeline | 4.3 (≈ 230) |
| Marble Blast Gold | fixed pipeline | 8.9–10.3 (≈ 100) |
| Warcraft III | vertex arrays (full screen) | 15.6 (≈ 64) |
| UT2004 Demo | VBOs, S3TC | 25.3 (≈ 40) |
| Nexuiz 2.5.2, GLSL renderer | GLSL | 35.3 (≈ 28) |
| Colin McRae Rally 2005 | ARB programs (full screen) | 47.8 (≈ 21) |
| DOOM 3 Demo | ARB programs, VBOs, 7 texture units, DXT | 56.6 (≈ 18) |
| Prey Demo | ARB programs, VBOs, DXT5 | 59.1 (≈ 17) |
| Nexuiz 2.5.2, ARB renderer | ARB programs | 80.2 (≈ 12) |

Mean frame time over the measured window, windowed and full-screen modes alike. Return to
Castle Wolfenstein has not been made to work yet.

**The limit is the CPU.** The G4 is emulated (there is no PowerPC virtualization on ARM or
x86), so the whole project is about taking work *away* from the emulated CPU: geometry,
textures, presentation and vertex-buffer conversion run on the host. Expect demo-grade frame
rates in the heavy titles, not native speed.

What is **not** done: Quartz Extreme / accelerated desktop compositing, Core Image.

## Quick start (prebuilt release, macOS on Apple Silicon)

Requirements: an Apple Silicon Mac running **macOS 26 (Tahoe) or later** (the bundled Homebrew
libraries are built for it), and **Mac OS X 10.4 Tiger PowerPC installation media that you
own**. No Apple ROM is needed: OpenBIOS boots Tiger. Nothing of Apple's is distributed here.

1. Download `pomppc-<version>-macos-arm64.tar.xz` from the
   [Releases](https://github.com/habib256/pomppc/releases) page, then extract it:

   ```bash
   tar xJf pomppc-*-macos-arm64.tar.xz && cd pomppc-*-macos-arm64
   xattr -dr com.apple.quarantine .      # binaries are ad-hoc signed, not notarized
   ```

   The archive is this repository at the release tag, plus `bin/` (patched `qemu-system-ppc`,
   `qemu-system-ppc64`, `qemu-img`, the `pomppc` front end), `lib/` (their libraries),
   `share/qemu/` (firmware) and `pomppc-guest.iso` (the guest package). `config.env` picks up
   `bin/` automatically.

2. Put your Tiger media in `images/MacOSX.4.iso` (or set `INSTALL_MEDIA=` in `config.env`). An
   `.iso` works as is; convert a compressed `.dmg` first
   (`hdiutil convert in.dmg -format UDTO -o out` then rename `out.cdr` to `.iso`).

3. Install Tiger (the installer takes a while under emulation):

   ```bash
   scripts/00-create-disk.sh   # creates disks/tiger.qcow2 (16 GB)
   scripts/10-install.sh       # boots the installer
   ```

   If you land at the Open Firmware `0 >` prompt instead of booting, type `boot cd:,\\:tbxi`
   during installation and `boot hd:,\\:tbxi` afterwards. If the installed disk does not boot
   because `BootX` is missing, see `scripts/inject-bootx.sh`.

4. Run Tiger with everything enabled:

   ```bash
   ./run_tiger.sh
   ```

   Two cores (MTTCG), Screamer sound, fast FPU, paravirtual GPU, user-mode network; the
   window is the ImGui front end. `POMPPC_FRONTEND=native ./run_tiger.sh` uses QEMU's own
   Cocoa window instead.

5. Install the 3D driver **once**, inside Tiger: the `POMPPC_GUEST` CD is already inserted.
   Open Terminal and type

   ```sh
   sudo sh /Volumes/POMPPC_GUEST/install.sh
   ```

   then reboot Tiger. An OpenGL application now reports
   `GL_RENDERER = "POMPPC qgpu (OpenGL host GPU)"`. `install.sh --remove` uninstalls. No Xcode
   is needed in the guest: the CD holds binaries built with Tiger's gcc 4.0.

Each launcher documents its variables in its header (`head -70 run_tiger.sh`): `GPU=0`,
`FASTFP=0`, `NET=0`, `SNAPSHOT=1` (throw-away disk), `QFB=1` (second monitor), and more.
Mac OS 9.2.2 runs with `./run_os9.sh` (single core, no 3D).

**Compatibility.** Tested with Tiger **10.4.6**, the build whose `OpenGL.framework` was
reverse-engineered (`docs/re/`). Other 10.4.x updates are untested; the plugin reads
GLEngine's internal structures, so a different GLEngine may not work.

## Building from source

Works on macOS (Apple Silicon) and Linux x86-64. QEMU 11.1.2 is cloned from upstream and patched;
nothing is fetched from third-party forks.

```bash
./scripts/build_qemu_qfb.sh        # clones QEMU v11.1.2 into ~/src/qemu, applies patches/,
                                   # builds, then probes the binary for each capability
cd frontend && ./setup.sh && cmake -S . -B build && cmake --build build -j   # front end
./tests/run-all.sh                 # test harness (add --slow for boot tests)
```

QEMU's `configure` needs Python ≥ 3.9 with `tomli` and `distlib`; on macOS a venv is enough
(`PYTHON=venv/bin/python3 ./scripts/build_qemu_qfb.sh`). The guest kext and plugin are compiled
**inside Tiger** with the Xcode 2.5 tools (gcc 4.0, SDK 10.4u): `./run_tiger.sh` burns a
`POMPPCSRC` CD with their sources, and `guest/gldriver/install.sh` builds and installs them.
`scripts/package_release_macos.sh` produces the release archive (`scripts/package_release_linux.sh`
for Linux x86-64).

On a Linux x86-64 PC, `QEMU_FAST=1 ./scripts/build_qemu_qfb.sh` also builds `~/src/qemu/build-fast/`
(`-O3 -march=native`, LTO, PGO; ~20 % faster in games), which `run_tiger.sh` then prefers
(`QEMU_FAST=0` for the reference build): see [docs/binaire-rapide-x86.md](docs/binaire-rapide-x86.md).

## How it works

- **QEMU 11.1.2 + patches** (`patches/`, applied by `scripts/build_qemu_qfb.sh`): SMP for `mac99`,
  the Screamer (AWACS) sound device, the `qfb-pci` paravirtual framebuffer, the `qgpu-pci`
  paravirtual GPU, a fast FPU mode that hands PowerPC floating point to the host FPU with
  bit-identical results, and a series of TCG speed-ups for the G4 (`patches/README.md`).
- **The 3D path, in three layers**, with a contract split across two headers
  (`patches/qgpu/`): `qgpu_abi.h` is the *transport* (registers, doorbell, per-client slices —
  the only thing the kext knows) and `qgpu_proto.h` the *semantics* (opcodes, state keys,
  formats — device and plugin only).
  1. **OpenGL plugin `GLDriver-POMPPC`** (`guest/gldriver/`): loaded by `OpenGL.framework` as
     a graphics-card driver. It reads GLEngine's state (offsets found by reverse engineering,
     `docs/re/`) and emits qgpu commands: fixed pipeline and ATI combiners, OpenGL 1.5, ARB
     vertex/fragment programs, GLSL, VBOs read directly by the host, S3TC, GPU-side copies
     and direct presentation.
  2. **Kext `POMPPCGPU`** (`kext/POMPPCGPU/`): an IOKit accelerator that shares a window with
     the device, split into client slices, with an asynchronous doorbell; it cleans up after
     dead clients and never interprets an opcode.
  3. **Device `qgpu-pci`** (`patches/qgpu/`): validates and decodes everything
     (`qgpu-core.c`), renders with the host's OpenGL (CGL on macOS, EGL on Linux), and ships a
     software reference rasterizer (`qgpu-soft.c`) so the protocol can be tested without a VM.
- **Verification**: native tests of the device core (`tests/qgpu_core_test.c`, software and
  GL backends), native replay of command dumps captured in the VM (`tests/qgpu_replay.c`),
  guest scenes compared with Apple's renderer, and the game matrix above.

The project's rule: **no fallback, extend the protocol**. Under ARB programs a fallback to
Apple's renderer kills the game, so a missing feature is added to the protocol instead.

Design documents (in French): `docs/architecture.md`, `docs/gpu-3d-tiger.md`,
`docs/protocole.md` (current protocol, v22, transport ABI v19), `docs/flottant-rapide.md`
(fast FPU), `docs/matrice-jeux.md` (game matrix).

## Repository map

| Path | Contents |
|---|---|
| `run_tiger.sh`, `run_os9.sh`, `run_frontend.sh`, `config.env` | launchers and the single configuration file |
| `scripts/` | disk creation, install, boot, QEMU build, capability probe, release packaging |
| `patches/` | QEMU and firmware patches; `patches/README.md` says what is applied |
| `kext/` | Tiger kexts: `POMPPCGPU` (GPU transport), `POMPPCQFB` (framebuffer), `POMPPCFsqrt` |
| `guest/` | Tiger OpenGL plugin, guest package installer, test programs |
| `frontend/` | Dear ImGui front end driving QEMU over D-Bus |
| `tests/` | test harness, native device tests, dump replayer |
| `tools/` | reverse-engineering tools, guest dev loop, game matrix |
| `docs/` | design, protocol versions, reverse-engineering notes, reports |

## License

- POMPPC's own code (launchers, kexts, OpenGL plugin, front end, tools, tests, documentation):
  **GPL-3.0-or-later**, see [LICENSE](LICENSE).
- Code compiled into QEMU (`patches/qgpu/`, `patches/qfb/`, and the patch series against
  QEMU): **GPL-2.0-or-later**, like QEMU, see [LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt).
  `qgpu_abi.h`, shared with the kext, is GPL-2.0-or-later too.
- The Screamer device (`patches/screamer/`) is Mark Cave-Ayland's, **MIT**; POMPPC's changes are
  under the same license.
- `patches/smp-mac99/openbios-smp-screamer.elf` is OpenBIOS (GPL-2.0); its exact source is
  `patches/smp-mac99/openbios-smp-screamer-source.patch`.
- The `qfb` register interface comes from Solra Bizna's
  [mac_qfb_driver](https://github.com/SolraBizna/mac_qfb_driver); the SMP bring-up builds on
  BALATON Zoltan's `mac99` work on qemu-ppc.

Release archives also contain the third-party libraries they bundle, with their licenses in
`THIRD-PARTY-LICENSES/`.

Mac OS, Mac OS X, Power Mac and OpenGL-related names are trademarks of their owners. This
project distributes no Apple software; you need your own copy of the operating system.
