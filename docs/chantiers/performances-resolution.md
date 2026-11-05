# Performances selon la résolution

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Actif sur le M4 ; à reprendre sur le PC ensuite.

**05/11 : A/B ImGuiDock contre natif fait sur le M4 ; le frontal n'explique pas de gros ralentissement.**
[A] Bureau 1920×1080×32 (le pire cas pour le frontal), Terminal, hôte au repos, trois parties par
bras, frontal contrôlé à chaque partie (`bench/tcg/ab/m4-frontal-0511-{fen,pe}`), ms/image, médianes :

| jeu | natif | ImGuiDock | écart | étendues |
|---|---:|---:|---:|---|
| zen fenêtre | 3,488 | 3,244 | **−7,00 %** | disjointes |
| cmr en course | 40,614 | 39,379 | −3,04 % | recouvertes |
| mb fenêtre | 6,718 | 6,568 | −2,23 % | recouvertes |
| ut fenêtre | 20,098 | 20,608 | +2,54 % | disjointes |
| d3 fenêtre | 36,638 | 37,720 | +2,95 % | disjointes |

Le critère de fermeture (écart < 2 % sur la matrice) n'est pas atteint au sens strict, mais il n'y a
pas de perte importante : cmr en 1920×1080 tourne au moins aussi vite sous ImGuiDock. Les petits
coûts d3 et ut (environ 1 et 0,5 ms) sont sous les 3 % : trois parties ne les prouvent pas selon la
règle du dépôt, même avec des étendues disjointes. [F] La cause des gains de zen et mb n'est pas
cherchée ; tranché par un profil du frontal natif, qui n'est pas celui de l'utilisateur. Les
fenêtres de cmr ne commencent pas au même numéro d'image selon le bras (~30 000 contre ~39 500) :
même nombre d'images, mais pas prouvé à scène égale. Les « gros ralentissements » du 27/10 ne sont
pas reproduits sur le M4 ; le PC reste à mesurer.

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
