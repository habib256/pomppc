POMPPC 0.4 brings **Quartz Extreme** to the prebuilt macOS package, at every display mode, and makes the heavy games much faster than 0.3.

## What's new

- **Accelerated desktop (Quartz Extreme), on by default.** Tiger's windows are composited on the host GPU. Every mode listed in System Preferences works, from 640×480 to 2560×1080. Switching to 1920×1080 used to give a striped screen; that's fixed.
- **Faster.** On an Apple M4, idle host, in ms/frame against 0.3: DOOM 3 28.9 vs 48.0, UT2004 19.3 vs 25.3, Zenerchi 2.6 vs 4.3. Colin McRae 41.7 at 1920×1080 vs 50.5 at 1024×768.
- **Colin McRae:** the false reflections on the car body are gone.
- **Single mouse cursor** in the ImGui front end.
- **Runs on macOS 15 and later**, without Homebrew (0.3 required macOS 26).
- **Better guide.** The README now covers checking the install, changing the resolution, upgrading and troubleshooting.

Details are in [CHANGELOG.md](https://github.com/habib256/pomppc/blob/0.4/CHANGELOG.md).

## Assets

| File | Contents |
|---|---|
| `pomppc-0.4-macos-arm64.tar.xz` | Repository at `0.4`, plus the patched `qemu-system-ppc`/`ppc64` 11.1.2, `qemu-img`, the ImGui front end, their libraries and the guest CD |
| `pomppc-0.4-guest.iso` | Guest CD only (`POMPPC_GUEST`), already in the archive |
| `*.sha256` | Checksums |

## Requirements

- An Apple Silicon Mac running **macOS 15 or later**. Homebrew isn't needed: the package carries its own libraries, built for macOS 15.0. It was developed and measured on an M4 under macOS 27, so frame times will differ on other chips: please report any problem.
- **Your own** Mac OS X 10.4 Tiger PowerPC install media. Nothing of Apple's is included.

## Getting started

```bash
tar xJf pomppc-0.4-macos-arm64.tar.xz && cd pomppc-0.4-macos-arm64
xattr -dr com.apple.quarantine .      # ad-hoc signed, not notarized
# put your Tiger media at images/MacOSX.4.iso, then:
scripts/00-create-disk.sh && scripts/10-install.sh
./run_tiger.sh
# inside Tiger, once, then reboot:
sudo sh /Volumes/POMPPC_GUEST/install.sh
```

**Upgrading:** extract the new archive, move your `disks/tiger.qcow2` into it, start Tiger, run the guest CD's `install.sh` again and reboot Tiger. Don't skip this step: an old guest driver can show a striped desktop.

Full instructions and troubleshooting are in the [README](https://github.com/habib256/pomppc/blob/0.4/README.md#quick-start-prebuilt-release-macos-on-apple-silicon).

## Known limitations

- Extract the archive somewhere with a short path, like `~/pomppc`: QEMU's monitor socket path must stay under 104 bytes.
- Colin McRae hasn't been retried since the resolution fix. DOOM 3, UT2004, Marble Blast and Zenerchi were, from a 1920×1080 or 2560×1080 desktop.
- Warcraft III sometimes fails to reach its first scene at launch; starting it again works.
- Return to Castle Wolfenstein does not work yet. Core Image is not implemented.
- Project docs and code comments are in French. The README and these notes are in English.

## Licenses

POMPPC is GPL-3.0-or-later. Code compiled into QEMU is GPL-2.0-or-later. The Screamer device is MIT. The bundled third-party libraries are listed with their licenses in `THIRD-PARTY-LICENSES/`.
