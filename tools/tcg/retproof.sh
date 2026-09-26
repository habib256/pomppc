#!/bin/bash
# retproof.sh <config>… — preuves de tcg/0008 (x-ret-inline, x-jc-idx,
# docs/tcg-g4.md §16) sur le disque de DÉVELOPPEMENT (jamais la VM quotidienne).
# Une config = « étiquette:SMP:propriétés CPU » ; pour chacune : démarrage du
# bureau, job smctest (code modifié/remappé, sortie comparée entre configs),
# puis Marble Blast (job tcgmb, chauffe + une passe de DUR s), arrêt propre ;
# le bilan « ret-verify » de QEMU (si x-ret-verify=on) est relevé à la sortie.
#
#   DEVDISK=… QEMU_BIN=~/src/qemu-ret/build/qret15 TCG_OPTS=x-jit-near=on \
#     tools/tcg/retproof.sh "v2:2:x-ret-inline=on,x-jc-idx=on,x-ret-verify=on" \
#                           "o2:2:"
#
# Propriétés de base toujours posées : celles de run_tiger.sh par défaut
# (x-fast-fp, x-sr-tlb, x-lfs-inline, x-vfp-fast, x-vperm-fast, x-fp-inline).
# NOMB=1 : smctest seul ; NOSMC=1 : Marble Blast seul. Résultats : bench/tcg/ret/<étiquette>-{smc.txt,mb.txt,
# verify.txt,run-*.log}.
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$WT"
DISK="${DEVDISK:?DEVDISK=<image raw de dev> requis}"
QB="${QEMU_BIN:?QEMU_BIN requis}"
BASE="${BASEPROPS-x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-fp-inline=on}"
RES="$WT/bench/tcg/ret"; mkdir -p "$RES"
for cfg in "$@"; do
  IFS=: read -r tag smp opts <<< "$cfg"
  echo "=== $tag : SMP=$smp props=${opts:-} ($(date +%H:%M:%S))"
  python3 - "$DISK" <<'PY'
import json, sys
box = json.load(open("bench/devloop/mailbox.json"))
with open(sys.argv[1], "r+b") as f:
    f.seek(box["IN"] * 512); f.write(b"\0" * 512)
PY
  DEVDISK="$DISK" QEMU_BIN="$QB" SMP="$smp" CPU_OPTS="$BASE${opts:+${BASE:+,}$opts}" \
    python3 tools/guest/devloop.py start --gui | tail -1
  if [ -z "${NOSMC:-}" ]; then
  J="$WT/bench/tcg/job-smc-$tag"; rm -rf "$J"; mkdir -p "$J"
  cp tools/guest/jobs/smctest/* "$J/"
  [ -n "${SMCN:-}" ] && echo "N=$SMCN" > "$J/env.sh"
  DEVDISK="$DISK" python3 tools/guest/devloop.py run "$J" --timeout 3000 > "$RES/$tag-run-smc.log" 2>&1
  out=$(sed -n 's/^→ //p' "$RES/$tag-run-smc.log" | tail -1)
  [ -n "$out" ] && cp "$out/smctest.txt" "$RES/$tag-smc.txt"
  echo "   smctest : $(grep -h 'total erreurs' "$RES/$tag-smc.txt" 2>/dev/null)"
  fi
  if [ -z "${NOMB:-}" ]; then
    J="$WT/bench/tcg/job-mb-$tag"
    sh tools/guest/jobs/stage.sh tcgmb "$J" > /dev/null && rm -rf "$J/src"
    printf 'TAG=%s\nNPASS=1\nDUR=%s\n' "$tag" "${DUR:-240}" > "$J/env.sh"
    DEVDISK="$DISK" python3 tools/guest/devloop.py run "$J" --timeout 2400 > "$RES/$tag-run-mb.log" 2>&1
    out=$(sed -n 's/^→ //p' "$RES/$tag-run-mb.log" | tail -1)
    [ -n "$out" ] && cp "$out/mb-$tag-1.txt" "$RES/$tag-mb.txt" 2>/dev/null
  fi
  DEVDISK="$DISK" python3 tools/guest/devloop.py shutdown --gui | tail -1
  sleep 5
  grep -a "ret-verify" bench/devloop/qemu.log | tail -1 > "$RES/$tag-verify.txt"
  grep -a "DIVERGENCE" bench/devloop/qemu.log | head -20 >> "$RES/$tag-verify.txt"
  echo "   $(cut -c1-600 "$RES/$tag-verify.txt" | head -1)"
done
echo "=== fini $(date +%H:%M:%S)"
