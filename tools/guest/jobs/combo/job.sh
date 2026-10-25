#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# Passe la VM de dev de 10.4.6 à 10.4.11 par la mise à jour combinée d'Apple
# (celle du PC le 02/10, docs/tablette-tiger-10.4.11.md ; refaite sur le M4 le
# 25/10, docs/parite-m4-2510.md). Sur une COPIE du disque de dev, au BUREAU :
#
#   CDROM=disks/combo-10.4.11.iso devloop.py start --gui
#   devloop.py run tools/guest/jobs/combo --timeout 5400
#   devloop.py shutdown --gui    (puis un démarrage : le combo finit au premier)
#
# `installer` au bureau, jamais en single-user (il s'y bloque, docs/gpu-3d-tiger.md).
PKG=MacOSXUpdCombo10.4.11PPC.pkg
V=$(ls -d /Volumes/*/$PKG 2>/dev/null | head -1)
if [ -z "$V" ]; then
  mkdir -p /combo
  for d in 1 2 3 4 5; do
    mount -t hfs -o rdonly /dev/disk${d}s3 /combo 2>/dev/null && break
    mount -t hfs -o rdonly /dev/disk$d /combo 2>/dev/null && break
  done
  V=/combo/$PKG
fi
[ -d "$V" ] || { echo "paquet introuvable : $(ls /Volumes) $(ls /dev/disk*)"; exit 1; }
echo "== avant"; sw_vers; uname -v
echo "== installer $V"
/usr/sbin/installer -verbose -pkg "$V" -target / > /tmp/combo-installer.log 2>&1
rc=$?
tail -40 /tmp/combo-installer.log
echo "== code $rc"
echo "== après (le noyau change au prochain démarrage)"
sw_vers
ls -l /mach_kernel
sync
