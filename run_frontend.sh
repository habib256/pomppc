#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# Lance le frontend ImGui POMPPC depuis n'importe quel répertoire.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT/frontend/build/pomppc"
# paquet publié : frontend déjà compilé dans bin/
if [ ! -x "$BIN" ] && [ -x "$ROOT/bin/pomppc" ]; then BIN="$ROOT/bin/pomppc"; fi

if [ ! -x "$BIN" ]; then
  echo "Frontend non compilé. Lance d'abord :" >&2
  echo "  cd \"$ROOT/frontend\" && ./setup.sh && cmake -S . -B build && cmake --build build -j" >&2
  exit 1
fi

# sans argument, le frontend cherche run_os9.sh deux niveaux au-dessus de lui : faux depuis bin/
[ $# -eq 0 ] && set -- "$ROOT/run_os9.sh"
exec "$BIN" "$@"
