#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# build_qemu_qfb.sh — reconstruit LE binaire de référence de POMPPC : QEMU 11.1.2 +
#
#   • le device audio « screamer » (AWACS PowerMac)  — patches/screamer/
#   • le device paravirtuel « qfb-pci »              — patches/qfb/
#   • le GPU paravirtuel « qgpu-pci » (+ backends)   — patches/qgpu/
#   • le bring-up SMP mac99 de BALATON Zoltan        — patches/smp-mac99/
#   • le reset des sources de niveau de l'OpenPIC     — patches/openpic/
#   • le flottant rapide (x-fast-fp)                  — patches/fastfp/
#   • le TLB gardé par jeu de segments (x-sr-tlb)     — patches/tcg/0001
#   • lfs/stfs sans helper (x-lfs-inline)             — patches/tcg/0002
#   • AltiVec : vfp à 4 voies, vperm par table       — patches/tcg/0003, 0004
#   • tampon du JIT près du texte (x-jit-near)        — patches/tcg/0006
#   • flottant scalaire simple sans helper (x-fp-inline) — patches/tcg/0007
#   • sorties indirectes en ligne (x-ret-inline, x-jc-idx) — patches/tcg/0008
#   • code réécrit par l'autre vCPU (courses, x-icbi-sync) — patches/tcg/0010
#   • taille du cache de sauts (x-jc-bits)             — patches/tcg/0011
#   • mtmsr/rfi sans verrou global (x-msr-nobql)       — patches/tcg/0012
#   • flottant scalaire sans branchement (x-fp-flat)   — patches/tcg/0013
#   • flottant scalaire natif, op TCG ppc_fp32 (x-fp-native) — patches/tcg/0014
#   • base de temps par le compteur de l'hôte (x-tb-fast)  — patches/tcg/0015
#   • flottant double natif (x-fp-native64)             — patches/tcg/0016
#   • les trois mêmes sur hôte x86-64 (op ppc_fp32, TSC, vperm) — patches/tcg/0017-0019
#   • flottant AltiVec à 4 voies par AVX/FMA3 sur hôte x86-64 — patches/tcg/0020
#   • AltiVec en ligne, flottant AltiVec natif, mftb sans div, JIT à 2 Gio — patches/tcg/0021-0024
#   • la tablette USB juste sous Tiger 10.4.11 (x-abs-margin)  — patches/usbhid/0001
#   • slirp (réseau user-mode) et PulseAudio, exigés explicitement
#
#   ./scripts/build_qemu_qfb.sh              # build dans ~/src/qemu
#   QEMU_SRC=/chemin ./scripts/build_qemu_qfb.sh
#   RECONFIGURE=1 ./scripts/build_qemu_qfb.sh   # force un ../configure
#   PYTHON=/chemin/python3 ./scripts/build_qemu_qfb.sh   # Python pour configure
#   CONFIGURE_EXTRA="--disable-sdl" …                    # options de configure en plus
#                                    (la publication macOS, scripts/package_release_macos.sh)
#
# macOS : affichage Cocoa et son CoreAudio au lieu de GTK/SDL et PulseAudio.
# Le configure de QEMU 11.1 exige un Python ≥ 3.9 avec « tomli » (ou ≥ 3.11) et
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
#
# Binaire RAPIDE du PC (Linux x86-64, docs/binaire-rapide-x86.md) : en plus de
# build/, ~/src/qemu/build-fast/ en -O3 -march=native, LTO et PGO, que
# run_tiger.sh préfère quand il est là (QEMU_FAST=0 pour la référence) :
#   QEMU_FAST=1 ./scripts/build_qemu_qfb.sh     # (ou --fast) build/ puis build-fast/
#   QEMU_FAST=only ./scripts/build_qemu_qfb.sh  # build-fast/ seulement
#   tools/tcg/pgo-train.sh                      # entraîne le profil (VM quotidienne)
set -euo pipefail
case "${1:-}" in
  --fast) export QEMU_FAST=1; shift ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${QEMU_SRC:-$HOME/src/qemu}"
# Version visée : POMPPC_QEMU_VERSION de config.env (une seule source).
TAG="${QEMU_TAG:-v$(sed -n 's/^POMPPC_QEMU_VERSION=//p' "$ROOT/config.env")}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

# -e et non -d : dans un worktree git, .git est un fichier.
if [ ! -e "$SRC/.git" ]; then
  echo "▶ clone de QEMU $TAG dans $SRC"
  git clone --depth 1 --branch "$TAG" https://gitlab.com/qemu-project/qemu.git "$SRC"
fi

cd "$SRC"

# La série de patches vise UNE version de QEMU. Un arbre d'une autre version (le
# ~/src/qemu en 9.2.0 d'avant le passage à 11.1.2, le 01/10/2026) recevrait des
# hunks posés de travers : refus net, plutôt qu'un binaire à moitié patché.
if [ "$(cat VERSION)" != "${TAG#v}" ]; then
  echo "⚠ $SRC est QEMU $(cat VERSION), la série de patches vise ${TAG#v}." >&2
  echo "  QEMU_SRC=<autre dossier> pour un clone neuf, ou QEMU_TAG= pour forcer." >&2
  exit 1
fi

# --- 0. Application des patches : aucun rejet silencieux ---
#
# GNU patch NE S'ARRÊTE PAS au premier hunk qui échoue (ce que disait le
# commentaire d'avant) : il écrit le hunk refusé dans un `.rej` à côté du
# fichier et CONTINUE. Un patch à moitié posé sort donc en erreur… que le
# `|| true` effaçait, et le seul témoin était un `.rej` que personne ne
# cherchait. Cas réel : le hunk qui retire `#if defined(TARGET_PPC)` de
# fpu/softfloat.c (12 hunks) peut échouer seul — la propriété `x-fast-fp`
# existe alors, le lanceur annonce « + FLOTTANT RAPIDE », et `can_use_fpu()`
# rend toujours faux.
#
# Les `.rej` sont nettoyés AVANT chaque patch, et on ne juge que ceux que ce
# patch vient de créer : l'arbre peut en porter d'anciens, parfaitement bénins
# — ceux des hunks « déjà appliqués » que `--forward` saute (c'est l'origine de
# hw/audio/Kconfig.rej, hw/audio/meson.build.rej et hw/ppc/Kconfig.rej vus dans
# l'arbre de dev). Les confondre avec un vrai échec bloquerait le build sur un
# arbre sain ; les ignorer laisserait passer un vrai échec.
rej_clean() { find . -name '*.rej' -delete; }
rej_show()  { find . -name '*.rej' | sort | sed 's/^/    /'; }
rej_any()   { [ -n "$(find . -name '*.rej' -print -quit)" ]; }

# Patch qui doit s'appliquer PROPREMENT : le moindre rejet est une erreur.
# --forward est INDISPENSABLE : sans tty, le patch d'Apple (2.0-12u11) répond
# « Assume -R? [y] » tout seul à un hunk déjà posé, DÉFAIT le fichier, sort en
# 0 et ne laisse aucun .rej — un arbre à moitié patché repartait à moitié
# dépatché, en silence (bug hunt du 29/09, 3e passe). Avec --forward, un hunk
# déjà posé donne un .rej et le code 1 : l'erreur voulue.
patch_strict() { # patch_strict <fichier.patch> [args de patch…]
  local p="$1" rc=0; shift
  rej_clean
  patch -p1 --forward "$@" < "$p" || rc=$?
  if [ "$rc" -ne 0 ] || rej_any; then
    echo "⚠ patch REJETÉ (code $rc) : $p" >&2
    rej_any && { echo "  hunks refusés, à relire :" >&2; rej_show >&2; }
    exit 1
  fi
}

# Patch posé avec --forward sur un arbre qui en porte peut-être déjà une
# partie. Là, un hunk « déjà appliqué » est NORMAL : patch le saute, l'écrit
# quand même dans un `.rej` et sort en 1. Le 1 n'est donc acceptable que si
# l'arbre porte, à l'arrivée, TOUS les marqueurs attendus — sinon c'est un vrai
# échec, et c'est cette différence-là que `|| true` faisait disparaître.
patch_forward() { # patch_forward <fichier.patch> <fichier> <motif> [<fichier> <motif>…]
  local p="$1" rc=0 f m; shift
  rej_clean
  patch -p1 --forward < "$p" || rc=$?
  if [ "$rc" -gt 1 ]; then
    echo "⚠ patch ÉCHOUÉ (code $rc) : $p" >&2
    rej_any && rej_show >&2
    exit 1
  fi
  while [ "$#" -ge 2 ]; do
    f="$1"; m="$2"; shift 2
    grep -q "$m" "$f" || {
      echo "⚠ patch incomplet : '$m' absent de $f (après $p)" >&2
      rej_any && { echo "  hunks refusés :" >&2; rej_show >&2; }
      exit 1; }
  done
  # Marqueurs tous présents : les .rej restants sont des « déjà appliqué ».
  if rej_any; then
    echo "  (hunks déjà appliqués, sautés par --forward — .rej bénins, nettoyés)"
    rej_clean
  fi
}

# --- 1. SMP mac99 (série BALATON, révisée : async_run_on_cpu, GPIO 4 seul,
#        CPU 1 tenu en reset ligne basse et démarré au relâchement) ---
# Le garde teste un marqueur PROPRE À CETTE VERSION du patch : « define
# GPIO_RESET_CPU1 » (29/09/2026). Pas le nom seul : KL_GPIO_RESET_CPU1 est déjà
# dans un commentaire des versions précédentes. Les marqueurs précédents
# (« CPU1 reset », puis macio_gpio_set_extirq) survivent dans les versions
# suivantes : un arbre portant une série plus ancienne les aurait satisfaits,
# l'étape aurait été sautée et la nouvelle série jamais appliquée — en silence.
# C'est la classe de panne que ce script existe pour supprimer.
if ! grep -q "define GPIO_RESET_CPU1" hw/misc/macio/gpio.c; then
  # Arbre portant une ANCIENNE série : ses hunks tomberaient sur des fichiers
  # déjà modifiés. Retour à l'amont sur les trois fichiers du patch d'abord.
  if grep -q "CPU1 reset" hw/misc/macio/gpio.c; then
    echo "▶ ancienne série SMP détectée : retour à l'amont de gpio.c," \
         "mac_newworld.c et openpic.c avant de poser la nouvelle"
    git checkout -- hw/misc/macio/gpio.c hw/ppc/mac_newworld.c hw/intc/openpic.c
  fi
  echo "▶ patch SMP mac99"
  # --fuzz=0 : cette série s'applique exactement sur v11.1.2 (aucun décalage).
  # Un fuzz toléré, c'est un hunk posé ailleurs qu'à sa place, et la seule
  # trace serait un bug de timing dans l'invité.
  patch_strict "$ROOT/patches/smp-mac99/qemu-mac99-cpus-v2.patch" --fuzz=0
  rm -f hw/misc/macio/gpio.c.orig hw/ppc/mac_newworld.c.orig hw/intc/openpic.c.orig
fi

# --- 1 bis. OpenPIC : le reset oublie l'état verrouillé des sources ---
# patches/openpic/, docs/gel-doom3-baddisplay.md. Sans lui, un system_reset
# fait pendant une panique de l'invité (OHCI qui tient sa ligne haute) bloque
# le démarrage suivant dans une interruption perpétuelle. APRÈS l'étape 1 :
# elle peut rendre openpic.c à l'amont (git checkout) avant de poser la série.
if ! grep -q "src->input = level" hw/intc/openpic.c; then
  echo "▶ patch OpenPIC : reset des sources de niveau"
  patch_forward "$ROOT/patches/openpic/0001-openpic-reset-niveau.patch" \
    hw/intc/openpic.c           "src->input = level" \
    hw/intc/openpic.c           "opp->src\[i\].pending = 0" \
    hw/intc/openpic.c           "opp->src\[i\].input = opp->src\[i\].pending" \
    include/hw/ppc/openpic.h    "int input;"
  rm -f hw/intc/openpic.c.orig include/hw/ppc/openpic.h.orig
fi
while read -r f m; do
  [ -z "$f" ] && continue
  grep -q "$m" "$f" || {
    echo "⚠ patch openpic 0001 incomplet : '$m' absent de $f (voir patches/openpic/)" >&2; exit 1; }
done <<'OPENPIC_MARKERS'
hw/intc/openpic.c src->input = level
hw/intc/openpic.c opp->src\[i\].pending = 0
include/hw/ppc/openpic.h int input;
OPENPIC_MARKERS

# --- 2. Constantes GPIO (absentes de 9.2.0 ; enum MacioGPIORegisterBits depuis 10.x) ---
# La série SMP les apporte désormais elle-même (OUT_DATA / IN_DATA / OUT_ENABLE
# en tête de gpio.c) : le garde les trouve et saute l'insertion. Le bloc reste
# pour un arbre patché avec une série plus ancienne, qui les supposait sans les
# définir.
# 11.1 les déclare dans une enum : y ajouter une macro du même nom casserait l'enum.
if ! grep -qE "define OUT_ENABLE|OUT_ENABLE *=" hw/misc/macio/gpio.c; then
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
# Marqueur PROPRE À CETTE VERSION (29/09/2026) : la ligne qui branche l'IRQ 2
# (réception) en OldWorld. « screamer » seul est aussi dans l'ancienne version,
# qui connectait l'IRQ 1 deux fois : elle aurait satisfait le garde, et le
# correctif ne serait jamais parti. Aucun autre patch ne touche ces cinq
# fichiers : un arbre à l'ancienne version est ramené à l'amont sur eux avant
# de poser la nouvelle.
WIRE_MARK="sysbus_connect_irq(sbd, 2, qdev_get_gpio_in(pic_dev, OLDWORLD_SCREAMER_RX_IRQ))"
if ! grep -qF "$WIRE_MARK" hw/misc/macio/macio.c; then
  if grep -q "screamer" hw/misc/macio/macio.c; then
    echo "▶ ancien câblage screamer détecté : retour à l'amont de ses cinq fichiers"
    git checkout -- hw/misc/macio/macio.c include/hw/misc/macio/macio.h \
      hw/audio/Kconfig hw/audio/meson.build hw/ppc/Kconfig
  fi
  echo "▶ câblage screamer (Kconfig / meson / macio)"
  # --forward : les hunks déjà appliqués (meson/Kconfig) sont sautés au lieu de
  # faire échouer le patch ; ceux qui manquent sont posés. Les cinq fichiers du
  # câblage sont vérifiés un par un — un arbre à moitié recâblé compile et ne
  # branche rien.
  patch_forward "$ROOT/patches/screamer/0001-wire-screamer-build.patch" \
    hw/misc/macio/macio.c         screamer \
    include/hw/misc/macio/macio.h screamer \
    hw/audio/meson.build          CONFIG_SCREAMER \
    hw/audio/Kconfig              'config SCREAMER' \
    hw/ppc/Kconfig                'select SCREAMER'
  rm -f hw/misc/macio/macio.c.orig include/hw/misc/macio/macio.h.orig
fi
grep -qF "$WIRE_MARK" hw/misc/macio/macio.c || {
  echo "⚠ câblage screamer : l'IRQ de réception OldWorld n'est pas branchée" >&2
  echo "  (ancienne version du patch dans l'arbre ; voir patches/screamer/)" >&2
  exit 1; }

# --- 4. Device qfb-pci ---
echo "▶ installation de hw/display/qfb-pci.c"
cp "$ROOT/patches/qfb/qfb-pci.c" hw/display/qfb-pci.c
if ! grep -q "qfb-pci.c" hw/display/meson.build; then
  echo "▶ câblage meson/Kconfig"
  patch_strict "$ROOT/patches/qfb/0002-wire-qfb-pci-build.patch"
fi

# --- 4 bis. GPU paravirtuel qgpu-pci : device + cœur + backends soft/GL ---
# Le backend GL se compile toujours ; sans OpenGL.framework (macOS) ni EGL+GL
# (Linux) il devient un stub et le device retombe sur le backend logiciel.
echo "▶ installation de hw/display/qgpu-*.c"
cp "$ROOT"/patches/qgpu/qgpu-pci.c "$ROOT"/patches/qgpu/qgpu-core.c \
   "$ROOT"/patches/qgpu/qgpu-core.h "$ROOT"/patches/qgpu/qgpu-soft.c \
   "$ROOT"/patches/qgpu/qgpu-gl.c "$ROOT"/patches/qgpu/qgpu_proto.h \
   "$ROOT"/patches/qgpu/qgpu_abi.h hw/display/
if ! grep -q "qgpu-pci.c" hw/display/meson.build; then
  echo "▶ câblage meson/Kconfig qgpu"
  patch_strict "$ROOT/patches/qgpu/0003-wire-qgpu-pci-build.patch"
fi

# --- 4 ter. Flottant rapide PowerPC (propriété de CPU x-fast-fp) ---
# Le garde teste ppc_fp_primed() dans target/ppc/fpu_helper.c, et pas le
# premier fichier venu : c'est le seul marqueur dont l'absence serait totalement
# SILENCIEUSE — la propriété existerait, `-cpu g4,x-fast-fp=on` serait accepté,
# et le mode rapide ne ferait rien du tout. Un A/B aurait conclu « aucun gain »
# au lieu de « patch à moitié appliqué ». Même leçon que le câblage du Screamer.
# (Le garde ne prouve rien sur les autres fichiers : patch n'arrête PAS au
# premier hunk raté, il en rejette un et continue. D'où les contrôles ci-dessous,
# faits qu'on soit passé par le patch ou non.)
if ! grep -q "ppc_fp_primed" target/ppc/fpu_helper.c; then
  echo "▶ patch flottant rapide (x-fast-fp)"
  patch_forward "$ROOT/patches/fastfp/0001-ppc-fast-fp.patch" \
    fpu/softfloat.c               float64r32_gen2 \
    include/fpu/softfloat-types.h no_hardfloat \
    target/ppc/cpu.h              fast_fp \
    target/ppc/cpu_init.c         x-fast-fp \
    target/ppc/fpu_helper.c       ppc_fp_primed
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
# Contrôle NÉGATIF — le seul qui attrape le hunk le plus silencieux du lot.
# fpu/softfloat.c a douze hunks ; celui qui RETIRE
#   #if defined(TARGET_PPC) || defined(__FAST_MATH__)
# peut être rejeté tout seul. Les cinq marqueurs positifs ci-dessus vivent dans
# d'autres hunks : ils passent au vert, la propriété x-fast-fp existe, le
# lanceur annonce « + FLOTTANT RAPIDE »… et QEMU_NO_HARDFLOAT reste posé à la
# compilation, donc can_use_fpu() rend toujours faux. Zéro gain, zéro message.
if grep -qE '^#if[[:space:]]+defined\(TARGET_PPC\)' fpu/softfloat.c; then
  echo "⚠ patch fastfp incomplet : fpu/softfloat.c désactive encore hardfloat" >&2
  echo "  pour TARGET_PPC (le hunk qui retire ce veto a été rejeté)." >&2
  echo "  Le mode rapide existerait sans rien accélérer. Voir patches/fastfp/" >&2
  echo "  et les .rej de l'arbre." >&2
  exit 1
fi

# --- 4 quater. Moins d'appels de helper par instruction flottante ---
# Patch séparé du précédent EXPRÈS : il ne change aucun comportement (vérifié
# octet pour octet contre un QEMU non patché), il ne fait que réduire le coût.
# Les garder séparables permet d'attribuer les gains en A/B — et de reverter
# celui-ci seul si jamais il régressait :  NO_FASTFP2=1 ./scripts/build_qemu_qfb.sh
# sur un arbre PROPRE (sur un arbre déjà patché, il est déjà là).
if [ -z "${NO_FASTFP2:-}" ]; then
  if ! grep -q "fp_prime_mask" target/ppc/cpu.h; then
    echo "▶ patch flottant rapide — réduction des appels de helper"
    patch_forward "$ROOT/patches/fastfp/0002-ppc-fewer-fp-helpers.patch" \
      target/ppc/cpu.h                   fp_prime_mask \
      target/ppc/translate/fp-impl.c.inc fprf_check_float64
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

# --- 4 quinquies. TCG : TLB gardé d'un jeu de segments à l'autre (x-sr-tlb) ---
# patches/tcg/, docs/tcg-g4.md. Comme le flottant rapide : une propriété de CPU,
# éteinte par défaut (SRTLB=1 ./run_tiger.sh l'allume), et sans elle le binaire
# se comporte comme avant — hors un branchement par écriture de registre de
# segment, par remplissage du TLB et par recalcul des hflags. NO_TCG=1 saute
# l'étape sur un arbre propre. Le garde teste le marqueur du DERNIER fichier du
# patch (mmu_helper.c) : un patch à moitié posé laisserait la propriété exister
# sans que store_sr cesse de vider tout le TLB — un A/B conclurait « aucun gain ».
if [ -z "${NO_TCG:-}" ]; then
  if ! grep -q "ppc_sr_tlb_fill" target/ppc/mmu_helper.c; then
    echo "▶ patch TCG : TLB gardé d'un jeu de segments à l'autre (x-sr-tlb)"
    patch_forward "$ROOT/patches/tcg/0001-ppc-sr-tlb.patch" \
      target/ppc/cpu.h          sr_snap_ok \
      target/ppc/cpu_init.c     x-sr-tlb \
      target/ppc/helper_regs.h  ppc_sr_tlb_fill \
      target/ppc/helper_regs.c  ppc_sr_tlb_check \
      target/ppc/mmu_helper.c   ppc_sr_tlb_fill
    rm -f target/ppc/cpu.h.orig target/ppc/cpu_init.c.orig target/ppc/helper_regs.h.orig \
          target/ppc/helper_regs.c.orig target/ppc/mmu_helper.c.orig
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0001 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG_MARKERS'
target/ppc/cpu.h sr_snap_ok
target/ppc/cpu_init.c x-sr-tlb
target/ppc/helper_regs.h ppc_sr_tlb_fill
target/ppc/helper_regs.c ppc_sr_tlb_check
target/ppc/mmu_helper.c ppc_sr_tlb_fill
target/ppc/mmu_helper.c TLB_NEED_SR_CHECK
TCG_MARKERS
  # --- 4 sexies. lfs/stfs sans helper (x-lfs-inline), par-dessus le 0001 ---
  # patches/tcg/0002, docs/tcg-g4.md §8 : les conversions simple ↔ double de
  # lfs/stfs en ops TCG entières, sans branchement, prouvées au bit près
  # (tools/tcg/lfsproof.sh, tools/guest/jobs/lfstest). Propriété éteinte par
  # défaut (LFSINLINE=1 ./run_tiger.sh). Le garde teste le marqueur du DERNIER
  # fichier (fp-impl.c.inc) : sans lui la propriété existerait sans rien changer.
  if ! grep -q "gen_todouble_inline" target/ppc/translate/fp-impl.c.inc; then
    echo "▶ patch TCG : lfs/stfs sans helper (x-lfs-inline)"
    patch_forward "$ROOT/patches/tcg/0002-ppc-lfs-inline.patch" \
      target/ppc/cpu.h                   lfs_inline \
      target/ppc/cpu_init.c              x-lfs-inline \
      target/ppc/translate.c             "ctx->lfs_inline = env->lfs_inline" \
      target/ppc/translate/fp-impl.c.inc gen_tosingle_inline
    rm -f target/ppc/cpu.h.orig target/ppc/cpu_init.c.orig target/ppc/translate.c.orig \
          target/ppc/translate/fp-impl.c.inc.orig
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0002 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG2_MARKERS'
target/ppc/cpu.h lfs_inline
target/ppc/cpu_init.c x-lfs-inline
target/ppc/translate.c ctx->lfs_inline = env->lfs_inline
target/ppc/translate/fp-impl.c.inc gen_todouble_inline
target/ppc/translate/fp-impl.c.inc gen_tosingle_inline
TCG2_MARKERS
  # --- 4 septies. AltiVec : flottant à 4 voies d'un coup (x-vfp-fast), vperm
  # par table (x-vperm-fast) — patches/tcg/0003 et 0004, docs/tcg-g4.md §9-10.
  # Propriétés éteintes par défaut (VFPFAST=1, VPERMFAST=1 ./run_tiger.sh).
  # Gardes : le marqueur du dernier fichier de chaque patch.
  if ! grep -q "vfp_fma4" target/ppc/int_helper.c; then
    echo "▶ patch TCG : flottant AltiVec à 4 voies (x-vfp-fast)"
    patch_forward "$ROOT/patches/tcg/0003-ppc-vfp-fast.patch" \
      target/ppc/cpu.h        "bool vfp_fast" \
      target/ppc/cpu_init.c   x-vfp-fast \
      target/ppc/int_helper.c vfp_fma4
    rm -f target/ppc/cpu.h.orig target/ppc/cpu_init.c.orig target/ppc/int_helper.c.orig
  fi
  if ! grep -q "gen_helper_VPERM_FAST" target/ppc/translate/vmx-impl.c.inc; then
    echo "▶ patch TCG : vperm par table (x-vperm-fast)"
    patch_forward "$ROOT/patches/tcg/0004-ppc-vperm-fast.patch" \
      target/ppc/cpu.h                   "bool vperm_fast" \
      target/ppc/cpu_init.c              x-vperm-fast \
      target/ppc/helper.h                VPERM_FAST \
      target/ppc/int_helper.c            helper_VPERM_FAST \
      target/ppc/translate.c             "ctx->vperm_fast = env->vperm_fast" \
      target/ppc/translate/vmx-impl.c.inc gen_helper_VPERM_FAST
    rm -f target/ppc/cpu.h.orig target/ppc/cpu_init.c.orig target/ppc/helper.h.orig \
          target/ppc/int_helper.c.orig target/ppc/translate.c.orig \
          target/ppc/translate/vmx-impl.c.inc.orig
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0003/0004 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG34_MARKERS'
target/ppc/cpu_init.c x-vfp-fast
target/ppc/int_helper.c vfp_add4
target/ppc/int_helper.c vfp_fma4
target/ppc/cpu_init.c x-vperm-fast
target/ppc/int_helper.c helper_VPERM_FAST
target/ppc/translate/vmx-impl.c.inc gen_helper_VPERM_FAST
TCG34_MARKERS
  # --- 4 octies. Placement du tampon du JIT (x-jit-near, x-jit-addr) ---
  # patches/tcg/0006, docs/tcg-g4.md §14 : propriétés de l'ACCÉLÉRATEUR (-accel
  # tcg,x-jit-near=on), éteintes par défaut (JITNEAR=1 ./run_tiger.sh). Sur Apple
  # M4, un tampon posé hors de la fenêtre de 4 Gio du texte de QEMU (un
  # lancement sur deux environ) ralentit tout le processus de ~10 % : c'étaient
  # « les deux régimes ». Le patch imprime aussi, toujours, où le tampon est posé.
  # Marqueur PROPRE À CETTE VERSION (29/09/2026, bug hunt 4) : « x-jit-near
  # sur l'alias », imprimé par le remap split-wx depuis que la recherche avec
  # réduction porte sur l'alias RX et non plus sur la vue RW. « tcg_jit_near »
  # est dans toutes les versions, et « split-wx, alias RX » dans la
  # précédente : l'un ou l'autre aurait satisfait un arbre à l'ancienne, et
  # le correctif ne serait jamais parti. Piège voisin :
  # « size = region.total_size » est une sous-chaîne de « tb_size = … » — le
  # contrôle d'après l'ancre en début de ligne. Aucun autre patch ne touche ces
  # trois fichiers (0008 non plus) : retour à l'amont sur eux, puis nouvelle
  # version.
  if ! grep -q "x-jit-near sur l'alias" tcg/region.c; then
    if grep -q "tcg_jit_near" tcg/region.c; then
      echo "▶ ancien patch x-jit-near détecté : retour à l'amont de ses trois fichiers"
      git checkout -- accel/tcg/tcg-all.c include/tcg/startup.h tcg/region.c
    fi
    echo "▶ patch TCG : placement du tampon du JIT (x-jit-near)"
    patch_forward "$ROOT/patches/tcg/0006-tcg-jit-near.patch" \
      accel/tcg/tcg-all.c   x-jit-near \
      include/tcg/startup.h tcg_jit_near \
      tcg/region.c          tcg_jit_near
    rm -f accel/tcg/tcg-all.c.orig include/tcg/startup.h.orig tcg/region.c.orig
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0006 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG6_MARKERS'
accel/tcg/tcg-all.c x-jit-near
include/tcg/startup.h tcg_jit_near
tcg/region.c tcg_jit_near
tcg/region.c tb_size = region.total_size
tcg/region.c ^    size = region.total_size;
tcg/region.c split-wx, alias RX
tcg/region.c x-jit-near sur l'alias
TCG6_MARKERS
  # --- 4 nonies. Flottant scalaire simple sans helper (x-fp-inline) ---
  # patches/tcg/0007, docs/tcg-g4.md §15 : fadds fsubs fmuls fmadds fmsubs
  # fnmadds fnmsubs fcmpu prennent un chemin court (un appel pur + FPRF/FI/FPCC
  # en ligne) quand le FPSCR est amorcé sans trappe et les opérandes des simples
  # normaux, les helpers d'origine sinon ; prouvé au bit près (tools/tcg/fpproof.sh,
  # x-fp-verify, tools/guest/jobs/fptest). Par-dessus 0001-0004 (et le 0002 de
  # patches/fastfp). Propriété éteinte par défaut (FPINLINE=1 ./run_tiger.sh).
  # Garde : le marqueur du traducteur (dernier fichier), sans lequel la propriété
  # existerait sans rien changer.
  if ! grep -q "do_fp_inline" target/ppc/translate/fp-impl.c.inc; then
    echo "▶ patch TCG : flottant scalaire simple sans helper (x-fp-inline)"
    patch_forward "$ROOT/patches/tcg/0007-ppc-fp-inline.patch" \
      target/ppc/cpu.h                   "bool fp_inline" \
      target/ppc/cpu_init.c              x-fp-inline \
      target/ppc/fpu_helper.c            helper_fp32_fast \
      target/ppc/helper.h                fp32_fast \
      target/ppc/internal.h              FPI_GATE_MASK \
      target/ppc/translate.c             "ctx->fp_inline = env->fp_inline" \
      target/ppc/translate/fp-impl.c.inc do_fp_inline
    rm -f target/ppc/cpu.h.orig target/ppc/cpu_init.c.orig target/ppc/fpu_helper.c.orig \
          target/ppc/helper.h.orig target/ppc/internal.h.orig target/ppc/translate.c.orig \
          target/ppc/translate/fp-impl.c.inc.orig
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0007 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG7_MARKERS'
target/ppc/cpu_init.c x-fp-inline
target/ppc/fpu_helper.c helper_fp32_fast
target/ppc/fpu_helper.c helper_fpv_arith
target/ppc/translate.c ctx->fp_inline = env->fp_inline
target/ppc/translate/fp-impl.c.inc do_fp_inline
target/ppc/translate/fp-impl.c.inc gen_fcmpu_inline
TCG7_MARKERS
  # --- 4 decies. Sorties indirectes : recherche en ligne, cache de sauts par
  # mmu_idx (x-ret-inline, x-jc-idx) --- patches/tcg/0008, docs/tcg-g4.md §16 :
  # blr, bctr… sondent le cache de sauts dans le code généré (helper sur un
  # raté), et un vidage du TLB ne jette que les entrées des mmu_idx vidés.
  # Mêmes blocs choisis que helper_lookup_tb_ptr (x-ret-verify, smctest).
  # Propriétés éteintes par défaut (RETINLINE=1 JCIDX=1 ./run_tiger.sh).
  # Garde : le marqueur du traducteur ppc (dernier fichier du patch).
  if ! grep -q "gen_goto_ptr_exit" target/ppc/translate.c; then
    echo "▶ patch TCG : sorties indirectes en ligne, cache de sauts par mmu_idx (x-ret-inline, x-jc-idx)"
    patch_forward "$ROOT/patches/tcg/0008-tcg-ret-inline.patch" \
      accel/tcg/cpu-exec.c        lookup_tb_ptr_check \
      accel/tcg/cputlb.c          pomppc_code_phys_nofault \
      accel/tcg/tb-jmp-cache.h    "uint8_t idx" \
      accel/tcg/tcg-runtime.h     lookup_tb_ptr_check \
      accel/tcg/translate-all.c   tcg_flush_jmp_cache_idx \
      accel/tcg/translator.c      translator_lookup_and_goto_ptr_inline \
      accel/tcg/internal-common.h pomppc_jc_flush_stat \
      include/exec/tb-flush.h     tcg_flush_jmp_cache_idx \
      include/exec/translator.h   translator_can_goto_ptr_inline \
      include/hw/core/cpu.h       jc_by_idx \
      include/tcg/tcg-op-common.h tcg_gen_goto_ptr \
      target/ppc/cpu.h            "bool ret_inline" \
      target/ppc/cpu_init.c       x-ret-inline \
      tcg/tcg-op.c                "void tcg_gen_goto_ptr" \
      target/ppc/translate.c      gen_goto_ptr_exit
    for f in accel/tcg/cpu-exec.c accel/tcg/cputlb.c accel/tcg/tb-jmp-cache.h \
             accel/tcg/tcg-runtime.h accel/tcg/translate-all.c accel/tcg/translator.c \
             include/exec/exec-all.h include/exec/tb-flush.h include/exec/translator.h \
             include/hw/core/cpu.h include/tcg/tcg-op-common.h target/ppc/cpu.h \
             target/ppc/cpu_init.c target/ppc/translate.c tcg/tcg-op.c; do
      rm -f "$f.orig"
    done
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0008 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG8_MARKERS'
accel/tcg/cpu-exec.c lookup_tb_ptr_check
accel/tcg/cputlb.c tcg_flush_jmp_cache_idx
accel/tcg/translator.c translator_lookup_and_goto_ptr_inline
target/ppc/cpu_init.c x-jc-idx
target/ppc/translate.c gen_goto_ptr_exit
TCG8_MARKERS
  # --- 4 undecies. Code réécrit par l'autre vCPU (MTTCG) --- patches/tcg/0010,
  # docs/tcg-g4.md §17 : trois courses de QEMU 9.2 où une écriture d'un vCPU
  # dans une page de code n'invalide plus les blocs de l'autre (page protégée
  # APRÈS la lecture du code par le traducteur ; TLB_NOTDIRTY retiré ou omis
  # sur un test fait hors du verrou du TLB) : corrigées sans condition. Plus
  # la propriété x-icbi-sync (éteinte par défaut dans QEMU, allumée par
  # run_tiger.sh, ICBISYNC=0 l'éteint) :
  # icbi invalide les blocs de sa ligne de cache (écriture invalidée AVANT
  # d'être faite). Garde : le marqueur du dernier fichier du patch.
  if ! grep -q "tb_invalidate_phys_line_sync" target/ppc/mem_helper.c; then
    echo "▶ patch TCG : code réécrit par l'autre vCPU (courses de l'invalidation, x-icbi-sync)"
    patch_forward "$ROOT/patches/tcg/0010-tcg-smc-mttcg.patch" \
      accel/tcg/cputlb.c      "ram_addr_t ram_addr)" \
      accel/tcg/tb-maint.c    tb_page_protect_locked \
      accel/tcg/tb-maint.c    tb_invalidate_phys_line_sync \
      include/exec/translation-block.h tb_invalidate_phys_line_sync \
      target/ppc/cpu.h        "bool icbi_sync" \
      target/ppc/cpu_init.c   x-icbi-sync \
      target/ppc/mem_helper.c tb_invalidate_phys_line_sync
    for f in accel/tcg/cputlb.c accel/tcg/tb-maint.c include/exec/exec-all.h \
             target/ppc/cpu.h target/ppc/cpu_init.c target/ppc/mem_helper.c; do
      rm -f "$f.orig"
    done
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0010 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG10_MARKERS'
accel/tcg/cputlb.c POMPPC tcg/0010
accel/tcg/tb-maint.c tb_page_protect_locked
accel/tcg/tb-maint.c tb_invalidate_phys_line_sync
target/ppc/cpu_init.c x-icbi-sync
target/ppc/mem_helper.c tb_invalidate_phys_line_sync
TCG10_MARKERS
  # --- 4 duodecies. Taille du cache de sauts (x-jc-bits, propriété de
  # l'ACCÉLÉRATEUR, 12 = QEMU d'origine, jusqu'à 16) --- patches/tcg/0011,
  # docs/tcg-g4.md §18 : hachage, vidages par page et journal par mmu_idx
  # suivent la taille choisie au démarrage (JCBITS=14 ./run_tiger.sh).
  # Garde : le marqueur du dernier fichier du patch.
  if ! grep -q "x-jc-bits" accel/tcg/tcg-all.c; then
    echo "▶ patch TCG : taille du cache de sauts (x-jc-bits)"
    patch_strict "$ROOT/patches/tcg/0011-tcg-jc-bits.patch"
    for f in accel/tcg/cpu-exec.c accel/tcg/tb-jmp-cache.h accel/tcg/tcg-all.c \
             accel/tcg/translate-all.c include/exec/exec-all.h; do
      rm -f "$f.orig"
    done
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0011 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG11_MARKERS'
accel/tcg/tb-jmp-cache.h TB_JMP_CACHE_MAX_BITS
accel/tcg/translate-all.c tb_jmp_cache_bits_frozen
accel/tcg/cpu-exec.c pomppc_jc_scan_stat
accel/tcg/tcg-all.c x-jc-bits
TCG11_MARKERS
  # --- 4 terdecies. mtmsr/rfi sans verrou global quand la ligne
  # d'interruption ne change pas (x-msr-nobql, x-msr-nobql-verify) ---
  # patches/tcg/0012, docs/tcg-g4.md §19 : compteur de séquence contre les
  # mises à jour sous verrou, EXITTB redondant de rfi/mtmsr supprimé.
  # Garde : le marqueur du dernier fichier du patch.
  if ! grep -q "hreg_store_msr_tb" target/ppc/helper_regs.h; then
    echo "▶ patch TCG : mtmsr/rfi sans verrou global (x-msr-nobql)"
    patch_strict "$ROOT/patches/tcg/0012-ppc-msr-nobql.patch"
    for f in target/ppc/cpu.h target/ppc/cpu_init.c target/ppc/excp_helper.c \
             target/ppc/helper_regs.c target/ppc/helper_regs.h; do
      rm -f "$f.orig"
    done
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0012 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG12_MARKERS'
target/ppc/cpu_init.c x-msr-nobql
target/ppc/excp_helper.c ppc_maybe_interrupt_nolock
target/ppc/helper_regs.c hreg_store_msr_tb
target/ppc/helper_regs.h hreg_store_msr_tb
TCG12_MARKERS
  # --- 4 quaterdecies. Flottant scalaire simple en un appel, sans branchement
  # (x-fp-flat) --- patches/tcg/0013, docs/tcg-g4.md §22 : les instructions de
  # x-fp-inline en un seul appel (chemin court ou séquence d'origine, en C),
  # plus de bloc de base coupé à chaque instruction. Garde : le marqueur du
  # dernier fichier du patch.
  if ! grep -q "do_fp_flat" target/ppc/translate/fp-impl.c.inc; then
    echo "▶ patch TCG : flottant scalaire sans branchement (x-fp-flat)"
    patch_strict "$ROOT/patches/tcg/0013-ppc-fp-flat.patch"
    for f in target/ppc/cpu.h target/ppc/cpu_init.c target/ppc/fpu_helper.c \
             target/ppc/helper.h target/ppc/translate.c target/ppc/translate/fp-impl.c.inc; do
      rm -f "$f.orig"
    done
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0013 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG13_MARKERS'
target/ppc/cpu_init.c x-fp-flat
target/ppc/fpu_helper.c helper_fp32_flat
target/ppc/helper.h fp32_flat
target/ppc/translate/fp-impl.c.inc do_fp_flat
TCG13_MARKERS
  # --- 4 quindecies. Le chemin court du flottant scalaire simple par le FPU de
  # l'hôte dans le code généré (x-fp-native) --- patches/tcg/0014,
  # docs/tcg-g4.md §22 : op TCG ppc_fp32 (cœur de TCG et backend arm64), chemin
  # lent hors ligne vers le helper de x-fp-flat. Garde : le marqueur du dernier
  # fichier du patch.
  if ! grep -q "do_fp_native" target/ppc/translate/fp-impl.c.inc; then
    echo "▶ patch TCG : flottant scalaire natif (x-fp-native)"
    patch_strict "$ROOT/patches/tcg/0014-tcg-fp-native.patch"
    for f in include/tcg/tcg-opc.h include/tcg/tcg.h include/tcg/tcg-op-common.h \
             tcg/tcg.c tcg/tcg-op.c tcg/tcg-ldst.c.inc tcg/optimize.c \
             tcg/aarch64/tcg-target.h tcg/aarch64/tcg-target-con-set.h \
             tcg/aarch64/tcg-target.c.inc target/ppc/cpu.h target/ppc/cpu_init.c \
             target/ppc/fpu_helper.c target/ppc/helper.h target/ppc/internal.h \
             target/ppc/translate.c target/ppc/translate/fp-impl.c.inc; do
      rm -f "$f.orig"
    done
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0014 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG14_MARKERS'
include/tcg/tcg-opc.h ppc_fp32
tcg/aarch64/tcg-target.c.inc tcg_out_ppc_fp32
target/ppc/cpu_init.c x-fp-native
target/ppc/fpu_helper.c ppc_fp32_native_slow
target/ppc/translate/fp-impl.c.inc do_fp_native
TCG14_MARKERS
  # --- 4 sexdecies. La base de temps par le compteur de l'hôte (x-tb-fast) ---
  # patches/tcg/0015, docs/tcg-g4.md §23 : get_clock() = cntvct_el0 + K quand
  # la propriété le demande, mftb sans clock_gettime ni division 128 bits.
  if ! grep -q "qemu_raw_clock_enable" util/qemu-timer-common.c; then
    echo "▶ patch TCG : base de temps par le compteur de l'hôte (x-tb-fast)"
    patch_strict "$ROOT/patches/tcg/0015-ppc-tb-fast.patch"
    for f in hw/ppc/ppc.c include/qemu/timer.h include/sysemu/cpu-timers.h \
             system/cpu-timers.c target/ppc/cpu.h target/ppc/cpu_init.c \
             util/qemu-timer-common.c; do
      rm -f "$f.orig"
    done
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0015 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG15_MARKERS'
util/qemu-timer-common.c qemu_raw_clock_enable
hw/ppc/ppc.c tbf_load
target/ppc/cpu_init.c x-tb-fast
TCG15_MARKERS
  # --- 4 septdecies. Le flottant double par le FPU de l'hôte (x-fp-native64) ---
  # patches/tcg/0016, docs/tcg-g4.md §24 : les formes double de l'op ppc_fp32.
  if ! grep -q "do_fpd_ab" target/ppc/translate/fp-impl.c.inc; then
    echo "▶ patch TCG : flottant double natif (x-fp-native64)"
    patch_strict "$ROOT/patches/tcg/0016-tcg-fp-native64.patch"
    for f in target/ppc/cpu.h target/ppc/cpu_init.c target/ppc/fpu_helper.c \
             target/ppc/internal.h target/ppc/translate.c \
             target/ppc/translate/fp-impl.c.inc tcg/aarch64/tcg-target.c.inc; do
      rm -f "$f.orig"
    done
  fi
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0016 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG16_MARKERS'
target/ppc/translate/fp-impl.c.inc do_fpd_ab
tcg/aarch64/tcg-target.c.inc pfp_zon64
target/ppc/fpu_helper.c fpi_fp64
target/ppc/cpu_init.c x-fp-native64
TCG16_MARKERS
  # --- 4 duodevicies. Les mêmes sur un hôte x86-64 (tcg/0017-0020) ---
  # patches/tcg/0017, docs/tcg-g4.md §25 : l'émetteur x86_64 de l'op ppc_fp32
  # (x-fp-native, x-fp-native64 : VEX et FMA3, sondés à l'exécution) ; 0018 :
  # x-tb-fast par le TSC invariant (Linux) ; 0019 : vperm par pshufb (AVX).
  if ! grep -q "TCG_TARGET_PPC_FP32_IMPL" tcg/tcg-has.h; then
    echo "▶ patch TCG : flottant natif sur hôte x86-64 (x-fp-native, x-fp-native64)"
    patch_strict "$ROOT/patches/tcg/0017-tcg-fp-native-x86.patch"
  fi
  if ! grep -q "qemu_raw_clock_mult" util/qemu-timer-common.c; then
    echo "▶ patch TCG : base de temps par le TSC sur hôte x86-64 (x-tb-fast)"
    patch_strict "$ROOT/patches/tcg/0018-ppc-tb-fast-x86.patch"
  fi
  if ! grep -q "vperm_fast_avx" target/ppc/int_helper.c; then
    echo "▶ patch TCG : vperm par pshufb sur hôte x86-64 (x-vperm-fast)"
    patch_strict "$ROOT/patches/tcg/0019-ppc-vperm-fast-x86.patch"
  fi
  # 0020 : vaddfp/vsubfp (AVX) et vmaddfp/vnmsubfp (FMA3) à 4 voies d'un coup ;
  # sans lui, x-vfp-fast n'accélérait pas les FMA AltiVec sur x86 (docs §26).
  if ! grep -q "vfp_fma4_fma3" target/ppc/int_helper.c; then
    echo "▶ patch TCG : flottant AltiVec par AVX/FMA3 sur hôte x86-64 (x-vfp-fast)"
    patch_strict "$ROOT/patches/tcg/0020-ppc-vfp-fast-x86.patch"
  fi
  for f in host/include/x86_64/host/cpuinfo.h hw/ppc/ppc.c include/qemu/cpuid.h \
           include/qemu/timer.h target/ppc/cpu.h target/ppc/cpu_init.c \
           target/ppc/fpu_helper.c target/ppc/int_helper.c target/ppc/translate/fp-impl.c.inc \
           tcg/aarch64/tcg-target-has.h tcg/tcg-has.h tcg/tcg.c \
           tcg/x86_64/tcg-target-con-set.h tcg/x86_64/tcg-target-has.h tcg/x86_64/tcg-target.c.inc \
           util/cpuinfo-i386.c util/qemu-timer-common.c; do
    rm -f "$f.orig"
  done
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0017-0020 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG17_MARKERS'
tcg/x86_64/tcg-target.c.inc tcg_out_ppc_fp32_slow_path
tcg/tcg-has.h TCG_TARGET_PPC_FP32_IMPL
util/cpuinfo-i386.c CPUINFO_FMA
util/qemu-timer-common.c qemu_raw_clock_mult
target/ppc/int_helper.c vperm_fast_avx
target/ppc/int_helper.c vfp_fma4_fma3
TCG17_MARKERS
  # --- 4 vicies. DOOM 3 sur hôte x86-64 (tcg/0021-0024), docs/tcg-g4.md §28 ---
  # 0021 : vsldoi, vmrghw/vmrglw, stve[bhw]x en ops TCG (x-vmx-inline) ; 0022 :
  # l'op TCG ppc_vfp, vaddfp/vsubfp/vmaddfp/vnmsubfp par le FPU de l'hôte dans le
  # code généré (x-vfp-native ; émetteurs x86_64 et aarch64) ; 0023 : mftb divise
  # par la constante 40. Propriétés éteintes par défaut (0023 : sous x-tb-fast).
  if ! grep -q "x-vmx-inline" target/ppc/cpu_init.c; then
    echo "▶ patch TCG : AltiVec en ligne (x-vmx-inline)"
    patch_strict "$ROOT/patches/tcg/0021-ppc-vmx-inline.patch"
  fi
  if ! grep -q "TCG_TARGET_PPC_VFP_IMPL" tcg/tcg-has.h; then
    echo "▶ patch TCG : flottant AltiVec dans le code généré (x-vfp-native)"
    patch_strict "$ROOT/patches/tcg/0022-tcg-vfp-native.patch"
  fi
  if ! grep -q "POMPPC tcg/0023" hw/ppc/ppc.c; then
    echo "▶ patch TCG : mftb sans division 64 bits (x-tb-fast)"
    patch_strict "$ROOT/patches/tcg/0023-ppc-tb-div.patch"
  fi
  # 0024 : tampon du JIT à moins de 2 Gio du texte sous Linux x86-64 (x-jit-rel32)
  if ! grep -q "tcg_jit_rel32" tcg/region.c; then
    echo "▶ patch TCG : tampon du JIT près du texte sous Linux x86-64 (x-jit-rel32)"
    patch_strict "$ROOT/patches/tcg/0024-tcg-jit-rel32.patch"
  fi
  for f in accel/tcg/tcg-all.c include/tcg/startup.h tcg/region.c \
           hw/ppc/ppc.c include/tcg/tcg-op-common.h include/tcg/tcg-opc.h \
           target/ppc/cpu.h target/ppc/cpu_init.c target/ppc/helper.h \
           target/ppc/int_helper.c target/ppc/mem_helper.c target/ppc/translate.c \
           target/ppc/translate/vmx-impl.c.inc tcg/aarch64/tcg-target-has.h \
           tcg/aarch64/tcg-target.c.inc tcg/optimize.c tcg/tcg-has.h tcg/tcg-op.c \
           tcg/tcg.c tcg/x86_64/tcg-target-has.h tcg/x86_64/tcg-target.c.inc; do
    rm -f "$f.orig"
  done
  while read -r f m; do
    [ -z "$f" ] && continue
    grep -q "$m" "$f" || {
      echo "⚠ patch tcg 0021-0024 incomplet : '$m' absent de $f (voir patches/tcg/)" >&2; exit 1; }
  done <<'TCG21_MARKERS'
target/ppc/cpu_init.c x-vmx-inline
target/ppc/mem_helper.c helper_vmx_verify_stve
target/ppc/cpu_init.c x-vfp-native
tcg/x86_64/tcg-target.c.inc tcg_out_ppc_vfp_slow_path
tcg/aarch64/tcg-target.c.inc tcg_out_ppc_vfp_slow_path
tcg/tcg.c tcg_reg_alloc_ppc_vfp
hw/ppc/ppc.c POMPPC tcg/0023
tcg/region.c tcg_jit_rel32
accel/tcg/tcg-all.c x-jit-rel32
TCG21_MARKERS
  # Existing patched trees also need the signed-zero fix (fresh trees get it
  # from 0003/0022). Apply each migration independently and check its marker.
  if grep -q 'ok &= prod_zero || is_inf' target/ppc/int_helper.c; then
    patch_strict "$ROOT/patches/tcg/fixes/0003-nmsub-zero.patch"
  fi
  if grep -q '^    pvf_3(s, PVF_ORR16B, u, u, t);' tcg/aarch64/tcg-target.c.inc; then
    patch_strict "$ROOT/patches/tcg/fixes/0022-neon-nmsub-zero.patch"
  fi
  # M4: proven fixes and optional variants; QEMU properties default off.
  if ! grep -q 'x-lmw-vector' target/ppc/cpu_init.c; then
    patch_strict "$ROOT/patches/tcg/0025-ppc-lmw-vector.patch"
  fi
  if ! grep -q 'x-jc-word' accel/tcg/tcg-all.c; then
    patch_strict "$ROOT/patches/tcg/0026-tcg-jc-word.patch"
  fi
  if ! grep -q 'saved_addr = tcg_jit_addr' tcg/region.c; then
    patch_strict "$ROOT/patches/tcg/0027-tcg-splitwx-rw-away.patch"
  fi
  if grep -q 'tcg_out_addi_ptr(s, TCG_REG_X1 + i' tcg/aarch64/tcg-target.c.inc; then
    patch_strict "$ROOT/patches/tcg/0028-tcg-vfp-neon-slow.patch"
  fi
  while read -r f m; do
    grep -q "$m" "$f" || {
      echo "⚠ patch M4 incomplet : '$m' absent de $f" >&2; exit 1; }
  done <<'TCG_M4_MARKERS'
target/ppc/int_helper.c prod_zero && !nres
tcg/aarch64/tcg-target.c.inc kind != PVF_NMSUB
target/ppc/lmw-vector.h ppc_lmw_vector
target/ppc/mem_helper.c ppc_stmw_vector
target/ppc/cpu_init.c x-lmw-vector
accel/tcg/tcg-all.c x-jc-word
accel/tcg/translator.c tb_jmp_cache_word
tcg/region.c saved_addr = tcg_jit_addr
tcg/aarch64/tcg-target.c.inc TCG_TYPE_PTR, arg
TCG_M4_MARKERS
fi

# --- 4 undevicies. Tablette USB pour Tiger 10.4.11 (x-abs-margin) ---
# patches/usbhid/0001 : l'IOHIDEventDriver de 10.4.11 retire 7,5 % de chaque bout
# des axes absolus ; la tablette rend ses coordonnées dans la fenêtre gardée.
if ! grep -q "abs_margin_pct" include/hw/input/hid.h; then
  echo "▶ patch : marge absolue de usb-tablet (x-abs-margin, Tiger 10.4.11)"
  patch_strict "$ROOT/patches/usbhid/0001-usb-tablet-abs-margin.patch"
  rm -f include/hw/input/hid.h.orig hw/input/hid.c.orig hw/usb/dev-hid.c.orig
fi
grep -q "x-abs-margin" hw/usb/dev-hid.c || {
  echo "⚠ patch usbhid 0001 incomplet : x-abs-margin absent de hw/usb/dev-hid.c" >&2; exit 1; }

# --- 5. Build ---
# QEMU_OPT (éteint par défaut, docs/vitesse-doom3-x86.md) : variantes de
# compilation, liste séparée par des virgules, construites dans build-<variante>
# (jamais dans build/, le binaire de référence) et sondées comme lui :
#   native   -O3 -march=native (binaire propre à CETTE machine). -O3 passe par
#            -Doptimization=3 : un -O3 dans --extra-cflags est écrasé par le -O2
#            que meson ajoute après (−2,4 % sur DOOM 3, docs/vitesse-doom3-x86.md §5.5)
#   nohard   sans les durcissements de QEMU (-fzero-call-used-regs=used-gpr,
#            -ftrivial-auto-var-init=zero) ni protection de pile
#   lto      optimisation à l'édition de liens (-Db_lto=true)
#   pgo-gen  instrumenté pour le profil (PGO_DIR, défaut <arbre>/pgo-data) : jouer
#            puis quitter QEMU proprement (les .gcda s'écrivent à la sortie)
#   pgo      même dossier de build, recompilé avec le profil (-fprofile-use)
# Ex. : QEMU_SRC=~/src/qemu-opt QEMU_OPT=native,nohard,lto ./scripts/build_qemu_qfb.sh
#       QEMU_SRC=~/src/qemu-opt QEMU_OPT=native,nohard,lto,pgo-gen …  (jouer)  puis
#       QEMU_SRC=~/src/qemu-opt QEMU_OPT=native,nohard,lto,pgo …
# Pour pgo-gen / pgo, le dossier de build est le même (build-<…>-pgo) : les .gcda
# sont rangés par chemin d'objet. QEMU_BUILD=<dossier> force le nom.
#
# QEMU_FAST (docs/binaire-rapide-x86.md) : le binaire RAPIDE du PC, build-fast/ à
# côté de build/ dans le MÊME arbre (build/ reste le binaire de référence, et la
# référence des A/B). Même série de patches, posée ci-dessus une seule fois :
#   QEMU_FAST=1     build/ comme d'habitude, PUIS build-fast/ (native,nohard,lto
#                   + pgo si le profil existe) — le script se relance lui-même
#   QEMU_FAST=only  build-fast/ seulement
#   QEMU_FAST_PGO=  auto (défaut : pgo si PGO_DIR a des .gcda pour build-fast,
#                   sinon sans PGO avec un avertissement), use (profil exigé),
#                   gen (instrumenté : tools/tcg/pgo-train.sh), non (sans PGO)
#   PGO_DIR         profil de build-fast, défaut <arbre>/pgo-fast (stable :
#                   tools/tcg/pgo-train.sh l'écrit, l'archive et sait importer
#                   celui d'un arbre voisin en renommant les .gcda)
# Une seule édition de liens LTO à la fois sur la machine : refus si un lto1
# tourne déjà (QEMU_FAST_FORCE=1 pour passer outre). Construire sous plafond
# mémoire (systemd-run --user --scope -p MemoryMax=20G -p MemorySwapMax=1G
# nice -n 10 …) : c'est ce que fait tools/tcg/pgo-train.sh.
# Empreinte de la série (pomppc_serie_hash) : un profil pris sur une autre série
# sert encore aux fichiers inchangés, mais il est temps de le refaire.
source "$ROOT/scripts/qemu_fast.sh"
FAST_AFTER=""
case "${QEMU_FAST:-0}" in
  0) ;;
  1|only)
    [ -z "${QEMU_OPT:-}" ] || { echo "⚠ QEMU_FAST et QEMU_OPT s'excluent" >&2; exit 1; }
    [ "$(uname -s):$(uname -m)" = Linux:x86_64 ] || {
      echo "⚠ QEMU_FAST : binaire rapide du PC Linux x86-64 seulement" >&2; exit 1; }
    if [ "$QEMU_FAST" = 1 ]; then
      FAST_AFTER=1                  # build/ d'abord, build-fast/ en fin de script
    else
      PGO_DIR="${PGO_DIR:-$SRC/pgo-fast}"
      # Les .gcda portent le chemin de l'objet, '/' → '#' : seuls ceux de
      # <arbre>/build-fast servent. Un profil d'un autre arbre, non renommé,
      # serait ignoré EN SILENCE (-Wno-missing-profile) : refus.
      _mangled="$(printf '%s' "$SRC/build-fast" | tr / '#')#"
      _ngcda="$(find "$PGO_DIR" -maxdepth 1 -name "${_mangled}*.gcda" 2>/dev/null | wc -l)"
      _nall="$(find "$PGO_DIR" -maxdepth 1 -name '*.gcda' 2>/dev/null | wc -l)"
      case "${QEMU_FAST_PGO:-auto}" in
        gen) _pgo=pgo-gen ;;
        non) _pgo="" ;;
        auto|use)
          if [ "$_nall" -gt 0 ] && [ "$_ngcda" -eq 0 ]; then
            echo "⚠ $PGO_DIR a $_nall .gcda, aucun pour $SRC/build-fast : profil d'un autre" >&2
            echo "  arbre ou d'un autre dossier de build. tools/tcg/pgo-train.sh --importer" >&2
            echo "  les renomme ; QEMU_FAST_PGO=non construit sans profil." >&2
            exit 1
          fi
          if [ "$_ngcda" -gt 0 ]; then
            _pgo=pgo
            echo "▶ binaire rapide : profil PGO $PGO_DIR ($_ngcda .gcda)"
            _serie="$(sed -n 's/^serie=//p' "$PGO_DIR/pomppc-profil.txt" 2>/dev/null)"
            if [ -n "$_serie" ] && [ "$_serie" != "$(pomppc_serie_hash "$ROOT/patches")" ]; then
              echo "  ⚠ profil pris sur une autre série de patches ($_serie) : les fichiers" >&2
              echo "    changés seront compilés sans profil ; réentraîner (tools/tcg/pgo-train.sh)" >&2
            fi
          elif [ "${QEMU_FAST_PGO:-auto}" = use ]; then
            echo "⚠ QEMU_FAST_PGO=use : aucun profil dans $PGO_DIR" >&2; exit 1
          else
            _pgo=""
            echo "⚠ binaire rapide SANS PGO : pas de profil dans $PGO_DIR" >&2
            echo "  (tools/tcg/pgo-train.sh l'entraîne ; docs/binaire-rapide-x86.md)" >&2
          fi ;;
        *) echo "⚠ QEMU_FAST_PGO attendu : auto, use, gen ou non" >&2; exit 1 ;;
      esac
      QEMU_OPT="native,nohard,lto${_pgo:+,$_pgo}"
      QEMU_BUILD=build-fast
    fi ;;
  *) echo "⚠ QEMU_FAST attendu : 1 ou only" >&2; exit 1 ;;
esac
BDIR=build
OPT_CFLAGS="" OPT_LDFLAGS="" OPT_CONF=()
if [ -n "${QEMU_OPT:-}" ]; then
  [ "$SRC" = "$HOME/src/qemu" ] && [ -z "${QEMU_BUILD:-}" ] && {
    echo "⚠ QEMU_OPT vise un arbre séparé (QEMU_SRC=~/src/qemu-opt) ou build-fast/" >&2
    echo "  (QEMU_FAST=1), jamais build/, le binaire de référence" >&2; exit 1; }
  [ "${QEMU_BUILD:-}" = build ] && {
    echo "⚠ QEMU_BUILD=build : une variante n'écrase jamais le binaire de référence" >&2; exit 1; }
  case ",$QEMU_OPT," in
    *,lto,*)
      if [ -z "${QEMU_FAST_FORCE:-}" ] && pgrep -x 'lto1|lto-wrapper' >/dev/null 2>&1; then
        echo "⚠ une édition de liens LTO tourne déjà sur la machine (pgrep lto1) :" >&2
        echo "  une seule à la fois (gel du PC le 03/10). QEMU_FAST_FORCE=1 pour passer outre." >&2
        exit 1
      fi ;;
  esac
  PGO_DIR="${PGO_DIR:-$SRC/pgo-data}"
  tag=""
  IFS=, read -r -a _opts <<< "$QEMU_OPT"
  for o in "${_opts[@]}"; do
    case "$o" in
      native)  OPT_CFLAGS="$OPT_CFLAGS -march=native"; OPT_CONF+=(-Doptimization=3) ;;
      nohard)  OPT_CFLAGS="$OPT_CFLAGS -fzero-call-used-regs=skip -ftrivial-auto-var-init=uninitialized"
               OPT_CONF+=(--disable-stack-protector) ;;
      # Une seule édition de liens à la fois, LTO_JOBS processus ltrans (6 par
      # défaut) : sinon les deux qemu-system-* se lient ensemble avec nproc ltrans
      # chacun, et sous pgo la mémoire déborde (PC 40 Go gelé au swap, 03/10/2026).
      lto)     OPT_CONF+=(--enable-lto -Dbackend_max_links=1 -Db_lto_threads="${LTO_JOBS:-6}") ;;
      pgo-gen) OPT_CFLAGS="$OPT_CFLAGS -fprofile-generate=$PGO_DIR -fprofile-update=prefer-atomic"
               OPT_LDFLAGS="$OPT_LDFLAGS -fprofile-generate=$PGO_DIR" ;;
      # -Wno-error=coverage-mismatch : un profil pris sur un arbre voisin (PGO_DIR
      # renommé) sert aux fichiers inchangés ; ceux qu'un patch a changés sont
      # compilés sans profil au lieu d'arrêter la construction.
      pgo)     OPT_CFLAGS="$OPT_CFLAGS -fprofile-use=$PGO_DIR -fprofile-partial-training -Wno-missing-profile -Wno-error=coverage-mismatch"
               OPT_LDFLAGS="$OPT_LDFLAGS -fprofile-use=$PGO_DIR" ;;
      *) echo "⚠ QEMU_OPT : variante inconnue « $o » (native, nohard, lto, pgo-gen, pgo)" >&2; exit 1 ;;
    esac
    case "$o" in pgo-gen|pgo) tag="$tag-pgo" ;; *) tag="$tag-$o" ;; esac
  done
  BDIR="${QEMU_BUILD:-build$tag}"
  echo "▶ variante de compilation $QEMU_OPT → $SRC/$BDIR"
  # options de configure changées : reconfigurer
  RECONFIGURE=1
fi
mkdir -p "$BDIR" && cd "$BDIR"
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
               --disable-docs --disable-werror ${CONFIGURE_EXTRA:-} \
               ${OPT_CFLAGS:+--extra-cflags="$OPT_CFLAGS"} ${OPT_LDFLAGS:+--extra-ldflags="$OPT_LDFLAGS"} \
               ${OPT_CONF[@]+"${OPT_CONF[@]}"}
fi
ninja -j"$JOBS"

# --- 6. Vérification de capacités (fait foi, et sert aux lanceurs) ---
BIN="$SRC/$BDIR/qemu-system-ppc"
mkdir -p "$ROOT/bench"
CAPS="$ROOT/bench/build-capabilities.txt"
# une variante n'écrase pas le relevé du binaire de référence
[ "$BDIR" != build ] && CAPS="$ROOT/bench/build-capabilities-$BDIR.txt"
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
# Capacité OPTIONNELLE : consignée comme les autres, mais son absence ne fait
# pas d'un binaire complet un binaire raté. Le backend GL en est une : il
# dépend d'EGL/GL sur la machine QUI COMPILE, et le device retombe proprement
# sur le backend logiciel (backend=auto) là où il manque.
check_opt() { # check_opt <étiquette> <prédicat…>
  if "${@:2}" >/dev/null 2>&1; then
    echo "  ✔ $1"; echo "$1=oui" >> "$CAPS"
  else
    echo "  ~ $1 (optionnel, absent)"; echo "$1=non" >> "$CAPS"
  fi
}
echo
echo "=== capacités du binaire produit ==="
# screamer : sondage MACHINE (le type peut être enregistré sans être câblé).
check screamer   qemu_machine_has  "$BIN" "mac99,via=pmu" screamer
check qfb-pci    qemu_has_device   "$BIN" qfb-pci
check qgpu-pci   qemu_has_device   "$BIN" qgpu-pci
check x-abs-margin qemu_dev_has_prop "$BIN" usb-tablet x-abs-margin
# Pas seulement « le device existe » : a-t-il un écran où présenter ?
check qgpu-scanout qemu_qgpu_has_scanout "$BIN" "mac99,via=pmu"
# Le backend de rendu hôte : le lanceur demande 'auto' (qui retombe toujours
# sur 'soft'), mais la 3D ne vaut que par 'gl'. Sondé sur le binaire produit,
# jamais déduit des en-têtes présentes au configure — et OPTIONNEL : un build
# sur une machine sans EGL reste un binaire de référence valide, il rendra en
# logiciel. C'est la capacité que le lanceur imposait sans l'avoir sondée.
check_opt qgpu-backend-gl qemu_qgpu_has_backend "$BIN" "mac99,via=pmu" gl
check slirp      qemu_has_netdev   "$BIN" user
case "$(uname -s)" in
  Darwin) check audio-coreaudio qemu_has_audiodev "$BIN" coreaudio ;;
  *)      check audio-pa        qemu_has_audiodev "$BIN" pa ;;
esac
# SMP : sondé sur LE BINAIRE, et sur celui que le lanceur emploie réellement
# (qemu-system-ppc64 dès SMP >= 2). Un grep dans hw/misc/macio/gpio.c ne
# disait que « le source est patché » — pas « ce binaire accepte -smp 2 », ce
# qui est la seule question. -smp 2 exige via=pmu : la machine sondée est celle
# du lanceur.
check smp-mac99  qemu_machine_smp_ok "${BIN}64" "mac99,via=pmu" 2
# Flottant rapide : sondé sur LES DEUX binaires. Le lanceur emploie
# qemu-system-ppc64 dès SMP >= 2, et rien ne garantit a priori que la propriété
# soit visible des deux côtés (le type de CPU n'a pas le même nom).
check x-fast-fp        qemu_cpu_has_fastfp "$BIN"        "mac99,via=pmu" g4
check x-fast-fp-ppc64  qemu_cpu_has_fastfp "${BIN}64"    "mac99,via=pmu" g4
# TLB par jeu de segments (patches/tcg/) : optionnel (NO_TCG=1 le retire).
check_opt x-sr-tlb       qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-sr-tlb=on
check_opt x-lfs-inline   qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-lfs-inline=on
check_opt x-vfp-fast     qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-vfp-fast=on
check_opt x-vperm-fast   qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-vperm-fast=on
check_opt x-jit-near     qemu_tcg_has_prop  "${BIN}64"    "mac99,via=pmu" x-jit-near=on
check_opt x-fp-inline    qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-fp-inline=on
check_opt x-ret-inline   qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-ret-inline=on
check_opt x-jc-idx       qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-jc-idx=on
check_opt x-icbi-sync    qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-icbi-sync=on
check_opt x-jc-bits      qemu_tcg_has_prop  "${BIN}64"    "mac99,via=pmu" x-jc-bits=14
check_opt x-msr-nobql    qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-msr-nobql=on
check_opt x-fp-flat      qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-fp-flat=on
check_opt x-fp-native    qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-fp-native=on
check_opt x-fp-native64  qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-fp-native64=on
check_opt x-tb-fast      qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-tb-fast=on
check_opt x-vmx-inline   qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-vmx-inline=on
check_opt x-vfp-native   qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-vfp-native=on
check_opt x-lmw-vector   qemu_cpu_has_prop  "${BIN}64"    "mac99,via=pmu" g4 x-lmw-vector=on
check_opt x-jc-word      qemu_tcg_has_prop  "${BIN}64"    "mac99,via=pmu" x-jc-word=on
echo
echo "→ $CAPS"

# Relevé d'une variante, À CÔTÉ du binaire : scripts/qemu_fast.sh le lit pour
# décider si run_tiger.sh peut prendre build-fast/ (complet, pas instrumenté,
# construit sur CE processeur : -march=native).
if [ "$BDIR" != build ]; then
  {
    echo "# relevé de construction (scripts/build_qemu_qfb.sh), lu par scripts/qemu_fast.sh"
    echo "date=$(date "+%Y-%m-%dT%H:%M:%S%z")"
    echo "variante=${QEMU_OPT:-}"
    case ",${QEMU_OPT:-}," in
      *,pgo,*|*,pgo-gen,*) echo "pgo_dir=$PGO_DIR"
                           echo "gcda=$(find "$PGO_DIR" -maxdepth 1 -name '*.gcda' 2>/dev/null | wc -l)" ;;
    esac
    echo "cpu=$(sed -n 's/^model name[[:space:]]*: *//p' /proc/cpuinfo 2>/dev/null | head -1)"
    echo "march=$(${CC:-cc} -march=native -Q --help=target 2>/dev/null | awk '$1=="-march=" {print $2; exit}')"
    echo "serie=$(pomppc_serie_hash "$ROOT/patches")"
    echo "pomppc=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)"
    echo "complet=$([ "$fail" -eq 0 ] && echo oui || echo non)"
  } > "$SRC/$BDIR/pomppc-build.txt"
  echo "→ $SRC/$BDIR/pomppc-build.txt"
fi

if [ "$fail" -ne 0 ]; then
  echo "⚠ build INCOMPLET : au moins une capacité manque (voir ci-dessus)." >&2
  echo "  Les lanceurs sonderont le binaire et dégraderont proprement," >&2
  echo "  mais ce binaire n'est pas le binaire de référence." >&2
  exit 1
fi
if [ "$BDIR" = build ]; then
  echo "✔ binaire de référence complet : $BIN"
else
  echo "✔ binaire complet ($BDIR, ${QEMU_OPT:-}) : $BIN"
fi
# QEMU_FAST=1 : la référence est faite, au tour de build-fast/ (même arbre, même
# série, déjà posée ; le second passage la revérifie, ce qui ne coûte rien).
if [ -n "$FAST_AFTER" ]; then
  echo
  echo "▶ binaire rapide : build-fast/ (QEMU_FAST=only)"
  exec env QEMU_FAST=only QEMU_SRC="$SRC" QEMU_TAG="$TAG" \
    bash "$ROOT/scripts/build_qemu_qfb.sh"
fi
