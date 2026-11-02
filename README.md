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

Mean frame time over the measured window, from matrix run `bench/matrice/20261001-tout` (the
0.1.0 release): windowed cell (full screen for Warcraft III and Colin McRae, which only have
that mode; both for Marble Blast); elsewhere the two modes differ by 2 % at most. Return to
Castle Wolfenstein has not been made to work yet.

These times have dropped since (TCG series and lighter guest plugin enabled by default on both
hosts). Idle host, ms/frame, Apple Silicon M4 (02/11/2026) / Linux x86-64 PC (01/11/2026): DOOM 3
windowed 28.9 / 78–80, UT2004 windowed 19.3 / 40, Marble Blast windowed 6.2 / 8.3, Zenerchi windowed
2.6 / 4.6, Colin McRae full screen *during the race* 41.7 / 82. Last full matrix run on Apple Silicon: 17/17 green (28/10/2026);
current table in `TODO.md`. The Linux x86-64 build is not released until its speed is
acceptable (target: DOOM 3 at 50 ms/frame).

**The limit is the CPU.** The G4 is emulated (there is no PowerPC virtualization on ARM or
x86), so the whole project is about taking work *away* from the emulated CPU: geometry,
textures, presentation and vertex-buffer conversion run on the host. Expect demo-grade frame
rates in the heavy titles, not native speed.

Quartz Extreme (the desktop composited through the host GPU) is on by default since release 0.4
on Apple Silicon, at every display mode Tiger offers (640×480 to 2560×1080), and in the repository
on Linux x86-64; `QE=0 ./run_tiger.sh` turns it off. What is **not** done: Core Image.

## Install the app (macOS on Apple Silicon)

You need an Apple Silicon Mac running **macOS 15 or later**, about 20 GB of free disk space, and
**Mac OS X 10.4 Tiger PowerPC installation media that you own** (`.iso`, `.cdr` or `.dmg`), or an
existing POMPPC disk. Nothing of Apple's is included. No Homebrew, Xcode or Terminal is needed on
the Mac.

1. Download `POMPPC-<version>-macos-arm64.dmg` from the
   [Releases](https://github.com/habib256/pomppc/releases) page, open it, and drag **POMPPC** onto
   **Applications**.
2. Open POMPPC from Applications. **macOS blocks it the first time**, because the app isn't
   notarized by Apple. Go to **System Settings ▸ Privacy & Security**, scroll down, click
   **Open Anyway** next to POMPPC, and confirm. You only do this once per version.
3. POMPPC asks for your Tiger media, or for an existing `.qcow2` disk. To install Tiger, follow its
   installer (in Disk Utility, erase the disk as *Mac OS Extended, Journaled*), close the window
   when it finishes, and open POMPPC again.
4. Once Tiger has started, install the 3D driver. Open Terminal inside Tiger (in Applications,
   Utilities) and type the line below, then restart Tiger. Do this again after each POMPPC update.

   ```sh
   sudo sh /Volumes/POMPPC_GUEST/install.sh
   ```

Your data is in the **`POMPPC` folder of your home folder** (`~/POMPPC`): your Tiger disk is
`disks/tiger.qcow2`. The app itself holds no data, so updating means replacing it in Applications.
Copy the disk while Tiger is off to back it up. Delete `~/POMPPC` to remove your data.

Then see [Changing the display resolution](#changing-the-display-resolution) and
[Troubleshooting](#troubleshooting).

## Advanced install (archive, Terminal)

The `.tar.xz` archive holds the same programs, with the scripts' variables at hand.

1. Download `pomppc-<version>-macos-arm64.tar.xz` from the
   [Releases](https://github.com/habib256/pomppc/releases) page, then extract it:

   ```bash
   tar xJf pomppc-*-macos-arm64.tar.xz && cd pomppc-*-macos-arm64
   xattr -dr com.apple.quarantine .      # binaries are ad-hoc signed, not notarized
   ```

   Extract it somewhere with a short path, like `~/pomppc`: QEMU's monitor socket path must
   stay under 104 bytes. The archive is this repository at the release tag, plus `bin/` (patched `qemu-system-ppc`,
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
   window is the ImGui front end (starts windowed; Alt+Enter, F11 or Ctrl+Cmd+F toggles full
   screen; absolute mouse in a window, captured relative mouse in full screen).
   `POMPPC_FRONTEND=native ./run_tiger.sh` uses QEMU's own window instead (Cocoa on macOS, GTK on Linux). With sound
   on, guest RAM is capped at 768 MB (the Screamer needs less than 1 GB); on macOS sound uses
   a [steadier audio profile](docs/audio-stabilite.md) (`POMPPC_AUDIO_PROFILE=default` restores
   QEMU's settings).

5. Install the 3D driver **once**, inside Tiger: the `POMPPC_GUEST` CD is already inserted.
   Open Terminal and type

   ```sh
   sudo sh /Volumes/POMPPC_GUEST/install.sh
   ```

   then reboot Tiger. An OpenGL application now reports
   `GL_RENDERER = "POMPPC qgpu (OpenGL host GPU)"`. `install.sh --remove` uninstalls. No Xcode
   is needed in the guest: the CD holds binaries built with Tiger's gcc 4.0.

6. Check the result. In Tiger's Terminal:

   ```sh
   kextstat | grep pomppc      # net.pomppc.POMPPCGPU and net.pomppc.POMPPCFsqrt
   system_profiler SPDisplaysDataType | grep -E "Resolution|Quartz"
   ```

   The second command should report `Quartz Extreme: Supported`. `/Volumes/POMPPC_GUEST/tools/`
   also holds small OpenGL test programs (`glwin`, `gltest`).

Your Tiger disk is the single file `disks/tiger.qcow2`: copy it while the VM is off to back it up.
The front end's menus and most script messages are in French: **Machine** holds the machine
controls, **Jeux** the game library, **Périphériques** the devices, **Vue** the display.

### Changing the display resolution

Use Tiger's **System Preferences ▸ Displays**, while Tiger runs. Every listed mode works, from
640×480 to 2560×1080, with Quartz Extreme on. Tiger remembers your choice across reboots. Both
**Thousands** and **Millions** of colors work.

`RES=1920x1080x32 ./run_tiger.sh` or `WIDE=1 ./run_tiger.sh` sets the firmware's mode at startup;
the mode saved by Tiger applies once its desktop starts.

### Upgrading from an earlier release

**With the app:** shut Tiger down, replace POMPPC in Applications, open it, install the driver again
and restart Tiger. Moving from the archive to the app: move your `disks/tiger.qcow2` to
`~/POMPPC/disks/` before opening the app, or choose it when the app asks.

**With the archive:** shut Tiger down and extract the new archive next to the old one. Move your
`disks/tiger.qcow2` into the new folder (and `images/`, if you keep your media there). Run
`./run_tiger.sh`, install the guest CD again as in step 5, and **reboot Tiger**.

**Do not skip the reinstall**: an old guest driver on a new release can show a striped or
wrongly sized desktop. Your applications and settings stay on the disk.

### Troubleshooting

| Symptom | What to do |
|---|---|
| macOS refuses to open POMPPC | System Settings ▸ Privacy & Security ▸ **Open Anyway**. With the archive, run the `xattr` line of its step 1. |
| The app quits or nothing appears | Read `~/POMPPC/.run/app.log` and attach it to your report. |
| `Tiger tourne déjà` ("Tiger is already running") | Close the other VM. Never delete `.run/tiger.lock`: two VMs writing to the same disk corrupt it. |
| Open Firmware `0 >` prompt | `boot cd:,\\:tbxi` while installing, `boot hd:,\\:tbxi` afterwards. |
| No 3D, or a striped desktop after an upgrade | Run the guest CD's `install.sh` again and restart Tiger. Check with the commands below. |
| The mouse is offset in the window | On Tiger 10.4.10 or older, start the archive with `TABLET_MARGIN=0 ./run_tiger.sh`. |
| A game wants relative mouse motion | Untick **Machine ▸ Souris absolue en fenêtre**, or play in full screen. |
| You need to type in a front-end field | Use **Machine ▸ Clavier → invité**. |
| Accelerated desktop misbehaves | With the archive, `QE=0 ./run_tiger.sh` turns Quartz Extreme off for that run. Please report it. |
| Warcraft III does not reach its first scene | Start it again: this launch failure is intermittent. |
| Return to Castle Wolfenstein | Not working yet. |

Bug reports: [GitHub issues](https://github.com/habib256/pomppc/issues). Please include the host
macOS version, the guest display mode and the output of these commands, typed in Tiger's Terminal:

```sh
kextstat | grep pomppc
system_profiler SPDisplaysDataType | grep -E "Resolution|Quartz"
```

Each launcher documents its variables in its header (`head -160 run_tiger.sh`): `GPU=0`,
`FASTFP=0`, `NET=0`, `SNAPSHOT=1` (throw-away disk), `QFB=1` (second monitor), and more.
Mac OS 9.2.2 runs with `./run_os9.sh` (single core, no 3D); `./run_os9.sh install` installs it
from the 9.2.2 CD in `disks/os9-install.iso` (or `OS9_CD=`).
Experimental CPU profiles from Tiger are available with
`OS9_PROFILE=complet SNAPSHOT=1 ./run_os9.sh disk`; the default remains the
standard `g4` profile. Scope and evidence: [OS 9 CPU profiles](docs/os9-optimisations-cpu.md).

**Compatibility.** Tested with Tiger **10.4.6**, the build whose `OpenGL.framework` was
reverse-engineered (`docs/re/`), and **10.4.11**, which the daily VMs of both development hosts
have run since October 2026. Other 10.4.x updates are untested; the plugin reads GLEngine's
internal structures, so a different GLEngine may not work.

## Building from source

Works on macOS (Apple Silicon) and Linux x86-64. QEMU 11.1.2 is cloned from upstream and patched;
nothing is fetched from third-party forks.

```bash
./scripts/build_qemu_qfb.sh        # clones QEMU v11.1.2 into ~/src/qemu, applies patches/,
                                   # builds, then probes the binary for each capability
cd frontend && ./setup.sh && cmake -S . -B build && cmake --build build -j   # front end
./tests/run-all.sh                 # test harness (--slow adds QEMU smoke tests up to Open Firmware)
```

QEMU's `configure` needs Python ≥ 3.9 with `tomli` (built in as `tomllib` from 3.11) and
`distlib`; on macOS a venv is enough
(`PYTHON=venv/bin/python3 ./scripts/build_qemu_qfb.sh`). The guest kext and plugin are compiled
**inside Tiger** with the Xcode 2.5 tools (gcc 4.0, SDK 10.4u): `./run_tiger.sh` burns a
`POMPPCSRC` CD with their sources, and `guest/gldriver/install.sh` builds and installs them
(or installs the prebuilt binaries of `disks/prebuilt`, burnt on the same CD, when gcc 4.0 is
missing). `scripts/package_release_macos.sh` produces the release archive
(`scripts/package_release_linux.sh` for Linux x86-64, not published); the full recipe is in
[docs/publier-macos.md](docs/publier-macos.md) (in French).

On a Linux x86-64 PC, `QEMU_FAST=1 ./scripts/build_qemu_qfb.sh` also builds `~/src/qemu/build-fast/`
(`-O3 -march=native`, LTO, PGO; about 10 % faster in DOOM 3 than the reference build at equal
settings), which `run_tiger.sh` then prefers
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
- **Verification**: native tests of the device core (`tests/qgpu_core_test.c`: 1,440 cases,
  660 software and 780 GL including DarkPlaces' GLSL permutations; 0 failures on the M4,
  01/11/2026), native replay of command dumps captured in the VM (`tests/qgpu_replay.c`),
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
