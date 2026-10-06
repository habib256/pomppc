#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# m4-serie-0031-0035-proofs.sh ÉTAPE… — la séance du M4 pour la parité arm64 de
# la série 0031-0035 (docs/parite-arm64-0031-0035.md) : pendant de
# tools/tcg/pc-serie-proofs.sh (preuves hôte) et des preuves invitées des
# §32-§34 de docs/tcg-g4.md, sur le modèle de tools/tcg/jit-m4-proof.sh.
# Écrit sur le PC le 06/10/2026 ; `bash -n` et l'étape `host` éprouvés sur le
# PC x86-64 seulement : premier vrai passage sur le M4.
#
# Étapes (dans cet ordre ; `all` = host guest) :
#   prep   copie ~/src/qemu (série 0001-0035 posée par build_qemu_qfb.sh) dans
#          ARM64_SRC (défaut ~/src/qemu-arm64, refusée si elle existe), y pose
#          tcg/0037-0039 (--forward, aucun .rej toléré) et la construit par
#          build_qemu_qfb.sh (QEMU_SRC=ARM64_SRC).
#   host   sans VM : encodages A64 (a64enc-check.py), modèle NEON de 0038 contre
#          la référence x86 du PC (vfncxcheck.sh), modèle et talon de 0033
#          contre softfloat (fpproof.sh + mutants), modèle de 0034/0038 contre
#          les helpers (vfpcmpproof.sh + mutants NEON), x-tlb-precise
#          (tlbpproof.py + mutants), propriétés présentes dans le binaire.
#   guest  VM de dev (DEVDISK, copie raw DÉDIÉE) : trois démarrages par valeur
#          de SMPS — « ref » (options de production du M4), « nat » (+ les
#          cinq propriétés), « ver » (+ leurs vérificateurs) — et la suite
#          fptest c/simple/d, vfptest c/origine, tlbtest, lmwtest, dcbztest,
#          smctest ; nat et ver doivent redonner les empreintes de ref (hors la
#          réservation de dcbztest, §32.4), les vérificateurs 0 DIVERGENCE.
#   mut    (long) mutants de l'émetteur arm64 RÉEL (0037/0038) : MUT_SRC, une
#          COPIE construite de ARM64_SRC (jamais ARM64_SRC lui-même), une
#          mutation à la fois, reconstruite, fptest c / vfptest c courts sous
#          les vérificateurs ; chacun doit être DÉTECTÉ. L'arbre est rendu
#          intact ensuite.
#
# Variables : ARM64_SRC (arbre à éprouver, défaut ~/src/qemu-arm64), OUT
# (défaut bench/m4-serie/<date>), DEVDISK (étapes guest et mut), SMPS (défaut
# « 2 1 » : ppc64 SMP=2 puis ppc SMP=1), FPN (vecteurs de fpproof, défaut
# 2000000 ; le PC a fait 20000000), VFN (vfpcmpproof, défaut 200000),
# MUT_SRC, MUT_N (défaut 16384).
# Aucune mesure de vitesse ici : l'A/B se fait ensuite par matab.sh (doc §6).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ARM64_SRC="${ARM64_SRC:-$HOME/src/qemu-arm64}"
OUT="${OUT:-$ROOT/bench/m4-serie/$(date +%Y%m%d-%H%M%S)}"
SMPS="${SMPS:-2 1}"
FPN="${FPN:-2000000}"
VFN="${VFN:-200000}"
MUT_N="${MUT_N:-16384}"
rc=0

step() { # step <nom> <commande…> : journal <OUT>/<nom>.log, verdict sur le code
  local name="$1"; shift
  echo "=== $name"
  if "$@" > "$OUT/$name.log" 2>&1; then
    echo "    OK"; else echo "    ÉCHEC (voir $OUT/$name.log)"; rc=1; fi
  grep -v '^real\|^user\|^sys\|^$' "$OUT/$name.log" | tail -4 | sed 's/^/    /'
}

sha() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

bin32() { echo "$ARM64_SRC/build/qemu-system-ppc"; }
bin64() { echo "$ARM64_SRC/build/qemu-system-ppc64"; }

# ---------------------------------------------------------------- prep
do_prep() {
  local p
  [ -e "$ARM64_SRC" ] && { echo "⚠ $ARM64_SRC existe déjà : prep refusé (le retirer ou ARM64_SRC=…)"; return 1; }
  grep -q 'x-dcbz-inline' "$HOME/src/qemu/target/ppc/cpu_init.c" || {
    echo "⚠ ~/src/qemu n'a pas la série jusqu'à 0035 : lancer d'abord ./scripts/build_qemu_qfb.sh"; return 1; }
  echo "▶ copie ~/src/qemu → $ARM64_SRC (sans build*)"
  rsync -a --exclude='build*' "$HOME/src/qemu/" "$ARM64_SRC/" || return 1
  for p in "$ROOT"/patches/tcg/0037-*.patch "$ROOT"/patches/tcg/0038-*.patch \
           "$ROOT"/patches/tcg/0039-*.patch; do
    echo "▶ $(basename "$p")"
    ( cd "$ARM64_SRC" && find . -name '*.rej' -delete &&
      patch -p1 --forward --fuzz=0 < "$p" ) || { echo "⚠ REJETÉ : $p"; return 1; }
    [ -z "$(cd "$ARM64_SRC" && find . -name '*.rej' -print -quit)" ] || {
      echo "⚠ .rej après $p"; return 1; }
  done
  echo "▶ construction (build_qemu_qfb.sh, QEMU_SRC=$ARM64_SRC)"
  QEMU_SRC="$ARM64_SRC" bash "$ROOT/scripts/build_qemu_qfb.sh" > "$OUT/prep-build.log" 2>&1 || {
    echo "⚠ construction ratée : $OUT/prep-build.log"
    grep -n -E 'error|erreur' "$OUT/prep-build.log" | head -20; return 1; }
  grep -n -E 'warning:' "$OUT/prep-build.log" | grep -E 'tcg-target|int_helper|translate\.c|cpu_init' | head -20
  sha "$(bin32)" "$(bin64)"
}

# ---------------------------------------------------------------- host
probe_props() {
  local b="$1" props="x-tlb-precise=on,x-tlb-precise-verify=256,x-mem-stats=on,x-lmw-inline=on,x-lmw-inline-verify=on,x-lmw-inline-max=8,x-dcbz-inline=on,x-dcbz-inline-verify=on,x-fast-fp=on,x-fp-native-cmp=on,x-fp-native-cmp-verify=on,x-vfp-fast=on,x-vfp-native=on,x-vfp-native-cmp=on,x-vfp-native-cmp-verify=on"
  # QEMU arrêté (-S), sans disque ni affichage : seulement « la propriété existe »
  echo quit | "$b" -M mac99 -cpu "g4,$props" -S -display none -nodefaults \
      -monitor stdio 2>&1 | grep -E "not found|Property|error" && return 1
  echo "propriétés présentes : $props"
}

do_host() {
  [ -x "$(bin64)" ] || { echo "⚠ $(bin64) absent : étape prep d'abord"; return 1; }
  step a64enc        python3 "$HERE/a64enc-check.py" "$ARM64_SRC"
  step vfncxcheck    bash "$HERE/vfncxcheck.sh" "$ARM64_SRC" 20000 "$HERE/vfncxcheck-ref.txt"
  step fpproof       bash "$HERE/fpproof.sh" "$ARM64_SRC" "$FPN"
  step fpproof-mut   bash "$HERE/fpproof-mut.sh" "$ARM64_SRC" 200000
  step vfpcmpproof   bash "$HERE/vfpcmpproof.sh" "$ARM64_SRC" "$VFN"
  step vfpcmpproof-mut bash "$HERE/vfpcmpproof-mut.sh" "$ARM64_SRC" 2000
  step tlbpproof     python3 "$HERE/tlbpproof.py" "$ARM64_SRC" 200000 mut
  step props64       probe_props "$(bin64)"
  step props32       probe_props "$(bin32)"
  # tlbpproof sort en 0 même si un mutant passe : le relire
  grep -q 'mutants détectés : 5 / 5' "$OUT/tlbpproof.log" || {
    echo "    ⚠ tlbpproof : mutants pas tous détectés"; rc=1; }
}

# ---------------------------------------------------------------- guest
DL="$ROOT/tools/guest/devloop.py"
# Options de production du M4 (run_tiger.sh, Darwin:arm64, 06/10) : x-fast-fp et
# compagnie, x-vfp-native, x-lmw-vector ; accélérateur : x-jit-near, x-jc-bits=14,
# x-jc-word. Rien de 0031-0039 n'est allumé par défaut sur le M4.
BASE="x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-fp-inline=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on,x-fp-native=on,x-fp-native64=on,x-tb-fast=on,x-vmx-inline=on,x-vfp-native=on,x-lmw-vector=on"
TCGB="x-jit-near=on,x-jc-bits=14,x-jc-word=on"
NAT="x-fp-native-cmp=on,x-vfp-native-cmp=on,x-tlb-precise=on,x-lmw-inline=on,x-dcbz-inline=on"
VER="$NAT,x-fp-native-cmp-verify=on,x-vfp-native-cmp-verify=on,x-vfp-native-verify=on,x-tlb-precise-verify=256,x-lmw-inline-verify=on,x-dcbz-inline-verify=on"
active=0
cleanup() { [ "$active" = 1 ] && python3 "$DL" shutdown > "$OUT/shutdown.log" 2>&1; return 0; }
trap cleanup EXIT

ensure_agent() { # repris de jit-m4-proof.sh (frappe perdue au démarrage)
  local boxes k probe="$OUT/probe-job"
  mkdir -p "$probe"; printf '#!/bin/sh\necho agent-ok\n' > "$probe/job.sh"
  boxes="$(python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); print(c["IN"], c["OU"])' \
           "$ROOT/bench/devloop/mailbox.json")"
  for k in 1 2 3 4 5; do
    python3 "$DL" run "$probe" --timeout 150 > /dev/null 2>&1 && return 0
    echo "agent muet (essai $k) : frappe relancée"
    python3 "$DL" type '\n'; sleep 2
    python3 "$DL" type 'mount -uw /\n'; sleep 4
    python3 "$DL" type "sh /pomppc/agent.sh $boxes\n"; sleep 15
  done
  echo "agent toujours muet" >&2; return 1
}

guest_job() { # guest_job <dossier> <MUT=0|1> : le job de la suite (MUT=1 : court)
  local j="$1" short="$2" n
  mkdir -p "$j"
  for n in fptest vfptest tlbtest lmwtest dcbztest smctest; do
    cp "$ROOT/tools/guest/jobs/$n/$n.c" "$j/"
  done
  if [ "$short" = 1 ]; then
    cat > "$j/job.sh" <<EOF
#!/bin/sh
# suite courte des mutants (m4-serie-0031-0035-proofs.sh)
OUT=\$PWD/out
gcc -O2 -Wall -o fptest fptest.c > \$OUT/build.txt 2>&1 || exit 1
gcc -O2 -faltivec -Wall -o vfptest vfptest.c >> \$OUT/build.txt 2>&1 || exit 1
./fptest c $MUT_N > \$OUT/fptest-c.txt 2>&1
NVEC=$MUT_N ./vfptest c > \$OUT/vfptest-c.txt 2>&1
tail -1 \$OUT/fptest-c.txt; tail -1 \$OUT/vfptest-c.txt
EOF
  else
    cat > "$j/job.sh" <<'EOF'
#!/bin/sh
# suite de preuve de 0031-0039 (m4-serie-0031-0035-proofs.sh) ; mêmes options
# de compilation que les job.sh de chaque banc
OUT=$PWD/out
gcc -O2 -Wall -o fptest fptest.c > $OUT/build.txt 2>&1 || exit 1
gcc -O2 -faltivec -Wall -o vfptest vfptest.c >> $OUT/build.txt 2>&1 || exit 1
gcc -O2 -Wall -o tlbtest tlbtest.c >> $OUT/build.txt 2>&1 || exit 1
gcc -O1 -mdynamic-no-pic -Wall -o lmwtest lmwtest.c >> $OUT/build.txt 2>&1 || exit 1
gcc -O2 -mdynamic-no-pic -Wall -o dcbztest dcbztest.c >> $OUT/build.txt 2>&1 || exit 1
gcc -O2 -pthread -Wall -o smctest smctest.c >> $OUT/build.txt 2>&1 || exit 1
./fptest c > $OUT/fptest-c.txt 2>&1
./fptest > $OUT/fptest-s.txt 2>&1
./fptest d 65536 > $OUT/fptest-d.txt 2>&1
./vfptest c > $OUT/vfptest-c.txt 2>&1
./vfptest > $OUT/vfptest-o.txt 2>&1
./tlbtest 1 > $OUT/tlbtest.txt 2>&1
./lmwtest > $OUT/lmwtest.txt 2>&1
./dcbztest > $OUT/dcbztest.txt 2>&1
./smctest > $OUT/smctest.txt 2>&1
for f in $OUT/*test*.txt; do echo "$(basename $f) : $(grep -i 'empreinte' $f | tail -1)"; done
EOF
  fi
}

boot() { # boot <SMP> <options CPU en plus> <binaire de base>
  export SMP="$1" CPU_OPTS="$BASE${2:+,$2}" TCG_OPTS="$TCGB" QEMU_BIN="$3"
  active=1
  python3 "$DL" start || return 1
  ensure_agent
}

finish() {
  python3 "$DL" shutdown > /dev/null 2>&1
  active=0
  cp "$ROOT/bench/devloop/qemu.log" "$OUT/$1-qemu.log"
}

# empreintes de référence relevées sur le PC (docs/tcg-g4.md §25.4, §26, §32.5,
# §33.4, §34.4) : le résultat invité ne dépend pas de l'hôte (helpers =
# softfloat), un écart avec le PC est donc à expliquer — mais le critère est
# ref = nat = ver sur le M4 même.
pc_ref() {
  case "$1" in
    fptest-c) echo 67cf96efa75f9286 ;; fptest-s) echo e80ec8026301ef1d ;;
    fptest-d) echo fb3e6006e03c4f53 ;; vfptest-c) echo 65be3233b17bc60b ;;
    vfptest-o) echo bc13e93c39e62602 ;; tlbtest) echo deee3d7026183c65 ;;
    lmwtest) echo 2575eafce78adf66 ;; dcbztest) echo e2f15e6f3ceae607 ;;
    *) echo "-" ;;
  esac
}

fingerprint() { # fingerprint <fichier> <banc> : la ligne d'empreinte qui compte
  case "$2" in
    dcbztest) grep 'empreinte hors reservation' "$1" | tail -1 | awk '{print $NF}' ;;
    smctest) grep -c 'erreurs 0' "$1" ;;
    fptest-*) grep '^total' "$1" | tail -1 | awk '{print $NF}' ;;
    *) grep -i 'empreinte' "$1" | tail -1 | awk '{print $NF}' ;;
  esac
}

do_guest() {
  local smp b arm f t ref nat ver pcr job
  : "${DEVDISK:?DEVDISK : copie raw DÉDIÉE de la VM de dev requise}"
  export DEVDISK
  if [ -f "$ROOT/bench/devloop/qemu.pid" ] && kill -0 "$(cat "$ROOT/bench/devloop/qemu.pid")" 2>/dev/null; then
    echo "⚠ une VM devloop tourne déjà dans ce dépôt"; return 1
  fi
  job="$OUT/suite-job"; guest_job "$job" 0
  for smp in $SMPS; do
    b="$(bin32)"   # devloop prend le voisin « 64 » quand SMP > 1
    for arm in ref nat ver; do
      echo "=== invité SMP=$smp, $arm"
      case "$arm" in ref) o="" ;; nat) o="$NAT" ;; ver) o="$VER" ;; esac
      boot "$smp" "$o" "$b" || { echo "    démarrage raté"; rc=1; finish "smp$smp-$arm"; continue; }
      python3 "$DL" run "$job" --timeout "${GUEST_TIMEOUT:-3600}" > "$OUT/smp$smp-$arm-run.log" 2>&1 || {
        echo "    job en erreur (voir smp$smp-$arm-run.log)"; rc=1; }
      rm -rf "$OUT/smp$smp-$arm"; cp -R "$ROOT/bench/devloop/last/out" "$OUT/smp$smp-$arm"
      finish "smp$smp-$arm"
    done
    printf '    %-10s %-18s %-18s %-18s %s\n' banc ref nat ver "PC (indicatif)"
    for t in fptest-c fptest-s fptest-d vfptest-c vfptest-o tlbtest lmwtest dcbztest smctest; do
      ref="$(fingerprint "$OUT/smp$smp-ref/$t.txt" "$t" 2>/dev/null)"
      nat="$(fingerprint "$OUT/smp$smp-nat/$t.txt" "$t" 2>/dev/null)"
      ver="$(fingerprint "$OUT/smp$smp-ver/$t.txt" "$t" 2>/dev/null)"
      pcr="$(pc_ref "$t")"
      printf '    %-10s %-18s %-18s %-18s %s' "$t" "${ref:-?}" "${nat:-?}" "${ver:-?}" "$pcr"
      if [ -z "$ref" ] || [ "$ref" != "$nat" ] || [ "$ref" != "$ver" ]; then
        echo "   ⚠ DIFFÈRE"; rc=1
      elif [ "$pcr" != "-" ] && [ "$pcr" != "$ref" ]; then
        echo "   (≠ PC : à expliquer, paramètres ?)"
      else echo; fi
    done
    for t in tlbtest lmwtest dcbztest smctest; do   # fautes, erreurs : même texte
      for arm in nat ver; do
        diff <(grep -v -i -E 'banc|ms$' "$OUT/smp$smp-ref/$t.txt") \
             <(grep -v -i -E 'banc|ms$' "$OUT/smp$smp-$arm/$t.txt") \
             > "$OUT/smp$smp-$arm-$t.diff" 2>&1 || echo "    $t ($arm) : texte différent, voir smp$smp-$arm-$t.diff (dcbztest : la réservation, §32.4)"
      done
    done
    f="$OUT/smp$smp-ver-qemu.log"
    echo "    vérificateurs (smp$smp-ver-qemu.log) :"
    grep -E 'fp-native-verify: [0-9]|vfp-native-cmp-verify:|vfp-native-verify:|lmw-inline-verify: cpu|dcbz-inline-verify: cpu|x-tlb-precise-verify.*divergent|ret-verify' "$f" | tail -12 | sed 's/^/      /'
    if grep -q 'DIVERGENCE' "$f"; then
      echo "    ⚠ DIVERGENCE dans $f :"; grep 'DIVERGENCE' "$f" | head -5 | sed 's/^/      /'; rc=1
    fi
    for arm in nat ver; do
      if grep -q -i -E 'not found|is not a valid' "$OUT/smp$smp-$arm-qemu.log" 2>/dev/null; then
        echo "    ⚠ propriété refusée au démarrage (smp$smp-$arm-qemu.log)"; rc=1
      fi
    done
  done
}

# ---------------------------------------------------------------- mut
# Mutations de l'émetteur arm64 RÉEL (0037/0038) et de la traduction (fsel, CR6),
# pendants de fpnatcmp-mut.sh et vfpcmp-mut.sh (x86_64). Une ligne : fichier|sed.
mutants_a64() {
  cat <<'MUT'
# --- 0037 (x-fp-native-cmp) ---
# 1. frsp : borne basse retirée (|frB| < FLT_MIN accepté)
tcg/aarch64/tcg-target.c.inc|s/tcg_out_movi(s, TCG_TYPE_I64, k1, 0x381ull << 53);/tcg_out_movi(s, TCG_TYPE_I64, k1, 0x001ull << 53);/
# 2. frsp : borne haute à 2^128 (OX manqué)
tcg/aarch64/tcg-target.c.inc|s/(0x47effffff0000000ull << 1) - (0x381ull << 53)/(0x47f0000000000000ull << 1) - (0x381ull << 53)/
# 3. fctiw tronque au lieu d'arrondir
tcg/aarch64/tcg-target.c.inc|s/op == PFP_FCTIWZ ? PFP_FCVTZS_XD : PFP_FCVTNS_XD/PFP_FCVTZS_XD/
# 4. fctiw(z) : pas de test de plage int32 (VXCVI manqué)
tcg/aarch64/tcg-target.c.inc|s/        pfp_cmp(s, t, xr);/        pfp_cmp(s, t, t);/
# 5. fctiw(z) : FI non posé
tcg/aarch64/tcg-target.c.inc|s/tcg_out_logicali(s, Ilogic_imm_ORRI, TCG_TYPE_I64, xnf, xf, PFP_FI);/tcg_out_mov(s, TCG_TYPE_I64, xnf, xf);/
# 6. fdiv(s) : diviseur nul accepté
tcg/aarch64/tcg-target.c.inc|/a zero divisor (t = frB << 1)/{n;n;d;}
# 7. fdiv(s) : quotient jamais testé (tc = XZR lu comme facteur nul)
tcg/aarch64/tcg-target.c.inc|s/            if (base != PFP_B_DIV) {/            if (true) {/
# 8. fcmpo/fcmpu : NaN de frB accepté
tcg/aarch64/tcg-target.c.inc|s/        pfp_cmp(s, tb, k3);/        pfp_cmp(s, tb, tb);/
# 9. fsel : NaN non testé (traduction, ops TCG)
target/ppc/translate/fp-impl.c.inc|s/tcg_constant_i64(0x7ff0000000000000ull), t1, r);/tcg_constant_i64(0xfff0000000000000ull), t1, r);/
# --- 0038 (x-vfp-native-cmp) ---
# 10. comparaisons : dénormaux acceptés
tcg/aarch64/tcg-target.c.inc|/^static void pvf_good/,/^}/s/pvf_3(s, PVF_CMHS4S, d, t, k1);/pvf_dup(s, d, 0xffffffff);/
# 11. comparaisons : NaN accepté
tcg/aarch64/tcg-target.c.inc|/^static void pvf_good/,/^}/s/pvf_3(s, PVF_CMHS4S, u, k4, t);/pvf_dup(s, u, 0xffffffff);/
# 12. vcmpgefp strict
tcg/aarch64/tcg-target.c.inc|s|pvf_3(s, PVF_FCMGE4S, r, xa, xb);           /\* a >= b \*/|pvf_3(s, PVF_FCMGT4S, r, xa, xb);|
# 13. vcmpbfp contre b au lieu de -b
tcg/aarch64/tcg-target.c.inc|s/            pvf_2(s, PVF_FNEG4S, t, xb);/            tcg_out_mov(s, TCG_TYPE_V128, t, xb);/
# 14. vcfux par scvtf
tcg/aarch64/tcg-target.c.inc|s/kind == PVF_CFSX ? PVF_SCVTF4S : PVF_UCVTF4S/PVF_SCVTF4S/
# 15. vcf* : porte retirée (seul le vérificateur le voit : inexact de vec_status)
tcg/aarch64/tcg-target.c.inc|/^static void tcg_out_ppc_vfp_cmp/,/^}/s/            if (args\[7 + 3 \* w\]) {/            if (0) {/
# 16. vctsxs : SAT haut jamais vu
tcg/aarch64/tcg-target.c.inc|s|pvf_3(s, PVF_FCMGE4S, hi, x, c);            /\* x >= 2^31 \*/|pvf_dup(s, hi, 0);|
# 17. VSCR[SAT] jamais posé
tcg/aarch64/tcg-target.c.inc|s/tcg_out_logicali(s, Ilogic_imm_ANDI, TCG_TYPE_I32, PVF_T1, PVF_T1, 1);/tcg_out_movi(s, TCG_TYPE_I32, PVF_T1, 0);/
# 18. vctuxs : saturation basse dès -0,5
tcg/aarch64/tcg-target.c.inc|s|pvf_dup(s, k1, 0xbf800000);                 /\* -1.0 \*/|pvf_dup(s, k1, 0xbf000000);|
# 19. vctsxs par fcvtzu
tcg/aarch64/tcg-target.c.inc|s/pvf_2(s, PVF_FCVTZS4S, r, x);/pvf_2(s, PVF_FCVTZU4S, r, x);/
# 20. talon de vcf* : uim oublié (helper_vcf*(env, &vD, &vB, 0))
tcg/aarch64/tcg-target.c.inc|s/tcg_out_movi(s, TCG_TYPE_I32, TCG_REG_X3, l->vfp_off\[3\]);/tcg_out_movi(s, TCG_TYPE_I32, TCG_REG_X3, 0);/
# 21. CR6 : « toutes les voies » faux (traduction)
target/ppc/translate/vmx-impl.c.inc|s/tcg_gen_setcondi_i64(TCG_COND_EQ, u, u, -1);/tcg_gen_setcondi_i64(TCG_COND_NE, u, u, -1);/
MUT
}
[ -n "${MUT_LIST:-}" ] && { mutants_a64; exit 0; }   # pour la relecture des motifs

do_mut() {
  local f m n=0 det=0 job ndiv b32 j
  : "${DEVDISK:?DEVDISK requis}"; : "${MUT_SRC:?MUT_SRC : copie CONSTRUITE, jamais ARM64_SRC}"
  export DEVDISK
  [ "$(cd "$MUT_SRC" && pwd)" != "$(cd "$ARM64_SRC" && pwd)" ] || { echo "⚠ MUT_SRC = ARM64_SRC : refusé"; return 1; }
  [ -x "$MUT_SRC/build/qemu-system-ppc64" ] || { echo "⚠ $MUT_SRC non construit"; return 1; }
  b32="$MUT_SRC/build/qemu-system-ppc"
  job="$OUT/mut-job"; guest_job "$job" 1
  export SMP=2
  # référence de l'arbre non muté, propriétés et vérificateurs allumés
  ( cd "$MUT_SRC/build" && ninja qemu-system-ppc qemu-system-ppc64 > /dev/null ) || return 1
  boot 2 "$VER" "$b32" || return 1
  python3 "$DL" run "$job" --timeout 1800 > /dev/null 2>&1
  rm -rf "$OUT/mut-ref"; cp -R "$ROOT/bench/devloop/last/out" "$OUT/mut-ref"; finish mut-ref
  grep -q DIVERGENCE "$OUT/mut-ref-qemu.log" && { echo "⚠ la référence des mutants diverge déjà"; return 1; }
  # fd 3 : devloop.py ne doit pas lire la liste sur l'entrée standard
  while IFS='|' read -r f m <&3; do
    [ -z "$f" ] && continue
    case "$f" in \#*) echo "$f"; continue ;; esac
    n=$((n + 1))
    cp "$MUT_SRC/$f" "$OUT/orig"
    sed -i.bak -e "$m" "$MUT_SRC/$f"; rm -f "$MUT_SRC/$f.bak"
    if cmp -s "$MUT_SRC/$f" "$OUT/orig"; then
      echo "mut$n : SANS EFFET ($m)"; cp "$OUT/orig" "$MUT_SRC/$f"; continue
    fi
    if ! ( cd "$MUT_SRC/build" && ninja qemu-system-ppc qemu-system-ppc64 > "$OUT/mut$n.build" 2>&1 ); then
      echo "mut$n : construction ratée"; cp "$OUT/orig" "$MUT_SRC/$f"; continue
    fi
    if boot 2 "$VER" "$b32"; then
      python3 "$DL" run "$job" --timeout 1800 > /dev/null 2>&1
      rm -rf "$OUT/mut$n"; cp -R "$ROOT/bench/devloop/last/out" "$OUT/mut$n"
    fi
    finish "mut$n"
    ndiv=$(grep -c DIVERGENCE "$OUT/mut$n-qemu.log")
    j=""
    for t in fptest-c vfptest-c; do
      cmp -s "$OUT/mut-ref/$t.txt" "$OUT/mut$n/$t.txt" || j="$j $t"
    done
    if [ -n "$j" ] || [ "$ndiv" -gt 0 ]; then
      det=$((det + 1)); echo "mut$n : DÉTECTÉ — sortie différente :${j:- aucune} ; $ndiv DIVERGENCE"
    else
      echo "mut$n : NON DÉTECTÉ ($m)"; rc=1
    fi
    cp "$OUT/orig" "$MUT_SRC/$f"
  done 3< <(mutants_a64)
  ( cd "$MUT_SRC/build" && ninja qemu-system-ppc qemu-system-ppc64 > /dev/null 2>&1 ) &&
    echo "arbre des mutants rendu intact et reconstruit"
  echo "$det mutants détectés sur $n"
}

[ $# -gt 0 ] || { sed -n '4,40p' "$0"; exit 2; }
mkdir -p "$OUT"
exec > >(tee -a "$OUT/bilan.txt") 2>&1
echo "# $(date +%Y-%m-%dT%H:%M:%S) $(uname -s) $(uname -m) — étapes : $*"
[ -x "$(bin64)" ] && sha "$(bin32)" "$(bin64)"
for st in "$@"; do
  case "$st" in
    prep)  do_prep || rc=1 ;;
    host)  do_host ;;
    guest) do_guest || rc=1 ;;
    mut)   do_mut || rc=1 ;;
    all)   do_host; do_guest || rc=1 ;;
    *) echo "étape inconnue : $st"; rc=1 ;;
  esac
done
echo "# fin, code $rc — $OUT"
exit $rc
