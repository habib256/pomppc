#!/bin/sh
# Lanceur POMPPC : Unreal Tournament 2004 Demo avec la trace du plugin OpenGL (pour apprendre).
T=/Users/tiger/pomppc-trace/UT2004-$(date +%H%M%S)
mkdir -p $T/gltrace
POMPPC_GL_STATS=1; export POMPPC_GL_STATS
POMPPC_GL_NOTE=$T/note.txt; export POMPPC_GL_NOTE
POMPPC_GL_FRAMES=$T/frames.csv; export POMPPC_GL_FRAMES
POMPPC_GLTRACE=$T/gltrace; export POMPPC_GLTRACE
# mouchard sur exit()/abort() : pile PPC + images chargées dans exitwatch.txt
POMPPC_EXITWATCH=$T/exitwatch.txt; export POMPPC_EXITWATCH
DYLD_INSERT_LIBRARIES=/Users/tiger/libexitwatch.dylib; export DYLD_INSERT_LIBRARIES
DYLD_FORCE_FLAT_NAMESPACE=1; export DYLD_FORCE_FLAT_NAMESPACE
cd "/Users/tiger/Desktop/Unreal Tournament 2004 Demo.app/Contents/MacOS"
exec "./Unreal Tournament 2004 Demo" > $T/stdout.txt 2>&1
