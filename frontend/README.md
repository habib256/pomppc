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

Requires `gio-unix-2.0`, `glfw3`, OpenGL, and `gdbus-codegen` (glib dev tools); on Linux,
`libpulse-simple` (optional) for the startup chime.
Dear ImGui must be the **docking** branch (`IMGUI_HAS_DOCK`; the build stops
otherwise): `setup.sh` clones the pinned tag **`v1.92.9b-docking`** (override with
`IMGUI_TAG=…`), replaces an older non-docking `./imgui`, and offline falls back to
a sibling Pomme's checkout only if it is a docking build too. On macOS the window
asks for a GL 3.2 core forward-compatible context (a 3.0 request fails there).
The `org.qemu.Display1` bindings are generated at build time from the vendored,
preprocessed `dbus/dbus-display1.xml` (copied from our QEMU 9.2 tree; the 11.1 interface only adds optional members).

## Run

```sh
../run_tiger.sh                    # Tiger in ImGuiDock, maximum tested optimizations
POMPPC_FRONTEND=native ../run_tiger.sh  # native QEMU window for diagnostics
../run_frontend.sh                 # from anywhere in the repo; no argument = ../run_os9.sh
./build/pomppc                     # Mac OS 9 (../run_os9.sh), or the OS kept in .run/imguidock.conf
./build/pomppc ../run_tiger.sh     # boot Mac OS X 10.4 instead
RES=1024x768x32 ./build/pomppc     # pick the guest resolution (SMP=N works too)
```

`run_frontend.sh` takes `frontend/build/pomppc` (or `bin/pomppc` in the published package),
rebuilds it first (`setup.sh` + cmake) when a file of `src/`, `CMakeLists.txt` or `setup.sh` is
newer than the binary (on failure it warns and launches the old one), and always passes a
launcher — `run_os9.sh` when given none — so the OS kept in `.run/imguidock.conf` only applies
to `./build/pomppc` started without an argument.

The window title names the system (`POMPPC — Mac OS X 10.4 Tiger`). The version of the QEMU actually
launched, read from its QMP greeting, is shown in the window instead, so a `QEMU_BIN` pointing at
another tree stays visible.

**Réglages gardés** : à l'arrêt (**Machine ▸ Quit**, fermeture de la fenêtre, Ctrl+C ou `SIGTERM`),
ImGuiDock écrit `.run/imguidock.conf` — OS, son, manette, clavier et souris, vue et zoom,
fenêtres affichées, plein écran, géométrie de la fenêtre — et le relit au lancement suivant
(un lanceur passé en argument prime sur l'OS retenu ; le plein écran est écrit pour
information seulement, ImGuiDock démarre toujours en fenêtre depuis le 26/10). Texte « clé = valeur », modifiable à la
main ; la disposition des fenêtres reste dans `.run/imgui2.ini`, le carillon dans
`.run/chime.conf`. Une épreuve `POMPPC_FE_SCRIPT` ne le lit ni ne l'écrit.

The *default* launcher is derived from the binary's own location (`build/` → repo root), so it
works from any working directory; an explicit argument is taken relative to your cwd. `RES` and
`SMP` are forwarded to whichever run script is launched, and the **OS** menu switches guests at
runtime (each switch relaunches QEMU).

Tiger's launcher delegates to this frontend (`exec run_frontend.sh run_tiger.sh`) before
taking its disk lock, after checking the QEMU version, unless `POMPPC_FRONTEND=native`,
`HEADLESS`, `POMPPC_DISPLAY` or `DBUS_DISPLAY` is set (any other `POMPPC_FRONTEND` value
than `imgui`/`native` stops it). The bridge's `DBUS_DISPLAY=1` (plus `QMP_SOCK`) then
selects QEMU directly, without recursion.
Tiger enables the tested CPU/TCG optimizations, GPU copies and GLSL by default;
explicit environment overrides (including `0`) are preserved. The headless and
explicit native-display paths remain available for automation.

The keyboard is routed to the guest from the start — toggle it with
**Machine ▸ Keyboard to Guest** when you need to type into an ImGui field.

### Carillon de démarrage (hôte macOS et Linux)

**Devices ▸ Startup Chime (Host)** règle l'activation, le volume
(0–100 %) et le chemin d'un WAV/AIFF (Entrée pour valider). Ces préférences
restent dans `.run/chime.conf`, indépendamment du son Screamer de Tiger.
Le lecteur (NSSound sur macOS ; PulseAudio `pa_simple` sur Linux, serveur pulse de PipeWire
compris, WAV et AIFF, `src/StartupChimeLinux.cpp`) est asynchrone : un lancement QEMU réussi
puis son premier
affichage valide déclenchent une seule lecture. Une pause, une nouvelle image,
un redimensionnement ou une réactivation du son ne la rejouent pas. Une relance
de QEMU ou **Machine ▸ Restart** réarme le carillon. Un redémarrage demandé
depuis l'invité seul n'est pas détecté. Une erreur audio est journalisée sans
empêcher le démarrage de la VM. Aucun son dans les sondes headless.

Surcharges au lancement : `POMPPC_CHIME=0`, `POMPPC_CHIME_VOLUME=0.5` (0–1),
`POMPPC_CHIME_FILE=/chemin/son.aiff`. Sous Linux sans `libpulse-simple` à la construction, ou
sur un autre hôte, le lecteur signale explicitement que cette fonction n'est pas disponible.

**Identification :** `sysctl hw.model` dans notre Tiger retourne `PowerMac3,1`,
le Power Mac G4 AGP (Sawtooth), et non un Macintosh LC II. Le profil attend
`disks/chimes/powermac3-1-4.2.8.wav`, à fournir localement depuis le firmware
Apple correspondant ; le dépôt ne distribue pas ce son propriétaire.
Le nom du fichier seul ne certifie pas sa provenance. Un chemin personnalisé
reste un choix utilisateur, sans identification automatique du carillon.
Ne pas confondre ce son du firmware avec un son propre à Mac OS X Tiger.

Extraction reproductible après acceptation de la licence Apple :
[`G4_FW_Update_4.2.8.smi.bin`](https://download.info.apple.com/Apple_Support_Area/Apple_Software_Updates/English-North_American/Macintosh/Power_Mac_G4/G4_FW_Update_4.2.8.smi.bin),
ouvrir l'image Disk Copy en lecture seule, puis passer le **fichier de données**
`Power Mac G4 Firmware` à `python3 scripts/extract_g4_chime.py "…/Power Mac G4 Firmware"`.
Ne jamais lancer le programme de mise à jour du firmware dans la VM.
L'extracteur refuse tout firmware dont le SHA-256 diffère de
`8d3e8ca5a01973b3100144affdc88a21692a89c5e3ef4054bcc4774ce9ecf212`.
Il lit le champ `BOOT-BEEP` du répertoire `sboot`, décode les 1 722 paquets IMA4,
et produit 110 208 échantillons PCM16 mono à 44 100 Hz (2,499 s).
SHA-256 du WAV : `f685b917bc5d277986b1c4c164f70d1aae2e8fcc10f8888841ba0eaf67ccc6c5`.
Le PCM extrait a été comparé au décodage Apple `afconvert` : **identique à l'octet**.
Le son est celui de cette version du firmware G4 ; cela ne prétend pas que
chaque modèle de Mac possède nécessairement un enregistrement différent.

Vérification : `ctest --test-dir frontend/build --output-on-failure`
teste le déclenchement unique, la coupure, les valeurs invalides et la
persistance des préférences du carillon (`startup_chime`), ainsi que la politique du pointeur
(`pointer_policy`) et `.run/imguidock.conf` (`frontend_settings`) ; cela ne prouve pas l'écoute sur les haut-parleurs.
Sur le démarrage réel du 27/09 (`bench/tiger-chime-final.log`, copie du PC sous
`bench/mac-20261009/`), NSSound accepte
une seule lecture du WAV G4 à 50 %, après le premier scanout ; le passage
640×480 → 1024×768 ne la rejoue pas. L'utilisateur a confirmé à l'écoute :
« Le chime est bon ».

### Layout, ratio, plein écran (F1)

- The guest screen (**Screen**) is docked in the centre of a full-frame DockSpace. Panels are closed
  by default (see *Window size*); once opened, **Library** goes on the right and **Log** / **Status**
  below. Every panel can be re-docked, the layout is saved in `<repo>/.run/imgui2.ini` and **View ▸ Default Layout** rebuilds it.
- The guest is always drawn at the largest rectangle with **its own aspect ratio**, centred,
  black bands around it — never stretched (**View ▸ Fit to Window**, the default).
  **View ▸ 50/100/150/200 %** still fixes the size in window mode (scrollbars if larger).
- **View ▸ Full Screen**, **Alt+Enter**, **Ctrl+Cmd+F** or **F11**: the host window goes to its monitor at
  native size (`glfwSetWindowMonitor`), menus and panels hidden, guest only, 4:3 guest on a
  16:9 screen = 1440×1080 between two 240-px bands on 1920×1080. **Same shortcut (Alt+Enter, F11 or Ctrl+Cmd+F)**
  goes back to the window at its previous size and position. ImGuiDock always starts in a
  window (since 26/10/2026, the full screen of the previous session is not restored). These keys
  (Enter, F, F11) are not sent to the guest; the modifiers held with them (Alt, Ctrl, Cmd) are,
  as ordinary keys. **Escape is always forwarded to the guest**, including in full screen,
  so games can open their menus without releasing capture. On macOS, GLFW's own *Window ▸ Enter Full Screen*
  (which owns Ctrl+Cmd+F and opens a Spaces full screen) is unbound, and the window's native
  full-screen behaviour is disabled — the green button zooms instead.
- The pointer is mapped on the **rectangle actually drawn** (`SetAbsPosition` in guest pixels,
  relative motion scaled by the same factor, remainders carried); the bands send nothing.
- **Setup.** With `POMPPC_APP` set and no disk, or with `.run/setup.state` present, the Screen window
  shows the setup (`SetupFlow`, `SetupUi`): pages without a VM, then a bar above the guest. Steps with an
  installer pass `POMPPC_NO_REBOOT=1`, so QEMU exits when the guest restarts and the next step starts.
  The driver step ends when `qom-get gpu0 guest-driver` is true (IRQ_MASK armed by the kext).
  `POMPPC_SETUP_CHOOSE=<file>` replaces macOS's file window in a test.
- **Dropping files.** Files or folders become a writable HFS+ disk in `Exchange/`, hot-plugged as
  `usb-storage` (QMP `blockdev-add`, `device_add`); a lone disc image is connected read-only, a `.dmg`
  being converted first. **Devices ▸ Eject** removes the device, then opens the disk on the host. The IDE CD
  drive can't do this: Mac OS locks it while a CD is mounted, and a forced swap goes unseen by Tiger.
- **Shared folder (`SharedFolder`).** A chosen folder becomes an HFS+ image in `.run/shared-disk.dmg`, with a
  manifest of sizes and dates (`shared-disk.list`), hot-plugged as `usb-storage`. `query-block` shows the guest's
  eject (`tray_open`). The changes come back by a three-way comparison of manifest, image and folder: new or
  changed files are copied, conflicts keep both, deletions go to the Trash, and only after a clean eject or
  shutdown. The image is never overwritten while it exists: a crash leaves it to be brought back at the next run.
  QEMU's `vvfat` was rejected: 503 MB at most, and files shown empty under Tiger. `POMPPC_SHARED_CHOOSE=<dir>`
  replaces macOS's window and `POMPPC_TRASH=<dir>` the Trash in a test.
- **Preferences, image, session.** Memory and processors go to the launchers (`RAM_MB`, `OS9_RAM`, `SMP`);
  sound and gamepad are set there too, under the same **Apply and Restart**. The window shows the current
  resolution and where to change it in Mac OS: `RES` doesn't reach the guests' desktop.
  **Sharp Image** multiplies the pixels by an integer through an FBO, then scales in bilinear; **Smooth**
  is bilinear. **Try Without Saving** passes `SNAPSHOT=1` for this session only.
- **Quit and restarts.** Closing the window, or **Machine ▸ Quit**, asks first while the guest runs:
  **Shut Down** sends the power key (`send-key power`), the guest shows its own dialog, and the front
  end closes when QEMU exits. `system_powerdown` has no effect on mac99. Sound, gamepad, system and
  **Machine ▸ Restart** ask too. SIGTERM doesn't ask. When QEMU stops, the Screen window says so, with
  **Start** or **Try Again**, and **Show Log** if `.run/app.log` exists.
- **Window size.** Panels are closed by default and the window follows the guest at 100 %, 0.7 s after
  its last mode change, until the user resizes it (**View ▸ Fit Window to Guest** turns it back on).
  Settings older than `disposition = 2` start again from this state, once.
- **Disks** (**View ▸ Disks**) chooses the guest's disks, per system: the
  startup disk, and up to two more at the same time (one under Mac OS 9 when `disks/games.iso`
  exists: the machine has 4 IDE slots, shared with the CDs). It lists the qcow2 images of `disks/`,
  creates new ones, and marks each disk HD or SSD. **Apply and Restart** restarts the VM. The choice
  is kept in `.run/imguidock.conf`; `DISK` or `OS9_DISK` set by hand take priority. SSD only sets
  `rotation_rate=1` and a model name in the guest: Tiger has no notion of SSD, and speed doesn't
  change. The launchers receive the choice through `POMPPC_EXTRA_DISKS` (`scripts/disques.sh`).
- **Machine ▸ Absolute Mouse in Window** (on by default): mac99 has both a tablet
  (virtio-tablet from `run_os9.sh`, usb-tablet with `TABLET=1`) and a USB HID mouse; the HID
  mouse becomes QEMU's *current* pointer as soon as the guest polls it, which made OS 9's
  pointer relative (clicks landed wherever the accelerated guest cursor was). The frontend
  follows `Mouse.IsAbsolute` changes and makes the tablet current again (`query-mice` +
  `mouse_set`); untick it for games that want relative motion.
  Tiger now gets `usb-tablet` by default with ImGuiDock (`TABLET=0` opts out).
  Selection is retried once per second while the requested pointer is absent,
  including during boot or after hotplug. Native QEMU keeps its previous default.
  Verified in Tiger at 1024×768: four positions through ImGui's real mapping,
  measured with `CGSGetCurrentCursorLocation` in the guest; error ≤1 guest pixel
  in the docked view and zero at both tested full-screen points. Logs:
  `bench/tiger-chime-pointer.log`, `bench/tiger-pointer-positions.log` (Mac host; on the PC,
  copies under `bench/mac-20261009/`).
  **Updated policy:** host full screen always selects the relative USB mouse,
  captures/hides the host cursor and uses unbounded motion (raw motion when
  supported), suitable for Doom 3. Returning to windowed mode restores the
  tablet preference. Alt+Enter, F11 or Ctrl+Cmd+F exits host full screen and releases capture;
  loss of focus also releases capture. Escape stays in the game. The earlier absolute full-screen measurements above are
  historical; absolute input is no longer used in full screen.
- **Single cursor (01/11/2026):** while the pointer is sent in absolute mode, the host cursor
  is hidden over the guest image (Tiger draws its own cursor there, which gave a double
  cursor); it comes back over menus and bands, in pause and when the VM is stopped. In
  relative mode without capture it stays visible (`wantsHiddenHostCursor`,
  `src/PointerPolicy.h`, tested by `pointer_policy`).
- **Help** has the guide, the data folder, the log, a problem report, the update check and About
  (`AppExtras`, like the dropped files, the hot-plugged disks and the screenshot).
- **Status** shows the guest size, the drawn rectangle and scale, the window size and the
  pointer mode; **Log** records launches, CDs, scanout changes, switches and left clicks
  (host point → guest pixel).

`POMPPC_FE_SCRIPT` plays a test script through ImGui's input queue (same path as the real
mouse and keyboard, no macOS Accessibility permission needed) and takes captures, e.g.:

```sh
POMPPC_FE_SCRIPT="wait 75; fs; wait 3; shot /tmp/fs.png; click 1568 262; click 1568 262; \
f11; wait 2; shot /tmp/win.png; quit" ./build/pomppc ../run_os9.sh
```

Steps: `wait <s>`, `move|click <x> <y>` (window points),
`guestmove <x> <y>` (fractions 0–1 of the drawn guest view, without clicks),
`fs` (Ctrl+Cmd+F), `altenter`, `f11`, `esc`,
`type <a-z 0-9 space .>`, `enter`, `shot <png>` (`screencapture -x`, so macOS host only),
`glshot <file.ppm>` (the window alone, without screen-recording rights: prefer it),
`drop <path>` (like a file dropped on the window), `close` (like the window's close button), `quit`.

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
  `system_reset` (Machine ▸ Restart), `stop`/`cont` (Pause), CD insert/eject
  on the `gamecd` drive (`blockdev-change-medium`/`eject`), and an OS selector
  that relaunches Tiger ↔ Mac OS 9. **View** zooms 50–200 %.
- **P4 (in progress)** —
  - ✅ **Unix.Map shared-memory fast path**: advertises
    `org.qemu.Display1.Listener.Unix.Map`; QEMU passes an fd we `mmap`
    (`ScanoutMap`) and only reports dirty rects (`UpdateMap`) — no pixels on
    the bus. Silently falls back to inline `Scanout`/`Update` for
    non-shareable surfaces. Verified: mac99 surfaces are shareable.
  - ✅ **Sound / Gamepad** (Devices menu): both **on by default** (Screamer output
    through PulseAudio on Linux, CoreAudio on macOS, menu item « Sound (Screamer) » + `usb-host` gamepad passthrough); toggling relaunches QEMU.
    The run scripts self-guard (no device → skipped).
  - ✅ **Clipboard peer** (`org.qemu.Display1.Clipboard`): we export our
    skeleton + a proxy on the main connection, `Register()`, and bridge the
    host clipboard (GLFW) both ways — host copy → `Grab` (accepted by QEMU);
    guest copy → QEMU `Grab`s us → we `Request` and set the host clipboard.
    Serial-counter ownership honored. **Caveat:** actual text landing *inside*
    the guest also needs a QEMU clipboard agent, which PPC Mac OS X / OS 9
    lack — so it's a correct, verified peer that stays latent until a guest
    agent (or another D-Bus peer) provides content.
  - ✅ **Library** (Games ▸ Library): a window listing the game CD images
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
