#!/usr/bin/env bash
# GPL3 - Copyleft VERHILLE Arnaud
# fpnat-mut.sh [arbre-QEMU] [build] — contre-épreuve de x-fp-native
# (patches/tcg/0014, docs/tcg-g4.md §22) : construit un binaire par mutation
# de l'émetteur arm64 (tcg/aarch64/tcg-target.c.inc, fonction
# tcg_out_ppc_fp32), rangé en <arbre>/bin/mutN/, l'arbre étant rendu intact
# ensuite. Chaque binaire se joue ensuite sous x-fp-verify avec le job fptest
# (tools/tcg/fpnatab.sh, JOB=fptest BANC=) : le vérificateur doit compter des
# divergences pour chacun.
set -uo pipefail
SRC="${1:-$HOME/src/qemu-fpnat}"
B="${2:-$SRC/bfn}"
F="$SRC/tcg/aarch64/tcg-target.c.inc"
cp "$F" "$F.mut-orig"
n=0
while IFS= read -r m; do
  [ -z "$m" ] && continue
  case "$m" in \#*) echo "$m"; continue ;; esac
  n=$((n + 1))
  cp "$F.mut-orig" "$F"
  sed -i '' -e "$m" "$F"
  if cmp -s "$F" "$F.mut-orig"; then
    echo "mutation $n sans effet : $m"; continue
  fi
  ( cd "$B" && nice ninja qemu-system-ppc64 > /dev/null 2>&1 ) || { echo "mutation $n : construction ratée"; continue; }
  mkdir -p "$SRC/bin/mut$n"
  cp "$B/qemu-system-ppc64" "$SRC/bin/mut$n/qemu-system-ppc64"
  ln -sf qemu-system-ppc64 "$SRC/bin/mut$n/qemu-system-ppc"
  echo "mut$n : $m"
done <<'MUT'
# 1. frB non testé (un dénormal, un NaN, un double passe)
s/            pfp_zon(s, bad, t, xb, k1, TCG_REG_TMP1, u);/            pfp_lsl(s, t, xb, 1);/
# 2. |r| = FLT_MIN accepté (sous-dépassement avant arrondi manqué)
s/            pfp_ccmp(s, tr, k3, 2, PFP_HI);/            pfp_ccmp(s, tr, k3, 2, PFP_HS);/
# 3. fmsubs sans la négation de frB
/        case PFP_MSUB:/{n;s/pfp_fneg(s, 1, 1);/;/;}
# 4. fcmpu : -0 et +0 différents
s/        pfp_cs(s, PFP_CSNEG, ta, ta, ta, PFP_GE);/        pfp_cs(s, 0xda800000, ta, ta, ta, PFP_GE);/
# 5. porte sans RN
s/                 PFP_XX | PFP_TRAPS | (op == PFP_CMPU ? 0 : 3));/                 PFP_XX | PFP_TRAPS);/
# 6. FPRF de -0 faux
s/pfp_lsl(s, TCG_REG_TMP1, TCG_REG_TMP0, 4);/pfp_lsl(s, TCG_REG_TMP1, TCG_REG_TMP0, 3);/
MUT
cp "$F.mut-orig" "$F"
rm -f "$F.mut-orig"
( cd "$B" && nice ninja qemu-system-ppc64 > /dev/null 2>&1 ) && echo "arbre rendu intact, binaire reconstruit"
