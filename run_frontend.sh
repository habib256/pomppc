#!/usr/bin/env bash
# Lance le frontend ImGui POMPPC depuis n'importe quel répertoire.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT/frontend/build/pomppc"

if [ ! -x "$BIN" ]; then
  echo "Frontend non compilé. Lance d'abord :" >&2
  echo "  cd \"$ROOT/frontend\" && ./setup.sh && cmake -S . -B build && cmake --build build -j" >&2
  exit 1
fi

exec "$BIN" "$@"
