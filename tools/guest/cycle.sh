#!/usr/bin/env bash
# cycle.sh — tue le jeu, recompile et réinstalle le plugin dans l'invité, relance Colin McRae
# jusqu'en course (Jouer, Entrée à l'écran titre, Entrée au menu), déclenche le vidage, capture
# l'écran. Journal sur stdout. Variables du jeu en plus : /tmp/cmr-env dans l'invité (lu par
# ~/cmr.command), p. ex. POMPPC_GL_CMRPROBE=60 ou POMPPC_GL_VAR=0.
#   tools/guest/cycle.sh              tout
#   NORUN=1 tools/guest/cycle.sh      plugin seul : transfert, compilation (gcc-4.0), installation
#   NOBUILD=1 / DUMP=1 / RACE_WAIT=s / SHOT_DIR=…
# Déplacé de .run/cmr/ (A5 scripts, 26/09/2026). Les sources viennent du dépôt où vit ce
# script (worktree compris) ; journaux et captures dans <dépôt principal>/.run/cmr/.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MAIN="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | sed 's|/\.git$||')"
R="${MAIN:-$ROOT}/.run/cmr"; mkdir -p "$R"; T="$ROOT/tools/guest/tssh.sh"
S=${SHOT_DIR:-$R}
step(){ echo "== $(date +%H:%M:%S) $*"; }
step "kill"; $T "killall 'Colin McRae Rally Mac' 2>/dev/null; sleep 2; killall -9 'Colin McRae Rally Mac' 2>/dev/null; true"
if [ "${NOBUILD:-}" != 1 ]; then
step "transfert"; (cd "$ROOT" && tar cf - guest/gldriver patches/qgpu/qgpu_proto.h patches/qgpu/qgpu_abi.h) | $T "rm -rf ~/pomppc-build/guest/gldriver && mkdir -p ~/pomppc-build/patches/qgpu && tar xf - -C ~/pomppc-build" || exit 1
step "compilation"; $T "cd ~/pomppc-build/guest/gldriver && cp ../../patches/qgpu/qgpu_proto.h ../../patches/qgpu/qgpu_abi.h . && make 2>&1 | grep -v '^/usr/bin/gcc\|^mkdir\|^cp ' ; test -x GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC && echo BUILD_OK" | tee "$R/build.log"
grep -q BUILD_OK "$R/build.log" || { echo "ÉCHEC compilation"; exit 1; }
step "installation"; $T "echo tiger974 | sudo -S sh -c 'rm -rf /System/Library/Extensions/GLDriver-POMPPC.bundle && cp -R /Users/tiger/pomppc-build/guest/gldriver/GLDriver-POMPPC.bundle /System/Library/Extensions/ && chown -R root:wheel /System/Library/Extensions/GLDriver-POMPPC.bundle && chmod -R 755 /System/Library/Extensions/GLDriver-POMPPC.bundle' 2>&1 | grep -v Password; echo INSTALL_OK"
fi
[ "${NORUN:-}" = 1 ] && exit 0
# le dialogue de plantage du lancement précédent (killgame.py) avalerait la première Entrée
step "lancement"; $T "killall ScreenSaverEngine UserNotificationCenter crashdump 2>/dev/null; hdiutil attach -readonly -noverify ~/Desktop/Colin_McRae_Ready_Mac-1.dmg >/dev/null 2>&1; open ~/cmr.command"; sleep 20
step "Jouer"; $T "osascript -e 'tell application \"System Events\" to tell process \"Colin McRae Rally Mac\" to set frontmost to true' -e 'delay 1' -e 'tell application \"System Events\" to keystroke return' 2>&1"; sleep 25
step "titre"; $T "osascript -e 'tell application \"System Events\" to keystroke return' 2>&1"; sleep 22
step "menu"; $T "osascript -e 'tell application \"System Events\" to keystroke return' 2>&1"; sleep ${RACE_WAIT:-60}
[ "${DUMP:-}" = 1 ] && { step "GO"; $T "touch /tmp/cmr-go"; sleep 20; }
step "capture"; python3 "$ROOT/scripts/moncmd.py" "${MAIN:-$ROOT}/.run/mon.sock" "screendump $S/cycle.ppm" >/dev/null; sips -s format png "$S/cycle.ppm" --out "$S/cycle.png" >/dev/null 2>&1
step "capture avant kill"; python3 "$ROOT/scripts/moncmd.py" "${MAIN:-$ROOT}/.run/mon.sock" "screendump $S/cycle2.ppm" >/dev/null; sips -s format png "$S/cycle2.ppm" --out "$S/cycle2.png" >/dev/null 2>&1
step "kill"; python3 "$ROOT/tools/guest/killgame.py"; sleep 15
step "journal"; for i in 1 2 3 4 5 6; do $T "cat ~/cmr-dump2/note.txt" > "$R/dump2-note.txt" 2>/dev/null && break; sleep 15; done
grep -n 'SONDE\|slot\|vbo \|DRAW_RAW\|fallback\|broken\|VAO' "$R/dump2-note.txt" | head -80; $T "cat ~/cmr-dump2/log.txt; ls ~/cmr-dump2/dump | wc -l; tail -1 ~/cmr-dump2/frames.csv"
step "fin"
