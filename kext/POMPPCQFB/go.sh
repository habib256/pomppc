#!/bin/sh
# go.sh — construit et installe POMPPCQFB.kext depuis le CD, en mode single-user.
#
#   mkdir /c ; mount_cd9660 /dev/disk1 /c ; sh /c/go.sh
#
# Journal complet : /var/log/pomppcqfb-build.log ; résumé sur la console.
LOG=/var/log/pomppcqfb-build.log
SRC=$(dirname "$0")
: > $LOG

log() { echo "$@" >> $LOG; }
say() { echo "$@"; echo "$@" >> $LOG; }

log "=== toolchain ==="
{ ls -d /usr/bin/gcc* /usr/bin/make /Developer/SDKs/* 2>&1; } >> $LOG
{ /usr/bin/gcc-4.0 --version 2>&1 | head -1; } >> $LOG

if [ ! -x /usr/bin/gcc-4.0 ] || [ ! -x /usr/bin/make ]; then
  say "TOOLCHAIN ABSENTE :"
  say "  gcc-4.0 : $([ -x /usr/bin/gcc-4.0 ] && echo oui || echo NON)"
  say "  make    : $([ -x /usr/bin/make ] && echo oui || echo NON)"
  say "  SDK     : $(ls -d /Developer/SDKs/MacOSX10.4u.sdk 2>/dev/null || echo NON)"
  say "-> installer les Xcode Tools depuis le DVD Tiger"
  exit 1
fi

log "=== copie des sources ==="
rm -rf /tmp/qfb; mkdir -p /tmp/qfb
cp "$SRC"/*.h "$SRC"/*.cpp "$SRC"/Info.plist "$SRC"/Makefile /tmp/qfb/ >> $LOG 2>&1
cd /tmp/qfb || exit 1

log "=== compilation ==="
make >> $LOG 2>&1
RC=$?
if [ $RC -ne 0 ]; then
  say "COMPILATION ÉCHOUÉE (rc=$RC), dernières lignes :"
  grep -i "error\|Error" $LOG | head -8
  tail -6 $LOG
  exit 1
fi
say "COMPILATION OK : $(ls -l /tmp/qfb/POMPPCQFB | awk '{print $5}') octets"

log "=== installation ==="
rm -rf /System/Library/Extensions/POMPPCQFB.kext
cp -R /tmp/qfb/POMPPCQFB.kext /System/Library/Extensions/ >> $LOG 2>&1
chown -R root:wheel /System/Library/Extensions/POMPPCQFB.kext
chmod -R 755 /System/Library/Extensions/POMPPCQFB.kext
rm -f /System/Library/Extensions.mkext /System/Library/Extensions.kextcache
touch /System/Library/Extensions
say "INSTALLÉ dans /System/Library/Extensions (cache invalidé)"
say "Redémarre : reboot"
