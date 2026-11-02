#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# pomppc-app.sh — démarrage de POMPPC.app : prépare ~/POMPPC, déroule l'assistant de premier
# lancement s'il n'y a aucun disque, puis lance le système installé. Textes en anglais.
#
# L'app ne se modifie pas : les données vivent dans ~/POMPPC (POMPPC_DATA pour un autre dossier).
# Ce dossier a la forme du dépôt, parce que tous les scripts déduisent leur racine de leur propre
# emplacement : disks/, images/ et .run/ y sont réels, les quatre fichiers lanceurs y sont copiés
# (le frontal résout les liens et prendrait sinon l'app pour racine), le reste y est un lien vers
# l'app, refait à chaque lancement pour suivre une mise à jour ou un déplacement.
#
# L'assistant ouvre la page du média sur Macintosh Garden et attend le fichier : il ne le télécharge
# pas lui-même. Mac OS reste sous droit d'auteur d'Apple, le téléchargement reste un geste de
# l'utilisateur (docs/publier-macos.md). Les MD5 sont ceux que publie le site (arch_md5.php).
set -u
export PATH="/usr/bin:/bin:/usr/sbin:/sbin"
RES="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
APP="$(cd "$RES/../.." && pwd -P)"
SRC="$RES/pomppc"
D="${POMPPC_DATA:-$HOME/POMPPC}"
TELECHARGEMENTS="${POMPPC_DOWNLOADS:-$HOME/Downloads}"
VERSION="$(cat "$RES/version" 2>/dev/null)"

TIGER_NOM="Mac OS X 10.4 Tiger"
TIGER_PAGE="https://macintoshgarden.org/apps/mac-osx-mac-os-10-ppc"
TIGER_FICHIER="tiger-10-4-6-ppc.iso"
TIGER_MD5="4764d50fecb9f9370b626fc146f09954"
OS9_NOM="Mac OS 9.2.2"
OS9_PAGE="https://macintoshgarden.org/apps/mac-os-922-universal"
OS9_FICHIER="macos-922-uni.zip"
OS9_MD5="5ba031dfd678a74b9dee414af93ea514"
# [B] Apple publie ce fichier gratuitement, et son SHA-1 : https://support.apple.com/kb/dl170.
# L'assistant peut donc le télécharger lui-même, contrairement au système complet.
COMBO_URL="https://download.info.apple.com/Mac_OS_X/061-3461.20071114.8Uy45/MacOSXUpdCombo10.4.11PPC.dmg"
COMBO_SHA1="3d403bfa769424c61a3cfac173f8527658f9d4af"

SCRIPT_DIALOGUE='on run argv
  set b to items 2 thru -1 of argv
  set r to display dialog (item 1 of argv) with title "POMPPC" buttons b default button (count of b)
  return button returned of r
end run'

# dialogue "message" bouton… : rend le bouton choisi, le dernier est celui par défaut.
dialogue() { osascript -e "$SCRIPT_DIALOGUE" -- "$@" 2>/dev/null; }
# choisir "message" : rend un chemin, rien si l'utilisateur annule.
choisir() {
  osascript -e 'on run argv' -e 'return POSIX path of (choose file with prompt (item 1 of argv))' \
    -e 'end run' -- "$1" 2>/dev/null
}
# liste "message" élément… : rend l'élément choisi (le premier est présélectionné), rien si annulé.
liste() {
  osascript -e 'on run argv' -e 'set l to items 2 thru -1 of argv' \
    -e 'set r to choose from list l with title "POMPPC" with prompt (item 1 of argv) default items {item 1 of l}' \
    -e 'if r is false then return ""' -e 'return item 1 of r' -e 'end run' -- "$@" 2>/dev/null
}
# saisie "message" "valeur" : rend le texte tapé, rien si annulé.
saisie() {
  osascript -e 'on run argv' \
    -e 'return text returned of (display dialog (item 1 of argv) with title "POMPPC" default answer (item 2 of argv))' \
    -e 'end run' -- "$1" "$2" 2>/dev/null
}
erreur() {
  dialogue "$1" "OK" >/dev/null
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

disque_de() { if [ "$1" = tiger ]; then echo "$D/disks/tiger.qcow2"; else echo "$D/disks/os9.qcow2"; fi; }
nom_de() { if [ "$1" = tiger ]; then echo "$TIGER_NOM"; else echo "$OS9_NOM"; fi; }

# taille_valide "texte" systeme : un nombre entier de Go, assez grand pour ce système.
taille_valide() {
  local minimum=1
  [ "$2" = tiger ] && minimum=5
  case "$1" in ''|*[!0-9]*) return 1 ;; esac
  [ "${#1}" -le 4 ] && [ "$1" -ge "$minimum" ] && [ "$1" -le 2048 ]
}

creer_disque() {
  local os="$1" choix go libre nom
  nom="$(nom_de "$os")"
  if [ "$os" = tiger ]; then
    choix="$(liste "Size of the new $nom disk. It only takes the space it really uses on your Mac." \
             "16 GB (recommended)" "32 GB" "64 GB" "128 GB" "Custom...")"
  else
    choix="$(liste "Size of the new $nom disk. It only takes the space it really uses on your Mac." \
             "8 GB (recommended)" "2 GB" "16 GB" "32 GB" "Custom...")"
  fi
  [ -n "$choix" ] || exit 0
  case "$choix" in
    Custom*)
      go="$(saisie "Disk size in GB:" "16")"
      [ -n "$go" ] || exit 0
      taille_valide "$go" "$os" ||
        erreur "This size isn't valid. $nom needs a whole number of GB, at least $([ "$os" = tiger ] && echo 5 || echo 1)." ;;
    *) go="${choix%% *}" ;;
  esac
  libre="$(df -g "$D" 2>/dev/null | awk 'NR==2{print $4}')"
  if [ -n "$libre" ] && [ "$go" -gt "$libre" ]; then
    case "$(dialogue "This disk can grow to $go GB, but your Mac only has $libre GB free. $nom may run out of space later." "Quit" "Continue")" in
      Continue) ;;
      *) exit 0 ;;
    esac
  fi
  "$SRC/bin/qemu-img" create -f qcow2 "$(disque_de "$os")" "${go}G" >>"$D/.run/app.log" 2>&1 ||
    erreur "Creating the disk failed: see $D/.run/app.log"
  # Avant le combo, Tiger n'a pas la marge de tablette de 10.4.11 : le pointeur serait décalé
  # (docs/tablette-tiger-10.4.11.md). preparer_combo la rétablit une fois le combo passé.
  if [ "$os" = tiger ]; then echo 0 > "$(disque_de tiger).tablet-margin"; fi
}

importer_disque() {
  local f
  f="$(choisir "Choose your $(nom_de "$1") disk (.qcow2).")"
  [ -n "$f" ] || exit 0
  "$SRC/bin/qemu-img" info "$f" 2>/dev/null | grep -q 'file format: qcow2' ||
    erreur "This file is not a .qcow2 disk."
  cp -c "$f" "$(disque_de "$1")" 2>/dev/null || cp "$f" "$(disque_de "$1")" ||
    erreur "Copying the disk failed."
}

# Le navigateur écrit à côté (.download, .crdownload, .part), puis renomme.
fichier_pret() {
  [ -s "$1" ] && [ ! -e "$1.download" ] && [ ! -e "$1.crdownload" ] && [ ! -e "$1.part" ]
}

# attendre_fichier chemin nom page : ouvre la page, rend le chemin du fichier obtenu.
attendre_fichier() {
  local chemin="$1" nom="$2" page="$3" sortie pid reponse
  if ! fichier_pret "$chemin"; then
    open "$page"
    sortie="$D/.run/attente-dialogue"
    : > "$sortie"
    osascript -e "$SCRIPT_DIALOGUE" -- "Macintosh Garden has opened in your browser. In the list of files, find $nom and click one of its links.

POMPPC is waiting for it in your Downloads folder and will continue on its own. If macOS asks, allow POMPPC to access Downloads." \
      "Quit" "Choose a File..." >"$sortie" 2>/dev/null &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
      if fichier_pret "$chemin"; then
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        printf '%s\n' "$chemin"
        return 0
      fi
      sleep 2
    done
    wait "$pid" 2>/dev/null
    reponse="$(cat "$sortie")"
    case "$reponse" in
      "Choose a File...")
        chemin="$(choisir "Choose the file you downloaded.")"
        [ -n "$chemin" ] || return 1 ;;
      *) return 1 ;;
    esac
  fi
  printf '%s\n' "$chemin"
}

# controler_md5 chemin somme : 0 si bon, ou si l'utilisateur choisit de continuer.
controler_md5() {
  [ "$(md5 -q "$1" 2>/dev/null)" = "$2" ] && return 0
  case "$(dialogue "This file doesn't match the checksum published by Macintosh Garden. It may be incomplete or a different version, so the installation may fail." "Quit" "Use It Anyway")" in
    "Use It Anyway") return 0 ;;
    *) return 1 ;;
  esac
}

# extraire_media archive sortie : la plus grosse image de disque de l'archive devient sortie.
extraire_media() {
  local dossier="${D:?}/.run/extraction" image r
  rm -rf -- "${D:?}/.run/extraction"
  mkdir -p "$dossier"
  unzip -q -o "$1" -d "$dossier" >>"$D/.run/app.log" 2>&1 || return 1
  image="$(find "$dossier" -type f \( -iname '*.iso' -o -iname '*.toast' -o -iname '*.cdr' -o -iname '*.img' -o -iname '*.dmg' \) \
           -not -path '*/__MACOSX/*' -exec stat -f '%z %N' {} + 2>/dev/null | sort -n | tail -1 | cut -d' ' -f2-)"
  if [ -z "$image" ]; then
    rm -rf -- "${D:?}/.run/extraction"
    return 1
  fi
  ranger_media "$image" "$2" mv
  r=$?
  rm -rf -- "${D:?}/.run/extraction"
  return $r
}

# ranger_media fichier sortie mv|lien : le média à l'emplacement que lit le lanceur.
ranger_media() {
  local f="$1" sortie="${2:?}" geste="$3" base="${2%.*}"
  rm -f -- "$sortie"
  case "$f" in
    *.zip|*.ZIP) extraire_media "$f" "$sortie" ;;
    *.dmg|*.DMG)
      rm -f -- "${base:?}.cdr"
      hdiutil convert "$f" -format UDTO -o "$base" >/dev/null 2>&1 && mv "$base.cdr" "$sortie" ;;
    *)
      if [ "$geste" = mv ]; then mv "$f" "$sortie"; else ln -s "$f" "$sortie"; fi ;;
  esac
}

obtenir_media() {
  local os="$1" sortie page fichier somme f reponse nom
  nom="$(nom_de "$os")"
  if [ "$os" = tiger ]; then
    sortie="$D/images/MacOSX.4.iso"; page="$TIGER_PAGE"; fichier="$TIGER_FICHIER"; somme="$TIGER_MD5"
  else
    sortie="$D/disks/os9-install.iso"; page="$OS9_PAGE"; fichier="$OS9_FICHIER"; somme="$OS9_MD5"
  fi
  [ -s "$sortie" ] && return 0
  reponse="$(dialogue "POMPPC needs the $nom installation disc.

Macintosh Garden, an independent archive not linked to Apple, keeps a copy: $fichier. Mac OS remains Apple's copyrighted software, so only download it if you're entitled to use it." \
             "Quit" "I Have the Disc" "Open Macintosh Garden")"
  case "$reponse" in
    "Open Macintosh Garden")
      f="$(attendre_fichier "$TELECHARGEMENTS/$fichier" "$fichier" "$page")" || exit 0
      if [ "$(basename "$f")" = "$fichier" ]; then
        controler_md5 "$f" "$somme" || exit 0
        ranger_media "$f" "$sortie" mv || erreur "POMPPC couldn't use this file. No disc image was found in it."
      else
        ranger_media "$f" "$sortie" lien || erreur "POMPPC couldn't use this file. No disc image was found in it."
      fi ;;
    "I Have the Disc")
      f="$(choisir "Choose your $nom installation disc (.iso, .toast, .cdr, .dmg or .zip).")"
      [ -n "$f" ] || exit 0
      ranger_media "$f" "$sortie" lien || erreur "POMPPC couldn't use this file. No disc image was found in it." ;;
    *) exit 0 ;;
  esac
}

lancer_installateur() {
  local os="$1"
  printf '%s\n' "$os" > "$D/.run/installation-en-cours"
  dialogue "The $(nom_de "$os") installer will now start. It takes a while under emulation.

In Disk Utility (in the installer's Utilities menu), erase the disk as Mac OS Extended, then install on it. When it finishes, close the window and open POMPPC again." "Continue" >/dev/null || exit 0
  if [ "$os" = tiger ]; then
    "$D/scripts/10-install.sh" >>"$D/.run/app.log" 2>&1
  else
    POMPPC_FRONTEND=native "$D/run_os9.sh" install >>"$D/.run/app.log" 2>&1
  fi
  exit 0
}

assistant() {
  local os reponse
  case "$(dialogue "Welcome to POMPPC.

Which system do you want to set up? Your data will be in $D." \
           "Quit" "$OS9_NOM" "$TIGER_NOM")" in
    "$TIGER_NOM") os=tiger ;;
    "$OS9_NOM") os=os9 ;;
    *) exit 0 ;;
  esac
  reponse="$(dialogue "$(nom_de "$os") needs a hard disk." "Quit" "Use an Existing Disk" "Create a New Disk")"
  case "$reponse" in
    "Use an Existing Disk") importer_disque "$os" ;;
    "Create a New Disk")
      creer_disque "$os"
      obtenir_media "$os"
      lancer_installateur "$os" ;;
    *) exit 0 ;;
  esac
}

reprendre_installation() {
  local os
  os="$(cat "$D/.run/installation-en-cours")"
  case "$(dialogue "Has the $(nom_de "$os") installation finished?" "Quit" "Resume Installing" "Yes, Start It")" in
    "Yes, Start It")
      rm -f -- "${D:?}/.run/installation-en-cours"
      if [ "$os" = tiger ]; then : > "$D/.run/combo-a-proposer"; fi ;;
    "Resume Installing") lancer_installateur "$os" ;;
    *) exit 0 ;;
  esac
}

# telecharger_combo sortie : le combo d'Apple, avec une fenêtre pour annuler. 1 si annulé ou raté.
telecharger_combo() {
  local sortie="$1" pc pd
  curl -fsSL --retry 3 -o "$sortie.tmp" "$COMBO_URL" 2>>"$D/.run/app.log" &
  pc=$!
  osascript -e "$SCRIPT_DIALOGUE" -- "Downloading the Mac OS X 10.4.11 update from Apple (181 MB)..." "Cancel" >/dev/null 2>&1 &
  pd=$!
  while kill -0 "$pc" 2>/dev/null; do
    if ! kill -0 "$pd" 2>/dev/null; then
      kill "$pc" 2>/dev/null
      wait "$pc" 2>/dev/null
      rm -f -- "${sortie:?}.tmp"
      return 1
    fi
    sleep 1
  done
  kill "$pd" 2>/dev/null
  wait "$pd" 2>/dev/null
  wait "$pc" || { rm -f -- "${sortie:?}.tmp"; return 1; }
  if [ "$(shasum -a 1 "$sortie.tmp" | cut -d' ' -f1)" != "$COMBO_SHA1" ]; then
    rm -f -- "${sortie:?}.tmp"
    return 1
  fi
  mv "$sortie.tmp" "$sortie"
}

# preparer_combo : 0 si le combo est prêt à installer ; 1 si l'utilisateur le saute.
preparer_combo() {
  local dmg="$D/disks/MacOSXUpdCombo10.4.11PPC.dmg" iso="$D/disks/combo-10.4.11.iso"
  case "$(dialogue "Tiger needs its last update, 10.4.11, for POMPPC's 3D games and mouse to work as tested. Apple offers it for free.

Install it before the 3D driver." "Quit" "Skip" "Download from Apple")" in
    "Download from Apple") ;;
    Skip) return 1 ;;
    *) exit 0 ;;
  esac
  until [ -s "$dmg" ] || telecharger_combo "$dmg"; do
    case "$(dialogue "The download didn't finish, or the file doesn't match Apple's checksum." "Quit" "Try Again")" in
      "Try Again") ;;
      *) exit 0 ;;
    esac
  done
  ranger_media "$dmg" "$iso" lien || erreur "Preparing the update failed: see $D/.run/app.log"
  rm -f -- "${dmg:?}"
  : > "$D/.run/combo-en-cours"
  return 0
}

dire_combo() {
  dialogue "Tiger will now start with the update's CD on its desktop.

1. Open the CD and double-click MacOSXUpdCombo10.4.11PPC.pkg.
2. Follow the installer, then restart Tiger when it asks. It takes a while under emulation.
3. Once Tiger is back, quit POMPPC and open it again." "OK" >/dev/null || exit 0
}

# etape_combo : 0 si Tiger doit démarrer avec le CD du combo, 1 sinon.
etape_combo() {
  if [ -f "$D/.run/combo-a-proposer" ]; then
    rm -f -- "${D:?}/.run/combo-a-proposer"
    if preparer_combo; then dire_combo; return 0; fi
    return 1
  fi
  [ -f "$D/.run/combo-en-cours" ] || return 1
  case "$(dialogue "Has the 10.4.11 update finished, and has Tiger restarted?" "Quit" "Not Yet" "Yes")" in
    Yes)
      rm -f -- "${D:?}/.run/combo-en-cours" "${D:?}/disks/combo-10.4.11.iso" "${D:?}/disks/tiger.qcow2.tablet-margin"
      return 1 ;;
    "Not Yet") dire_combo; return 0 ;;
    *) exit 0 ;;
  esac
}

dire_pilote() {
  [ "$(cat "$D/.run/pilote-annonce" 2>/dev/null)" = "$VERSION" ] && return 0
  dialogue "One last step, once per POMPPC version, or 3D will be missing and the desktop may look striped.

Once Tiger has started, open Terminal (in Applications, Utilities) and type:

sudo sh /Volumes/POMPPC_GUEST/install.sh

Then restart Tiger." "OK" >/dev/null || exit 0
  printf '%s\n' "$VERSION" > "$D/.run/pilote-annonce"
}

principal() {
  # Une app téléchargée garde sa quarantaine sur chaque fichier : l'accord donné à l'app ne
  # couvre pas QEMU, lancé ensuite par un script. Sans effet si l'app n'est pas modifiable.
  xattr -dr com.apple.quarantine "$APP" 2>/dev/null
  preparer_donnees
  if [ -f "$D/.run/installation-en-cours" ]; then
    reprendre_installation
  elif [ ! -f "$D/disks/tiger.qcow2" ] && [ ! -f "$D/disks/os9.qcow2" ]; then
    assistant
  fi
  local lanceur=run_os9.sh verrou=os9.lock
  if [ -f "$D/disks/tiger.qcow2" ]; then lanceur=run_tiger.sh; verrou=tiger.lock; fi
  # La fenêtre du frontal n'a plus de terminal pour dire ce refus du lanceur.
  : >> "$D/.run/$verrou"
  if ! perl -e 'use Fcntl ":flock"; open(my $f, "<", $ARGV[0]) or exit 0;
                exit(flock($f, LOCK_EX | LOCK_NB) ? 0 : 1)' "$D/.run/$verrou"; then
    erreur "POMPPC is already running."
  fi
  # Le combo avant le pilote : POMPPCFsqrt est lié à la version exacte du noyau.
  if [ "$lanceur" = run_tiger.sh ]; then
    if etape_combo; then export POMPPC_CD="$D/disks/combo-10.4.11.iso"; else dire_pilote; fi
  fi
  # Sans outils de développement, /usr/bin/python3 ouvrirait une fenêtre d'installation. Le relais
  # web de Tiger est le seul à en dépendre au démarrage : il attend ces outils.
  xcode-select -p >/dev/null 2>&1 || export WEBPROXY=0
  cd "$D" || exit 1
  exec >>"$D/.run/app.log" 2>&1
  exec "$D/run_frontend.sh" "$D/$lanceur"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then principal; fi
