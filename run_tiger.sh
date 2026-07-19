#!/usr/bin/env bash
# run_tiger.sh — lance Mac OS X 10.4 (Tiger) dans une fenêtre.
#
#   ./run_tiger.sh            # SMP 2 cœurs (MTTCG) + SON, fenêtre GTK, disque persistant
#   SMP=1 ./run_tiger.sh      # mono-cœur + SON (chemin stable d'origine)
#   NOSOUND=1 ./run_tiger.sh  # coupe l'audio (permet RAM 1024 au lieu de 768)
#   WIDE=1 ./run_tiger.sh     # 16:9 plein écran 1920x1080 (sinon RES=WxHxD au choix)
#   SNAPSHOT=1 ./run_tiger.sh # disque jetable (writes annulés -> boot toujours propre, pas de fsck)
#   NET=1 ./run_tiger.sh      # réseau (si ton QEMU a slirp compilé)
#   HEADLESS=1 ./run_tiger.sh # sans fenêtre (moniteur seul, pour scripting)
#
# Build UNIFIÉ : QEMU 9.2 (device Screamer porté du fork mcayland) + OpenBIOS
# fusionné (bring-up SMP balaton + nœud audio screamer). SMP *et* son ensemble.
# SMP >= 2 : qemu-system-ppc64 (target MTTCG-safe) + réveil CPU secondaire via
# GPIO KeyLargo. Vérifie le nb de CPU dans « À propos de ce Mac ». Son via
# PulseAudio (activer la Mémoire Virtuelle côté invité aide). Fenêtre fermée -> quitte.
set -euo pipefail
USER_SMP="${SMP:-}"                        # intention user AVANT que config.env n'impose SMP=1
USER_RES="${RES:-}"                        # RES explicite de l'utilisateur, prioritaire
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT/config.env"

# WIDE=1 -> 16:9 plein écran (1920x1080). Le ndrv stock offre nativement ce mode
# (contrairement à 1440x900/1600x900 qui retombent en 800x600). RES= reste prioritaire.
[ -n "${WIDE:-}" ] && [ -z "$USER_RES" ] && RES="1920x1080x32"
[ -n "$USER_RES" ] && RES="$USER_RES"

SMP_N="${USER_SMP:-2}"                     # défaut = 2 cœurs (la nouveauté)
# OpenBIOS UNIFIÉ : bring-up SMP (balaton) + nœud audio screamer (mcayland),
# buildé en -O1 (gcc-13 miscompile ce code OpenBIOS à -Os). Marche mono ET SMP.
UNI_OBIOS="$ROOT/patches/smp-mac99/openbios-smp-screamer.elf"
QEMU_BIN64="${QEMU_BIN}64"                # qemu-system-ppc -> qemu-system-ppc64

# --- Disque déjà verrouillé ? ---
if fuser "$DISK" >/dev/null 2>&1; then
  echo "⚠  $DISK est déjà utilisé par un autre QEMU. Ferme-le d'abord." >&2
  exit 1
fi

# --- Sélection binaire / accélérateur / firmware ---
# Build UNIFIÉ (QEMU 9.2 + device Screamer porté) : SMP *et* son ensemble.
EXTRA=(); AUDIO=(); RAM="$RAM_MB"
[ -f "$UNI_OBIOS" ] || { echo "⚠  OpenBIOS unifié introuvable ($UNI_OBIOS)." >&2; exit 1; }
EXTRA+=(-bios "$UNI_OBIOS")

# Son ON par défaut (Screamer intégré au build 9.2) ; NOSOUND=1 pour couper.
if [ -z "${NOSOUND:-}" ]; then
  AUDIO=(-audiodev pa,id=snd0 -global screamer.audiodev=snd0)
  export PULSE_SERVER="${PULSE_SERVER:-unix:/run/user/$(id -u)/pulse/native}"
  [ "$RAM" -gt 768 ] && RAM=768          # le Screamer exige < 1 Go
fi

if [ "$SMP_N" -ge 2 ]; then
  [ -x "$QEMU_BIN64" ] || { echo "⚠  SMP demandé mais $QEMU_BIN64 introuvable." >&2; exit 1; }
  BIN="$QEMU_BIN64"
  EXTRA+=(-accel tcg,thread=multi)
  MODE="SMP ${SMP_N} cœurs (MTTCG, ppc64)$([ -z "${NOSOUND:-}" ] && echo ' + SON')"
else
  BIN="$QEMU_BIN"
  MODE="mono-cœur$([ -z "${NOSOUND:-}" ] && echo ' + SON')"
fi

# --- Affichage ---
if [ -n "${HEADLESS:-}" ]; then DISP="none"; else DISP="gtk"; export DISPLAY="${DISPLAY:-:1}"; fi

# --- Réseau (coupé par défaut : build sans slirp) ---
if [ -n "${NET:-}" ]; then NET_ARGS=(-netdev "$NETDEV" -device "$NETNIC"); else NET_ARGS=(-nic none); fi

# --- Disque jetable optionnel ---
SNAP_ARGS=(); [ -n "${SNAPSHOT:-}" ] && SNAP_ARGS=(-snapshot)

BOOTDEV='hd:10,\System\Library\CoreServices\BootX'
SCR="${POMPPC_SCRATCH:-$ROOT/.run}"; mkdir -p "$SCR"
MON="$SCR/mon.sock"; rm -f "$MON"

echo "▶ Tiger : $MODE | cpu=$CPU ram=${RAM}Mo affichage=$DISP \
réseau=$([ -n "${NET:-}" ] && echo on || echo off) \
disque=$([ -n "${SNAPSHOT:-}" ] && echo jetable || echo persistant)"
echo "  moniteur QEMU : $MON"
[ "$SMP_N" -ge 2 ] && echo "  (1er boot en persistant = fsck possible ~1min ; ensuite rapide)"

# --- Manette USB : auto-passthrough (idem run_os9.sh) ; NOPAD=1 pour couper ---
PAD_ARGS=()
if [ -z "${NOPAD:-}" ] && [ -e /dev/input/js0 ] \
   && "$BIN" -device help 2>/dev/null | grep -q '"usb-host"'; then
  PAD_VID=$(udevadm info -q property -n /dev/input/js0 2>/dev/null | sed -n 's/^ID_VENDOR_ID=//p')
  PAD_PID=$(udevadm info -q property -n /dev/input/js0 2>/dev/null | sed -n 's/^ID_MODEL_ID=//p')
  PAD_USB="/dev/bus/usb/$(lsusb 2>/dev/null | awk -v v="$PAD_VID" -v p="$PAD_PID" 'tolower($6)==v":"p{printf "%s/%s", $2, substr($4,1,3)}')"
  if [ -n "$PAD_VID" ] && [ -n "$PAD_PID" ] && [ -w "$PAD_USB" ]; then
    PAD_ARGS=(-device "usb-host,vendorid=0x$PAD_VID,productid=0x$PAD_PID")
    echo "  🎮 manette $PAD_VID:$PAD_PID → passthrough"
  fi
fi

exec "$BIN" -M "$MACHINE" -cpu "$CPU" -m "$RAM" -smp "$SMP_N" \
  -display "$DISP" -g "$RES" \
  -drive "file=$DISK,format=qcow2,media=disk" "${SNAP_ARGS[@]}" \
  "${NET_ARGS[@]}" "${EXTRA[@]}" "${AUDIO[@]}" \
  -device usb-tablet "${PAD_ARGS[@]}" \
  -prom-env 'auto-boot?=true' \
  -prom-env "boot-device=$BOOTDEV" \
  -prom-env 'boot-args=-v' \
  -name "Tiger" \
  -monitor "unix:$MON,server,nowait"
