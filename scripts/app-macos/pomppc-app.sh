#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# pomppc-app.sh — démarrage de POMPPC.app : prépare ~/POMPPC, puis passe la main au frontal, qui
# déroule l'assistant de premier lancement s'il le faut (frontend/src/SetupFlow.h).
#
# L'app ne se modifie pas : les données vivent dans ~/POMPPC (POMPPC_DATA pour un autre dossier).
# Ce dossier a la forme du dépôt, parce que tous les scripts déduisent leur racine de leur propre
# emplacement : disks/, images/ et .run/ y sont réels, les quatre fichiers lanceurs y sont copiés
# (le frontal résout les liens et prendrait sinon l'app pour racine), le reste y est un lien vers
# l'app, refait à chaque lancement pour suivre une mise à jour ou un déplacement.
set -u
export PATH="/usr/bin:/bin:/usr/sbin:/sbin"
RES="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
APP="$(cd "$RES/../.." && pwd -P)"
SRC="$RES/pomppc"
D="${POMPPC_DATA:-$HOME/POMPPC}"

erreur() {
  osascript -e 'on run argv' \
    -e 'display dialog (item 1 of argv) with title "POMPPC" buttons {"OK"} default button "OK"' \
    -e 'end run' -- "$1" >/dev/null 2>&1
  exit 1
}

preparer_donnees() {
  mkdir -p "$D/disks" "$D/images" "$D/.run" "$D/bench" || erreur "Could not create the folder $D."
  local e nom c
  for e in "$D"/* "$D"/.[!.]*; do                # liens d'une version précédente
    [ -L "$e" ] && case "$(readlink "$e")" in */POMPPC.app/*) rm -f -- "$e" ;; esac
  done
  for e in "$SRC"/* "$SRC"/.[!.]*; do
    [ -e "$e" ] || continue
    nom="$(basename "$e")"
    case "$nom" in
      run_tiger.sh|run_os9.sh|run_frontend.sh|config.env)
        cp -f "$e" "${D:?}/${nom:?}" ;;
      disks|images|.run|bench)
        for c in "$e"/*; do
          [ -e "$c" ] || continue
          if [ -L "$D/$nom/$(basename "$c")" ] || [ ! -e "$D/$nom/$(basename "$c")" ]; then
            ln -sfn "$c" "$D/$nom/$(basename "$c")"
          fi
        done ;;
      *)
        if [ -L "$D/$nom" ] || [ ! -e "$D/$nom" ]; then ln -sfn "$e" "$D/$nom"; fi ;;
    esac
    # Dans le Finder, seuls disks/ et images/ regardent l'utilisateur.
    case "$nom" in disks|images) ;; *) chflags -h hidden "$D/$nom" 2>/dev/null ;; esac
  done
}

# Le dernier système utilisé, s'il a son disque ; sinon Tiger, sauf s'il n'y a qu'OS 9.
lanceur_a_ouvrir() {
  local os
  os="$(sed -n 's/^os = //p' "$D/.run/imguidock.conf" 2>/dev/null | head -1)"
  if [ -f "$D/disks/os9.qcow2" ] && { [ "$os" = os9 ] || [ ! -f "$D/disks/tiger.qcow2" ]; }; then
    echo run_os9.sh
  else
    echo run_tiger.sh
  fi
}

principal() {
  # Une app téléchargée garde sa quarantaine sur chaque fichier : l'accord donné à l'app ne
  # couvre pas QEMU, lancé ensuite par un script. Sans effet si l'app n'est pas modifiable.
  xattr -dr com.apple.quarantine "$APP" 2>/dev/null
  preparer_donnees
  local lanceur verrou
  lanceur="$(lanceur_a_ouvrir)"
  verrou=tiger.lock
  if [ "$lanceur" = run_os9.sh ]; then verrou=os9.lock; fi
  # La fenêtre du frontal n'a plus de terminal pour dire ce refus du lanceur.
  : >> "$D/.run/$verrou"
  if ! perl -e 'use Fcntl ":flock"; open(my $f, "<", $ARGV[0]) or exit 0;
                exit(flock($f, LOCK_EX | LOCK_NB) ? 0 : 1)' "$D/.run/$verrou"; then
    erreur "POMPPC is already running."
  fi
  # Le frontal sait qu'il est dans l'app : assistant, rappel du pilote.
  POMPPC_APP=1
  POMPPC_VERSION="$(cat "$RES/version" 2>/dev/null)"
  export POMPPC_APP POMPPC_VERSION
  # Sans outils de développement, /usr/bin/python3 ouvrirait une fenêtre d'installation. Le relais
  # web de Tiger est le seul à en dépendre au démarrage : il attend ces outils.
  xcode-select -p >/dev/null 2>&1 || export WEBPROXY=0
  # Pour l'utilisateur, le démarrage de Mac OS X plutôt que le texte du noyau.
  export POMPPC_BOOT_ARGS=""
  cd "$D" || exit 1
  exec >>"$D/.run/app.log" 2>&1
  exec "$D/run_frontend.sh" "$D/$lanceur"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then principal; fi
