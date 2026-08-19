#!/usr/bin/env bash
# Fonctions partagées. Sourcé par les scripts numérotés.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/config.env"

die() { echo "ERREUR: $*" >&2; exit 1; }

need() {
  command -v "$1" >/dev/null 2>&1 && return 0
  case "$1" in
    */*) die "'$1' introuvable (QEMU_BIN). Construis-le (scripts/build_qemu_qfb.sh), ou laisse config.env retomber sur le paquet distro: sudo apt-get install -y qemu-system-ppc qemu-utils" ;;
    *)   die "'$1' introuvable. Installe QEMU: sudo apt-get install -y qemu-system-ppc qemu-utils" ;;
  esac
}

# Les scripts écrivent leurs logs/mesures dans bench/ (gitignoré, donc absent
# d'un clone neuf) : le créer avant d'y écrire.
BENCH="$ROOT/bench"
mkdir -p "$BENCH"

# Options QEMU communes à l'install et au run. $1 = mode boot ('d'=cdrom, 'c'=disque).
qemu_common() {
  local bootdev="$1"
  QEMU_ARGS=(
    -M "$MACHINE"
    -cpu "$CPU"
    -m "$RAM_MB"
    -smp "$SMP"
    -boot "$bootdev"
    -g "$RES"
    -drive "file=$DISK,format=qcow2,media=disk"
    # NET_ARGS vient de config.env : vide si NONET=1. Le passer en dur ici rendait
    # NONET inopérant — or le build source par défaut est compilé SANS slirp, donc
    # '-netdev user' y échoue au démarrage.
    "${NET_ARGS[@]}"
    # -v = boot verbeux du kernel XNU : indispensable pour diagnostiquer un boot qui cale.
    -prom-env 'boot-args=-v'
  )
}
