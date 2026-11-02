#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# disques_test.sh — les arguments QEMU des disques (scripts/disques.sh), sans VM.
set -u
RACINE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "${T:?}"' EXIT
# shellcheck source=/dev/null
source "$RACINE/scripts/disques.sh"
echoues=0
contient() { # contient "libellé" "motif" : motif présent dans les arguments
  case "${DISQUES_ARGS[*]-}" in *"$2"*) echo "ok   $1" ;; *) echo "ÉCHEC $1 : ${DISQUES_ARGS[*]-}"; echoues=$((echoues + 1)) ;; esac
}
exclut() {
  case "${DISQUES_ARGS[*]-}" in *"$2"*) echo "ÉCHEC $1 : ${DISQUES_ARGS[*]-}"; echoues=$((echoues + 1)) ;; *) echo "ok   $1" ;; esac
}
touch "$T/boot.qcow2" "$T/a.qcow2" "$T/b.qcow2" "$T/c.qcow2" "$T/d,e.qcow2" "$T/esp ace.qcow2"
unset POMPPC_EXTRA_DISKS
disques_args "$T/boot.qcow2" 2
[ "${#DISQUES_ARGS[@]}" = 0 ] && echo "ok   sans choix : aucun argument" || { echo "ÉCHEC sans choix"; echoues=$((echoues + 1)); }

export POMPPC_EXTRA_DISKS="$T/a.qcow2;$T/b.qcow2"
disques_args "$T/boot.qcow2" 2 2>/dev/null
contient "premier en haut" "drive=disque1,bus=ide.1,unit=1"
contient "second dessous" "drive=disque2,bus=ide.1,unit=0"
contient "HD par défaut" "rotation_rate=0,model=QEMU HARDDISK"
exclut "pas de global" "-global"

echo ssd > "$T/a.qcow2.type"
disques_args "$T/boot.qcow2" 2 2>/dev/null
contient "SSD en plus" "unit=1,rotation_rate=1,model=POMPPC SSD"
contient "HD à côté d'un SSD" "unit=0,rotation_rate=0"
rm -f "$T/a.qcow2.type"

echo ssd > "$T/boot.qcow2.type"
disques_args "$T/boot.qcow2" 2 2>/dev/null
contient "démarrage SSD : global" "-global ide-hd.rotation_rate=1"
contient "et les autres gardent HD" "rotation_rate=0,model=QEMU HARDDISK"
rm -f "$T/boot.qcow2.type"

POMPPC_EXTRA_DISKS="$T/a.qcow2;$T/b.qcow2;$T/c.qcow2"
disques_args "$T/boot.qcow2" 2 2>/dev/null
exclut "surplus ignoré" "c.qcow2"
disques_args "$T/boot.qcow2" 1 2>/dev/null
contient "un seul emplacement : le haut" "unit=1"
exclut "un seul emplacement : pas d'autre" "unit=0"
disques_args "$T/boot.qcow2" 0 2>/dev/null
exclut "aucun emplacement" "-drive"

POMPPC_EXTRA_DISKS="$T/boot.qcow2;$T/a.qcow2;$T/a.qcow2;$T/absent.qcow2"
disques_args "$T/boot.qcow2" 2 2>/dev/null
exclut "démarrage jamais en double" "file=$T/boot.qcow2"
exclut "absent ignoré" "absent.qcow2"
exclut "doublon ignoré" "disque2"

POMPPC_EXTRA_DISKS="$T/d,e.qcow2;$T/esp ace.qcow2"
disques_args "$T/boot.qcow2" 2 2>/dev/null
contient "virgule échappée" "d,,e.qcow2"
[ "${DISQUES_ARGS[5]-}" = "if=none,id=disque2,format=qcow2,file=$T/esp ace.qcow2" ] &&
  echo "ok   espace : un seul argument" || { echo "ÉCHEC espace : ${DISQUES_ARGS[5]-}"; echoues=$((echoues + 1)); }

echo "$echoues échec(s)"
[ "$echoues" = 0 ]
