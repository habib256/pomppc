#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# smoke_windows.sh <dossier du paquet> <version> <dossier de construction du frontal> — les binaires
# PAQUETÉS tournent, avec un PATH de machine neuve : System32 seul. Dans MSYS2, sur le runner.
#
# Les bancs viennent de la construction : launchplan_test (la ligne de QEMU) et bridge_probe (lance cette
# ligne pour de bon, puis le bus D-Bus, le listener et la surface, contre Open Firmware).
#
# Ce que ceci NE prouve pas, et rien ne le peut sur ce runner sans GPU, sans son ni disque Tiger : le
# rendu GL de qgpu sur un vrai pilote, le démarrage de Tiger, le son, la fenêtre du frontal, le clavier.
# docs/publier-windows.md §5.
set -uo pipefail
R="$(cd "${1:?dossier du paquet attendu}" && pwd)"
VER="${2:?version attendue}"
B="$(cd "${3:?construction du frontal attendue}" && pwd)"
T="$(mktemp -d)"
DATA="$T/data"
mkdir -p "$DATA/.run" "$DATA/disks"
cleanup() { rm -rf "$T"; }
trap cleanup EXIT

# La copie du paquet : les bancs y sont déposés (le chargeur cherche les DLL à côté de l'exe), le zip non.
PAQ="$T/paquet"
cp -R "$R" "$PAQ"
cp "$B/bridge_probe.exe" "$B/launchplan_test.exe" "$PAQ/"

export PATH="/c/Windows/System32:/c/Windows:/usr/bin"
# /usr/bin reste pour timeout, grep, sleep, mais les .exe natifs n'y trouvent aucune DLL de MinGW :
# /ucrt64/bin n'est plus dans le PATH.

echoues=0
verifie() { # verifie "libellé" commande…
  local l="$1"; shift
  if "$@"; then echo "ok   $l"; else echo "ÉCHEC $l"; echoues=$((echoues + 1)); fi
}
contient() { grep -qF -- "$2" <<< "$1"; }

QV="$(sed -n 's/^POMPPC_QEMU_VERSION=//p' "$(dirname "$0")/../../config.env")"

echo "== binaires"
verifie "POMPPC.exe --version = $VER" test "$("$PAQ/POMPPC.exe" --version 2>&1 | tr -d '\r')" = "$VER"
for b in qemu-system-ppc qemu-system-ppc64; do
  verifie "$b --version = $QV" contient "$("$PAQ/$b.exe" --version 2>&1 | head -1)" "version $QV"
done
verifie "-display help : dbus" contient "$("$PAQ/qemu-system-ppc.exe" -display help 2>&1 | tr -d '\r')" "dbus"
verifie "-audiodev help : dsound" contient "$("$PAQ/qemu-system-ppc.exe" -audiodev help 2>&1 | tr -d '\r')" "dsound"
verifie "qemu-img" "$PAQ/qemu-img.exe" create -q -f qcow2 "$(cygpath -m "$DATA/disks/tiger.qcow2")" 64M
verifie "la ligne de QEMU (launchplan_test)" "$PAQ/launchplan_test.exe"

echo "== Open Firmware, QMP"
ofw() { # la machine tourne quelques secondes, puis QMP répond
  { sleep 6
    printf '%s\n' '{"execute":"qmp_capabilities"}' '{"execute":"query-status"}' '{"execute":"quit"}'
  } | timeout 120 "$PAQ/qemu-system-ppc.exe" -L "$(cygpath -m "$PAQ/share/qemu")" -display none -qmp stdio "$@" 2>&1 | tr -d '\r'
}
SORTIE="$(ofw -M mac99,via=pmu -cpu g4 -m 512)"
verifie "mac99 tourne, firmware de share/qemu" contient "$SORTIE" '"status": "running"'
SORTIE="$(ofw -M mac99,via=pmu -cpu g4 -m 512 -smp 2 -accel tcg,thread=multi \
              -bios "$(cygpath -m "$PAQ/patches/smp-mac99/openbios-smp-screamer.elf")")"
verifie "mac99 SMP, MTTCG, OpenBIOS unifié" contient "$SORTIE" '"status": "running"'
if contient "$SORTIE" "not yet converted to MTTCG"; then verifie "MTTCG déclaré sur la cible 32 bits" false; fi

echo "== le bus, le listener, la surface, l'image (bridge_probe contre Open Firmware)"
export POMPPC_PACKAGE_ROOT
POMPPC_PACKAGE_ROOT="$(cygpath -m "$PAQ")"
LANCEUR="$(cygpath -m "$DATA/run_tiger.sh")"
SORTIE="$(cd "$T" && timeout 180 "$PAQ/bridge_probe.exe" "$LANCEUR" 40 2>&1 | tr -d '\r')"
printf '%s\n' "$SORTIE"
echo "-- qemu.log"; cat "$DATA/.run/qemu.log" 2>/dev/null || true
verifie "première image reçue" contient "$SORTIE" "FIRST FRAME"
# Win32.Map vaut Unix.Map : le chemin rapide. QEMU retombe en silence sur Scanout en ligne s'il ne peut
# pas (GUnixConnection, PID du pair, DuplicateHandle) : c'est la preuve que ces trois-là tiennent.
verifie "surface partagée (Win32.Map)" contient "$SORTIE" "ScanoutMap (Win32.Map)"
COUL="$(sed -n 's/.*image colors=\([0-9]*\).*/\1/p' <<< "$SORTIE" | tail -1)"
verifie "image non vide, ${COUL:-0} couleurs, hachée" test "${COUL:-0}" -gt 1
sed -n 's/.*\(image colors=.*\)/note \1/p' <<< "$SORTIE"
if contient "$(cat "$DATA/.run/qemu.log" 2>/dev/null)" "backend gl refusé"; then
  echo "note qgpu retombé sur le backend logiciel (attendu sur ce runner sans GPU)"
fi

echo "== le verrou (obligatoire sous Windows : QEMU ouvre le qcow2 en partage)"
export PROBE_PERSISTENT=1
"$PAQ/bridge_probe.exe" "$LANCEUR" 30 > "$T/premier.log" 2>&1 &
PREMIER=$!
for _ in $(seq 1 40); do
  if grep -qF "FIRST FRAME" "$T/premier.log" 2>/dev/null; then break; fi
  sleep 1
done
verifie "le premier tient le disque" grep -qF "FIRST FRAME" "$T/premier.log"
verifie "le verrou existe" test -f "$DATA/.run/tiger.lock"
SECOND="$(timeout 60 "$PAQ/bridge_probe.exe" "$LANCEUR" 5 2>&1 | tr -d '\r')"
verifie "le second est refusé" contient "$SECOND" "already running"
wait "$PREMIER" || true
verifie "le fichier du verrou reste après la fin" test -f "$DATA/.run/tiger.lock"
unset PROBE_PERSISTENT

echo
echo "$echoues échec(s)"
[ "$echoues" = 0 ]
