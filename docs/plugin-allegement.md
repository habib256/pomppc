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
  `pomppc_proc_pre`. Mesure : Zen, Marble Blast. **Construit le 22/10 : §3.**
- Plus tard : attente de l'hôte (C), verrous (D).

## 3. Levier B — l'échange de chaque image (22/10/2026)

Plugin `20261022-swb`, branche `worktree-agent-a8afc67924706083b`. Cinq variables, toutes
**éteintes par défaut** (`*_DEFAULT 0`). Ni `qgpu_proto.h` ni `qgpu_abi.h` ne changent :
QEMU et le kext restent tels quels.

### 3.1 Où part le temps de l'échange

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

### 3.2 Les leviers

| variable | ce qu'elle fait | exactitude |
|---|---|---|
| `POMPPC_GL_WINSEED=1` (2 = contrôle) | la vérification de fenêtre (`direct_window_ok`) et « fenêtre cachée » (`drawable_hidden`) ne sont refaites en entier que si la **géométrie des fenêtres** a bougé : `CGSGeometrySeed()`, un mot de la mémoire partagée du WindowServer (lu sans message), et, quand elle existe, `CGSWindowVisibleRegionSeed(cid, wid)` (région visible de notre fenêtre ; refusée — 1010 — pour une fenêtre GLUT de la VM de dev : la géométrie vaut alors seule). Graines inchangées depuis une vérification complète **réussie** : ne sont revus que l'alpha des fenêtres invisibles qui touchaient notre rectangle (d'ordinaire aucune) et le curseur (même code qu'avant). `GetFrontProcess` reste demandé toutes les 10 images. Un refus n'est jamais repris (sauf un refus dû au seul curseur, dont la géométrie tient). | contrôle `=2` : la vérification complète est faite quand même et comparée — **0 écart** sur `swapbench` move (déplacements), cover (recouvrement), pop (fenêtre mise devant, **derrière** par `glutPushWindow` — l'ordre seul —, devant, retirée) : 21 + 149 + 133 reprises comparées, 41 + 298 + 266 « cachée » comparées |
| `POMPPC_GL_TEXDEFER=1` | `TEX_DESTROY` sans `flush()` : la destruction part avec la soumission suivante, dans l'ordre du flux (ce qui emploie la texture la précède, un `TEX_CREATE` qui reprend l'identifiant la suit) | flux identique hors le découpage en soumissions ; `plugmd5` (dont `texdelmid`) |
| `POMPPC_GL_DISPSEED=1` (2 = contrôle) | base, pas, profondeur et taille de l'écran (Q6) gardés tant que `CGSDisplayChangedSeed` et `CGSDisplayMappingChangedSeed` n'ont pas bougé — ce sont **les graines sur lesquelles CoreGraphics règle lui-même son cache d'écran** (désassemblage de `updateAllDisplayInfoAsNeeded`). Seulement quand l'hôte écrit l'image (`G.scanout` : SURF_PRESENT borné à la fenêtre visible, pas et profondeur refusés s'ils ne sont pas ceux de l'écran, Q3) ; relu de toute façon toutes les 10 images ; une lecture n'est retenue que si les graines n'ont pas bougé pendant | contrôle : relit et compare, **0 écart** (449 à 1 349 lectures par passe) |
| `POMPPC_GL_SWAPLITE=1` (2 = contrôle) | la ligne de `frames.csv` (trace : l'usage réel, lanceurs « trace » et matrice) formatée à la main au lieu de `fprintf` — même texte, `%.3f` arrondi sur la valeur exacte (`fmsub`), en 32 bits ; la boucle de `surftex_swapped` sur toutes les textures sautée tant qu'aucune texture de surface n'a existé | contrôle : `snprintf` aussi, comparé, **0 écart** (toutes les lignes de toutes les passes) |
| `POMPPC_GL_PRESENTQ=1` | présentation par l'hôte : l'image n est soumise **avant** d'attendre la n−1 (attente reportée au changement de moitié, même barrière) | rien ne lit l'image n−1 entre-temps quand l'hôte présente ; **neutre** en mesure (§3.4) |

Essayé puis retiré : **SWAPWIN**, un fil veilleur qui faisait les vérifications de fenêtre à
la place du fil du jeu (commit 01984aa). −13 à −15 % sur `swapbench` en fenêtre, mais +8 %
d'instructions par image (pour rester dans la borne de 10 images, il demande plus tôt, donc
plus souvent) ; WINSEED fait mieux sans fil.

### 3.3 Défaut corrigé au passage : `us_fill` reprenait un PTex libéré

`swapbench` avec une texture créée, employée et détruite à chaque image (`SWB_TEXDEL=1`)
faisait **planter le plugin `20261021-d3p` sans aucun levier** (SIGBUS dans
`gldUpdateDispatch` → `geom_ok` → `texturing_on` → `us_get` → `us_fill` → `tex_cp_fill` →
`tex_surface` sur l'objet de GLEngine libéré ; `POMPPC_GL_TEXMEMO=0` l'évitait, le rendu
d'Apple aussi). `us_fill` remettait `p->us_del` à l'époque courante à chaque unité parcourue,
même sans texture relue : le couple `(us_dt, us_t)` d'avant une destruction y survivait,
déclaré frais, et GLEngine redonne volontiers la même adresse à la texture suivante. La
mémoire d'une époque passée est maintenant effacée pour toute unité qui n'est pas relue.
Correctif sans variable (c'est un défaut) ; `plugmd5` et `gpu` inchangés.

### 3.4 Preuves et mesures (VM de dev `tiger-dev-plugin2.raw`, RTX, SMP 2, binaire rapide)

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

Ce que cela vaut dans les jeux, d'après les profils du §3.1 (à confirmer par l'A/B) : Marble
Blast ~5-8 % (vérifications de fenêtre 3,6 %, attente des soumissions en trop ~3 %, écran et
trace ~1 %), Zen ~3 % (écran 1,7 %, liste « cachée » 0,85 %, trace ~1 %), UT2004 et DOOM 3 de
l'ordre du pour cent.

Reste, non traité : `GetFrontProcess` toutes les 10 images (0,7 % de Zen), l'échange normal
de `DIRECT_REFRESH` (1,8 % de Marble Blast), `crash_hook_check` (deux `sigaction` par image,
0,3 %), la latence de l'hôte à l'échange (levier C).

### 3.5 A/B à jouer sur la VM quotidienne

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
