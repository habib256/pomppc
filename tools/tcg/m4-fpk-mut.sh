#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# m4-fpk-mut.sh — mutants de l'émetteur aarch64 RÉEL de tcg/0045-0046, de la
# correction de tcg/0037 (diviseur nul sous PFP_K_B), de tcg/0051 et tcg/0052
# (docs/parite-m4-2510.md §5.5). Pendant M4 de mut.sh / lfsk-mut.py / tlbb-mut.py
# du PC, qui passent par qemu-ppc linux-user (absent sous macOS) : ici chaque
# mutant est construit dans MUT_SRC (une COPIE construite, jamais l'arbre
# d'usage), puis éprouvé par un démarrage de la VM de dev, SMP=2 MTTCG, leviers
# ET vérificateurs allumés, job fpk-suite court ; détecté si une empreinte
# diffère de la référence non mutée ou si un vérificateur diverge. L'arbre est
# rendu intact à la fin.
#
#   DEVDISK=disks/<copie>.raw MUT_SRC=~/src/qemu-arm64-mut tools/tcg/m4-fpk-mut.sh
#
# Variables : OUT (défaut bench/m4-fpk-mut/<date>), NK (programmes du mode k,
# défaut 1024), LLO/LHI (mots du mode l, défaut [0x7f000000, 0x80800000) :
# exposants 254-255, -0 et dénormaux négatifs), N (tours de tlbbtest, 2000).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${DEVDISK:?DEVDISK : copie raw de la VM de dev requise}"
: "${MUT_SRC:?MUT_SRC : copie construite de ~/src/qemu-arm64}"
OUT="${OUT:-$ROOT/bench/m4-fpk-mut/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"
exec > >(tee -a "$OUT/bilan.txt") 2>&1
export DEVDISK SMP=2 PPC32MT=1
DL="$ROOT/tools/guest/devloop.py"
F=tcg/aarch64/tcg-target.c.inc
B="x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-fp-inline=on,x-ret-inline=on,x-jc-idx=on,x-icbi-sync=on,x-msr-nobql=on,x-fp-native=on,x-fp-native64=on,x-tb-fast=on,x-vmx-inline=on,x-vfp-native=on,x-lmw-vector=on"
NAT="x-fp-native-cmp=on,x-vfp-native-cmp=on,x-tlb-precise=on,x-lmw-inline=on,x-dcbz-inline=on"
LEV="x-fp-known=on,x-fprf-lazy=on,x-stfs-known=on,x-lfs-known=on,x-tlb-base=on"
VER="x-fp-verify=on,x-fp-native-cmp-verify=on,x-tlb-base-verify=on"
export CPU_OPTS="$B,$NAT,$LEV,$VER" TCG_OPTS="x-jit-near=on,x-jc-bits=14,x-jc-word=on"
export QEMU_BIN="$MUT_SRC/build/qemu-system-ppc"
active=0
cleanup() { [ "$active" = 1 ] && python3 "$DL" shutdown > /dev/null 2>&1; return 0; }
trap cleanup EXIT

JOB="$OUT/job"
mkdir -p "$JOB"
cp "$ROOT/tools/guest/jobs/fptest/fptest.c" "$ROOT/tools/guest/jobs/tlbbtest/tlbbtest.c" "$JOB/"
cat > "$JOB/job.sh" <<EOF
#!/bin/sh
# suite courte des mutants (m4-fpk-mut.sh)
OUT=\$PWD/out
gcc -O2 -Wall -o fptest fptest.c > \$OUT/build.txt 2>&1 || exit 1
gcc -O2 -Wall -o tlbbtest tlbbtest.c >> \$OUT/build.txt 2>&1 || exit 1
./fptest k ${NK:-1024} > \$OUT/k.txt 2>&1
./fptest c 16384 > \$OUT/c.txt 2>&1
./fptest l ${LLO:-7f000000} ${LHI:-80800000} > \$OUT/l.txt 2>&1
./tlbbtest ${N:-2000} > \$OUT/tlbbtest.txt 2>&1
tail -1 \$OUT/k.txt; tail -1 \$OUT/c.txt; tail -1 \$OUT/l.txt; tail -2 \$OUT/tlbbtest.txt
EOF

# Sonde de l'agent (repris de jit-m4-proof.sh : la frappe se perd parfois au démarrage)
PROBE="$OUT/probe"
mkdir -p "$PROBE"
printf '#!/bin/sh\necho agent-ok\n' > "$PROBE/job.sh"
boxes="$(python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); print(c["IN"], c["OU"])' \
         "$ROOT/bench/devloop/mailbox.json")"
run_one() { # run_one <nom> : un démarrage, le job, l'arrêt ; sorties dans OUT/<nom>
  local k
  active=1
  python3 "$DL" start > /dev/null 2>&1 || { echo "  démarrage raté"; return 1; }
  for k in 1 2 3 4 5; do
    python3 "$DL" run "$PROBE" --timeout 150 > /dev/null 2>&1 && break
    python3 "$DL" type '\n'; sleep 2
    python3 "$DL" type 'mount -uw /\n'; sleep 4
    python3 "$DL" type "sh /pomppc/agent.sh $boxes\n"; sleep 15
  done
  python3 "$DL" run "$JOB" --timeout 3600 > /dev/null 2>&1
  rm -rf "$OUT/$1"; cp -R "$ROOT/bench/devloop/last/out" "$OUT/$1"
  python3 "$DL" shutdown > /dev/null 2>&1
  active=0
  cp "$ROOT/bench/devloop/qemu.log" "$OUT/$1-qemu.log"
}
ndiv() { grep -aEc 'DIVERGENCE|[ (][1-9][0-9]* divergences?|[1-9][0-9]* ecarts|[1-9][0-9]* erreurs' "$1"; }

mutants() {
  cat <<'EOF'
# --- 0045 (x-fp-known) ---
# 1. porte jamais testée (PFP_K_G supposé partout)
s/^    if (sel & PFP_K_G) {/    if (1) {/
# 2. frA des ops arithmétiques jamais testé (PFP_K_A supposé)
s/^        if (sel & PFP_K_A) {/        if (1) {/
# 3. frB des ops arithmétiques jamais testé
s/^        if (use_b && !(sel & PFP_K_B)) {/        if (0) {/
# 4. somme minuscule acceptée (PFP_K_R ignoré)
s/^            if (dbl || (sel & PFP_K_R)) {/            if (dbl) {/
# 5. fcmpu : NaN de frA jamais testé
s/^        if (!(sel & PFP_K_A)) {/        if (0) {/
# --- 0046 (x-fprf-lazy) ---
# 6. fprf_src = r jamais rangé
s/^            tcg_out_st(s, TCG_TYPE_I64, xr, TCG_AREG0, lofs);/            ;/
# 7. fcmpu/fcmpo : fprf_src = r au lieu de PPC_FPRF_NONE
s/^        if (op == PFP_CMPU || op == PFP_CMPO) {/        if (0) {/
# 8. fctiw(z) : fprf_src écrasé (la reprise de 0037 l'évite)
s/^    if (lazy && op != PFP_FCTIW && op != PFP_FCTIWZ) {/    if (lazy) {/
# --- 0037 sous 0045 : la correction du 25/10 retirée ---
# 9. diviseur nul : t pris de pfp_zon() (absent sous PFP_K_B)
s/^            pfp_lsl(s, t, xb, 1);$//
# --- 0051 (x-lfs-known) ---
# 10. exposant 255 (infini, NaN) pris pour normal
s/254 << 10/255 << 10/
# 11. ccmp sur la mauvaise condition (dénormaux pris pour nuls)
s/0x7a400800 | PFP_HS << 12/0x7a400800 | PFP_LO << 12/
# --- 0052 (x-tlb-base) ---
# 12. borne de page : un octet de trop
s/TARGET_PAGE_SIZE - memop_size(mop));/TARGET_PAGE_SIZE - memop_size(mop) + 1);/
# 13. comparaison signée au lieu de non signée
s/B_C, TCG_COND_GTU, 0);/B_C, TCG_COND_GT, 0);/
# 14. index de 64 bits au lieu de 32 (bits hauts de l'adresse invitée)
s/^        h.index_ext = TCG_TYPE_I32;/        h.index_ext = TCG_TYPE_I64;/
EOF
}

echo "# $(date +%Y-%m-%dT%H:%M:%S) m4-fpk-mut.sh — MUT_SRC=$MUT_SRC"
( cd "$MUT_SRC/build" && ninja qemu-system-ppc > /dev/null ) || { echo "⚠ MUT_SRC ne se construit pas"; exit 1; }
run_one ref
[ "$(ndiv "$OUT/ref-qemu.log")" -eq 0 ] || { echo "⚠ la référence diverge déjà"; exit 1; }
cat "$OUT/ref/log.txt" 2>/dev/null | tail -6
n=0; det=0; rc=0
while IFS= read -r m <&3; do
  case "$m" in \#*) echo "$m"; continue ;; "") continue ;; esac
  n=$((n + 1))
  cp "$MUT_SRC/$F" "$OUT/orig"
  sed -i.bak -e "$m" "$MUT_SRC/$F"; rm -f "$MUT_SRC/$F.bak"
  if cmp -s "$MUT_SRC/$F" "$OUT/orig"; then
    echo "mut$n : SANS EFFET ($m)"; rc=1; continue
  fi
  if ! ( cd "$MUT_SRC/build" && ninja qemu-system-ppc > "$OUT/mut$n.build" 2>&1 ); then
    echo "mut$n : construction ratée"; cp "$OUT/orig" "$MUT_SRC/$F"; rc=1; continue
  fi
  run_one "mut$n"
  d=$(ndiv "$OUT/mut$n-qemu.log"); j=""
  for t in k c l tlbbtest; do
    cmp -s "$OUT/ref/$t.txt" "$OUT/mut$n/$t.txt" 2>/dev/null || j="$j $t"
  done
  if [ -n "$j" ] || [ "$d" -gt 0 ]; then
    det=$((det + 1)); echo "mut$n : DÉTECTÉ — sortie différente :${j:- aucune} ; $d ligne(s) de divergence"
  else
    echo "mut$n : NON DÉTECTÉ ($m)"; rc=1
  fi
  cp "$OUT/orig" "$MUT_SRC/$F"
done 3< <(mutants)
( cd "$MUT_SRC/build" && ninja qemu-system-ppc > /dev/null 2>&1 ) && echo "arbre des mutants rendu intact et reconstruit"
echo "$det mutants détectés sur $n"
exit $rc
