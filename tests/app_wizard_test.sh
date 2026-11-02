#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# app_wizard_test.sh — le démarrage de POMPPC.app : dossier de données et système à ouvrir (macOS).
# L'assistant lui-même est dans le frontal : frontend/tests/setup_test.cpp.
set -u
RACINE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "${T:?}"' EXIT
echoues=0
verifie() { # verifie "libellé" commande…
  local l="$1"; shift
  if "$@"; then echo "ok   $l"; else echo "ÉCHEC $l"; echoues=$((echoues + 1)); fi
}
absent() { [ ! -e "$1" ] && [ ! -L "$1" ]; }

# Une app minimale : le vrai script et quelques fichiers de paquet.
R="$T/POMPPC.app/Contents/Resources"
mkdir -p "$R/pomppc/bin" "$R/pomppc/scripts" "$R/pomppc/disks/extras"
cp "$RACINE/scripts/app-macos/pomppc-app.sh" "$R/"
for f in run_tiger.sh run_os9.sh run_frontend.sh config.env; do echo "app" > "$R/pomppc/$f"; done
echo doc > "$R/pomppc/README.md"

export POMPPC_DATA="$T/donnees"
# shellcheck source=/dev/null
source "$R/pomppc-app.sh"

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
visible() { ! masque "$1"; }
verifie "technique masqué dans le Finder" masque "$D/scripts"
verifie "disks visible" visible "$D/disks"
rm -f "$D/disks/tiger.qcow2"

echo "== système à ouvrir"
rm -f "$D/.run/imguidock.conf"
lance() { [ "$(lanceur_a_ouvrir)" = "$1" ]; }
verifie "aucun disque : le frontal décide" lance run_tiger.sh
echo qcow > "$D/disks/Autre Nom.qcow2"
verifie "autre nom seul : Tiger" lance run_tiger.sh
echo qcow > "$D/disks/os9.qcow2"
verifie "OS 9 seul au nom attendu : OS 9" lance run_os9.sh
echo qcow > "$D/disks/tiger.qcow2"
verifie "les deux : Tiger par défaut" lance run_tiger.sh
echo "os = os9" > "$D/.run/imguidock.conf"
verifie "les deux, OS 9 en dernier : OS 9" lance run_os9.sh
rm -f "$D/disks/os9.qcow2"
verifie "OS 9 en dernier, disque parti : Tiger" lance run_tiger.sh

echo "$echoues échec(s)"
[ "$echoues" = 0 ]
