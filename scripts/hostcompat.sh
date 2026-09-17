#!/usr/bin/env bash
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

host_detach() {
  if command -v setsid >/dev/null 2>&1; then
    setsid "$@"
  else
    nohup "$@"
  fi
}
