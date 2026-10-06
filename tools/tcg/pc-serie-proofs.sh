#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# pc-serie-proofs.sh <arbre-QEMU> <dossier> — les preuves HÔTE de la série
# 0025-0029 et des correctifs vnmsubfp sur un PC x86-64 (docs/tcg-g4.md §30),
# un journal par preuve dans <dossier>. Aucune VM n'est lancée, sauf les QEMU
# arrêtés (-S, sans disque) de jitcheck.py.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${1:?arbre QEMU construit requis}"
OUT="${2:?dossier de journaux requis}"
N="${VFPN:-20000000}"
mkdir -p "$OUT"
exec > >(tee "$OUT/bilan.txt") 2>&1
echo "# $(date -Iseconds) $(uname -m) $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2)"
sha256sum "$SRC/build/qemu-system-ppc" "$SRC/build/qemu-system-ppc64" 2>/dev/null
rc=0
step() { # step <nom> <commande…>
  local name="$1"; shift
  echo "=== $name"
  if "$@" > "$OUT/$name.log" 2>&1; then
    echo "    OK"; else echo "    ÉCHEC (voir $OUT/$name.log)"; rc=1; fi
  grep -v '^real\|^user\|^sys\|^$' "$OUT/$name.log" | tail -5 | sed 's/^/    /'
}
step lmwvectorproof      bash "$HERE/lmwvectorproof.sh" "$SRC"
step lmwvectorproof-mut  bash "$HERE/lmwvectorproof-x86-mut.sh" "$SRC"
step vfpproof-helpers    bash "$HERE/vfpproof.sh" "$SRC" "$N"
step vfpproof-native     env VFPPROOF_NATIVE=1 bash "$HERE/vfpproof.sh" "$SRC" "$N"
step vfpproof-x86-mut    bash "$HERE/vfpproof-x86-mut.sh" "$SRC"
step vfpproof-native-mut bash "$HERE/vfpproof-native-mut.sh" "$SRC"
step jcwordproof         python3 "$HERE/jcwordproof.py" "$SRC"
# Sous Linux, jitcheck-near et les deux split-wx échouent PAR CONSTRUCTION
# (noyau à ~30 Tio du texte ; split-wx par memfd, que rien ne place) : ce sont
# des relevés, pas des preuves, et ils ne comptent pas dans le code de sortie.
step_obs() { local r=$rc; step "$@"; rc=$r; }
step_obs jitcheck-near   python3 "$HERE/jitcheck.py" "$SRC/build/qemu-system-ppc" --runs 5
step_obs jitcheck-splitwx python3 "$HERE/jitcheck.py" "$SRC/build/qemu-system-ppc" --runs 20 --split-wx
step jitcheck-rel32      python3 "$HERE/jitcheck.py" "$SRC/build/qemu-system-ppc" --runs 5 --rel32
step_obs jitcheck-rel32-splitwx python3 "$HERE/jitcheck.py" "$SRC/build/qemu-system-ppc" --runs 20 --rel32 --split-wx
exit $rc
