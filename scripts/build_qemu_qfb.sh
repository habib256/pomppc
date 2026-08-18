#!/usr/bin/env bash
# build_qemu_qfb.sh — reconstruit QEMU 9.2.0 avec :
#   • le device paravirtuel « qfb-pci » (patches/qfb/)
#   • le bring-up SMP mac99 de BALATON Zoltan (patches/smp-mac99/)
#
#   ./scripts/build_qemu_qfb.sh              # build dans ~/src/qemu
#   QEMU_SRC=/chemin ./scripts/build_qemu_qfb.sh
#
# Le binaire produit (build/qemu-system-ppc et ...ppc64) est celui que
# config.env attend par défaut.
#
# NOTE : ce build n'embarque PAS le device audio « screamer » (port maison
# absent de l'arbre amont) ni slirp (libslirp-dev non installé) : utiliser
# NOSOUND=1 et NET absent, ou réappliquer le port Screamer par-dessus.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${QEMU_SRC:-$HOME/src/qemu}"
TAG="${QEMU_TAG:-v9.2.0}"
JOBS="${JOBS:-$(nproc)}"

if [ ! -d "$SRC/.git" ]; then
  echo "▶ clone de QEMU $TAG dans $SRC"
  git clone --depth 1 --branch "$TAG" https://gitlab.com/qemu-project/qemu.git "$SRC"
fi

cd "$SRC"

# --- 1. SMP mac99 (patch BALATON, appliqué avec fuzz sur 9.2.0) ---
if ! grep -q "CPU1 reset" hw/misc/macio/gpio.c; then
  echo "▶ patch SMP mac99"
  patch -p1 --fuzz=3 < "$ROOT/patches/smp-mac99/qemu-mac99-cpus-v2.patch"
  rm -f hw/misc/macio/gpio.c.orig hw/ppc/mac_newworld.c.orig
fi

# --- 2. Constantes GPIO absentes de 9.2.0 (le patch SMP les suppose) ---
if ! grep -q "define OUT_ENABLE" hw/misc/macio/gpio.c; then
  echo "▶ ajout des constantes GPIO IN_DATA / OUT_ENABLE"
  python3 - <<'PY'
p = 'hw/misc/macio/gpio.c'
s = open(p).read()
a = '#include "trace.h"'
s = s.replace(a, a + '\n\n/* bits des registres GPIO (noms repris de la série SMP de BALATON Zoltan) */\n'
                     '#define IN_DATA     0x02\n#define OUT_ENABLE  0x04\n', 1)
open(p, 'w').write(s)
PY
fi

# --- 3. Device qfb-pci ---
echo "▶ installation de hw/display/qfb-pci.c"
cp "$ROOT/patches/qfb/qfb-pci.c" hw/display/qfb-pci.c
if ! grep -q "qfb-pci.c" hw/display/meson.build; then
  echo "▶ câblage meson/Kconfig"
  patch -p1 < "$ROOT/patches/qfb/0002-wire-qfb-pci-build.patch"
fi

# --- 4. Build ---
mkdir -p build && cd build
if [ ! -f build.ninja ]; then
  ../configure --target-list=ppc-softmmu,ppc64-softmmu \
               --enable-gtk --enable-sdl --disable-docs --disable-werror
fi
ninja -j"$JOBS"

echo
echo "✔ $SRC/build/qemu-system-ppc"
./qemu-system-ppc -device help 2>/dev/null | grep -i qfb || {
  echo "⚠ le device qfb-pci n'apparaît pas dans -device help" >&2; exit 1; }
