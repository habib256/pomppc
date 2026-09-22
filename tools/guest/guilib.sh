# guilib.sh — à sourcer dans un job.sh de devloop en mode bureau.
#
#   . ./guilib.sh
#   gui_run 'commande shell' [délai_s]
#
# Exécute la commande DANS la session graphique de l'utilisateur de test (via
# POMPPCGuiRunner), depuis le dossier courant du job, et affiche sa sortie.
# Les fichiers produits dans ce dossier sont visibles du job à la fin.
#
# REND LE CODE DE SORTIE DE LA COMMANDE (bug hunt T4) : le relais écrit
# « rc=N » en dernière ligne de gui-log.txt, on le relit. Avant, gui_run
# rendait le rc de `cat` (donc 0 quand la commande avait échoué) et 0 sur
# timeout : exactement l'inverse de ce qu'il faut. Conventions :
#   124 = timeout (comme timeout(1)), 125 = relais muet (pas de ligne rc=).
# Un appelant qui met gui_run dans un TUBE (`gui_run … | sed …`) reperd le
# code : capturer d'abord, filtrer ensuite.
gui_run() {
  _cmd=$1; _to=${2:-240}
  _d=/tmp/pomppc-gui/job-$$-$(date +%s)
  mkdir -p "$_d"
  printf '#!/bin/sh\ncd "%s"\n%s\n' "$PWD" "$_cmd" > "$_d/gui.sh"
  chmod -R 777 "$_d" "$PWD"
  touch "$_d/ready"
  _i=0
  while [ ! -f "$_d/done" ] && [ $_i -lt $_to ]; do sleep 1; _i=$((_i+1)); done
  if [ -f "$_d/done" ]; then
    cat "$_d/gui-log.txt"
    _rc=$(sed -n 's/^rc=\([0-9][0-9]*\)$/\1/p' "$_d/gui-log.txt" | tail -1)
    [ -n "$_rc" ] || _rc=125
    return $_rc
  fi
  echo "gui_run : TIMEOUT ($_to s) ; relais : $(tail -2 /tmp/pomppc-gui-runner.log 2>/dev/null)"
  return 124
}
