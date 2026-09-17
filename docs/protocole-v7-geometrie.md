# Protocole qgpu v7 — la géométrie sur l'hôte

Jusqu'à la v6, l'invité envoyait au device des sommets **déjà transformés, éclairés, découpés et
aplatis** par GLEngine sur le PowerPC émulé : x, y en pixels, z fenêtre, couleur finale,
coordonnées de texture déjà divisées par w. C'est ce travail-là qui limite aujourd'hui les jeux
(cf. `docs/todo-gpu-3d.md`, axe 1). La v7 ajoute de quoi confier **tout le pipeline fixe
d'OpenGL 1.x** à l'hôte : matrices, viewport, éclairage, matériaux, génération de coordonnées de
texture, plans de découpe, et un dessin indexé de sommets **bruts**.

Règle inchangée à chaque version : **rien n'est retiré, aucune longueur de commande existante ne
change**. Les opcodes de dessin v1–v6 ignorent tout l'état ci-dessous et gardent exactement leur
sémantique ; un flux v6 s'exécute tel quel sur un hôte v7.

## 1. Conventions de repère et sens de l'image

C'est le point où une erreur coûte une journée, donc il est écrit une fois pour toutes.

* **Le chemin existant** (`DRAW_TRIANGLES*`, `DRAW_LINES`, `DRAW_POINTS`) travaille en **pixels de
  surface** : origine en haut à gauche, y vers le bas — la convention QuickDraw/Quartz, celle que
  GLEngine produit, celle dans laquelle l'invité relit la surface.
* **Le chemin brut** (`DRAW_RAW`) travaille en **coordonnées OpenGL** : l'invité recopie ses
  matrices et son viewport tels que l'état GL les lui donne, donc en repère GL, viewport
  d'origine **en bas** à gauche, y vers le haut.
* **La couture est faite par l'hôte** : une coordonnée fenêtre GL `yw` devient la ligne de surface
  `hauteur − yw`. Le viewport est donc rapporté au **bas** de la surface, et les deux chemins
  produisent une image **dans le même sens**. L'invité n'a rien à retourner, ni à l'aller ni au
  retour.
* **Le sens des faces** (`QGPU_SK_FRONT_FACE`, `QGPU_SK_CULL_*`) est établi sur les coordonnées
  fenêtre **GL**, donc *avant* ce retournement : `GL_CCW` veut dire la même chose côté invité et
  côté hôte. (Le backend OpenGL inverse `glFrontFace` pour compenser son propre retournement ;
  c'est un détail interne, invisible du protocole.)
* **Profondeur** : `DEPTH_RANGE` est `glDepthRange`, deux flottants dans [0,1], initialement 0 et 1.
  `zw = n + (zd+1)/2 · (f − n)`, borné à [0,1], comme en OpenGL.

### La projection « pixels »

Pour dessiner en pixels par le chemin brut — c'est-à-dire pour reproduire exactement le chemin
existant — il suffit de poser modèle-vue = identité et projection = `glOrtho(0, w, h, 0, 0, −1)`,
la projection que le plugin utilise déjà. Un sommet `(x, y, z)` y donne le pixel `(x, y)` et la
profondeur fenêtre `z`. Le test `(a)` de `tests/qgpu_core_test.c` dessine le même triangle
asymétrique par les deux chemins et vérifie que les mêmes pixels sont peints, dans le même sens.

## 2. Ce que l'invité envoie, et dans quel ordre

Rien n'est obligatoire : chaque commande met à jour une part de l'état du contexte courant, qui
persiste jusqu'au prochain changement. L'ordre naturel dans une soumission est :

1. `CTX_BIND`, `SURF_BIND` (inchangé — le device reste mono-contexte courant) ;
2. `SET_MATRIX` pour la modèle-vue, la projection, et les matrices de texture des unités
   employées ;
3. `VIEWPORT` et `DEPTH_RANGE` si l'invité ne veut pas de la surface entière ;
4. `SET_STATE` pour l'éclairage, l'ombrage, l'élimination des faces, le color material, le modèle
   d'éclairage, le brouillard (clés ci-dessous) ;
5. `SET_LIGHT` par lumière changée, `SET_MATERIAL` par face, `SET_LIGHT_MODEL` pour l'ambiante
   globale ;
6. `SET_TEXGEN` par (unité, coordonnée) changée, `SET_CLIP_PLANE` par plan ;
7. `SET_CURRENT` pour les attributs que le format de sommet n'emporte pas ;
8. `DRAW_RAW`.

### Coordonnées œil : ce que l'invité ne doit *pas* retransformer

OpenGL transforme certaines valeurs **au moment de l'appel**, par la modèle-vue courante ; c'est
la valeur transformée que GLEngine range dans son état. Le protocole prend donc, telles quelles,
les valeurs **déjà en coordonnées œil** :

* `SET_LIGHT` : la **position** (4 composantes) et la **direction de spot** (3) ;
* `SET_TEXGEN` : le **plan œil** (le plan objet, lui, n'est jamais transformé) ;
* `SET_CLIP_PLANE` : l'**équation**.

Autrement dit, l'invité recopie ce qu'il lit dans l'état GL et ne multiplie rien par la
modèle-vue. Le backend OpenGL repose ces valeurs avec une modèle-vue **identité**, puis charge la
vraie modèle-vue : le résultat est exactement celui de l'application invitée.

### Matrices

`SET_MATRIX [quelle, m0..m15]` — 16 flottants big-endian dans **l'ordre colonne** d'OpenGL
(`m0..m3` = première colonne), c'est-à-dire la disposition que `glLoadMatrixf` attend et que
GLEngine range en mémoire. L'invité recopie, il ne transpose pas. `quelle` :
`QGPU_MTX_MODELVIEW`, `QGPU_MTX_PROJECTION`, `QGPU_MTX_TEXTURE0 + u` (u de 0 à 3).

### Clés d'état ajoutées

| Clé | Domaine | Valeur initiale |
|---|---|---|
| `QGPU_SK_LIGHTING` | booléen | 0 |
| `QGPU_SK_NORMALIZE` | booléen (`GL_NORMALIZE`) | 0 |
| `QGPU_SK_RESCALE_NORMAL` | booléen ; ignoré si `NORMALIZE` | 0 |
| `QGPU_SK_SHADE_MODEL` | `GL_FLAT` / `GL_SMOOTH` | `GL_SMOOTH` |
| `QGPU_SK_CULL_FACE` | booléen | 0 |
| `QGPU_SK_CULL_MODE` | `GL_FRONT`, `GL_BACK`, `GL_FRONT_AND_BACK` | `GL_BACK` |
| `QGPU_SK_FRONT_FACE` | `GL_CW` / `GL_CCW` | `GL_CCW` |
| `QGPU_SK_COLOR_MATERIAL` | booléen | 0 |
| `QGPU_SK_COLOR_MAT_FACE` | trois faces | `GL_FRONT_AND_BACK` |
| `QGPU_SK_COLOR_MAT_MODE` | `EMISSION`, `AMBIENT`, `DIFFUSE`, `SPECULAR`, `AMBIENT_AND_DIFFUSE` | `AMBIENT_AND_DIFFUSE` |
| `QGPU_SK_LOCAL_VIEWER` | booléen | 0 |
| `QGPU_SK_TWO_SIDE` | booléen | 0 |
| `QGPU_SK_COLOR_CONTROL` | `GL_SINGLE_COLOR` / `GL_SEPARATE_SPECULAR_COLOR` | `SINGLE_COLOR` |
| `QGPU_SK_FOG_MODE` | `QGPU_FOG_VERTEX`, `GL_LINEAR`, `GL_EXP`, `GL_EXP2` | `QGPU_FOG_VERTEX` |
| `QGPU_SK_FOG_DENSITY` / `_START` / `_END` | flottants | 1, 0, 1 |

**Brouillard, et la compatibilité.** `QGPU_SK_FOG_MODE` vaut initialement `QGPU_FOG_VERTEX` : le
facteur vient du sommet, exactement comme en v4 — c'est ce qui garde les flux v4–v6 valides. Avec
`GL_LINEAR`, `GL_EXP` ou `GL_EXP2`, **et seulement pour `DRAW_RAW`**, l'hôte calcule le facteur :
depuis la coordonnée de brouillard du sommet si le format en emporte une (`GL_FOG_COORDINATE`),
sinon depuis `|z œil|` (`GL_FRAGMENT_DEPTH`). Les opcodes antérieurs ignorent ce mode et gardent
le facteur par sommet, quelle que soit la valeur de la clé.

## 3. Format de sommet

`DRAW_RAW [mode, n, voff, pas, format, ioff, itype, premier, nverts]`

* `mode` : un des dix modes d'OpenGL (`QGPU_PRIM_MODE_POINTS` … `_POLYGON`, valeurs GL recopiées) ;
* `n` : nombre de sommets dessinés (`itype = NONE`) ou d'indices lus ;
* `voff` : offset des sommets dans BAR0, multiple de 4 ;
* `pas` : pas d'un sommet **en mots** ; 0 = serré. Un pas inférieur au format vaut `BAD_ARG` ;
* `format` : masque `QGPU_VF_*` ;
* `ioff`, `itype` : indices — `QGPU_IDX_NONE`, `_U16` ou `_U32`, big-endian ;
* `premier` : comme `glDrawArrays` ; doit valoir 0 quand des indices sont donnés ;
* `nverts` : **nombre de sommets présents** dans le tableau. C'est contre lui que les indices sont
  validés. Il est explicite plutôt que déduit parce qu'un tableau indexé ne dit pas sa taille, et
  parce que c'est la seule façon de garantir qu'un indice fautif ne fait jamais lire hors de BAR0.

Les attributs sont des flottants big-endian, dans un **ordre fixe**, chacun n'occupant de la place
que s'il est présent :

| Ordre | Attribut | Mots | Bit |
|---|---|---|---|
| 1 | position | 2, 3 ou 4 | champ de 2 bits (`QGPU_VF_POS(n)`), toujours présente |
| 2 | normale | 3 | `QGPU_VF_NORMAL` |
| 3 | couleur | 4 | `QGPU_VF_COLOR` |
| 4 | couleur secondaire | 3 | `QGPU_VF_SEC_COLOR` |
| 5 | coordonnée de brouillard | 1 | `QGPU_VF_FOG` |
| 6-9 | coordonnées de texture, unités 0 à 3 | 4 chacune | `QGPU_VF_TEX(u)` |

`QGPU_VF_WORDS(format)` donne la taille. Une position à 2 composantes complète `z = 0`, `w = 1` ;
à 3, `w = 1`. Un attribut absent prend la **valeur courante** posée par
`SET_CURRENT [quoi, x, y, z, w]` (`QGPU_CUR_NORMAL`, `_COLOR`, `_SEC_COLOR`, `_FOG`,
`_TEXCOORD0 + u`) — l'équivalent de `glNormal` / `glColor` / `glTexCoord` hors tableau. Valeurs
initiales : normale (0,0,1), couleur (1,1,1,1), secondaire (0,0,0), brouillard 1,0 (« pas de
brouillard », convention du fil), coordonnées (0,0,0,1).

## 4. Validation

Règle du projet : **toute validation se fait dans le cœur, une fois ; un backend ne revérifie
rien.** Pour la v7, le cœur refuse (`QGPU_ST_BAD_ARG`, ou `QGPU_ST_OOB` pour un accès hors de la
fenêtre partagée) :

* un mode inconnu, un `itype` inconnu, un bit de format inconnu, une position à 5 composantes,
  un pas plus petit que le format ;
* un indice ≥ `nverts` — jamais de lecture hors de BAR0 ;
* un NaN ou un infini, partout : sommets, matrices, lumières, matériaux, plans ;
* les bornes d'OpenGL : indice de lumière ≥ 8, plan de découpe ≥ 6, unité ≥ 4, coordonnée de
  texgen ≥ 4, exposant de spot hors [0,128], angle de coupure hors [0,90] ∪ {180}, atténuation
  négative, brillance hors [0,128], `SPHERE_MAP` sur R ou Q, `NORMAL_MAP`/`REFLECTION_MAP` sur Q.

Le cœur remet aussi les sommets **à plat** (serrés, hôte-natifs) et élargit les indices en
`uint32_t` avant d'appeler le backend : un backend reçoit des données déjà propres.

`QGPU_MAX_CMD_ARGS` (26) nomme la longueur d'argument maximale — `SET_LIGHT` est la plus longue
commande du protocole. La limite de 8 mots de `qgpu_core_execute` a été remplacée par cette
constante, nommée dans le protocole pour que l'hôte et l'invité ne puissent pas en avoir deux
idées.

## 5. Écarts assumés du backend de référence

`qgpu-soft.c` est la vérité terrain des tests, pas un chemin rapide. Il produit des sommets au
format interne existant et réutilise `soft_tri` / `soft_line` / `soft_point` : **un seul
rasteriseur**, donc un seul jeu de règles de remplissage entre les deux chemins. Ses écarts
connus avec un vrai OpenGL :

* **Interpolation des couleurs sans correction de perspective.** Le rasteriseur interpole les
  couleurs linéairement en espace écran (les coordonnées de texture, elles, passent en `s/w`,
  `t/w`, `r/w`, `q/w` et restent correctes). En perspective forte, un dégradé peut différer de
  quelques unités du backend GL. Les tests en perspective utilisent des couleurs uniformes ou
  l'ombrage plat.
* **Brouillard par sommet.** Le facteur est calculé au sommet et interpolé ; OpenGL le calcule
  généralement par fragment. Identique quand la primitive est à profondeur œil constante, ce que
  les tests garantissent.
* **Niveau de détail des mipmaps constant par triangle** (limite héritée de la v3, inchangée).
* **Lignes et points non texturés** dans les deux chemins (comme en v4 ; les lignes et points
  texturés sont la tâche 3.5).
* **`GL_RESCALE_NORMAL`** est implémenté à la lettre de la spécification (1/‖3ᵉ ligne de l'inverse
  de la modèle-vue‖) ; il n'est pas comparé au backend GL, dont les pilotes divergent sur ce point.
  `GL_NORMALIZE`, lui, est testé des deux côtés.

Le backend OpenGL, lui, remet l'état GL à plat après chaque dessin brut (lumières, texgen,
matrices de texture, plans de découpe, color material, color sum, élimination des faces, tableaux
clients) : le chemin existant ne voit jamais rien de l'étage géométrique.

## 6. Preuve

`run_v7` dans `tests/qgpu_core_test.c`, sur les deux backends, sans jamais tester un pixel sur une
arête : équivalence des deux chemins et sens de l'image, modèle-vue (translation, rotation) et
projection en perspective, les dix modes de primitives — non indexés puis en u16 et u32, image
identique au pixel près —, sommet provoquant en ombrage plat pour `TRIANGLES`, `TRIANGLE_STRIP` et
`QUADS`, élimination des faces avant/arrière et sens des faces, viewport, `DEPTH_RANGE`, éclairage
(diffuse directionnelle sur face inclinée, ponctuelle avec atténuation, spot dedans et dehors,
spéculaire, observateur local, color material, deux faces, normalisation sous matrice d'échelle —
valeurs attendues calculées à la main dans les commentaires, tolérance ±2), texgen
`OBJECT_LINEAR`, `EYE_LINEAR` et `SPHERE_MAP` plus matrice de texture, couleur secondaire, plan de
découpe utilisateur et découpe par le plan proche d'un triangle traversant l'œil, brouillard
`LINEAR` et `EXP` calculés par l'hôte, valeurs courantes, multitexture brute sur deux unités en
`GL_COMBINE`, stencil et profondeur avec `DRAW_RAW`, et onze validations.

## 7. Ce qui reste (tâche 1.4)

Le plugin invité doit maintenant : poser l'octet `+0x79` du bloc de configuration pour que
GLEngine lui remette la géométrie brute (tâche 1.1), lire les matrices, lumières, matériaux,
texgen et plans de découpe dans l'état GLEngine aux offsets relevés dans
`docs/re/tableaux-de-sommets.md`, les transmettre par les opcodes ci-dessus, et convertir les
descripteurs de tableaux de sommets en `format` + `pas` + `voff`. Hors domaine — un format
d'attribut que la v7 ne porte pas, un état non géré —, le repli sur le chemin v6 reste exact :
c'est toujours le même device, le même contexte et la même surface.
