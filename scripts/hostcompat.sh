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
#   host_sock_alive CHEMIN  # 0 si un QEMU écoute sur ce socket unix
#   host_mon_path CHEMIN    # socket moniteur à utiliser : CHEMIN pour la VM qui
#                           # tient le verrou, CHEMIN-PID.sock en SNAPSHOT=1
#   host_publish_vm NOM MON PORT  # .run/NOM.mon et .run/NOM.sshport (VM verrouillée)
#   host_locked FICHIER     # 0 si un processus tient le verrou de FICHIER
#   host_wait_unlocked FICHIER S  # attend S s que le verrou soit libre ; 1 sinon
#   host_lock_holders FICHIER     # PID des processus qui ont FICHIER ouvert
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
    # LC_ALL=C sur les DEUX côtés du tube : l'awk de macOS suit LC_NUMERIC et,
    # en locale française, lisait « 43.31 » comme 43.
    LC_ALL=C ps -o time= -p "$pid" 2>/dev/null |
      LC_ALL=C awk -F: 'NF { t = 0; for (i = 1; i <= NF; i++) t = t * 60 + $i; print t; ok = 1 }
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

# Un moniteur HMP n'accepte qu'un client : pendant qu'un outil le tient
# (killgame.py garde sa connexion toute sa boucle), les connect() suivants
# peuvent rendre ECONNREFUSED une fois la file d'attente pleine. Un refus ne
# prouve donc pas que le socket est mort : il l'est seulement si aucun QEMU
# vivant ne l'a sur sa ligne de commande (-monitor unix:CHEMIN,server…).
host_sock_alive() {
  [ -S "$1" ] || return 1
  if python3 -c 'import socket, sys
s = socket.socket(socket.AF_UNIX)
s.settimeout(1)
try:
    s.connect(sys.argv[1])
except OSError:
    sys.exit(1)' "$1" 2>/dev/null; then
    return 0
  fi
  pgrep -f "unix:$1,server" >/dev/null 2>&1
}

# Le socket moniteur se choisit selon le MODE, pas selon la vivacité du
# socket (29/09/2026, bug hunt 4) : la VM qui tient le verrou disque garde
# toujours CHEMIN, que tous les outils visent ; une VM SNAPSHOT=1 prend
# toujours CHEMIN-PID.sock. Avant, la dernière VM arrivée prenait le socket
# propre à l'instance, qu'elle soit la VM quotidienne ou non : la matrice,
# killgame.py et cycle.sh visaient alors la VM jetable d'un autre agent.
# Si CHEMIN est encore tenu par une VM (lanceur d'avant ce correctif, en
# SNAPSHOT=1), on ne le supprime pas : on prend le nôtre et on le dit — les
# outils le trouvent par host_publish_vm.
host_mon_path() {
  local base="$1"
  if [ -n "${SNAPSHOT:-}" ]; then
    echo "${base%.sock}-$$.sock"
  elif host_sock_alive "$base"; then
    echo "⚠  $base est tenu par une autre VM : moniteur ${base%.sock}-$$.sock" >&2
    echo "${base%.sock}-$$.sock"
  else
    rm -f "$base"
    echo "$base"
  fi
}

# Publié par la VM qui tient le verrou (jamais en SNAPSHOT=1) : les outils
# lisent le moniteur et le port ssh EFFECTIFS au lieu de supposer mon.sock et
# 2222 (le port glisse si 2222 est pris). Écriture atomique (mv).
host_publish_vm() {
  local nom="$1" mon="$2" port="$3" d
  [ -n "${SNAPSHOT:-}" ] && return 0
  d="$(dirname "$mon")"
  printf '%s\n' "$mon" > "$d/$nom.mon.$$" && mv -f "$d/$nom.mon.$$" "$d/$nom.mon"
  printf '%s\n' "$port" > "$d/$nom.sshport.$$" && mv -f "$d/$nom.sshport.$$" "$d/$nom.sshport"
}

# JAMAIS de `rm -f tiger.lock` (29/09/2026, bug hunt 4) : le verrou est un
# flock sur l'inode. Supprimer le chemin puis relancer crée un nouvel inode et
# un second verrou alors que l'ancien QEMU tourne peut-être encore — et sous
# macOS QEMU ne verrouille pas l'image lui-même (pas de F_OFD_SETLK) : deux
# QEMU écrivent alors le même qcow2, le HFS+ de l'invité se corrompt. Un flock
# disparaît de lui-même avec le dernier processus qui tient le fd : un fichier
# resté après un arrêt brutal ne verrouille rien, il n'y a rien à supprimer.
host_locked() {
  [ -e "$1" ] || return 1
  perl -e 'use Fcntl ":flock"; open(my $f, "<", $ARGV[0]) or exit 1;
           if (flock($f, LOCK_EX | LOCK_NB)) { flock($f, LOCK_UN); exit 1 } exit 0' "$1"
}

host_lock_holders() {
  lsof -t "$1" 2>/dev/null | tr '\n' ' '
}

host_wait_unlocked() {
  local file="$1" delai="${2:-60}" i=0
  while host_locked "$file"; do
    if [ "$i" -ge "$delai" ]; then
      echo "⚠  le verrou $file est toujours tenu (PID : $(host_lock_holders "$file"))." >&2
      echo "   Rien n'est relancé : arrêter cette VM à la main, sans supprimer le verrou." >&2
      return 1
    fi
    sleep 1; i=$((i + 1))
  done
  return 0
}

host_detach() {
  if command -v setsid >/dev/null 2>&1; then
    setsid "$@"
  else
    nohup "$@"
  fi
}
