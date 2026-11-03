#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# build_windows.sh — QEMU, le frontal, puis dist/POMPPC-<version>-windows-x64.zip. Dans un shell MSYS2
# UCRT64 : c'est l'équivalent de packaging/linux/build_in_bullseye.sh, sans image à épingler (les
# paquets viennent de MSYS2, leur version est relevée dans THIRD-PARTY-LICENSES/README.txt).
#
# BIBLIOTHÈQUES. Comme l'AppImage, pas d'outil qui décide à notre place. On suit les « DLL Name » de
# objdump à partir des .exe, et on ne prend que ce qui est dans /ucrt64/bin. Le reste vient de Windows,
# et check_windows_package.sh le compare à une liste blanche : opengl32.dll et les pilotes GL ne sont
# jamais embarqués, ni rien de msys-2.0.
#
# Env : POMPPC_VERSION (défaut : frontend/CMakeLists.txt), GUEST_ISO (exigé), JOBS, QEMU_SRC.
set -euxo pipefail

case "$(uname -s)" in
  UCRT64*|MINGW64*) ;;
  *) echo "À lancer dans un shell MSYS2 UCRT64 (uname : $(uname -s))" >&2; exit 1 ;;
esac
[ "${MSYSTEM:-}" = UCRT64 ] || { echo "MSYSTEM=UCRT64 attendu, pas ${MSYSTEM:-rien}" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
VER="${POMPPC_VERSION:-$(sed -n 's/^project(POMPPC VERSION \([0-9.]*\).*/\1/p' frontend/CMakeLists.txt)}"
[ -n "$VER" ] || { echo "version introuvable" >&2; exit 1; }
: "${GUEST_ISO:?GUEST_ISO attendu : packaging/linux/fetch_guest_iso.sh le télécharge}"
[ -f "$GUEST_ISO" ] || { echo "$GUEST_ISO manque" >&2; exit 1; }
export JOBS="${JOBS:-$(nproc)}"

# --- QEMU : profil paquet, générique. Un chemin court : meson et ninja y passent des lignes longues. ---
export QEMU_SRC="${QEMU_SRC:-/d/q}"
# -k 0 : toutes les erreurs d'un coup, un passage de CI coûte dix minutes.
NINJA_ARGS='-k 0' QEMU_PROFIL=paquet-windows scripts/build_qemu_qfb.sh

# --- Frontal ---
frontend/setup.sh
cmake -S frontend -B frontend/build-windows -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF
cmake --build frontend/build-windows --parallel "$JOBS"

# --- Paquet ---
NAME="POMPPC-$VER-windows-x64"
WORK="$ROOT/build-windows"
PAY="$WORK/$NAME"
rm -rf "$WORK"
mkdir -p "$PAY/share/qemu" "$PAY/patches/smp-mac99" "$PAY/THIRD-PARTY-LICENSES" "$ROOT/dist"

Q="$QEMU_SRC/build"
for b in qemu-system-ppc qemu-system-ppc64 qemu-img; do
  [ -f "$Q/$b.exe" ] || { echo "$Q/$b.exe manque" >&2; exit 1; }
  cp "$Q/$b.exe" "$PAY/"
done
cp frontend/build-windows/POMPPC.exe "$PAY/"
strip --strip-unneeded "$PAY"/*.exe

# Directement depuis l'arbre de QEMU, sans ninja install : son préfixe et ses chemins sous Windows
# n'apportent rien ici, LaunchPlan passe -L. [D] QEMU pc-bios/meson.build @ v11.1.2 : ce qu'une machine mac99
# peut demander (firmware, ndrv, BIOS de VGA, ROM réseau). Les tables de clavier ne servent qu'à VNC.
for f in openbios-ppc qemu_vga.ndrv; do
  cp "$QEMU_SRC/pc-bios/$f" "$PAY/share/qemu/"
done
cp "$QEMU_SRC"/pc-bios/vgabios*.bin "$QEMU_SRC"/pc-bios/efi-*.rom "$QEMU_SRC"/pc-bios/pxe-*.rom "$PAY/share/qemu/"

cp patches/smp-mac99/openbios-smp-screamer.elf patches/smp-mac99/openbios-smp-screamer-source.patch "$PAY/patches/smp-mac99/"
cp "$GUEST_ISO" "$PAY/pomppc-guest.iso"
cp LICENSE "$PAY/LICENSE"
sed "s/@VERSION@/$VER/g" packaging/windows/README-windows.txt > "$PAY/README.txt"

# --- DLL : la file des imports, limitée à /ucrt64/bin ---
ORIG="$WORK/origines.txt"; : > "$ORIG"
declare -A VU=()
queue=("$PAY"/*.exe)
while [ "${#queue[@]}" -gt 0 ]; do
  f="${queue[0]}"; queue=("${queue[@]:1}")
  while read -r dll; do
    [ -n "$dll" ] || continue
    bas="${dll,,}"
    [ -z "${VU[$bas]:-}" ] || continue
    VU[$bas]=1
    if [ -f "/ucrt64/bin/$dll" ]; then
      cp "/ucrt64/bin/$dll" "$PAY/$dll"
      echo "$dll" >> "$ORIG"
      queue+=("$PAY/$dll")
    fi
  done < <(objdump -p "$f" | tr -d '\r' | sed -n 's/^[[:space:]]*DLL Name: //p')
done
sort "$ORIG" | sed 's/^/  /'

# --- Licences ---
TP="$PAY/THIRD-PARTY-LICENSES"
{
  echo "Third-party components bundled in $NAME"
  echo
  echo "QEMU $(cat "$QEMU_SRC/VERSION") (GPL-2.0, https://www.qemu.org) with POMPPC's patches (patches/, GPL-2.0-or-later);"
  echo "  source: https://gitlab.com/qemu-project/qemu.git tag v$(cat "$QEMU_SRC/VERSION") + scripts/build_qemu_qfb.sh."
  echo "OpenBIOS (GPL-2.0): patches/smp-mac99/openbios-smp-screamer.elf, source in"
  echo "  patches/smp-mac99/openbios-smp-screamer-source.patch."
  echo "Dear ImGui (MIT, https://github.com/ocornut/imgui), statically linked in POMPPC.exe."
  echo "Roboto (Apache-2.0, https://github.com/googlefonts/roboto), embedded in POMPPC.exe."
  echo
  echo "DLLs from MSYS2 UCRT64 (https://www.msys2.org); sources and licenses are in its packages:"
  sort "$ORIG" | while read -r dll; do
    pkg="$(pacman -Qo "/ucrt64/bin/$dll" 2>/dev/null | sed 's/.* is owned by //' || true)"
    echo "  $dll  <-  ${pkg:-unknown}"
    # « mingw-w64-ucrt-x86_64-glib2 2.84.0-1 » : /ucrt64/share/licenses/<nom sans le préfixe>/
    base="${pkg%% *}"; base="${base#mingw-w64-ucrt-x86_64-}"
    if [ -n "$base" ] && [ -d "/ucrt64/share/licenses/$base" ] && [ ! -e "$TP/$base" ]; then
      cp -RL "/ucrt64/share/licenses/$base" "$TP/$base"
    fi
  done
} > "$TP/README.txt"
cp "$QEMU_SRC/COPYING" "$TP/qemu-COPYING"
if [ -f frontend/imgui/LICENSE.txt ]; then cp frontend/imgui/LICENSE.txt "$TP/imgui-LICENSE.txt"; fi

# --- Archive : racine unique, comme un dossier à décompresser où l'on veut ---
OUT="$ROOT/dist/$NAME.zip"
rm -f "$OUT"
( cd "$WORK" && zip -q -r -9 "$OUT" "$NAME" )
ls -l "$OUT"
