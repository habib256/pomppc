#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# check_workflow_pins.sh — chaque `uses:` et chaque image de conteneur sous .github/ est épinglé
# sur quelque chose qui ne bouge pas. Repris de POM2 (tools/check_workflow_pins.sh), avec ses témoins.
#
# La mine du jour de publication, c'est la dépendance qui a changé entre deux tags : une action
# retaguée, une image reconstruite. Donc :
#   · `uses: owner/repo@REF`  → REF est un commit de 40 hex (`# vX.Y.Z` dit ce qu'il était) ;
#   · toute ligne qui nomme une image → elle porte `@sha256:` ;
#   · `version: latest` refusé partout.
# Les `uses: ./…` locaux sont exemptés, mais l'action composite qu'ils désignent est lue, d'où le
# find plus bas et pas un glob sur workflows/*.yml.
#
# Une variable d'image vide (`BUILDER_IMAGE: ''`) passe : aucun digest n'existe avant la première
# construction de l'image, et c'est le job qui refuse alors de tourner (release.yml, job linux).
#
#   tools/check_workflow_pins.sh               # les workflows de ce dépôt
#   tools/check_workflow_pins.sh --self-test   # témoins négatifs, arbre jetable
set -euo pipefail

if [ "${1:-}" = "--self-test" ]; then
    here="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    fails=0
    # Le script se place dans SON parent : l'arbre d'essai doit en porter une copie, sinon on
    # relit le dépôt et chaque témoin « passe ».
    mk() {  # mk <corps du workflow>
        rm -rf "$tmp/t"; mkdir -p "$tmp/t/.github/workflows" "$tmp/t/tools"
        cp "$here" "$tmp/t/tools/check_workflow_pins.sh"
        printf '%s\n' "$1" > "$tmp/t/.github/workflows/w.yml"
    }
    expect() {  # expect <pass|fail> <libellé> <corps>
        mk "$3"
        if bash "$tmp/t/tools/check_workflow_pins.sh" >/dev/null 2>&1; then got=pass; else got=fail; fi
        if [ "$got" = "$1" ]; then printf '  ok    %s\n' "$2"
        else printf '  ÉCHEC %s (attendu %s, obtenu %s)\n' "$2" "$1" "$got"; fails=$((fails+1)); fi
    }
    SHA=0123456789012345678901234567890123456789
    DIG=sha256:0123456789012345678901234567890123456789012345678901234567890123
    expect pass "un workflow épinglé passe" \
"jobs:
  a:
    steps:
      - uses: actions/checkout@$SHA # v5.1.0"
    expect fail "une action sur un tag est refusée" \
"jobs:
  a:
    steps:
      - uses: actions/checkout@v5"
    expect fail "une image de registre avec un tag mobile est refusée" \
"jobs:
  a:
    container:
      image: ghcr.io/owner/b:latest"
    expect fail "une image Docker Hub en nom court est refusée" \
"jobs:
  a:
    container:
      image: emscripten/emsdk:3.1.64"
    expect fail "une image à un seul segment est refusée" \
"jobs:
  a:
    container: node:20"
    expect fail "un docker run sans tag est refusé" \
"jobs:
  a:
    steps:
      - run: docker run --rm alpine sh -c true"
    expect fail "BUILDER_IMAGE sur un tag est refusé" \
"env:
  BUILDER_IMAGE: ghcr.io/owner/b:v3"
    expect fail "'version: latest' est refusé" \
"jobs:
  a:
    steps:
      - uses: ./.github/actions/x
        with:
          version: latest"
    expect pass "une image épinglée par digest passe" \
"jobs:
  a:
    container:
      image: emscripten/emsdk:3.1.64@$DIG"
    expect pass "un docker run sur plusieurs lignes, image épinglée, passe" \
"jobs:
  a:
    steps:
      - run: |
          docker run --rm \\
            -v \"\$PWD\":/work \\
            debian:bookworm@$DIG \\
            bash /work/x.sh"
    expect pass "une image derrière une variable passe (sa définition est lue)" \
"env:
  BUILDER_IMAGE: ghcr.io/owner/b:v3@$DIG
jobs:
  a:
    steps:
      - run: docker run --rm \"\$BUILDER_IMAGE\" true"
    expect pass "une variable d'image vide passe (premier passage)" \
"env:
  BUILDER_IMAGE: ''"
    if [ "$fails" -ne 0 ]; then
        echo "check_workflow_pins : autotest ÉCHOUÉ ($fails)"; exit 1
    fi
    echo "check_workflow_pins : autotest passé"
    exit 0
fi

cd "$(dirname "$0")/.."

# -H sur chaque grep : avec UN seul fichier, `grep -n` n'imprime pas son nom, et tout se décale.
FILES=()
while IFS= read -r f; do FILES+=("$f"); done < <(
    find .github \( -path '*/workflows/*' -o -name 'action.yml' -o -name 'action.yaml' \) \
         \( -name '*.yml' -o -name '*.yaml' \) -type f | sort)
if [ "${#FILES[@]}" -eq 0 ]; then
    echo "check_workflow_pins : aucun workflow sous .github/" >&2
    exit 2
fi
echo "check_workflow_pins : ${#FILES[@]} fichier(s)"

bad=0

# ── `uses:` ────────────────────────────────────────────────────────────────
while IFS= read -r line; do
    file=${line%%:*}; rest=${line#*:}; lineno=${rest%%:*}; text=${rest#*:}
    ref=$(printf '%s' "$text" | sed -n 's/.*uses:[[:space:]]*\([^[:space:]#]*\).*/\1/p')
    case "$ref" in
        ./*|'') continue ;;
    esac
    sha=${ref##*@}
    if ! printf '%s' "$sha" | grep -Eq '^[0-9a-f]{40}$'; then
        echo "NON ÉPINGLÉ  $file:$lineno  $ref"
        bad=1
    fi
done < <(grep -nH 'uses:' "${FILES[@]}" | grep -v '^[^:]*:[0-9]*:[[:space:]]*#')

# ── images nommées n'importe où sur la ligne ───────────────────────────────
# Un nom de distribution nu doit être suivi de `:` : `runs-on: ubuntu-latest` n'est pas une image.
IMAGE_RE='(([a-z0-9.-]+\.[a-z]{2,}(:[0-9]+)?/)[a-z0-9._/-]+|(^|[^a-z0-9._/-])(debian|ubuntu|alpine|fedora|rockylinux|centos))(:[A-Za-z0-9._-]+)'
while IFS= read -r line; do
    file=${line%%:*}; rest=${line#*:}; lineno=${rest%%:*}; text=${rest#*:}
    printf '%s' "$text" | grep -q 'uses:' && continue
    printf '%s' "$text" | grep -q '@sha256:' && continue
    echo "IMAGE NON ÉPINGLÉE  $file:$lineno  $(printf '%s' "$text" | sed 's/^[[:space:]]*//')"
    bad=1
done < <(grep -nHE "$IMAGE_RE" "${FILES[@]}" | grep -v '^[^:]*:[0-9]*:[[:space:]]*#')

# ── images en VALEUR d'une clé ─────────────────────────────────────────────
# Question inversée : ne pas reconnaître une image, exiger le digest. Une indirection `${{ … }}` /
# `$VAR` est sautée : la ligne qui DÉFINIT la variable est elle-même une de ces clés.
while IFS= read -r line; do
    file=${line%%:*}; rest=${line#*:}; lineno=${rest%%:*}; text=${rest#*:}
    value=$(printf '%s' "$text" | sed -E 's/^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*:[[:space:]]*//; s/^["'"'"']//; s/["'"'"']$//')
    case "$value" in
        ''|'|'*|'>'*|'#'*|'${{'*|'$'*) continue ;;
    esac
    printf '%s' "$value" | grep -q '@sha256:' && continue
    echo "IMAGE NON ÉPINGLÉE  $file:$lineno  $(printf '%s' "$text" | sed 's/^[[:space:]]*//')"
    bad=1
done < <(grep -nHE '^[[:space:]]*(image|container|[A-Za-z_]*_?IMAGE):[[:space:]]*\S' "${FILES[@]}" \
         | grep -v '^[^:]*:[0-9]*:[[:space:]]*#' || true)

# ── docker run/pull ────────────────────────────────────────────────────────
# La LIGNE LOGIQUE : un `docker run` s'écrit sur plusieurs lignes, l'image sur l'une des dernières.
for f in "${FILES[@]}"; do
    while IFS= read -r line; do
        lineno=${line%%:*}; text=${line#*:}
        printf '%s' "$text" | grep -q '@sha256:' && continue
        printf '%s' "$text" | grep -qE '\$\{\{|\$[A-Za-z_{]' && continue   # indirection
        echo "IMAGE NON ÉPINGLÉE  $f:$lineno  $(printf '%s' "$text" | sed 's/^[[:space:]]*//' | cut -c1-90)"
        bad=1
    done < <(awk '
        { line = $0
          sub(/^[ \t]+/, "", line)
          if (buf == "") { start = NR; buf = line } else { buf = buf " " line }
          if (line ~ /\\$/) { sub(/\\$/, "", buf); next }
          if (buf ~ /docker[ \t]+(run|pull)[ \t]/ && buf !~ /^#/) print start ":" buf
          buf = "" }
        END { if (buf ~ /docker[ \t]+(run|pull)[ \t]/ && buf !~ /^#/) print start ":" buf }
    ' "$f")
done

# ── `version: latest` ──────────────────────────────────────────────────────
if grep -nE '^[[:space:]]*version:[[:space:]]*latest\b' "${FILES[@]}"; then
    echo "NON ÉPINGLÉ  'version: latest'"
    bad=1
fi

if [ "$bad" -ne 0 ]; then
    echo "check_workflow_pins : ÉCHEC, épingler sur un commit ou un digest (voir l'en-tête)."
    exit 1
fi
echo "check_workflow_pins : passé."
