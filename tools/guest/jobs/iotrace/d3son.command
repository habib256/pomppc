#!/bin/sh
# GPL3 - Copyleft VERHILLE Arnaud
# À copier dans l'invité : ~/son/d3.command (tools/son/partie.py le lance).
# DOOM 3, relevé des saccades du son (pas de gltrap). FS=1 : plein écran.
[ -f /Users/tiger/son/env ] && . /Users/tiger/son/env
D=/Users/tiger/son/d3; rm -rf $D; mkdir -p $D
APP="/Users/tiger/Desktop/Doom 3 Demo/Doom 3 Demo.app"
POMPPC_GL_STATS=1; export POMPPC_GL_STATS
POMPPC_GL_NOTE=$D/note.txt; export POMPPC_GL_NOTE
POMPPC_GL_FRAMES=$D/frames.csv; export POMPPC_GL_FRAMES
if [ "${IOTRACE_OFF:-0}" != 1 ]; then
  DYLD_INSERT_LIBRARIES=/Users/tiger/son/iotrace.dylib; export DYLD_INSERT_LIBRARIES
  IOTRACE=$D/iotrace.csv; export IOTRACE
fi
if [ "${FS:-0}" = 1 ]; then F=1; M=5; else F=0; M=3; fi
echo "start $(date +%s) fs=$F $EXTRA" >> $D/log.txt
cd "$APP/Contents/MacOS"
"./Doom 3 Demo" +set r_fullscreen $F +set r_mode $M +set com_showFPS 1 +set r_useARBProgram 1 $EXTRA +map game/demo_mars_city1 < /dev/null > $D/stdout.txt 2>&1
echo "exit $? $(date +%s)" >> $D/log.txt
