#!/usr/bin/env bash
# build_qemu_qfb.sh — reconstruit LE binaire de référence de POMPPC : QEMU 9.2.0 +
#
#   • le device audio « screamer » (AWACS PowerMac)  — patches/screamer/
#   • le device paravirtuel « qfb-pci »              — patches/qfb/
#   • le GPU paravirtuel « qgpu-pci » (+ backends)   — patches/qgpu/
#   • le bring-up SMP mac99 de BALATON Zoltan        — patches/smp-mac99/
#   • le flottant rapide (x-fast-fp)                  — patches/fastfp/
#   • le TLB gardé par jeu de segments (x-sr-tlb)     — patches/tcg/0001
#   • lfs/stfs sans helper (x-lfs-inline)             — patches/tcg/0002
#   • AltiVec : vfp à 4 voies, vperm par table       — patches/tcg/0003, 0004
#   • tampon du JIT près du texte (x-jit-near)        — patches/tcg/0006
#   • flottant scalaire simple sans helper (x-fp-inline) — patches/tcg/0007
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
patch_strict() { # patch_strict <fichier.patch> [args de patch…]
  local p="$1" rc=0; shift
  rej_clean
  patch -p1 "$@" < "$p" || rc=$?
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

# --- 1. SMP mac99 (série BALATON, révisée : async_run_on_cpu, GPIO 3 et 4) ---
# Le garde teste un marqueur PROPRE À CETTE VERSION du patch. L'ancien
# (« CPU1 reset ») ne convient plus : il survit dans un commentaire des deux
# versions, si bien qu'un arbre portant la série précédente l'aurait satisfait,
# l'étape aurait été sautée et la nouvelle série jamais appliquée — en silence.
# C'est la classe de panne que ce script existe pour supprimer.
if ! grep -q "macio_gpio_set_extirq" hw/misc/macio/gpio.c; then
  # Arbre portant l'ANCIENNE série : ses hunks tomberaient sur des fichiers
  # déjà modifiés. Retour à l'amont sur les trois fichiers du patch d'abord.
  if grep -q "CPU1 reset" hw/misc/macio/gpio.c; then
    echo "▶ ancienne série SMP détectée : retour à l'amont de gpio.c," \
         "mac_newworld.c et openpic.c avant de poser la nouvelle"
    git checkout -- hw/misc/macio/gpio.c hw/ppc/mac_newworld.c hw/intc/openpic.c
  fi
  echo "▶ patch SMP mac99"
  # --fuzz=0 : cette série s'applique exactement sur v9.2.0 (aucun décalage).
  # Un fuzz toléré, c'est un hunk posé ailleurs qu'à sa place, et la seule
  # trace serait un bug de timing dans l'invité.
  patch_strict "$ROOT/patches/smp-mac99/qemu-mac99-cpus-v2.patch" --fuzz=0
  rm -f hw/misc/macio/gpio.c.orig hw/ppc/mac_newworld.c.orig hw/intc/openpic.c.orig
fi

# --- 2. Constantes GPIO absentes de 9.2.0 ---
# La série SMP les apporte désormais elle-même (OUT_DATA / IN_DATA / OUT_ENABLE
# en tête de gpio.c) : le garde les trouve et saute l'insertion. Le bloc reste
# pour un arbre patché avec une série plus ancienne, qui les supposait sans les
# définir.
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
  if ! grep -q "tcg_jit_near" tcg/region.c; then
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
echo
echo "→ $CAPS"

if [ "$fail" -ne 0 ]; then
  echo "⚠ build INCOMPLET : au moins une capacité manque (voir ci-dessus)." >&2
  echo "  Les lanceurs sonderont le binaire et dégraderont proprement," >&2
  echo "  mais ce binaire n'est pas le binaire de référence." >&2
  exit 1
fi
echo "✔ binaire de référence complet : $BIN"
