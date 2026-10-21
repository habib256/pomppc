#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# t3proof.sh QEMU_BIN — preuves des régions compilées tcg/0049 (x-tier3,
# x-tier2-verify) sur le banc nu tools/tcg/t3proof.S (mac99 sans OpenBIOS,
# mode réel, 1 vCPU) : boucles imbriquées, appel et retour d'une autre page,
# exception précise dans une boucle, code modifié, accès hors journal
# (docs/plan-traducteur-rapide.md §10).
#
#   tools/tcg/t3proof.sh ~/src/qemu-t3/build/qemu-system-ppc
#
# Passes : ref (x-tier2=0), t3 (x-tier2=64,x-tier3=1000), verif (… ,
# x-tier2-verify=1), mutant1 (POMPPC_T3_MUTANT=1 : les branchements du
# premier bloc d'une région sur deux inversés), mutant2 (POMPPC_T3_MUTANT=2 :
# une région sur deux oublie de ranger un registre invité modifié à ses
# sorties, le défaut d'un flot de données faux), mutant3 (POMPPC_T2_MUTANT=3 :
# le garde des accès hors journal coupé). Verdict : même somme et même nombre
# de pièges en ref/t3/verif, régions formées et mortes (code modifié),
# 0 divergence en verif, les trois mutants vus (divergences ou somme fausse).
# Code de sortie 0 si tout est conforme.
set -u
# compilation dans le fil du vCPU (déterministe : le banc est court, la
# compilation à part installerait les régions trop tard pour les éprouver)
export POMPPC_T3_SYNC="${POMPPC_T3_SYNC-1}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QB="${1:?QEMU_BIN requis}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
powerpc-linux-gnu-as -mregnames -many -o "$OUT/t3proof.o" "$HERE/t3proof.S" || exit 2
powerpc-linux-gnu-objcopy -O binary "$OUT/t3proof.o" "$OUT/t3proof.bin" || exit 2

run() {   # run NOM PROPS_CPU [ENV...] → « somme pièges » sur stdout, bilan dans $OUT/NOM.err
    local name=$1 props=$2; shift 2
    local sock="$OUT/$name.sock"
    env "$@" "$QB" -M mac99 -cpu "g4${props:+,$props}" -m 256 -smp 1 \
        -bios "$OUT/t3proof.bin" -display none -serial none -parallel none \
        -nodefaults -audio none -monitor "unix:$sock,server=on,wait=off" \
        > "$OUT/$name.out" 2> "$OUT/$name.err" &
    local pid=$! sum="" t
    for t in $(seq 1 600); do
        sleep 0.5
        [ -S "$sock" ] || continue
        local r
        r=$(python3 "$HERE/hmp.py" "$sock" "xp /3wx 0x3000" 2>/dev/null | tail -1)
        if echo "$r" | grep -q "0xc0ffee00"; then
            sum=$(echo "$r" | awk '{print $3 "/" $4}')
            break
        fi
        kill -0 $pid 2>/dev/null || break
    done
    python3 "$HERE/hmp.py" "$sock" "quit" > /dev/null 2>&1
    wait $pid 2>/dev/null
    echo "${sum:-AUCUNE}"
}

fail=0
T3="x-tier2=64,x-tier3=1000"
REF=$(run ref "x-tier2=0")
T=$(run t3 "$T3")
VER=$(run verif "$T3,x-tier2-verify=1")
M1=$(run mutant1 "$T3,x-tier2-verify=1" POMPPC_T3_MUTANT=1)
M2=$(run mutant2 "$T3,x-tier2-verify=1" POMPPC_T3_MUTANT=2)
M3=$(run mutant3 "$T3,x-tier2-verify=1" POMPPC_T2_MUTANT=3)
echo "somme/pièges ref $REF  t3 $T  verif $VER  mutant1 $M1  mutant2 $M2  mutant3 $M3"
for n in t3 verif mutant1 mutant2 mutant3; do
    echo "--- $n"; grep -a "x-tier" "$OUT/$n.err" | grep -av "DIVERGENCE\|région [0-9]* non" | sed 's/^/  /'
    grep -a DIVERGENCE "$OUT/$n.err" | head -2 | cut -c1-220 | sed 's/^/  /'
done
divs() { grep -a "traces vérifiées" "$OUT/$1.err" | sed -n 's/.* \([0-9]*\) DIVERGENCES.*/\1/p'; }
[ "$REF" != AUCUNE ] || { echo "ÉCHEC : la passe de référence ne finit pas"; fail=1; }
[ "$T" = "$REF" ] || { echo "ÉCHEC : somme x-tier3 différente de la référence"; fail=1; }
[ "$VER" = "$REF" ] || { echo "ÉCHEC : somme x-tier2-verify différente (l'invité est altéré)"; fail=1; }
nreg=$(grep -ao "demandes, [0-9]* régions" "$OUT/t3.err" | awk '{print $2}')
[ "${nreg:-0}" -gt 0 ] || { echo "ÉCHEC : aucune région formée"; fail=1; }
dead=$(grep -ao "[0-9]* mortes" "$OUT/t3.err" | awk '{print $1}')
[ "${dead:-0}" -gt 0 ] || { echo "ÉCHEC : aucune région morte (le code modifié n'a rien tué)"; fail=1; }
vline=$(grep -a "traces vérifiées" "$OUT/verif.err")
vrun=$(echo "$vline" | awk '{print $3}')
vbad=$(divs verif)
[ "${vrun:-0}" -gt 0 ] || { echo "ÉCHEC : aucune région vérifiée"; fail=1; }
[ "${vbad:-1}" = 0 ] || { echo "ÉCHEC : $vbad divergence(s) en verif"; fail=1; }
for m in 1 2 3; do
    eval "ms=\$M$m"
    mb=$(divs mutant$m)
    if [ "${mb:-0}" = 0 ] && [ "$ms" = "$REF" ]; then
        echo "ÉCHEC : le mutant $m passe inaperçu"; fail=1
    fi
done
[ $fail = 0 ] && echo "t3proof : CONFORME" || echo "t3proof : NON CONFORME"
exit $fail
