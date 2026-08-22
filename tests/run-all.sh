#!/usr/bin/env bash
# run-all.sh — harnais de non-régression de POMPPC.
#
#   ./tests/run-all.sh          # tout ce qui ne demande ni disque invité ni X
#   ./tests/run-all.sh --slow   # + les tests qui bootent réellement un invité
#
# Le dépôt est majoritairement du Bash, et les scripts SONT l'interface
# utilisateur : une faute de frappe dans un lanceur est un bug produit. Jusqu'ici
# rien ne lançait tests/qfb_smoke.py ni bridge_probe, et « nettoyage de cohérence »
# voulait dire relecture à l'œil. Ce script rend ces vérifications exécutables.
#
# Code de sortie : 0 si tout passe.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SLOW=0; [ "${1:-}" = "--slow" ] && SLOW=1

pass=0; fail=0; skip=0
ok()   { echo "  ✔ $*"; pass=$((pass+1)); }
ko()   { echo "  ✘ $*"; fail=$((fail+1)); }
noop() { echo "  – $* (ignoré)"; skip=$((skip+1)); }

echo "=== 1. syntaxe des scripts shell ==="
# Tout ce qui est exécutable et commence par un shebang bash/sh.
mapfile -t SH < <(git ls-files | while read -r f; do
  [ -f "$f" ] || continue
  head -c 2 "$f" 2>/dev/null | grep -q '#!' || continue
  head -1 "$f" | grep -qE 'bash|/bin/sh' && echo "$f"
done)
for f in "${SH[@]}"; do
  if bash -n "$f" 2>/dev/null; then ok "bash -n $f"; else ko "bash -n $f"; fi
done

echo
echo "=== 2. shellcheck (si installé) ==="
if command -v shellcheck >/dev/null 2>&1; then
  for f in "${SH[@]}"; do
    # SC1091 : les 'source' dynamiques (config.env, caps.sh) ne sont pas suivis.
    if shellcheck -e SC1091 -S warning "$f" >/dev/null 2>&1; then ok "shellcheck $f"
    else ko "shellcheck $f  ($(shellcheck -e SC1091 -S warning -f gcc "$f" 2>/dev/null | head -1))"; fi
  done
else
  noop "shellcheck absent (sudo apt-get install -y shellcheck)"
fi

echo
echo "=== 3. syntaxe Python ==="
for f in $(git ls-files '*.py'); do
  if python3 -m py_compile "$f" 2>/dev/null; then ok "py_compile $f"; else ko "py_compile $f"; fi
done
rm -rf tests/__pycache__ scripts/__pycache__ 2>/dev/null

echo
echo "=== 4. cohérence doc ↔ binaire ==="
# La classe de bug la plus coûteuse du projet : le README décrivait un binaire
# (avec Screamer, sans slirp) qui ne correspondait plus à celui qui tournait.
# Comparer la doc au dépôt ne l'attrapait pas ; il faut interroger le binaire.
source config.env
source scripts/caps.sh
if [ -x "$QEMU_BIN" ] || command -v "$QEMU_BIN" >/dev/null 2>&1; then
  echo "  binaire : $QEMU_BIN"
  # screamer : sondage MACHINE. Le type peut être compilé sans que le macio
  # l'instancie — cas réellement rencontré, et le sondage par type disait « oui »
  # sur un binaire muet.
  # Code 2 = sondage impossible (≠ capacité absente) : on le signale comme
  # « ignoré » plutôt que comme un échec, sinon un hôte saturé produit un faux
  # rouge — le pendant du faux vert que tout ce dispositif existe pour éviter.
  cap() { # cap <étiquette> <prédicat…>
    "${@:2}"; local rc=$?
    case $rc in
      0) ok "$1" ;;
      2) noop "$1 — sondage impossible" ;;
      *) ko "$1 ABSENT — ./scripts/build_qemu_qfb.sh" ;;
    esac
  }
  cap "screamer instancié dans $MACHINE" qemu_machine_has "$QEMU_BIN" "$MACHINE" screamer
  cap "device qfb-pci"                   qemu_has_device   "$QEMU_BIN" qfb-pci
  cap "slirp"                            qemu_has_netdev   "$QEMU_BIN" user
  cap "backend audio pa"                 qemu_has_audiodev "$QEMU_BIN" pa
else
  noop "QEMU introuvable ($QEMU_BIN)"
fi

echo
echo "=== 5. le kext et le device partagent le même contrat de registres ==="
# qfb_regs.h (invité) doit refléter hw/display/qfb-pci.c (hôte) au bit près :
# une divergence donne un écran corrompu très difficile à diagnostiquer.
python3 - <<'PY' && ok "registres qfb alignés" || ko "registres qfb DÉSALIGNÉS"
import re, sys
host = open('patches/qfb/qfb-pci.c').read()
guest = open('kext/POMPPCQFB/qfb_regs.h').read()
def regs(txt):
    return {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r'#define\s+(QFB_(?:VERSION|MODE_\w+|PAL_\w+|LUT_\w+|IRQ|IRQ_MASK|CUSTOM_\w+))\s+0x([0-9A-Fa-f]+)', txt)}
h, g = regs(host), regs(guest)
common = set(h) & set(g)
bad = [k for k in sorted(common) if h[k] != g[k]]
if not common:
    print("   aucun registre commun trouvé — parsing cassé"); sys.exit(1)
for k in bad:
    print("   %-20s hôte=0x%02x  invité=0x%02x" % (k, h[k], g[k]))
print("   %d registres comparés" % len(common))
sys.exit(1 if bad else 0)
PY

echo
echo "=== 6. device QFB de bout en bout (Open Firmware, sans invité) ==="
if [ "$SLOW" = 1 ]; then
  if python3 tests/qfb_smoke.py; then ok "qfb_smoke.py"; else ko "qfb_smoke.py"; fi
else
  noop "qfb_smoke.py (--slow pour l'exécuter, ~30 s)"
fi

echo
echo "=== 7. pont D-Bus de bout en bout ==="
if [ "$SLOW" = 1 ]; then
  if [ -x frontend/build/bridge_probe ]; then
    if frontend/build/bridge_probe "$ROOT/run_tiger.sh" 40 >/dev/null 2>&1; then
      ok "bridge_probe"; else ko "bridge_probe"; fi
  else
    noop "bridge_probe non construit (cmake -S frontend -B frontend/build && cmake --build frontend/build)"
  fi
else
  noop "bridge_probe (--slow pour l'exécuter, ~40 s)"
fi

echo
echo "──────────────────────────────────────────"
printf "  %d OK, %d échec(s), %d ignoré(s)\n" "$pass" "$fail" "$skip"
exit $(( fail > 0 ? 1 : 0 ))
