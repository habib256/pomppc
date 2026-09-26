#!/bin/sh
# lance.command — lanceur unique de la matrice de jeux, dans l'invité
# (copié en ~/matrice/lance.command par tools/matrice/matrice.py).
#
# `open ~/matrice/lance.command` passe par Terminal, donc par la session
# graphique de l'utilisateur ; mais `env VAR=… open …` ne transmet rien quand
# Terminal tourne déjà. Tout vient donc d'un fichier : ~/matrice/cellule.sh,
# écrit par l'hôte avant chaque lancement (variables POMPPC_GL_*, dossier D,
# commande du jeu dans la fonction `jeu`). Le lanceur note le début, l'état de
# sortie et la fin dans $D/log.txt ; l'hôte attend « exit » pour conclure.
C=/Users/tiger/matrice/cellule.sh
[ -f "$C" ] || { echo "pas de $C"; exit 1; }
. "$C"
mkdir -p "$D" ${POMPPC_GL_DUMP:+"$POMPPC_GL_DUMP"}
echo "start $(date '+%Y-%m-%d %H:%M:%S')" >> "$D/log.txt"
env | grep '^POMPPC' >> "$D/log.txt"
jeu > "$D/stdout.txt" 2>&1 < /dev/null
echo "exit $? $(date '+%Y-%m-%d %H:%M:%S')" >> "$D/log.txt"
