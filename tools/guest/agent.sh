#!/bin/sh
# agent.sh — agent de la boucle de développement, DANS l'invité Tiger.
#
#   sh /pomppc/agent.sh <secteur_inbox> <secteur_outbox>
#
# Deux fichiers préalloués et contigus du disque (/pomppc/mbx-in.bin et
# /pomppc/mbx-out.bin) servent de boîtes aux lettres.
#  - inbox : lue par le périphérique BRUT /dev/rdisk0 (secteurs absolus), jamais
#    par le système de fichiers, dont le cache ne verrait pas ce que l'hôte vient
#    d'écrire dans l'image ;
#  - outbox : écrite À TRAVERS le fichier (dd conv=notrunc, puis sync) : Tiger
#    refuse toute écriture brute sur un disque dont une partition est montée
#    (« Resource busy », vu en vrai). HFS+ ne déplace pas les blocs d'un fichier
#    réécrit en place, l'hôte les relit donc au même endroit de l'image.
#
# Protocole (tools/guest/devloop.py côté hôte) :
#   inbox  : secteur 0 = "JOB <id> <nsecteurs>\n", puis un tar contenant job.sh
#   outbox : secteur 0 = "OUT <id> <nsecteurs>\n", puis un tar du dossier out/
# L'agent écrit les données AVANT l'en-tête : l'hôte ne voit jamais un résultat
# incomplet.
IN=$1; OUT=$2
[ -n "$IN" ] && [ -n "$OUT" ] || { echo "usage: agent.sh IN OUT"; exit 1; }
# Les fichiers posés depuis l'hôte (hdiutil, propriétaires ignorés) ne sont
# pas à root : SystemStarter refuse alors le StartupItem. On corrige ici, en
# single-user, avant le premier démarrage du bureau.
if [ "$(id -u)" = 0 ]; then
  for d in /Library/StartupItems/POMPPCAgent /Applications/POMPPCGuiRunner.app; do
    [ -e "$d" ] && chown -R root:wheel "$d" && chmod -R 755 "$d"
  done
  chown root:wheel /pomppc/agent.sh /pomppc/agent.conf 2>/dev/null
  u=$(stat -f %u /Users/tiger 2>/dev/null)
  [ -n "$u" ] && chown "$u" /Users/tiger/Library/Preferences/loginwindow.plist 2>/dev/null
fi
# Un job resté dans la boîte d'une session précédente (fait, ou interrompu)
# n'est PAS rejoué : vu en vrai, l'agent relancé reprenait un job bloquant.
# devloop.py n'écrit un job qu'une fois l'agent prêt, rien ne se perd.
hdr=$(dd if=/dev/rdisk0 bs=512 skip=$IN count=1 2>/dev/null | tr -d '\000' | head -1)
set -- $hdr
last=""; [ "$1" = JOB ] && last=$2
echo "agent: prêt (inbox $IN, outbox $OUT)"
while :; do
  hdr=$(dd if=/dev/rdisk0 bs=512 skip=$IN count=1 2>/dev/null | tr -d '\000' | head -1)
  set -- $hdr
  if [ "$1" = JOB ] && [ "$2" != "$last" ]; then
    id=$2; n=$3
    echo "agent: job $id ($n secteurs)"
    rm -rf /tmp/job; mkdir -p /tmp/job/out; cd /tmp/job
    dd if=/dev/rdisk0 bs=512 skip=$((IN + 1)) count=$n 2>/dev/null | tar xf - 2>/tmp/job/out/untar.txt
    start=$(date +%s)
    sh ./job.sh > /tmp/job/out/log.txt 2>&1
    rc=$?
    echo "rc=$rc duree=$(( $(date +%s) - start ))s" >> /tmp/job/out/log.txt
    cd /tmp/job && tar cf /tmp/out.tar out
    size=$(ls -l /tmp/out.tar | awk '{print $5}')
    m=$(( (size + 511) / 512 ))
    dd if=/tmp/out.tar of=/pomppc/mbx-out.bin bs=512 seek=1 conv=sync,notrunc 2>/dev/null
    sync
    printf "OUT %s %s\n" "$id" "$m" | dd of=/pomppc/mbx-out.bin bs=512 seek=0 conv=sync,notrunc 2>/dev/null
    sync
    echo "agent: job $id terminé rc=$rc"
    last=$id
    cd /
  fi
  sleep 2
done
