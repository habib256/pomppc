#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# check_version_strings.sh — une seule source pour la version du projet : `project(POMPPC VERSION …)`
# de frontend/CMakeLists.txt. Le reste, qui ne peut pas l'inclure, doit s'accorder avec elle :
#   · docs/notes-version-V.md existe et se présente comme la V (première ligne « POMPPC V »), et
#     nomme le paquet POMPPC-V-x86_64.AppImage : le job `publish` en fait le corps de la Release ;
#   · CHANGELOG.md a une section « ## V » ;
#   · README.md : tout nom de paquet qui porte un numéro (POMPPC-1.2.3-…, pomppc-1.2.3-…) le porte.
# Ne sont PAS concernés : POMPPC_QEMU_VERSION (la version de QEMU visée par la série de patches) et
# QGPU_PROTO_VERSION (le protocole), qui ont leur propre numéro. POMPPC_VERSION à l'exécution vient de
# l'environnement : c'est AppRun (packaging/linux/AppRun) ou pomppc-app.sh qui la pose.
#
#   tools/check_version_strings.sh [--self-test]
#   POMPPC_VERSION_ROOT=<arbre>     # l'arbre à contrôler (l'autotest s'en sert)
set -euo pipefail
export LC_ALL=C      # octet par octet : sed de macOS abandonne sur l'UTF-8 sous certaines locales

self_test() {
    local tmp here
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    here="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    mk() {  # un arbre cohérent à la version $1
        rm -rf "$tmp/t"; mkdir -p "$tmp/t/frontend" "$tmp/t/docs"
        printf 'project(POMPPC VERSION %s LANGUAGES C CXX)\n' "$1" > "$tmp/t/frontend/CMakeLists.txt"
        printf 'POMPPC %s is a release.\n\n| `POMPPC-%s-x86_64.AppImage` | x |\n' "$1" "$1" > "$tmp/t/docs/notes-version-$1.md"
        printf '# Changelog\n\n## %s (non publiée)\n\ntexte\n\n## 0.0.1\n' "$1" > "$tmp/t/CHANGELOG.md"
        printf '# POMPPC\nDownload `POMPPC-<version>-x86_64.AppImage`.\nDepuis la v9.9.9.\n' > "$tmp/t/README.md"
    }
    expect() {  # expect <0|1> <libellé>
        local rc=0
        POMPPC_VERSION_ROOT="$tmp/t" bash "$here" > "$tmp/out" 2>&1 || rc=$?
        if { [ "$1" = 0 ] && [ "$rc" != 0 ]; } || { [ "$1" = 1 ] && [ "$rc" = 0 ]; }; then
            echo "autotest ÉCHOUÉ : $2 (code $rc)"; cat "$tmp/out"; exit 1
        fi
        echo "  ok    $2"
    }
    mk 1.2.3; expect 0 "un arbre cohérent passe"
    mk 1.2.3; rm "$tmp/t/docs/notes-version-1.2.3.md"; expect 1 "notes de version absentes"
    mk 1.2.3; : > "$tmp/t/docs/notes-version-1.2.3.md"; expect 1 "notes de version vides"
    mk 1.2.3; sed -i.bak '1s/1.2.3/1.2.2/' "$tmp/t/docs/notes-version-1.2.3.md"; expect 1 "notes d'une autre version"
    mk 1.2.3; sed -i.bak '/AppImage/d' "$tmp/t/docs/notes-version-1.2.3.md"; expect 1 "notes sans le paquet Linux"
    mk 1.2.3; sed -i.bak 's/^## 1.2.3.*/## 1.2.2/' "$tmp/t/CHANGELOG.md"; expect 1 "CHANGELOG sans la section"
    mk 1.2.3; printf 'Get `POMPPC-1.2.2-x86_64.AppImage`.\n' >> "$tmp/t/README.md"; expect 1 "README : paquet périmé"
    mk 1.2.3; printf 'Get `pomppc-1.2.2-guest.iso`.\n' >> "$tmp/t/README.md"; expect 1 "README : CD périmé"
    mk 1.2.3; printf 'Get `POMPPC-1.2.3-x86_64.AppImage`.\n' >> "$tmp/t/README.md"; expect 0 "README : paquet à jour"
    mk 1.2.3; rm "$tmp/t/CHANGELOG.md"; expect 1 "un fichier absent est un échec"
    mk 1.2.3; sed -i.bak 's/VERSION 1.2.3/VERSION/' "$tmp/t/frontend/CMakeLists.txt"; expect 1 "version illisible"
    echo "check_version_strings : autotest passé"
}

if [ "${1:-}" = "--self-test" ]; then self_test; exit 0; fi

ROOT=${POMPPC_VERSION_ROOT:-"$(cd "$(dirname "$0")/.." && pwd)"}
cd "$ROOT"

V=$(sed -n 's/^project(POMPPC VERSION \([0-9][0-9.]*\).*/\1/p' frontend/CMakeLists.txt)
if [ -z "$V" ]; then
    echo "check_version_strings : project(POMPPC VERSION …) illisible dans frontend/CMakeLists.txt" >&2
    exit 2
fi
for f in CHANGELOG.md README.md; do
    [ -f "$f" ] || { echo "check_version_strings : $f manque" >&2; exit 2; }
done

bad=0
NOTES="docs/notes-version-$V.md"
if [ ! -s "$NOTES" ]; then
    echo "MANQUE $NOTES : le corps de la Release n'existerait pas"
    bad=1
else
    head -1 "$NOTES" | grep -qF "POMPPC $V" ||
        { echo "PÉRIMÉ $NOTES : sa première ligne ne dit pas « POMPPC $V »"; bad=1; }
    grep -qF "POMPPC-$V-x86_64.AppImage" "$NOTES" ||
        { echo "MANQUE dans $NOTES : POMPPC-$V-x86_64.AppImage"; bad=1; }
fi
V_RE=$(printf '%s' "$V" | sed 's/\./\\./g')
grep -qE "^## $V_RE( |\$)" CHANGELOG.md ||
    { echo "MANQUE dans CHANGELOG.md : la section « ## $V »"; bad=1; }
while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    got=${hit#*-}
    [ "$got" = "$V" ] || { echo "PÉRIMÉ README.md : $hit, la version est $V"; bad=1; }
done < <(grep -oE '[Pp][Oo][Mm][Pp][Pp][Cc]-[0-9]+(\.[0-9]+)+' README.md || true)

if [ "$bad" != 0 ]; then
    echo "check_version_strings : ÉCHEC"
    exit 1
fi
echo "check_version_strings : passé ($V)"
