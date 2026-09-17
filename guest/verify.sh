#!/bin/sh
# verify.sh — vérification automatique du kext POMPPCGPU DANS l'invité Tiger,
# pensée pour tourner à l'aveugle en single-user (tapée via `sendkey`, résultat
# relu sur le disque depuis l'hôte). Tout est journalisé dans /pomppc/result.txt.
#
#   mount -uw / ; sh /pomppc/verify.sh
#
# Étapes : compile le kext, le charge (kextload -t), compile qgpu_test, le
# lance. Chaque étape écrit une ligne STEP=… ; la dernière ligne est VERDICT=.
R=/pomppc/result.txt
: > $R
say() { echo "$@" | tee -a $R; }
say "START=$(date)"
say "TOOLCHAIN gcc=$([ -x /usr/bin/gcc-4.0 ] && echo oui || echo NON) make=$([ -x /usr/bin/make ] && echo oui || echo NON)"

cd /pomppc/POMPPCGPU || { say "VERDICT=NO_SOURCES"; exit 1; }
make clean >/dev/null 2>&1
if make >> $R 2>&1; then say "STEP=build_kext OK"; else say "STEP=build_kext FAIL"; say "VERDICT=BUILD_FAIL"; exit 1; fi

chown -R root:wheel POMPPCGPU.kext; chmod -R 755 POMPPCGPU.kext
if /sbin/kextload -t POMPPCGPU.kext >> $R 2>&1; then say "STEP=kextload OK"; else say "STEP=kextload FAIL"; say "VERDICT=LOAD_FAIL"; exit 1; fi
/usr/sbin/kextstat 2>/dev/null | grep -i pomppc >> $R
/usr/sbin/ioreg -c POMPPCGPU -r 2>/dev/null | grep -iE "QGPU|POMPPCGPU" | head -8 >> $R

cd /pomppc/qgpu-test || { say "VERDICT=NO_TEST"; exit 1; }
make clean >/dev/null 2>&1
if make >> $R 2>&1; then say "STEP=build_test OK"; else say "STEP=build_test FAIL"; say "VERDICT=TEST_BUILD_FAIL"; exit 1; fi
if ./qgpu_test >> $R 2>&1; then say "STEP=run_test OK"; say "VERDICT=PASS"; else say "STEP=run_test FAIL"; say "VERDICT=TEST_FAIL"; exit 1; fi
