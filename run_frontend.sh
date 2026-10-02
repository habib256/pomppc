#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# Lance le frontend ImGui POMPPC depuis n'importe quel répertoire.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT/frontend/build/pomppc"
# paquet publié : frontend déjà compilé dans bin/
if [ ! -x "$BIN" ] && [ -x "$ROOT/bin/pomppc" ]; then BIN="$ROOT/bin/pomppc"; fi

# Build périmé (sources plus récentes que le binaire) : reconstruit avant de
# lancer. Le 02/10/2026 sur le PC, un frontend du 22/08, compilé contre une
# ImGui sans docking, passait pour « ImGuiDock qui ne se lance pas ».
FE="$ROOT/frontend"
if [ "$BIN" = "$FE/build/pomppc" ] && [ -x "$BIN" ] &&
   [ -n "$(find "$FE/src" "$FE/CMakeLists.txt" "$FE/setup.sh" -newer "$BIN" -type f 2>/dev/null | head -1)" ]; then
  echo "▶ frontend plus vieux que ses sources : reconstruction (setup.sh + cmake)"
  if ! ( cd "$FE" && ./setup.sh >/dev/null && cmake -S . -B build >/dev/null &&
         cmake --build build -j >/dev/null ); then
    echo "⚠  reconstruction du frontend échouée : lancement de l'ancien binaire" >&2
  fi
fi

if [ ! -x "$BIN" ]; then
  echo "Frontend non compilé. Lance d'abord :" >&2
  echo "  cd \"$ROOT/frontend\" && ./setup.sh && cmake -S . -B build && cmake --build build -j" >&2
  exit 1
fi

# sans argument, le frontend cherche run_os9.sh deux niveaux au-dessus de lui : faux depuis bin/
[ $# -eq 0 ] && set -- "$ROOT/run_os9.sh"
exec "$BIN" "$@"
