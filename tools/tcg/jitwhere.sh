#!/bin/bash
# jitwhere.sh N [BINAIRE] [ARGS…] — où le noyau pose-t-il le tampon du JIT ?
# (docs/tcg-g4.md §14). Lance N fois QEMU arrêté (-S, sans invité, sans
# affichage), relève par `vmmap` la base du tampon du JIT (régions rwx/rwx
# de 64 Mio) et celle du texte du binaire, et imprime une ligne par lancement :
#   texte  jit  fenêtre(même|AUTRE)
# Ne perturbe rien : QEMU arrêté ne consomme rien, `vmmap` ~1 s.
# JITOPT=x-jit-near=on (ou x-jit-addr=…) : propriétés de l'accélérateur (tcg/0006) ;
# la dernière colonne est alors la ligne « tcg: tampon JIT … » que le patch imprime.
set -u
N=${1:-10}; BIN=${2:-$HOME/src/qemu-tcg19/build/qsr1564}; shift 2 2>/dev/null || shift $#
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for i in $(seq "$N"); do
  "$BIN" -M mac99,via=pmu -cpu g4 -m 768 -smp 2 -accel tcg,thread=multi${JITOPT:+,$JITOPT} -S \
    -display none -monitor none -serial none -nic none "$@" > "$TMP/log" 2>&1 &
  pid=$!
  sleep 1.5
  vmmap -interleaved "$pid" > "$TMP/vm" 2>/dev/null
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  jit=$(awk '/rwx\/rwx SM=ZER/ {split($2,a,"-"); print a[1]; exit}' "$TMP/vm")
  txt=$(awk -v b="$(basename "$BIN")" '$1=="__TEXT" && $0 ~ b"$" {split($2,a,"-"); print a[1]; exit}' "$TMP/vm")
  w="AUTRE"; [ "${jit:0:1}" = "${txt:0:1}" ] && [ ${#jit} = ${#txt} ] && w="même"
  echo "$txt $jit $w $(grep -h 'tampon JIT' "$TMP/log" | head -1)"
done
