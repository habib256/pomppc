#!/usr/bin/env bash
# Crée le disque dur virtuel (qcow2, alloué à la demande => ne prend pas 16G tout de suite).
source "$(dirname "$0")/lib.sh"
need qemu-img

if [[ -f "$DISK" ]]; then
  die "$DISK existe déjà. Supprime-le à la main si tu veux repartir de zéro (perte de données)."
fi

qemu-img create -f qcow2 "$DISK" "$DISK_SIZE"
echo "Disque créé: $DISK ($DISK_SIZE, alloué à la demande)"
