#!/bin/sh
# Nexuiz 2.5.2 (DarkPlaces) — banc GL de POMPPC (27/09/2026).
# ~/nexuiz.env (facultatif) : variables POMPPC_* et NX_ARGS (arguments du moteur),
# ex. NX_ARGS="+r_glsl 0 -benchmark demos/demo1", ou "+vid_fullscreen 1 +vid_width 1024
# +vid_height 768 -benchmark demos/demo1" (plein écran à la taille du bureau).
# ~/premierplan.dylib (tools/guest/launchers/premierplan.c) : sans lui, le jeu lancé par son
# exécutable reste « background only », jamais au premier plan, et le plugin replie chaque
# échange (Swap60 + Swap5c, 8,9 img/s au lieu de 10,1 sur demo1).
D=/Users/tiger/nx-dump; rm -rf $D; mkdir -p $D
APP=/Users/tiger/Nexuiz/Nexuiz.app
POMPPC_GL_STATS=1; export POMPPC_GL_STATS
POMPPC_GL_NOTE=$D/note.txt; export POMPPC_GL_NOTE
POMPPC_GL_FRAMES=$D/frames.csv; export POMPPC_GL_FRAMES
DYLD_INSERT_LIBRARIES=/Users/tiger/libgltrap.dylib:/Users/tiger/premierplan.dylib; export DYLD_INSERT_LIBRARIES
POMPPC_GLTRAP=$D/gltrap.txt; export POMPPC_GLTRAP
POMPPC_GLTRAP_WINDOW=big200:60; export POMPPC_GLTRAP_WINDOW
NX_ARGS="-benchmark demos/demo1"
[ -f /Users/tiger/nexuiz.env ] && . /Users/tiger/nexuiz.env
echo "start $(date) $NX_ARGS" >> $D/log.txt
env | grep POMPPC >> $D/log.txt
cd "$APP/Contents/MacOS"
./nexuiz-osx-agl -basedir /Users/tiger/Nexuiz -nosound +vid_fullscreen 0 +vid_width 800 +vid_height 600 +developer 1 $NX_ARGS < /dev/null > $D/stdout.txt 2>&1
echo "exit $? $(date)" >> $D/log.txt
