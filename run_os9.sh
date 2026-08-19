#!/usr/bin/env bash
# run_os9.sh — lance Mac OS 9.2.2 sur QEMU (mac99, mono-cœur), sans intervention.
#
#   ./run_os9.sh            # AUTO : boote le disque installé si présent,
#                           #        sinon boote le CD 9.2.2 en live (bureau direct)
#   ./run_os9.sh install    # force le boot CD (pour (ré)installer sur le disque)
#   ./run_os9.sh disk       # force le boot du disque installé
#   OS9_CD=/chemin/os9.iso ./run_os9.sh install   # CD d'install ailleurs que disks/
#   CDR=<image> ./run_os9.sh          # monte une image CD de disks/cdr/ (ou chemin absolu)
#   TABLET=1 ./run_os9.sh   # souris absolue via usb-tablet (via=cuda + extension à installer
#                           # d'abord — voir disks/extras/README.md ; INUTILE par défaut :
#                           # le partage virtio donne déjà la souris absolue en via=pmu)
#   (dossier partagé ./shared monté PAR DÉFAUT au boot disque, volume 'Shared' + souris absolue virtio)
#   SHARE=/chemin ./run_os9.sh  # partage un autre dossier hôte à la place de ./shared
#   NOSHARE=1 ./run_os9.sh  # coupe le partage/virtio (boot disque nu)
#   (manette USB auto-passthrough si branchée + accessible ; NOPAD=1 pour couper)
#   NOSOUND=1 ./run_os9.sh  # coupe l'audio (repli sur l'OpenBIOS stock)
#   SNAPSHOT=1 ./run_os9.sh # disque jetable (writes annulés -> boot toujours propre)
#
# Piloté par le frontend ImGui : DBUS_DISPLAY=1 (sortie -display dbus,p2p=on) et
# QMP_SOCK=<chemin> (socket QMP). POMPPC_SCRATCH déplace .run/ (socket moniteur).
#
# OS 9 = mono-cœur (SMP OS 9 buggé). Fenêtre GTK ; ferme-la pour quitter.
# Auto-boot activé : aucune invite OpenFirmware. (Secours '0 >' : boot cd:,\:tbxi)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT/config.env"

# OS9_DISK / OS9_CD viennent de config.env (surchargeables par l'environnement).
RAM=512
MODE="${1:-auto}"

# --- Souris absolue/fluide via USB tablet (extension invité kanjitalk755) ---
# Voie SECONDAIRE : le partage virtio ci-dessous fournit déjà virtio-tablet-pci, donc la
# souris absolue en via=pmu sans rien installer. TABLET=1 n'a d'intérêt que si tu veux
# spécifiquement usb-tablet (ex. NOSHARE=1).
# L'extension USBTabletINIT EXIGE via=cuda (elle plante sous via=pmu) et doit être installée
# à la main dans le Dossier Système. Le dépôt fournit l'archive .sit, pas un CD prêt à
# monter : recette de fabrication du CD dans disks/extras/README.md.
VIA=pmu; TABLET_DEV=()
if [ -n "${TABLET:-}" ]; then
  VIA=cuda
  TABLET_DEV=(-device usb-tablet)
fi

export DISPLAY="${DISPLAY:-:1}"
SCR="${POMPPC_SCRATCH:-$ROOT/.run}"; mkdir -p "$SCR"
MON="$SCR/os9-mon.sock"; rm -f "$MON"
[ -f "$OS9_DISK" ] || qemu-img create -f qcow2 "$OS9_DISK" "$OS9_DISK_SIZE" >/dev/null

# --- SON via le build UNIFIÉ (QEMU 9.2 + device Screamer porté + OpenBIOS fusionné) ---
# Un seul binaire pour OS 9 ET Tiger. L'OpenBIOS fusionné publie le nœud audio ;
# il faut le passer explicitement (l'OpenBIOS stock du 9.2 n'a pas le screamer).
BIN="$QEMU_BIN"
UNI_OBIOS="$ROOT/patches/smp-mac99/openbios-smp-screamer.elf"
BIOS_ARGS=()
if [ -f "$UNI_OBIOS" ] && [ -z "${NOSOUND:-}" ]; then
  BIOS_ARGS=(-bios "$UNI_OBIOS")
  AUDIO=(-audiodev pa,id=snd0 -global screamer.audiodev=snd0)
  export PULSE_SERVER="${PULSE_SERVER:-unix:/run/user/$(id -u)/pulse/native}"
  SOUND="son ON (Screamer intégré 9.2 + PulseAudio)"
else
  AUDIO=(); SOUND="son OFF (OpenBIOS stock)"
fi

# --- Le disque a-t-il un système installé ? (heuristique : > 200 Mo de données écrites) ---
disk_installed() {
  local alloc
  alloc=$(qemu-img map --output=json "$OS9_DISK" 2>/dev/null \
          | python3 -c 'import sys,json;print(sum(e.get("length",0) for e in json.load(sys.stdin) if e.get("data")))' 2>/dev/null || echo 0)
  [ "${alloc:-0}" -gt 209715200 ]   # 200 Mo
}

# --- Décider quoi booter ---
case "$MODE" in
  disk)    BOOT=disk ;;
  install) BOOT=cd ;;
  auto|*)  if disk_installed; then BOOT=disk; else BOOT=cd; fi ;;
esac

DRIVES=(-drive "file=$OS9_DISK,format=qcow2,media=disk")
GAMES_ISO="$ROOT/disks/games.iso"

# --- CD de jeu supplémentaire : CDR=<image .cdr/.iso/.img> ./run_os9.sh ---
# OS 9 le monte tout seul (comme insérer un CD). Chemin absolu ou relatif à disks/cdr/.
if [ -n "${CDR:-}" ]; then
  [ -f "$CDR" ] || CDR="$ROOT/disks/cdr/$CDR"
  if [ -f "$CDR" ]; then DRIVES+=(-drive "file=$CDR,format=raw,media=cdrom")
  else echo "⚠ image CD introuvable : $CDR" >&2; fi
fi

# --- Dossier partagé hôte↔OS9 via virtio-9p (drivers elliotnunn, injectés au boot) ---
# ACTIVÉ PAR DÉFAUT (partage ./shared) au boot du disque. NOSHARE=1 pour couper.
# SHARE=/chemin/abs pour partager un autre dossier. Le ndrvloader injecte les pilotes
# virtio SANS installation invité + monte le volume "Shared" sur le bureau. Bonus :
# virtio-tablet = souris absolue (marche en via=pmu, sans extension).
VIRTIO_ARGS=(); BOOTCMD_ARGS=()
NDRVLOADER="$ROOT/disks/extras/classicvirtio/ndrv/ndrvloader"
# défaut : partage ./shared ; désactivé si NOSHARE=1, en mode install, ou si loader absent
SHARE="${SHARE:-1}"
[ -n "${NOSHARE:-}" ] && SHARE=""
[ "$BOOT" != disk ] && SHARE=""            # pas de virtio pendant l'install CD
if [ -n "$SHARE" ]; then
  [ "$SHARE" = 1 ] && SHARE="$ROOT/shared"
  if [ -x "$NDRVLOADER" ]; then
    mkdir -p "$SHARE"
    VIRTIO_ARGS=(
      -device "loader,addr=0x4000000,file=$NDRVLOADER"
      -device virtio-9p-pci,fsdev=sh0,mount_tag=Shared
      -fsdev "local,id=sh0,security_model=none,path=$SHARE"
      -device virtio-tablet-pci
    )
    BOOTCMD_ARGS=(-prom-env 'boot-command=init-program go')
  else
    echo "⚠ ndrvloader absent ($NDRVLOADER) → partage désactivé (NOSHARE implicite)" >&2
    SHARE=""
  fi
fi

# --- Manette USB : auto-passthrough vers OS 9 (driver HID générique) ---
# ACTIVÉ PAR DÉFAUT si une manette est branchée ET accessible. NOPAD=1 pour couper.
# Conditions de sécurité (sinon on skippe sans casser le boot) : QEMU compilé avec
# usb-host (libusb) + /dev/input/js0 présent + nœud USB accessible en écriture (règle
# udev pack/99-qemu-gamepad.rules). L'ID vendor:product est trouvé via udevadm.
PAD_ARGS=()
if [ -z "${NOPAD:-}" ] && [ "$BOOT" = disk ] && [ -e /dev/input/js0 ] \
   && "$BIN" -device help 2>/dev/null | grep -q '"usb-host"'; then
  PAD_VID=$(udevadm info -q property -n /dev/input/js0 2>/dev/null | sed -n 's/^ID_VENDOR_ID=//p')
  PAD_PID=$(udevadm info -q property -n /dev/input/js0 2>/dev/null | sed -n 's/^ID_MODEL_ID=//p')
  PAD_NODE=$(udevadm info -q property -n /dev/input/js0 2>/dev/null | sed -n 's/^DEVNAME=//p')
  # nœud USB brut accessible en écriture ? (via la règle udev)
  PAD_USB="/dev/bus/usb/$(lsusb 2>/dev/null | awk -v v="$PAD_VID" -v p="$PAD_PID" 'tolower($6)==v":"p{printf "%s/%s", $2, substr($4,1,3)}')"
  if [ -n "$PAD_VID" ] && [ -n "$PAD_PID" ] && [ -w "$PAD_USB" ]; then
    PAD_ARGS=(-device "usb-host,vendorid=0x$PAD_VID,productid=0x$PAD_PID")
    PAD_INFO="manette $PAD_VID:$PAD_PID → passthrough OS 9"
  elif [ -n "$PAD_VID" ]; then
    echo "ℹ manette $PAD_VID:$PAD_PID détectée mais nœud USB non accessible → applique la règle udev (voir pack/99-qemu-gamepad.rules), ou NOPAD=1" >&2
  fi
fi

if [ "$BOOT" = disk ]; then
  BOOTDEV='hd:,\\:tbxi'
  # En usage normal on monte le CD de jeux (pas le CD d'install)
  [ -f "$GAMES_ISO" ] && DRIVES+=(-drive "file=$GAMES_ISO,format=raw,media=cdrom")
  # Lecteur CD amovible dédié (vide) pour l'échange à chaud via ./mount
  DRIVES+=(-drive "id=gamecd,if=ide,media=cdrom")
  echo "▶ Mac OS 9 — boot du DISQUE installé (auto-boot)"
  [ -f "$GAMES_ISO" ] && echo "  CD 'OS9_GAMES' monté → décompresse les jeux avec StuffIt Expander"
  echo "  lecteur 'gamecd' prêt → ./mount <nom>  pour insérer un CD à chaud"
else
  [ -f "$OS9_CD" ] || { echo "⚠ CD d’installation OS 9 introuvable : $OS9_CD" >&2
                        echo "   dépose ton ISO 9.2.2 dans disks/ sous ce nom, ou : OS9_CD=/chemin/os9.iso ./run_os9.sh $MODE" >&2
                        exit 1; }
  DRIVES+=(-drive "file=$OS9_CD,format=raw,media=cdrom")
  BOOTDEV='cd:,\\:tbxi'
  if [ "$MODE" = install ]; then
    echo "▶ Mac OS 9.2.2 — boot CD pour INSTALLER (Utilities→Drive Setup, puis Mac OS Install)"
  else
    echo "▶ Mac OS 9.2.2 — boot CD en LIVE (pas encore installé ; bureau direct, sans intervention)"
  fi
fi
echo "  $SOUND  |  moniteur QEMU : $MON"

[ -n "${SHARE:-}" ] && echo "  dossier partagé : $SHARE → volume 'Shared' sur le bureau (+ souris absolue virtio)"
[ -n "${PAD_INFO:-}" ] && echo "  🎮 $PAD_INFO"

# Affichage : DBUS_DISPLAY=1 → -display dbus,p2p=on pour le frontend ImGui.
if [ -n "${DBUS_DISPLAY:-}" ]; then DISP="dbus,p2p=on"; else DISP="gtk"; fi
QMP_ARGS=()
[ -n "${QMP_SOCK:-}" ] && QMP_ARGS=(-qmp "unix:$QMP_SOCK,server=on,wait=off")
# SNAPSHOT=1 : disque jetable (writes annulés → boot toujours propre).
SNAP_ARGS=(); [ -n "${SNAPSHOT:-}" ] && SNAP_ARGS=(-snapshot)

exec "$BIN" -M "mac99,via=$VIA" -cpu g4 -m "$RAM" -smp 1 \
  -display "$DISP" -g "$RES" "${BIOS_ARGS[@]}" "${SNAP_ARGS[@]}" \
  "${DRIVES[@]}" -nic none "${AUDIO[@]}" "${TABLET_DEV[@]}" "${VIRTIO_ARGS[@]}" "${PAD_ARGS[@]}" \
  -prom-env 'auto-boot?=true' -prom-env "boot-device=$BOOTDEV" "${BOOTCMD_ARGS[@]}" \
  -name "MacOS9" "${QMP_ARGS[@]}" -monitor "unix:$MON,server,nowait"
