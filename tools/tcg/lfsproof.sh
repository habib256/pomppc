#!/usr/bin/env bash
# lfsproof.sh [arbre-QEMU] — preuve hôte de patches/tcg/0002-ppc-lfs-inline.patch.
# Extrait helper_todouble / helper_tosingle TELS QUELS de target/ppc/fpu_helper.c
# de l'arbre (défaut ~/src/qemu-tcg19), compile tools/tcg/lfsproof.c contre eux et
# le lance (tous les float32 ; 2^35 float64 + cas limites ; ~1 min sur un M4).
# Vérifie aussi que le modèle C de lfsproof.c est bien celui que l'arbre émet :
# la suite des tcg_gen_* de gen_todouble_inline / gen_tosingle_inline est
# comparée à celle, écrite en commentaire, dans le modèle.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:-$HOME/src/qemu-tcg19}"
OUT="${TMPDIR:-/tmp}/lfsproof.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
F="$SRC/target/ppc/fpu_helper.c"
for fn in helper_todouble helper_tosingle; do
  awk -v fn="$fn" '
    $0 ~ "^uint(64|32)_t " fn "\\(" { on = 1 }
    on { print }
    on && /^}/ { exit }' "$F"
  echo
done > "$OUT/lfsproof-helpers.h"
grep -q helper_todouble "$OUT/lfsproof-helpers.h" && grep -q helper_tosingle "$OUT/lfsproof-helpers.h" \
  || { echo "helpers introuvables dans $F" >&2; exit 2; }

# Les ops émises (fp-impl.c.inc) contre les ops modélisées (commentaires de lfsproof.c).
I="$SRC/target/ppc/translate/fp-impl.c.inc"
ops_src() { awk -v fn="$1" '$0 ~ "^static void " fn "\\(" {on=1} on && /^}/ {exit} on' "$I" \
            | grep -o 'tcg_gen_[a-z0-9_]*' | sed 's/tcg_gen_//; s/_i64$//'; }
ops_mod() { awk -v fn="$1" '$0 ~ "^static uint(64|32)_t " fn "\\(" {on=1} on && /^}/ {exit} on' "$HERE/lfsproof.c" \
            | grep -o '/\* [a-z0-9_]*' | sed 's,/\* ,,'; }
if [ -f "$I" ] && grep -q gen_todouble_inline "$I"; then
  for p in "gen_todouble_inline model_todouble" "gen_tosingle_inline model_tosingle"; do
    set -- $p
    diff <(ops_src "$1") <(ops_mod "$2" | grep -Ev '^(ld_i64|st_i64)?$') > "$OUT/ops.diff" || {
      echo "⚠ le modèle de lfsproof.c ne suit plus $1 :" >&2; cat "$OUT/ops.diff" >&2; exit 1; }
    echo "ops de $1 = modèle ($(ops_src "$1" | wc -l | tr -d ' ') ops)"
  done
else
  echo "(arbre sans le patch 0002 : seuls les helpers sont pris, pas de contrôle des ops)"
fi

cc -O2 -Wall -I"$OUT" -o "$OUT/lfsproof" "$HERE/lfsproof.c" -lpthread
time "$OUT/lfsproof"
