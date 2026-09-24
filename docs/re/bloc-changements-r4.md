# Le bloc de changements de GLEngine, bit par bit (relevé R4, 24/09/2026)

Lot 3 du verdict unique (`TODO.md` §2, `docs/re/etude-court-circuit-glengine.md` §4-§5).
`gldUpdateDispatch` reçoit en troisième argument le **bloc de changements** `gctx+0x310` :
19 mots que GLEngine remplit entre deux dessins et remet à zéro après chaque dispatch
(`_gleDrawArraysOrElements_VBO_Exec`, 0xa7d40). La question du relevé R4 : **quel bit pose
chaque appel GL**, pour savoir quels dispatches peuvent reprendre le verdict gardé (lot 2) sans
le recalculer.

## 1. Méthode

- **Sonde** `POMPPC_GL_BLOCKDUMP=a[:n]` (plugin `20260924-liste`) : pour les images
  `[a, a+n)`, le plugin écrit sur **stderr** une ligne par dispatch (`BLOC dispatch k image i :
  +00=… +04=…`, mots non nuls seulement, et ce que la liste blanche en a fait) et une par dessin
  (`BLOC dessin …`). Sur stderr plutôt que dans la note : lancé avec `2>&1`, le programme de test
  intercale ses propres lignes **dans l'ordre des appels**.
- **Scène `gltest r4`** (`guest/gltest/gltest.c`) : un triangle en `glDrawArrays` depuis des VBO
  (positions, coordonnées de texture), texture 2D allumée sur l'unité 0 ; puis ~170 étapes qui
  font chacune **un seul** changement suivi d'un dessin, précédé de la ligne `R4 <appel>`. Une
  deuxième partie refait les appels sous programmes ARB de sommets et de fragments (env, local,
  liaison, attributs génériques, textures des unités 0 et 4). Témoins « rien » : un dessin sans
  changement ne provoque aucun dispatch.
  `POMPPC_GL_NOTE=/tmp/r4.note POMPPC_GL_BLOCKDUMP=0:1000000 ./gltest r4 > r4.log 2>&1`.
- **Dans les jeux** : `POMPPC_GL_COUNT=1` (lot 0) note maintenant aussi, toutes les 500 images,
  combien de dispatches ont posé **chaque bit** de chaque mot (`COUNT image N bits +0xNN : …`).

## 2. Ce que pose chaque appel

Mots notés par leur décalage dans le bloc (`+00` = `gctx+0x310`). Sans dispatch : l'appel ne
pose rien que GLEngine juge utile au pilote — le dessin suivant part sans `gldUpdateDispatch`.

### +0x00 — état de fragment et de rastérisation

| bit | appels |
|---|---|
| `00000001` | `glEnable/Disable(GL_ALPHA_TEST)`, `glAlphaFunc` |
| `00000002` | `glEnable/Disable(GL_BLEND)`, `glBlendFunc`, `glBlendEquation` |
| `00000008` | `glClearColor` |
| `00000080` | `glDrawBuffer` (changement réel ; `GL_BACK` sur `GL_BACK` : rien) |
| `00000200` | `glEnable/Disable(GL_DEPTH_TEST)`, `glDepthFunc` |
| `00000800` | `glFog*` (paramètres, `GL_FOG_COORDINATE_SOURCE`, mode), `glEnable(GL_COLOR_SUM)` ; avec `00800000` : `glEnable/Disable(GL_LIGHTING)` |
| `00001000` | `glEnable/Disable(GL_FOG)` |
| `00002000` | `glHint` (tous, `GL_FOG_HINT` compris) |
| `00004000` | `glLineWidth`, `glEnable/Disable(GL_LINE_SMOOTH)` |
| `00008000` | `glLineStipple` |
| `00010000` | `glEnable/Disable(GL_LINE_STIPPLE)` |
| `00020000` | `glEnable/Disable(GL_COLOR_LOGIC_OP)`, `glLogicOp` |
| `00040000` | `glColorMask` |
| `00080000` | `glDepthMask` |
| `00100000` | `glStencilMask` |
| `00200000` | `glPixelStorei` |
| `00400000` | `glPointSize`, `glEnable(GL_POINT_SMOOTH)`, `glPointParameterfv` |
| `00800000` | `glEnable(GL_CULL_FACE)`, `glCullFace`, `glFrontFace`, `glPolygonOffset` et son allumage, `glPolygonMode`, `glEnable(GL_POLYGON_SMOOTH)`, `glLightModeli` (avec `+0c 1`), allumage et extinction des programmes de sommets (avec `+0c`) |
| `01000000` | `glPolygonStipple` |
| `02000000` | `glEnable/Disable(GL_POLYGON_STIPPLE)` |
| `04000000` | `glEnable/Disable(GL_SCISSOR_TEST)`, `glScissor` (avec `+08 1`) |
| `08000000` | `glShadeModel` |
| `10000000` | `glEnable/Disable(GL_STENCIL_TEST)`, `glStencilFunc`, `glStencilOp`, `GL_STENCIL_TEST_TWO_SIDE_EXT`, `glStencilOp` de la face arrière |

### +0x04 — unités de texture

| bit | appels |
|---|---|
| `0000000n` (bit *u*) | **`glBindTexture` sur l'unité *u*** (même la texture déjà liée, même unité éteinte, même sous programme), `glTexParameteri`, `glTexImage2D`, `glTexSubImage2D` de la texture liée ; avec `+08 1<<(16+u)` : `glEnable/Disable(GL_TEXTURE_2D / CUBE_MAP)` |
| `00010000` | `glEnable/Disable(GL_TEXTURE_GEN_S)`, `glTexGeni` (unité 0) |

### +0x08 — transformation

| bit | appels |
|---|---|
| `00000001` | `glViewport`, `glScissor`, `glDepthRange`, `glDepthBoundsEXT`, allumage d'un programme de sommets |
| `00000008` | `glLoadMatrixf` sur `GL_PROJECTION` |
| `00000010` | `glLoadMatrixf` / `glLoadIdentity` sur `GL_MODELVIEW`, `glEnable(GL_LIGHTING)` |
| `00010000 << u` | matrice de texture de l'unité *u*, allumage d'une cible de texture de l'unité *u* |
| `01000000` | `glEnable/Disable(GL_CLIP_PLANE0)` |
| `40000000` | `glEnable/Disable(GL_NORMALIZE)` |

### +0x0c — éclairage, programmes, tableaux

| bit | appels |
|---|---|
| `00000001` | `glMaterialfv`, `glLightModeli`, extinction de `GL_COLOR_MATERIAL` |
| `00000002` | `glEnable/Disable(GL_LIGHTING)` |
| `00000004` | `glLightfv` |
| `00000008` | `glEnable/Disable(GL_LIGHT0)` |
| `00040000` | `glEnable/Disable(GL_COLOR_MATERIAL)` |
| **`00100000`** | **tableaux salis** (posé par GLEngine lui-même au dessin, 0xa7b40) : `glVertexPointer`, `glTexCoordPointer`, `glColorPointer`, `glVertexAttribPointerARB` (même VBO et même décalage compris), `glEnable/DisableClientState`, `glEnable/DisableVertexAttribArrayARB`, `glBufferDataARB` / `glBufferSubDataARB` d'un VBO en service |
| `00400000` | `glBindProgramARB(GL_VERTEX_PROGRAM_ARB)` (même le programme déjà lié), `glProgramLocalParameter` de sommets, extinction du programme de sommets |
| **`00800000`** | **`glProgramEnvParameter`** de sommets et de fragments, `glLoadMatrixf` sous programme (matrices suivies), liaison et `local` de sommets |
| `01000000` | `glBindProgramARB(GL_FRAGMENT_PROGRAM_ARB)`, `glProgramLocalParameter` de fragments, allumage et extinction du programme de fragments |
| **`02000000`** | `glProgramEnvParameter` de fragments |

### +0x10 — environnement de texture (unité *u* : bit *u*)

`glTexEnvi(GL_TEXTURE_ENV_MODE)` pose le bit de l'unité ; `glTexEnvfv(GL_TEXTURE_ENV_COLOR)`
`00000100` ; les paramètres du combinateur (`GL_COMBINE_RGB`…) `01000000`.

### +0x14 à +0x48 — valeurs de paramètres

Jamais posés seuls. `+0x14` et `+0x38` sont des masques de paramètres de programmes salis
(DOOM 3 pose `+14=00007fff +38=00000024` avec ses env de sommets et de fragments, un bit par
indice ; la liaison d'un programme pose `+14 00000020` ou `+38 00000002` avec le bit `+0c` de
liaison), les matrices suivies `+14 0000000f` ; `+0x14 00000001` et `+0x48 00000001`
accompagnent toute valeur de paramètre (lumière, matériau, brouillard, point, couleur
d'environnement, plage de profondeur, env/local).

### Aucun dispatch

`glActiveTexture`, `glClientActiveTexture`, `glActiveStencilFaceEXT`, `glColor4f`,
`glMatrixMode`, `glReadBuffer`, `glDrawBuffer` inchangé, **`glBindBufferARB` seul** (tableau
ou indices : c'est le `gl*Pointer` qui suit qui pose `+0c 00100000`), un dessin sans rien
changer.

## 3. Ce que le verdict lit, et la liste blanche qui en sort

Le verdict (lot 2) = `geom_ok` && `texture_ok`, `TexInfo`, `geom_format`, `va_gen_sizes` ; la clé
`vd_key_of` couvre ce qui bouge sans dispatch (image, époque des textures, état du plugin,
VAO et `VA_EN`, table des unités, drawable, étage de sommets, programmes courants et actifs).
Trois classes de bits, dans `wl_mask` (`guest/gldriver/pomppc_accel.c`) :

1. **Neutres** (l'état n'est lu par aucune des quatre fonctions) : `+00` `glClearColor`,
   masques de couleur, profondeur et pochoir, découpe, modèle d'ombrage (`WL_N0 = 0c1c0008`) ;
   `+08` fenêtre et plages `1`, projection `8`, modèle-vue `10` ; `+0c` env de sommets
   `00800000` et de fragments `02000000` ; `+0x14..+0x48` entiers.
2. **Rastérisation** (`WL_R0 = 13c3e203`) : lus par `accel_ok_for` et les lignes « points et
   lignes » de `geom_ok` (test alpha, mélange, profondeur, indications, lignes, opération
   logique, points, faces et modes de polygone, pointillé de polygone, pochoir). Admis à
   condition que **`geom_raster_ok`** (extrait de `geom_ok`, même code) tienne encore : ~20
   lectures au lieu du verdict entier. Un lissage de ligne ou de point allumé fait échouer
   `geom_raster_ok` : recalcul, et le verdict (non) recalculé n'est plus « ok gardé » pour la
   suite.
3. **Tableaux** `+0c 00100000` (`WL_GS`) : `geom_format` ne lit que `VA_EN` (dans la clé) ; la
   taille des génériques et leur source lisible, si — `va_gen_sizes` est refait seul.

Tout le reste **n'est pas neutre** et fait recalculer : unités (`+04`), allumage des cibles et
matrices de texture (`+08` bits 16-23), environnement de texture (`+10`), éclairage,
brouillard, couleur secondaire, texgen, plans de coupe, normalisation (`+00 800`, `+00 1000`,
`+0c` bas), programmes liés, allumés ou leurs `local` (`+0c 00400000`, `01000000`), tampon de
dessin (`+00 80`), `glPixelStorei` (`+00 00200000`, par prudence). En plus du bloc, la reprise
exige un verdict **ok** gardé et une **clé identique** (`wl_take`).

## 4. Les motifs de DOOM 3, lus

`demo_mars_city1`, plugin `20260924-liste`, `POMPPC_GL_COUNT=1` (5 premiers mots) :

| motif | quoi | liste blanche |
|---|---|---|
| `10800000 0 0 0 0` | volumes d'ombre : `glStencilOp`/`glStencilFunc` (`10000000`) et `glCullFace` (`00800000`) entre deux passes | court-circuité (rastérisation) |
| `0 00000032 0 02900000 0` | interaction : `glBindTexture` des unités 1, 4, 5 (bosselage, diffuse, spéculaire), env de sommets et de fragments, tableaux | recalculé (unités) |
| `10800000 0 0 00100000 0` | volume d'ombre suivant, nouveaux pointeurs | court-circuité |
| `0 0 0 00100000 0` | surface suivante, même état, nouveaux pointeurs | court-circuité |
| `0 00000032 00000010 02900000 0` | interaction d'une autre entité (modèle-vue) | recalculé (unités) |
| `14800000 0 00000011 00900000 0` | nouvelle lumière : rectangle de découpe, pochoir, faces, fenêtre, modèle-vue, env de sommets | court-circuité |
| `0 00000001 00000010 00900000 0` | liaison sur l'unité 0 (probablement le remplissage de profondeur), modèle-vue, env | recalculé (unité 0) |
| `0 0 00000010 00100000 0`, `0 0 00000010 02900000 0` | entité suivante, même matériau | court-circuité |

Prey (« Fuite à toute vitesse ») : en tête `00000001 0 0 00100000 0` (test alpha, tableaux),
court-circuité, puis les mêmes interactions `0 00000032 … 02900000` que DOOM 3.

Les motifs 1 et 3 du lot 0 (`0 1 10 00900000`, `0 32 0 02900000`) lient des textures à chaque
dessin : ils restent recalculés, et **aucun** dispatch « non neutre par les seules unités » n'a
retrouvé la table des unités telle qu'au rangement (0 sur 206 407 dans une tranche de
500 images) — DOOM 3 ne relie jamais la même texture ; une piste « unités inchangées » ne
rapporterait rien.
