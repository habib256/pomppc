# Alléger le plugin pour tous les jeux (22/10/2026)

Après l'arrêt du second niveau du traducteur (`docs/plan-traducteur-rapide.md` §10.8), le
levier suivant est le travail que le plugin GL (`guest/gldriver/pomppc_accel.c`) fait sur le
fil du jeu, dans le G4 émulé. Consigne de l'utilisateur : **ce qui est fait doit servir à toute
l'émulation, pas seulement à DOOM 3**. Le plugin est du code invité PowerPC : un gain y vaut
sur les deux hôtes (PC et M4) sans travail de parité.

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
  Marble Blast, Zen.
- **B. Échange de l'image** : vérifications de fenêtre moins chères (ou déclenchées par un
  changement), attente de la moitié précédente, coût fixe par image de `stats_frame` et
  `pomppc_proc_pre`. Mesure : Zen, Marble Blast.
- Plus tard : attente de l'hôte (C), verrous (D).

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
