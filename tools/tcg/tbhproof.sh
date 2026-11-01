#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# tbhproof.sh QEMU_BIN — épreuve de la tête de bloc allégée tcg/0064 (x-tb-head,
# docs/anatomie-ppc32.md) sur le banc nu tools/tcg/tbhproof.S (mac99 sans OpenBIOS,
# mode réel, 1 vCPU) : interruptions du décrémenteur dans une boucle sur elle-même et
# dans un cycle de blocs chaînés vers l'avant, lectures d'E/S au milieu et en fin de bloc.
#
#   tools/tcg/tbhproof.sh ~/src/qemu-anat/build-mt/qemu-system-ppc
#
# Passes (chacune en thread=single et thread=multi) : ref (x-tb-head=0), tete
# (x-tb-head=3), verif (x-tb-head=7 : stores de can_do_io gardés, valeur déduite
# comparée à chaque E/S) ; puis mutant1 (POMPPC_TBHEAD_MUTANT=1 : toute arête chaînée
# après la vérification d'interruption — la boucle 1 ne doit plus jamais finir) et
# mutant2 (POMPPC_TBHEAD_MUTANT=2 sous x-tb-head=5 : règle de can_do_io à l'envers — le
# vérificateur doit compter des divergences). Code de sortie 0 si tout est conforme.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QB="${1:?QEMU_BIN requis}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
powerpc-linux-gnu-as -mregnames -many -o "$OUT/t.o" "$HERE/tbhproof.S" || exit 2
powerpc-linux-gnu-objcopy -O binary "$OUT/t.o" "$OUT/t.bin" || exit 2
NPH=100

run() {   # run NOM THREAD TBHEAD DÉLAI [ENV...] → « marque nb A B C E/S faute » sur stdout
    local name=$1 thr=$2 th=$3 lim=$4; shift 4
    # socket relative (chemin AF_UNIX < 108 octets quel que soit TMPDIR)
    local sock="$name.sock" res="" t
    cd "$OUT" || return
    env "$@" "$QB" -M mac99 -cpu g4 -m 256 -smp 1 -accel "tcg,thread=$thr,x-tb-head=$th" \
        -bios "$OUT/t.bin" -display none -serial none -parallel none \
        -nodefaults -audio none -monitor "unix:$sock,server=on,wait=off" \
        > "$OUT/$name.out" 2> "$OUT/$name.err" &
    local pid=$!
    for t in $(seq 1 $((lim * 2))); do
        sleep 0.5
        [ -S "$sock" ] || continue
        local r
        r=$(timeout 10 python3 "$HERE/hmp.py" "$sock" "xp /9wx 0x3000" 2>/dev/null | tr -s ' \n' ' ')
        if echo "$r" | grep -q "0xc0ffee00"; then
            # 3000: marque nb (3008) ; 3010: A B C E/S ; 3020: faute
            res=$(echo "$r" | awk '{print $2, $3, $7, $8, $9, $10, $12}')
            break
        fi
        kill -0 $pid 2>/dev/null || break
    done
    timeout 10 python3 "$HERE/hmp.py" "$sock" "quit" > /dev/null 2>&1
    kill $pid 2>/dev/null; sleep 0.5; kill -9 $pid 2>/dev/null
    wait $pid 2>/dev/null
    cd - > /dev/null
    echo "${res:-AUCUNE}"
}

fail=0
nb_ok() { [ "$(printf '%d' "$(echo "$1" | awk '{print $2}')" 2>/dev/null)" = $((3 * NPH)) ]; }
for thr in single multi; do
    for p in "ref 0" "tete 3" "verif 7"; do
        set -- $p
        R=$(run "$1-$thr" $thr $2 60)
        echo "$1 ($thr, x-tb-head=$2) : $R"
        grep -a "tb-head:" "$OUT/$1-$thr.err" | sed 's/^/  /'
        if [ "$R" = AUCUNE ] || ! nb_ok "$R"; then
            echo "ÉCHEC : $1/$thr ne finit pas avec $((3 * NPH)) interruptions"; fail=1
        fi
        fault=$(echo "$R" | awk '{print $7}')
        [ "${fault:-0x00000000}" = 0x00000000 ] || { echo "ÉCHEC : faute $fault ($1/$thr)"; fail=1; }
    done
    io=$(grep -ao "E/S : [0-9]* par la dernière instruction de leur bloc, [0-9]* au milieu" "$OUT/tete-$thr.err")
    last=$(echo "$io" | awk '{print $3}'); mid=$(echo "$io" | awk '{print $11}')
    { [ "${last:-0}" -gt 0 ] && [ "${mid:-0}" -gt 0 ]; } || {
        echo "ÉCHEC : la phase 3 n'a pas fait d'E/S en fin ET au milieu de bloc ($thr)"; fail=1; }
    fwd=$(grep -ao "[0-9]* vers l'avant" "$OUT/tete-$thr.err" | awk '{print $1}')
    [ "${fwd:-0}" -gt 0 ] || { echo "ÉCHEC : aucun chaînage vers l'avant ($thr)"; fail=1; }
    vl=$(grep -a "vérification :" "$OUT/verif-$thr.err")
    vok=$(echo "$vl" | sed -n 's/.*vérification : \([0-9]*\) identiques.*/\1/p')
    vbad=$(echo "$vl" | sed -n 's/.* \([0-9]*\) DIVERGENCES.*/\1/p')
    { [ "${vok:-0}" -gt 0 ] && [ "${vbad:-1}" = 0 ]; } || {
        echo "ÉCHEC : vérification de can_do_io ($thr) : ${vok:-?} identiques, ${vbad:-?} divergences"; fail=1; }
done
M1=$(run mutant1 multi 3 20 POMPPC_TBHEAD_MUTANT=1)
echo "mutant1 (toute arête après la vérification) : $M1"
[ "$M1" = AUCUNE ] || { echo "ÉCHEC : le mutant 1 finit (la boucle sur elle-même devait ne plus voir le décrémenteur)"; fail=1; }
M2=$(run mutant2 multi 5 60 POMPPC_TBHEAD_MUTANT=2)
m2bad=$(grep -a "vérification :" "$OUT/mutant2.err" | sed -n 's/.* \([0-9]*\) DIVERGENCES.*/\1/p')
echo "mutant2 (règle de can_do_io à l'envers) : $M2 ; ${m2bad:-?} divergences"
[ "${m2bad:-0}" -gt 0 ] || { echo "ÉCHEC : le mutant 2 n'est pas vu par la vérification"; fail=1; }
[ $fail = 0 ] && echo "tbhproof : CONFORME" || echo "tbhproof : NON CONFORME"
exit $fail
