# POMPPC frontend — Dear ImGui shell over QEMU

Same *habillage* as the other Pommes (pom68k, POM1/2, POMIIGS): a Dear ImGui +
GLFW/OpenGL3 window. But POMPPC's engine is **QEMU**, so instead of an
in-process CPU core we drive a QEMU process through **GDBus + QMP**.

## How it works (voie 3 — affichage embarqué)

`QemuBridge` launches one of the repo's run scripts with `DBUS_DISPLAY=1`, which
switches QEMU to `-display dbus,p2p=on`. The bridge then:

1. connects to QEMU's QMP socket and, via `getfd` + `add_client @dbus-display`,
   hands QEMU one end of a `socketpair` — establishing a peer-to-peer GDBus bus
   (the exact bootstrap of `qemu/tests/qtest/dbus-display-test.c`);
2. registers an `org.qemu.Display1.Listener`, receiving `Scanout`/`Update`
   framebuffers that the render thread uploads as a texture (`ImGui::Image`);
3. forwards input back through `Keyboard.Press/Release` and
   `Mouse.SetAbsPosition/Press/Release`.

All D-Bus I/O lives on a private GLib thread; the render loop only touches the
framebuffer under a mutex.

## Build

```sh
./setup.sh                                  # fetch Dear ImGui (docking) into ./imgui
cmake -S . -B build && cmake --build build -j
```

Requires `gio-unix-2.0`, `glfw3`, OpenGL, and `gdbus-codegen` (glib dev tools).
Dear ImGui must be the **docking** branch (`IMGUI_HAS_DOCK`; the build stops
otherwise): `setup.sh` clones the pinned tag **`v1.92.9b-docking`** (override with
`IMGUI_TAG=…`), replaces an older non-docking `./imgui`, and offline falls back to
a sibling Pomme's checkout only if it is a docking build too. On macOS the window
asks for a GL 3.2 core forward-compatible context (a 3.0 request fails there).
The `org.qemu.Display1` bindings are generated at build time from the vendored,
preprocessed `dbus/dbus-display1.xml` (copied from our QEMU 9.2 tree).

## Run

```sh
../run_frontend.sh                 # from anywhere in the repo (same binary, resolved path)
./build/pomppc                     # boots Mac OS 9 (../run_os9.sh) by default
./build/pomppc ../run_tiger.sh     # boot Mac OS X 10.4 instead
RES=1024x768x32 ./build/pomppc     # pick the guest resolution (SMP=N works too)
```

The *default* launcher is derived from the binary's own location (`build/` → repo root), so it
works from any working directory; an explicit argument is taken relative to your cwd. `RES` and
`SMP` are forwarded to whichever run script is launched, and the **OS** menu switches guests at
runtime (each switch relaunches QEMU).

The keyboard is routed to the guest from the start — toggle it with
**Machine ▸ Clavier → invité** when you need to type into an ImGui field.

### Layout, ratio, plein écran (F1)

- The guest screen (**Écran**) is docked in the centre of a full-frame DockSpace, with
  **Ludothèque** on the right and **Journal** / **Bilan** below; every panel can be
  re-docked, the layout is saved in `<repo>/.run/imgui.ini` and **Vue ▸ Disposition par
  défaut** rebuilds it.
- The guest is always drawn at the largest rectangle with **its own aspect ratio**, centred,
  black bands around it — never stretched (**Vue ▸ Ajuster à la fenêtre**, the default).
  **Vue ▸ 50/100/150/200 %** still fixes the size in window mode (scrollbars if larger).
- **Vue ▸ Plein écran**, **Ctrl+Cmd+F** or **F11**: the host window goes to its monitor at
  native size (`glfwSetWindowMonitor`), menus and panels hidden, guest only, 4:3 guest on a
  16:9 screen = 1440×1080 between two 240-px bands on 1920×1080. **Same shortcut or Échap**
  goes back to the window at its previous size and position. These keys are not sent to the
  guest (Échap only while in full screen). On macOS, GLFW's own *Window ▸ Enter Full Screen*
  (which owns Ctrl+Cmd+F and opens a Spaces full screen) is unbound, and the window's native
  full-screen behaviour is disabled — the green button zooms instead.
- The pointer is mapped on the **rectangle actually drawn** (`SetAbsPosition` in guest pixels,
  relative motion scaled by the same factor, remainders carried); the bands send nothing.
- **Machine ▸ Souris absolue (tablette)** (on by default): mac99 has both a tablet
  (virtio-tablet from `run_os9.sh`, usb-tablet with `TABLET=1`) and a USB HID mouse; the HID
  mouse becomes QEMU's *current* pointer as soon as the guest polls it, which made OS 9's
  pointer relative (clicks landed wherever the accelerated guest cursor was). The frontend
  follows `Mouse.IsAbsolute` changes and makes the tablet current again (`query-mice` +
  `mouse_set`); untick it for games that want relative motion.
- **Bilan** shows the guest size, the drawn rectangle and scale, the window size and the
  pointer mode; **Journal** logs launches, CDs, scanout changes, switches and left clicks
  (host point → guest pixel).

`POMPPC_FE_SCRIPT` plays a test script through ImGui's input queue (same path as the real
mouse and keyboard, no macOS Accessibility permission needed) and takes captures, e.g.:

```sh
POMPPC_FE_SCRIPT="wait 75; fs; wait 3; shot /tmp/fs.png; click 1568 262; click 1568 262; \
esc; wait 2; shot /tmp/win.png; quit" ./build/pomppc ../run_os9.sh
```

Steps: `wait <s>`, `move|click <x> <y>` (window points), `fs` (Ctrl+Cmd+F), `f11`, `esc`,
`type <a-z 0-9 space .>`, `enter`, `shot <png>`, `quit`.

The headless probe defaults to the *other* guest, and always runs `-snapshot`:

```sh
./build/bridge_probe               # run_tiger.sh, waits for the first live Scanout → PPM
./build/bridge_probe ../run_os9.sh 60      # explicit launcher + timeout (s)
```

## Status

- **P1 (display)** ✅ — Scanout/Update over the bus (inline pixels, no
  shared-memory map yet). Proven headless with `bridge_probe`.
- **P2 (input)** ✅ — set-1 keycode table, absolute pointer (clamped to the
  guest surface), relative-motion fallback, wheel, scaled coordinate mapping.
- **P3 (menus → QMP)** ✅ — persistent QMP channel; menu bar drives
  `system_reset` (Machine ▸ Redémarrer), `stop`/`cont` (Pause), CD insert/eject
  on the `gamecd` drive (`blockdev-change-medium`/`eject`), and an OS selector
  that relaunches Tiger ↔ Mac OS 9. `Vue` zooms 50–200 %.
- **P4 (in progress)** —
  - ✅ **Unix.Map shared-memory fast path**: advertises
    `org.qemu.Display1.Listener.Unix.Map`; QEMU passes an fd we `mmap`
    (`ScanoutMap`) and only reports dirty rects (`UpdateMap`) — no pixels on
    the bus. Silently falls back to inline `Scanout`/`Update` for
    non-shareable surfaces. Verified: mac99 surfaces are shareable.
  - ✅ **Son / Manette** (Périphériques menu): both **on by default** (PulseAudio
    Screamer output + `usb-host` gamepad passthrough); toggling relaunches QEMU.
    The run scripts self-guard (no device → skipped).
  - ✅ **Clipboard peer** (`org.qemu.Display1.Clipboard`): we export our
    skeleton + a proxy on the main connection, `Register()`, and bridge the
    host clipboard (GLFW) both ways — host copy → `Grab` (accepted by QEMU);
    guest copy → QEMU `Grab`s us → we `Request` and set the host clipboard.
    Serial-counter ownership honored. **Caveat:** actual text landing *inside*
    the guest also needs a QEMU clipboard agent, which PPC Mac OS X / OS 9
    lack — so it's a correct, verified peer that stays latent until a guest
    agent (or another D-Bus peer) provides content.
  - ✅ **Ludothèque** (Jeux ▸ Ludothèque): a window listing the game CD images
    in `disks/cdr/` and `disks/`; click one to hot-insert it into the `gamecd` drive
    (`blockdev-change-medium`, verified on a live OS 9), with an eject button
    and the currently-mounted title. `games.iso` (the auto-mounted master) is
    excluded.
  - Remaining: OS profiles.
- **F1 (docked shell, ratio, host full screen)** ✅ — see *Layout, ratio, plein écran*
  above. Proven on the Mac host (1920×1080) with Mac OS 9 (`run_os9.sh`, `SNAPSHOT=1`,
  guest 640×480) and OpenBIOS (1024×768): window view 932×699 (4:3), full screen 1440×1080
  between 240-px bands (measured on `screencapture`), double-clicks on the *Shared* (full
  screen) and *Trash* (window, after coming back) desktop icons open them, keyboard typed
  into OpenBIOS in full screen with no stray key from the shortcut.
