#!/bin/bash
# shellcheck disable=SC2329 # fonctions appelées par le script testé
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# app_wizard_test.sh — l'assistant de POMPPC.app, réponses de l'utilisateur simulées (macOS).
set -u
RACINE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "${T:?}"' EXIT
echoues=0
verifie() { # verifie "libellé" commande…
  local l="$1"; shift
  if "$@"; then echo "ok   $l"; else echo "ÉCHEC $l"; echoues=$((echoues + 1)); fi
}
absent() { [ ! -e "$1" ] && [ ! -L "$1" ]; }

# Une app minimale : le vrai script, un faux qemu-img, quelques fichiers de paquet.
R="$T/POMPPC.app/Contents/Resources"
mkdir -p "$R/pomppc/bin" "$R/pomppc/scripts" "$R/pomppc/disks/extras"
cp "$RACINE/scripts/app-macos/pomppc-app.sh" "$R/"
echo 9.9 > "$R/version"
cat > "$R/pomppc/bin/qemu-img" <<'FAUX'
#!/bin/bash
case "$1" in
  create) echo "$5" > "$4" ;;
  info) grep -q qcow "$2" && echo "file format: qcow2" ;;
esac
FAUX
chmod +x "$R/pomppc/bin/qemu-img"
for f in run_tiger.sh run_os9.sh run_frontend.sh config.env; do echo "app" > "$R/pomppc/$f"; done
echo doc > "$R/pomppc/README.md"

export POMPPC_DATA="$T/donnees" POMPPC_DOWNLOADS="$T/dl"
mkdir -p "$POMPPC_DOWNLOADS"
# shellcheck source=/dev/null
source "$R/pomppc-app.sh"
# Dans un fichier : les réponses sont lues depuis des sous-shells, par $(dialogue …).
rep() { if [ $# -gt 0 ]; then printf '%s\n' "$@" > "$T/reponses"; else : > "$T/reponses"; fi; }
reponse() { head -1 "$T/reponses"; sed -i '' 1d "$T/reponses"; }
rep
dialogue() { reponse; }
choisir() { reponse; }
liste() { reponse; }
saisie() { reponse; }
open() { :; }
osascript() { sleep 30; }
sortie_de() { ( "$@" ) >/dev/null 2>&1; }

echo "== dossier de données"
mkdir -p "$D/disks"
echo "mon disque" > "$D/disks/tiger.qcow2"
ln -s /x/POMPPC.app/ancien "$D/perime"
echo "à moi" > "$D/README.md"
preparer_donnees
verifie "disque de l'utilisateur intact" grep -q "mon disque" "$D/disks/tiger.qcow2"
verifie "fichier réel de l'utilisateur non remplacé" grep -q "à moi" "$D/README.md"
verifie "lien d'une ancienne version retiré" absent "$D/perime"
verifie "lanceur copié, pas lié" test ! -L "$D/run_tiger.sh"
verifie "scripts/ lié à l'app" test -L "$D/scripts"
verifie "contenu de disks/ de l'app lié" test -L "$D/disks/extras"
masque() { stat -f '%Sf' "$1" | grep -q hidden; }
verifie "technique masqué dans le Finder" masque "$D/scripts"
visible() { ! masque "$1"; }
verifie "disks visible" visible "$D/disks"
rm -f "$D/disks/tiger.qcow2"

echo "== taille du disque"
valide() { taille_valide "$1" "$2"; }
invalide() { ! taille_valide "$1" "$2"; }
verifie "16 Go pour Tiger" valide 16 tiger
verifie "4 Go refusés pour Tiger" invalide 4 tiger
verifie "1 Go pour OS 9" valide 1 os9
verifie "texte refusé" invalide 12a tiger
verifie "vide refusé" invalide "" os9
verifie "négatif refusé" invalide -3 os9
verifie "démesuré refusé" invalide 99999999999999999999 os9
rep "32 GB"; creer_disque tiger
verifie "choix de la liste" grep -q '^32G$' "$D/disks/tiger.qcow2"
rm -f "$D/disks/tiger.qcow2"
rep "Custom..." "20"; creer_disque os9
verifie "taille personnalisée" grep -q '^20G$' "$D/disks/os9.qcow2"
rm -f "$D/disks/os9.qcow2"
rep "Custom..." "abc" "OK"
sortie_de creer_disque tiger
verifie "taille invalide : pas de disque" absent "$D/disks/tiger.qcow2"
rep ""
sortie_de creer_disque tiger
verifie "annulation : pas de disque" absent "$D/disks/tiger.qcow2"
df() { printf 'x\ny y y 1 y\n'; }
rep "64 GB" "Quit"
sortie_de creer_disque tiger
verifie "place insuffisante, Quit : pas de disque" absent "$D/disks/tiger.qcow2"
unset -f df

echo "== téléchargement"
F="$POMPPC_DOWNLOADS/f.iso"
echo x > "$F"
pret() { fichier_pret "$F"; }
pas_pret() { ! fichier_pret "$F"; }
verifie "fichier complet" pret
for s in download crdownload part; do
  : > "$F.$s"
  verifie "en cours (.$s)" pas_pret
  rm -f "$F.$s"
done
rm -f "$F"; : > "$F"
verifie "fichier vide : pas prêt" pas_pret
rm -f "$F"
( sleep 3; echo fini > "$F" ) &
debut=$SECONDS
r="$(attendre_fichier "$F" f.iso http://exemple)"
wait
verifie "attente : fichier détecté" test "$r" = "$F"
verifie "attente : sans attendre le dialogue" test $((SECONDS - debut)) -lt 20
pkill -f 'sleep 30' 2>/dev/null
echo bon > "$F"
somme="$(md5 -q "$F")"
verifie "MD5 bon" controler_md5 "$F" "$somme"
rep "Quit"; mauvais_quitte() { ! controler_md5 "$F" 00; }
verifie "MD5 faux, Quit" mauvais_quitte
rep "Use It Anyway"; verifie "MD5 faux, continuer" controler_md5 "$F" 00
rm -f "$F"

echo "== média"
Z="$T/z"; mkdir -p "$Z/sous/__MACOSX"
head -c 5000 /dev/zero | tr '\0' a > "$Z/sous/grand.iso"
echo petit > "$Z/petit.img"
head -c 9000 /dev/zero | tr '\0' b > "$Z/sous/__MACOSX/faux.iso"
echo lisez > "$Z/lisez.txt"
( cd "$Z" && zip -qr ../archive.zip . )
S1="$D/disks/os9-install.iso"
ranger_media "$T/archive.zip" "$S1" mv
verifie "zip : plus grosse image, hors __MACOSX" cmp -s "$S1" "$Z/sous/grand.iso"
verifie "zip : extraction nettoyée" absent "$D/.run/extraction"
rm -f "$S1"
( cd "$Z" && zip -qr ../vide.zip lisez.txt )
echoue() { ! "$@"; }
verifie "zip sans image : refus" echoue ranger_media "$T/vide.zip" "$S1" mv
echo image > "$T/mien.iso"
ranger_media "$T/mien.iso" "$S1" lien
verifie "disque de l'utilisateur : lié, pas déplacé" test -L "$S1" -a -f "$T/mien.iso"
rm -f "$S1"
if hdiutil create -quiet -size 1m -fs HFS+ -volname t "$T/t.dmg" 2>/dev/null; then
  ranger_media "$T/t.dmg" "$S1" lien
  verifie "dmg converti" test -s "$S1"
  verifie "dmg : rien de .cdr restant" absent "$D/disks/os9-install.cdr"
  rm -f "$S1"
fi

echo "== parcours"
echo disque > "$POMPPC_DOWNLOADS/$TIGER_FICHIER"
# shellcheck disable=SC2034 # lu par obtenir_media
TIGER_MD5="$(md5 -q "$POMPPC_DOWNLOADS/$TIGER_FICHIER")"
rep "Open Macintosh Garden"
obtenir_media tiger
verifie "Tiger : média en place" grep -q disque "$D/images/MacOSX.4.iso"
verifie "Tiger : déplacé depuis Téléchargements" absent "$POMPPC_DOWNLOADS/$TIGER_FICHIER"
rep
obtenir_media tiger
verifie "média déjà là : aucune question" grep -q disque "$D/images/MacOSX.4.iso"
rm -f "$D/images/MacOSX.4.iso"
rep "Quit"
sortie_de obtenir_media tiger
verifie "Quit : pas de média" absent "$D/images/MacOSX.4.iso"
echo qcow > "$T/existant.qcow2"
rep "$T/existant.qcow2"; importer_disque os9
verifie "disque existant importé" grep -q qcow "$D/disks/os9.qcow2"
rm -f "$D/disks/os9.qcow2"
echo pasbon > "$T/autre.bin"
rep "$T/autre.bin" "OK"
sortie_de importer_disque tiger
verifie "autre format refusé" absent "$D/disks/tiger.qcow2"

echo "== combo 10.4.11"
curl() { # écrit le fichier demandé : -o chemin
  while [ $# -gt 0 ]; do
    if [ "$1" = -o ]; then printf '%s' "${CONTENU:-combo}" > "$2"; fi
    shift
  done
}
# shellcheck disable=SC2034 # lu par telecharger_combo
COMBO_SHA1="$(printf combo | shasum -a 1 | cut -d' ' -f1)"
ranger_media() { echo cd > "$2"; }
marge="$D/disks/tiger.qcow2.tablet-margin"
rep "16 GB (recommended)"; creer_disque tiger
verifie "disque neuf : marge de tablette à 0" grep -q '^0$' "$marge"
: > "$D/.run/installation-en-cours"; echo tiger > "$D/.run/installation-en-cours"
rep "Yes, Start It"; reprendre_installation
verifie "fin d'installation : combo à proposer" test -f "$D/.run/combo-a-proposer"
rep "Skip" "OK"
sans_combo() { ! etape_combo; }
verifie "combo sauté : pas de CD du combo" sans_combo
verifie "combo sauté : marge gardée" test -f "$marge"
: > "$D/.run/combo-a-proposer"
rep "Download from Apple" "OK"
verifie "combo téléchargé : Tiger démarre avec son CD" etape_combo
verifie "combo : CD prêt" test -s "$D/disks/combo-10.4.11.iso"
verifie "combo : dmg retiré" absent "$D/disks/MacOSXUpdCombo10.4.11PPC.dmg"
verifie "combo : en cours" test -f "$D/.run/combo-en-cours"
rep "Not Yet" "OK"
verifie "pas fini : CD gardé" etape_combo
verifie "pas fini : marge gardée" test -f "$marge"
rep "Yes"
verifie "fini : le pilote peut suivre" sans_combo
verifie "fini : marge de 10.4.11 rétablie" absent "$marge"
verifie "fini : CD retiré" absent "$D/disks/combo-10.4.11.iso"
verifie "fini : plus de marqueur" absent "$D/.run/combo-en-cours"
CONTENU=faux
: > "$D/.run/combo-a-proposer"
rep "Download from Apple" "Quit"
sortie_de etape_combo
verifie "SHA-1 faux : rien de gardé" absent "$D/disks/MacOSXUpdCombo10.4.11PPC.dmg"
verifie "SHA-1 faux : rien de temporaire" absent "$D/disks/MacOSXUpdCombo10.4.11PPC.dmg.tmp"
verifie "SHA-1 faux : pas de CD" absent "$D/disks/combo-10.4.11.iso"
pkill -f 'sleep 30' 2>/dev/null

echo "$echoues échec(s)"
[ "$echoues" = 0 ]
