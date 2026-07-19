#!/usr/bin/env bash
# Boote le média d'installation Tiger pour installer OS X sur le disque virtuel.
source "$(dirname "$0")/lib.sh"
need "$QEMU_BIN"

[[ -f "$DISK" ]]          || die "Disque absent. Lance d'abord: scripts/00-create-disk.sh"
[[ -f "$INSTALL_MEDIA" ]] || die "Média d'install absent: $INSTALL_MEDIA (dépose ton ISO/DMG Tiger PPC dans images/ et ajuste config.env)"

qemu_common d   # boot depuis le cdrom
QEMU_ARGS+=( -drive "file=$INSTALL_MEDIA,format=raw,media=cdrom" )

echo ">>> $QEMU_BIN ${QEMU_ARGS[*]}"
echo
echo "PIÈGE OPENFIRMWARE — si ça s'arrête sur un prompt '0 >' au lieu de booter le CD :"
echo "  L'auto-boot ne trouve pas toujours le bless du CD. À l'invite OF, tape :"
echo "      boot cd:,\\\\:tbxi"
echo "  (le ':tbxi' cible le fichier bootinfo 'blessé' du CD d'install Apple)"
echo
exec "$QEMU_BIN" "${QEMU_ARGS[@]}"
