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
