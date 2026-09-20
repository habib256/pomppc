#!/bin/sh
set -e
W=$1
echo $$ > "$W/runner.pid"
exec > "$W/run.log" 2>&1
RES=$(cat "$W/resolution.txt")
UT='/Users/tiger/Desktop/Unreal Tournament 2004 Demo.app'
P='/Users/tiger/Library/Application Support/Unreal Tournament 2004 Demo/System/UT2004.ini'
[ -x "$UT/System/ut2004-bin" ] && [ -f "$P" ]
ps axww > "$W/processes-before.txt"
if ps axww | grep '[Z]enerchi.app/Contents/MacOS/Zenerchi' >/dev/null; then
    echo 'Close Zenerchi before running the fullscreen benchmark'
    exit 1
fi
killall ut2004-bin 2>/dev/null || true
sleep 3
cp "$P" "$W/original.ini"
p=
cleanup() {
    if [ -n "$p" ]; then
        kill "$p" 2>/dev/null || true
        status=0
        wait "$p" 2>/dev/null || status=$?
        printf '%s\n' "$status" > "$W/exit-status.txt"
        p=
    fi
    cat "$W/original.ini" > "$P"
    touch "$W/restored"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
sed -e "s/^FullscreenViewportX=.*/FullscreenViewportX=${RES%x*}/" \
    -e "s/^FullscreenViewportY=.*/FullscreenViewportY=${RES#*x}/" \
    -e 's/^StartupFullscreen=.*/StartupFullscreen=True/' \
    -e 's/^MinDesiredFrameRate=.*/MinDesiredFrameRate=0.000000/' \
    -e 's/^UseVSync=.*/UseVSync=False/' \
    -e 's/^MaxTextureUnits=.*/MaxTextureUnits=4/' "$P" > "$W/settings.ini"
cat "$W/settings.ini" > "$P"
cd "$UT/System"
# QuickStart=False also enables QuickStart in this demo; omit it entirely.
DYLD_INSERT_LIBRARIES="$W/seed.dylib" UT_FLYBY_CLOCK="$W/clock.csv" \
 UT_FLYBY_CAPTURE="$W/frame-74.ppm" POMPPC_GL_FRAMES="$W/frames.csv" \
 POMPPC_GL_STATS="$W/stats.txt" ./ut2004-bin \
 'AS-Convoy?game=UT2k4Assault.ASGameInfo?NumBots=0' -fullscreen > "$W/game.log" 2>&1 &
p=$!
echo "$p" > "$W/pid"
sleep 8
osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $p) to true" 2>/dev/null || true
# The preload fixes UGameEngine::Tick to a 0.2 s simulation step. Keep
# frames 13..73 (60 intervals = 12 simulated seconds), regardless of wall time.
# Wait beyond frame 73 for the driver's buffered trace to be flushed.
n=0
while :; do
    kill -0 "$p" || { echo 'UT exited early'; exit 1; }
    if ps axww | grep '[Z]enerchi.app/Contents/MacOS/Zenerchi' >/dev/null; then
        echo 'Zenerchi started during measurement; rejecting run'
        exit 1
    fi
    if [ -s "$W/frames.csv" ]; then
        bounds=$(awk -F, '$1==13 {begin=$3} $1==73 {end=$3} NR>1 {last=$3} END {if(begin!="" && end!="" && last>=end+6000) printf "%.6f %.6f\n",begin/1000,(end-begin)/1000}' "$W/frames.csv")
        if [ -n "$bounds" ]; then
            printf '%s\n' "$bounds" > "$W/window.txt"
            break
        fi
    fi
    [ "$n" -lt 540 ] || { echo 'trace timeout'; exit 1; }
    sleep 2
    n=$((n+2))
done
# Refuse results if fixed-step telemetry is missing or contradicts the run.
awk -F, 'NR>1 && $1>=13 && $1<=73 {n++; if($3!=1 || $4!=0 || $5<0.19999 || $5>0.20001 || $6<0.19999 || $6>0.20001 || (n>1 && $2!=tick+1)) bad=1; tick=$2} END {exit(n!=61 || bad)}' "$W/clock.csv"
[ -s "$W/frame-74.ppm" ] || { echo 'missing fixed-frame capture'; exit 1; }
# Diagnostic capture is after the measured interval.
cp '/Users/tiger/Library/Application Support/Unreal Tournament 2004 Demo/System/UT2004.log' "$W/engine.log" || true
printf 'scene=AS-Convoy intro cinematic\nresolution=%s\nbots=0\nmax_texture_units=4\nlibc_seed=0\nsimulation_step_seconds=0.2\nfirst_frame=13\nlast_frame=73\nintervals=60\nsimulated_seconds=12\n' "$RES" > "$W/manifest.txt"
cleanup
trap - EXIT
ps axww > "$W/processes-after.txt"
case "$(cat "$W/exit-status.txt")" in
    0|143) ;;
    *) echo 'UT crashed during cleanup; run is incomplete'; exit 1;;
esac
echo complete > "$W/complete"
