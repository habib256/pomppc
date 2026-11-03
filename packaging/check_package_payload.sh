#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# check_package_payload.sh <racine du paquet> — ce qu'un paquet doit porter, et ce qu'il ne
# doit jamais porter. La racine est celle du dépôt (usr/share/pomppc dans l'AppDir).
#
# Deux pannes sont muettes autrement : un fichier manquant se découvre chez l'utilisateur, et une
# fuite (un disque, une ROM, un média d'Apple qui traînait dans l'arbre) fait de la publication
# une redistribution de ce qui n'est pas à nous. Le dépôt ne distribue rien d'Apple.
#
#   packaging/check_package_payload.sh <racine>
#   packaging/check_package_payload.sh --self-test      # témoins négatifs, sans paquet
set -euo pipefail

verifier() { # verifier <racine> : imprime un défaut par ligne
  local r="$1" f
  for f in run_tiger.sh run_os9.sh run_frontend.sh config.env LICENSE \
           scripts/caps.sh scripts/hostcompat.sh scripts/disques.sh scripts/qemu_fast.sh \
           scripts/web-proxy.py \
           patches/smp-mac99/openbios-smp-screamer.elf \
           share/qemu/openbios-ppc \
           pomppc-guest.iso \
           THIRD-PARTY-LICENSES/README.txt THIRD-PARTY-LICENSES/qemu-COPYING; do
    [ -f "$r/$f" ] || echo "manque : $f"
  done
  for f in bin/qemu-system-ppc bin/qemu-system-ppc64 bin/qemu-img bin/pomppc; do
    [ -x "$r/$f" ] || echo "manque (exécutable) : $f"
  done
  [ -n "$(find "$r/lib" -maxdepth 1 -name '*.so*' 2>/dev/null | head -1)" ] || echo "manque : lib/ vide"

  # Médias, disques, firmware d'Apple : aucune extension de ce genre, à part le CD invité et le
  # firmware libre que QEMU installe dans share/qemu/ (efi-*.rom, s390-*.img…).
  find "$r" -type f \( -iname '*.qcow2' -o -iname '*.raw' -o -iname '*.rom' -o -iname '*.toast' \
       -o -iname '*.cdr' -o -iname '*.dmg' -o -iname '*.img' -o -iname '*.dsk' -o -iname '*.hfs' \
       -o -iname '*.iso' -o -iname '*.pram' \) ! -path "$r/pomppc-guest.iso" ! -path "$r/share/qemu/*" |
    sed "s|^$r/|fuite : |"
  # Ce que l'arbre de travail garde et que le commit n'a pas : .run/ (réglages, verrous, jeux),
  # bench/, images/, les carillons extraits d'une ROM, les constructions.
  for f in .run bench images disks/chimes disks/cdr disks/prebuilt frontend/build \
           frontend/build-appimage frontend/imgui build-appimage dist; do
    if [ -e "$r/$f" ]; then echo "fuite : $f"; fi
  done
}

if [ "${1:-}" = --self-test ]; then
  T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
  fails=0
  mk() { # un paquet qui passe
    local f
    rm -rf "$T/p"
    for f in run_tiger.sh run_os9.sh run_frontend.sh config.env LICENSE scripts/caps.sh \
             scripts/hostcompat.sh scripts/disques.sh scripts/qemu_fast.sh scripts/web-proxy.py \
             patches/smp-mac99/openbios-smp-screamer.elf share/qemu/openbios-ppc pomppc-guest.iso \
             THIRD-PARTY-LICENSES/README.txt THIRD-PARTY-LICENSES/qemu-COPYING lib/libglib-2.0.so.0; do
      mkdir -p "$T/p/$(dirname "$f")"; : > "$T/p/$f"
    done
    mkdir -p "$T/p/bin"
    for f in qemu-system-ppc qemu-system-ppc64 qemu-img pomppc; do : > "$T/p/bin/$f"; chmod +x "$T/p/bin/$f"; done
  }
  attend() { # attend <0|1> <libellé>
    local got=0
    [ -z "$(verifier "$T/p")" ] || got=1
    if [ "$got" = "$1" ]; then echo "  ok    $2"; else echo "  ÉCHEC $2"; fails=$((fails + 1)); fi
  }
  mk; attend 0 "un paquet complet passe"
  mk; : > "$T/p/share/qemu/efi-sungem.rom"; attend 0 "le firmware de QEMU n'est pas une fuite"
  mk; rm "$T/p/bin/pomppc"; attend 1 "frontal absent"
  mk; rm "$T/p/pomppc-guest.iso"; attend 1 "CD invité absent"
  mk; rm "$T/p/lib/libglib-2.0.so.0"; attend 1 "lib/ vide"
  mk; rm "$T/p/share/qemu/openbios-ppc"; attend 1 "firmware absent"
  mk; rm "$T/p/patches/smp-mac99/openbios-smp-screamer.elf"; attend 1 "OpenBIOS unifié absent"
  mk; mkdir -p "$T/p/disks"; : > "$T/p/disks/tiger.qcow2"; attend 1 "disque Tiger qui fuit"
  mk; mkdir -p "$T/p/disks"; : > "$T/p/disks/games.ISO"; attend 1 "média qui fuit, casse changée"
  mk; : > "$T/p/MacOSX.dmg"; attend 1 "dmg qui fuit"
  mk; mkdir -p "$T/p/.run"; attend 1 ".run/ qui fuit"
  mk; mkdir -p "$T/p/disks/chimes"; attend 1 "carillons extraits qui fuient"
  if [ "$fails" -ne 0 ]; then echo "check_package_payload : autotest ÉCHOUÉ ($fails)"; exit 1; fi
  echo "check_package_payload : autotest passé"
  exit 0
fi

R="${1:?racine du paquet attendue}"
[ -d "$R" ] || { echo "$R n'est pas un dossier" >&2; exit 2; }
R="$(cd "$R" && pwd)"
DEFAUTS="$(verifier "$R")"
if [ -n "$DEFAUTS" ]; then
  printf '%s\n' "$DEFAUTS" >&2
  echo "check_package_payload : ÉCHEC" >&2
  exit 1
fi
echo "check_package_payload : OK ($R)"
