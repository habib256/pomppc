#!/bin/sh
# fpbench — le flottant PowerPC : preuve d'équivalence (fpcheck) et vitesse (fpbench).
#
# Se lance identiquement sur le QEMU actuel (mode exact) et sur le QEMU « flottant
# rapide » (`CPU_OPTS=x-fast-fp=on`) : ce sont les sorties qu'on compare.
#
#   tools/guest/jobs/stage.sh fpbench /tmp/j
#   CPU_OPTS=x-fast-fp=on QEMU_BIN=... python3 tools/guest/devloop.py start
#   python3 tools/guest/devloop.py run /tmp/j --timeout 900
#
# Variables :
#   FPCHECK_N     itérations par section de fpcheck (défaut 4000)
#   FPCHECK_RUNS  nombre d'exécutions de fpcheck (défaut 2 : la 2e prouve le
#                 déterminisme ; mettre 1 quand il est déjà établi)
#   FPBENCH_RUNS  nombre d'exécutions de fpbench (défaut 3)
#   FPBENCH_PCT   échelle des itérations de fpbench, en pour-cent (défaut 100)
#   FPBENCH_CAL   =1 : passe de calibrage au lieu des mesures
#   NOALTIVEC     =1 : compile sans -faltivec
. ./lib.sh
FPCHECK_N=${FPCHECK_N:-4000}
FPCHECK_RUNS=${FPCHECK_RUNS:-2}
FPBENCH_RUNS=${FPBENCH_RUNS:-3}
FPBENCH_PCT=${FPBENCH_PCT:-100}

# repère sur la console (et disque synchronisé) : si une étape bloque, la capture
# d'écran de devloop dit laquelle, et arrêter la VM n'abîme rien
step() { sync; echo "fpbench: $*" > /dev/console; echo "== $*"; }

step "compilation"
cd $SRC/guest/fpbench || exit 1
make clean > /dev/null 2>&1
if [ -n "$NOALTIVEC" ]; then
  make noav > $OUT/build.txt 2>&1
else
  make > $OUT/build.txt 2>&1
fi
if [ $? != 0 ]; then
  echo "ÉCHEC de compilation :"; tail -30 $OUT/build.txt; exit 1
fi
grep -i 'warning' $OUT/build.txt | head -10
ls -l fpcheck fpbench

step "environnement"
{ echo "uname: $(uname -a)"; echo "gcc: $(/usr/bin/gcc-4.0 -v 2>&1 | tail -1)";
  echo "hw.cpusubtype: $(sysctl -n hw.cpusubtype 2>/dev/null)";
  echo "hw.vectorunit: $(sysctl -n hw.vectorunit 2>/dev/null)";
  echo "hw.optional.altivec: $(sysctl -n hw.optional.altivec 2>/dev/null)"; } | tee $OUT/env.txt

i=1
while [ $i -le $FPCHECK_RUNS ]; do
  step "fpcheck passe $i/$FPCHECK_RUNS (N=$FPCHECK_N)"
  ./fpcheck $FPCHECK_N > $OUT/fpcheck-$i.txt 2>&1 || { echo "fpcheck rc=$?"; tail -5 $OUT/fpcheck-$i.txt; }
  echo "sections : $(grep -c -v '^#' $OUT/fpcheck-$i.txt)" \
       "  ops : $(grep -v '^#' $OUT/fpcheck-$i.txt | awk '{s+=$2} END {print s}')"
  i=$((i + 1))
done

if [ $FPCHECK_RUNS -ge 2 ]; then
  step "déterminisme de fpcheck"
  if diff $OUT/fpcheck-1.txt $OUT/fpcheck-2.txt > $OUT/fpcheck-diff.txt 2>&1; then
    echo "DÉTERMINISTE : passes 1 et 2 identiques"
  else
    echo "NON DÉTERMINISTE :"; head -20 $OUT/fpcheck-diff.txt
  fi
fi

if [ -n "$FPBENCH_CAL" ]; then
  step "fpbench --calibrer"
  ./fpbench --calibrer > $OUT/fpbench-calibrage.txt 2>&1
  cat $OUT/fpbench-calibrage.txt
else
  i=1
  while [ $i -le $FPBENCH_RUNS ]; do
    step "fpbench passe $i/$FPBENCH_RUNS ($FPBENCH_PCT %)"
    ./fpbench $FPBENCH_PCT > $OUT/fpbench-$i.txt 2>&1
    cat $OUT/fpbench-$i.txt
    i=$((i + 1))
  done
  step "sommes de contrôle stables d'une passe à l'autre ?"
  for f in $OUT/fpbench-*.txt; do
    grep -v '^#' $f | awk '{print $1, $4}'
  done | sort | uniq -c | awk '$1 < '"$FPBENCH_RUNS"' {print "ÉCART :", $0}'
  echo "(aucune ligne ÉCART ci-dessus = sommes identiques sur les $FPBENCH_RUNS passes)"
fi

step "extraits"
if [ -f $OUT/fpcheck-1.txt ]; then
  head -6 $OUT/fpcheck-1.txt
  echo "..."
  grep -v '^#' $OUT/fpcheck-1.txt | head -5
else
  echo "(fpcheck non exécuté : FPCHECK_RUNS=$FPCHECK_RUNS)"
fi
sync
