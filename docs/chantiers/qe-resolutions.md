# Quartz Extreme : finir la preuve des résolutions

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

**Clos le 04/11/2026 : M4 fait le 03/11, PC fait le 03-04/11, écarts écrits ci-dessous.**

## Bilan du PC, 03-04/11 (plugin `20261102-modes`, EGL/NVIDIA, RTX 4060 Ti)

- **Balayage `guest/gltest/setmode.c`** : 66 changements (11 définitions × 3 profondeurs, montée et descente),
  0 écart, planches regardées (`bench/qe-modes-0311-pc/`). `glwin` en fenêtres de 2400 px : juste en 32 et 16 bits.
  **Écart :** les fenêtres de 400 px n'ont que le verdict `OK (0 échec)` de `glwin`, leur capture n'a pas montré
  la fenêtre.
- **Matrice de preuve** (`bench/matrice/preuve-pc-0311`) : **8 cellules automatisées vertes sur 8**, 0 repli ;
  mb-fen 12,4 ; mb-pe 11,8 ; zen-fen 4,8 ; d3-fen 84,1 ; d3-pe 77,9 ; ut-fen 37,1 ; ut-pe 36,6 ; cmr-pe 84,5 ms/image.
  Temps pris hors repos strict : ils ne remplacent pas ceux de l'A/B.
- **Colin McRae depuis quatre bureaux** (`qe-res-0311-cmr-*`), vert, 0,00 % de la référence : 1920×1080×32 83,6 ;
  1920×1080×16 82,6 ; 2560×1080×32 94,5 ; 1024×768×32 87,5 ms/image. Partie unique par bureau : l'écart du
  2560×1080 n'est pas établi. Même limite qu'au M4 : c'est le bureau de départ qui varie.
- **A/B kext et plugin du 02/11 contre ceux d'avant** (`bench/tcg/ab/plugin-0311-*`, 3 parties par bras, `ap` =
  neuf, `av` = ancien), ms/image, médianes :

  | jeu | `ap` | `av` | lecture |
  |---|---:|---:|---|
  | d3-fen | 77,67 | 77,83 | neutre |
  | zen-fen | 5,00 | 5,01 | neutre |
  | ut-fen | 37,37 | 36,43 | neuf plus lent de 2,6 %, sous le seuil |
  | cmr-pe | 77,75 | 76,23 | neuf plus lent de 2,0 %, sous le seuil, plages recouvertes |
  | mb-fen | 8,61 | 9,80 | neuf plus rapide, plages recouvertes, dispersion 17 %, charge jusqu'à 2,82 : **pas un gain** |

  Le plugin ne perd de façon prouvée sur aucun des cinq jeux. Tours sans vidage : leur verdict `rouge` est le
  motif `--sans-vidage`, ils mesurent vitesse et replis ; la justesse vient de la matrice.

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
