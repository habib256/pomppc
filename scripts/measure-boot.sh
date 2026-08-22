#!/usr/bin/env bash
# Mesure le temps de boot (headless) jusqu'à l'écran bleu (login/bureau).
# Détection par couleur moyenne: boot=gris neutre / kernel -v=noir / login=bleu.
# Usage: scripts/measure-boot.sh [timeout_s]
#
# C'est L'INSTRUMENT DU PROJET : toute la campagne d'optimisation repose sur le
# CPU_qemu qu'il produit. Trois garde-fous, tous appris à la dure :
#
#   • le PID vient de -pidfile, pas d'un pgrep sur la ligne de commande. Un
#     QEMU orphelin d'un run précédent faisait lire /proc/<mauvais_pid>/stat et
#     produisait une mesure FAUSSE MAIS PLAUSIBLE — le pire mode de panne.
#   • un trap tue la VM sur Ctrl-C / erreur / fin. Sans lui, une mesure
#     interrompue laissait un QEMU tourner en fond, c'est-à-dire exactement la
#     « charge hôte » qui a déjà faussé une campagne entière.
#   • un pré-vol refuse de mesurer si un autre QEMU tourne ou si l'hôte est
#     chargé. Le README demandait de le vérifier à la main ; c'est le genre de
#     vérification qu'on saute précisément quand on est pressé.
#
# POMPPC_FORCE=1 passe outre le pré-vol (à n'utiliser que pour du debug, jamais
# pour un chiffre qu'on garde).
set -uo pipefail   # pas de -e: les comparaisons arithmétiques fausses ne doivent pas tuer le script
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/config.env"
TIMEOUT="${1:-360}"
MAXLOAD="${MAXLOAD:-1.0}"

SCR="${POMPPC_SCRATCH:-$ROOT/.run}"; mkdir -p "$SCR"
MON="$SCR/mon.sock"; rm -f "$MON"
PIDFILE="$SCR/measure.pid"; rm -f "$PIDFILE"
FRAMES="$ROOT/bench/frames"; mkdir -p "$FRAMES"; rm -f "$FRAMES"/*.png 2>/dev/null || true
BOOTDEV='hd:10,\System\Library\CoreServices\BootX'

# --- outils indispensables ---
# Sans ImageMagick, meancolor() renvoyait du vide, la comparaison arithmétique
# valait 0, et on obtenait un timeout silencieux au lieu d'une erreur.
for tool in convert bc python3; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "ERREUR: '$tool' introuvable. La détection d'écran bleu en dépend." >&2
    echo "        sudo apt-get install -y imagemagick bc python3" >&2
    exit 2; }
done
[ -x "$QEMU_BIN" ] || command -v "$QEMU_BIN" >/dev/null 2>&1 || {
  echo "ERREUR: QEMU introuvable ($QEMU_BIN)." >&2; exit 2; }

# --- pré-vol : hôte au repos ? ---
preflight() {
  local other load
  # `pgrep -c -x` était doublement inutilisable ici : -x ne matche pas
  # « qemu-system-ppc64 » (comm est tronqué à 15 caractères) et -c imprime « 0 »
  # TOUT EN sortant en erreur, si bien que le `|| echo 0` ajoutait un second 0 —
  # d'où un « 0\n0 » qui cassait l'arithmétique et laissait passer le pré-vol.
  # `[q]` empêche pgrep de se compter lui-même ; wc -l rend toujours un entier.
  other=$(pgrep -f '[q]emu-system-ppc' 2>/dev/null | wc -l)
  if [ "$other" -gt 0 ]; then
    echo "PRÉ-VOL: $other QEMU tourne(nt) déjà — la mesure serait faussée." >&2
    echo "         pkill -x qemu-system-ppc ; ou POMPPC_FORCE=1 pour passer outre." >&2
    return 1
  fi
  load=$(cut -d' ' -f1 /proc/loadavg)
  if [ "$(echo "$load > $MAXLOAD" | bc -l)" = "1" ]; then
    echo "PRÉ-VOL: charge hôte $load > $MAXLOAD — attends que la machine se calme." >&2
    echo "         (même le CPU_qemu gonfle sous charge : l'invité spin-attend ses timers)" >&2
    echo "         MAXLOAD=<n> pour relever le seuil, POMPPC_FORCE=1 pour passer outre." >&2
    return 1
  fi
  return 0
}
if [ -z "${POMPPC_FORCE:-}" ]; then
  preflight || exit 3
else
  echo "⚠ pré-vol contourné (POMPPC_FORCE=1) : ce chiffre n'est pas publiable."
fi

# --- nettoyage : ne JAMAIS laisser un QEMU derrière soi ---
QPID=""
cleanup() {
  # Relire le pidfile si QPID n'a pas encore été affecté : sinon un QEMU qui
  # démarre lentement (pidfile pas encore écrit quand on abandonne) survit au
  # script et pollue la mesure SUIVANTE — précisément le mode de panne que ce
  # trap existe pour supprimer. Vu en vrai sur une boucle A/B : le premier run
  # laissait un orphelin qui verrouillait le qcow2 et faisait échouer les trois
  # suivants.
  [ -z "$QPID" ] && QPID=$(cat "$PIDFILE" 2>/dev/null || true)
  if [ -n "$QPID" ] && kill -0 "$QPID" 2>/dev/null; then
    kill -TERM "$QPID" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$QPID" 2>/dev/null || break; sleep 0.1; done
    kill -KILL "$QPID" 2>/dev/null
  fi
  rm -f "$PIDFILE"
}
trap cleanup EXIT INT TERM

# Overrides pour expérimenter (SMP, MTTCG, tb-size…). Ex:
#   SMP_N=2 EXTRA_ARGS="-accel tcg,thread=multi,tb-size=256" scripts/measure-boot.sh
SMP_N="${SMP_N:-$SMP}"
read -r -a EXTRA <<< "${EXTRA_ARGS:-}"
echo "### config: SMP=$SMP_N  extra='${EXTRA_ARGS:-}'  charge=$(cut -d' ' -f1 /proc/loadavg)"

qmon(){ python3 "$ROOT/scripts/moncmd.py" "$MON" "$1" 2>/dev/null; }
meancolor(){ # -> "R G B" moyen d'un ppm
  convert "$1" -resize 1x1 -format "%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]" info:
}

START=$(date +%s.%N)
setsid "$QEMU_BIN" -M "$MACHINE" -cpu "$CPU" -m "$RAM_MB" -smp "$SMP_N" \
  -display none -g "$RES" \
  -drive "file=$DISK,format=qcow2,media=disk" \
  "${NET_ARGS[@]}" \
  -prom-env 'auto-boot?=true' -prom-env "boot-device=$BOOTDEV" -prom-env 'boot-args=-v' \
  -serial "file:$ROOT/bench/measure.log" -name "POMPPC-measure" \
  -pidfile "$PIDFILE" \
  -monitor "unix:$MON,server,nowait" "${EXTRA[@]}" \
  >/dev/null 2>"$SCR/measure-qemu.err" &

# attendre le socket ET le pidfile (QEMU écrit le second juste après le premier)
for _ in $(seq 1 30); do [ -S "$MON" ] && [ -s "$PIDFILE" ] && break; sleep 0.5; done
QPID=$(cat "$PIDFILE" 2>/dev/null || true)
if [ -z "$QPID" ] || ! kill -0 "$QPID" 2>/dev/null; then
  echo "ERREUR: QEMU n'a pas démarré (pas de PID dans $PIDFILE)." >&2
  # Sa stderr était jetée : on ne savait jamais POURQUOI (typiquement un autre
  # QEMU détenant le verrou du qcow2).
  [ -s "$SCR/measure-qemu.err" ] && sed 's/^/  qemu: /' "$SCR/measure-qemu.err" >&2
  exit 4
fi
echo "### qemu pid=$QPID (via -pidfile)"

HZ=$(getconf CLK_TCK)
cpu_time(){ # temps CPU (s) de QEMU: (utime+stime)/HZ — immunisé à la contention
  [ -r "/proc/$QPID/stat" ] || { echo "?"; return; }
  awk -v hz="$HZ" '{print ($14+$15)/hz}' "/proc/$QPID/stat"
}

echo "t(s)  R   G   B   état"
BOOT_T=""
CPU_T=""
i=0
while :; do
  NOW=$(date +%s.%N); EL=$(echo "$NOW - $START" | bc)
  ELI=${EL%.*}; ELI=${ELI:-0}; [ -z "$ELI" ] && ELI=0
  if (( ELI > TIMEOUT )); then echo "timeout ${TIMEOUT}s"; break; fi
  PPM="$SCR/mframe.ppm"
  qmon "screendump $PPM" >/dev/null 2>&1 || { echo "VM disparue à ${ELI}s"; break; }
  [ -f "$PPM" ] || { sleep 2; continue; }
  read R G B < <(meancolor "$PPM")
  # snapshot horodaté pour vérification visuelle
  printf -v idx "%03d" "$i"; convert "$PPM" "$FRAMES/f${idx}_${ELI}s.png" 2>/dev/null || true
  # login/bureau = bleu dominant et lumineux
  STATE="boot"
  if (( B > R + 12 )) && (( B > 110 )); then STATE="BLEU(login/bureau)"; fi
  printf "%4s %3s %3s %3s  %s\n" "$ELI" "$R" "$G" "$B" "$STATE"
  if [ "$STATE" != "boot" ]; then
    BOOT_T="$ELI"
    CPU_T=$(cpu_time)   # lu AVANT le quit, sinon /proc/<pid> a disparu
    break
  fi
  i=$((i+1)); sleep 2
done

[ -n "$CPU_T" ] || CPU_T=$(cpu_time)
echo
if [ -n "$BOOT_T" ]; then
  echo "=== écran bleu atteint : wall=${BOOT_T}s | CPU_qemu=${CPU_T}s (${SMP_N} CPU, ${RAM_MB}Mo) ==="
  echo "wall=$BOOT_T cpu=$CPU_T smp=$SMP_N extra='${EXTRA_ARGS:-}'" > "$ROOT/bench/last-boot.txt"
else
  echo "=== écran bleu non détecté (voir bench/frames/) | CPU_qemu=${CPU_T}s ==="
fi
# couper la VM (le trap s'en charge aussi, mais un quit propre vaut mieux)
qmon "quit" >/dev/null 2>&1 || true
echo "Frames: $ROOT/bench/frames/"
