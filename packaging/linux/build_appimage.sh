#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# build_appimage.sh — dépose QEMU, le frontal et leurs bibliothèques dans un AppDir, puis le
# compresse en AppImage. Tourne dans l'image bullseye (build_in_bullseye.sh, après compilation).
#
# Le paquet a la forme de celui de macOS (scripts/package_release_macos.sh) : le dépôt au commit
# courant, plus bin/, lib/, share/qemu/, pomppc-guest.iso et les licences, ici dans
# usr/share/pomppc/ de l'AppDir. AppRun en fait un dossier ~/POMPPC à la forme du dépôt.
#
# BIBLIOTHÈQUES. Pas de linuxdeploy : la liste des exclusions fait ici partie du contrat, et un
# outil qui décide seul d'embarquer libGL ou de poser LD_LIBRARY_PATH la romprait sans bruit.
#   · embarqué : ce que binaires et bibliothèques demandent, en suivant les NEEDED ;
#   · JAMAIS   : EXCLU ci-dessous. Le pilote GL vient de l'hôte, la glibc aussi. Les clients X
#     aussi : un pilote de l'hôte qui demande libxcb prendrait sinon la nôtre, déjà chargée, plus
#     vieille que celle pour laquelle il a été compilé (le chargeur compare les SONAME).
#   · trouvées par RUNPATH ($ORIGIN/../lib, $ORIGIN) : LD_LIBRARY_PATH passerait aux enfants, à
#     bash, python3, curl, et au Mesa de l'hôte qui les chargerait à la place des siennes.
#
# Env : POMPPC_VERSION, GUEST_ISO, FRONTEND, QEMU_SRC, POMPPC_APPIMAGE_TOOLS_DIR.
# Sortie : dist/POMPPC-<version>-x86_64.AppImage.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

VER="${POMPPC_VERSION:-$(sed -n 's/^project(POMPPC VERSION \([0-9.]*\).*/\1/p' frontend/CMakeLists.txt)}"
[ -n "$VER" ] || { echo "version introuvable" >&2; exit 1; }
QSRC="${QEMU_SRC:-/tmp/qemu-src}"
FRONTEND="${FRONTEND:-$ROOT/frontend/build-appimage/pomppc}"
TOOLS="${POMPPC_APPIMAGE_TOOLS_DIR:-/opt/appimage-tools}"
WORK="$ROOT/build-appimage"
APPDIR="$WORK/AppDir"
PAY="$APPDIR/usr/share/pomppc"
OUT="$ROOT/dist/POMPPC-$VER-x86_64.AppImage"

EXCLU='^(ld-linux.*|libc|libm|libdl|librt|libpthread|libresolv|libutil|libanl|libnss_.*|libmvec|libstdc\+\+|libgcc_s|libGL|libEGL|libGLX|libOpenGL|libGLdispatch|libGLES.*|libgbm|libdrm|libglapi|libvulkan|libwayland-.*|libxkbcommon.*|libX11|libX11-xcb|libxcb.*|libXau|libXdmcp|libXext|libXrender|libXcursor|libXi|libXrandr|libXinerama|libXfixes)\.so'

log() { printf '\n▶ %s\n' "$*"; }
trap 'echo "build_appimage.sh : échec ligne $LINENO" >&2' ERR

[ "$(uname -s)/$(uname -m)" = Linux/x86_64 ] || { echo "Linux x86-64 seulement" >&2; exit 1; }
[ -z "$(git status --porcelain --untracked-files=no)" ] ||
  { echo "arbre de travail modifié : commiter d'abord (le paquet est le commit)" >&2; exit 1; }
for f in "$QSRC/build/qemu-system-ppc" "$QSRC/build/qemu-system-ppc64" "$FRONTEND"; do
  [ -x "$f" ] || { echo "$f manque" >&2; exit 1; }
done
[ -f "${GUEST_ISO:-}" ] || { echo "GUEST_ISO manque : ${GUEST_ISO:-non défini}" >&2; exit 1; }
# Le paquet est GÉNÉRIQUE (garde reprise de scripts/package_release_linux.sh) : un binaire
# -march=native mourrait sur SIGILL ailleurs, docs/binaire-rapide-x86.md.
if grep -qE -- '-march=native|-fprofile-(use|generate)' "$QSRC/build/build.ninja"; then
  echo "$QSRC/build est compilé en -march=native ou avec un profil PGO : le paquet doit être générique" >&2
  exit 1
fi

log "dépôt $(git rev-parse --short HEAD)"
rm -rf "$WORK/stage" "$APPDIR"
mkdir -p "$PAY/bin" "$PAY/lib" "$PAY/share" "$WORK/stage"
git archive HEAD | tar xf - -C "$PAY"

log "QEMU (ninja install)"
DESTDIR="$WORK/stage" ninja -C "$QSRC/build" install >/dev/null
cp "$WORK/stage/usr/local/bin/"{qemu-system-ppc,qemu-system-ppc64,qemu-img} "$PAY/bin/"
cp -R "$WORK/stage/usr/local/share/qemu" "$PAY/share/"
# Les firmwares UEFI ne servent pas à un G4 (rien à retirer si les cibles ne les ont pas installés).
rm -f "$PAY/share/qemu"/edk2-*
cp "$FRONTEND" "$PAY/bin/pomppc"
strip --strip-unneeded "$PAY"/bin/*

log "bibliothèques"
# Une file sur les NEEDED, sans descendre dans ce qui est exclu : `ldd` seul rend la fermeture
# complète et ferait entrer ce que seules libGL ou libxcb demandent.
ORIG="$WORK/origines.txt"; : > "$ORIG"
declare -A VU=()
queue=("$PAY"/bin/*)
while [ "${#queue[@]}" -gt 0 ]; do
  f="${queue[0]}"; queue=("${queue[@]:1}")
  while read -r so; do
    [ -n "$so" ] && [ -z "${VU[$so]:-}" ] || continue
    VU[$so]=1
    if [[ "$so" =~ $EXCLU ]]; then continue; fi
    # Sans `exit` dans awk (ldd mourrait sur SIGPIPE) et sans croire le code de ldd : avec pipefail,
    # l'un ou l'autre tuait le script sans un mot. Le contrôle qui suit dit ce qui manque.
    reel="$( { ldd "$f" 2>/dev/null || true; } | awk -v s="$so" '$1 == s && $2 == "=>" && !v { v = $3 } END { if (v) print v }')"
    [ -n "$reel" ] && [ -e "$reel" ] || { echo "$so (demandée par $f) introuvable" >&2; exit 1; }
    cp -L "$reel" "$PAY/lib/$so"; chmod u+w "$PAY/lib/$so"
    echo "$so $(readlink -f "$reel")" >> "$ORIG"
    queue+=("$PAY/lib/$so")
  done < <(objdump -p "$f" | awk '$1 == "NEEDED" { print $2 }')
done
sort "$ORIG" | sed 's/^/  /'

log "RUNPATH"
# Dans cet ordre : strip avant patchelf, l'inverse abîme les segments réécrits. Chaque bibliothèque
# a le sien : un RUNPATH ne vaut que pour les NEEDED directs de l'objet qui le porte.
for f in "$PAY"/bin/*; do patchelf --set-rpath '$ORIGIN/../lib' "$f"; done
for f in "$PAY"/lib/*; do patchelf --set-rpath '$ORIGIN' "$f"; done

log "licences tierces"
TP="$PAY/THIRD-PARTY-LICENSES"; mkdir -p "$TP"
{
  echo "Third-party components bundled in POMPPC-$VER-x86_64.AppImage"
  echo
  echo "QEMU $(cat "$QSRC/VERSION") (GPL-2.0, https://www.qemu.org) with POMPPC's patches (patches/, GPL-2.0-or-later);"
  echo "  source: https://gitlab.com/qemu-project/qemu.git tag v$(cat "$QSRC/VERSION") + scripts/build_qemu_qfb.sh."
  echo "OpenBIOS (GPL-2.0): patches/smp-mac99/openbios-smp-screamer.elf, source in"
  echo "  patches/smp-mac99/openbios-smp-screamer-source.patch."
  echo "Dear ImGui (MIT, https://github.com/ocornut/imgui), statically linked in bin/pomppc."
  echo "Roboto (Apache-2.0, https://github.com/googlefonts/roboto), embedded in bin/pomppc."
  echo "GLFW (zlib, https://www.glfw.org), statically linked in bin/pomppc."
  echo "libstdc++ and libgcc (GPL-3.0 with the GCC Runtime Library Exception), statically linked."
  echo
  echo "Shared libraries in lib/, from Debian 11 (license texts in this folder):"
  sort "$ORIG" | while read -r so reel; do
    pkg="$(dpkg -S "$reel" 2>/dev/null || dpkg -S "${reel#/usr}" 2>/dev/null || true)"
    pkg="${pkg%%:*}"
    if [ -n "$pkg" ]; then
      echo "  $so  <-  $pkg $(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || true)"
      if [ -f "/usr/share/doc/$pkg/copyright" ]; then cp "/usr/share/doc/$pkg/copyright" "$TP/$pkg-copyright"; fi
    else
      echo "  $so  <-  $reel"
    fi
  done
} > "$TP/README.txt"
if [ -f frontend/imgui/LICENSE.txt ]; then cp frontend/imgui/LICENSE.txt "$TP/imgui-LICENSE.txt"; fi
if [ -f /opt/licenses/glfw-LICENSE.md ]; then cp /opt/licenses/glfw-LICENSE.md "$TP/glfw-LICENSE.md"; fi
cp "$QSRC/COPYING" "$TP/qemu-COPYING"

log "CD invité"
cp "$GUEST_ISO" "$PAY/pomppc-guest.iso"

log "AppDir"
install -m 0755 packaging/linux/AppRun "$APPDIR/AppRun"
printf '%s\n' "$VER" > "$APPDIR/version"
cp packaging/linux/pomppc.desktop packaging/linux/pomppc.svg "$APPDIR/"
bash packaging/check_package_payload.sh "$PAY"

log "AppImage"
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
ARCH=x86_64 "$TOOLS/appimagetool.AppDir/AppRun" --no-appstream \
  --runtime-file "$TOOLS/runtime-x86_64" "$APPDIR" "$OUT"

# Le runtime et le compresseur sont deux outils, et rien en amont ne vérifie qu'ils s'accordent :
# une image qu'ils n'ont pas accordée a le bon nom, le bon type ELF et la bonne magie, et ne s'ouvre
# jamais (vécu sur POM2). Lui demander ici, où la réponse coûte une seconde.
SONDE="$WORK/sonde"; rm -rf "$SONDE"; mkdir -p "$SONDE"
if ( cd "$SONDE" && "$OUT" --appimage-extract >/dev/null 2>&1 ) && [ -x "$SONDE/squashfs-root/AppRun" ]; then
  echo "auto-extraction : OK"
  rm -rf "$SONDE"
else
  echo "$OUT ne sait pas s'extraire : runtime et compression en désaccord" >&2
  exit 1
fi
ls -lh "$OUT"
