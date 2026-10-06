#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# qemu_fast.sh — choix du binaire QEMU RAPIDE du PC (docs/binaire-rapide-x86.md).
#
# Sourcé par run_tiger.sh APRÈS config.env et scripts/caps.sh (il emploie leurs
# sondes : qemu_machine_has, qemu_has_device, qemu_machine_smp_ok, mémoïsées, donc
# réutilisées telles quelles par le lanceur ensuite).
#
#   pomppc_pick_qemu <smp>   pose QEMU_BIN et QEMU_BIN_LABEL ; code 1 seulement si
#                            QEMU_FAST=1 a été demandé et que le binaire rapide
#                            n'est pas utilisable (refus net, raison sur stderr).
#
# Règles (dans l'ordre) :
#   • QEMU_BIN explicite (USER_QEMU_BIN) prime toujours : « binaire imposé » ;
#   • QEMU_FAST=0 : binaire de référence (build/), sans rien sonder ;
#   • hors Linux x86-64, ou depuis un paquet publié (bin/ à la racine) : rien ne change ;
#   • sinon $QEMU_FAST_BIN (config.env : ~/src/qemu/build-fast/qemu-system-ppc) est pris
#     s'il existe ET passe pomppc_fast_ok ; écarté avec un avertissement sinon
#     (QEMU_FAST=1 : refus au lieu du repli).
#
# Le relevé de construction <dossier>/pomppc-build.txt (écrit par
# scripts/build_qemu_qfb.sh) dit la variante, le processeur de construction et si
# le contrôle de capacités est passé : un binaire -march=native construit ailleurs
# peut mourir sur SIGILL, un binaire pgo-gen est l'instrumenté de l'entraînement
# (3-4 fois plus lent), un binaire construit sur une autre série de patches que ce
# dépôt n'a pas les derniers (la référence a été reconstruite sans lui).

# Empreinte de la série de patches : les fichiers de patches/ que
# scripts/build_qemu_qfb.sh applique ou copie réellement (ceux qu'il nomme par
# "$ROOT"/patches/…), écrite dans le relevé de construction et dans celui du
# profil PGO, comparée au lancement. Jusqu'au 07/10 elle couvrait tout patches/
# (sauf essais/ et l'OpenBIOS) : un patch posé pour un autre hôte (tcg/0037-0039,
# arm64) ou une ligne de README écartait le binaire rapide sans qu'il ait changé.
pomppc_serie_hash() { # pomppc_serie_hash <dossier patches>
  local d="$1" b="$1/../scripts/build_qemu_qfb.sh"
  {
    if [ -f "$b" ]; then
      grep -o '"\$ROOT"\?/patches/[^" ]*' "$b" | sed 's|^"\$ROOT"\?/patches/||' |
        LC_ALL=C sort -u | while read -r f; do [ -f "$d/$f" ] && echo "$d/$f"; done
    else
      find "$d" -type f -not -path '*/essais/*' -not -name '*.elf' | LC_ALL=C sort
    fi
  } | while read -r f; do cat "$f"; done | { sha256sum 2>/dev/null || shasum -a 256; } | cut -c1-16
}

pomppc_fast_cpu() { sed -n 's/^model name[[:space:]]*: *//p' /proc/cpuinfo 2>/dev/null | head -1; }

pomppc_fast_ver() { "$1" --version 2>/dev/null | sed -n '1s/.*version \([0-9][0-9.]*\).*/\1/p'; }

POMPPC_FAST_WHY=""
# pomppc_fast_ok <qemu-system-ppc> <smp> [machine] -> 0 si utilisable, sinon 1 et POMPPC_FAST_WHY
pomppc_fast_ok() {
  local b="$1" n="${2:-1}" m="${3:-${MACHINE:-mac99,via=pmu}}" st run v="" x cpu
  st="$(dirname "$b")/pomppc-build.txt"
  POMPPC_FAST_WHY=""
  [ -x "$b" ] || { POMPPC_FAST_WHY="$b absent"; return 1; }
  if [ "$n" -ge 2 ]; then
    [ -x "${b}64" ] || { POMPPC_FAST_WHY="${b}64 absent (SMP $n)"; return 1; }
    run="${b}64"
  else
    run="$b"
  fi
  [ -f "$st" ] || { POMPPC_FAST_WHY="pas de relevé de construction ($st)"; return 1; }
  grep -q '^complet=oui' "$st" ||
    { POMPPC_FAST_WHY="construction incomplète (contrôle de capacités raté, $st)"; return 1; }
  if grep -q '^variante=.*pgo-gen' "$st"; then
    POMPPC_FAST_WHY="binaire instrumenté (pgo-gen) : entraînement du profil en cours ou interrompu"
    return 1
  fi
  cpu="$(pomppc_fast_cpu)"
  x="$(sed -n 's/^cpu=//p' "$st")"
  if [ -n "$x" ] && [ -n "$cpu" ] && [ "$x" != "$cpu" ]; then
    POMPPC_FAST_WHY="construit pour « $x », cet hôte est « $cpu » (-march=native)"
    return 1
  fi
  # Construit sur une AUTRE série de patches que celle de ce dépôt : la référence
  # a sans doute été reconstruite depuis (nouveaux patches), pas le binaire rapide.
  # Le lancer serait remonter le temps en silence. (Une date ne suffit pas : le
  # script de construction recopie des sources à chaque passage, et une
  # reconstruction à vide de build/ le rendrait « plus récent ».)
  x="$(sed -n 's/^serie=//p' "$st")"
  if [ -n "$x" ] && [ -d "${ROOT:-}/patches" ] && [ "$x" != "$(pomppc_serie_hash "$ROOT/patches")" ]; then
    POMPPC_FAST_WHY="construit sur une autre série de patches que ce dépôt (relevé $x)"
    return 1
  fi
  for x in "$b" "$run"; do
    [ "$x" = "$b" ] && [ "$x" = "$run" ] && [ -n "$v" ] && continue
    v="$(pomppc_fast_ver "$x")"
    if [ -n "${POMPPC_QEMU_VERSION:-}" ] && [ "$v" != "$POMPPC_QEMU_VERSION" ]; then
      POMPPC_FAST_WHY="$x est QEMU ${v:-? (ne démarre pas)}, la série vise $POMPPC_QEMU_VERSION"
      return 1
    fi
  done
  # Capacités, sondées sur le binaire qui sera LANCÉ (ppc64 en SMP) : celles sans
  # lesquelles le lanceur dégraderait (son, GPU, SMP). Mieux vaut la référence
  # complète qu'un binaire rapide amputé.
  qemu_machine_has "$run" "$m" screamer >/dev/null 2>&1 ||
    { POMPPC_FAST_WHY="pas de Screamer dans $m ($run)"; return 1; }
  qemu_has_device "$run" qgpu-pci >/dev/null 2>&1 ||
    { POMPPC_FAST_WHY="pas de qgpu-pci ($run)"; return 1; }
  if [ "$n" -ge 2 ]; then
    qemu_machine_smp_ok "$run" "$m" "$n" >/dev/null 2>&1 ||
      { POMPPC_FAST_WHY="-smp $n refusé ($run)"; return 1; }
  fi
  return 0
}

# Étiquette lisible d'un binaire rapide, d'après son relevé.
pomppc_fast_label() {
  local st="$(dirname "$1")/pomppc-build.txt" var
  var="$(sed -n 's/^variante=//p' "$st" 2>/dev/null)"
  case ",$var," in
    *,pgo,*) echo "binaire rapide (PGO, -O3, -march=native)" ;;
    *)       echo "binaire rapide (-O3, -march=native, LTO, sans PGO)" ;;
  esac
}

pomppc_pick_qemu() { # pomppc_pick_qemu <smp>
  local n="${1:-1}" host b
  QEMU_BIN_LABEL="binaire de référence"
  if [ -n "${USER_QEMU_BIN:-}" ]; then
    QEMU_BIN_LABEL="binaire imposé (QEMU_BIN)"; return 0
  fi
  case "${QEMU_FAST:-}" in
    0) return 0 ;;
    ""|1) ;;
    *) echo "⚠  QEMU_FAST attendu : 0 ou 1 (vide = automatique)" >&2; return 1 ;;
  esac
  host="${POMPPC_FAST_HOST:-$(uname -s):$(uname -m)}"
  if [ "$host" != Linux:x86_64 ]; then
    [ "${QEMU_FAST:-}" = 1 ] && echo "⚠  QEMU_FAST=1 : binaire rapide du PC seulement (Linux x86-64)" >&2
    return 0
  fi
  # Paquet publié : son bin/ est générique et passe avant tout (config.env).
  [ -n "${ROOT:-}" ] && [ -x "$ROOT/bin/qemu-system-ppc" ] && return 0
  b="${QEMU_FAST_BIN:-}"
  [ -n "$b" ] || return 0
  # Pas de binaire rapide construit : silence, c'est le cas de tout le monde.
  [ -e "$b" ] || [ "${QEMU_FAST:-}" = 1 ] || return 0
  if pomppc_fast_ok "$b" "$n"; then
    QEMU_BIN="$b"
    QEMU_BIN_LABEL="$(pomppc_fast_label "$b")"
    return 0
  fi
  if [ "${QEMU_FAST:-}" = 1 ]; then
    echo "⚠  QEMU_FAST=1 : binaire rapide inutilisable — $POMPPC_FAST_WHY." >&2
    echo "   Reconstruire : QEMU_FAST=1 ./scripts/build_qemu_qfb.sh (docs/binaire-rapide-x86.md)" >&2
    return 1
  fi
  echo "⚠  binaire rapide écarté — $POMPPC_FAST_WHY ; binaire de référence." >&2
  echo "   (QEMU_FAST=1 ./scripts/build_qemu_qfb.sh le reconstruit ; QEMU_FAST=0 tait ce message)" >&2
  return 0
}
