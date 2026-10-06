#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# pgo-train.sh — entraîne le profil PGO du binaire RAPIDE du PC (build-fast/) et le
# reconstruit avec (docs/binaire-rapide-x86.md). Trois étapes :
#
#   gen    build-fast/ instrumenté (QEMU_FAST=only QEMU_FAST_PGO=gen), le profil
#          précédent archivé en <PGO_DIR>.<date> ;
#   jouer  charge MIXTE sur la VM QUOTIDIENNE par tools/tcg/matab.sh (un bras
#          « train » sur le binaire instrumenté, matrice sans vidage) : par défaut
#          Marble Blast, Zenerchi, UT2004 et DOOM 3 en fenêtre, une partie (≈ 45 min :
#          le binaire instrumenté est lent, DOOM 3 seul a pris 25 min le 03/10) ;
#          les .gcda s'écrivent quand QEMU quitte (matab arrête l'invité proprement) ;
#   use    build-fast/ reconstruit avec le profil (QEMU_FAST_PGO=use).
#
#   tools/tcg/pgo-train.sh --dry-run                 # montre tout, ne lance rien
#   tools/tcg/pgo-train.sh                           # gen, jouer, use
#   tools/tcg/pgo-train.sh --parties 2 --jeux d3,ut  # plus long, autre mélange
#   tools/tcg/pgo-train.sh --etapes use              # reconstruire seulement
#   tools/tcg/pgo-train.sh --importer ~/src/qemu-d3tcg/pgo-data3   # profil d'un arbre
#                       voisin, .gcda renommés pour <arbre>/build-fast (--de <préfixe>
#                       si la devinette se trompe), puis --etapes use
#
# Environnement : QEMU_SRC (défaut ~/src/qemu), PGO_DIR (défaut $QEMU_SRC/pgo-fast),
# PGO_MEM (plafond mémoire des constructions, défaut 20G ; swap 1G), LTO_JOBS (6).
# Les constructions tournent en nice -n 10 sous systemd-run --user --scope -p
# MemoryMax : deux liaisons LTO+PGO simultanées ont gelé le PC le 03/10 ; sous
# plafond, le noyau tue la construction au lieu de geler la machine.
#
# Hôte AU REPOS pour l'étape jouer (pas d'autre construction, pas d'autre QEMU) :
# le profil ne dépend pas de la vitesse, mais la matrice abandonne une scène
# trop lente, et une scène non atteinte joue moins de code utile.
set -euo pipefail
WT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# dépôt principal (disks/, .run/, bench/), même depuis un worktree
R="$(git -C "$WT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's|/\.git$||')"
R="${R:-$WT}"
SRC="${QEMU_SRC:-$HOME/src/qemu}"
PGO_DIR="${PGO_DIR:-$SRC/pgo-fast}"
MEM="${PGO_MEM:-20G}"
DRY=""; FORCE=""; PARTIES=1; JEUX="mb,zen,ut,d3"; MODE=fen; ETAPES=""
IMPORT=""; DE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run)  DRY=1 ;;
    --force)    FORCE=1 ;;
    --parties)  PARTIES="$2"; shift ;;
    --jeux)     JEUX="$2"; shift ;;
    --mode)     MODE="$2"; shift ;;
    --etapes)   ETAPES="$2"; shift ;;
    --importer) IMPORT="$2"; shift ;;
    --de)       DE="$2"; shift ;;
    -h|--help)  sed -n '4,32p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "option inconnue : $1 (--help)" >&2; exit 2 ;;
  esac
  shift
done
# Sans --etapes : tout l'entraînement ; avec --importer, l'import SEUL (le profil
# importé remplace la partie ; --etapes use pour reconstruire dans la foulée).
[ -n "$ETAPES" ] || [ -n "$IMPORT" ] || ETAPES="gen,jouer,use"
STAMP="$(date +%Y%m%d-%H%M)"
LOGS="$R/bench/vitesse/pgo-fast/$STAMP"
FAST="$SRC/build-fast/qemu-system-ppc"

say() { echo "▶ $*"; }
die() { echo "⚠ $*" >&2; exit 1; }
run() { # run <cmd…> : exécute, ou montre seulement en --dry-run
  if [ -n "$DRY" ]; then printf '  +'; printf ' %q' "$@"; echo; else "$@"; fi
}
# Même empreinte que le relevé de construction (scripts/qemu_fast.sh).
source "$WT/scripts/qemu_fast.sh"
serie_hash() { pomppc_serie_hash "$WT/patches"; }
mangle() { printf '%s#' "$(printf '%s' "$1" | tr / '#')"; }
ngcda() { find "$PGO_DIR" -maxdepth 1 -name '*.gcda' 2>/dev/null | wc -l; }
archive_profile() {
  if [ "$(ngcda)" -gt 0 ] || [ -f "$PGO_DIR/pomppc-profil.txt" ]; then
    say "profil précédent archivé : $PGO_DIR.$STAMP"
    run mv "$PGO_DIR" "$PGO_DIR.$STAMP"
  fi
  run mkdir -p "$PGO_DIR"
}

# --- contrôles préalables ---
[ "$(uname -s):$(uname -m)" = Linux:x86_64 ] || die "binaire rapide du PC : Linux x86-64 seulement"
[ -e "$SRC/.git" ] || [ -f "$SRC/VERSION" ] || die "$SRC n'est pas un arbre QEMU (QEMU_SRC=…)"
[ -x "$WT/scripts/build_qemu_qfb.sh" ] || die "$WT/scripts/build_qemu_qfb.sh introuvable"
for e in ${ETAPES//,/ }; do
  case "$e" in gen|jouer|use) ;; *) die "étape inconnue : $e (gen, jouer, use)" ;; esac
done
has() { case ",$ETAPES," in *",$1,"*) return 0 ;; esac; return 1; }

# Une seule construction LTO à la fois sur la machine ; une autre construction
# quelconque (ninja) n'est pas dangereuse sous plafond, mais l'hôte n'est pas au repos.
if pgrep -x 'lto1|lto-wrapper' >/dev/null 2>&1; then
  if [ -n "$DRY" ]; then echo "  (une édition de liens LTO tourne : la vraie exécution refuserait)"
  elif has gen || has use; then die "une édition de liens LTO tourne déjà (pgrep lto1) : attendre"; fi
fi
if pgrep -x ninja >/dev/null 2>&1; then
  echo "  ⚠ une autre construction tourne (ninja) : l'hôte n'est pas au repos" >&2
  [ -n "$DRY$FORCE" ] || ! has jouer || die "étape jouer sur un hôte occupé : --force pour passer outre"
fi

BUILD_WRAP=(nice -n 10)
if command -v systemd-run >/dev/null 2>&1; then
  BUILD_WRAP=(systemd-run --user --scope --quiet -p "MemoryMax=$MEM" -p MemorySwapMax=1G
              nice -n 10)
else
  echo "  ⚠ systemd-run absent : construction SANS plafond mémoire (nice seulement)" >&2
fi
build() { # build <gen|use>
  say "construction build-fast/ ($1) — $SRC, plafond $MEM, journal $LOGS/build-$1.log"
  run mkdir -p "$LOGS"
  if [ -n "$DRY" ]; then
    run "${BUILD_WRAP[@]}" env QEMU_SRC="$SRC" QEMU_FAST=only QEMU_FAST_PGO="$1" \
      PGO_DIR="$PGO_DIR" LTO_JOBS="${LTO_JOBS:-6}" bash "$WT/scripts/build_qemu_qfb.sh"
    return 0
  fi
  local t0=$SECONDS rc=0 unit="pomppc-fast-$1-$$" wrap=("${BUILD_WRAP[@]}") sampler="" peak
  # Scope nommé : sa mémoire max (memory.peak du cgroup, TOUS les processus de la
  # construction, pas seulement le plus gros comme le « Maximum resident » de time)
  # est relevée toutes les 5 s dans peak-<étape>.txt.
  if [ "${wrap[0]}" = systemd-run ]; then
    wrap=(systemd-run --user --scope --quiet --unit="$unit" "${wrap[@]:4}")
    ( cg=""
      for _ in $(seq 1 60); do
        cg="$(systemctl --user show -p ControlGroup --value "$unit.scope" 2>/dev/null)" || true
        [ -n "$cg" ] && break; sleep 1
      done
      while [ -n "$cg" ] && [ -r "/sys/fs/cgroup$cg/memory.peak" ]; do
        cat "/sys/fs/cgroup$cg/memory.peak" > "$LOGS/peak-$1.txt" 2>/dev/null || true; sleep 5
      done ) &
    sampler=$!
  fi
  "${wrap[@]}" env QEMU_SRC="$SRC" QEMU_FAST=only QEMU_FAST_PGO="$1" \
      PGO_DIR="$PGO_DIR" LTO_JOBS="${LTO_JOBS:-6}" \
      /usr/bin/time -v bash "$WT/scripts/build_qemu_qfb.sh" > "$LOGS/build-$1.log" 2>&1 || rc=$?
  [ -z "$sampler" ] || { kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null || true; }
  grep -E '^(✔|⚠)|Maximum resident|Elapsed' "$LOGS/build-$1.log" | sed 's/^/  /' || true
  peak="$(cat "$LOGS/peak-$1.txt" 2>/dev/null || true)"
  [ -z "$peak" ] || echo "  mémoire max de la construction (cgroup) : $((peak / 1048576)) Mio"
  [ "$rc" -eq 0 ] || die "construction $1 ratée (code $rc) : $LOGS/build-$1.log"
  say "construction $1 faite en $(( (SECONDS - t0) / 60 )) min"
}

# --- importer un profil voisin ---
if [ -n "$IMPORT" ]; then
  [ -d "$IMPORT" ] || die "$IMPORT n'est pas un dossier"
  first="$(find "$IMPORT" -maxdepth 1 -name '*.gcda' -printf '%f\n' | LC_ALL=C sort | sed -n 1p)"  # sed lit tout : pas de SIGPIPE sous pipefail
  [ -n "$first" ] || die "aucun .gcda dans $IMPORT"
  if [ -z "$DE" ]; then
    # #home#…#<arbre>#<dossier de build>#<objet>.gcda : le dossier de build est le
    # premier composant qui commence par « build ».
    pre="${first%%#build*}"
    rest="${first#"$pre"#}"
    DE="$pre#${rest%%#*}#"
  else
    case "$DE" in '#'*) ;; *) DE="$(mangle "${DE%/}")" ;; esac
  fi
  TO="$(mangle "$SRC/build-fast")"
  say "import de $IMPORT : « $DE » → « $TO »"
  archive_profile
  n=0; skip=0
  while read -r f; do
    case "$f" in
      "$DE"*) [ -n "$DRY" ] || cp -p "$IMPORT/$f" "$PGO_DIR/$TO${f#"$DE"}"; n=$((n + 1)) ;;
      *) skip=$((skip + 1)) ;;
    esac
  done < <(find "$IMPORT" -maxdepth 1 -name '*.gcda' -printf '%f\n')
  [ "$n" -gt 0 ] || die "aucun .gcda ne commence par « $DE » (--de <préfixe>)"
  [ "$skip" -eq 0 ] || echo "  ⚠ $skip .gcda d'un autre préfixe ignorés" >&2
  if [ -z "$DRY" ]; then
    { echo "# profil importé par tools/tcg/pgo-train.sh"
      echo "date=$(date "+%Y-%m-%dT%H:%M:%S%z")"
      echo "origine=$IMPORT ($DE)"
      echo "serie=importé"
      echo "gcda=$n"; } > "$PGO_DIR/pomppc-profil.txt"
  fi
  say "$n .gcda importés dans $PGO_DIR (fichiers changés depuis : compilés sans profil)"
fi

# --- gen : binaire instrumenté ---
if has gen; then
  archive_profile
  build gen
  # Le contrôle de capacités a lancé l'instrumenté (QEMU -M none…) : ces .gcda-là
  # ne sont pas du jeu.
  say "profil remis à zéro avant la partie"
  run find "$PGO_DIR" -maxdepth 1 -name '*.gcda' -delete
fi

# --- jouer : charge mixte sur la VM quotidienne ---
if has jouer; then
  [ -x "$FAST" ] || [ -n "$DRY" ] || die "$FAST absent : étape gen d'abord"
  if [ -z "$DRY" ]; then
    grep -q '^variante=.*pgo-gen' "$SRC/build-fast/pomppc-build.txt" 2>/dev/null ||
      die "$FAST n'est pas l'instrumenté (pgo-gen) : étape gen d'abord"
  fi
  [ -f "$R/disks/tiger.qcow2" ] || [ -n "$DRY" ] || die "VM quotidienne absente ($R/disks/tiger.qcow2)"
  [ -f "$R/disks/tiger.qcow2" ] || echo "  (VM quotidienne absente : $R/disks/tiger.qcow2)"
  C="pgo-train-$STAMP"
  say "partie d'entraînement : $JEUX $MODE, $PARTIES partie(s) → $R/bench/tcg/ab/$C"
  run env -u QEMU_BIN "$WT/tools/tcg/matab.sh" "$C" "$PARTIES" "train:QEMU_BIN=$FAST" "$JEUX" "$MODE"
  if [ -z "$DRY" ]; then
    n="$(ngcda)"
    # 03/10 : 1 356 .gcda pour 25 min de DOOM 3 ; quelques dizaines = QEMU n'a pas
    # quitté proprement ou n'a presque pas tourné.
    [ "$n" -ge 500 ] || die "seulement $n .gcda dans $PGO_DIR : partie ratée ? (bench/tcg/ab/$C)"
    { echo "# profil entraîné par tools/tcg/pgo-train.sh"
      echo "date=$(date "+%Y-%m-%dT%H:%M:%S%z")"
      echo "campagne=$R/bench/tcg/ab/$C"
      echo "jeux=$JEUX $MODE, $PARTIES partie(s)"
      echo "binaire=$FAST"
      echo "serie=$(serie_hash)"
      echo "pomppc=$(git -C "$WT" rev-parse --short HEAD 2>/dev/null)"
      echo "gcda=$n"; } > "$PGO_DIR/pomppc-profil.txt"
    say "$n .gcda dans $PGO_DIR"
  fi
fi

# --- use : binaire rapide avec le profil ---
if has use; then
  [ "$(ngcda)" -gt 0 ] || [ -n "$DRY" ] || die "pas de profil dans $PGO_DIR"
  build use
fi

cat <<EOF

Suite (docs/binaire-rapide-x86.md) :
  A/B contre la référence :  tools/tcg/matab.sh fast-ab 3 "ref:" "fast:QEMU_BIN=$FAST" d3 fen
  matrice avec vidage :      MATAB_VIDAGE=1 tools/tcg/matab.sh fast-mat 1 "ref:" "fast:QEMU_BIN=$FAST" mb,zen,ut,d3 fen
  VM quotidienne :           tools/tcg/matab.sh --restore   (run_tiger.sh prend build-fast/)
EOF
