#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# apprun_test.sh — le démarrage de l'AppImage, sans squashfs : dossier de données, système à ouvrir,
# verrous, environnement passé au frontal (packaging/linux/AppRun). Pendant de tests/app_wizard_test.sh.
# Le paquet réel est éprouvé par packaging/linux/smoke_appimage.sh, en CI.
set -u
RACINE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "${T:?}"' EXIT
echoues=0
verifie() { # verifie "libellé" commande…
  local l="$1"; shift
  if "$@"; then echo "ok   $l"; else echo "ÉCHEC $l"; echoues=$((echoues + 1)); fi
}
absent() { [ ! -e "$1" ] && [ ! -L "$1" ]; }
contient() { grep -qF -- "$2" "$1"; }

# Un AppDir minimal : le vrai AppRun, un lanceur factice qui relève ce qu'il a reçu.
fabrique() { # fabrique <dossier>
  local A="$1" P="$1/usr/share/pomppc"
  mkdir -p "$P/bin" "$P/scripts" "$P/disks/extras"
  cp "$RACINE/packaging/linux/AppRun" "$A/AppRun"; chmod +x "$A/AppRun"
  echo 9.8.7 > "$A/version"
  for f in run_tiger.sh run_os9.sh config.env; do echo "paquet" > "$P/$f"; done
  cat > "$P/run_frontend.sh" <<'EOF'
#!/bin/bash
# Le frontal factice : relève l'argument et l'environnement, à côté du dossier de données.
{ echo "arg=$1"; env; } > "$(dirname "$0")/releve.txt"
EOF
  chmod +x "$P/run_frontend.sh"
  echo doc > "$P/README.md"
  echo iso > "$P/pomppc-guest.iso"
}

fabrique "$T/a"
export POMPPC_SANS_DIALOGUE=1 POMPPC_NO_HOST_CHECK=1
export POMPPC_DATA="$T/donnees"
# shellcheck source=/dev/null
source "$T/a/AppRun"

echo "== dossier de données"
mkdir -p "$D/disks"
echo "mon disque" > "$D/disks/tiger.qcow2"
ln -s /tmp/.mount_POMPPCancien/usr/share/pomppc/perime "$D/perime"
ln -s /tmp/.mount_POMPPCancien/usr/share/pomppc/disks/vieux "$D/disks/vieux"
ln -s /ailleurs/moi "$D/a-moi"
echo "à moi" > "$D/README.md"
preparer_donnees
verifie "disque de l'utilisateur intact" grep -q "mon disque" "$D/disks/tiger.qcow2"
verifie "fichier réel de l'utilisateur non remplacé" grep -q "à moi" "$D/README.md"
verifie "lien de l'utilisateur gardé" test -L "$D/a-moi"
verifie "lien d'un ancien montage retiré" absent "$D/perime"
verifie "lien d'un ancien montage retiré dans disks/" absent "$D/disks/vieux"
verifie "lanceur copié, pas lié" test ! -L "$D/run_tiger.sh"
verifie "scripts/ lié au paquet" test -L "$D/scripts"
verifie "bin/ lié au paquet" test -L "$D/bin"
verifie "CD invité lié" test -L "$D/pomppc-guest.iso"
verifie "contenu de disks/ du paquet lié" test -L "$D/disks/extras"
verifie "le technique est masqué" contient "$D/.hidden" scripts
if grep -qx disks "$D/.hidden"; then verifie "disks reste visible" false; else verifie "disks reste visible" true; fi
# Un autre montage au lancement suivant : les liens suivent.
fabrique "$T/b"
SRC="$T/b/usr/share/pomppc" preparer_donnees
verifie "liens refaits vers le nouveau montage" test "$(readlink "$D/scripts")" = "$T/b/usr/share/pomppc/scripts"
rm -f "$D/disks/tiger.qcow2"

echo "== système à ouvrir"
rm -f "$D/.run/imguidock.conf"
lance() { [ "$(lanceur_a_ouvrir)" = "$1" ]; }
verifie "aucun disque : Tiger" lance run_tiger.sh
echo qcow > "$D/disks/os9.qcow2"
verifie "OS 9 seul : OS 9" lance run_os9.sh
echo qcow > "$D/disks/tiger.qcow2"
verifie "les deux : Tiger" lance run_tiger.sh
echo "os = os9" > "$D/.run/imguidock.conf"
verifie "les deux, OS 9 en dernier : OS 9" lance run_os9.sh
rm -f "$D/disks/os9.qcow2" "$D/.run/imguidock.conf"

echo "== options, sans toucher au dossier"
export POMPPC_DATA="$T/vierge"
verifie "--version" test "$("$T/a/AppRun" --version)" = 9.8.7
"$T/a/AppRun" --help > "$T/aide.txt" 2>&1
verifie "--help" contient "$T/aide.txt" "POMPPC-9.8.7-x86_64.AppImage"
verifie "--help ne crée rien" absent "$T/vierge"
"$T/a/AppRun" --prepare </dev/null
verifie "--prepare prépare" test -f "$T/vierge/run_tiger.sh"
verifie "--prepare ne lance rien" absent "$T/vierge/releve.txt"

echo "== passage au frontal"
export POMPPC_DATA="$T/lance"
unset LD_LIBRARY_PATH EGL_PLATFORM
HOME="$T" "$T/a/AppRun" </dev/null >/dev/null 2>&1
R="$POMPPC_DATA/releve.txt"
verifie "le frontal est lancé" test -f "$R"
verifie "avec le lanceur de ~/POMPPC" contient "$R" "arg=$POMPPC_DATA/run_tiger.sh"
verifie "POMPPC_APP" contient "$R" "POMPPC_APP=1"
verifie "POMPPC_VERSION" contient "$R" "POMPPC_VERSION=9.8.7"
verifie "qemu-img au PATH" contient "$R" "$T/a/usr/share/pomppc/bin:"
verifie "modules GIO de l'hôte écartés" contient "$R" "GIO_MODULE_DIR="
# Les deux pièges du paquet : hérités par les enfants et par le pilote GL.
if grep -q '^LD_LIBRARY_PATH=' "$R"; then verifie "pas de LD_LIBRARY_PATH" false; else verifie "pas de LD_LIBRARY_PATH" true; fi
if grep -q '^EGL_PLATFORM=' "$R"; then verifie "EGL_PLATFORM laissé à run_tiger.sh" false; else verifie "EGL_PLATFORM laissé à run_tiger.sh" true; fi

echo "== verrous"
if command -v flock >/dev/null 2>&1; then
  export POMPPC_DATA="$T/verrou"
  mkdir -p "$POMPPC_DATA/.run"
  rm -f "$POMPPC_DATA/releve.txt"
  # Une VM tient tiger.lock : refus, sans lancer le frontal, et sans supprimer le verrou.
  ( flock -n 7 && sleep 5 ) 7>"$POMPPC_DATA/.run/tiger.lock" &
  tenu=$!
  sleep 0.3
  if "$T/a/AppRun" </dev/null >/dev/null 2>"$T/refus.txt"; then verifie "VM en cours : refus" false; else verifie "VM en cours : refus" true; fi
  verifie "le refus se dit" contient "$T/refus.txt" "already running"
  verifie "le frontal n'est pas lancé" absent "$POMPPC_DATA/releve.txt"
  verifie "le verrou reste" test -e "$POMPPC_DATA/.run/tiger.lock"
  kill "$tenu" 2>/dev/null; wait "$tenu" 2>/dev/null
  # Une autre instance tient app.lock : refus AVANT de refaire les liens.
  ( flock -n 7 && sleep 5 ) 7>"$POMPPC_DATA/.run/app.lock" &
  tenu=$!
  sleep 0.3
  if "$T/a/AppRun" </dev/null >/dev/null 2>&1; then verifie "autre instance : refus" false; else verifie "autre instance : refus" true; fi
  verifie "les liens restent à l'instance en cours" absent "$POMPPC_DATA/scripts"
  kill "$tenu" 2>/dev/null; wait "$tenu" 2>/dev/null
else
  echo "(flock absent : verrous non testés)"
fi

echo "== bibliothèques de l'hôte"
# Un faux ldconfig : le contrôle est lu, sans dépendre de ce que la machine a.
mkdir -p "$T/fauxbin"
cat > "$T/fauxbin/ldconfig" <<'EOF'
#!/bin/sh
printf '\tlibc.so.6 (libc6,x86-64) => /lib/libc.so.6\n'
EOF
chmod +x "$T/fauxbin/ldconfig"
export POMPPC_DATA="$T/hote"
unset POMPPC_NO_HOST_CHECK
if PATH="$T/fauxbin:$PATH" "$T/a/AppRun" </dev/null >/dev/null 2>"$T/hote.txt"; then
  verifie "pilote GL manquant : refus" false
else
  verifie "pilote GL manquant : refus" true
fi
verifie "et il dit quoi installer" contient "$T/hote.txt" "libEGL.so.1"

echo "$echoues échec(s)"
[ "$echoues" = 0 ]
