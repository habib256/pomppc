# Protocole qgpu v8 — la fin du pipeline fixe

La v7 a confié à l'hôte la *géométrie* (matrices, éclairage, texgen, découpe, `DRAW_RAW`). Il
restait, entre elle et OpenGL 1.5, une poignée d'étages du pipeline fixe que le device ne savait
pas faire : le mélange à couleur constante et ses équations minimum/maximum, les opérations
logiques, les modes de polygone, les pointillés de ligne et de polygone, et les requêtes
d'occlusion. La v8 les ajoute (tâches 3.2, 3.3 et 3.7 de `docs/todo-gpu-3d.md`).

Règle inchangée à chaque version : **rien n'est retiré, aucune longueur de commande existante ne
change**. La v8 n'ajoute que des clés d'état et quatre opcodes ; toutes les valeurs initiales sont
celles d'OpenGL, et toutes sont neutres. Un flux v1–v7 rend exactement la même image sur un hôte
v8.

Une différence de nature avec la v7, qui explique le reste de ce document : **les clés v8 valent
pour TOUS les chemins de dessin**, l'ancien comme le brut. Les clés v7 ne servaient qu'à
`DRAW_RAW` parce qu'elles décrivent un étage *géométrique* que les anciens opcodes ont déjà
traversé chez GLEngine ; les clés v8, elles, agissent au *fragment* (mélange, opération logique,
pointillé de polygone) ou sur l'*assemblage des triangles* (mode de polygone, pointillé de ligne),
c'est-à-dire après l'endroit où les deux chemins se rejoignent.

---

## 1. Mélange : couleur constante, minimum et maximum

| Ajout | Où |
|---|---|
| `GL_CONSTANT_COLOR` (0x8001), `GL_ONE_MINUS_CONSTANT_COLOR` (0x8002), `GL_CONSTANT_ALPHA` (0x8003), `GL_ONE_MINUS_CONSTANT_ALPHA` (0x8004) | les **quatre clés de facteurs existantes** (`QGPU_SK_BLEND_SRC_RGB`, `_DST_RGB`, `_SRC_A`, `_DST_A`) |
| `QGPU_SK_BLEND_COLOR` (clé 77) | nouvelle, `0xAARRGGBB`, initialement 0 |
| `GL_MIN` (0x8007), `GL_MAX` (0x8008) | les **deux clés d'équation existantes** |

Aucun opcode nouveau : la couleur constante est une clé d'état comme la couleur de brouillard ou
celle d'environnement de texture, donc un simple `SET_STATE`.

**Avec `GL_MIN` et `GL_MAX`, les facteurs sont ignorés** — la sortie est `min(source, destination)`
ou `max(source, destination)`, canal par canal. C'est la lettre de la spécification, et c'est écrit
comme un court-circuit dans `blend()` (qgpu-soft.c), *avant* d'évaluer les facteurs, pour qu'on ne
puisse pas l'oublier en relisant. Le test `run_v8` (b) met les deux facteurs à `GL_ZERO` : avec
`GL_FUNC_ADD` le pixel serait noir, il ne l'est pas.

## 2. Opérations logiques

`QGPU_SK_LOGIC_OP` (booléen) et `QGPU_SK_LOGIC_OP_MODE` (les 16 opérations `GL_CLEAR` 0x1500 …
`GL_SET` 0x150F, contiguës, recopiées telles quelles ; initial `GL_COPY`).

Trois points de sémantique, tenus **identiquement** par les deux backends :

* **L'opération logique remplace le mélange.** La spécification dit que le mélange est ignoré
  quand elle est active ; le backend OpenGL coupe donc `GL_BLEND` explicitement plutôt que de s'en
  remettre au pilote, et le backend de référence n'appelle tout simplement pas `blend()`.
* **Elle travaille sur les quatre canaux de 8 bits du pixel.** Le backend de référence l'applique
  sur les 32 bits d'un coup (chaque bit est indépendant : c'est exact, et c'est plus court à lire
  que quatre boucles). L'alpha est inclus, comme en OpenGL.
* **Son résultat passe par le masque de couleur**, qui est le dernier étage.

`glClear` n'en dépend pas (il ne voit que les ciseaux et les masques) : les deux backends le
rendent explicite.

## 3. Modes de polygone

`QGPU_SK_POLYGON_MODE_FRONT` et `QGPU_SK_POLYGON_MODE_BACK` (`GL_POINT` 0x1B00, `GL_LINE` 0x1B01,
`GL_FILL` 0x1B02 ; initial `FILL`), plus `QGPU_SK_POLY_OFFSET_LINE` et `QGPU_SK_POLY_OFFSET_POINT`
(booléens — le facteur et les unités sont déjà là depuis la v4, et OpenGL les partage entre les
trois interrupteurs).

### Le sens des faces, et pourquoi il vaut aussi pour les anciens opcodes

Les modes de polygone s'appliquent aux triangles de `DRAW_RAW` **et** à ceux des
`DRAW_TRIANGLES*` antérieurs. Il a donc fallu donner un sens à « face avant » sur un chemin qui
n'en avait jamais eu besoin (les opcodes v1–v6 continuent, eux, de ne pas éliminer de faces : le
`CULL_FACE` de la v7 leur reste étranger).

La convention retenue est celle de la v7, lue à l'endroit : **la face avant est celle dont les
sommets tournent dans le sens trigonométrique TEL QU'ON LE VOIT** (quand `QGPU_SK_FRONT_FACE` vaut
`GL_CCW`).

* Chemin brut : `docs/protocole-v7-geometrie.md` §1 pose le sens des faces sur les coordonnées
  fenêtre **GL** (y vers le haut), *avant* le retournement que l'hôte applique pour ranger l'image
  dans le sens de la surface. Comme l'image finale est dans le sens de l'invité, « trigonométrique
  en fenêtre GL » et « trigonométrique à l'écran » sont la même chose.
* Chemin hérité : les sommets sont en pixels de surface, **y vers le bas**. Le lacet (formule du
  déterminant) y est donc de signe opposé à ce qu'on voit : le backend de référence le **nie**
  (`tri_polygon_mode`), et le backend OpenGL **inverse `glFrontFace`** — exactement ce que
  `gl_draw_raw` faisait déjà pour le chemin brut, et pour la même raison.

Un test le vérifie des deux côtés : le même triangle, sondé une fois par `DRAW_RAW` et une fois par
`DRAW_TRIANGLES`, est une face **arrière** dans les deux cas.

### Arêtes de contour : ce que la v8 tient, et l'écart assumé

Les **drapeaux d'arête ne sont pas transmis en v8**, et ne le seront pas : le code 6 du descripteur
de sortie de sommet de GLEngine (`cfg+0x11c`) *plante* le moteur si on le demande
(`docs/re/descripteur-de-sommet.md` §5). L'hôte reconstitue donc l'information lui-même, là où il
l'a.

* **`DRAW_RAW`, modes `GL_TRIANGLES`, `_STRIP`, `_FAN`** : les trois arêtes de chaque triangle sont
  du contour — c'est exact, un triangle n'a pas d'arête interne.
* **`DRAW_RAW`, modes `GL_QUADS`, `GL_QUAD_STRIP`, `GL_POLYGON`** : seules les arêtes du
  **contour** sont tracées, jamais les diagonales de la décomposition en triangles. Le backend de
  référence porte, à côté de chaque triangle, un masque de trois bits disant quelles arêtes sont
  d'origine ; ce masque **traverse la découpe** (Sutherland-Hodgman) selon les règles d'OpenGL —
  l'arête née d'un plan de découpe n'est jamais une arête de contour, un morceau d'arête d'origine
  le reste. Le backend OpenGL, lui, dessine ces modes **nativement** (`glDrawArrays(GL_QUADS, …)`),
  et `glPolygonMode` fait alors ce qu'il faut tout seul.
* **Écart assumé — les anciens opcodes.** `DRAW_TRIANGLES`, `_TEX`, `_TEX2`, `_TEXN` reçoivent des
  triangles déjà décomposés par GLEngine : l'information de contour du quadrilatère ou du polygone
  d'origine est **perdue avant d'arriver à l'hôte**. En mode `GL_LINE`, les trois arêtes de chaque
  triangle sont donc tracées, diagonales comprises. Les deux backends font la même chose, donc
  l'écart est stable et testable ; il n'est visible que pour un invité qui dessinerait des
  quadrilatères en fil de fer par le chemin hérité — c'est-à-dire un invité qui devrait passer au
  chemin brut.

### Décalage de polygone en mode ligne et point

Le décalage garde la **pente du polygone** (comme le veut la spécification) même quand ce sont des
lignes ou des points qui sont rastérisés : `poly_zoff()` a été extraite de `soft_tri` pour cela, et
le décalage est appliqué aux sommets *avant* de tracer. Le test dessine le classique fil de fer sur
face pleine coplanaire : sans `POLY_OFFSET_LINE` le fil échoue au test `LESS`, avec lui il passe.

## 4. Pointillés

### Ligne

`QGPU_SK_LINE_STIPPLE` (booléen), `QGPU_SK_LINE_STIPPLE_FACTOR` (1..256) et
`QGPU_SK_LINE_STIPPLE_PATTERN` (16 bits ; initial 0xFFFF). Le bit employé par un fragment est
`(compteur / facteur) & 15`.

Le **compteur** suit la règle d'OpenGL :

* remis à 0 au début de chaque `DRAW_RAW` (c'est le `glBegin`) ;
* remis à 0 **à chaque segment** pour `GL_LINES` et pour l'ancien `DRAW_LINES` ;
* **continu** le long d'un `GL_LINE_STRIP` et d'un `GL_LINE_LOOP` ;
* remis à 0 pour chaque arête d'un polygone tracé en mode `GL_LINE`.

Dans le backend de référence, le compteur d'un fragment vaut
`base + direction × (indice de pixel sur l'axe majeur − pixel de départ)` : le pointillé est un
simple test par fragment dans `soft_tri`, et non un découpage du segment. L'avance du compteur d'un
segment à l'autre est `round(|Δ sur l'axe majeur|)`, ce qui recoud exactement un ruban (le pixel de
jonction reçoit le même compteur par les deux segments).

### Polygone

`QGPU_SK_POLYGON_STIPPLE` (booléen) et l'opcode `QGPU_OP_SET_POLYGON_STIPPLE` (0x0060), **32 mots
de 32 bits** — la commande la plus longue du protocole, ce qui porte `QGPU_MAX_CMD_ARGS` de 26 à
32.

**Conventions de sens, le point à ne pas se tromper.**

* **En x**, le bit de **poids fort** du mot (bit 31) est la colonne `x = 0` du motif : c'est
  l'ordre de `glPolygonStipple`, dont le premier octet a son MSB à gauche (`GL_UNPACK_LSB_FIRST`
  faux). Colonne employée : `x mod 32`.
* **En y**, le **mot 0 est la ligne `yw = 0` de la coordonnée fenêtre OpenGL**, c'est-à-dire le
  **bas** de l'image — encore la convention de `glPolygonStipple`. Or le protocole range les
  surfaces avec la **ligne 0 en haut** (QuickDraw, cf. v7 §1). L'hôte fait donc la même couture
  qu'en v7 : la ligne de surface `ys` emploie le mot `(hauteur_de_la_surface − ys) mod 32`.

Ce choix a une conséquence pratique : le motif **ne dépend pas du chemin de dessin**. Le même motif
donne la même image par `DRAW_RAW` et par `DRAW_TRIANGLES`, ce qui ne serait pas vrai si on avait
indexé par la coordonnée fenêtre *native* de chaque chemin. La fonction `qgpu_stipple_row()` est
dans `qgpu-core.h` précisément pour que les deux backends ne puissent pas en avoir deux idées : le
backend OpenGL s'en sert pour **permuter les 32 lignes une fois** avant `glPolygonStipple` (dont
l'indexation, elle, est la ligne de surface, le FBO ayant sa ligne 0 en haut), et le backend de
référence pour choisir la ligne du motif au fil de la rastérisation.

Le pointillé de polygone ne vaut que pour les polygones **remplis** : en mode `GL_LINE` ou
`GL_POINT`, c'est le pointillé de ligne (ou rien) qui s'applique, comme en OpenGL.

Deux tests fixent les deux sens : un damier `0xAAAAAAAA`/`0x55555555` (le pixel est allumé quand
`x + ys` est pair, sur une surface de 64 lignes) pour la parité horizontale, et **une seule ligne
pleine, la quatrième**, pour le sens vertical — elle doit tomber sur la ligne de surface 28, et non
sur la 4.

## 5. Requêtes d'occlusion (OpenGL 1.5)

Trois opcodes et une limite :

| | |
|---|---|
| `QGPU_OP_QUERY_BEGIN` 0x0061 `[id]` | ouvre la requête et remet son compte à zéro |
| `QGPU_OP_QUERY_END` 0x0062 `[id]` | la ferme |
| `QGPU_OP_QUERY_RESULT` 0x0063 `[id, off]` | écrit à `off` dans BAR0 **deux mots big-endian** : disponible (0/1), puis le nombre d'échantillons passés, **saturé à 2³²−1** |
| `QGPU_MAX_QUERIES` = 64 | `QGPU_CLIENT_QUERY_IDS` = 16 par client |

**Le découpage par client est celui des textures.** `QGPU_CLIENT_TEX_IDS` découpe l'espace global
des textures en quatre tranches ; les requêtes font pareil (`query_base = index ×
QGPU_CLIENT_QUERY_IDS`), pour qu'aucun client ne puisse lire ni écraser la requête d'un autre. 16
requêtes en vol par client est très au-delà de ce que GLEngine demande — il n'en ouvre qu'une à la
fois.

**Une seule requête active par contexte.** Ouvrir une requête alors qu'une autre court, fermer une
requête qui ne court pas *sur ce contexte*, demander le résultat d'une requête jamais ouverte :
`QGPU_ST_BAD_ARG`. Un offset hors de la fenêtre ou mal aligné : `QGPU_ST_OOB`. Comme toujours, tout
cela est vérifié **dans le cœur**, une fois ; un backend ne revérifie rien. Détruire un contexte
referme la requête qu'il aurait laissée ouverte, sans quoi son identifiant resterait bloqué.

**Ce qui est compté** : tout fragment qui passe **tous** les tests — ciseaux, alpha, stencil,
profondeur —, que le masque de couleur soit ouvert ou fermé. C'est précisément l'usage : dessiner
une boîte englobante sans rien peindre. Un fragment tué par un pointillé n'est pas compté (le
pointillé fait partie de la rastérisation).

**Backends.** Le backend de référence incrémente un compteur au seul endroit où il sait qu'un
fragment a tout passé, juste avant l'écriture du pixel. Le backend OpenGL emploie
`GL_SAMPLES_PASSED` natif : `glGenQueries` / `glBeginQuery` / `glEndQuery` /
`glGetQueryObjectuiv`, résolus par `gl_proc` comme le reste (avec repli sur les noms `…ARB`). Le
device étant synchrone, `QUERY_RESULT` **attend** le résultat — `glGetQueryObjectuiv(…,
GL_QUERY_RESULT, …)` bloque, ce qui est exactement la sémantique attendue.

**Capacité.** Ces entrées sont **facultatives** : si l'hôte ne les a pas, `gl_init` n'annonce pas
`QGPU_CAP_OCCLUSION` (nouveau bit, 0x4) et le cœur refuse les opcodes `QUERY_*` avec
`QGPU_ST_BACKEND` — l'invité se replie, il ne plante pas. Le backend de référence, lui, l'annonce
toujours : c'est la vérité terrain des tests.

*Réglé en v10* : le device publie désormais `core.caps` (`docs/protocole-v10-textures.md` §5).
*Note d'origine* : `QgpuCore` porte désormais un champ `caps` (les
`QGPU_CAP_*` réellement tenus, backend plus ce que son `init()` a résolu à chaud), alors que
`qgpu-pci.c` publie encore `core.be->cap` dans `QGPU_REG_CAPS`. Une ligne à changer là-bas le jour
où l'invité devra *lire* la capacité d'occlusion ; rien ne dépend de ce changement aujourd'hui,
puisque le refus propre passe par le statut de la commande.

## 6. Écarts assumés du backend de référence

`qgpu-soft.c` reste la vérité terrain des tests, pas un chemin rapide. Aux écarts déjà listés en
v7 §5 s'ajoutent, pour la v8 :

* **Lignes et points sont des paires de triangles.** Le rasteriseur inclut les fragments dont le
  centre tombe **sur** une arête (`w >= 0`), donc un centre de pixel posé exactement sur la
  diagonale d'une décomposition est couvert par les deux triangles. C'est invisible au rendu (même
  couleur), mais une **requête d'occlusion le compterait deux fois**. Les tests choisissent donc
  des rectangles dont la diagonale ne passe par aucun centre de pixel — pour le rectangle
  (8,8)–(25,24), la fonction d'arête vaut `17j − 16i − 7,5`, qui n'est jamais nulle. Le backend
  OpenGL, lui, applique la règle d'arête d'OpenGL et ne compte jamais deux fois.
* **Ensemble de fragments d'un segment.** Le parallélogramme du backend de référence peut inclure
  un fragment de plus que la règle « sortie du losange » d'OpenGL, à l'extrémité finale d'un
  segment. Le **compteur de pointillé**, lui, suit la règle d'OpenGL exactement, de sorte que les
  pixels allumés et éteints tombent aux mêmes abscisses des deux côtés partout où la règle
  d'extrémité ne joue pas — c'est-à-dire partout sauf sur le dernier pixel, qui n'est jamais
  sondé, comme aucune arête ne l'est.
* **Pas de lissage** (`GL_LINE_SMOOTH`, `GL_POINT_SMOOTH`, `GL_POLYGON_SMOOTH`) : hors périmètre,
  de même que les sprites de points. Le reste de la tâche 3.2 (lissage) et la 3.5 restent à faire.

## 7. Preuve

`run_v8` dans `tests/qgpu_core_test.c`, sur les **deux backends**, sans jamais tester un pixel sur
une arête, avec une tolérance de ±2/255 là où un arrondi intervient : les quatre facteurs
constants ; `GL_MIN` et `GL_MAX` avec les facteurs à `ZERO` (donc ignorés) ; six opérations
logiques dont `XOR`, `INVERT`, `COPY_INVERTED` et `AND`, plus le mélange ignoré et le masque de
couleur respecté ; modes `GL_LINE` et `GL_POINT` sur un triangle (arêtes posées sur des centres de
pixels, largeur de ligne 3, sonde au milieu d'une arête) ; faces avant et arrière avec des modes
différents, par le chemin brut **et** par `DRAW_TRIANGLES` ; contour seul d'un `GL_QUADS` (la
diagonale, qui passerait par le centre du pixel (30,30), n'est pas tracée) ; décalage de polygone
en mode ligne, actif puis inactif ; pointillé de ligne `0x00FF` au facteur 2 aux bonnes abscisses,
et continuité sur une bande de deux segments ; pointillé de polygone en damier (parité) et à une
seule ligne pleine (sens vertical) ; requêtes d'occlusion — quad entièrement visible (272
échantillons, l'aire exacte), entièrement caché par la profondeur (0), coupé de moitié par les
ciseaux (136), masque de couleur fermé (compte inchangé), résultat relu deux fois — et onze
validations.

## 8. Ce que le plugin invité devra envoyer

Rien n'est obligatoire : sans une seule commande v8, le rendu est celui de la v7.

1. **Mélange** : recopier `GL_BLEND_COLOR` dans `QGPU_SK_BLEND_COLOR` (format `0xAARRGGBB`, comme
   toutes les couleurs du fil) quand l'application change `glBlendColor`, et laisser passer les
   quatre nouveaux facteurs et les deux nouvelles équations dans les clés qu'il remplit déjà. Aucun
   repli n'est plus nécessaire pour ces valeurs.
2. **Opération logique** : `QGPU_SK_LOGIC_OP` et `QGPU_SK_LOGIC_OP_MODE`, valeurs GL recopiées. Ne
   pas se donner la peine de couper le mélange : l'hôte le fait.
3. **Modes de polygone** : `QGPU_SK_POLYGON_MODE_FRONT` / `_BACK`, et les deux interrupteurs de
   décalage. **À n'employer que par le chemin brut si la scène dessine des quadrilatères ou des
   polygones en fil de fer** (cf. §3, l'écart des anciens opcodes) ; pour des triangles, les deux
   chemins sont équivalents.
4. **Pointillé de ligne** : les trois clés, telles que `glLineStipple` les donne. Le compteur est
   tenu par l'hôte, primitive par primitive : le plugin n'a rien à suivre.
5. **Pointillé de polygone** : `SET_POLYGON_STIPPLE` avec les 32 mots **dans l'ordre de
   `glPolygonStipple`** — mot 0 = première ligne du masque = bas de l'image, MSB à gauche. Le
   masque de GLEngine est un tableau de 128 octets dans ce même ordre : le plugin les empaquette
   quatre par quatre en big-endian et n'a **rien à retourner**. C'est l'hôte qui fait la couture
   avec le sens d'image du protocole.
6. **Requêtes d'occlusion** : identifiants dans `[query_base, query_base + QGPU_CLIENT_QUERY_IDS)`,
   où `query_base = index de tranche × QGPU_CLIENT_QUERY_IDS` (l'index vient de
   `QGPU_UC_GET_SLOT`, comme pour les textures). Une seule ouverte à la fois par contexte.
   `QUERY_RESULT` est **synchrone** : le résultat est là au retour du doorbell, et « disponible »
   vaut toujours 1 après un `QUERY_END` — le mot existe pour le jour où le device deviendra
   asynchrone (tâche 2.2), et le plugin doit déjà le lire plutôt que le supposer.
7. **Négociation** : `QGPU_CAP_OCCLUSION` dit si l'hôte tient les requêtes. Tant que `qgpu-pci.c`
   publie `core.be->cap` (cf. §5), ce bit n'apparaît pas dans `QGPU_REG_CAPS` ; en attendant, le
   plugin reconnaît l'absence au statut `QGPU_ST_BACKEND` d'un `QUERY_BEGIN` et se replie — c'est
   de toute façon ce qu'il doit savoir faire.
