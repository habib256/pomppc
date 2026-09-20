#!/bin/sh
# Persistent UT2004 fullscreen settings and GUI launch. Requires the fullscreen
# GLDriver fix. Put 800x600 (default) or 1024x768 in resolution.txt in this job.
. ./guilib.sh
set -e
RES=800x600
[ ! -f resolution.txt ] || RES=$(cat resolution.txt)
case "$RES" in 800x600|1024x768) ;; *) echo "unsupported resolution: $RES"; exit 1;; esac
mkdir -p out
printf '%s\n' "$RES" > out/resolution.txt
cat > launch.sh <<'GUI'
#!/bin/sh
set -e
RES=$(cat out/resolution.txt)
UT='/Users/tiger/Desktop/Unreal Tournament 2004 Demo.app'
P='/Users/tiger/Library/Application Support/Unreal Tournament 2004 Demo/System/UT2004.ini'
[ -x "$UT/System/ut2004-bin" ] && [ -f "$P" ]
killall ut2004-bin 2>/dev/null || true
sleep 3
cp "$P" out/before.ini
[ -f "$P.before-fullscreen" ] || cp "$P" "$P.before-fullscreen"
sed -e "s/^FullscreenViewportX=.*/FullscreenViewportX=${RES%x*}/" \
    -e "s/^FullscreenViewportY=.*/FullscreenViewportY=${RES#*x}/" \
    -e 's/^StartupFullscreen=.*/StartupFullscreen=True/' "$P" > out/settings.ini
cat out/settings.ini > "$P"
W=/tmp/ut-fullscreen-$RES
mkdir -p "$W"
cd "$UT/System"
POMPPC_GL_STATS="$W/stats.txt" ./ut2004-bin -fullscreen > "$W/game.log" 2>&1 &
p=$!
echo "$p" > "$W/pid"
sleep 8
osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $p) to true" 2>/dev/null || true
sleep 20
kill -0 "$p"
GUI
gui_run 'sh ./launch.sh' 60
W=/tmp/ut-fullscreen-$RES
cp "$W"/* out/
kill -0 "$(cat "$W/pid")"
echo "UT2004 running: fullscreen $RES"
