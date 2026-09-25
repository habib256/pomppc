#!/bin/bash
# smpab.sh SMP:ÉTIQUETTE… — campagne « combien de cœurs » (docs/smp-coeurs.md) sur le
# disque de DÉVELOPPEMENT (jamais la VM quotidienne). Chaque argument = un processus
# QEMU neuf, un démarrage d'invité, lancé par tools/tcg/regab.sh (NGUEST=1) avec
# `-smp SMP` : bureau, `vmmap`, job `regime` (regbench + Marble Blast 110 s de
# chauffe + DUR s de mesure), `sample` hôte 10 s dans la passe de mesure, arrêt
# propre. En plus : `ps -M` (CPU par fil de QEMU) toutes les 20 s.
#
#   DEVDISK=… QEMU_BIN=<…/qemu-system-ppc, SMP>1 : même nom + 64> \
#       tools/tcg/smpab.sh 1:a1 2:a2 4:a4 3:a3 2:b2 1:b1 …
#
# Tout ce que regab.sh lit passe (CPU_OPTS, DUR, NTOUR, SAMPLE_AT) ; TCG_OPTS vaut
# x-jit-near=on par défaut (placement du tampon du JIT forcé, docs/tcg-g4.md §14) et
# QEXTRA nomme les fils (`-name …,debug-threads=on`) pour `sample`.
# Résultats : bench/reg/res/<étiquette>-* (voir regab.sh), <étiquette>-psM.txt ;
# bilan : tools/tcg/regreport.py "$(cat bench/reg/reffast.txt)" bench/reg/res ÉTIQUETTE…
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$WT"
RES="$WT/bench/reg/res"; mkdir -p "$RES"
export TCG_OPTS="${TCG_OPTS-x-jit-near=on}"
export QEXTRA="${QEXTRA--name smp,debug-threads=on}"
export NGUEST=1
for A in "$@"; do
  n=${A%%:*}; tag=${A#*:}
  rm -f bench/devloop/qemu.pid
  SMP="$n" bash tools/tcg/regab.sh "$tag" &
  ab=$!
  ( while kill -0 "$ab" 2>/dev/null; do
      if [ -f bench/devloop/qemu.pid ]; then
        p=$(cat bench/devloop/qemu.pid)
        { echo "== $(date +%H:%M:%S)"; ps -M -p "$p" 2>/dev/null; } >> "$RES/$tag-psM.txt"
      fi
      sleep 20
    done ) &
  wait "$ab"
  sleep 2
done
