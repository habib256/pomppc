#!/bin/sh
# Lanceur POMPPC : Unreal Tournament 2004 Demo avec la trace du plugin OpenGL (pour apprendre).
T=/Users/tiger/pomppc-trace/UT2004-$(date +%H%M%S)
mkdir -p $T/gltrace
POMPPC_GL_STATS=1; export POMPPC_GL_STATS
POMPPC_GL_NOTE=$T/note.txt; export POMPPC_GL_NOTE
POMPPC_GL_FRAMES=$T/frames.csv; export POMPPC_GL_FRAMES
# trace intégrale des appels coupée pour l'A/B synchrone (trop lente) : POMPPC_GLTRACE=$T/gltrace
# mouchard exit()/abort()/_exit() par INTERPOSITION dyld (pas d'espace plat) → exitwatch.txt
POMPPC_EXITWATCH=$T/exitwatch.txt; export POMPPC_EXITWATCH
DYLD_INSERT_LIBRARIES=/Users/tiger/libexitwatch.dylib; export DYLD_INSERT_LIBRARIES
# chemin brut ACTIF (défaut) ; sonde des sommets aberrants (traînées) → note.txt
cd "/Users/tiger/Desktop/Unreal Tournament 2004 Demo.app/Contents/MacOS"
exec "./Unreal Tournament 2004 Demo" > $T/stdout.txt 2>&1
