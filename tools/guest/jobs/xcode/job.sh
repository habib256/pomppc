#!/bin/sh
# Installe dans la VM de dev les Xcode Tools du DVD de Tiger, ceux dont le
# dépôt a besoin et rien de plus : gcc 4.0, cctools et make, SDK 10.4u (kext,
# plugin, programmes de test), en-têtes BSD et OpenGL.
#
#   CDROM=work/Mac_OS_X.hfs devloop.py start ; devloop.py run tools/guest/jobs/xcode
#
# PAS `installer` : en single-user il se bloque sur le premier paquet (vu en
# vrai, sans un octet écrit). On dépaquette les archives à la racine, et on
# refait à la main ce que les scripts des paquets font d'utile ici :
# gcc_select 4.0 (gcc4.0.pkg) et le retrait de /usr/bin/ld10.3 sur 10.4
# (MacOSX10.4.Universal.pkg). Le reste (Spotlight, crontab de distcc,
# CrashReporter) est sans objet pour la VM de dev.
DVD=/dvd
mkdir -p $DVD
mount | grep -q " on $DVD " || for d in 1 2 3 4; do
  mount -t hfs -o rdonly /dev/disk$d $DVD 2>/dev/null && break
done
P="$DVD/Xcode Tools/Packages"
[ -d "$P" ] || { echo "DVD introuvable : $(ls /dev/disk*)"; exit 1; }
cd /
for p in DeveloperTools gcc4.0 DevSDK MacOSX10.4.Universal BSDSDK OpenGLSDK; do
  echo "== $p"
  pax -rz -pe -f "$P/$p.pkg/Contents/Archive.pax.gz" 2>&1 | tail -3
done
rm -f /usr/bin/ld10.3
[ -x /usr/sbin/gcc_select ] && /usr/sbin/gcc_select 4.0 2>&1 | tail -1
echo "== résultat"
ls -l /usr/bin/gcc-4.0 /usr/bin/g++-4.0 /usr/bin/make /usr/bin/ld 2>&1
ls -d /Developer/SDKs/* 2>&1
/usr/bin/gcc-4.0 --version 2>&1 | head -1
sync
umount $DVD
