#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Lanceur Tiger : ombre deja preparee, aucun asset installe par ce script.
# MD5 de shaders.big valide (SHA-256 f9fe55f4...813e4f6), deux octets TEX -> TXP.

DMG="$HOME/Desktop/Colin_McRae_Ready_Mac-1.dmg"
SHADOW="$HOME/Library/Application Support/POMPPC/cmr-track-projection.shadow"
VOLUME='/Volumes/Colin McRae Ready Mac'
APP="$VOLUME/Colin McRae Rally Mac.app"
ARCHIVE="$APP/Contents/Resources/data/Rally_Shaders/shaders.big"
EXPECTED_MD5='5761b6eb3081cf0f10d7934df4a98045'
ORIGINAL_MD5='51a6cd53223114f59bd8c81ecff86938'

fail()
{
    printf '\nColin McRae : %s\n' "$1" >&2
    if [ -t 0 ]; then
        printf 'Appuyez sur Entree pour fermer ce lanceur.\n' >&2
        IFS= read -r _answer
    fi
    exit 1
}

archive_valid()
{
    [ -f "$ARCHIVE" ] || return 1
    actual_md5=$(/sbin/md5 -q "$ARCHIVE" 2>/dev/null) || return 1
    [ "$actual_md5" = "$EXPECTED_MD5" ]
}

[ -f "$DMG" ] || fail "DMG original absent : $DMG"
[ -f "$SHADOW" ] && [ -s "$SHADOW" ] ||
    fail "Ombre corrigee absente : $SHADOW. Faites d'abord installer et verifier cette ombre."

# Une bonne archive deja montee peut etre ouverte sans toucher au montage,
# meme si open doit simplement ramener une partie existante au premier plan.
if archive_valid; then
    /usr/bin/open "$APP" || fail 'Impossible d ouvrir le jeu deja monte.'
    exit 0
fi

# Ne jamais tuer une partie ni detacher son volume. -c donne le nom reel du
# processus, pas les arguments d'un shell qui cite le nom de l'application.
# [A] Tiger refuse le mot-cle comm (« keyword not found ») : la garde ne voyait rien.
if /bin/ps -axc -o command | /usr/bin/grep -F 'Colin McRae Rally Mac' >/dev/null 2>&1; then
    fail 'Le jeu est deja lance avec une autre archive. Quittez-le normalement, puis relancez cette commande.'
fi

if [ -d "$VOLUME" ]; then
    mounted_md5=$(/sbin/md5 -q "$ARCHIVE" 2>/dev/null) ||
        fail 'Volume monte inconnu : archive absente ou illisible. Aucun demontage tente.'
    [ "$mounted_md5" = "$ORIGINAL_MD5" ] ||
        fail 'Archive montee inconnue : ni original connu ni correctif TXP valide. Aucun demontage tente.'
    printf 'Le volume monte ne contient pas le correctif verifie ; demontage normal...\n'
    if ! /usr/bin/hdiutil detach "$VOLUME"; then
        fail 'Volume occupe ou impossible a demonter. Fermez le jeu et les fichiers ouverts sur ce volume, puis reessayez. Aucun demontage force.'
    fi
fi

printf 'Ouverture du DMG original avec son ombre corrigee...\n'
if ! /usr/bin/hdiutil attach "$DMG" -shadow "$SHADOW" -mountpoint "$VOLUME" -nobrowse; then
    fail 'Montage de l ombre impossible. Le volume ou l ombre peut etre occupe ; aucun demontage force ni processus arrete.'
fi

if ! archive_valid; then
    fail 'Archive inconnue dans l ombre montee : empreinte MD5 incorrecte ou shaders.big absent. Jeu non lance ; faites verifier l ombre.'
fi

printf 'Archive TXP verifiee ; lancement du jeu.\n'
/usr/bin/open "$APP" || fail 'Archive verifiee mais ouverture du jeu impossible.'
