#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# invite-syms.sh — exécuté DANS l'invité (sh, par ssh, sous sudo) : écrit dans
# $1 (défaut /tmp/endurance-syms) un fichier de symboles par kext chargé
# (kextload -n -s -A : lié aux adresses du démarrage en cours, sans rien
# charger), plus kextstat.txt, la liste des adresses de CE démarrage.
# L'hôte en tire, pour une adresse d'un démarrage quelconque, le décalage
# dans le kext (adresse − base du kext ce jour-là) et le symbole.
OUT="${1:-/tmp/endurance-syms}"
rm -rf "$OUT"; mkdir -p "$OUT"
kextstat > "$OUT/kextstat.txt"
# identifiant → chemin du bundle, pour tous les kexts du système (PlugIns compris)
find /System/Library/Extensions /Library/Extensions -name Info.plist -path '*.kext/Contents/Info.plist' 2>/dev/null |
while read -r p; do
  id=$(awk '/<key>CFBundleIdentifier<\/key>/{getline; gsub(/.*<string>|<\/string>.*/,""); print; exit}' "$p")
  echo "$id ${p%/Contents/Info.plist}"
done > "$OUT/chemins.txt"
awk 'NR>1 && $3 != "0x0" {print $6}' "$OUT/kextstat.txt" | while read -r id; do
  chemin=$(awk -v i="$id" '$1==i {print $2; exit}' "$OUT/chemins.txt")
  [ -n "$chemin" ] || { echo "sans chemin : $id" >> "$OUT/erreurs.txt"; continue; }
  kextload -n -s "$OUT" -A "$chemin" >> "$OUT/kextload.log" 2>&1 ||
    echo "échec : $id ($chemin)" >> "$OUT/erreurs.txt"
done
ls "$OUT" | wc -l
