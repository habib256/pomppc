#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# smoke_appimage.sh <squashfs-root> <version> — les binaires PAQUETÉS tournent, sur une machine qui
# n'a pas construit le paquet. check_appimage.sh d'abord (il laisse squashfs-root/).
#
# Depuis squashfs-root et pas par --appimage-extract-and-run : même chemin (AppRun, RUNPATH), sans
# dépendre de FUSE, et sans qu'un AppImageLauncher installé avale l'option.
#
# Ce que ceci NE prouve pas (rien ne le peut sur un runner sans GPU ni disque Tiger) : le démarrage
# de Tiger, le backend GL de qgpu sur un vrai GPU, le son, le frontal ouvert sur un écran.
set -uo pipefail
R="$(cd "${1:?squashfs-root attendu}" && pwd)"
VER="${2:?version attendue}"
PAY="$R/usr/share/pomppc"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
echoues=0
verifie() { # verifie "libellé" commande…
  local l="$1"; shift
  if "$@"; then echo "ok   $l"; else echo "ÉCHEC $l"; echoues=$((echoues + 1)); fi
}
contient() { grep -qF -- "$2" <<< "$1"; }

echo "== AppRun"
export POMPPC_SANS_DIALOGUE=1
verifie "--version" test "$("$R/AppRun" --version)" = "$VER"
verifie "--help" contient "$("$R/AppRun" --help 2>&1)" "Usage: POMPPC-$VER-x86_64.AppImage"

echo "== chargeur"
# Tous les NEEDED se résolvent ici, RUNPATH compris, avec seulement les paquets du système.
PERDU="$(for f in "$PAY"/bin/* "$PAY"/lib/*; do ldd "$f" 2>/dev/null; done | awk '/not found/ { print $1 }' | sort -u)"
verifie "aucune bibliothèque introuvable (${PERDU:-rien})" test -z "$PERDU"
# Qu'elles viennent bien du paquet : hors de lib/, seuls l'hôte du contrat (GL, X, glibc).
HORS="$(ldd "$PAY/bin/qemu-system-ppc" | awk '$2 == "=>" { print $1, $3 }' | grep -E '^lib(glib|gio|gobject|gmodule|pixman|slirp|pulse)' | grep -v "$PAY/lib/" || true)"
verifie "glib, pixman, slirp, pulse pris dans le paquet (${HORS:-oui})" test -z "$HORS"

echo "== binaires"
QV="$(sed -n 's/^POMPPC_QEMU_VERSION=//p' "$PAY/config.env")"
for b in qemu-system-ppc qemu-system-ppc64; do
  verifie "$b --version = $QV" contient "$("$PAY/bin/$b" --version 2>&1 | head -1)" "version $QV"
done
verifie "qemu-img" "$PAY/bin/qemu-img" create -q -f qcow2 "$T/d.qcow2" 16M
# Le frontal n'a pas d'option : un lanceur absent le fait sortir après le chargement de toutes
# ses bibliothèques, avant toute fenêtre. Il ne faut donc pas d'écran, et c'est ce qui est prouvé.
verifie "frontal chargé" contient "$("$PAY/bin/pomppc" /nonexistent/run_tiger.sh 2>&1)" "launcher not found"

echo "== Open Firmware, QMP"
ofw() { # ofw <binaire> <args…> : la machine tourne quelques secondes, puis QMP répond
  local bin="$1"; shift
  { sleep 6
    printf '%s\n' '{"execute":"qmp_capabilities"}' '{"execute":"query-status"}' '{"execute":"quit"}'
  } | timeout 90 "$bin" -display none -qmp stdio "$@" 2>&1
}
# Sans -bios : le firmware de share/qemu, trouvé relativement au binaire.
SORTIE="$(ofw "$PAY/bin/qemu-system-ppc" -M mac99,via=pmu -cpu g4 -m 512)"
verifie "mac99 tourne, firmware de share/qemu" contient "$SORTIE" '"status": "running"'
# La ligne du lanceur : OpenBIOS unifié, deux cœurs, MTTCG sur la cible 32 bits.
SORTIE="$(ofw "$PAY/bin/qemu-system-ppc" -M mac99,via=pmu -cpu g4 -m 512 -smp 2 -accel tcg,thread=multi \
              -bios "$PAY/patches/smp-mac99/openbios-smp-screamer.elf")"
verifie "mac99 SMP, MTTCG, OpenBIOS unifié" contient "$SORTIE" '"status": "running"'
if contient "$SORTIE" "not yet converted to MTTCG"; then
  verifie "MTTCG déclaré sur la cible 32 bits" false
fi

echo "== capacités"
# La règle du dépôt : un binaire complet se sonde, il ne se déduit pas (scripts/caps.sh).
# shellcheck source=/dev/null
source "$PAY/scripts/caps.sh"
M="mac99,via=pmu"; B="$PAY/bin/qemu-system-ppc"; B64="${B}64"
verifie "screamer instancié"      qemu_machine_has "$B" "$M" screamer
verifie "qfb-pci"                 qemu_has_device "$B" qfb-pci
verifie "qgpu-pci"                qemu_has_device "$B" qgpu-pci
verifie "qgpu a un écran"         qemu_qgpu_has_scanout "$B" "$M"
verifie "slirp"                   qemu_has_netdev "$B" user
verifie "audio pa"                qemu_has_audiodev "$B" pa
verifie "-display dbus"           qemu_has_display "$B" dbus
verifie "-smp 2 ppc64"            qemu_machine_smp_ok "$B64" "$M" 2
verifie "mttcg ppc32"             qemu_ppc32_mttcg_ok "$B" "$M" 2
verifie "x-fast-fp"               qemu_cpu_has_fastfp "$B" "$M" g4
verifie "x-abs-margin"            qemu_dev_has_prop "$B" usb-tablet x-abs-margin
for p in x-lfs-known=on x-tlb-base=on x-fp-native=on x-tb-fast=on x-tlb-precise=on; do
  verifie "$p"                    qemu_cpu_has_prop "$B" "$M" g4 "$p"
done
verifie "x-tb-head"               qemu_tcg_has_prop "$B" "$M" x-tb-head=3
# Le backend GL se lie (check_appimage.sh), mais un runner sans GPU peut ne pas l'initialiser :
# dit, pas exigé. La vraie preuve est un jeu sur un vrai GPU (docs/publier-linux.md §5).
EGL_PLATFORM=device qemu_qgpu_has_backend "$B" "$M" gl && echo "note qgpu-gl utilisable sur ce runner" ||
  echo "note qgpu-gl non initialisé sur ce runner (attendu sans GPU)"

echo "== dossier de données"
export POMPPC_DATA="$T/POMPPC" POMPPC_NO_HOST_CHECK=1
verifie "AppRun prépare le dossier" "$R/AppRun" --prepare
verifie "lanceur copié" test -f "$POMPPC_DATA/run_tiger.sh"
verifie "lanceur pas lié" test ! -L "$POMPPC_DATA/run_tiger.sh"
verifie "bin/ lié" test -L "$POMPPC_DATA/bin"
# Le binaire joint par le LIEN de ~/POMPPC, comme le fait le lanceur : RUNPATH suit le vrai
# chemin (/proc/self/exe), et share/qemu aussi.
verifie "config.env trouve le QEMU du paquet" bash -c \
  'source "$1/config.env"; [ "$QEMU_BIN" = "$1/bin/qemu-system-ppc" ] && "$QEMU_BIN" --version >/dev/null && qemu-img --version >/dev/null' \
  _ "$POMPPC_DATA"

echo
echo "$echoues échec(s)"
[ "$echoues" = 0 ]
