# Quartz Extreme : finir la preuve des résolutions

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Actif sur les deux hôtes. **M4 : fait le 03/11 ; reste le PC.**

## Bilan du M4, 03/11 (plugin `20261102-modes`, macOS 27.2)

- **Matrice de preuve : 16 cellules automatisées vertes sur 16**, images justes, 0 repli, bureau de la session
  en 1024×768×32 par `guest/gltest/setmode.c`, sans toucher au réglage du disque. Tours
  `bench/matrice/qe-res-0311-{fen,fen2,pe}`. Neuf références refaites et regardées (`bench/` perdu le 01/11) ;
  `nx-fen` retrouve l'empreinte exacte de sa référence du 27/09.
- **Colin McRae depuis quatre bureaux** (1920×1080×32, 1920×1080×16, 2560×1080×32, 1024×768×32) : vert, 41,6 à
  43,1 ms/image, `bench/matrice/qe-res-0311-cmr-*`. **Limite :** le jeu passe lui-même en 1920×1080 dans les
  quatre cas : c'est le bureau de départ qui varie, pas la définition du rendu.
- **A/B kext + plugin du 02/11 contre ceux d'avant** (`bench/tcg/ab/m4-plugin-0311-fen`, 3 parties, écran du Mac
  éteint dans les deux bras) : d3 +0,45 %, ut −0,59 %, zen +2,00 %, sous le seuil de 3 % ; **mb +7,40 %**, plages
  disjointes. Repris écran rallumé avant chaque mesure (`m4-plugin-0311-mb-allume`, 3 parties) : **mb −2,65 %**, sous le
  seuil, charge de l'hôte élevée (jusqu'à 7,9 hors mesure) : ce n'est pas un gain. Le plugin ne perd sur aucun des
  quatre jeux joués ; le +7,40 % tenait à l'écran éteint [F : mécanisme non établi].
- Le disque quotidien est en **1024×768×16** depuis le 02/11 à 18 h 51 (et non 1920×1080×16 comme écrit la veille).

- [ ] **[QE — les deux hôtes] Résolutions : finir la preuve** (corrigé le 02/11,
  `docs/quartz-extreme.md`, journal). DOOM 3, UT2004 et Marble Blast passent depuis un bureau en 1920×1080 ou
  2560×1080 (paquet 0.4). Restent : Colin McRae ; matrice de preuve et A/B du plugin `20261102-modes` sur un disque au réglage des
  références ; balayage `guest/gltest/setmode.c` sur le PC (EGL/NVIDIA). **Fermeture :** ces
  essais justes, ou leur écart écrit.
