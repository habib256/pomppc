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
#   qemu_has_device     <bin> <nom>            # le TYPE QOM est-il enregistré ?
#   qemu_machine_has    <bin> <machine> <nom>  # le device est-il INSTANCIÉ ?
#   qemu_has_netdev     <bin> <nom>            # -netdev help    (user = slirp)
#   qemu_has_audiodev   <bin> <nom>            # -audiodev help  (pa, alsa…)
#   qemu_cpu_has_fastfp <bin> <machine> <cpu>  # propriété de CPU x-fast-fp ?
#   qemu_machine_smp_ok <bin> <machine> <n>    # la machine accepte-t-elle -smp n ?
#   qemu_qgpu_has_backend   <bin> <machine> <backend>  # backend de rendu hôte utilisable ?
#   qemu_qgpu_backend_taken <bin> <machine> <backend>  # … et lequel est réellement pris
#   qemu_qgpu_device_ok <bin> <machine> <options>      # -device qgpu-pci,<options> accepté ?
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
#
# ⚠ Jamais `printf … | grep -q` ici : les appelants tournent sous `set -o
# pipefail`, grep -q s'arrête au premier résultat, printf prend SIGPIPE et le
# tube échoue — « device absent » alors qu'il est présent. La liste des types
# de qemu-system-ppc64 est assez longue pour que ça arrive (vu en vrai avec
# GPU=1). D'où les redirections `<<<`.

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
  grep -qF "\"name\": \"$2\"" <<< "$_CAPS_DEV"
}

# -netdev help / -audiodev help impriment un nom par ligne.
qemu_has_netdev() {
  _caps_load "$1" || return 2
  grep -qx "[[:space:]]*$2[[:space:]]*" <<< "$_CAPS_NET"
}

qemu_has_audiodev() {
  _caps_load "$1" || return 2
  grep -qx "[[:space:]]*$2[[:space:]]*" <<< "$_CAPS_AUD"
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
  grep -q "($name)" <<< "$_MACH_TREE"
}

# Le GPU paravirtuel a-t-il une CIBLE DE PRÉSENTATION ? Le device ne tient
# SURF_PRESENT (v13) que s'il a trouvé un écran où écrire : qfb-pci, sinon le
# framebuffer VGA de la machine. Sans cible, QGPU_CAP_SCANOUT manque et l'invité
# relit chaque image pour la recopier lui-même — soit 10 % de débit en moins sur
# un jeu. Le device était présent, la version bonne, les tests natifs verts, et
# ça n'a été découvert qu'en lisant QGPUCaps dans l'invité : d'où ce sondage.
# La trace du device dit à sa naissance quelle cible il a prise.
qemu_qgpu_has_scanout() { # <bin> <machine>
  local out
  out="$(printf 'quit\n' | "$1" -M "$2" -S -display none \
         -device qgpu-pci,trace=on -monitor stdio 2>&1 || true)"
  grep -q 'scanout sur' <<< "$out"
}

# --- Sondage par DÉMARRAGE : « QEMU accepte-t-il cette ligne de commande ? » ---
#
# Certaines capacités ne sont ni un type QOM ni un nœud de l'arbre : ce sont des
# options (propriété de CPU, option de device, topologie -smp). Pour celles-là
# le seul sondage honnête est de démarrer la machine FIGÉE (-S, rien ne tourne)
# avec la ligne exacte du lanceur et de regarder si QEMU répond.
#
# ⚠ Une option refusée n'est PAS un warning : QEMU s'arrête, APRÈS avoir
# imprimé le salut QMP mais sans jamais répondre à qmp_capabilities. C'est ce
# qui distingue les deux cas — et c'est pourquoi un lanceur ne doit jamais
# poser une de ces options à l'aveugle : il ne dégraderait pas, il ne
# démarrerait pas.
#
# stdout et stderr sont FUSIONNÉS dans _CAPS_BOOT_OUT : les devices tracent sur
# stderr (« qgpu-pci: backend gl »), et c'est ce qui permet de sonder non
# seulement « ça démarre » mais « avec quoi ».
_CAPS_BOOT_OUT=""
_caps_boot() { # <bin> <args…>   -> 0 si QEMU a démarré
  local bin="$1"; shift
  _CAPS_BOOT_OUT="$(printf '%s\n' \
      '{"execute":"qmp_capabilities"}' \
      '{"execute":"quit"}' \
    | "$bin" "$@" -S -display none -qmp stdio 2>&1 || true)"
  grep -q '"return"' <<< "$_CAPS_BOOT_OUT"
}

# La propriété de CPU « x-fast-fp » (mode flottant rapide, patches/fastfp/) :
# le binaire l'a-t-il ?
#
# Le sondage ne peut pas passer par qom-list-types : x-fast-fp n'est pas un
# type, c'est une PROPRIÉTÉ d'un type de CPU, et le nom de ce type dépend de la
# version choisie derrière l'alias (« g4 » -> « 7400_v2.9-powerpc-cpu » côté
# ppc, « ...-powerpc64-cpu » côté ppc64).
#
# Il ne passe plus non plus par un qom-get sur /machine/unattached/device[0] :
# rien ne garantit que ce nœud soit le CPU (c'est l'ordre de création des
# objets non rattachés, pas un contrat), et sur une machine où il ne l'est pas
# le sondage répondait « absente » — repli silencieux en flottant exact sur un
# binaire qui avait la propriété. Le critère juste, et le seul qui compte pour
# le lanceur, est : « QEMU démarre-t-il avec -cpu g4,x-fast-fp=on ? ».
#
# Codes : 0 = présente, 1 = absente, 2 = sondage impossible. Le 2 demande un
# témoin (la même machine SANS la propriété) : sans lui, un hôte saturé se
# lirait « absente » — le faux rouge que ce fichier existe pour éviter.
_FFP_KEY=""; _FFP_RC=2
# qemu_cpu_has_prop <bin> <machine> <cpu> <prop=val> : QEMU démarre-t-il avec
# -cpu <cpu>,<prop=val> ? (non mémoïsé ; ~150 ms)
qemu_cpu_has_prop() {
  _caps_boot "$1" -M "$2" -cpu "$3,$4"
}

# qemu_tcg_has_prop <bin> <machine> <prop=val> : l'accélérateur TCG accepte-t-il
# -accel tcg,<prop=val> ? (x-jit-near, patches/tcg/0006 ; une propriété inconnue
# fait quitter QEMU, d'où le sondage)
qemu_tcg_has_prop() {
  _caps_boot "$1" -M "$2" -accel "tcg,$3"
}

qemu_cpu_has_fastfp() { # <bin> <machine> <cpu>
  local key="$1|$2|$3"
  [ "$key" = "$_FFP_KEY" ] && return $_FFP_RC
  _FFP_KEY="$key"
  if _caps_boot "$1" -M "$2" -cpu "$3,x-fast-fp=on"; then
    _FFP_RC=0
  elif _caps_boot "$1" -M "$2" -cpu "$3"; then
    _FFP_RC=1                      # QEMU démarre sans : la propriété manque.
  else
    echo "caps: sondage x-fast-fp de '$1' échoué" >&2
    _FFP_KEY=""; _FFP_RC=2         # même sans la propriété, rien ne démarre.
  fi
  return $_FFP_RC
}

# La machine accepte-t-elle N processeurs sur CE binaire ?
#
# SMP était la seule capacité du lanceur jamais sondée : `[ -x "$QEMU_BIN64" ]`
# teste qu'un fichier existe, pas que la machine ait le bring-up SMP mac99
# (patches/smp-mac99/). Sur un binaire distro, le fichier existe et QEMU meurt
# à l'exec final sur « Invalid SMP CPUs 2 » — exactement le mode de panne que
# ce fichier existe pour supprimer, un cran plus tard.
#
# ⚠ -smp 2 sur mac99 EXIGE via=pmu : sans lui le device 'gpio' n'existe pas et
# le câblage du kick CPU1 part sur un IRQ nul. Sonder avec la machine RÉELLE
# du lanceur (« mac99,via=pmu ») vérifie donc les deux d'un coup.
#
# Codes : 0 = accepté, 1 = refusé, 2 = sondage impossible (témoin -smp 1).
_SMP_KEY=""; _SMP_RC=2
qemu_machine_smp_ok() { # <bin> <machine> <n>
  local key="$1|$2|$3"
  [ "$key" = "$_SMP_KEY" ] && return $_SMP_RC
  _SMP_KEY="$key"
  if _caps_boot "$1" -M "$2" -smp "$3"; then
    _SMP_RC=0
  elif _caps_boot "$1" -M "$2" -smp 1; then
    _SMP_RC=1                      # QEMU démarre en mono : c'est bien -smp N.
  else
    echo "caps: sondage -smp $3 de '$1' échoué" >&2
    _SMP_KEY=""; _SMP_RC=2
  fi
  return $_SMP_RC
}

# Le device qgpu-pci accepte-t-il ces options sur ce binaire ?
# Ex. : qemu_qgpu_device_ok "$BIN" "$MACHINE" "scanout=qfb"
# Codes : 0 = accepté, 1 = refusé, 2 = sondage impossible.
qemu_qgpu_device_ok() { # <bin> <machine> <options>
  if _caps_boot "$1" -M "$2" -device "qgpu-pci,$3"; then
    return 0
  elif _caps_boot "$1" -M "$2"; then
    return 1
  fi
  echo "caps: sondage de 'qgpu-pci,$3' sur '$1' échoué" >&2
  return 2
}

# Quel backend de rendu hôte le device prend-il RÉELLEMENT ?
#
# C'est le pendant du sondage Screamer, pour un piège plus dur : `backend=gl`
# n'a AUCUN repli côté cœur (qgpu-core.c : le cas 'gl' laisse c->be à NULL si
# l'init échoue) et realize() fait alors error_setg — QEMU refuse de démarrer
# sur un hôte sans EGL (binaire construit sans les en-têtes, session sans DRI,
# conteneur). Le lanceur imposait `backend=gl` que personne n'avait sondé,
# pendant que le build sondait en `auto`, qui réussit toujours : la capacité
# annoncée n'était jamais celle qui était vérifiée.
#
# `trace=on` fait imprimer « qgpu-pci: backend <nom> » par realize() : un seul
# démarrage répond aux deux questions (accepté ? et avec quoi ?), ce qui permet
# à la bannière d'annoncer le backend PRIS et non le backend demandé — en
# `auto`, 'gl' et 'soft' se ressemblent sur la ligne de commande et pas du tout
# à l'écran.
#
# Le nom est imprimé sur stdout (vide si le binaire est trop vieux pour tracer,
# ce qui n'empêche pas le 0 : le device, lui, a bien été accepté).
# Codes : 0 = accepté, 1 = refusé, 2 = sondage impossible.
_QGPU_KEY=""; _QGPU_RC=2; _QGPU_BE=""
qemu_qgpu_backend_taken() { # <bin> <machine> <backend>
  local key="$1|$2|$3"
  if [ "$key" != "$_QGPU_KEY" ]; then
    _QGPU_KEY="$key"; _QGPU_BE=""
    if _caps_boot "$1" -M "$2" -device "qgpu-pci,backend=$3,trace=on"; then
      _QGPU_RC=0
      _QGPU_BE="$(sed -n 's/.*qgpu-pci: backend \([A-Za-z0-9_]*\).*/\1/p' \
                  <<< "$_CAPS_BOOT_OUT" | head -1)"
    elif _caps_boot "$1" -M "$2"; then
      _QGPU_RC=1                   # QEMU démarre sans le device : backend KO.
    else
      echo "caps: sondage du backend qgpu '$3' sur '$1' échoué" >&2
      _QGPU_KEY=""; _QGPU_RC=2
    fi
  fi
  [ -n "$_QGPU_BE" ] && printf '%s\n' "$_QGPU_BE"
  return $_QGPU_RC
}

# Même sondage, sans le nom : « ce backend est-il utilisable ici ? »
qemu_qgpu_has_backend() { # <bin> <machine> <backend>
  local rc=0
  qemu_qgpu_backend_taken "$1" "$2" "$3" >/dev/null || rc=$?
  return $rc
}
