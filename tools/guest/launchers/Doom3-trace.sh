#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# Lanceur POMPPC : DOOM 3 Demo avec la trace du plugin OpenGL (pour apprendre).
# Bundle : ~/Desktop/Doom 3 trace.app/Contents/{Info.plist,MacOS/Doom3-trace}.
T=/Users/tiger/pomppc-trace/Doom3-$(date +%H%M%S)
mkdir -p $T/gltrace
POMPPC_GL_STATS=1; export POMPPC_GL_STATS
POMPPC_GL_NOTE=$T/note.txt; export POMPPC_GL_NOTE
POMPPC_GL_FRAMES=$T/frames.csv; export POMPPC_GL_FRAMES
# trace intégrale des appels coupée (trop lente en jeu) : POMPPC_GLTRACE=$T/gltrace
if [ -f /Users/tiger/libexitwatch.dylib ]; then
  # mouchard exit()/abort()/_exit() par INTERPOSITION dyld → exitwatch.txt
  POMPPC_EXITWATCH=$T/exitwatch.txt; export POMPPC_EXITWATCH
  DYLD_INSERT_LIBRARIES=/Users/tiger/libexitwatch.dylib; export DYLD_INSERT_LIBRARIES
fi
cd "/Users/tiger/Desktop/Doom 3 Demo/Doom 3 Demo.app/Contents/MacOS"
exec "./Doom 3 Demo" +set com_showFPS 1 > $T/stdout.txt 2>&1
