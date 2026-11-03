#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
#
# Le finaliseur générique de pools de tcg/0066 sur x86-64 : suites invitées sans et avec la zone
# froide (x-tcg-cold), en SMP=1 puis en SMP=2 sur tampon de 64 Mio, vérificateurs allumés. Pendant
# de docs/tcg-zone-froide-arm64.md, dont les preuves natives M4 ne valent pas pour cet hôte.
# Aucune vitesse n'est à tirer de ces essais instrumentés.
#
#   DEVDISK=copie.raw QEMU_BIN=~/src/qemu/build-fast/qemu-system-ppc tools/tcg/froid-x86-proof.sh
#
# Sortie : FROID_OUT/{temoin,froid}-smp{1,2}/, qemu-*.log, bilan.txt. Code 1 si une sortie diffère
# ou si un test invité compte une erreur.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${DEVDISK:?copie raw dédiée requise}"
: "${QEMU_BIN:?qemu-system-ppc requis}"
export DEVDISK QEMU_BIN
OUT="${FROID_OUT:-$ROOT/bench/tcg/froid-x86-$(date +%m%d-%H%M)}"
[ ! -e "$OUT" ] || { echo "Dossier déjà présent : $OUT" >&2; exit 1; }
mkdir -p "$OUT/job"
DL="$ROOT/tools/guest/devloop.py"
if [ -f "$ROOT/bench/devloop/qemu.pid" ] && kill -0 "$(cat "$ROOT/bench/devloop/qemu.pid")" 2>/dev/null; then
  echo "VM de dev déjà active." >&2; exit 1
fi
J="$ROOT/tools/guest/jobs"
for n in vfptest vmxtest lmwtest tbtest smctest jctest tlbbtest; do cp "$J/$n/$n.c" "$OUT/job/"; done
cp "$J/fptest/fptest.c" "$J/fptest/fpexctest.c" "$OUT/job/"
cat > "$OUT/job/job.sh" <<'JOB'
#!/bin/sh
OUT=$PWD/out
export NVEC=4194304
for n in vfptest vmxtest; do
  gcc -O2 -faltivec -Wall -o $n $n.c > $OUT/$n-build.txt 2>&1 && ./$n > $OUT/$n.txt 2> $OUT/$n.err
  echo "$n rc=$?"
done
gcc -O1 -mdynamic-no-pic -Wall -o lmwtest lmwtest.c > $OUT/lmwtest-build.txt 2>&1 && ./lmwtest > $OUT/lmwtest.txt 2>&1
echo "lmwtest rc=$?"
gcc -O2 -Wall -o tbtest tbtest.c > $OUT/tbtest-build.txt 2>&1 && ./tbtest > $OUT/tbtest.txt 2>&1
echo "tbtest rc=$?"
gcc -O2 -pthread -Wall -o smctest smctest.c > $OUT/smctest-build.txt 2>&1 && ./smctest > $OUT/smctest.txt 2>&1
echo "smctest rc=$?"
gcc -O2 -Wall -o jctest jctest.c > $OUT/jctest-build.txt 2>&1 && ./jctest 200000 > $OUT/jctest.txt 2>&1
echo "jctest rc=$?"
gcc -O2 -Wall -o tlbbtest tlbbtest.c > $OUT/tlbbtest-build.txt 2>&1 && ./tlbbtest 20000 > $OUT/tlbbtest.txt 2>&1
echo "tlbbtest rc=$?"
gcc -O2 -Wall -o fptest fptest.c > $OUT/fptest-build.txt 2>&1
for m in s d c k; do
  MODE=$m; [ $m = s ] && MODE=
  MODE=$MODE NRAND=65536 ./fptest > $OUT/fptest-$m.txt 2>&1
  echo "fptest-$m rc=$?"
done
gcc -std=gnu99 -O2 -Wall -o fpexctest fpexctest.c > $OUT/fpexctest-build.txt 2>&1 && ./fpexctest > $OUT/fpexctest.txt 2>&1
echo "fpexctest rc=$?"
JOB
BASE="$(sed -n 's/^BASE="\(.*\)"$/\1/p' "$ROOT/tools/tcg/jit-m4-proof.sh")"
[ -n "$BASE" ] || { echo "BASE introuvable dans jit-m4-proof.sh" >&2; exit 1; }
VERIF="x-vfp-native=on,x-vfp-native-verify=on,x-vmx-verify=on,x-tb-verify=on,x-lmw-vector=on"
PROBE="$OUT/sonde"; mkdir -p "$PROBE"; printf '#!/bin/sh\necho agent-ok\n' > "$PROBE/job.sh"
agent() {
  local boxes k
  boxes="$(python3 -c 'import json,sys; c=json.load(open(sys.argv[1])); print(c["IN"], c["OU"])' "$ROOT/bench/devloop/mailbox.json")"
  for k in 1 2 3 4 5; do
    python3 "$DL" run "$PROBE" --timeout 150 >/dev/null 2>&1 && return 0
    echo "agent muet ($k) : frappe relancée"
    python3 "$DL" type '\n'; sleep 2
    python3 "$DL" type 'mount -uw /\n'; sleep 4
    python3 "$DL" type "sh /pomppc/agent.sh $boxes\n"; sleep 15
  done
  return 1
}
rc=0
bras() {  # nom smp tcg_opts
  local nom="$1"; echo "=== $nom : SMP=$2 TCG=$3 ($(date +%H:%M)) ==="
  export SMP="$2" PPC32MT=1 CPU_OPTS="$BASE,$VERIF" TCG_OPTS="$3"
  python3 "$DL" start || { echo "$nom : démarrage raté"; rc=1; return; }
  unset DEVFSCK
  if agent; then
    python3 "$DL" run "$OUT/job" --timeout 3600 2>&1 | tail -20
    cp -R "$ROOT/bench/devloop/last/out" "$OUT/$nom"
  else
    echo "$nom : agent muet"; rc=1
  fi
  python3 "$DL" shutdown >/dev/null 2>&1
  cp "$ROOT/bench/devloop/qemu.log" "$OUT/qemu-$nom.log"
}
NEAR="x-jit-near=on,x-jc-bits=14,x-jc-word=on"
bras temoin-smp1 1 "$NEAR"
bras froid-smp1  1 "$NEAR,x-tcg-cold=on"
bras temoin-smp2 2 "$NEAR,tb-size=64"
bras froid-smp2  2 "$NEAR,tb-size=64,x-tcg-cold=on"
{
  for s in smp1 smp2; do
    echo "--- temoin-$s / froid-$s"
    for f in "$OUT/temoin-$s"/*.txt; do
      n="$(basename "$f")"; case "$n" in *-build.txt) continue ;; esac
      case "$n" in log.txt) continue ;; esac   # durées
      # jctest et tbtest impriment des temps : seuls les résultats se comparent.
      net() { grep -v '^banc' "$1" | sed -E 's/ns=[0-9.]+//; s/[0-9.]+ ns//g; s/plus grand saut [0-9]+//; s/fréquence [0-9.]+ MHz//'; }
      if diff <(net "$f") <(net "$OUT/froid-$s/$n") >/dev/null 2>&1; then
        echo "identique  $n"
      else
        echo "DIFFÈRE    $n"; rc=1
      fi
    done
  done
  echo "--- vérificateurs (signalements par journal)"
  for l in "$OUT"/qemu-*.log; do
    printf '%s divergences=%s tlb=%s ret=%s\n' "$(basename "$l")" \
      "$(grep -cE 'DIVERGENCE|[1-9][0-9]* divergences|vfp-native-verify: op |vmx-verify: (op |stve )' "$l")" \
      "$(grep -c 'tlb.*verify' "$l")" "$(grep -c 'ret-verify' "$l")"
  done
  echo "--- résumés des tests"
  grep -H -E 'erreurs? [1-9]|erreurs=[1-9]|^exceptions FP|FAIL|ÉCHEC' "$OUT"/{temoin,froid}-smp?/*.txt 2>/dev/null | sed "s#$OUT/##" | cut -c1-120
  echo "rc=$rc"
} | tee "$OUT/bilan.txt"
exit "$rc"
