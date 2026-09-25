#!/bin/bash
# mbab.sh <config>… — A/B Marble Blast entrelacé sur le disque de DÉVELOPPEMENT
# (jamais la VM quotidienne). Une config = « étiquette:SMP:propriétés CPU » ;
# chaque config = un démarrage de la VM, une chauffe, NPASS passes de 240 s
# (job tools/guest/jobs/tcgmb), puis arrêt propre. Exemple :
#
#   DEVDISK=bench/tcg/tiger-tcg.raw tools/tcg/mbab.sh s2off:2: s2on:2:x-sr-tlb=on \
#       s1off:1: s2off:2: s2on:2:x-sr-tlb=on s1off:1:
#
# x-fast-fp=on est toujours posé (comme run_tiger.sh). Résultats :
# bench/tcg/res/<manche>-mb-<étiquette>-<n>.txt, où la manche vaut ROUND (défaut r)
# suivi du rang de l étiquette dans la liste (s2off:2: s2on:2:… s2off:2:
# donne r1 puis r2 pour s2off ; ROUND=p : p1, p2…) ; `info jit` dans <manche>-jit-<étiquette>.txt,
# charge de l'hôte au début de chaque config dans load.txt. Bilan : mbreport.sh.
#   SAMPLE_AT=S  `sample` hôte de QEMU pendant 10 s, S secondes après le début
#                du job (chauffe comprise) → <manche>-sample-<étiquette>.txt
#   PLUGIN=…     greffon TCG ajouté (p. ex. "tools/tcg/libppcmix.dylib,out=…,interval=10")
#
# ⚠ Le StartupItem de l'agent rejoue au démarrage le DERNIER job de la boîte
# d'entrée (un `shutdown` ici) : l'en-tête de la boîte est effacé avant chaque
# démarrage (secteurs lus dans bench/devloop/mailbox.json).
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$WT"
DISK="${DEVDISK:?DEVDISK=<image raw de dev> requis}"
QB="${QEMU_BIN:-$HOME/src/qemu-tcg/build/qsr}"
NPASS="${NPASS:-2}"
RES="$WT/bench/tcg/res"; mkdir -p "$RES"
MON="$WT/bench/devloop/mon.sock"
vus=" "
for cfg in "$@"; do
  IFS=: read -r tag smp opts <<< "$cfg"
  vus="$vus$tag "
  k=$(tr ' ' '\n' <<< "$vus" | grep -cx "$tag")
  rnd="${ROUND:-r}$k"
  echo "=== $rnd $tag : SMP=$smp props=${opts:-} ($(date +%H:%M:%S))"
  python3 - "$DISK" <<'PY'
import json, sys
box = json.load(open("bench/devloop/mailbox.json"))
with open(sys.argv[1], "r+b") as f:
    f.seek(box["IN"] * 512); f.write(b"\0" * 512)
PY
  DEVDISK="$DISK" QEMU_BIN="$QB" SMP="$smp" CPU_OPTS="x-fast-fp=on${opts:+,$opts}" \
    QEMU_EXTRA="-monitor unix:$MON,server=on,wait=off${PLUGIN:+ -plugin $PLUGIN}" \
    python3 tools/guest/devloop.py start --gui | tail -1
  { echo "== $tag $(date)"; top -l 2 -s 3 -n 6 -o cpu -stats pid,command,cpu | tail -7; } >> "$RES/load.txt"
  J="$WT/bench/tcg/job-$tag"
  sh tools/guest/jobs/stage.sh tcgmb "$J" > /dev/null && rm -rf "$J/src"
  printf 'TAG=%s\nNPASS=%s\n' "$tag" "$NPASS" > "$J/env.sh"
  python3 tools/tcg/jitpoll.py "$MON" 10 $(( 200 + NPASS * 340 )) > "$RES/$rnd-jit-$tag.txt" 2>&1 &
  if [ -n "${SAMPLE_AT:-}" ]; then
    ( sleep "$SAMPLE_AT"; sample "$(pgrep -f "$(basename "$DISK")" | head -1)" 10 \
        -file "$RES/$rnd-sample-$tag.txt" > /dev/null 2>&1 ) &
  fi
  DEVDISK="$DISK" python3 tools/guest/devloop.py run "$J" --timeout $(( 600 + NPASS * 400 )) \
    > "$RES/$rnd-run-$tag.log" 2>&1
  out=$(sed -n 's/^→ //p' "$RES/$rnd-run-$tag.log" | tail -1)
  if [ -n "$out" ]; then
    for f in "$out"/mb-"$tag"-*.txt; do
      [ -f "$f" ] && cp "$f" "$RES/$rnd-$(basename "$f")"
    done
  fi
  grep -h "x-sr-tlb-verify" bench/devloop/qemu.log | tail -3 > "$RES/$rnd-verify-$tag.txt" 2>/dev/null
  DEVDISK="$DISK" python3 tools/guest/devloop.py shutdown --gui | tail -1
  sleep 5
done
echo "=== fini $(date +%H:%M:%S)"
