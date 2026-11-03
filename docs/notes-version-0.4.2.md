POMPPC 0.4.2 adds **Linux x86-64**: a single-file AppImage that runs on Debian 11, Ubuntu 20.04 and anything newer. It's slower than on an Apple Silicon Mac, and the numbers are below. It also adds an **experimental Windows x64 package**, which nobody has yet run on a real PC: read its section before trying it. Nothing changes for macOS beyond the fixes listed here.

## What's new

- **`POMPPC-0.4.2-x86_64.AppImage`.** Download it, make it executable, run it. Your disks and settings live in `~/POMPPC`. The first-run assistant is the one from the Mac: it creates the disk, opens the Macintosh Garden page for the install disc, checks it by MD5, runs the installation, downloads the free 10.4.11 update from Apple, and installs the POMPPC driver.
- **Fixed:** starting POMPPC while the Mac's display was asleep crashed the front end.
- **Preferences.** Sound and the USB gamepad can be set there too, and the window shows the current display resolution and where to change it in Mac OS. An empty Library has a **Show Folder** button.

Details are in [CHANGELOG.md](https://github.com/habib256/pomppc/blob/0.4.2/CHANGELOG.md).

## How fast on Linux

Milliseconds per frame, lower is better, idle host. These were measured on a Core i7-10700F with an RTX 4060 Ti, **with a build tuned for that one machine** (`-O3 -march=native`, LTO, PGO). The AppImage is a generic build, so **expect it to be slower than this column** (about 10 % in DOOM 3). The Mac column is an Apple M4.

| Game | Linux PC (tuned build) | Apple M4 |
|---|---:|---:|
| DOOM 3, windowed | 78–80 | 28.9 |
| UT2004, windowed | 40 | 19.3 |
| Marble Blast, windowed | 8.3 | 6.2 |
| Zenerchi, windowed | 4.6 | 2.6 |
| Colin McRae, full screen, in the race | 82 | 41.7 |

So a demanding game runs about two to three times slower than on an M4. The goal for the PC is DOOM 3 at 50 ms per frame, and it isn't reached yet.

## Assets

| File | Contents |
|---|---|
| `POMPPC-0.4.2-x86_64.AppImage` | **Linux x86-64. Start here on a PC.** |
| `POMPPC-0.4.2-windows-x64.zip` | Windows 10 1903 or newer. **Experimental**, see below. |
| `POMPPC-0.4.2-macos-arm64.dmg` | The Mac app. Start here on a Mac. |
| `pomppc-0.4.2-macos-arm64.tar.xz` | The same in a folder, with the launch scripts, for Terminal use |
| `pomppc-0.4.2-guest.iso` | Guest CD only (`POMPPC_GUEST`), already in all of the above |
| `*.sha256`, `SHA256SUMS-ci.txt` | Checksums |

## Requirements on Linux

- A 64-bit PC with **glibc 2.31 or newer**. That's Debian 11, Ubuntu 20.04, Fedora 32, or later.
- **An OpenGL driver for your graphics card** (`libEGL`, `libGL`). POMPPC never bundles one: it uses yours. On Debian or Ubuntu, `sudo apt install libegl1 libgl1 libxcb1` if it says they're missing.
- **bash**, which every desktop distribution has. For the assistant: `curl`, `unzip` for Mac OS 9, and `zenity` or `kdialog` for the file chooser. Python 3 is optional and only serves Tiger's web relay.
- PulseAudio or PipeWire for sound. A pure ALSA system has no sound.
- **Your own** Mac OS install media, or the page the assistant opens. Nothing of Apple's is included.

```bash
chmod +x POMPPC-0.4.2-x86_64.AppImage
./POMPPC-0.4.2-x86_64.AppImage
```

If your system lacks FUSE 2, run it with `--appimage-extract-and-run`.

## Known limitations on Linux

- The automated build proves the package loads, that QEMU reaches Open Firmware and that its features are present. It can't prove Tiger, the GPU or sound, because its machines have neither a Tiger disk nor a GPU.
- The development host is Ubuntu 26.04 with an NVIDIA card. AMD and Intel drivers, other distributions and the oldest supported glibc haven't been tried.
- **Dropping files on the window and sharing a folder aren't available on Linux yet.** Dropping a disc image works.

## Windows x64 (experimental)

Unzip the folder anywhere and run `POMPPC.exe`. Your disks and settings live in `%USERPROFILE%\POMPPC`. You need Windows 10 1903 or newer and an OpenGL driver for your graphics card; POMPPC never bundles one.

**This package has only been built and tested by an automated machine with no graphics card, no sound and no Mac OS disk.** That machine proves that the package loads, that QEMU reaches Open Firmware with two processors, that the display reaches the front end, and that a second POMPPC is refused while one holds the disk. Everything else is unproven:

- OpenGL rendering on NVIDIA, AMD and Intel drivers. If the driver is refused, POMPPC falls back to software rendering, which most games can't use.
- Booting Tiger and the first-run assistant from start to finish.
- Two emulated processors under the Windows scheduler, over hours.
- Speed. Windows lacks several of the accelerations of the Linux build (native floating point, the fast clock, the near code buffer), so expect it to be slower than the Linux figures above. Nobody has measured how much.
- Sound through DirectSound, the keyboard, the mouse, high-density screens and full screen.
- Windows SmartScreen and antivirus software: the program isn't signed and it generates code at run time, so expect a warning at least.

Not available on Windows: Tiger's web relay, USB gamepad passthrough, the startup chime, sharing a folder and dropping files on the window. What's known and what isn't: [docs/publier-windows.md](https://github.com/habib256/pomppc/blob/0.4.2/docs/publier-windows.md), in French.

## Older limitations

The limitations of 0.4.1 still apply: Colin McRae hasn't been retried since the resolution fix on the Mac, Warcraft III sometimes fails to reach its first scene (starting it again works), Return to Castle Wolfenstein doesn't work yet, and Core Image isn't implemented. Project docs and code comments are in French. The README and these notes are in English.

## Licenses

POMPPC is GPL-3.0-or-later. Code compiled into QEMU is GPL-2.0-or-later. The Screamer device is MIT. Bundled third-party libraries are listed with their licenses in the package (`usr/share/pomppc/THIRD-PARTY-LICENSES` in the AppImage, `THIRD-PARTY-LICENSES` in the Windows folder).
