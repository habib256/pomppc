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

export PATH="/usr/bin:/c/Windows/System32:/c/Windows"
# /usr/bin d'abord : [A] CI du 04/11/2026, System32 a son propre timeout.exe, qui refuse cette syntaxe,
# et son find et son sort. Les .exe natifs n'y trouvent aucune DLL de MinGW (/ucrt64/bin n'est plus
# dans le PATH).

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
  # Par TCP : [A] CI du 04/11/2026, `-qmp stdio` sur un tube ne répond rien sous Windows. Bash parle
  # le socket lui-même : pas de Python dans ce PATH, et c'est le paquet qu'on éprouve, pas un outil.
  local log="$T/ofw.log"
  timeout 120 "$PAQ/qemu-system-ppc.exe" -L "$(cygpath -m "$PAQ/share/qemu")" -display none \
    -qmp tcp:127.0.0.1:44555,server=on,wait=off "$@" > "$log" 2>&1 &
  local q=$!
  sleep 6
  if { exec 3<>/dev/tcp/127.0.0.1/44555; } 2>/dev/null; then
    # quit seulement après la réponse : [A] CI du 04/11/2026, le socket se ferme par un reset sous
    # Windows, qui peut jeter ce qui n'a pas encore été lu.
    printf '%s\n' '{"execute":"qmp_capabilities"}' '{"execute":"query-status"}' >&3
    local l
    while IFS= read -r -t 20 l <&3; do
      printf '%s\n' "$l" | tr -d '\r'
      case "$l" in *'"status"'*) break ;; esac
    done
    printf '%s\n' '{"execute":"quit"}' >&3
    while IFS= read -r -t 20 l <&3; do printf '%s\n' "$l" | tr -d '\r'; done 2>/dev/null
    exec 3<&-
  else
    echo "QMP injoignable"
  fi
  wait "$q" || true
  tr -d '\r' < "$log"
}
SORTIE="$(ofw -M mac99,via=pmu -cpu g4 -m 512)"
verifie "mac99 tourne, firmware de share/qemu" contient "$SORTIE" '"status": "running"'
contient "$SORTIE" '"status": "running"' || printf '%s\n' "$SORTIE"
SORTIE="$(ofw -M mac99,via=pmu -cpu g4 -m 512 -smp 2 -accel tcg,thread=multi \
              -bios "$(cygpath -m "$PAQ/patches/smp-mac99/openbios-smp-screamer.elf")")"
verifie "mac99 SMP, MTTCG, OpenBIOS unifié" contient "$SORTIE" '"status": "running"'
contient "$SORTIE" '"status": "running"' || printf '%s\n' "$SORTIE"
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
echo "-- premier.log"; tr -d '\r' < "$T/premier.log" | tail -20
echo "-- second"; printf '%s\n' "$SECOND" | tail -5
verifie "le fichier du verrou reste après la fin" test -f "$DATA/.run/tiger.lock"
unset PROBE_PERSISTENT

echo
echo "$echoues échec(s)"
[ "$echoues" = 0 ]
