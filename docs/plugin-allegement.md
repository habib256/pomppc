# Alléger le plugin pour tous les jeux (22/10/2026)

Après l'arrêt du second niveau du traducteur (`docs/plan-traducteur-rapide.md` §10.8), le
levier suivant est le travail que le plugin GL (`guest/gldriver/pomppc_accel.c`) fait sur le
fil du jeu, dans le G4 émulé. Consigne de l'utilisateur : **ce qui est fait doit servir à toute
l'émulation, pas seulement à DOOM 3**. Le plugin est du code invité PowerPC : un gain y vaut
sur les deux hôtes (PC et M4) sans travail de parité.

**Statut (22/10) : leviers A et B construits, A/B fait sur la VM quotidienne, six leviers
allumés par défaut dans le plugin `20261022-abon` (PRESENTQ reste éteint) : §6.**

**23/10 : levier E (verdict par unité élargi, `POMPPC_GL_UVWIDE`) construit et prouvé sur la
VM de dev, éteint par défaut, A/B des quatre jeux à jouer : §7.**

## 1. Profil des quatre jeux de la matrice du PC

`sample` de 20 s du jeu dans l'invité, VM quotidienne, binaire rapide, plugin `20261021-d3p`
(leviers du §10 de `d3-plugin-x86.md` allumés), une partie en fenêtre par jeu
(`bench/tcg/ab/gprof-2210-{mb,zen,ut,d3}`). Parts du fil principal du jeu ; « pile GL » = tout
ce qui est sous un point d'entrée `gl*`/`gle*`/`gld*`/`CGL*` appelé par le jeu, réparti par
propriétaire du code qui s'exécute (fonctions définies dans `guest/gldriver/*.c` = plugin).

| jeu (ms/image) | plugin | `libSystem` sous GL | GLEngine/libGL | attente sous GL | hors GL |
|---|---|---|---|---|---|
| Marble Blast (13,1) | **23,5 %** | 5,6 % | 11,8 % | 10,3 % | 48,8 % |
| Zen (5,3) | **15,1 %** | 4,5 % | 14,8 % | 3,7 % | 62,0 % (dont 33,6 % d'attente) |
| UT2004 (42,6) | **18,9 %** | 5,6 % | 12,3 % | 2,3 % | 60,9 % |
| DOOM 3 (93,9) | **22,5 %** | 5,6 % | 7,3 % | 1,3 % | 63,3 % |

Le plugin et ce qu'il appelle dans `libSystem` (`memcpy`, `memcmp`, verrous) font **20 à 29 %
du fil du jeu dans les quatre**. Inclusif, par chemin :

| chemin (inclusif) | mb | zen | ut | d3 |
|---|---|---|---|---|
| `gldUpdateDispatch` (verdict au dispatch) | 4,0 | 7,9 | 9,1 | 10,4 |
| `geom_begin` (mode immédiat Begin/End) | **21,9** | 6,3 | 0 | 0 |
| `geom_draw_client` (tableaux, `DRAW_NATIVE`) | 0 | 0 | 11,4 | 16,4 |
| `pomppc_proc_pre` (échange : présentation, bilan) | 6,9 | 9,0 | 2,9 | 1,4 |
| `CGLFlushDrawable` (échange, inclut le précédent) | 8,5 | 10,1 | 3,4 | 1,6 |

Ce qui ressort, hors des postes déjà travaillés pour DOOM 3 (dispatch, `DRAW_NATIVE`) :

1. **Le chemin Begin/End n'a jamais été allégé.** Dans Marble Blast, `geom_begin` refait à
   chaque lot tout le verdict : `texture_ok` 5,3 % (dont `texture_unit_ok` 3,7 %), `geom_ok`
   4,4 %, `geom_send_all` 3,5 %, `send_state` 2,3 %, `geom_format` 2,2 %. Il y a ~34 Begin et
   ~37 dispatches par image, soit ~37 µs de verdict par Begin. Le mode immédiat est le chemin de
   la plupart des jeux de l'époque de Tiger.
2. **L'échange de chaque image** coûte cher aux jeux rapides. `present_direct` fait 5,1 % sur
   mb et 5,4 % sur zen. Toutes les 10 images, `direct_window_ok` interroge le WindowServer par
   Mach (`CGSGetWindowBounds` 2,8 %, `CGSGetSurfaceBounds` 0,7 %, liste des fenêtres). À chaque
   image, il y a aussi `wait_half` (attente de la moitié précédente, 1,8 % sur zen), puis
   `CGSMainDisplayID`, `GetFrontProcess` et `drawable_hidden`. Enfin, `stats_frame` fait des
   `fprintf` (0,6-0,9 %), sous les variables de trace de la matrice.
3. **Attente de l'hôte** : Marble Blast passe 10,3 % de son fil dans `mach_msg_trap` sous GL.
   La note (`VIDAGE`, `ZONES`) montre ~1,6 vidages et barrières attendues par image, pour
   0,5-0,75 ms/image.
4. **Verrous et `libSystem`** : `__spin_lock`/`unlock`, `pthread_mutex_*`, `memcmp` et
   `__memcpy` font environ 5 % dans tous les jeux.

## 2. Leviers

Méthode du §10 de `d3-plugin-x86.md` : chaque levier derrière une variable `POMPPC_GL_*`
éteinte d'abord, un mode `=2` de contrôle (refait la voie complète et compte les écarts), et
les preuves `plugmd5` (66 scènes à l'octet), job `gpu` et compteurs de contrôle sur la VM de
dev. Ensuite un A/B sur la VM quotidienne pour les **quatre** jeux, pas seulement DOOM 3. Un
levier n'est allumé que s'il ne perd sur aucun jeu. Leçon du §10.6 : le compte à l'instruction
d'une scène ne prédit le jeu qu'au-delà de ~1 000 instructions par dessin ; viser des postes
visibles dans ces profils.

- **A. Chemin Begin/End** : reprendre au Begin ce que le dispatch a déjà décidé
  (verdict, unités, format, programmes, état, matrices) quand les blocs reçus depuis le dernier
  Begin n'y touchent pas, sur le modèle de DISPFAST, STUNIT et MTXDIRTY au dispatch. Mesure :
  Marble Blast, Zen. **Construit le 22/10 : §3 ; allumé par défaut : §6.**
- **B. Échange de l'image** : vérifications de fenêtre moins chères (ou déclenchées par un
  changement), attente de la moitié précédente, coût fixe par image de `stats_frame` et
  `pomppc_proc_pre`. Mesure : Zen, Marble Blast. **Construit le 22/10 : §4 ; allumé par
  défaut (sauf PRESENTQ) : §6.**
- Plus tard : attente de l'hôte (C), verrous (D). Au profil du 22-23/10 aux six leviers,
  ils sont devenus petits (≤ 3 % et ~1,5 %).
- **E. Verdict par unité élargi** (23/10) : le verdict complet refait au dispatch est le
  poste commun le plus lourd (`geom_dispatch_locked` 5 à 10 % du fil du jeu). **Construit :
  §7.**

## 3. Levier A : le chemin Begin/End (22/10, révision `20261022-beg`)

Branche `worktree-agent-ad42beca86c281438`. Deux variables, éteintes par défaut
(`BEGINVD_DEFAULT 0`, `LIGHTDIRTY_DEFAULT 0`), un mode `=2` de contrôle chacune, compteurs
sur la ligne `BEG` de la note (toutes les 500 images et à la destruction du contexte).
Aucun changement de `qgpu_proto.h` ni de `qgpu_abi.h` : ni QEMU ni kext à reconstruire.

### 3.1 Ce que faisait `geom_begin`

À chaque `glBegin` (et à chaque `glDrawElements` que GLEngine déroule en mode immédiat :
c'est par là que passent les `TSMesh` de Marble Blast, `gleDrawArraysOrElements_IMM_Exec` →
`gleBeginPrimitiveTCLFunc` → `geom_begin`), le verdict entier était refait : `geom_ok`,
`texture_ok`, `geom_format`. Hors de la mémoire des unités, en plus (`us_open` n'est
ouvert qu'au dispatch) : `texturing_on`, `unit_drvtex`, `find_tex` et `upload_texture`
pour chaque unité, à chaque appel. Or le `gldUpdateDispatch` qui précède presque chaque
`glBegin` vient de calculer ce verdict (ou de le reprendre : liste blanche, verdict par
unité) et de le ranger dans le `PCtx` avec sa clé (`vd_store`, lot 2) — la reprise que
`geom_draw_client` fait depuis le lot 2, jamais faite au Begin.

### 3.2 Les leviers

| levier | variable | ce qu'il fait |
|---|---|---|
| verdict repris | `POMPPC_GL_BEGINVD=1` (2 = contrôle) | au Begin, si un verdict « ok » est rangé et que sa clé (`vd_key_of` : image, époque des textures, état du plugin et du contexte dont `geom_lost`, VAO et masques, table des unités, drawable, étage de sommets, programmes) n'a pas bougé : ni `geom_ok`, ni `texture_ok`, ni `geom_format` — unités et format du verdict rangé. Clé changée ou verdict « non » : la voie complète, refus et `geom_lost` compris. Plus deux allègements exacts : les unités coupées du verdict repris remises à zéro une fois par verdict rangé (`bv_norm` : leurs autres champs ne sont pas définis, seuls `tu_same`/`memcmp` de `send_state_blk` les lisaient et refaisaient pour rien des séries d'unités — `mbimm` : 82 séries de 372 unités → 55 de 73) ; `sync_to_host` appelé seulement si la couleur ou la profondeur sont plus récentes chez l'invité (sinon il ne fait que lire l'heure deux fois sous `POMPPC_GL_STATS` : 3 % de `geom_begin` dans le `sample` de Marble Blast) |
| lumières | `POMPPC_GL_LIGHTDIRTY=1` (2 = contrôle) | `geom_send_lights` ne compare les 0x60 octets d'une lumière que si un dispatch a signalé, depuis sa dernière comparaison, un bit de cette lumière dans `+0x0c` (`4 << 2i` paramètres, `8 << 2i` allumage : relevé complété par la scène `litdirty`, `docs/re/bloc-changements-r4.md`) ; tout comparer après `invalidate_mirrors_from` et avant le premier envoi. Les matériaux (que `GL_COLOR_MATERIAL` réécrit au `glColor`, sans dispatch), le masque des lumières et le modèle d'éclairage restent comparés à chaque fois |

Pourquoi `BEGINVD` est exact : le verdict ne lit que l'état que GLEngine signale par un
dispatch avant tout dessin qui le suit (et ce dispatch refait ou reprend le verdict rangé
selon son bloc), plus ce que porte la clé. Un `glTexImage` ou un `glTexSubImage` entre deux
`glBegin` (l'atlas de police WC3, P11) fait avancer `vd_epoch` (et pose le bit d'unité au
dispatch suivant) : la clé change, la voie complète est refaite. Le contrôle `=2` refait
toujours la voie complète (c'est elle qui sert) et compare ce que la reprise aurait donné
(ok, format, unités : texture, environnement, combinaison ; et le recalcul ne doit toucher
aucune texture).

Premier essai de `LIGHTDIRTY` sur le seul bit `0x4` (celui du relevé R4, lumière 0) : la
scène `litdirty` l'a pris en faute — deux écarts au contrôle et une image différente
(`glCallList` qui change la lumière 1 : `+0c=00000030` ; `glLightf` sur la lumière 1 :
`+0c=00000010`). D'où le relevé par lumière, vérifié sur les lumières 0, 1, 2 et 7.

### 3.3 Mesure (greffon TCG `hotblocks`, VM de dev, `tiger-dev-plugin.raw`, RTX)

Nouvelle scène **`mbimm`** (`gltest`) : une image du Torque Game Engine en mode immédiat
réduite — ciel, 16 + 4 surfaces d'intérieur (unité 0 changée à chaque surface, carte de
lumière en unité 1, deux surfaces de suite sans changement d'état sur quatre), bille éclairée
(six rubans consécutifs, une matrice, lumière reposée une image sur deux), ombre
translucide, interface en projection orthogonale (atlas de police réécrit par
`glTexSubImage2D` entre deux lignes, et à la première image une texture liée sans image,
dessinée, puis définie et redessinée : P11). 32 `glBegin` et ~27 dispatches par image.
Instructions comptées par différence de deux passes (`GLTEST_MBFR=2` puis `12` : 320
`glBegin`), dispatches compris, donc « par `glBegin` » au sens de l'image entière :

| scène, par `glBegin` (ou par dessin) | plugin | `memcmp` (libSystem) |
|---|---|---|
| `mbimm`, variables éteintes | 12 992 | 423 (4,9 appels) |
| `mbimm`, `BEGINVD=1` | 7 522 | 423 |
| `mbimm`, `LIGHTDIRTY=1` | 12 978 | 185 (3,4 appels) |
| `mbimm`, les deux | **7 509 (−42 %)** | **185** |
| `game` (60 `glBegin` par passe, `glFinish` à chaque image), éteintes → les deux | 33 514 → 22 929 (−32 %) | 638 → 638 |
| `d3inter` (tableaux, DOOM 3), éteintes → les deux, par dessin | 12 104 → 12 108 | 796 → 796 |

Dans `mbimm`, ce qui part : `texturing_on` 850, `texture_unit_ok` 844 → 193 (celui du
dispatch), `us_get` 688 → 200, `unit_drvtex` 613 → 0, `texture_ok` 454 → 51,
`geom_texture_unit_ok` 424 → 95, `geom_ok` 384 → 43, `geom_format` 515 → 197, `find_tex`
340 → 0, `send_state` 1 695 → 1 042 (séries d'unités), `tex_prm_sig`, `upload_texture_levels`… ;
ce qui vient : la clé (`vd_key_of` 296 et sa comparaison, ~240). `LIGHTDIRTY` retire huit
`memcmp` de 96 octets par `glBegin` éclairé (6 sur 32 dans la scène).

Ce qui reste par `glBegin` dans `mbimm` (7 509) : `send_state` 1 042 (dont `su_diff`, boucle sur
16 unités, ~340), `geom_send_all` 577, le dispatch (`geom_dispatch_locked` 526, `us_fill` 371,
crochetage 238, `geom_format` 197, `texture_unit_ok` 193…), `vd_key_of` 296, `prog_state` 233,
`geom_end` 186, `close_raw` 154. Sous le seuil du §10.6 de `d3-plugin-x86.md` : rien d'autre
n'est construit.

**Part attendue sur Marble Blast** (profil du §1, 13,1 ms/image) : le verdict refait au
Begin y faisait `geom_ok` 4,4 % + `texture_ok` 5,3 % + `geom_format` 2,2 % = 11,9 % du fil
du jeu ; la clé en reprend ~9 % ; s'y ajoutent `sync_to_host` (0,7 %), les `memcmp` des
lumières (`geom_send_all` 3,5 % dont `memcmp` 1,8 %) et les séries d'unités de `send_state`.
Ordre de grandeur : **−11 à −13 % du fil du jeu**, soit ~1,2 à 1,6 ms/image si le fil du jeu
fait l'image. Zen (`geom_begin` 6,3 %) : ~−3 %. UT2004 et DOOM 3 n'ont pas de `glBegin` dans
leur profil : neutres attendus (`d3inter` +4 instructions par dessin).

### 3.4 Preuves (VM de dev)

- `plugmd5`, **68 scènes** (les 66 du job jusqu'ici, `d3idx` compris, plus `mbimm` et
  `litdirty`), images comparées à l'octet, six modes : `ref`, `bv` (`BEGINVD=1`), `ld`
  (`LIGHTDIRTY=1`), `all` (les deux), `chk` (`BEGINVD=2 LIGHTDIRTY=2 STUNIT=2`), `bvsu`
  (`BEGINVD=1 LIGHTDIRTY=2 STUNIT=2` : la remise à zéro des unités coupées sous le contrôle
  des séries d'unités) : **0 écart**. Compteurs sur l'ensemble des scènes : 1 402 `glBegin`
  dont le verdict aurait été repris, recalculés et comparés, **0 écart** (6 refaits, clé
  changée) ; 126 comparaisons de lumière faites, 410 sautées, **0 changement non signalé** ;
  séries d'unités contrôlées 2 867 (`chk`) et 2 471 (`bvsu`), **0 écart** ; matrices
  (`MTXDIRTY`) 0 écart. (`arbvp`…`glsldp` rendent 1 dans tous les modes, comme au §10.3.)
- Job `gpu` : défaut **56 OK, 0 échec** (`qgpu_test` 0) ; les deux variables allumées
  **56 OK, 0 échec**. `mbimm` et `litdirty` comparées au rendu d'Apple (job `gpu`,
  `SCENES="mbimm litdirty"`, leviers allumés) : écart hors arêtes 0 et 1 (32 bits), 17 et
  16 (16 bits, sous le seuil de 17).
- Bundle construit dans la VM de dev : `bench/beginvd-2210/GLDriver-POMPPC-20261022-beg.bundle.tar`
  du worktree (binaire identique à celui des preuves `plugmd5`).

### 3.5 A/B sur la VM quotidienne (à jouer)

Installer le bundle `20261022-beg`, puis une partie sous contrôle (ligne `BEG image N` de la
note : écarts à 0 ; `D3P` : `stunit 2` à 0) :

```
tools/tcg/matab.sh beg-chk 1 "chk:QEMU_FAST=1 POMPPC_GL_BEGINVD=2 POMPPC_GL_LIGHTDIRTY=2 POMPPC_GL_STUNIT=2" mb fen
```

puis l'A/B des quatre jeux (un levier n'est allumé par défaut que s'il ne perd sur aucun) :

```
tools/tcg/matab.sh beg-mb 3 "ref:QEMU_FAST=1" "a:QEMU_FAST=1 POMPPC_GL_BEGINVD=1 POMPPC_GL_LIGHTDIRTY=1" mb fen
tools/tcg/matab.sh beg-zen 3 "ref:QEMU_FAST=1" "a:QEMU_FAST=1 POMPPC_GL_BEGINVD=1 POMPPC_GL_LIGHTDIRTY=1" zen fen
tools/tcg/matab.sh beg-ut 3 "ref:QEMU_FAST=1" "a:QEMU_FAST=1 POMPPC_GL_BEGINVD=1 POMPPC_GL_LIGHTDIRTY=1" ut fen
tools/tcg/matab.sh beg-d3 3 "ref:QEMU_FAST=1" "a:QEMU_FAST=1 POMPPC_GL_BEGINVD=1 POMPPC_GL_LIGHTDIRTY=1" d3 fen
```

Pour séparer les deux leviers sur Marble Blast : bras `"b:QEMU_FAST=1 POMPPC_GL_BEGINVD=1"`
et `"l:QEMU_FAST=1 POMPPC_GL_LIGHTDIRTY=1"` dans la même campagne.

## 4. Levier B — l'échange de chaque image (22/10/2026)

Plugin `20261022-swb`, branche `worktree-agent-a8afc67924706083b`. Cinq variables, toutes
**éteintes par défaut** (`*_DEFAULT 0`). Ni `qgpu_proto.h` ni `qgpu_abi.h` ne changent :
QEMU et le kext restent tels quels.

### 4.1 Où part le temps de l'échange

Les profils du §1 (`sample`, échantillons du fil du jeu sous `pomppc_proc_pre`) donnent :

| poste | Marble Blast (1 761 éch.) | Zen (1 774 éch.) |
|---|---|---|
| `direct_window_ok` : `CGSGetWindowBounds` (notre fenêtre et **chaque** fenêtre au-dessus : 2 messages Mach chacune), `CGSGetSurfaceBounds`, liste | 65 | — (chemin plein écran) |
| `GetFrontProcess` (toutes les 10 images) | 3 | 13 |
| `drawable_hidden` (`CGSGetOnScreenWindowList`, toutes les 10 images) | 2 | 15 |
| six lectures CoreGraphics de l'écran (Q6, à chaque image) | 9 | 30 |
| `wait_half_at` (barrière de l'image n−1) + `flush_at` (soumission) | 9 | 50 |
| `stats_frame` : `fprintf` de `frames.csv`, `crash_hook_check`, `fflush` | 20 | 29 |
| échange normal d'Apple toutes les `DIRECT_REFRESH` = 90 images | 31 | — |

Le compte à l'instruction (greffon `hotblocks`, VM de dev, `swapbench fen`) chiffre ce que le
`sample` ne fait que montrer : **une vérification complète de la fenêtre coûte environ un
million d'instructions invitées et ~8 ms** (une quinzaine de fenêtres au-dessus de la nôtre :
le Dock et ses tuiles, la barre des menus ; la moitié du travail est dans le WindowServer et le
noyau). Les six lectures de l'écran ne sont pas « une structure déjà en mémoire » comme le
supposait Q6 : chacune refait `updateAllDisplayInfoAsNeeded`, dont `bindDisplayMapping`
alloue, compare et libère une région par écran (~14 000 instructions par image en tout).

**L'attente de Marble Blast (10,3 % de son fil, ~0,5 ms/image).** Les lignes `VIDAGE` de la
note de `gprof-2210-mb` le disent : 1,67 soumissions par image, dont **0,67 viennent de
`pomppc_texture_deleted`** (`L4581` : un `flush()` après chaque `TEX_DESTROY`, hérité du mode
synchrone de 18749d3). Avec deux moitiés, la soumission de plus fait que le changement de
moitié qui suit attend une soumission partie ~5 ms plus tôt (`W4255` : 333 attentes réelles
sur 836, 0,40-0,44 ms/image) ; l'attente de l'échange lui-même (`W17023`) n'est que de
0,06 ms/image. C'est de la latence (doorbell → fil hôte → barrière), pas une lenteur de l'hôte.
Une conception plus profonde (trois moitiés, deux soumissions en vol) a déjà été écartée pour
la place de l'arène (en-tête de la disposition de la tranche) ; il suffit de ne pas faire la
soumission de trop : **une soumission par image et l'attente disparaît**. Ce qui reste de
l'attente à l'échange, dans Zen (`W17023`, âge 5,04 ms, 0,12 ms/image), c'est la latence de
l'hôte pour une image (≈ la durée d'une image) : du ressort du levier C.

### 4.2 Les leviers

| variable | ce qu'elle fait | exactitude |
|---|---|---|
| `POMPPC_GL_WINSEED=1` (2 = contrôle) | la vérification de fenêtre (`direct_window_ok`) et « fenêtre cachée » (`drawable_hidden`) ne sont refaites en entier que si la **géométrie des fenêtres** a bougé : `CGSGeometrySeed()`, un mot de la mémoire partagée du WindowServer (lu sans message), et, quand elle existe, `CGSWindowVisibleRegionSeed(cid, wid)` (région visible de notre fenêtre ; refusée — 1010 — pour une fenêtre GLUT de la VM de dev : la géométrie vaut alors seule). Graines inchangées depuis une vérification complète **réussie** : ne sont revus que l'alpha des fenêtres invisibles qui touchaient notre rectangle (d'ordinaire aucune) et le curseur (même code qu'avant). `GetFrontProcess` reste demandé toutes les 10 images. Un refus n'est jamais repris (sauf un refus dû au seul curseur, dont la géométrie tient). | contrôle `=2` : la vérification complète est faite quand même et comparée — **0 écart** sur `swapbench` move (déplacements), cover (recouvrement), pop (fenêtre mise devant, **derrière** par `glutPushWindow` — l'ordre seul —, devant, retirée) : 21 + 149 + 133 reprises comparées, 41 + 298 + 266 « cachée » comparées |
| `POMPPC_GL_TEXDEFER=1` | `TEX_DESTROY` sans `flush()` : la destruction part avec la soumission suivante, dans l'ordre du flux (ce qui emploie la texture la précède, un `TEX_CREATE` qui reprend l'identifiant la suit) | flux identique hors le découpage en soumissions ; `plugmd5` (dont `texdelmid`) |
| `POMPPC_GL_DISPSEED=1` (2 = contrôle) | base, pas, profondeur et taille de l'écran (Q6) gardés tant que `CGSDisplayChangedSeed` et `CGSDisplayMappingChangedSeed` n'ont pas bougé — ce sont **les graines sur lesquelles CoreGraphics règle lui-même son cache d'écran** (désassemblage de `updateAllDisplayInfoAsNeeded`). Seulement quand l'hôte écrit l'image (`G.scanout` : SURF_PRESENT borné à la fenêtre visible, pas et profondeur refusés s'ils ne sont pas ceux de l'écran, Q3) ; relu de toute façon toutes les 10 images ; une lecture n'est retenue que si les graines n'ont pas bougé pendant | contrôle : relit et compare, **0 écart** (449 à 1 349 lectures par passe) |
| `POMPPC_GL_SWAPLITE=1` (2 = contrôle) | la ligne de `frames.csv` (trace : l'usage réel, lanceurs « trace » et matrice) formatée à la main au lieu de `fprintf` — même texte, `%.3f` arrondi sur la valeur exacte (`fmsub`), en 32 bits ; la boucle de `surftex_swapped` sur toutes les textures sautée tant qu'aucune texture de surface n'a existé | contrôle : `snprintf` aussi, comparé, **0 écart** (toutes les lignes de toutes les passes) |
| `POMPPC_GL_PRESENTQ=1` | présentation par l'hôte : l'image n est soumise **avant** d'attendre la n−1 (attente reportée au changement de moitié, même barrière) | rien ne lit l'image n−1 entre-temps quand l'hôte présente ; **neutre** en mesure (§4.4) |

Essayé puis retiré : **SWAPWIN**, un fil veilleur qui faisait les vérifications de fenêtre à
la place du fil du jeu (commit 01984aa). −13 à −15 % sur `swapbench` en fenêtre, mais +8 %
d'instructions par image (pour rester dans la borne de 10 images, il demande plus tôt, donc
plus souvent) ; WINSEED fait mieux sans fil.

### 4.3 Défaut corrigé au passage : `us_fill` reprenait un PTex libéré

`swapbench` avec une texture créée, employée et détruite à chaque image (`SWB_TEXDEL=1`)
faisait **planter le plugin `20261021-d3p` sans aucun levier** (SIGBUS dans
`gldUpdateDispatch` → `geom_ok` → `texturing_on` → `us_get` → `us_fill` → `tex_cp_fill` →
`tex_surface` sur l'objet de GLEngine libéré ; `POMPPC_GL_TEXMEMO=0` l'évitait, le rendu
d'Apple aussi). `us_fill` remettait `p->us_del` à l'époque courante à chaque unité parcourue,
même sans texture relue : le couple `(us_dt, us_t)` d'avant une destruction y survivait,
déclaré frais, et GLEngine redonne volontiers la même adresse à la texture suivante. La
mémoire d'une époque passée est maintenant effacée pour toute unité qui n'est pas relue.
Correctif sans variable (c'est un défaut) ; `plugmd5` et `gpu` inchangés.

### 4.4 Preuves et mesures (VM de dev `tiger-dev-plugin2.raw`, RTX, SMP 2, binaire rapide)

- `plugmd5`, **66 scènes × 4 modes** (`ref`, `all` = les cinq, `chk` = contrôles `WINSEED=2
  SWAPLITE=2 DISPSEED=2` + `TEXDEFER PRESENTQ`, `tdf`), `POMPPC_GL_FRAMES` posé :
  **0 écart** ; 586 vidages évités par TEXDEFER sur l'ensemble des scènes.
- Job `gpu` : défauts **56 OK, 0 échec** (`qgpu_test` 0) ; les cinq leviers exportés
  **56 OK, 0 échec**.
- Session graphique (job `swapwin`, nouveau, et `guest/gltest/swapbench.c`, nouveau),
  `ref`/`all`/`chk` : fenêtre, fenêtre déplacée, deux fenêtres qui se recouvrent, seconde
  fenêtre devant/derrière/retirée, **plein écran CGL** en 1024×768 (SURF_PRESENT) et en
  800×600 (changement de mode : voie de relecture, SURF_PRESENT refusé par le VGA de la VM de
  dev comme avant) : la zone recouverte ne montre **jamais** la fenêtre de derrière
  (870 + 361 relevés par mode), centre de l'écran juste en plein écran, mêmes résultats dans
  les trois modes. (`move` signale le premier déplacement, où l'ancien et le nouveau
  rectangle se chevauchent : défaut du relevé, identique sans levier.)

Temps par image, `swapbench` avec du travail simulé (`SWB_WORK=20`, régime d'un jeu : le G4
est le facteur limitant), 3 passes par mode entrelacées, 1 000 images, médianes :

| mode | fenêtre 320×240 (ms) | plein écran 1024×768 (ms) |
|---|---|---|
| référence | 5,327 | 4,023 |
| WINSEED | 4,513 (**−15,3 %**) | 4,014 (0) |
| DISPSEED | 5,272 (−1,0 %) | 3,976 (−1,2 %) |
| SWAPLITE | 5,303 (−0,4 %) | 4,006 (−0,4 %) |
| PRESENTQ | 5,337 (+0,2 %) | 4,021 (0) |
| les cinq | 4,406 (**−17,3 %**) | 3,958 (−1,6 %) |

Avec une texture détruite par image (`SWB_TEXDEL=1`, le cas de Marble Blast), 2 passes :

| mode | fenêtre (ms) | plein écran 800×600 (ms) | vidages / image | attente des barrières (ms/image) |
|---|---|---|---|---|
| référence | 6,48 | 7,08 | 2,00 | 0,87 (fen.) / 2,26 (p. é.) |
| TEXDEFER | 5,55 (**−14 %**) | 5,19 (**−27 %**) | 1,00 | 0,08 / 0,18 |
| les cinq | 4,61 (−29 %) | 5,14 (−27 %) | 1,00 | |

Instructions invitées par image (`hotblocks`, toute la VM, `swapbench fen` sans travail
simulé, 2 passes) : référence ~1,03 M ; WINSEED −0,1 M ; DISPSEED −14 000 dans les
bibliothèques partagées (`bindDisplayMapping`, `szone_*`) ; SWAPLITE −6 100 dans
`libSystem` (`__vfprintf`, `__sfvwrite`, `__dtoa`) pour +770 dans le plugin ; les cinq
−0,18 M (−17 %).

Ce que cela vaut dans les jeux, d'après les profils du §4.1 (à confirmer par l'A/B) : Marble
Blast ~5-8 % (vérifications de fenêtre 3,6 %, attente des soumissions en trop ~3 %, écran et
trace ~1 %), Zen ~3 % (écran 1,7 %, liste « cachée » 0,85 %, trace ~1 %), UT2004 et DOOM 3 de
l'ordre du pour cent.

Reste, non traité : `GetFrontProcess` toutes les 10 images (0,7 % de Zen), l'échange normal
de `DIRECT_REFRESH` (1,8 % de Marble Blast), `crash_hook_check` (deux `sigaction` par image,
0,3 %), la latence de l'hôte à l'échange (levier C).

### 4.5 A/B à jouer sur la VM quotidienne

Installer le bundle construit dans la VM de dev (`bench/plugB-2210/GLDriver-POMPPC-20261022-swb.bundle.tar`
du worktree, md5 `a8787c29f51c307c5a6bc4e0b88dac2c`), VM quotidienne démarrée :

```
cat bench/plugB-2210/GLDriver-POMPPC-20261022-swb.bundle.tar | tools/guest/tssh.sh \
  "cat > /tmp/plugB.tar && cd /tmp && rm -rf GLDriver-POMPPC.bundle && tar xf plugB.tar && \
   echo tiger974 | sudo -S sh -c 'rm -rf /System/Library/Extensions/GLDriver-POMPPC.bundle && \
   cp -R /tmp/GLDriver-POMPPC.bundle /System/Library/Extensions/ && \
   chown -R root:wheel /System/Library/Extensions/GLDriver-POMPPC.bundle && \
   chmod -R 755 /System/Library/Extensions/GLDriver-POMPPC.bundle'"
```

D'abord une partie sous contrôle (ligne `ECHANGE` de la note : `winseed … 0 écarts`,
`dispseed … 0 écarts`, `swaplite … 0 écarts`), en fenêtre puis en plein écran :

```
tools/tcg/matab.sh plugB-chk 1 "chk:QEMU_FAST=1 POMPPC_GL_WINSEED=2 POMPPC_GL_DISPSEED=2 POMPPC_GL_SWAPLITE=2 POMPPC_GL_TEXDEFER=1" mb fen
tools/tcg/matab.sh plugB-chk-pe 1 "chk:QEMU_FAST=1 POMPPC_GL_WINSEED=2 POMPPC_GL_DISPSEED=2 POMPPC_GL_SWAPLITE=2 POMPPC_GL_TEXDEFER=1" zen pe
```

puis l'A/B (bras `b` : les quatre leviers qui gagnent dans la VM de dev ; bras `c` : `b` +
PRESENTQ, neutre dans la VM de dev, à trancher sur les jeux) :

```
B="QEMU_FAST=1 POMPPC_GL_WINSEED=1 POMPPC_GL_TEXDEFER=1 POMPPC_GL_DISPSEED=1 POMPPC_GL_SWAPLITE=1"
tools/tcg/matab.sh plugB-zen 3 "ref:QEMU_FAST=1" "b:$B" "c:$B POMPPC_GL_PRESENTQ=1" zen fen
tools/tcg/matab.sh plugB-mb  3 "ref:QEMU_FAST=1" "b:$B" "c:$B POMPPC_GL_PRESENTQ=1" mb fen
tools/tcg/matab.sh plugB-ut  3 "ref:QEMU_FAST=1" "b:$B" ut fen
tools/tcg/matab.sh plugB-d3  3 "ref:QEMU_FAST=1" "b:$B" d3 fen
tools/tcg/matab.sh plugB-zen-pe 3 "ref:QEMU_FAST=1" "b:$B" zen pe
```

Décision : passer les `*_DEFAULT` à 1 levier par levier si aucun jeu ne perd (WINSEED et
TEXDEFER d'abord, ce sont les deux postes visibles dans les profils).

## 5. Les deux leviers réunis (22/10, révision `20261022-ab`)

Fusion des branches A (§3) et B (§4), tous les `*_DEFAULT` à 0. Bundle construit dans la VM
de dev : `bench/plug-ab-2210/GLDriver-POMPPC-20261022-ab.bundle.tar`, md5
`850af1c4695bfdb035aa59bad6cc054e` (binaire identique dans la session graphique et en
single-user).

Le correctif `us_fill` de B et la reprise du verdict au Begin de A (`ti = p->vd_ti`, sans
`texture_ok`) ne se gênent pas : le seul `free` d'un PTex est dans
`pomppc_texture_deleted`, qui fait d'abord `VD_BUMP()` (et `tex_del_epoch++`), verrou tenu.
`vd_epoch` est dans la clé du verdict (`vd_key_of`, `vd_key_head`) ; après toute destruction
la clé rangée ne correspond plus, `bv_same` rend 0 et le Begin refait la voie complète
(`texture_ok` → `us_fill`, qui voit `us_del != tex_del_epoch` et efface sa mémoire). Un PTex
libéré ne peut donc pas être repris par `vd_ti`. TEXDEFER ne reporte que le `flush` du
`TEX_DESTROY`, pas l'époque.

Preuves (VM de dev `tiger-dev-plugin.raw`, binaire rapide, SMP 2, RTX) :

- `plugmd5` en single-user (en session graphique `GLTEST_NOWS` rend `CGLChoosePixelFormat`
  10006 et toutes les scènes rc=2 sans image, et le job compte quand même 0 écart : à lancer
  en single-user), 68 scènes, modes `ref`, `all` (BEGINVD LIGHTDIRTY WINSEED TEXDEFER
  DISPSEED SWAPLITE à 1) et `chk` (BEGINVD=2 LIGHTDIRTY=2 STUNIT=2 WINSEED=2 DISPSEED=2
  SWAPLITE=2 TEXDEFER=1) : **0 écart** (60 scènes rc=0, `arbvp`…`glsldp` rc=1 dans les trois
  modes). `all` : 2 170 verdicts repris au glBegin, 6 refaits ; `chk` : 2 170 contrôles BEG,
  0 écart ; 126 comparaisons de lumière, 0 écart ; STUNIT 3 376 contrôles, 0 écart ; MTXDIRTY
  0 écart ; 595 vidages évités par TEXDEFER.
- Job `gpu` : défauts **56 OK, 0 échec** (`qgpu_test` 0) ; les six leviers exportés (le job
  ne lit pas `env.sh` : ligne `. ./env.sh` ajoutée à la copie préparée) **56 OK, 0 échec**.
- `swapwin` en session graphique, `TEXDEL=1` (une texture détruite par image), `fen move
  cover pop fs`, modes `ref`, `all`, `all`+PRESENTQ, `chk` : **aucun plantage**, `cover` et
  `pop` 0 relevé mauvais, plein écran juste ; contrôles WINSEED (147 et 132) et DISPSEED (449
  et 404) 0 écart, SWAPLITE 0 écart. Les 4 échecs du job sont les 4 `move` (premier
  déplacement, défaut du relevé déjà vu sans levier au §4.4).

## 6. A/B sur la VM quotidienne et allumage (22/10, révision `20261022-abon`)

VM quotidienne, binaire rapide, bundle `20261022-ab` (§5), médianes en ms/image.

**A seul** (`bench/tcg/ab/beg-*`, 3 parties par bras) :

| jeu | ref | BEGINVD + LIGHTDIRTY |
|---|---|---|
| Marble Blast | 13,2 | 10,9 (BEGINVD seul 11,1) |
| Zen | 5,4 | 5,1 |
| UT2004 | 43,2 | 44,2 |
| DOOM 3 | 93,4 | 95,2 (bruit : les parties se recouvrent) |

Contrôle sur Marble Blast (`beg-chk`) : 418 440 contrôles BEG, 0 écart ; LIGHTDIRTY 0 écart ;
STUNIT 0 écart.

**B seul** (`bench/tcg/ab/plugB-*`, 3 parties par bras) :

| jeu | ref | WINSEED TEXDEFER DISPSEED SWAPLITE | + PRESENTQ |
|---|---|---|---|
| Marble Blast | 13,2 | 12,6 | 12,4 |
| Zen | 5,4 | 5,1 | 5,1 |
| UT2004 | 43,0 | 42,3 | |
| DOOM 3 | 92,2 | 92,5 | |

Contrôle sur Marble Blast : 0 écart WINSEED, DISPSEED, SWAPLITE. Zen en plein écran n'est pas
mesurable par la matrice (la case du menu Options est dans `prefs.dat`, chiffré).

**A+B** (`bench/tcg/ab/ab-*`, 5 parties par bras, les six leviers sans PRESENTQ) :

| jeu | ref | A+B | écart |
|---|---|---|---|
| Marble Blast | 13,1 (parties 12,7-13,3) | 10,1 (parties 9,8-10,6) | **−23 %** |
| Zen | 5,4 | 5,0 | −7 % |
| UT2004 fenêtre | 42,7 | 41,8 | −2 % |
| UT2004 plein écran | 44,0 | 43,0 | −2 % |
| DOOM 3 | 93,5 | 93,1 | neutre |

Contrôle sur UT2004 en plein écran (`ab-chk-utpe`) : tous les compteurs à 0 écart.

**Décision** : BEGINVD, LIGHTDIRTY, WINSEED, TEXDEFER, DISPSEED et SWAPLITE allumés par défaut
(`*_DEFAULT 1`, révision `20261022-abon`) ; chaque variable à `=0` éteint son levier, `=2`
garde le contrôle ; BEGINVD reste soumis au verdict unique (`POMPPC_GL_VERDICT`). PRESENTQ
reste éteint : neutre dans la VM de dev, à peine mieux sur Marble Blast, non tranché.

Pièges notés en route :

- `plugmd5` lancé en session graphique (`devloop.py start --gui`) est un **faux vert** :
  `GLTEST_NOWS` y rend `CGLChoosePixelFormat` 10006, toutes les scènes sortent rc=2 sans
  image, et le job compte 0 écart. Le lancer en single-user.
- Le job `gpu` ne lit pas `env.sh` : pour exporter des variables, ajouter `. ./env.sh` à la
  copie préparée.

Preuves aux nouveaux défauts (VM de dev `tiger-dev-plugin.raw`, aucune variable posée) :
bundle `bench/plug-ab-2210/GLDriver-POMPPC-20261022-abon.bundle.tar`, md5
`c16990df7088c6b44adc6c659f82fc21`.

- `plugmd5` en single-user, 68 scènes, `off` (les six variables à `=0`, référence) contre
  `def` (rien) : **0 écart** (60 scènes rc=0, `arbvp`…`glsldp` rc=1 dans les deux modes).
  Sans variable, la note montre les leviers actifs : `ECHANGE : texdefer=1 swaplite=1
  dispseed=1 presentq=0 winseed=1`, lignes `BEG … beginvd 1 … lightdirty 1` (2 170 verdicts
  repris au glBegin, 6 refaits ; 595 vidages évités) ; sous `off`, aucune ligne BEG et
  `texdefer=0 swaplite=0 dispseed=0 winseed=0`.
- Job `gpu` aux défauts : **56 OK, 0 échec** (`qgpu_test` 0).
- `swapwin` en session graphique, `TEXDEL=1`, `fen move cover pop fs`, modes `def` et `off` :
  aucun plantage ; `cover`, `pop`, `fs` justes ; aux défauts `ECHANGE image 500 : texdefer 500
  vidages évités … winseed 49 reprises` (cover). Les 2 échecs du job sont les deux `move`
  (défaut du relevé, identique sous `off`).

## 7. Levier E : verdict par unité élargi (23/10, révision `20261023-uvw`)

Branche `worktree-agent-a245ab1fae021e4d6`. Une variable, **éteinte par défaut**
(`UVWIDE_DEFAULT 0`) : `POMPPC_GL_UVWIDE=1` l'allume, `=2` contrôle. Elle n'agit que sous
`UNITVD`, donc sous le verdict unique. Compteurs sur une ligne `UVW` de la note, toutes les
500 images et à la destruction du contexte. Ni `qgpu_proto.h` ni `qgpu_abi.h` ne changent :
QEMU et le kext restent tels quels.

### 7.1 Pourquoi

Profil invité neuf des quatre jeux aux six leviers (`bench/tcg/ab/gprof-2210b-*`). Les leviers
C (attente de l'hôte, ≤ 3 %) et D (verrous, ~1,5 %) sont devenus petits. Le poste commun le
plus lourd est le verdict complet refait au dispatch : `geom_dispatch_locked` fait 5,2 % du
fil du jeu dans Zen, 10,2 % dans UT2004, 8,8 % dans DOOM 3 et 4,8 % dans Marble Blast.

La note d'UT2004 dit `verdict par unité (unitvd 1) 1 repris, 223 586 complets`, celle de Zen
`0 repris, 342 400 complets`. `uv_take` (le verdict refait pour les seules unités désignées,
`docs/d3-plugin-x86.md` §3) ne reprenait le verdict que si le bloc ne portait, hors de la
liste blanche, que des bits d'unité de `+0x04`. Or le relevé `POMPPC_GL_BLOCKDUMP=600:2`
d'UT2004 (`bench/tcg/ab/bdump-2210-ut`) montre surtout `+08=00000010 +10=00000100
+14=00000001 +48=00000001` : `glTexEnvfv(GL_TEXTURE_ENV_COLOR)` de l'unité 0, plus la
modèle-vue. Viennent ensuite le brouillard (`+00 00000800`), le texgen et la matrice de
texture (`+04`/`+08 0x10000 << u`), l'éclairage (`+0c` 0x1, 0x155, 0x403fd), puis le mode, la
couleur et le combinateur de chaque unité (`+10`).

Simulation sur ce relevé (487 dispatches, 2 neutres pour la liste blanche) :

- l'ancienne condition reprenait 0 dispatch sur 485 ;
- la condition élargie en reprend **481 (99,2 %)** ;
- les 4 restants portent `+00 00000010` (`glClearDepth`, non admis).

### 7.2 Les bits admis, et qui les lit

Le relevé est vérifié sur les unités 0 à 3 par la nouvelle scène `gltest uvwide`, sous
`POMPPC_GL_BLOCKDUMP` et `GLTEST_STEPS=1` (`docs/re/bloc-changements-r4.md` §2, complété).
Pour chaque bit, on a cherché qui lit l'état dans `geom_ok`, `geom_raster_ok`, `accel_ok_for`,
`geom_texture_ok`, `texture_ok`, `texture_unit_ok`, `geom_texture_unit_ok`, `geom_format`,
`va_gen_sizes` et `vd_key_of`.

**Bits d'unité.** Ils désignent l'unité *u*, qui est refaite par `geom_texture_unit_ok` et
`texture_unit_ok`. Ces deux fonctions relisent la cible allumée (`unit_mask`), la texture,
`TU_ENV_MODE`, `TU_ENV_COLOR` et le combinateur (`combine_ok`) : tout ce que garde `TexUnit`.

| mot | bits | appels |
|---|---|---|
| `+0x04` | `1 << u` | liaison, paramètres, image de la texture liée (l'ancienne condition) |
| `+0x08` | `0x10000 << u` | allumage d'une cible de l'unité, matrice de texture |
| `+0x10` | `1 << u` | `glTexEnv(GL_TEXTURE_ENV_MODE)` |
| `+0x10` | `0x100 << u` | `glTexEnv(GL_TEXTURE_ENV_COLOR)` |
| `+0x10` | `0x1000000 << u` | tout paramètre du combinateur (`COMBINE_*`, `SOURCEn_*`, `OPERANDn_*`, `RGB_SCALE`, `ALPHA_SCALE`) |

**Bits de format seul.** Dans le verdict, l'état qu'ils signalent n'est lu que par
`geom_format`, que la reprise refait toujours (et `va_gen_sizes` aussi si le format bouge).
`accel_ok_for` n'est appelé par le verdict qu'au chemin brut (`geom_raster_ok`, `raw = 1`) :
le test du brouillard `GL_NICEST` y est sauté. Le dessin non brut l'appelle lui-même.

| mot | bits | appels | lecteur dans le verdict |
|---|---|---|---|
| `+0x00` | `0x800` | `glFog*` (mode, densité, couleur, début, fin, source de la coordonnée), `glEnable/Disable(GL_COLOR_SUM)` ; avec `0x800000` : allumage de l'éclairage | `geom_format` (`GS_FOG_COORD_SRC`, `GS_COLOR_SUM`) |
| `+0x00` | `0x1000` | `glEnable/Disable(GL_FOG)` | `geom_format` (`GS_FOG`) |
| `+0x04` | `0x10000 << u` | texgen de l'unité *u* (allumage, `glTexGeni`, `glTexGenfv`) | `geom_format` (`texgen_needs_normal`) |
| `+0x0c` | `0x1` | `glMaterialfv`, `glLightModel*`, `glColorMaterial` (avec `0x40000`) | aucun |
| `+0x0c` | `0x2` | `glEnable/Disable(GL_LIGHTING)` (avec `+00 0x800800`, `+08 0x10`) | `geom_format` (`GS_LIGHTING`) |
| `+0x0c` | `4 << 2i`, `8 << 2i` (jusqu'à `0x3fffc`) | lumière *i* : paramètres, allumage | aucun |
| `+0x0c` | `0x40000` | `glEnable/Disable(GL_COLOR_MATERIAL)` | aucun |
| `+0x10` | `0x10000 << u` | `glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS)` | aucun : seul `compute_state` lit `TU_LOD_BIAS` |

Masques (`uvw_mask` dans `pomppc_accel.c`), en plus de `wl_mask` :

- `+00 0x00001800` ;
- `+04 0x00ff00ff` ;
- `+08 0x00ff0000` ;
- `+0c 0x0007ffff` ;
- `+10 0xffffffff` (les quatre octets ci-dessus).

Unités désignées : `(+04 | +08 >> 16 | +10 | +10 >> 8 | +10 >> 24) & 0xff`. Tout le reste
n'est toujours pas neutre, notamment :

- `+08 0x01000000` (plan de coupe) et `0x40000000` (normalisation) ;
- `+0c 0x00080000` et au-delà (programmes, « T&L perdu ») ;
- `+00 0x10` (`glClearDepth`), `0x80` (tampon de dessin), `0x200000` (`glPixelStorei`).

**L'état envoyé.** `st_dirty` est posé par `geom_dispatch_locked` avant le verdict, d'après
`st_mask` seul, comme avant. Les bits de brouillard, d'éclairage, de texgen (sauf l'unité 0)
et de biais de LOD n'y sont pas : `compute_state` est refait. `LIGHTDIRTY` et `MTXDIRTY` lisent
le bloc en tête de dispatch, eux aussi indépendamment du verdict. Les clés d'unités
(`STUNIT`, `su_diff`) suivent le `TexInfo` rangé, qui porte la nouvelle couleur ou le nouveau
mode de l'unité refaite.

### 7.3 Deux cas de plus, démontrés

- **Aucune unité désignée** (`umask == 0` : le bloc ne porte que des bits de format, par
  exemple `glFog` ou `glLight` entre deux dessins). Toutes les unités sont gardées, après le
  même contrôle qu'avant : même objet de GLEngine (`us_get`) qu'au verdict rangé, unité
  coupée restée coupée.
- **Texturage éteint au relevé** (`!sc->on`). Le complet rend « ok » sans aucune unité :
  `geom_texture_ok` et `texture_ok` s'arrêtent à `texturing_on`, qui lit le même relevé. La
  reprise rend un `TexInfo` nul, sans regarder les unités. Avant, `uv_take` renvoyait au
  complet. Le cas « aucune unité texturée au verdict gardé » (`!any`) est repris lui aussi :
  les unités non désignées doivent être coupées (même test), les désignées sont refaites
  comme le complet les ferait.

Sous `UVWIDE=0`, `uv_take` est inchangé : même condition de bloc et mêmes refus `!any` et
`!sc->on`. `df_classify` (`DISPFAST`) calcule la condition élargie dans la même passe (bit
`DF_UVW`), avec la même réponse que `uvw_block_ok` mot à mot. Le mode `chknd` des preuves
couvre le chemin sans `DISPFAST`.

### 7.4 Compteurs

Ligne `UVW … : uvwide N, R reprises élargies (U unités refaites, E texturage éteint, Z sans
unité désignée) ; contrôle S identiques, D écarts`. Une reprise est « élargie » si l'ancienne
condition l'aurait refusée : bloc hors de l'ancienne condition, `!any` ou `!sc->on`. Les
reprises qui tenaient déjà restent comptées sur la ligne `D3X`.

Sous `UVWIDE=2`, chaque reprise élargie est calculée puis **non prise** : le verdict complet
suit et sert, et il est comparé à la reprise (ok, format, tailles des génériques, unités).
Sous `VERDICTCHECK=1`, les reprises élargies vont aussi dans ces compteurs, et un écart est
noté `UVWIDE écart`.

### 7.5 Preuves (VM de dev `tiger-dev-plugin.raw`, binaire rapide, SMP 2, RTX)

- **Relevé** : scène `uvwide` (nouvelle, 56 cases, une par pas), `GLTEST_STEPS=1
  POMPPC_GL_BLOCKDUMP=0:1000000`. Chaque bit du §7.2 a été vu sur les unités 0 à 3. Il n'y
  a aucune surprise : le mot `+0x10` est entièrement par unité ; le texgen de l'unité 1 pose
  `+04 00020000` ; `glColorMaterial` pose `+0c 00040001`.
- **`plugmd5` en single-user**, **69 scènes** (les 68 du §3.4 et `uvwide`), images comparées
  à l'octet, six modes :

  | mode | variables |
  |---|---|
  | `off` | défauts, `UVWIDE` éteint |
  | `uvw` | `UVWIDE=1` |
  | `chk` | `UVWIDE=2` |
  | `chknd` | `UVWIDE=2 DISPFAST=0` : condition de bloc sans `df_classify` |
  | `vchk` | `UVWIDE=1 VERDICTCHECK=1` |
  | `all2` | `UVWIDE=1`, avec `BEGINVD`, `STUNIT`, `LIGHTDIRTY`, `MTXDIRTY` et `DISPFAST` à 2 : les lecteurs du verdict rangé sous contrôle |

  Résultat : **0 écart**. 61 scènes sortent rc=0 ; `arbvp`…`glsldp` sortent rc=1 dans les six
  modes, comme au §3.4.
- **Compteurs sur l'ensemble des scènes** :

  | compteur | `off` | `uvw` |
  |---|---|---|
  | verdicts repris par unité | 2 499 | 2 688 |
  | dont reprises élargies | — | 189 (99 unités refaites, 83 texturage éteint, 87 sans unité désignée) |
  | verdicts complets | 389 | 200 |

  Contrôles, tous à **0 écart** :

  - `chk` et `chknd` : 189 reprises élargies contrôlées. Les deux chemins de classement
    donnent les mêmes comptes.
  - `vchk` : 189 reprises élargies, 2 499 reprises par unité, 4 135 reprises du dessin et
    2 792 dispatches court-circuités contrôlés.
  - `all2` : `BEGINVD` 1 458 contrôles, `STUNIT` 2 912, clés `DISPFAST` 2 324, `MTXDIRTY` et
    `LIGHTDIRTY`.
- **La scène `uvwide` seule** : 55 de ses 58 dispatches sont repris sous `UVWIDE=1`, contre
  0 sans le levier. Ce sont 55 reprises élargies : 26 unités refaites, 6 avec le texturage
  éteint, 28 sans unité désignée. Sous `=2`, les 55 sont contrôlées, 0 écart. Sur la scène
  `r4`, ce sont 51 reprises élargies sous `UVWIDE=1`, et 51 contrôlées sans écart sous `=2`.
- **Job `gpu`** :
  - aux défauts : **56 OK, 0 échec** (`qgpu_test` 0) ;
  - `UVWIDE=1` exporté, avec `uvwide`, `mbimm` et `litdirty` en plus : **59 OK, 0 échec**.
    Le job ne lit pas `env.sh` : la ligne `. ./env.sh` a été ajoutée à la copie préparée.
  - Écart hors arêtes au rendu d'Apple : 1/255 pour `uvwide`, 0 pour `mbimm`, 1 pour
    `litdirty`.
  - Les références d'Apple en échec (`entry`, `v15`, `texlod`…) sont les mêmes dans les deux
    passes.
- **Bundle** : construit dans la VM de dev, aux défauts (`UVWIDE_DEFAULT 0`). Le binaire est
  identique dans les deux constructions (md5 du binaire `a1097d67afb9e4baaa4ae51ca1e3656e`).
  - fichier : `bench/uvwide-2310/GLDriver-POMPPC-20261023-uvw.bundle.tar` du worktree ;
  - md5 du tar : `4b3b72507063e6c164acf159c525a767`.

### 7.6 A/B à jouer sur la VM quotidienne

Installer le bundle (VM quotidienne démarrée) :

```
cat /home/gistarcade/src/pomppc/.claude/worktrees/agent-a245ab1fae021e4d6/bench/uvwide-2310/GLDriver-POMPPC-20261023-uvw.bundle.tar | tools/guest/tssh.sh \
  "cat > /tmp/uvw.tar && cd /tmp && rm -rf GLDriver-POMPPC.bundle && tar xf uvw.tar && \
   echo tiger974 | sudo -S sh -c 'rm -rf /System/Library/Extensions/GLDriver-POMPPC.bundle && \
   cp -R /tmp/GLDriver-POMPPC.bundle /System/Library/Extensions/ && \
   chown -R root:wheel /System/Library/Extensions/GLDriver-POMPPC.bundle && \
   chmod -R 755 /System/Library/Extensions/GLDriver-POMPPC.bundle'"
```

D'abord une partie sous contrôle par jeu. Attendu sur la ligne `UVW` de la note : `contrôle
N identiques, 0 écarts` avec N grand. Rien d'autre ne change ; les six leviers restent à leur
défaut, allumé :

```
tools/tcg/matab.sh uvw-chk-ut  1 "chk:QEMU_FAST=1 POMPPC_GL_UVWIDE=2" ut fen
tools/tcg/matab.sh uvw-chk-zen 1 "chk:QEMU_FAST=1 POMPPC_GL_UVWIDE=2" zen fen
```

Puis l'A/B, 3 parties par bras. `ref` est le bundle aux défauts (`UVWIDE=0`, les six leviers
allumés) ; `e` est le même bundle avec le levier E :

```
tools/tcg/matab.sh uvw-mb  3 "ref:QEMU_FAST=1" "e:QEMU_FAST=1 POMPPC_GL_UVWIDE=1" mb fen
tools/tcg/matab.sh uvw-zen 3 "ref:QEMU_FAST=1" "e:QEMU_FAST=1 POMPPC_GL_UVWIDE=1" zen fen
tools/tcg/matab.sh uvw-ut  3 "ref:QEMU_FAST=1" "e:QEMU_FAST=1 POMPPC_GL_UVWIDE=1" ut fen
tools/tcg/matab.sh uvw-d3  3 "ref:QEMU_FAST=1" "e:QEMU_FAST=1 POMPPC_GL_UVWIDE=1" d3 fen
```

Ce qu'il faut regarder :

- les ms/image de `bilan.txt` ;
- les lignes `D3X`/`UVW` de la note du bras `e` : part des dispatches repris contre
  complets ;
- les ms/image d'UT2004 en plein écran (`ut pe`), si UT2004 gagne en fenêtre.

Décision : `UVWIDE_DEFAULT 1` si aucun des quatre jeux ne perd.
