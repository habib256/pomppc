# guilib.sh — à sourcer dans un job.sh de devloop en mode bureau.
#
#   . ./guilib.sh
#   gui_run 'commande shell' [délai_s]
#
# Exécute la commande DANS la session graphique de l'utilisateur de test (via
# POMPPCGuiRunner), depuis le dossier courant du job, et affiche sa sortie.
# Les fichiers produits dans ce dossier sont visibles du job à la fin.
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
  else
    echo "gui_run : TIMEOUT ($_to s) ; relais : $(tail -2 /tmp/pomppc-gui-runner.log 2>/dev/null)"
  fi
}
