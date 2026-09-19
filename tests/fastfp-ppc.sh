#!/usr/bin/env bash
# fastfp-ppc.sh — exécute tests/fastfp-ppc.c sous qemu-ppc dans les deux modes
# et compare, au bit près, résultats et FPSCR.
#
#   ./tests/fastfp-ppc.sh <qemu-ppc>              # exact vs rapide
#   ./tests/fastfp-ppc.sh <qemu-ppc> <qemu-ppc-de-référence>
#                                                 # + non patché vs exact
#
# Pas dans tests/run-all.sh, et c'est délibéré : il faut deux choses que le
# dépôt ne fabrique pas — un powerpc-linux-gnu-gcc et un build ppc-linux-user
# de QEMU (le dépôt ne construit que ppc-softmmu / ppc64-softmmu) :
#
#   mkdir -p ~/src/qemu-fastfp/build-user && cd $_
#   ../configure --target-list=ppc-linux-user --disable-docs --disable-werror
#   ninja
#
# C'est la preuve la plus forte disponible côté hôte : elle porte sur les
# instructions PowerPC réellement exécutées (arithmétique, fcmpu/fcmpo,
# mffs/mcrfs/mtfsb0/mtfsb1/mtfsfi), pas sur softfloat pris isolément.
# Contrat attendu : docs/flottant-rapide.md § 3.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU_PPC="${1:-}"
QEMU_REF="${2:-}"
CPU="${FASTFP_CPU:-7400}"

if [ -z "$QEMU_PPC" ] || [ ! -x "$QEMU_PPC" ]; then
  echo "usage: $0 <chemin/vers/qemu-ppc> [qemu-ppc-non-patché]" >&2; exit 2
fi
command -v powerpc-linux-gnu-gcc >/dev/null 2>&1 || {
  echo "powerpc-linux-gnu-gcc absent (apt install gcc-powerpc-linux-gnu)" >&2; exit 2; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
powerpc-linux-gnu-gcc -O1 -static -nostdlib -ffreestanding -fno-stack-protector \
  -o "$TMP/fastfp-ppc" "$ROOT/tests/fastfp-ppc.c" || exit 1

"$QEMU_PPC" -cpu "$CPU"              "$TMP/fastfp-ppc" > "$TMP/exact.txt" || exit 1
"$QEMU_PPC" -cpu "$CPU,x-fast-fp=on" "$TMP/fastfp-ppc" > "$TMP/fast.txt"  || {
  echo "✘ -cpu $CPU,x-fast-fp=on refusé : ce qemu-ppc n'a pas la propriété." >&2; exit 1; }

if [ -n "$QEMU_REF" ]; then
  "$QEMU_REF" -cpu "$CPU" "$TMP/fastfp-ppc" > "$TMP/ref.txt" || exit 1
  if cmp -s "$TMP/ref.txt" "$TMP/exact.txt"; then
    echo "✔ QEMU non patché == mode exact, octet pour octet"
  else
    echo "✘ le mode exact DIVERGE du QEMU non patché :"; diff "$TMP/ref.txt" "$TMP/exact.txt" | head -10
    exit 1
  fi
fi

python3 - "$TMP/exact.txt" "$TMP/fast.txt" <<'PY'
import sys
FX, FR, FI = 1 << 31, 1 << 18, 1 << 17
def load(p):
    ph, out = None, []
    for line in open(p):
        line = line.rstrip('\n')
        if line.startswith('=== phase'):
            ph = int(line.split()[2]); continue
        t = line.split(); out.append((ph, tuple(t[:-1]), int(t[-1], 16)))
    return out
a, b = load(sys.argv[1]), load(sys.argv[2])
if len(a) != len(b):
    print("✘ les deux exécutions n'ont pas le même nombre d'enregistrements"); sys.exit(1)
val = masked = fx_ok = fx_bad = fi = fr = 0
# En phase 0 le FPSCR est remis à 0 AVANT CHAQUE instruction, donc XX = 0 et
# l'amorçage ne doit jamais s'armer : aucune différence, FX et FI compris.
# Le bloc de manipulation du FPSCR fait exception, et c'est voulu : il pose XX
# lui-même (mtfsb1 6) au milieu de la séquence, ce qui arme légitimement
# l'amorçage pour les instructions qui suivent.
MISC = {'mffs', 'mcrfs', 'mtfsb0', 'apres0', 'mtfsb1', 'apres1', 'mtfsfi'}
phase0 = 0
per, ex = {}, []
for x, y in zip(a, b):
    ph = x[0]; per.setdefault(ph, [0, 0, 0]); per[ph][0] += 1
    if x[1] != y[1]:
        # seule dérogation admise : mcrfs du champ 0, qui recopie FX dans CR.
        ok = x[1][0] == 'mcrfs' and x[1][2] == '00' and \
             (int(x[1][-1], 16) ^ int(y[1][-1], 16)) == 0x800000
        if not ok:
            val += 1; per[ph][1] += 1
            if len(ex) < 5: ex.append(("VALEUR", x, y))
    d = x[2] ^ y[2]
    if d & ~(FX | FR | FI):
        masked += 1; per[ph][2] += 1
        if len(ex) < 5: ex.append(("FPSCR", x, y, hex(d)))
    if d & FX:
        if x[2] & FX: fx_ok += 1
        else: fx_bad += 1
    if d & FI: fi += 1
    if d & FR: fr += 1
    if ph == 0 and x[1][0] not in MISC and (d or x[1] != y[1]): phase0 += 1
print("  %d enregistrements comparés" % len(a))
print("  écarts de résultat/CR interdits      : %d" % val)
print("  FPSCR différents hors FX/FR/FI       : %d" % masked)
print("  FX posé en exact et pas en rapide    : %d (inverse : %d)" % (fx_ok, fx_bad))
print("  FI différent                         : %d" % fi)
print("  FR différent                         : %d  (doit être 0)" % fr)
print("  phase 0 (XX=0, hors bloc mtfs*)      : %d  (doit être 0)" % phase0)
for k in sorted(per):
    print("   phase %d : %d enregistrements, %d écarts de valeur, %d de FPSCR"
          % (k, per[k][0], per[k][1], per[k][2]))
for e in ex: print("   ex:", e)
bad = val or masked or fx_bad or fr or phase0
print("✘ contrat VIOLÉ" if bad else "✔ contrat respecté (§ 3 de docs/flottant-rapide.md)")
sys.exit(1 if bad else 0)
PY
