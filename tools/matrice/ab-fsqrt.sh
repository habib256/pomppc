#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# ab-fsqrt.sh N [JEUX] [MODES] — A/B entrelacé de kext/POMPPCFsqrt (kHasFsqrt dans la
# commpage : la libm prend `fsqrt` au lieu de sa racine logicielle).
#
#   tools/matrice/ab-fsqrt.sh 3 nx,prey fen
#
# Pour chaque tour i et chaque bras (ordre alterné : 0/1 puis 1/0), met le kext dans
# l'état voulu (kextunload / kextload depuis ~/pomppc-build/fsqrt/POMPPCFsqrt (persistant : /tmp est vidé au démarrage), construit par
# `make` dans l'invité), joue la matrice --sans-vidage, puis relit le bit : une
# cellule dont l'invité a redémarré entre-temps (kext perdu) est marquée INVALIDE.
# Sorties : bench/vitesse/ab-fsqrt/<bras><i>/ et resume.txt.
set -u
cd "$(dirname "$0")/../.."
N=${1:-3}; JEUX=${2:-nx,prey}; MODES=${3:-fen}
O=bench/vitesse/ab-fsqrt; mkdir -p $O
T=tools/guest/tssh.sh

etat() {   # 1 si kHasFsqrt est levé dans la commpage de l'invité
    $T 'printf "%s" "$(~/pomppc-build/fsqrt/sqrttest/sqrttest 0 2>/dev/null | grep -c levé)"' 2>/dev/null
}
met() {    # met le kext dans l'état $1
    if [ "$1" = 1 ]; then
        $T 'cd ~/pomppc-build/fsqrt/POMPPCFsqrt && echo tiger974 | sudo -S make load >/dev/null 2>&1; true'
    else
        $T 'echo tiger974 | sudo -S kextunload -b net.pomppc.POMPPCFsqrt >/dev/null 2>&1; true'
    fi
    [ "$(etat)" = "$1" ] || { echo "état $1 non obtenu" >&2; return 1; }
}

for i in $(seq 1 $N); do
    if [ $((i % 2)) = 1 ]; then ordre="0 1"; else ordre="1 0"; fi
    for b in $ordre; do
        d=$O/b$b-$i
        met $b || { echo "b$b-$i : INVALIDE (état)" >> $O/resume.txt; continue; }
        python3 tools/matrice/matrice.py -j $JEUX -m $MODES --sans-vidage --sortie $d > $d.log 2>&1
        apres=$(etat)
        v=""; [ "$apres" = "$b" ] || v=" INVALIDE (kext perdu : invité redémarré ?)"
        awk -F, -v b=$b -v i=$i -v v="$v" 'NR>1 {print "b" b "-" i, $1, $2, $8 v}' $d/resultats.csv >> $O/resume.txt
    done
done
echo FINI >> $O/resume.txt
