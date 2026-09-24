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
# (boucle while plutôt que mapfile : le bash 3.2 de macOS n'a pas mapfile.)
SH=()
while read -r f; do SH+=("$f"); done < <(git ls-files | while read -r f; do
  [ -f "$f" ] || continue
  head -c 2 "$f" 2>/dev/null | grep -q '#!' || continue
  head -1 "$f" | grep -qE 'bash|/bin/sh' && echo "$f"
done)
for f in ${SH[@]+"${SH[@]}"}; do
  if bash -n "$f" 2>/dev/null; then ok "bash -n $f"; else ko "bash -n $f"; fi
done

echo
echo "=== 2. shellcheck (si installé) ==="
if command -v shellcheck >/dev/null 2>&1; then
  for f in ${SH[@]+"${SH[@]}"}; do
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
if python3 tests/frame_report_test.py; then ok "frame-time report"; else ko "frame-time report"; fi
if python3 tests/flyby_report_test.py; then ok "fixed-step flyby report"; else ko "fixed-step flyby report"; fi
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
  cap "device qgpu-pci"                  qemu_has_device   "$QEMU_BIN" qgpu-pci
  cap "slirp"                            qemu_has_netdev   "$QEMU_BIN" user
  # SMP : sondé sur le binaire 64 bits, celui que run_tiger.sh lance dès
  # SMP >= 2 — et sur la machine du lanceur, car -smp 2 exige via=pmu. C'était
  # la seule capacité annoncée par le README que rien ne vérifiait.
  if [ -x "${QEMU_BIN}64" ]; then
    cap "-smp 2 sur ${MACHINE} (ppc64)"  qemu_machine_smp_ok "${QEMU_BIN}64" "$MACHINE" 2
  else
    noop "-smp 2 — ${QEMU_BIN}64 absent"
  fi
  # PulseAudio est le backend de référence sur Linux ; sur macOS c'est coreaudio.
  case "$(uname -s)" in
    Darwin) cap "backend audio coreaudio"    qemu_has_audiodev "$QEMU_BIN" coreaudio ;;
    *)      cap "backend audio pa"           qemu_has_audiodev "$QEMU_BIN" pa ;;
  esac
  # Flottant rapide : la propriété de CPU x-fast-fp (patches/fastfp/). Son
  # absence n'est PAS un échec — elle veut dire que le binaire de référence n'a
  # pas encore été reconstruit depuis que le patch existe, et run_tiger.sh le
  # sait (FASTFP=1 prévient et repart en flottant exact). Ce qui serait un bug,
  # c'est que le lanceur l'annonce sans que le binaire l'ait : c'est justement
  # ce que qemu_cpu_has_fastfp interdit, ici comme là-bas.
  qemu_cpu_has_fastfp "$QEMU_BIN" "$MACHINE" "$CPU"; ffp_rc=$?
  case $ffp_rc in
    0) ok   "propriété CPU x-fast-fp (FASTFP=1 utilisable)" ;;
    2) noop "x-fast-fp — sondage impossible" ;;
    *) noop "x-fast-fp absent du binaire de référence (./scripts/build_qemu_qfb.sh)" ;;
  esac
else
  noop "QEMU introuvable ($QEMU_BIN)"
fi

echo
echo "=== 4 bis. flottant rapide : test différentiel hôte (softfloat) ==="
# tests/fastfp-diff.c compare, pour des millions d'opérandes, le MODE EXACT et
# le MODE RAPIDE de l'objet softfloat que le binaire embarque réellement. Il se
# lie à fpu_softfloat.c.o de l'arbre QEMU : sans cet arbre (ou sans le patch),
# il n'y a rien à tester, et c'est « ignoré », pas un échec.
FFP_SRC="${QEMU_SRC:-$(dirname "$(dirname "$QEMU_BIN")")}"
FFP_OBJ="$FFP_SRC/build/libqemu-ppc-softmmu.a.p/fpu_softfloat.c.o"
if ! grep -qs "no_hardfloat" "$FFP_SRC/include/fpu/softfloat-types.h"; then
  noop "arbre QEMU sans le patch fastfp ($FFP_SRC)"
elif [ ! -f "$FFP_OBJ" ] || [ ! -f "$FFP_SRC/build/compile_commands.json" ]; then
  noop "objet softfloat non construit ($FFP_OBJ)"
elif ! command -v cc >/dev/null 2>&1 || ! command -v pkg-config >/dev/null 2>&1; then
  noop "pas de compilateur C ou de pkg-config"
else
  # Les -I/-D de l'objet softfloat, repris tels quels : les en-têtes de QEMU ne
  # se compilent qu'avec eux, et la disposition de float_status doit être LA
  # MÊME des deux côtés du lien.
  FFP_FLAGS="$(python3 - "$FFP_SRC/build/compile_commands.json" <<'PY'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e['output'].endswith('libqemu-ppc-softmmu.a.p/fpu_softfloat.c.o'):
        t = shlex.split(e['command']); out = []; i = 0
        while i < len(t):
            if t[i].startswith(('-I', '-iquote', '-isystem', '-D', '-include')):
                out.append(t[i])
                if t[i] in ('-iquote', '-isystem', '-include', '-I', '-D'):
                    i += 1; out.append(t[i])
            i += 1
        print(' '.join(shlex.quote(x) for x in out)); break
PY
)"
  FFP_BIN="${TMPDIR:-/tmp}/fastfp-diff.$$"
  # Compilé DEPUIS le répertoire de build : plusieurs -I/-iquote de QEMU sont
  # relatifs (« -I. », « -I.. », « -iquote . »), et ailleurs ils ne désignent
  # plus rien — le lien passe quand même, mais les en-têtes ne se trouvent pas.
  if [ -z "$FFP_FLAGS" ]; then
    noop "fpu_softfloat.c.o absent de compile_commands.json"
  elif ( cd "$FFP_SRC/build" &&
         eval cc -O2 -Wall -fno-strict-aliasing "$FFP_FLAGS" \
              "$ROOT/tests/fastfp-diff.c" \
              "$FFP_OBJ" -lm "$(pkg-config --libs glib-2.0)" -o "$FFP_BIN" ) 2>/dev/null; then
    if FFP_OUT="$("$FFP_BIN" 60000 2>&1)"; then
      ok "fastfp-diff — $(echo "$FFP_OUT" | sed -n 's/^ *\([0-9]* cas comparés.*\)$/\1/p')"
    else
      ko "fastfp-diff : $(echo "$FFP_OUT" | grep '✘' | head -1)"
    fi
  else
    ko "tests/fastfp-diff.c ne compile pas contre $FFP_SRC"
  fi
  rm -f "$FFP_BIN"
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

# Les offsets ne sont que la moitié du contrat. La GÉOMÉTRIE — taille de VRAM,
# résolution maximale, et surtout le PAS DE LIGNE — est l'autre moitié, et
# c'est la seule dont un désaccord donne le fameux « bureau strié » : le kext
# calcule rowBytes avec sa macro, le device parcourt la VRAM avec sa fonction,
# et un octet d'écart décale chaque ligne. Côté hôte c'est une fonction C,
# côté invité une macro : on ne peut pas les comparer texte à texte, on les
# compile toutes les deux et on les évalue sur des largeurs représentatives
# (dont les impaires et les non multiples de 4, où l'arrondi se joue).
python3 - <<'PY' && ok "géométrie qfb alignée (VRAM, max, pas de ligne)" || ko "géométrie qfb DÉSALIGNÉE"
import os, re, shutil, subprocess, sys, tempfile

host = open('patches/qfb/qfb-pci.c').read()
guest = open('kext/POMPPCQFB/qfb_regs.h').read()
bad = 0

# --- constantes : mêmes valeurs, écritures différentes (32 * MiB vs 32UL*1024UL*1024UL)
ENV = {'MiB': 1 << 20, 'KiB': 1 << 10, 'GiB': 1 << 30}
def const(txt, name):
    m = re.search(r'#define\s+%s\s+([^\n]+)' % name, txt)
    if not m:
        return None
    e = m.group(1).split('/*')[0].split('//')[0].strip()
    e = re.sub(r'\b(\d+)[uUlL]+', r'\1', e)          # 32UL -> 32
    try:
        return int(eval(e, {'__builtins__': {}}, ENV))
    except Exception:
        return None

for name in ('QFB_VRAM_SIZE', 'QFB_MAX_WIDTH', 'QFB_MAX_HEIGHT'):
    h, g = const(host, name), const(guest, name)
    if h is None or g is None:
        print("   %-16s hôte=%s invité=%s — introuvable ou non évaluable"
              % (name, h, g)); bad += 1
    elif h != g:
        print("   %-16s hôte=%d  invité=%d" % (name, h, g)); bad += 1

# --- pas de ligne : fonction (hôte) contre macro (invité)
fn = re.search(r'^static uint32_t qfb_calculate_stride\(.*?^\}', host, re.S | re.M)
mac = re.search(r'^#define\s+QFB_STRIDE\([^)]*\)[^\n]*$', guest, re.M)
if not fn or not mac:
    print("   qfb_calculate_stride / QFB_STRIDE introuvable — parsing cassé")
    sys.exit(1)
cc = os.environ.get('CC') or shutil.which('cc')
if not cc:
    print("   (pas de compilateur C : pas de ligne non comparé)")
    sys.exit(1 if bad else 0)
src = """#include <stdio.h>
#include <stdint.h>
@HOST@
@GUEST@
int main(void) {
    static const unsigned W[] = {1, 3, 7, 13, 17, 21, 33, 100, 512, 640, 800,
                                 1023, 1024, 1152, 1280, 1281, 1366, 1440,
                                 1600, 1920, 3840};
    static const unsigned D[] = {8, 16, 24, 32};
    unsigned i, j, bad = 0;
    for (i = 0; i < sizeof(W) / sizeof(W[0]); i++)
        for (j = 0; j < sizeof(D) / sizeof(D[0]); j++) {
            unsigned long long h = qfb_calculate_stride(W[i], D[j]);
            unsigned long long g = QFB_STRIDE(W[i], D[j]);
            if (h != g) {
                printf("   stride %ux%u bpp : hote=%llu invite=%llu\\n",
                       W[i], D[j], h, g);
                bad++;
            }
        }
    printf("   %u largeurs x %u profondeurs comparees\\n",
           (unsigned)(sizeof(W) / sizeof(W[0])), (unsigned)(sizeof(D) / sizeof(D[0])));
    return bad ? 1 : 0;
}
""".replace('@HOST@', fn.group(0)).replace('@GUEST@', mac.group(0))
sys.stdout.flush()          # le binaire écrit sur le même stdout : garder l'ordre
d = tempfile.mkdtemp()
try:
    c, b = os.path.join(d, 's.c'), os.path.join(d, 's')
    open(c, 'w').write(src)
    if subprocess.call([cc, '-O1', '-o', b, c],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL):
        print("   les deux définitions du pas de ligne ne compilent pas ensemble")
        bad += 1
    elif subprocess.call([b]):
        bad += 1
finally:
    shutil.rmtree(d, ignore_errors=True)
sys.exit(1 if bad else 0)
PY

echo
echo "=== 5 bis. contrat qgpu : une seule source de vérité, copiée à l'identique ==="
# Le protocole du GPU paravirtuel n'est PAS relu par regex : le même fichier
# est copié tel quel côté hôte et côté invité, et doit le rester au bit près.
# v19 (chantier A1) : le kext ne reçoit que l'ABI de transport (qgpu_abi.h) ;
# la sémantique (qgpu_proto.h) reste dans patches/qgpu/, partagée par le
# device, ses tests et le plugin, qui la copient à la construction.
if cmp -s patches/qgpu/qgpu_abi.h kext/POMPPCGPU/qgpu_abi.h; then
  ok "qgpu_abi.h identique hôte/kext"
else
  ko "qgpu_abi.h DIVERGE entre patches/qgpu/ et kext/POMPPCGPU/ (cp l'un sur l'autre)"
fi
# Le kext ne doit connaître AUCUN symbole de la sémantique : ni opcode, ni
# limite d'objets, ni plage d'identifiants, ni version du protocole. C'est la
# preuve mécanique que changer qgpu_proto.h ne demande pas de reconstruire le
# kext (le 23/09/2026, un kext d'un autre en-tête a coûté deux plantages).
if [ -e kext/POMPPCGPU/qgpu_proto.h ]; then
  ko "kext/POMPPCGPU/qgpu_proto.h existe : le kext ne doit voir que qgpu_abi.h"
else
  ok "le kext n'a pas de copie de qgpu_proto.h"
fi
# (Les commentaires ont le droit d'en parler : on les retire avant de chercher.)
LEAK=$(python3 - <<'PY'
import re, sys
strip = lambda t: re.sub(r'/\*.*?\*/', '', t, flags=re.S)
sym = re.compile(r'QGPU_(OP|SK|MAX|CLIENT|CLASS|PROTO|LEN|FMT|VF)_|QGPU_CMD_HDR|qgpu_proto\.h')
for f in ('kext/POMPPCGPU/POMPPCGPU.cpp', 'kext/POMPPCGPU/POMPPCGPU.h', 'kext/POMPPCGPU/qgpu_abi.h'):
    for line in strip(open(f).read()).splitlines():
        line = line.split('//')[0]
        if sym.search(line):
            print("%s : %s" % (f, line.strip()[:70])); sys.exit(0)
PY
)
if [ -z "$LEAK" ]; then
  ok "le kext ne nomme aucun symbole de qgpu_proto.h (hors commentaires)"
else
  ko "le kext nomme la sémantique : $LEAK"
fi

echo "=== 5 ter. cœur qgpu + backends, en natif sur l'hôte ==="
# Compile et exécute tests/qgpu_core_test.c : même flux que qgpu_smoke.py et
# que le programme invité, sans QEMU. Le backend GL est testé s'il démarre ici.
if command -v cc >/dev/null 2>&1; then
  QGPU_BIN="${TMPDIR:-/tmp}/qgpu_core_test.$$"
  case "$(uname -s)" in
    Darwin) QGPU_LIBS=(-framework OpenGL) ;;
    *)      QGPU_LIBS=(); pkg-config --exists egl gl 2>/dev/null && QGPU_LIBS=($(pkg-config --libs egl gl)) ;;
  esac
  if cc -std=gnu11 -O1 -pthread -I patches/qgpu tests/qgpu_core_test.c \
        patches/qgpu/qgpu-core.c patches/qgpu/qgpu-soft.c patches/qgpu/qgpu-gl.c \
        ${QGPU_LIBS[@]+"${QGPU_LIBS[@]}"} -lm -o "$QGPU_BIN" 2>/dev/null; then
    if "$QGPU_BIN" >/dev/null 2>&1; then ok "qgpu_core_test (soft + gl si dispo)"
    else ko "qgpu_core_test ($("$QGPU_BIN" 2>&1 | grep FAIL | head -1))"; fi
  else
    ko "qgpu_core_test ne compile pas"
  fi
  rm -f "$QGPU_BIN"
  # Banc d'essai des BACKENDS (soft vs gl sur le même flux), s'il existe :
  # mêmes drapeaux que qgpu_core_test, et rien à signaler tant qu'il n'est pas
  # écrit — un test absent ne doit ni rougir ni faire croire qu'il est passé.
  if [ -f tests/qgpu_backend_test.c ]; then
    QGPU_BE_BIN="${TMPDIR:-/tmp}/qgpu_backend_test.$$"
    if cc -std=gnu11 -O1 -pthread -I patches/qgpu tests/qgpu_backend_test.c \
          patches/qgpu/qgpu-core.c patches/qgpu/qgpu-soft.c patches/qgpu/qgpu-gl.c \
          ${QGPU_LIBS[@]+"${QGPU_LIBS[@]}"} -lm -o "$QGPU_BE_BIN" 2>/dev/null; then
      if "$QGPU_BE_BIN" >/dev/null 2>&1; then ok "qgpu_backend_test"
      else ko "qgpu_backend_test ($("$QGPU_BE_BIN" 2>&1 | grep -i 'fail\|✘' | head -1))"; fi
    else
      ko "qgpu_backend_test ne compile pas"
    fi
    rm -f "$QGPU_BE_BIN"
  fi
else
  noop "pas de compilateur C"
fi

echo
echo "=== 5 quater. plugin OpenGL : sources générées à jour ==="
# gld_tramp.s est produit par tools/gld/gen_tramp.py et livré tel quel à
# l'invité (qui n'a pas Python) : il ne doit jamais diverger du générateur.
if python3 tools/gld/gen_tramp.py | cmp -s - guest/gldriver/gld_tramp.s; then
  ok "gld_tramp.s = sortie de gen_tramp.py"
else
  ko "gld_tramp.s périmé (python3 tools/gld/gen_tramp.py > guest/gldriver/gld_tramp.s)"
fi
# La liste des points d'entrée de pomppc_gld.h (X-macro) doit suivre le même
# ordre que le générateur : c'est l'ordre de la table de GLEngine.
if python3 - <<'PY'
import re, subprocess, sys
gen = subprocess.check_output(["python3", "tools/gld/gen_tramp.py", "--names"]).decode().split()
hdr = re.findall(r"X\((\w+)\)", open("guest/gldriver/pomppc_gld.h").read().split("enum")[0])
sys.exit(0 if gen == hdr else 1)
PY
then ok "GLD_LIST alignée sur le générateur"; else ko "GLD_LIST de pomppc_gld.h désalignée"; fi

echo
echo "=== 6. device QFB de bout en bout (Open Firmware, sans invité) ==="
if [ "$SLOW" = 1 ]; then
  if python3 tests/qfb_smoke.py; then ok "qfb_smoke.py"; else ko "qfb_smoke.py"; fi
else
  noop "qfb_smoke.py (--slow pour l'exécuter, ~30 s)"
fi

echo
echo "=== 6 bis. device qgpu de bout en bout (Open Firmware, sans invité) ==="
if [ "$SLOW" = 1 ]; then
  if python3 tests/qgpu_smoke.py; then ok "qgpu_smoke.py"; else ko "qgpu_smoke.py"; fi
else
  noop "qgpu_smoke.py (--slow pour l'exécuter, ~3 min)"
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
