#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# pomppc-app.sh — démarrage de POMPPC.app : prépare ~/POMPPC, accompagne le premier lancement
# par des fenêtres, puis lance Tiger.
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
VERSION="$(cat "$RES/version")"

fr() { case "$(defaults read -g AppleLocale 2>/dev/null)" in fr*) return 0 ;; *) return 1 ;; esac; }
t() { if fr; then printf '%s' "$1"; else printf '%s' "$2"; fi; }

# dialogue "message" bouton… : rend le bouton choisi, le dernier est celui par défaut.
dialogue() {
  osascript -e 'on run argv' \
    -e 'set b to items 2 thru -1 of argv' \
    -e 'set r to display dialog (item 1 of argv) with title "POMPPC" buttons b default button (count of b)' \
    -e 'return button returned of r' -e 'end run' -- "$@" 2>/dev/null
}
# choisir "message" : rend un chemin, rien si l'utilisateur annule.
choisir() {
  osascript -e 'on run argv' -e 'return POSIX path of (choose file with prompt (item 1 of argv))' \
    -e 'end run' -- "$1" 2>/dev/null
}
erreur() {
  dialogue "$1" "OK" >/dev/null
  exit 1
}

# Une app téléchargée garde sa quarantaine sur chaque fichier : l'accord donné à l'app ne
# couvre pas QEMU, lancé ensuite par un script. Sans effet si l'app n'est pas modifiable.
xattr -dr com.apple.quarantine "$APP" 2>/dev/null

preparer_donnees() {
  mkdir -p "$D/disks" "$D/images" "$D/.run" "$D/bench" ||
    erreur "$(t "Impossible de créer le dossier $D." "Could not create the folder $D.")"
  local e nom c
  for e in "$D"/* "$D"/.[!.]*; do                # liens d'une version précédente
    [ -L "$e" ] && case "$(readlink "$e")" in */POMPPC.app/*) rm -f "$e" ;; esac
  done
  for e in "$SRC"/* "$SRC"/.[!.]*; do
    [ -e "$e" ] || continue
    nom="$(basename "$e")"
    case "$nom" in
      run_tiger.sh|run_os9.sh|run_frontend.sh|config.env)
        rm -f "$D/$nom"
        cp "$e" "$D/$nom" ;;
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
  done
}

importer_disque() {
  local f
  f="$(choisir "$(t "Choisis ton disque Tiger (.qcow2)." "Choose your Tiger disk (.qcow2).")")"
  [ -n "$f" ] || exit 0
  "$SRC/bin/qemu-img" info "$f" 2>/dev/null | grep -q 'file format: qcow2' ||
    erreur "$(t "Ce fichier n'est pas un disque .qcow2." "This file is not a .qcow2 disk.")"
  cp -c "$f" "$D/disks/tiger.qcow2" 2>/dev/null || cp "$f" "$D/disks/tiger.qcow2" ||
    erreur "$(t "La copie du disque a échoué." "Copying the disk failed.")"
}

installer_tiger() {
  local f sortie="$D/images/MacOSX.4"
  f="$(choisir "$(t "Choisis ton média d'installation de Mac OS X 10.4 Tiger PowerPC (.iso, .cdr ou .dmg)." \
                    "Choose your Mac OS X 10.4 Tiger PowerPC install media (.iso, .cdr or .dmg).")")"
  [ -n "$f" ] || exit 0
  rm -f "$sortie.iso"
  case "$f" in
    *.dmg|*.DMG)
      rm -f "$sortie.cdr"
      if ! hdiutil convert "$f" -format UDTO -o "$sortie" >/dev/null 2>&1 || ! mv "$sortie.cdr" "$sortie.iso"; then
        erreur "$(t "La conversion du .dmg a échoué." "Converting the .dmg failed.")"
      fi ;;
    *) ln -s "$f" "$sortie.iso" ;;
  esac
  dialogue "$(t "L'installateur de Tiger va démarrer, et prendra un moment sous émulation.

Dans Utilitaire de disque, efface le disque (Mac OS étendu, journalisé), puis installe Tiger dessus.
À la fin, ferme la fenêtre et rouvre POMPPC." \
"The Tiger installer will start. It takes a while under emulation.

In Disk Utility, erase the disk (Mac OS Extended, Journaled), then install Tiger on it.
When it finishes, close the window and open POMPPC again.")" "$(t "Continuer" "Continue")" >/dev/null || exit 0
  [ -f "$D/disks/tiger.qcow2" ] || "$D/scripts/00-create-disk.sh" >>"$D/.run/app.log" 2>&1 ||
    erreur "$(t "La création du disque a échoué : voir $D/.run/app.log" "Creating the disk failed: see $D/.run/app.log")"
  : > "$D/.run/installation-en-cours"
  "$D/scripts/10-install.sh" >>"$D/.run/app.log" 2>&1
  exit 0
}

dire_pilote() {
  [ "$(cat "$D/.run/pilote-annonce" 2>/dev/null)" = "$VERSION" ] && return 0
  dialogue "$(t "Dernière étape, à faire une fois par version de POMPPC, sinon la 3D manque ou le bureau peut s'afficher rayé.

Quand Tiger a démarré, ouvre Terminal (dans Applications, Utilitaires) et tape :

sudo sh /Volumes/POMPPC_GUEST/install.sh

Redémarre ensuite Tiger." \
"Last step, once per POMPPC version, or 3D will be missing and the desktop may look striped.

Once Tiger has started, open Terminal (in Applications, Utilities) and type:

sudo sh /Volumes/POMPPC_GUEST/install.sh

Then restart Tiger.")" "OK" >/dev/null || exit 0
  printf '%s\n' "$VERSION" > "$D/.run/pilote-annonce"
}

preparer_donnees

if [ -f "$D/.run/installation-en-cours" ]; then
  case "$(dialogue "$(t "L'installation de Tiger est-elle terminée ?" "Has the Tiger installation finished?")" \
          "$(t "Quitter" "Quit")" "$(t "Reprendre l'installation" "Resume installing")" \
          "$(t "Oui, lancer Tiger" "Yes, start Tiger")")" in
    "Oui, lancer Tiger"|"Yes, start Tiger") rm -f "$D/.run/installation-en-cours" ;;
    "Reprendre l'installation"|"Resume installing") installer_tiger ;;
    *) exit 0 ;;
  esac
elif [ ! -f "$D/disks/tiger.qcow2" ]; then
  case "$(dialogue "$(t "Bienvenue dans POMPPC.

Il faut Mac OS X 10.4 Tiger pour PowerPC : ton propre média d'installation, ou un disque POMPPC existant. Apple n'est pas distribué ici. Tes données seront dans $D." \
"Welcome to POMPPC.

You need Mac OS X 10.4 Tiger for PowerPC: your own install media, or an existing POMPPC disk. Nothing of Apple's is included. Your data will be in $D.")" \
          "$(t "Quitter" "Quit")" "$(t "Utiliser un disque existant" "Use an existing disk")" \
          "$(t "Installer Tiger" "Install Tiger")")" in
    "Installer Tiger"|"Install Tiger") installer_tiger ;;
    "Utiliser un disque existant"|"Use an existing disk") importer_disque ;;
    *) exit 0 ;;
  esac
fi

# La fenêtre du frontal n'a plus de terminal pour dire ce refus de run_tiger.sh.
: >> "$D/.run/tiger.lock"
if ! perl -e 'use Fcntl ":flock"; open(my $f, "<", $ARGV[0]) or exit 0;
              exit(flock($f, LOCK_EX | LOCK_NB) ? 0 : 1)' "$D/.run/tiger.lock"; then
  erreur "$(t "Tiger tourne déjà." "Tiger is already running.")"
fi

dire_pilote

# Sans outils de développement, /usr/bin/python3 ouvrirait une fenêtre d'installation. Le relais
# web de Tiger est le seul à en dépendre au démarrage : il attend ces outils.
xcode-select -p >/dev/null 2>&1 || export WEBPROXY=0

cd "$D" || exit 1
exec >>"$D/.run/app.log" 2>&1
exec "$D/run_tiger.sh"
