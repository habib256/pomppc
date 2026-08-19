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
./setup.sh                                  # fetch Dear ImGui into ./imgui
cmake -S . -B build && cmake --build build -j
```

Requires `gio-unix-2.0`, `glfw3`, OpenGL, and `gdbus-codegen` (glib dev tools).
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
