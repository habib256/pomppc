#!/usr/bin/env bash
# GPL3 - Copyleft VERHILLE Arnaud
# hostcompat.sh — ce qui diffère entre un hôte Linux et un hôte macOS.
#
# Sourcé par run_tiger.sh, run_os9.sh et scripts/boot.sh. Les lanceurs ont été
# écrits sous Linux ; sous macOS il n'y a ni flock(1), ni setsid(1), ni GTK
# (le QEMU construit par scripts/build_qemu_qfb.sh y a Cocoa), ni PulseAudio
# (CoreAudio). Sans ce fichier, `flock` introuvable se lisait « verrou déjà
# pris » et le lanceur refusait de démarrer (vu en vrai).
#
# API :
#   host_lock_fd FD         # verrou exclusif non bloquant sur le fd FD ; 1 si déjà pris
#   HOST_DISPLAY            # affichage par défaut : cocoa (macOS) ou gtk
#   HOST_AUDIODEV           # backend audio : coreaudio (macOS) ou pa
#   host_audio_env          # prépare l'environnement du backend audio
#   host_detach CMD…        # lance CMD détaché de la session (setsid si présent)
#   host_loadavg            # charge moyenne sur 1 min (point décimal)
#   host_now                # horloge murale en secondes, avec décimales
#   host_cpu_time PID       # temps CPU cumulé (s) de PID, « ? » s'il est parti
#   host_lock_vm FD FICHIER # prend le verrou disque de la VM sur FD, ou sort
#
# Autre piège macOS, traité dans les scripts eux-mêmes : bash 3.2 (celui de
# macOS) tient un tableau VIDE pour non défini sous `set -u`, et
# "${TAB[@]}" arrête le script (« unbound variable »). Les scripts écrivent
# donc ${TAB[@]+"${TAB[@]}"}, qui vaut la même chose sous bash 5.

case "$(uname -s)" in
  Darwin) HOST_DISPLAY=cocoa; HOST_AUDIODEV=coreaudio ;;
  *)      HOST_DISPLAY=gtk;   HOST_AUDIODEV=pa ;;
esac

# Le verrou appartient à la description de fichier ouverte : il survit à la
# sortie de perl tant que le shell (puis QEMU, après exec) garde le fd ouvert.
host_lock_fd() {
  local fd="$1"
  if command -v flock >/dev/null 2>&1; then
    flock -n "$fd"
  else
    perl -e 'use Fcntl ":flock"; open(my $f, ">&=", $ARGV[0]) or exit 2;
             exit(flock($f, LOCK_EX | LOCK_NB) ? 0 : 1)' "$fd"
  fi
}

host_audio_env() {
  if [ "$HOST_AUDIODEV" = pa ]; then
    export PULSE_SERVER="${PULSE_SERVER:-unix:/run/user/$(id -u)/pulse/native}"
  fi
  if [ "$HOST_DISPLAY" = gtk ]; then
    export DISPLAY="${DISPLAY:-:1}"
  fi
}

# /proc n'existe pas sous macOS : vm.loadavg rend « { 1.23 1.45 1.67 } », avec
# une virgule décimale si la locale est française — d'où LC_ALL=C.
host_loadavg() {
  if [ -r /proc/loadavg ]; then
    cut -d' ' -f1 /proc/loadavg
  else
    LC_ALL=C sysctl -n vm.loadavg | awk '{print $2}'
  fi
}

# `date +%s.%N` rend « …%N » littéral sous macOS : bc échoue ensuite.
host_now() {
  perl -MTime::HiRes=time -e 'printf "%.3f\n", time'
}

host_cpu_time() {
  local pid="$1" hz
  if [ -r "/proc/$pid/stat" ]; then
    hz=$(getconf CLK_TCK)
    awk -v hz="$hz" '{print ($14+$15)/hz}' "/proc/$pid/stat"
  else
    # ps : [[h:]m:]s.cc
    LC_ALL=C ps -o time= -p "$pid" 2>/dev/null |
      awk -F: 'NF { t = 0; for (i = 1; i <= NF; i++) t = t * 60 + $i; print t; ok = 1 }
               END { if (!ok) print "?" }'
  fi
}

# Même verrou que run_tiger.sh (même qcow2, même socket moniteur) : les outils
# de boot ne doivent ni lancer un second QEMU sur le disque d'une VM vivante,
# ni supprimer son socket moniteur. Le fd survit au lancement de QEMU.
host_lock_vm() {
  local fd="$1" file="$2"
  eval "exec $fd>\"\$file\""
  if ! host_lock_fd "$fd"; then
    echo "⚠  une VM Tiger tourne déjà (verrou $file) : ferme-la d'abord." >&2
    exit 1
  fi
}

host_detach() {
  if command -v setsid >/dev/null 2>&1; then
    setsid "$@"
  else
    nohup "$@"
  fi
}
