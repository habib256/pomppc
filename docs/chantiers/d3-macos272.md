# DOOM 3 sur le M4 : 29 ms sans ombres, 37 ms avec

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

**Clos le 04/11/2026.** Le passage de `r_shadows=0` à `1` explique les 29 → 37 ms.
La référence courante est **37 ms/image avec ombres activées**. Aucun ralentissement de macOS
27.2 n'est démontré par cette comparaison ; les préférences du disque quotidien sont conservées.

DOOM 3 en fenêtre mesurait **36,6 à 37,8 ms/image** le 03/11, contre **28,9** le 02/11 au matin
(`m4-pollns-0211`, `m4-progressif-0211`), soit environ +27 %. UT2004 et Zenerchi n'ont pas bougé ; Marble Blast
non plus écran allumé (6,1 à 6,3 ms). macOS 27.2 a été installé le 03/11 à 12 h 48, avant toutes ces mesures.

**Profil à l'origine du soupçon** (`bench/tcg/ab/m4-os272-0311/actuel`, comparé à `m4-profils-0111n-fen/ref-1`) :
`CPU 0/TCG` occupé 34,5 % contre 67,2 %, `CPU 1/TCG` 69,6 % contre 83,4 %, `qgpu-render` 23,2 % contre 17,8 %
(`do_draw_native` 4,3 % contre 2,2 %). Ces profils comparaient aussi deux qualités de rendu différentes :
ils ne permettaient pas d'attribuer cet écart à macOS. Le 04/11, les réglages du disque témoin
et les profils invités ont révélé la différence manquante : **`r_shadows=0` puis `1`**.

## Essais du 03/11 (`bench/tcg/ab/m4-os272-0311/`, une partie sauf mention)

| Essai | DOOM 3, ms/image |
|---|---|
| Témoin | 37,26 |
| Kext et plugin d'avant le 02/11 (3 parties, écran éteint) | médiane 36,88, contre 37,05 |
| `NOSOUND=1` | 37,80 |
| `POLLNS=3` | 37,20 |
| `VCPUQOS=1` | 37,17 |
| Binaire du 26/10 (`~/src/qemu-arm64`), `QE=0` | 37,21 |
| Réglages de jeu d'avant (`D3_SET`, depuis `DoomConfig.cfg.avant-nettoyage`) | 36,72, contre 37,31 |

L'essai « réglages d'avant » ne testait pas le vrai état antérieur : la sauvegarde utilisée ne
différait pas sur `r_shadows`. Il **n'écartait donc pas les réglages du jeu**, contrairement au
bilan initial. Le nom de cette fiche garde la piste suivie le 03/11 ; elle est corrigée ci-dessous.

## Diagnostic du 04/11 : comparaison des disques, puis d'un seul réglage

[A] Mesuré sur ce M4 sous **macOS 27.2, 26B123**, écran allumé, un seul QEMU, barrière de repos
avant chaque partie et charge relevée toutes les cinq secondes. Frontal Cocoa lancé depuis le
Terminal, SMP=2, 768 Mo, QE et son allumés, bureau **1024×768×32**, jeu en fenêtre 640×480.
Les profils et captures sont pris **après** la fenêtre de vitesse. Une partie par configuration :
il s'agit d'identifier un écart de réglage d'environ 27 %, pas de prouver un petit gain d'optimisation.

Deux copies APFS isolées, sans remplacer le disque quotidien :
`disks/tiger.qcow2` du 03/11 et le témoin immuable
`disks/tiger-endurance-m4-ombre-0111n.qcow2` du **01/11 à 22 h 53**. Le noyau a le même MD5
`def99c6ef0231ea3c2a627dd26201371` dans les deux. Le témoin porte `r_shadows=0`, le disque actuel
`r_shadows=1` ; `image_anisotropy` et `com_machineSpec` diffèrent aussi. Le deuxième essai isole
donc **seulement `r_shadows` sur la copie actuelle**, par `D3_SET=r_shadows=0` puis `=1` ;
la matrice rend le fichier de préférences après chaque partie.

| Essai | Disque / réglage | QEMU | ms/image |
|---|---|---|---:|
| `m4-d3-terminal-0411/actuel-1` | actuel, ombres activées | paquet 0.4.1 | 37,349 |
| `m4-d3-terminal-0411/avant-1` | témoin du 01/11, ombres désactivées | paquet 0.4.1 | 29,361 |
| `m4-d3-ombres-0411/sans-1` | actuel, seul `r_shadows=0` | reconstruit du 04/11 | 29,221 |
| `m4-d3-ombres-0411/avec-1` | actuel, seul `r_shadows=1` | même binaire | 37,039 |

**Zéro repli dans les quatre parties.** Ce sont des tours de vitesse sans vidage : ils ne
constituent pas une nouvelle matrice d'image verte. Les deux captures `scene.png` du dernier
essai ont été regardées : même scène, ombres visibles avec `=1`. Elles sont prises après les
profils, et le compteur FPS affiché sur ces captures n'est pas la mesure de vitesse.

Les profils invités du témoin ancien et du bras sans ombres n'ont pas de
`RB_StencilShadowPass` ; celui du disque actuel avec ombres en a une. Le profil du 02/11
à 28,9 ms n'en avait pas non plus. **Les 29 ms sont retrouvés sous macOS 27.2, en 32 bits,
avec le plugin actuel et le QEMU reconstruit** : ni le système hôte ni la profondeur du bureau
ne sont nécessaires pour expliquer l'écart.

QEMU reconstruit sur `fd78c51` : SHA-256 `d6e6e3e2caf092f66…` ; QEMU du paquet local 0.4.1 :
`403546c583f5413e…`. Sources, empreintes complètes, configurations et pilotage dans
`bench/tcg/ab/m4-d3-disque-0411/{provenance.json,pilotage/}` ; résultats des deux comparaisons
dans `bench/tcg/ab/m4-d3-{terminal,ombres}-0411/`. Ce sont des preuves locales au M4.

## Pourquoi les ombres coûtent 7,82 ms de plus

[A] Les profils du dernier essai localisent un travail supplémentaire des deux côtés.
`threadbusy.py`, blocs d'un même fil fusionnés, donne ces estimations d'activité par image :

| Poste | Sans ombres | Avec ombres | Écart |
|---|---:|---:|---:|
| Somme des deux vCPU | 30,64 ms | 38,56 ms | +7,92 ms |
| Fil `qgpu-render` | 5,35 ms | 8,46 ms | +3,10 ms |

Calcul : fraction d'échantillons hors attentes connues × temps d'image mesuré avant le profil.
**Ces coûts se chevauchent : ne pas les additionner.** Ce n'est ni un chronométrage GPU, ni une
décomposition prouvée du chemin critique ; certains appels de pilote peuvent eux-mêmes attendre.
L'activité des vCPU en proportion du temps reste voisine de 1,05 cœur, mais l'image dure plus longtemps.

Dans le profil invité avec ombres, `RB_StencilShadowPass` apparaît dans **114/863 échantillons
du fil principal (13,2 %)** ; `R_CreateShadowVolume` dans 32/863 pour son appel principal (3,7 %).
Sous la passe stencil, **65/114** passent par `geom_draw_client`, dont 23 par `geom_draw_native` :
scan des indices, copies, états, synchronisation et envoi de géométrie sont visibles. Ces nombres
sont **inclusifs**, pas des coûts propres à additionner. Les deux fonctions d'ombres sont absentes
du profil sans ombres.

Les compteurs `VIDAGE image 6500` (images 6001..6500 du niveau) donnent aussi **1 → 2 vidages
par image** et environ **39,5 → 107,1 milliers de mots de commandes par image**. Avec ombres,
le premier envoi se fait vers 65 470 mots, près des 65 536 disponibles ; les appels de `flush`
correspondants dans le plugin testent la place restante dans `CMD_WORDS`. L'attente des barrières
passe de 0,019 à 0,750 ms/image sur ce bloc. Cela identifie un effet du tampon plein, mais ne
prouve pas qu'agrandir le tampon récupérerait tout le coût, ni que ces 0,73 ms expliquent les 7,82 ms.

La piste à instruire est donc le chemin géométrique commun du plugin, avec les ombres conservées.
Rien ne prouve encore quelle part de ces 7,82 ms serait récupérable. Suite dans
[`alleger-plugin.md`](alleger-plugin.md), avec équivalence d'image et A/B des cinq jeux.

## Deux pièges du banc rencontrés pendant cette reprise

- **Lancement depuis le service des outils** : QEMU et le service apparaissaient à `PRI=4`,
  sans les indicateurs Darwin de processus en arrière-plan ou supprimé. Cinématique très lente
  avec le QEMU reconstruit **et** celui du paquet. Depuis le Terminal, QEMU est reconnu comme
  application et la progression normale revient, avec le même disque et le même paquet.
  Les tentatives `m4-d3-disque-0411/actuel-1` et `m4-d3-paquet-0411/actuel-1` ont été interrompues,
  traces conservées, invité arrêté proprement ; **aucun temps de niveau n'en est retenu**.
  [F] Le mécanisme précis de cette politique héritée n'est pas établi : ne pas l'appeler App Nap
  sans preuve. Pour reprendre ces mesures, lancer le banc depuis le Terminal.
- **Profil du mauvais PID avec un disque explicite** : `pgrep -f` peut choisir le shell parent,
  dont la ligne reprend les arguments du bras. Les profils hôte de `m4-d3-terminal-0411` sont
  ceux de bash : écartés. Pour `m4-d3-ombres-0411`, attente fixe du bureau et
  `POMPPC_PERF_CMD` choisissant l'unique `pgrep -x qemu-system-ppc` ; PID et vrai nom du
  processus conservés dans chaque profil. Correction générale à faire dans
  [`metrologie.md`](metrologie.md). Les profils invités restent utilisables.
