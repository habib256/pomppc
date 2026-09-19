#!/bin/sh
# stage.sh — assemble un job devloop : le job.sh de tools/guest/jobs/<nom>/ et,
# dans src/, les sources invité du dépôt (même disposition que le dépôt).
#
#   tools/guest/jobs/stage.sh gpu /tmp/job && devloop.py run /tmp/job
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
NAME=$1; DEST=$2
[ -n "$NAME" ] && [ -n "$DEST" ] || { echo "usage: stage.sh NOM DOSSIER"; exit 1; }
rm -rf "$DEST"; mkdir -p "$DEST/src/kext" "$DEST/src/guest"
cp "$ROOT/tools/guest/jobs/$NAME/job.sh" "$DEST/"
cp -R "$ROOT/kext/POMPPCGPU" "$DEST/src/kext/"
cp -R "$ROOT/guest/gldriver" "$ROOT/guest/gltest" "$ROOT/guest/qgpu-test" "$DEST/src/guest/"
# rien de compilé côté hôte ne doit partir (objets d'une autre machine)
find "$DEST/src" \( -name '*.o' -o -name '*.bundle' -o -name 'glres' -o -name '*.kext' \) \
     -prune -exec rm -rf {} +
echo "$DEST"
