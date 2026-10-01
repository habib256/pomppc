/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/* premierplan.c — fait d'un exécutable lancé depuis un shell une application
 * « de premier plan » (POMPPC, 27/09/2026).
 *
 * Nexuiz (DarkPlaces AGL) lancé par son exécutable `nexuiz-osx-agl-bin` et
 * non par LaunchServices reste un processus « background only » : son nom ne
 * correspond pas au CFBundleExecutable du paquet (`nexuiz-osx-agl`, un script
 * qui fait exec du -bin), et vid_agl.c n'appelle pas TransformProcessType
 * (SDL le fait). Il n'a ni menu, ni Dock, ni premier plan : osascript ne peut
 * pas le mettre devant, le plugin POMPPC voit « not frontmost » et replie
 * chaque échange vers Apple (Swap60 + le glFinish Swap5c : deux replis par
 * image).
 *
 * Préchargé (DYLD_INSERT_LIBRARIES), ce constructeur transforme le processus
 * en application de premier plan et le met devant.
 *
 *   MACOSX_DEPLOYMENT_TARGET=10.4 gcc-4.0 -arch ppc -dynamiclib -O2 \
 *       -framework ApplicationServices premierplan.c -o premierplan.dylib
 */
#include <ApplicationServices/ApplicationServices.h>

__attribute__((constructor))
static void premier_plan(void)
{
    ProcessSerialNumber psn = { 0, kCurrentProcess };
    TransformProcessType(&psn, kProcessTransformToForegroundApplication);
    SetFrontProcess(&psn);
}
