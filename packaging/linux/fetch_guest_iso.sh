#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# fetch_guest_iso.sh — télécharge le CD invité de packaging/guest-iso.lock et vérifie sa somme.
#
#   packaging/linux/fetch_guest_iso.sh <sortie.iso> [--exige-version <version>]
#
# --exige-version : pour un tag. Le CD doit être celui de cette version : un paquet publié ne
# porte pas les pilotes de la précédente.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOCK="${GUEST_ISO_LOCK:-$ROOT/packaging/guest-iso.lock}"
OUT="${1:?sortie attendue}"
EXIGE=""
if [ "${2:-}" = --exige-version ]; then EXIGE="${3:?version attendue}"; fi

champ() { sed -n "s/^$1=//p" "$LOCK" | head -1; }
V="$(champ version)"; U="$(champ url)"; S="$(champ sha256)"

if [ -z "$V" ] || [ -z "$U" ] || ! printf '%s' "$S" | grep -Eq '^[0-9a-f]{64}$'; then
  echo "ERREUR : $LOCK incomplet (version, url, sha256 attendus)." >&2
  exit 1
fi
if [ -n "$EXIGE" ] && [ "$V" != "$EXIGE" ]; then
  echo "ERREUR : le CD invité de $LOCK est celui de la $V, le tag publie la $EXIGE." >&2
  echo "Graver le CD de la $EXIGE, le déposer et refaire le fichier : docs/publier-linux.md §3." >&2
  exit 1
fi
mkdir -p "$(dirname "$OUT")"
curl -fsSL --retry 3 -o "$OUT.tmp" "$U"
echo "$S  $OUT.tmp" | sha256sum -c -
mv "$OUT.tmp" "$OUT"
echo "CD invité $V : $OUT"
