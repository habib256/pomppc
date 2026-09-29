#!/usr/bin/env bash
# GPL3 - Copyleft VERHILLE Arnaud
# Sourced after hostcompat.sh. Keep OS 9 and Linux's existing audio defaults.
tiger_audio_device() {
  local profile="${POMPPC_AUDIO_PROFILE:-stable}"
  case "$profile" in
    stable|default) ;;
    # muet : le Screamer reste instancié (même machine, RAM plafonnée à 768 Mo
    # comme avec le son) mais rien ne sort — bancs sans surveillance
    # (tools/endurance/), qui ne doivent pas jouer le carillon à chaque démarrage.
    muet) printf '%s\n' 'none,id=snd0'; return 0 ;;
    *) echo "POMPPC_AUDIO_PROFILE doit être stable, default ou muet" >&2; return 2 ;;
  esac
  if [ "$HOST_AUDIODEV" = coreaudio ] && [ "$profile" = stable ]; then
    # ~93 ms of queued PCM instead of ~46 ms; 5 ms producer tick instead of 10.
    # This is reserve capacity, not a guarantee of actual output latency.
    printf '%s\n' 'coreaudio,id=snd0,timer-period=5000,out.frequency=44100,out.buffer-length=11610,out.buffer-count=8'
  else
    printf '%s,id=snd0\n' "$HOST_AUDIODEV"
  fi
}
