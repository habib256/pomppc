#!/usr/bin/env bash
# Fonctions partagées. Sourcé par les scripts numérotés.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/config.env"

die() { echo "ERREUR: $*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "'$1' introuvable. Installe QEMU: sudo apt-get install -y qemu-system-ppc qemu-utils"; }

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
    -netdev "$NETDEV"
    -device "$NETNIC"
    # -v = boot verbeux du kernel XNU : indispensable pour diagnostiquer un boot qui cale.
    -prom-env 'boot-args=-v'
  )
}
