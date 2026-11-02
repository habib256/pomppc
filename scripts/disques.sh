#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# disques.sh — les disques d'une VM : celui de démarrage et ceux que choisit le frontal.
# Sourcé par run_tiger.sh et run_os9.sh.
#
#   POMPPC_EXTRA_DISKS="/chemin/a.qcow2;/chemin/b.qcow2"   disques en plus de celui de démarrage
#   <disque>.type (une ligne : ssd)                        ce disque se présente comme un SSD
#
# [D] QEMU hw/ppc/mac_newworld.c:519-525 @ v11.1.2 — deux canaux IDE, soit 4 emplacements.
# [A] Mesuré sous Tiger 10.4.11 (M4, 02/11) : démarrage, CD et deux disques en ide.1 visibles
#     ensemble par diskutil.
#
# Tiger n'a pas de notion de SSD. Le type pose rotation_rate=1 (mot 217 d'IDENTIFY) et un modèle
# lisible dans Informations Système ; il ne change pas la vitesse, faite par le disque de l'hôte.

DISQUES_MODELE_HD="QEMU HARDDISK"
DISQUES_MODELE_SSD="POMPPC SSD"

disques_type() {
  if [ "$(cat "$1.type" 2>/dev/null)" = ssd ]; then echo ssd; else echo hd; fi
}

# disques_args disque_de_demarrage emplacements_libres : remplit DISQUES_ARGS.
# Le disque de démarrage reste créé par la machine (-drive sans if=none) : sa place et l'ordre des
# CD ne changent pas. -global le déclare SSD, et les autres disques fixent leurs propres valeurs.
disques_args() {
  local boot="$1" libres="$2" liste f i=0 unite=1 rotation modele vus=";"
  DISQUES_ARGS=()
  if [ "$(disques_type "$boot")" = ssd ]; then
    DISQUES_ARGS+=(-global ide-hd.rotation_rate=1 -global "ide-hd.model=$DISQUES_MODELE_SSD")
  fi
  [ -n "${POMPPC_EXTRA_DISKS:-}" ] || return 0
  IFS=';' read -r -a liste <<< "$POMPPC_EXTRA_DISKS"
  for f in "${liste[@]}"; do
    [ -n "$f" ] || continue
    if [ "$f" = "$boot" ]; then continue; fi
    case "$vus" in *";$f;"*) continue ;; esac
    vus="$vus$f;"
    if [ ! -f "$f" ]; then
      echo "⚠  disque absent, ignoré : $f" >&2
      continue
    fi
    if [ "$libres" -le 0 ]; then
      echo "⚠  plus d'emplacement IDE libre, disque ignoré : $f" >&2
      continue
    fi
    if [ "$(disques_type "$f")" = ssd ]; then
      rotation=1; modele="$DISQUES_MODELE_SSD"
    else
      rotation=0; modele="$DISQUES_MODELE_HD"
    fi
    i=$((i + 1))
    # Les CD prennent les emplacements libres depuis le bas : on remplit ide.1 depuis le haut.
    DISQUES_ARGS+=(-drive "if=none,id=disque$i,format=qcow2,file=${f//,/,,}"
                   -device "ide-hd,drive=disque$i,bus=ide.1,unit=$unite,rotation_rate=$rotation,model=$modele")
    unite=$((unite - 1))
    libres=$((libres - 1))
  done
}
