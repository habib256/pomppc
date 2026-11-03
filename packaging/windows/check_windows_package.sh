#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# check_windows_package.sh <dossier> — le pendant de packaging/check_package_payload.sh et de
# packaging/linux/check_appimage.sh : ce que le zip doit porter, et ce qu'il ne doit jamais porter.
#
#   · chaque DLL demandée est dans le dossier, ou dans la liste blanche de ce que Windows fournit ;
#   · jamais de pilote GL ni de msys-2.0.dll dans le dossier : un .exe qui en dépend ne tourne que sur
#     la machine qui l'a construit, ou prend le pas sur le pilote de l'utilisateur ;
#   · QEMU lie bien opengl32 (sinon qgpu est un talon logiciel, en silence) ;
#   · aucun média ni disque : le dépôt ne distribue rien d'Apple.
#
#   check_windows_package.sh <dossier>
#   check_windows_package.sh --self-test     # témoins négatifs, sans paquet ni objdump
set -euo pipefail

# Ce que Windows fournit à tout le monde. API-MS-WIN-* : les API de l'UCRT, redirigées par le chargeur.
# [A] Liste relevée au premier passage de la CI ; un nouveau nom y fait échouer le job, à relire.
SYSTEME='^(advapi32|bcrypt|bcryptprimitives|cfgmgr32|comctl32|comdlg32|crypt32|d3d11|dbghelp|dnsapi|dsound|dwmapi|dxgi|gdi32|imm32|iphlpapi|kernel32|kernelbase|mswsock|msvcrt|ncrypt|netapi32|ntdll|ole32|oleaut32|opengl32|psapi|rpcrt4|secur32|setupapi|shcore|shell32|shlwapi|ucrtbase|user32|userenv|uxtheme|version|winhttp|winmm|ws2_32|wsock32|api-ms-win-.*|ext-ms-.*)\.dll$'
INTERDIT='^(opengl32|glu32|d3d[0-9].*|dxgi|ig.*icd.*|nvoglv.*|atio.*|libegl|libglesv2|libgl|libglapi|msys-2\.0|msys-.*|cygwin1)\.dll$'

defauts_dll() { # defauts_dll <fichier imports : « module dll » par ligne> <fichier présentes>
  local mod dll bas
  while read -r mod dll; do
    bas="${dll,,}"
    if grep -qix -- "$dll" "$2"; then continue; fi
    if [[ "$bas" =~ $SYSTEME ]]; then continue; fi
    echo "DLL ni au paquet ni à Windows : $dll (demandée par $mod)"
  done < "$1"
  while read -r dll; do
    bas="${dll,,}"
    if [[ "$bas" =~ $INTERDIT ]]; then echo "interdit au paquet : $dll"; fi
  done < "$2"
}

defauts_contenu() { # defauts_contenu <dossier>
  local r="$1" f
  for f in POMPPC.exe qemu-system-ppc.exe qemu-system-ppc64.exe qemu-img.exe share/qemu/openbios-ppc \
           share/qemu/qemu_vga.ndrv patches/smp-mac99/openbios-smp-screamer.elf pomppc-guest.iso LICENSE README.txt \
           THIRD-PARTY-LICENSES/README.txt THIRD-PARTY-LICENSES/qemu-COPYING; do
    [ -f "$r/$f" ] || echo "manque : $f"
  done
  # Le firmware libre de QEMU (efi-*.rom…) a sa place sous share/qemu ; le CD invité à la racine.
  find "$r" -type f \( -iname '*.qcow2' -o -iname '*.raw' -o -iname '*.toast' -o -iname '*.cdr' \
       -o -iname '*.dmg' -o -iname '*.img' -o -iname '*.dsk' -o -iname '*.hfs' -o -iname '*.iso' \
       -o -iname '*.rom' -o -iname '*.pram' \) ! -path "$r/pomppc-guest.iso" ! -path "$r/share/qemu/*" |
    sed "s|^$r/|fuite : |"
  [ ! -e "$r/.run" ] || echo "fuite : .run"
  [ ! -e "$r/disks" ] || echo "fuite : disks"
}

if [ "${1:-}" = --self-test ]; then
  T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
  fails=0
  attend() { # attend <0|1> <libellé> <sortie>
    local got=0
    [ -z "$3" ] || got=1
    if [ "$got" = "$1" ]; then echo "  ok    $2"; else echo "  ÉCHEC $2 : $3"; fails=$((fails + 1)); fi
  }
  printf 'libglib-2.0-0.dll\n' > "$T/ici"
  printf 'a.exe libglib-2.0-0.dll\na.exe KERNEL32.dll\na.exe OPENGL32.dll\n' > "$T/imp"
  attend 0 "le paquet et Windows couvrent tout" "$(defauts_dll "$T/imp" "$T/ici")"
  printf 'a.exe libinconnue.dll\n' > "$T/imp"
  attend 1 "DLL qui n'est nulle part" "$(defauts_dll "$T/imp" "$T/ici")"
  printf 'a.exe msys-2.0.dll\n' > "$T/imp"
  attend 1 "dépendance à msys-2.0" "$(defauts_dll "$T/imp" "$T/ici")"
  : > "$T/imp"; printf 'opengl32.dll\n' > "$T/ici"
  attend 1 "opengl32 embarquée" "$(defauts_dll "$T/imp" "$T/ici")"
  printf 'nvoglv64.dll\n' > "$T/ici"
  attend 1 "pilote NVIDIA embarqué" "$(defauts_dll "$T/imp" "$T/ici")"
  mk() {
    rm -rf "$T/p"; mkdir -p "$T/p/share/qemu" "$T/p/patches/smp-mac99" "$T/p/THIRD-PARTY-LICENSES"
    for f in POMPPC.exe qemu-system-ppc.exe qemu-system-ppc64.exe qemu-img.exe share/qemu/openbios-ppc \
             share/qemu/qemu_vga.ndrv patches/smp-mac99/openbios-smp-screamer.elf pomppc-guest.iso LICENSE README.txt \
             THIRD-PARTY-LICENSES/README.txt THIRD-PARTY-LICENSES/qemu-COPYING; do : > "$T/p/$f"; done
  }
  mk; attend 0 "un paquet complet passe" "$(defauts_contenu "$T/p")"
  mk; : > "$T/p/share/qemu/efi-sungem.rom"; attend 0 "firmware de QEMU permis" "$(defauts_contenu "$T/p")"
  mk; rm "$T/p/POMPPC.exe"; attend 1 "frontal absent" "$(defauts_contenu "$T/p")"
  mk; rm "$T/p/pomppc-guest.iso"; attend 1 "CD invité absent" "$(defauts_contenu "$T/p")"
  mk; : > "$T/p/tiger.qcow2"; attend 1 "disque qui fuit" "$(defauts_contenu "$T/p")"
  mk; : > "$T/p/MacOSX.DMG"; attend 1 "dmg qui fuit, casse changée" "$(defauts_contenu "$T/p")"
  if [ "$fails" -ne 0 ]; then echo "check_windows_package : autotest ÉCHOUÉ ($fails)"; exit 1; fi
  echo "check_windows_package : autotest passé"
  exit 0
fi

R="${1:?dossier du paquet attendu}"
[ -d "$R" ] || { echo "$R n'est pas un dossier" >&2; exit 2; }
R="$(cd "$R" && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT

for f in "$R"/*.exe "$R"/*.dll; do
  [ -f "$f" ] || continue
  objdump -p "$f" | tr -d '\r' | sed -n 's/^[[:space:]]*DLL Name: //p' | sed "s|^|$(basename "$f") |"
done > "$T/imp"
find "$R" -maxdepth 1 -iname '*.dll' -exec basename {} \; > "$T/ici"
echo "== DLL du paquet"; sort "$T/ici" | sed 's/^/  /'
echo "== DLL de Windows"; awk '{print $2}' "$T/imp" | sort -fu | grep -vixFf "$T/ici" | sed 's/^/  /' || true

DEFAUTS="$(defauts_dll "$T/imp" "$T/ici"; defauts_contenu "$R")"
if ! objdump -p "$R/qemu-system-ppc.exe" | grep -qi 'DLL Name: OPENGL32\.dll'; then
  DEFAUTS="$DEFAUTS
qemu-system-ppc.exe ne lie pas OPENGL32.dll : qgpu n'y a que son talon logiciel"
fi
if [ -n "$DEFAUTS" ]; then
  printf '%s\n' "$DEFAUTS" >&2
  echo "check_windows_package : ÉCHEC" >&2
  exit 1
fi
echo "check_windows_package : OK ($R)"
