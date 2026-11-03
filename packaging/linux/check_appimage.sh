#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# check_appimage.sh <POMPPC-version-x86_64.AppImage> <version> — la FORME du paquet, sans le lancer. Laisse
# squashfs-root/ dans le dossier courant, que smoke_appimage.sh reprend.
#
# Lancé depuis le répertoire de travail du job (le paquet appartient à root, sorti du conteneur :
# on le lit et on l'exécute, on ne fait pas de chmod).
set -euo pipefail
APP="${1:?AppImage attendue}"
VER="${2:?version attendue}"
PLANCHER="${POMPPC_GLIBC_PLANCHER:-2.31}"     # [A] glibc de l'image bullseye : le paquet n'exige rien au-dessus
die() { printf 'ERREUR : %s\n' "$*" >&2; exit 1; }

[ "$(basename "$APP")" = "POMPPC-$VER-x86_64.AppImage" ] || die "nom inattendu : $APP"

# ET_EXEC : AppImageLauncher rejette le runtime ET_DYN des appimagetool récents (« type -1 »).
TYPE="$(LC_ALL=C readelf -h "$APP" | awk -F'[ :]+' '/^ *Type:/ { print $3; exit }')"
[ "$TYPE" = EXEC ] || die "runtime $TYPE, ET_EXEC attendu"
# AI\x02 à l'offset 8 : AppImage de type 2.
MAGIE="$(xxd -s 8 -l 3 -p "$APP")"
[ "$MAGIE" = 414902 ] || die "magie $MAGIE, 414902 attendu"
echo "ok : runtime ET_EXEC, magie AppImage 2"

rm -rf squashfs-root
"$APP" --appimage-extract >/dev/null
PAY=squashfs-root/usr/share/pomppc
[ -x squashfs-root/AppRun ] || die "AppRun manque"
[ "$(cat squashfs-root/version)" = "$VER" ] || die "version embarquée : $(cat squashfs-root/version)"
bash "$(dirname "${BASH_SOURCE[0]}")/../check_package_payload.sh" "$PAY"

# Tous les objets ELF du paquet, pas seulement le frontal : une bibliothèque embarquée qui demande
# un GLIBC_2.34 refuse de se charger exactement comme un binaire qui le demande.
ELF=()
while read -r f; do
  if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ]; then ELF+=("$f"); fi
done < <(find "$PAY/bin" "$PAY/lib" -type f)
[ "${#ELF[@]}" -ge 4 ] || die "seulement ${#ELF[@]} objets ELF"

MAX=""; BAD=0
for f in "${ELF[@]}"; do
  S="$(objdump -T "$f" 2>/dev/null)"
  if grep -q 'GLIBC_PRIVATE' <<< "$S"; then echo "GLIBC_PRIVATE : $f" >&2; BAD=1; fi
  if grep -qE 'GLIBCXX_|CXXABI_' <<< "$S"; then echo "libstdc++ demandée : $f" >&2; BAD=1; fi
  M="$(grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' <<< "$S" | sed 's/GLIBC_//' | sort -V | tail -1 || true)"
  if [ -n "$M" ] && [ "$(printf '%s\n%s\n' "$M" "$PLANCHER" | sort -V | tail -1)" != "$PLANCHER" ]; then
    echo "GLIBC_$M > $PLANCHER : $f" >&2; BAD=1
  fi
  if [ -n "$M" ]; then MAX="$(printf '%s\n%s\n' "$M" "$MAX" | sort -V | tail -1)"; fi
done
[ "$BAD" = 0 ] || die "plancher dépassé"
echo "ok : ${#ELF[@]} objets, glibc exigée $MAX <= $PLANCHER, ni libstdc++, ni GLIBC_PRIVATE"

# Le pilote graphique vient de l'hôte. Embarquer libGL écraserait celui de l'utilisateur : rendu
# logiciel au mieux, désaccord d'ABI avec le pilote au pire. La glibc et libstdc++ pareil.
INTERDIT="$(find "$PAY/lib" -maxdepth 1 -printf '%f\n' | grep -E '^(lib(GL|EGL|GLX|OpenGL|GLdispatch|GLES[^.]*|gbm|drm|glapi|vulkan|wayland-[^.]*)|libc|libm|libdl|libpthread|librt|ld-linux[^.]*|libstdc\+\+|libgcc_s)\.so' || true)"
[ -z "$INTERDIT" ] || die "bibliothèques interdites embarquées : $INTERDIT"
echo "ok : ni GL, ni EGL, ni glibc, ni libstdc++ embarquées"

# RUNPATH partout, jamais RPATH (RPATH passerait devant LD_LIBRARY_PATH, et il se propage).
for f in "$PAY"/bin/*; do
  D="$(LC_ALL=C readelf -d "$f")"
  grep -q 'RUNPATH.*\$ORIGIN/\.\./lib' <<< "$D" || die "RUNPATH absent : $f"
  if grep -q '(RPATH)' <<< "$D"; then die "RPATH : $f"; fi
done
for f in "$PAY"/lib/*; do
  LC_ALL=C readelf -d "$f" | grep -q 'RUNPATH.*\$ORIGIN' || die "RUNPATH absent : $f"
done
if grep -q 'LD_LIBRARY_PATH=' squashfs-root/AppRun; then die "AppRun pose LD_LIBRARY_PATH"; fi
echo "ok : RUNPATH \$ORIGIN, pas de LD_LIBRARY_PATH"

# qgpu lie EGL et GL directement (patches/qgpu/qgpu-gl.c:52-54, 0003-wire-qgpu-pci-build.patch) ;
# sans eux à la compilation, il retombe sur un stub et le paquet n'a plus que le rendu logiciel, en
# passant tous les autres contrôles. GTK, SDL, epoxy et gbm n'ont rien à faire là.
for b in qemu-system-ppc qemu-system-ppc64; do
  N="$(LC_ALL=C readelf -d "$PAY/bin/$b" | grep NEEDED)"
  for lib in libEGL.so.1 libGL.so.1 libpulse.so.0 libslirp.so.0 libgio-2.0.so.0; do
    grep -qF "[$lib]" <<< "$N" || die "$b ne lie pas $lib"
  done
  if grep -qE '\[lib(gtk|gdk|SDL|epoxy|gbm|asound|jack)' <<< "$N"; then die "$b lie une bibliothèque hors profil : $N"; fi
done
echo "ok : QEMU lie EGL et GL, sans GTK, SDL, epoxy ni gbm"
N="$(LC_ALL=C readelf -d "$PAY/bin/pomppc" | grep NEEDED)"
grep -qF '[libGL.so.1]' <<< "$N" || die "le frontal ne lie pas libGL.so.1"
if grep -qE '\[lib(stdc\+\+|gcc_s|glfw|OpenGL)' <<< "$N"; then die "frontal : $N"; fi
echo "ok : frontal sur libGL.so.1, GLFW et libstdc++ statiques"

du -sh "$APP" "$PAY"
