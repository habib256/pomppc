#!/bin/bash
# GPL3 - Copyleft VERHILLE Arnaud
# ab-attente.sh — kit d'A/B en VM du chantier « attente de l'invité sur l'hôte »
# (30/09/2026, docs/backend-gl-attente.md). Trois épreuves, sur la VM QUOTIDIENNE
# (libre : aucun autre intervenant), chacune par tools/tcg/matab.sh (A/B
# ENTRELACÉ, QEMU relancé à chaque partie, matrice sans vidage) :
#
#   g8      QGPU_PRESENT_SKIP=8 (7 relectures de présentation sur 8 sautées ;
#           image figée, cellule rouge) contre la référence, sur Nexuiz GLSL
#           fenêtre et Colin McRae plein écran. Critère : si l'attente médiane
#           (wait_ms par image affichée) ne baisse pas d'au moins 1 ms/image,
#           G8 (PBO en rotation pour SURF_PRESENT) est clos « mesuré, rien à
#           gagner ». Prévu par le rejeu : < 0,2 ms (§3 de la note).
#   flush   QGPU_GL_FLUSH=1 (glFlush en fin de soumission) contre la référence,
#           sur Nexuiz GLSL et ARB fenêtre, DOOM 3 fenêtre, Colin McRae.
#           Prévu par le rejeu : attente −0,1 à −0,3 ms/image (Nexuiz), ms/image
#           dans le bruit. Seul, il ne vaut pas 2 % : c'est le prérequis de la
#           soumission anticipée des requêtes par le plugin (§4).
#   matrice un tour complet de la matrice sous QGPU_GL_FLUSH=1 (16 cellules,
#           images comprises) : condition pour l'allumer par défaut.
#
#   tools/matrice/ab-attente.sh [N] [épreuves]
#     N         : parties par mode et par cellule (défaut 3 ; 6 pour conclure)
#     épreuves  : défaut « g8 flush » ; « matrice » à part (≈ 2 h)
#   QEMU_BIN    : défaut ~/src/qemu-backend/build/qemu-system-ppc (« 64 » ajouté
#                 par run_tiger.sh en SMP) — un QEMU construit avec les
#                 patches/qgpu/ de cette branche (mode d'emploi : note §6).
#
# Sorties : <dépôt principal>/bench/tcg/ab/attente-<horodatage>-<épreuve>-<cellule>/
# (une campagne matab.sh par cellule), bilan de l'attente par
# tools/re/attente.py dans …/attente-<horodatage>-bilan.txt ; la matrice dans
# bench/matrice/attente-<horodatage>-flush/. La VM est rendue à la fin sur le
# QEMU de référence (matab.sh --restore). JAMAIS de suppression de .run/tiger.lock.
set -u
R=/Users/mercure/src/pomppc
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
N="${1:-3}"
EPREUVES="${2:-g8 flush}"
export QEMU_BIN="${QEMU_BIN:-/Users/mercure/src/qemu-backend/build/qemu-system-ppc}"
H=$(date +%Y%m%d-%H%M)
BILAN=$R/bench/tcg/ab/attente-$H-bilan.txt
mkdir -p "$R/bench/tcg/ab"

[ -x "$QEMU_BIN" ] || { echo "QEMU_BIN introuvable : $QEMU_BIN (note §6)"; exit 1; }
echo "ab-attente $H : QEMU_BIN=$QEMU_BIN, $N partie(s) par mode, épreuves : $EPREUVES" | tee -a "$BILAN"

TS=$WT/tools/guest/tssh.sh
LOCK=$R/.run/tiger.lock
. "$WT/scripts/hostcompat.sh"
mon() { local m; m="$(cat "$R/.run/tiger.mon" 2>/dev/null)"; echo "${m:-$R/.run/mon.sock}"; }
# comme tools/tcg/matab.sh : on attend que le verrou se libère, on ne le supprime pas
stop_vm() {
  if host_locked "$LOCK"; then
    $TS "echo tiger974 | sudo -S shutdown -h now" >/dev/null 2>&1
    host_wait_unlocked "$LOCK" 150 2>/dev/null && return 0
    python3 "$WT/tools/tcg/hmp.py" "$(mon)" quit >/dev/null 2>&1
    host_wait_unlocked "$LOCK" 60 || { echo "verrou toujours tenu : abandon"; exit 1; }
  fi
}
wait_ssh() {
  for _ in $(seq 1 60); do $TS true >/dev/null 2>&1 && return 0; sleep 5; done
  return 1
}

campagne() { # campagne ÉPREUVE JEU MODE "B:VAR=v"
  local c="attente-$H-$1-$2-$3"
  bash "$WT/tools/tcg/matab.sh" "$c" "$N" "ref:" "$4" "$2" "$3"
  python3 "$WT/tools/re/attente.py" "$R/bench/tcg/ab/$c" | sed "s/^/[$1 $2-$3] /" | tee -a "$BILAN"
}

for e in $EPREUVES; do
  case $e in
    g8)
      campagne g8 nxg fen "g8:QGPU_PRESENT_SKIP=8"
      campagne g8 cmr pe "g8:QGPU_PRESENT_SKIP=8"
      ;;
    flush)
      campagne flush nxg fen "flush:QGPU_GL_FLUSH=1"
      campagne flush nx fen "flush:QGPU_GL_FLUSH=1"
      campagne flush d3 fen "flush:QGPU_GL_FLUSH=1"
      campagne flush cmr pe "flush:QGPU_GL_FLUSH=1"
      ;;
    matrice)
      # VM relancée sur QEMU_BIN avec QGPU_GL_FLUSH=1 ; la matrice relance QEMU
      # elle-même si l'invité gèle : la variable reste dans son environnement
      OUT=$R/bench/matrice/attente-$H-flush
      mkdir -p "$OUT"
      stop_vm
      ( cd "$R" && env POMPPC_FRONTEND=native QGPU_GL_FLUSH=1 nohup ./run_tiger.sh \
          > "$OUT/run_tiger.log" 2>&1 & )
      sleep 20
      wait_ssh || { python3 "$WT/tools/tcg/hmp.py" "$(mon)" system_reset >/dev/null 2>&1; wait_ssh; }
      sleep 60                                  # bureau au repos
      grep -h "QGPU_GL_FLUSH" "$OUT/run_tiger.log" | head -1 | tee -a "$BILAN"
      QGPU_GL_FLUSH=1 python3 "$WT/tools/matrice/matrice.py" --sortie "$OUT" \
          2>&1 | tee "$OUT/matrice.log"
      python3 "$WT/tools/re/attente.py" "$OUT" | sed "s/^/[matrice flush] /" | tee -a "$BILAN"
      ;;
    *) echo "épreuve inconnue : $e" ;;
  esac
done
env -u QEMU_BIN bash "$WT/tools/tcg/matab.sh" --restore      # QEMU de référence
echo "bilan : $BILAN"
