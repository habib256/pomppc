POMPPC 0.4.1 is a **real macOS app**: open the `.dmg`, drag POMPPC to Applications, and a single assistant in the window takes you from an empty disk to a running system. No Terminal needed on the Mac.

## What's new

- **POMPPC.app in a `.dmg`.** Your disks and settings live in `~/POMPPC`.
- **First-run assistant.** It creates the disk, opens the Macintosh Garden page for the install disc so you download it yourself and checks it by MD5, runs the installation, downloads the free 10.4.11 update from Apple, and installs the POMPPC driver by double-click. You can quit and resume. Mac OS X 10.4 Tiger and Mac OS 9.2.2 are both offered.
- **Share a folder of your Mac.** In **View ▸ Disks**, choose a folder: it appears as a disk in Mac OS. It's a copy, not a live view. Changes come back when you eject the disk or shut down, deleted files go to the Trash, and a conflict keeps both versions.
- **Drop files on the window** to get a *From Mac* disk that Mac OS can read and write, or drop a disc image to connect it read-only.
- **Choose your disks.** Several hard disks or SSDs, alone or together, from the **Disks** panel.
- **Don't lose work.** Closing POMPPC offers to shut Mac OS down properly. Sound, gamepad and system changes ask first.
- **Fixed:** writing to a USB disk under Tiger killed the emulated USB controller, and the keyboard and mouse with it.
- **Comfort.** Sharper text, a **Help** menu, **Preferences** (memory, processors), **Sharp** or **Smooth** image, **Try Without Saving**, screenshots, update check. The interface is now in English.

Details are in [CHANGELOG.md](https://github.com/habib256/pomppc/blob/0.4.1/CHANGELOG.md). Guest drivers are those of 0.4, plus the driver installer.

## Assets

| File | Contents |
|---|---|
| `POMPPC-0.4.1-macos-arm64.dmg` | **The app. Start here.** |
| `pomppc-0.4.1-macos-arm64.tar.xz` | The same in a folder, with the launch scripts, for Terminal use |
| `pomppc-0.4.1-guest.iso` | Guest CD only (`POMPPC_GUEST`), already in both of the above |
| `*.sha256` | Checksums |

## Requirements

- An Apple Silicon Mac running **macOS 15 or later**. Homebrew isn't needed. Built for macOS 15.0, but tested on an M4 under macOS 27 only: **macOS 15 itself and M1 haven't been tried yet**.
- **Your own** Mac OS install media, or the page the assistant opens. Nothing of Apple's is included.

## First launch

POMPPC is ad-hoc signed, not notarized, so macOS refuses it at first. Go to **System Settings ▸ Privacy & Security**, scroll down, click **Open Anyway** next to POMPPC, and confirm. You only do this once per version.

Upgrading from an archive: move your `disks/tiger.qcow2` to `~/POMPPC/disks/` before the first launch. See the [README](https://github.com/habib256/pomppc/blob/0.4.1/README.md).

## Known limitations

- Not fully tried: a complete installation from real media through the assistant (the steps were tried separately), Retina displays, and macOS's first-launch approval on a downloaded copy.
- The shared folder isn't live, and macOS may ask for permission if the folder is in Documents or Desktop (not tried). It takes the folder's size plus about 1 GB on your Mac while connected.
- Speed wasn't measured again since 0.4; QEMU changed only for USB.
- Colin McRae hasn't been retried since the resolution fix. Warcraft III sometimes fails to reach its first scene; starting it again works. Return to Castle Wolfenstein doesn't work yet, and Core Image isn't implemented.
- Project docs and code comments are in French. The README and these notes are in English.

## Licenses

POMPPC is GPL-3.0-or-later. Code compiled into QEMU is GPL-2.0-or-later. The Screamer device is MIT. Bundled third-party libraries are listed with their licenses in the package.
