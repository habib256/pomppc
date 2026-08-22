#!/usr/bin/env bash
# caps.sh — sondage des capacités réelles d'un binaire QEMU.
#
# Sourcé par les lanceurs (run_tiger.sh, run_os9.sh) et par scripts/build_qemu_qfb.sh.
# Règle du dépôt :
#
#   UN LANCEUR N'ANNONCE JAMAIS UNE CAPACITÉ QUE LE BINAIRE N'A PAS.
#
# Cette règle vient d'un bug coûteux : le binaire de référence a été reconstruit
# sans le device « screamer », et run_tiger.sh a continué à afficher « + SON »,
# à passer -global screamer.audiodev=snd0 (que QEMU ignore avec un simple
# warning) et — surtout — à raboter la RAM invité de 1024 à 768 Mo « parce que
# le Screamer exige < 1 Go ». 256 Mo perdus pour un device absent.
#
# API :
#   qemu_has_device   <bin> <nom>            # le TYPE QOM est-il enregistré ?
#   qemu_machine_has  <bin> <machine> <nom>  # le device est-il INSTANCIÉ ?
#   qemu_has_netdev   <bin> <nom>            # -netdev help    (user = slirp)
#   qemu_has_audiodev <bin> <nom>            # -audiodev help  (pa, alsa…)
#
# ⚠ « type enregistré » ≠ « device présent dans la machine ». Pour un enfant
# interne comme le Screamer, c'est le câblage macio qui compte, et il peut
# disparaître sans que le type bouge : c'est arrivé (un `git checkout macio.c`
# pour un A/B), et le sondage par type a répondu « oui » sur un binaire
# totalement muet. Pour tout ce qui n'est PAS instancié en ligne de commande,
# utiliser qemu_machine_has.
#
# Le sondage des devices passe par QMP `qom-list-types`, PAS par `-device help` :
# ce dernier ne liste que les devices instanciables en ligne de commande, et le
# Screamer est un enfant interne du macio — il n'y figure donc jamais, même
# présent. Un premier jet de ce fichier s'y est fait piéger. `-M none` suffit
# (les types QOM sont enregistrés indépendamment de la machine) : ~100 ms.
#
# Les trois sondages sont mémoïsés par binaire : les appeler en boucle est gratuit.

_CAPS_BIN=""; _CAPS_DEV=""; _CAPS_NET=""; _CAPS_AUD=""

# Renvoie 0 si le sondage a abouti, 1 s'il a échoué (QEMU n'a pas démarré, hôte
# saturé…). Ce n'est PAS la même chose qu'une capacité absente, et les appelants
# doivent pouvoir faire la différence : sans ça un sondage raté se lit
# « device absent » et déclenche un faux rouge — le pendant exact du faux vert
# que ce fichier existe pour éviter. Une reprise suffit à absorber le transitoire.
_caps_load() {
  local bin="$1" try
  [ "$bin" = "$_CAPS_BIN" ] && return 0
  for try in 1 2; do
    _CAPS_DEV="$(printf '%s\n' \
        '{"execute":"qmp_capabilities"}' \
        '{"execute":"qom-list-types","arguments":{"implements":"device"}}' \
        '{"execute":"quit"}' \
      | "$bin" -M none -display none -qmp stdio 2>/dev/null \
      | tr -d '\r' | tr ',' '\n' || true)"
    # La liste des types QOM fait des dizaines de Kio : une réponse minuscule
    # signifie que QEMU n'a pas répondu, pas qu'il n'a aucun device.
    [ "${#_CAPS_DEV}" -gt 1000 ] && break
  done
  if [ "${#_CAPS_DEV}" -le 1000 ]; then
    echo "caps: sondage QOM de '$bin' échoué (réponse tronquée)" >&2
    _CAPS_BIN=""; return 1
  fi
  _CAPS_BIN="$bin"
  _CAPS_NET="$("$bin" -netdev help   2>/dev/null | tr -d '\r' || true)"
  _CAPS_AUD="$("$bin" -audiodev help 2>/dev/null | tr -d '\r' || true)"
  return 0
}

# qom-list-types imprime : {"name": "screamer"} (après le tr ',' '\n' ci-dessus).
# PAS d'ancre de fin de ligne : les réponses QMP se terminent par \r\n, si bien
# que l'ancre ne matchait QUE si l'entrée n'était pas la dernière de la liste —
# et cet ordre varie d'un lancement à l'autre. Le test était intermittent, ce qui
# est pire qu'un test faux. Les guillemets fermants suffisent à être exact.
qemu_has_device() {
  _caps_load "$1" || return 2      # 2 = sondage impossible, ≠ 1 = absent
  printf '%s\n' "$_CAPS_DEV" | grep -qF "\"name\": \"$2\""
}

# -netdev help / -audiodev help impriment un nom par ligne.
qemu_has_netdev() {
  _caps_load "$1" || return 2
  printf '%s\n' "$_CAPS_NET" | grep -qx "[[:space:]]*$2[[:space:]]*"
}

qemu_has_audiodev() {
  _caps_load "$1" || return 2
  printf '%s\n' "$_CAPS_AUD" | grep -qx "[[:space:]]*$2[[:space:]]*"
}

# Le device est-il réellement présent dans l'arbre QOM de la machine ?
# On démarre la machine figée (-S : rien ne tourne) et on lit `info qom-tree`.
# ~300 ms, mémoïsé par (binaire, machine).
_MACH_KEY=""; _MACH_TREE=""
qemu_machine_has() {
  local bin="$1" machine="$2" name="$3" key="$1|$2"
  if [ "$key" != "$_MACH_KEY" ]; then
    _MACH_KEY="$key"
    _MACH_TREE="$(printf '%s\n' \
        '{"execute":"qmp_capabilities"}' \
        '{"execute":"human-monitor-command","arguments":{"command-line":"info qom-tree"}}' \
        '{"execute":"quit"}' \
      | "$bin" -M "$machine" -S -display none -qmp stdio 2>/dev/null || true)"
  fi
  if [ "${#_MACH_TREE}" -le 1000 ]; then
    echo "caps: sondage qom-tree de '$machine' échoué" >&2
    _MACH_KEY=""; return 2
  fi
  printf '%s\n' "$_MACH_TREE" | grep -q "($name)"
}
