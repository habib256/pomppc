#!/usr/bin/env bash
# build_qemu_qfb.sh — reconstruit LE binaire de référence de POMPPC : QEMU 9.2.0 +
#
#   • le device audio « screamer » (AWACS PowerMac)  — patches/screamer/
#   • le device paravirtuel « qfb-pci »              — patches/qfb/
#   • le GPU paravirtuel « qgpu-pci » (+ backends)   — patches/qgpu/
#   • le bring-up SMP mac99 de BALATON Zoltan        — patches/smp-mac99/
#   • slirp (réseau user-mode) et PulseAudio, exigés explicitement
#
#   ./scripts/build_qemu_qfb.sh              # build dans ~/src/qemu
#   QEMU_SRC=/chemin ./scripts/build_qemu_qfb.sh
#   RECONFIGURE=1 ./scripts/build_qemu_qfb.sh   # force un ../configure
#   PYTHON=/chemin/python3 ./scripts/build_qemu_qfb.sh   # Python pour configure
#
# macOS : affichage Cocoa et son CoreAudio au lieu de GTK/SDL et PulseAudio.
# Le configure de QEMU 9.2 exige un Python ≥ 3.8 avec « tomli » (ou ≥ 3.11) et
# « distlib » : sur macOS, un venv suffit (python3 -m venv v && v/bin/pip
# install distlib tomli ; PYTHON=v/bin/python3).
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
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

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

# --- 4 bis. GPU paravirtuel qgpu-pci : device + cœur + backends soft/GL ---
# Le backend GL se compile toujours ; sans OpenGL.framework (macOS) ni EGL+GL
# (Linux) il devient un stub et le device retombe sur le backend logiciel.
echo "▶ installation de hw/display/qgpu-*.c"
cp "$ROOT"/patches/qgpu/qgpu-pci.c "$ROOT"/patches/qgpu/qgpu-core.c \
   "$ROOT"/patches/qgpu/qgpu-core.h "$ROOT"/patches/qgpu/qgpu-soft.c \
   "$ROOT"/patches/qgpu/qgpu-gl.c "$ROOT"/patches/qgpu/qgpu_proto.h hw/display/
if ! grep -q "qgpu-pci.c" hw/display/meson.build; then
  echo "▶ câblage meson/Kconfig qgpu"
  patch -p1 < "$ROOT/patches/qgpu/0003-wire-qgpu-pci-build.patch"
fi

# --- 4 ter. Flottant rapide PowerPC (propriété de CPU x-fast-fp) ---
# Le garde teste ppc_fp_primed() dans target/ppc/fpu_helper.c, et pas le
# premier fichier venu. Deux raisons : c'est le DERNIER fichier du patch (si le
# marqueur y est, les quatre précédents ont été posés, patch s'arrêtant au
# premier échec), et c'est surtout le seul dont l'absence serait totalement
# SILENCIEUSE — la propriété existerait, `-cpu g4,x-fast-fp=on` serait accepté,
# et le mode rapide ne ferait rien du tout. Un A/B aurait conclu « aucun gain »
# au lieu de « patch à moitié appliqué ». Même leçon que le câblage du Screamer.
if ! grep -q "ppc_fp_primed" target/ppc/fpu_helper.c; then
  echo "▶ patch flottant rapide (x-fast-fp)"
  patch -p1 --forward < "$ROOT/patches/fastfp/0001-ppc-fast-fp.patch" || true
  rm -f fpu/softfloat.c.orig include/fpu/softfloat-types.h.orig \
        target/ppc/cpu.h.orig target/ppc/cpu_init.c.orig target/ppc/fpu_helper.c.orig
fi
# Les cinq morceaux, vérifiés un par un : un patch appliqué à moitié compile.
while read -r f m; do
  [ -z "$f" ] && continue
  grep -q "$m" "$f" || {
    echo "⚠ patch fastfp incomplet : '$m' absent de $f" >&2
    echo "  (voir patches/fastfp/ et d'éventuels .rej)" >&2; exit 1; }
done <<'FASTFP_MARKERS'
fpu/softfloat.c float64r32_gen2
include/fpu/softfloat-types.h no_hardfloat
target/ppc/cpu.h fast_fp
target/ppc/cpu_init.c x-fast-fp
target/ppc/fpu_helper.c ppc_fp_primed
FASTFP_MARKERS

# --- 4 quater. Moins d'appels de helper par instruction flottante ---
# Patch séparé du précédent EXPRÈS : il ne change aucun comportement (vérifié
# octet pour octet contre un QEMU non patché), il ne fait que réduire le coût.
# Les garder séparables permet d'attribuer les gains en A/B — et de reverter
# celui-ci seul si jamais il régressait :  NO_FASTFP2=1 ./scripts/build_qemu_qfb.sh
# sur un arbre PROPRE (sur un arbre déjà patché, il est déjà là).
if [ -z "${NO_FASTFP2:-}" ]; then
  if ! grep -q "fp_prime_mask" target/ppc/cpu.h; then
    echo "▶ patch flottant rapide — réduction des appels de helper"
    patch -p1 --forward < "$ROOT/patches/fastfp/0002-ppc-fewer-fp-helpers.patch" || true
    rm -f target/ppc/cpu.h.orig target/ppc/cpu_init.c.orig \
          target/ppc/fpu_helper.c.orig target/ppc/helper.h.orig \
          target/ppc/translate/fp-impl.c.inc.orig
  fi
  # Même logique que ci-dessus : le marqueur le plus silencieux est celui du
  # traducteur. Sans lui le code généré appellerait toujours les helpers, et la
  # seule trace serait une mesure A/B décevante.
  grep -q "fprf_check_float64" target/ppc/translate/fp-impl.c.inc || {
    echo "⚠ patch fastfp 0002 incomplet : le traducteur n'est pas modifié." >&2
    exit 1; }
fi

# --- 5. Build ---
mkdir -p build && cd build
if [ ! -f build.ninja ] || [ -n "${RECONFIGURE:-}" ]; then
  # slirp et pa sont demandés EXPLICITEMENT : sans cela ils sont auto-détectés,
  # et leur absence produit un binaire silencieusement amputé (le piège qui a
  # coûté le son et le réseau une première fois).
  case "$(uname -s)" in
    Darwin) UI_OPTS=(--enable-cocoa --audio-drv-list=coreaudio) ;;
    *)      UI_OPTS=(--enable-gtk --enable-sdl --audio-drv-list=pa,alsa) ;;
  esac
  PY_OPTS=()
  [ -n "${PYTHON:-}" ] && PY_OPTS=(--python="$PYTHON")
  ../configure --target-list=ppc-softmmu,ppc64-softmmu \
               ${UI_OPTS[@]+"${UI_OPTS[@]}"} --enable-slirp ${PY_OPTS[@]+"${PY_OPTS[@]}"} \
               --disable-docs --disable-werror
fi
ninja -j"$JOBS"

# --- 6. Vérification de capacités (fait foi, et sert aux lanceurs) ---
BIN="$SRC/build/qemu-system-ppc"
mkdir -p "$ROOT/bench"
CAPS="$ROOT/bench/build-capabilities.txt"
fail=0
{
  echo "# généré par scripts/build_qemu_qfb.sh le $(date "+%Y-%m-%dT%H:%M:%S%z")"
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
check qgpu-pci   qemu_has_device   "$BIN" qgpu-pci
# Pas seulement « le device existe » : a-t-il un écran où présenter ?
check qgpu-scanout qemu_qgpu_has_scanout "$BIN" "mac99,via=pmu"
check slirp      qemu_has_netdev   "$BIN" user
case "$(uname -s)" in
  Darwin) check audio-coreaudio qemu_has_audiodev "$BIN" coreaudio ;;
  *)      check audio-pa        qemu_has_audiodev "$BIN" pa ;;
esac
check smp-mac99  has_smp
# Flottant rapide : sondé sur LES DEUX binaires. Le lanceur emploie
# qemu-system-ppc64 dès SMP >= 2, et rien ne garantit a priori que la propriété
# soit visible des deux côtés (le type de CPU n'a pas le même nom).
check x-fast-fp        qemu_cpu_has_fastfp "$BIN"        "mac99,via=pmu" g4
check x-fast-fp-ppc64  qemu_cpu_has_fastfp "${BIN}64"    "mac99,via=pmu" g4
echo
echo "→ $CAPS"

if [ "$fail" -ne 0 ]; then
  echo "⚠ build INCOMPLET : au moins une capacité manque (voir ci-dessus)." >&2
  echo "  Les lanceurs sonderont le binaire et dégraderont proprement," >&2
  echo "  mais ce binaire n'est pas le binaire de référence." >&2
  exit 1
fi
echo "✔ binaire de référence complet : $BIN"
