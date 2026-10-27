#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# t2proof.sh QEMU_BIN — non-régression du second niveau tcg/0048 (x-tier2,
# x-tier2-verify) sur le banc nu tools/tcg/t2proof.S (mac99 sans OpenBIOS,
# mode réel, 1 vCPU) : accès hors journal (lswx/stswx) dans une trace qui fait
# glisser un tampon sur lui-même, et long bloc droit dont la copie journalisée
# était coupée par TCG (docs/plan-traducteur-rapide.md §9.5).
#
#   tools/tcg/t2proof.sh ~/src/qemu-t2fix/build/qemu-system-ppc
# Sur macOS, l'assemblage passe par tools/guest/tssh.sh (Tiger démarré).
# T2PROOF_ROM=… réutilise une ROM déjà assemblée ; T2PROOF_OUT=/tmp/… garde
# les journaux (chemin court pour les sockets Unix). T2PROOF_CPU=… ajoute
# des propriétés CPU ; activer x-fast-fp et les leviers natifs pour la phase
# flottante de régression. Le M4 emploie split-wx, comme run_tiger.sh.
#
# Passes : ref (x-tier2=0), t2 (x-tier2=64), verif (x-tier2=64,x-tier2-verify=1),
# mutant (POMPPC_T2_MUTANT=2 : toutes les sorties latérales « pris » à l'envers,
# le vérificateur doit l'attraper), mutant3 (POMPPC_T2_MUTANT=3 : le garde des
# accès hors journal coupé, c.-à-d. le défaut du 21/10 : divergences ou somme
# fausse attendues). Verdict : même somme en ref/t2/verif, traces formées,
# 0 divergence en verif avec des traces identiques ET des traces déclarées non
# vérifiables (lswx/stswx), les deux mutants vus.
# Code de sortie 0 si tout est conforme.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QB="${1:?QEMU_BIN requis}"
ACCEL=tcg,x-jit-near=on
# Le compteur de chaleur écrit dans l'en-tête du TB : MAP_JIT interdit
# ces écritures depuis le code exécuté. La vue RW de split-wx les permet.
case "$(uname -s):$(uname -m)" in Darwin:arm64) ACCEL="$ACCEL,split-wx=on" ;; esac
OUT="${T2PROOF_OUT:-$(mktemp -d)}"
mkdir -p "$OUT"
[ -n "${T2PROOF_OUT:-}" ] || trap 'rm -rf "$OUT"' EXIT
if [ -n "${T2PROOF_ROM:-}" ]; then
    cp "$T2PROOF_ROM" "$OUT/t2proof.bin" || exit 2
else
    python3 "$HERE/t2proof-assemble.py" "$OUT/t2proof.bin" || exit 2
fi

run() {   # run NOM PROPS_CPU [ENV...] → somme sur stdout, bilan dans $OUT/NOM.err
    local name=$1 props=$2; shift 2
    local sock="$OUT/$name.sock"
    env "$@" "$QB" -M mac99 -cpu "g4${props:+,$props}${T2PROOF_CPU:+,$T2PROOF_CPU}" -m 256 -smp 1 \
        -accel "$ACCEL" \
        -bios "$OUT/t2proof.bin" -display none -serial none -parallel none \
        -nodefaults -audio none -monitor "unix:$sock,server=on,wait=off" \
        > "$OUT/$name.out" 2> "$OUT/$name.err" &
    local pid=$! sum="" t
    for t in $(seq 1 600); do
        sleep 0.5
        kill -0 $pid 2>/dev/null || break
        [ -S "$sock" ] || continue
        local r
        r=$(python3 "$HERE/hmp.py" "$sock" "xp /2wx 0x3000" 2>/dev/null | tail -1)
        if echo "$r" | grep -q "0xc0ffee00"; then
            sum=$(echo "$r" | awk '{print $3}')
            break
        fi
        kill -0 $pid 2>/dev/null || break
    done
    python3 - "$HERE/hmp.py" "$sock" <<'PY' > /dev/null 2>&1
import subprocess, sys
try:
    subprocess.run([sys.executable, sys.argv[1], sys.argv[2], "quit"], timeout=5)
except subprocess.TimeoutExpired:
    pass
PY
    # Un vCPU bloqué dans le code émis peut ignorer quit et SIGTERM.
    # Ne jamais laisser ce processus de banc derrière nous.
    for t in $(seq 1 20); do
        kill -0 $pid 2>/dev/null || break
        sleep 0.1
    done
    kill -0 $pid 2>/dev/null && kill -KILL $pid 2>/dev/null
    wait $pid 2>/dev/null
    echo "${sum:-AUCUNE}"
}

fail=0
REF=$(run ref "x-tier2=0")
T2=$(run t2 "x-tier2=64")
VER=$(run verif "x-tier2=64,x-tier2-verify=1")
MUT=$(run mutant "x-tier2=64,x-tier2-verify=1" POMPPC_T2_MUTANT=2)
MUT3=$(run mutant3 "x-tier2=64,x-tier2-verify=1" POMPPC_T2_MUTANT=3)
echo "somme ref $REF  t2 $T2  verif $VER  mutant $MUT  mutant3 $MUT3"
for n in t2 verif mutant mutant3; do
    echo "--- $n"; grep -a "x-tier2" "$OUT/$n.err" | grep -av DIVERGENCE | sed 's/^/  /'
    grep -a DIVERGENCE "$OUT/$n.err" | head -2 | cut -c1-200 | sed 's/^/  /'
done
[ "$REF" != AUCUNE ] || { echo "ÉCHEC : la passe de référence ne finit pas"; fail=1; }
[ "$T2" = "$REF" ] || { echo "ÉCHEC : somme x-tier2 différente de la référence"; fail=1; }
[ "$VER" = "$REF" ] || { echo "ÉCHEC : somme x-tier2-verify différente (l'invité est altéré)"; fail=1; }
traces=$(grep -ao "demandes, [0-9]* traces" "$OUT/t2.err" | awk '{print $2}')
[ "${traces:-0}" -gt 0 ] || { echo "ÉCHEC : aucune trace formée"; fail=1; }
vline=$(grep -a "traces vérifiées" "$OUT/verif.err")
vrun=$(echo "$vline" | awk '{print $3}')
vbad=$(echo "$vline" | sed -n 's/.* \([0-9]*\) DIVERGENCES.*/\1/p')
[ "${vrun:-0}" -gt 0 ] || { echo "ÉCHEC : aucune trace vérifiée"; fail=1; }
[ "${vbad:-1}" = 0 ] || { echo "ÉCHEC : $vbad divergence(s) en verif"; fail=1; }
mbad=$(grep -a "traces vérifiées" "$OUT/mutant.err" | sed -n 's/.* \([0-9]*\) DIVERGENCES.*/\1/p')
[ "${mbad:-0}" -gt 0 ] || { echo "ÉCHEC : le mutant 2 (sorties latérales à l'envers) n'est pas attrapé"; fail=1; }
m3bad=$(grep -a "traces vérifiées" "$OUT/mutant3.err" | sed -n 's/.* \([0-9]*\) DIVERGENCES.*/\1/p')
if [ "${m3bad:-0}" = 0 ] && [ "$MUT3" = "$REF" ]; then
    echo "ÉCHEC : le mutant 3 (accès hors journal rejoués, défaut du 21/10) passe inaperçu"; fail=1
fi
cut=$(grep -ao "[0-9]* raccourcies" "$OUT/verif.err" | awk '{print $1}')
lcut=$(grep -ao "[0-9]* copies coupées" "$OUT/verif.err" | awk '{print $1}')
vskip=$(echo "$vline" | sed -n 's/.*DIVERGENCES, \([0-9]*\) non vérifiables.*/\1/p')
vok=$(echo "$vline" | sed -n 's/.*vérifiées, \([0-9]*\) identiques.*/\1/p')
echo "verif : ${vok:-?} identiques, ${vskip:-?} non vérifiables (lswx/stswx), ${cut:-?} trace(s) raccourcie(s), ${lcut:-?} copie(s) journalisée(s) coupée(s)"
[ "${cut:-0}" -gt 0 ] || { echo "ÉCHEC : la trace trop longue n'a pas été raccourcie"; fail=1; }
[ "${vskip:-0}" -gt 0 ] || { echo "ÉCHEC : la trace à lswx/stswx n'est pas déclarée non vérifiable"; fail=1; }
[ "${vok:-0}" -gt 0 ] || { echo "ÉCHEC : aucune trace identique"; fail=1; }
[ $fail = 0 ] && echo "t2proof : CONFORME" || echo "t2proof : NON CONFORME"
exit $fail
