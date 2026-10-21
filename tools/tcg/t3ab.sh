#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# t3ab.sh CAMPAGNE N "nom:PROPS[:ENV=v…]" … — A/B ENTRELACÉ du banc t2bench sur la VM de DEV
# (tools/tcg/t2dev.sh : single-user, 3 passes par démarrage), N tours, les bras dans l'ordre
# donné à chaque tour ; bilan : médiane des passes par bras, md5. Jamais la VM quotidienne.
#
#   DEVDISK=~/src/pomppc/disks/tiger-dev-t3.raw QEMU_BIN=~/src/qemu-t3/build/qemu-system-ppc \
#     tools/tcg/t3ab.sh t3ab 3 "base:x-tier2=0" "t2:x-tier2=4096" \
#       "t3:x-tier2=131072,x-tier3=200"
#
# Un bras peut porter des variables d'environnement pour QEMU après un second « : »
# (séparées par des virgules), p. ex. "ctl:x-tier2=131072,x-tier3=200:POMPPC_T3_NOFORM=1".
# Sorties : bench/tcg/t2/CAMPAGNE-<bras><k>/ et bench/tcg/t2/CAMPAGNE.bilan.
set -u
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
R="$(git -C "$WT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's|/\.git$||')"
R="${R:-$WT}"
C=${1:?campagne}; N=${2:?tours}; shift 2
B="$R/bench/tcg/t2/$C.bilan"
: > "$B"
for k in $(seq 1 "$N"); do
  for arm in "$@"; do
    name=${arm%%:*}; rest=${arm#*:}
    props=${rest%%:*}
    envs=""; [ "$rest" != "$props" ] && envs=${rest#*:}
    echo "=== tour $k bras $name ($props${envs:+ ; $envs}) $(date +%H:%M:%S) charge $(cut -d' ' -f1 /proc/loadavg)" | tee -a "$B"
    # shellcheck disable=SC2086
    env ${envs//,/ } "$WT/tools/tcg/t2dev.sh" "$C-$name$k" "$props" > /dev/null 2>&1
    grep -a '^passe' "$R/bench/tcg/t2/$C-$name$k/job.log" | sed "s/^/$name $k /" | tee -a "$B"
  done
done
echo "--- médianes" | tee -a "$B"
python3 - "$B" <<'PY' | tee -a "$B"
import re, statistics, sys
t = {}
md5 = {}
for line in open(sys.argv[1]):
    m = re.match(r"(\S+) (\d+) passe (\d+) rc=0 ([\d.]+) s md5 (\S+)", line)
    if m:
        t.setdefault(m.group(1), []).append(float(m.group(4)))
        md5.setdefault(m.group(1), set()).add(m.group(5))
base = None
for name, v in t.items():
    med = statistics.median(v)
    if base is None:
        base = med
    print("%-6s %2d passes  médiane %.3f s  (%+.1f %%)  min %.3f max %.3f  md5 %s"
          % (name, len(v), med, 100 * (med / base - 1), min(v), max(v),
             ",".join(sorted(x[:8] for x in md5[name]))))
PY
