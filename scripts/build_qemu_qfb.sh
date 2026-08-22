#!/usr/bin/env bash
# build_qemu_qfb.sh — reconstruit LE binaire de référence de POMPPC : QEMU 9.2.0 +
#
#   • le device audio « screamer » (AWACS PowerMac)  — patches/screamer/
#   • le device paravirtuel « qfb-pci »              — patches/qfb/
#   • le bring-up SMP mac99 de BALATON Zoltan        — patches/smp-mac99/
#   • slirp (réseau user-mode) et PulseAudio, exigés explicitement
#
#   ./scripts/build_qemu_qfb.sh              # build dans ~/src/qemu
#   QEMU_SRC=/chemin ./scripts/build_qemu_qfb.sh
#   RECONFIGURE=1 ./scripts/build_qemu_qfb.sh   # force un ../configure
#
# Le binaire produit (build/qemu-system-ppc et ...ppc64) est à l'emplacement que
# config.env attend par défaut. TOUT est dans ce dépôt : aucun fork tiers n'est
# récupéré au build, seul l'amont qemu-project est cloné.
#
# Le script se termine par une VÉRIFICATION DE CAPACITÉS (screamer, qfb-pci,
# slirp, audio pa) et écrit le résultat dans bench/build-capabilities.txt. Les
# lanceurs sondent le binaire de la même façon : un build incomplet dégrade
# proprement au lieu de mentir.
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

# --- 3. Device audio screamer (sources vendues + câblage macio) ---
echo "▶ installation de hw/audio/screamer.c"
cp "$ROOT/patches/screamer/screamer.c" hw/audio/screamer.c
cp "$ROOT/patches/screamer/screamer.h" include/hw/audio/screamer.h
# Le garde teste macio.c, PAS meson.build : le patch touche cinq fichiers, et
# tester le plus facile à satisfaire laisse passer un arbre à moitié recâblé.
# Vu en vrai : un `git checkout hw/misc/macio/macio.c` pour un A/B avait défait
# l'instanciation, meson.build portait toujours CONFIG_SCREAMER, le patch a été
# sauté — et le binaire compilait le device sans jamais le brancher.
if ! grep -q "screamer" hw/misc/macio/macio.c; then
  echo "▶ câblage screamer (Kconfig / meson / macio)"
  # --forward : les hunks déjà appliqués (meson/Kconfig) sont ignorés au lieu de
  # faire échouer le patch ; ceux qui manquent sont posés.
  patch -p1 --forward < "$ROOT/patches/screamer/0001-wire-screamer-build.patch" || true
  rm -f hw/misc/macio/macio.c.orig include/hw/misc/macio/macio.h.orig
  grep -q "screamer" hw/misc/macio/macio.c || {
    echo "⚠ le câblage macio du screamer n'a pas pu être appliqué." >&2; exit 1; }
fi

# --- 4. Device qfb-pci ---
echo "▶ installation de hw/display/qfb-pci.c"
cp "$ROOT/patches/qfb/qfb-pci.c" hw/display/qfb-pci.c
if ! grep -q "qfb-pci.c" hw/display/meson.build; then
  echo "▶ câblage meson/Kconfig"
  patch -p1 < "$ROOT/patches/qfb/0002-wire-qfb-pci-build.patch"
fi

# --- 5. Build ---
mkdir -p build && cd build
if [ ! -f build.ninja ] || [ -n "${RECONFIGURE:-}" ]; then
  # slirp et pa sont demandés EXPLICITEMENT : sans cela ils sont auto-détectés,
  # et leur absence produit un binaire silencieusement amputé (le piège qui a
  # coûté le son et le réseau une première fois).
  ../configure --target-list=ppc-softmmu,ppc64-softmmu \
               --enable-gtk --enable-sdl --enable-slirp --audio-drv-list=pa,alsa \
               --disable-docs --disable-werror
fi
ninja -j"$JOBS"

# --- 6. Vérification de capacités (fait foi, et sert aux lanceurs) ---
BIN="$SRC/build/qemu-system-ppc"
mkdir -p "$ROOT/bench"
CAPS="$ROOT/bench/build-capabilities.txt"
fail=0
{
  echo "# généré par scripts/build_qemu_qfb.sh le $(date -Is)"
  echo "binaire=$BIN"
  echo "version=$("$BIN" --version | head -1)"
} > "$CAPS"

# Le sondage passe par scripts/caps.sh (QOM, pas `-device help` : le Screamer
# est un enfant interne du macio et n'apparaît jamais dans `-device help`).
source "$ROOT/scripts/caps.sh"
check() { # check <étiquette> <prédicat…>
  if "${@:2}" >/dev/null 2>&1; then
    echo "  ✔ $1"; echo "$1=oui" >> "$CAPS"
  else
    echo "  ✘ $1"; echo "$1=non" >> "$CAPS"; fail=1
  fi
}
has_smp() { grep -q 'CPU1 reset' "$SRC/hw/misc/macio/gpio.c"; }
echo
echo "=== capacités du binaire produit ==="
# screamer : sondage MACHINE (le type peut être enregistré sans être câblé).
check screamer   qemu_machine_has  "$BIN" "mac99,via=pmu" screamer
check qfb-pci    qemu_has_device   "$BIN" qfb-pci
check slirp      qemu_has_netdev   "$BIN" user
check audio-pa   qemu_has_audiodev "$BIN" pa
check smp-mac99  has_smp
echo
echo "→ $CAPS"

if [ "$fail" -ne 0 ]; then
  echo "⚠ build INCOMPLET : au moins une capacité manque (voir ci-dessus)." >&2
  echo "  Les lanceurs sonderont le binaire et dégraderont proprement," >&2
  echo "  mais ce binaire n'est pas le binaire de référence." >&2
  exit 1
fi
echo "✔ binaire de référence complet : $BIN"
