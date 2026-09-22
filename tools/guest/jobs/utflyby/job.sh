#!/bin/sh
# AS-Convoy's shipped intro camera sequence, no bots and no -benchmark.
# Copy this whole directory to a devloop job; resolution.txt selects the mode.
. ./guilib.sh
set -e
RES=800x600
[ ! -f resolution.txt ] || RES=$(cat resolution.txt)
case "$RES" in 800x600|1024x768) ;; *) echo "bad resolution: $RES"; exit 1;; esac
W=$(mktemp -d /tmp/utflyby-XXXXXX)
mkdir -p out
cp seed.c out/
cp run.sh "$W/"
MACOSX_DEPLOYMENT_TARGET=10.4 /usr/bin/gcc-4.0 -arch ppc -dynamiclib -undefined dynamic_lookup -O2 seed.c -o "$W/seed.dylib" > out/build.txt 2>&1 || { cat out/build.txt; exit 1; }
printf '%s\n' "$RES" > "$W/resolution.txt"
chmod -R 777 "$W"
# gui_run rend le code de sortie de la session (bug hunt T4) ; le garder
# sans sauter le nettoyage ni le rapatriement des journaux.
if gui_run "sh '$W/run.sh' '$W'" 600; then grc=0; else grc=$?; fi
echo "run.sh dans la session : rc=$grc (124 = timeout du relais)"
if [ ! -f "$W/complete" ] && [ -f "$W/runner.pid" ]; then
    kill "$(cat "$W/runner.pid")" 2>/dev/null || true
    n=0
    while [ ! -f "$W/restored" ] && [ "$n" -lt 20 ]; do
        sleep 1
        n=$((n+1))
    done
fi
cp "$W"/* out/
[ -f "$W/complete" ] || { echo "INCOMPLETE: see game.log/run.log"; exit 1; }
[ "$grc" = 0 ] || { echo "run.sh a échoué (rc=$grc)"; exit "$grc"; }
cat "$W/manifest.txt"
