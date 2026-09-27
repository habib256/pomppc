#!/bin/sh
# Raccourci interactif pour le bureau Tiger : menu du jeu, GLSL, son activé.
# Le banc chronométré reste dans nexuiz.command et tools/matrice/jeux/nx.py.
BASE=/Users/tiger/Nexuiz
APP="$BASE/Nexuiz.app/Contents/MacOS"
PP=/Users/tiger/premierplan.dylib
if [ ! -x "$APP/nexuiz-osx-agl" ] || [ ! -f "$PP" ]; then
    echo "Nexuiz ou premierplan.dylib manque dans /Users/tiger."
    exit 1
fi
mkdir -p /Users/tiger/nexuiz-tests || exit 1
LOG=$(mktemp -d /Users/tiger/nexuiz-tests/session.XXXXXX) || exit 1
export DYLD_INSERT_LIBRARIES="$PP"
export POMPPC_GL_STATS=1
export POMPPC_GL_NOTE="$LOG/note.txt"
export POMPPC_GL_FRAMES="$LOG/frames.csv"
cd "$APP" || exit 1
exec ./nexuiz-osx-agl -basedir "$BASE" +r_glsl 1 +vid_fullscreen 0 \
    +vid_width 800 +vid_height 600 > "$LOG/game.log" 2>&1
