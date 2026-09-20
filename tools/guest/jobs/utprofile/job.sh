#!/bin/sh
# Diagnostic: DM-Rankin, stationary pre-match camera, no bots.
# This is NOT the final 30 fps gameplay acceptance test. Do not send input
# during the run. Start the VM with CPU_OPTS=x-fast-fp=on/off for an A/B.
# TAG=fast/exact DUR=60 LOAD_TIMEOUT=240. Run through devloop in GUI mode.
. ./guilib.sh
set -e
TAG=${TAG:-fast}
DUR=${DUR:-60}
LOAD_TIMEOUT=${LOAD_TIMEOUT:-240}
UT=${UT:-'/Users/tiger/Desktop/Unreal Tournament 2004 Demo.app'}
case "$TAG" in ''|*[!a-zA-Z0-9_-]*) echo 'invalid TAG'; exit 1;; esac
case "$DUR:$LOAD_TIMEOUT" in *[!0-9:]*|:*|*:) echo 'invalid duration'; exit 1;; esac
[ "$DUR" -gt 0 ] && [ "$LOAD_TIMEOUT" -gt 0 ]
[ -x "$UT/System/ut2004-bin" ] || { echo "UT missing: $UT"; exit 1; }
P='/Users/tiger/Library/Application Support/Unreal Tournament 2004 Demo/System/UT2004.ini'
[ -f "$P" ] || { echo 'Launch UT once to create UT2004.ini'; exit 1; }
W=$(mktemp -d /tmp/utprofile-XXXXXX)
mkdir -p out
cp "$P" "$W/original.ini"
trap 'cat "$W/original.ini" > "$P"' EXIT
sed -e 's/^WindowedViewportX=.*/WindowedViewportX=800/' \
    -e 's/^WindowedViewportY=.*/WindowedViewportY=600/' \
    -e 's/^StartupFullscreen=.*/StartupFullscreen=False/' \
    -e 's/^MaxTextureUnits=.*/MaxTextureUnits=4/' "$P" > "$W/settings.ini"
cat "$W/settings.ini" > "$P"
printf '%s\n' "$UT" > "$W/app-path"
printf '%s\n' "$DUR" > "$W/duration"
printf '%s\n' "$LOAD_TIMEOUT" > "$W/load-timeout"
# Fix the libc random seed without -benchmark (which crashes this demo in
# FStats::UpdateString). This is a measurement-only preload, never installed
# in the game or system. Confirm scene/triangle counts agree across runs;
# a fixed seed alone does not make the entire engine deterministic.
cat > "$W/seed.c" <<'SEED'
#include <stdlib.h>
#include <stdio.h>
static void seed_srand(unsigned int seed)
{
    fprintf(stderr, "utprofile: srand(%u) -> srand(0)\n", seed);
    srand(0);
}
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement; const void *original; } hooks[] = {
    { (const void *)seed_srand, (const void *)srand }
};
SEED
/usr/bin/gcc-4.0 -arch ppc -dynamiclib -O2 -o "$W/seed.dylib" "$W/seed.c"
cat > "$W/run.sh" <<'GUI'
#!/bin/sh
W=$1
UT=$(cat "$W/app-path")
DUR=$(cat "$W/duration")
LIMIT=$(cat "$W/load-timeout")
cd "$UT/System" || exit 1
DYLD_INSERT_LIBRARIES="$W/seed.dylib" \
 POMPPC_GL_STATS="$W/stats.txt" POMPPC_GL_FRAMES="$W/frames.csv" \
 ./ut2004-bin 'DM-Rankin?game=XGame.xDeathMatch?NumBots=0' -windowed > "$W/game.log" 2>&1 &
p=$!
echo "$p" > "$W/pid"
trap 'kill "$p" 2>/dev/null || true' EXIT
sleep 5
osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $p) to true" 2>/dev/null
n=0
while [ ! -s "$W/stats.txt" ]; do
    kill -0 "$p" 2>/dev/null || { echo 'UT exited before first measurement'; exit 1; }
    [ "$n" -lt "$LIMIT" ] || { echo 'first-frame timeout'; exit 1; }
    sleep 2
    n=$((n+2))
done
# Loading and initial precaching are excluded from the intended window.
sleep 10
date > "$W/measurement-start.txt"
tail -1 "$W/frames.csv" >> "$W/measurement-start.txt"
sleep "$DUR"
kill -0 "$p" 2>/dev/null || { echo 'UT exited during measurement'; exit 1; }
date > "$W/measurement-end.txt"
tail -1 "$W/frames.csv" >> "$W/measurement-end.txt"
# Profiling is AFTER the undisturbed frame-time window.
/usr/sbin/screencapture -x "$W/scene.png"
top -l 2 -s 2 -n 8 -o cpu > "$W/top.txt" 2>&1
/usr/bin/sample "$p" 10 -file "$W/sample.txt" > "$W/sample.log" 2>&1
kill "$p" 2>/dev/null || true
wait "$p" 2>/dev/null
trap - EXIT
echo complete > "$W/complete"
GUI
chmod -R 777 "$W"
gui_run "sh '$W/run.sh' '$W'" $((LOAD_TIMEOUT + DUR + 90))
if [ ! -f "$W/complete" ] && [ -f "$W/pid" ]; then
    kill "$(cat "$W/pid")" 2>/dev/null || true
    sleep 3
fi
cp "$W"/* out/
cp '/Users/tiger/Library/Application Support/Unreal Tournament 2004 Demo/System/UT2004.log' out/engine.log
printf 'tag=%s\nscene=DM-Rankin stationary pre-match, no input, no bots\nresolution=800x600\nmax_texture_units=4\nlibc_seed=0 (measurement preload; verify scene identity)\n' "$TAG" > out/manifest.txt
sysctl -n hw.ncpu >> out/manifest.txt
tail -12 out/stats.txt
[ -f "$W/complete" ] || { echo 'INCOMPLETE RUN'; exit 1; }
