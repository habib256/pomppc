#!/usr/bin/env bash
# run_tiger.sh — lance Mac OS X 10.4 (Tiger) dans une fenêtre.
#
#   ./run_tiger.sh            # SMP 2 cœurs (MTTCG) + SON, fenêtre GTK, disque persistant
#   SMP=1 ./run_tiger.sh      # mono-cœur + SON (chemin stable d'origine)
#   NOSOUND=1 ./run_tiger.sh  # coupe l'audio (rend la RAM pleine : 1024 au lieu de 768)
#   WIDE=1 ./run_tiger.sh     # 16:9 plein écran 1920x1080 (sinon RES=WxHxD au choix)
#   SNAPSHOT=1 ./run_tiger.sh # disque jetable (writes annulés -> boot toujours propre, pas de fsck)
#   NET=1 ./run_tiger.sh      # réseau (si ton QEMU a slirp compilé)
#   HEADLESS=1 ./run_tiger.sh # sans fenêtre (moniteur seul, pour scripting)
#   QFB=1 ./run_tiger.sh      # + écran paravirtuel qfb-pci (kext POMPPCQFB)
#   QFB_RES=1280x800          # mode par défaut proposé par l'écran QFB (avec QFB=1)
#   NOPAD=1 ./run_tiger.sh    # coupe le passthrough de la manette USB
#   NOCD=1 ./run_tiger.sh     # omet le lecteur CD amovible vide 'gamecd'
#   EXTRA_ARGS="-device ..."  # arguments QEMU supplémentaires
#
# Piloté par le frontend ImGui : DBUS_DISPLAY=1 (sortie -display dbus,p2p=on) et
# QMP_SOCK=<chemin> (socket QMP). POMPPC_SCRATCH déplace .run/ (socket moniteur).
#
# Build UNIFIÉ : QEMU 9.2 (device Screamer, patches/screamer/) + OpenBIOS fusionné
# (bring-up SMP balaton + nœud audio screamer). SMP *et* son ensemble, produits
# tous les deux par scripts/build_qemu_qfb.sh.
# Le son est SONDÉ, pas supposé : si le binaire n'a pas la classe 'screamer', le
# lanceur le dit, coupe l'audio et NE rabote PAS la RAM. (Un -global sur une
# classe absente n'est qu'un warning côté QEMU : rien ne signalait la panne.)
# SMP >= 2 : qemu-system-ppc64 (target MTTCG-safe) + réveil CPU secondaire via
# GPIO KeyLargo. Vérifie le nb de CPU dans « À propos de ce Mac ». Son via
# PulseAudio (activer la Mémoire Virtuelle côté invité aide). Fenêtre fermée -> quitte.
set -euo pipefail
USER_SMP="${SMP:-}"                        # intention user AVANT que config.env n'impose SMP=1
USER_RES="${RES:-}"                        # RES explicite de l'utilisateur, prioritaire
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT/config.env"
source "$ROOT/scripts/caps.sh"

# WIDE=1 -> 16:9 plein écran (1920x1080). Le ndrv stock offre nativement ce mode
# (contrairement à 1440x900/1600x900 qui retombent en 800x600). RES= reste prioritaire.
[ -n "${WIDE:-}" ] && [ -z "$USER_RES" ] && RES="1920x1080x32"
[ -n "$USER_RES" ] && RES="$USER_RES"

SMP_N="${USER_SMP:-2}"                     # défaut = 2 cœurs (la nouveauté)
# OpenBIOS UNIFIÉ : bring-up SMP (balaton) + nœud audio screamer (mcayland),
# buildé en -O1 (gcc-13 miscompile ce code OpenBIOS à -Os). Marche mono ET SMP.
UNI_OBIOS="$ROOT/patches/smp-mac99/openbios-smp-screamer.elf"
QEMU_BIN64="${QEMU_BIN}64"                # qemu-system-ppc -> qemu-system-ppc64

# --- Verrou disque ---
# flock plutôt que fuser : fuser vient de psmisc, et s'il manque le test
# échouait en silence — plus aucun garde-fou, deux QEMU sur le même qcow2,
# corruption. Le fd 9 reste ouvert à travers l'exec final : le verrou vit donc
# aussi longtemps que QEMU. En SNAPSHOT=1 les écritures sont jetées, plusieurs
# instances sont légitimes : pas de verrou.
SCR="${POMPPC_SCRATCH:-$ROOT/.run}"; mkdir -p "$SCR"
if [ -z "${SNAPSHOT:-}" ]; then
  exec 9>"$SCR/tiger.lock"
  if ! flock -n 9; then
    echo "⚠  Tiger tourne déjà (verrou $SCR/tiger.lock). Ferme-le d'abord," >&2
    echo "   ou lance en disque jetable : SNAPSHOT=1 ./run_tiger.sh" >&2
    exit 1
  fi
fi

# --- Sélection binaire / accélérateur / firmware ---
# Build UNIFIÉ (QEMU 9.2 + device Screamer porté) : SMP *et* son ensemble.
EXTRA=(); AUDIO=(); RAM="$RAM_MB"
[ -f "$UNI_OBIOS" ] || { echo "⚠  OpenBIOS unifié introuvable ($UNI_OBIOS)." >&2; exit 1; }
EXTRA+=(-bios "$UNI_OBIOS")

# Son ON par défaut ; NOSOUND=1 pour couper. On SONDE le binaire : sans la
# classe 'screamer' QEMU se contente d'un warning sur le -global, et on se
# retrouvait avec 768 Mo de RAM et zéro son sans le savoir.
SND_ON=0
if [ -z "${NOSOUND:-}" ]; then
  if qemu_machine_has "$QEMU_BIN" "$MACHINE" screamer; then
    AUDIO=(-audiodev pa,id=snd0 -global screamer.audiodev=snd0)
    export PULSE_SERVER="${PULSE_SERVER:-unix:/run/user/$(id -u)/pulse/native}"
    [ "$RAM" -gt 768 ] && RAM=768        # le Screamer exige < 1 Go
    SND_ON=1
  else
    echo "⚠  son indisponible : ce QEMU n'a pas le device 'screamer'." >&2
    echo "   Reconstruis le binaire de référence : ./scripts/build_qemu_qfb.sh" >&2
    echo "   (RAM laissée à ${RAM} Mo — pas de plafond 768 sans Screamer.)" >&2
  fi
fi

SND_TAG=""; [ "$SND_ON" = 1 ] && SND_TAG=" + SON"   # reflète le sondage, pas l'intention

if [ "$SMP_N" -ge 2 ]; then
  [ -x "$QEMU_BIN64" ] || { echo "⚠  SMP demandé mais $QEMU_BIN64 introuvable." >&2; exit 1; }
  BIN="$QEMU_BIN64"
  EXTRA+=(-accel tcg,thread=multi)
  MODE="SMP ${SMP_N} cœurs (MTTCG, ppc64)${SND_TAG}"
else
  BIN="$QEMU_BIN"
  MODE="mono-cœur${SND_TAG}"
fi

# --- Affichage ---
# DBUS_DISPLAY=1 : sortie via -display dbus,p2p=on pour le frontend ImGui
# (POMPPC/frontend). Le frontend fournit QMP_SOCK et se branche en add_client.
if [ -n "${DBUS_DISPLAY:-}" ]; then DISP="dbus,p2p=on";
elif [ -n "${HEADLESS:-}" ]; then DISP="none";
else DISP="gtk"; export DISPLAY="${DISPLAY:-:1}"; fi

# QMP pour pilotage par le frontend (add_client @dbus-display, reset, etc.).
QMP_ARGS=()
[ -n "${QMP_SOCK:-}" ] && QMP_ARGS=(-qmp "unix:$QMP_SOCK,server=on,wait=off")

# --- Réseau (coupé par défaut : build sans slirp) ---
if [ -n "${NET:-}" ]; then
  if qemu_has_netdev "$QEMU_BIN" user; then
    NET_ARGS=(-netdev "$NETDEV" -device "$NETNIC")
  else
    echo "⚠  NET=1 demandé mais ce QEMU n'a pas slirp — réseau coupé." >&2
    NET_ARGS=(-nic none); NET=""
  fi
else NET_ARGS=(-nic none); fi

# --- Disque jetable optionnel ---
SNAP_ARGS=(); [ -n "${SNAPSHOT:-}" ] && SNAP_ARGS=(-snapshot)

# --- Lecteur CD amovible 'gamecd' (vide) : insertion à chaud via le frontend
#     (QMP blockdev-change-medium) ou ./mount. NOCD=1 pour l'omettre. ---
CD_ARGS=(); [ -z "${NOCD:-}" ] && CD_ARGS=(-drive "id=gamecd,if=ide,media=cdrom")

# --- Écran paravirtuel QFB (device qfb-pci + kext POMPPCQFB) ---
#     QFB=1 ajoute un second écran piloté par notre kext ; l'écran VGA reste la
#     console Open Firmware. QFB_RES=LxH choisit le mode par défaut proposé.
QFB_ARGS=()
if [ -n "${QFB:-}" ]; then
  if qemu_has_device "$BIN" qfb-pci; then
    QFB_RES="${QFB_RES:-1280x800}"
    QFB_ARGS=(-device "qfb-pci,id=qfb0,width=${QFB_RES%x*},height=${QFB_RES#*x},depth=8")
    echo "  🖵  écran QFB ${QFB_RES} (second moniteur)"
  else
    echo "⚠  QFB=1 demandé mais ce QEMU n'a pas le device qfb-pci." >&2
    echo "   Reconstruis-le : ./scripts/build_qemu_qfb.sh" >&2
    exit 1
  fi
fi

# --- Arguments QEMU ad hoc : EXTRA_ARGS="-device ..." ./run_tiger.sh ---
read -r -a USER_EXTRA <<< "${EXTRA_ARGS:-}"

BOOTDEV='hd:10,\System\Library\CoreServices\BootX'
MON="$SCR/mon.sock"; rm -f "$MON"

echo "▶ Tiger : $MODE | cpu=$CPU ram=${RAM}Mo affichage=$DISP \
réseau=$([ -n "${NET:-}" ] && echo on || echo off) \
disque=$([ -n "${SNAPSHOT:-}" ] && echo jetable || echo persistant)"
echo "  moniteur QEMU : $MON"
[ "$SMP_N" -ge 2 ] && echo "  (1er boot en persistant = fsck possible ~1min ; ensuite rapide)"

# --- Manette USB : auto-passthrough (idem run_os9.sh) ; NOPAD=1 pour couper ---
PAD_ARGS=()
if [ -z "${NOPAD:-}" ] && [ -e /dev/input/js0 ] \
   && qemu_has_device "$BIN" usb-host; then
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
  -drive "file=$DISK,format=qcow2,media=disk" "${SNAP_ARGS[@]}" "${CD_ARGS[@]}" \
  "${NET_ARGS[@]}" "${EXTRA[@]}" "${AUDIO[@]}" \
  -device usb-tablet "${PAD_ARGS[@]}" "${QFB_ARGS[@]}" "${USER_EXTRA[@]}" \
  -prom-env 'auto-boot?=true' \
  -prom-env "boot-device=$BOOTDEV" \
  -prom-env 'boot-args=-v' \
  -name "Tiger" \
  "${QMP_ARGS[@]}" \
  -monitor "unix:$MON,server,nowait"
