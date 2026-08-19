#!/usr/bin/env bash
# build_qemu_qfb.sh — reconstruit QEMU 9.2.0 avec :
#   • le device paravirtuel « qfb-pci » (patches/qfb/)
#   • le bring-up SMP mac99 de BALATON Zoltan (patches/smp-mac99/)
#
#   ./scripts/build_qemu_qfb.sh              # build dans ~/src/qemu
#   QEMU_SRC=/chemin ./scripts/build_qemu_qfb.sh
#
# Le binaire produit (build/qemu-system-ppc et ...ppc64) est à l'emplacement que
# config.env attend par défaut.
#
# ⚠ CE BUILD NE REPRODUIT PAS ENTIÈREMENT LE BINAIRE DE RÉFÉRENCE. Il lui manque :
#
#   • le device audio « screamer » — port maison depuis le fork mcayland, absent de
#     l'arbre amont ET de ce dépôt (rien à réappliquer dans patches/) ;
#   • slirp (libslirp-dev non installé), donc pas de réseau user-mode.
#
# Conséquence concrète : run_tiger.sh et run_os9.sh démarrent avec le SON ACTIF par
# défaut, donc passent '-global screamer.audiodev=snd0'. Sur un binaire sans la classe
# screamer, QEMU refuse le global et sort en erreur. Avec ce build, lancer :
#
#   NOSOUND=1 ./run_tiger.sh        (et ne pas utiliser NET=1)
#
# Pour retrouver le son, il faut réappliquer le port Screamer par-dessus cet arbre —
# ce que ce script ne sait pas faire.
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
