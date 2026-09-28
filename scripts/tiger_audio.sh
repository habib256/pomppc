#!/usr/bin/env bash
# GPL3 - Copyleft VERHILLE Arnaud
# Sourced after hostcompat.sh. Keep OS 9 and Linux's existing audio defaults.
tiger_audio_device() {
  local profile="${POMPPC_AUDIO_PROFILE:-stable}"
  case "$profile" in
    stable|default) ;;
    *) echo "POMPPC_AUDIO_PROFILE doit être stable ou default" >&2; return 2 ;;
  esac
  if [ "$HOST_AUDIODEV" = coreaudio ] && [ "$profile" = stable ]; then
    # ~93 ms of queued PCM instead of ~46 ms; 5 ms producer tick instead of 10.
    # This is reserve capacity, not a guarantee of actual output latency.
    printf '%s\n' 'coreaudio,id=snd0,timer-period=5000,out.frequency=44100,out.buffer-length=11610,out.buffer-count=8'
  else
    printf '%s,id=snd0\n' "$HOST_AUDIODEV"
  fi
}
