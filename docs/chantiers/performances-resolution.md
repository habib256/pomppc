# Performances selon la résolution

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Actif sur le M4 ; à reprendre sur le PC ensuite.

**03/11 : A/B ImGuiDock contre natif à refaire, aucun chiffre valable.** Les parties de
`bench/tcg/ab/m4-frontal-0311-fen.sans-frontal` ont tourné **sans frontal** : `pomppc` plantait au lancement, écran
du Mac éteint (corrigé, `9f7788b`). Pour la reprise : `POMPPC_FRONTEND=imgui` dans un bras, vérifier
`pgrep -x pomppc` pendant la partie, `MATAB_AVANT` doit fermer le frontal, et l'écran doit être **allumé** :
`caffeinate -d` empêche la veille sans réveiller, il faut d'abord `caffeinate -u -t 3`.

- [ ] **[Jeux] Performances selon la résolution** — gros ralentissements signalés par
  l'utilisateur dans **Colin McRae en 1920 × 1080** (27/10 ; sur le M4 la cellule y est à +11 %
  contre 1024×768). Étendre les essais aux autres résolutions, jeux et modes ; relever les temps
  d'image et les replis sur une même scène. Les cellules vertes aux résolutions de la matrice ne
  valent pas pour toutes. Premier suspect : le frontal ImGuiDock (piste ci-dessous), dont le
  coût par image croît avec la résolution ; dans les captures du 31/10
  (`bench/tcg/ab/cmr-res-3110/`), la 1920×1080×16 regardée le 01/11 est juste ; les autres
  restent à regarder.

## Piste proposée le 01/11 (à arbitrer par l'utilisateur)

1. **Mesurer ce que l'utilisateur voit : ImGuiDock contre le frontal natif.** Tous les A/B passent
   sous le frontal natif de QEMU (`matab.sh` : GTK sur le PC, Cocoa sur le M4) ; l'utilisateur
   joue sous ImGuiDock (`-display dbus,p2p=on`, images
   poussées par D-Bus puis texture, `frontend/README.md`). Ce coût n'a jamais été mesuré et il
   croît avec la résolution (3 Mo par image en 1024×768×32, 8 Mo en 1920×1080) : c'est un
   suspect des « gros ralentissements en 1920×1080 » de cmr. Travail : A/B des cinq cellules
   sous les deux frontaux à scène égale ; si écart, scanout en mémoire partagée (memfd ou
   dmabuf, prévus par `org.qemu.Display1`) et rectangles sales seulement. **Fermeture :** écart
   ImGuiDock − frontal natif < 2 % sur la matrice, cmr 1920×1080 remesuré. Coût : une journée.
   Gain :
   tout l'écart mesuré, sur les deux hôtes, dans l'usage réel.
