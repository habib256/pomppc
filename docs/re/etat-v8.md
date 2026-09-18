# État GLEngine des fonctions v8 — mélange constant, opération logique, modes de polygone, pointillés, requêtes (Tiger 10.4.6)

Relevé le **18/09/2026**. Deux méthodes, et elles se recoupent :

* **par l'expérience**, scène `v8probe` de `guest/gltest` — un réglage GL par `glClear`, l'état de
  GLEngine vidé à chaque effacement (`POMPPC_GLTRACE_STATE=1`), puis diff des vidages par
  `tools/re/diffstate.py diff … --tag clear-glstate` ; rendu d'Apple seul
  (`POMPPC_GL_DISABLE=1`), 64×64 hors écran ;
* **par lecture**, désassemblage annoté de `GLEngine` et de `GLDriver` (`tools/re/ppcanno.py`)
  pour les requêtes d'occlusion, qui ne se voient pas dans le bloc d'état.

Chaque offset du §1 a été **posé deux fois avec des valeurs différentes**, et relu par `glGet`
dans la même exécution : ce n'est ni une coïncidence ni une déduction.

Convention : `GS` = base du bloc d'état GL de GLEngine (`drvctx+0x0c`), comme dans
`docs/re/etat-tcl.md`.

---

## 1. Les offsets, et la preuve de chacun

| Offset | Type | Champ | Ce que le diff a montré |
|---|---|---|---|
| `GS+0x2d70` | f32 ×4 | **couleur de mélange** (`glBlendColor`), ordre R, G, B, A | `00000000…` → `3e000000 3e800000 3ec00000 3f000000` (0,125 / 0,25 / 0,375 / 0,5), puis → `3f400000 3f600000 3d800000 3e800000` |
| `GS+0x2d68`…`0x2d6e` | u16 ×4 | facteurs de mélange (déjà connus, v2) — ils **acceptent** les quatre constantes | `0001 0000 0001 0000` → `8001 8004 8001 8004` après `glBlendFunc(GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_ALPHA)` |
| `GS+0x2d80` / `0x2d82` | u16 ×2 | équations (déjà connues, v2) — elles **acceptent** `GL_MIN` et `GL_MAX` | `8006 8006` → `8007 8007` → `8008 8008` |
| **`GS+0x2e30`** | u16 | **opération logique** (`glLogicOp`) | `1503` (GL_COPY) → `1506` (XOR) → `150e` (NAND) |
| `GS+0x2e33` | u8 | `GL_COLOR_LOGIC_OP` (déjà connu) | `00` → `01` |
| **`GS+0x2e26`** | u16 | **facteur du pointillé de ligne** (1..256) | `0001` → `0003` → `0061` (97) |
| **`GS+0x2e28`** | u16 | **motif du pointillé de ligne** | `ffff` → `a5a5` → `1234` |
| `GS+0x2e2c` | u8 | `GL_LINE_STIPPLE` (déjà connu) | `00` → `01` |
| **`GS+0x30e8`** | 128 o | **motif de pointillé de polygone**, 32 lignes × 4 octets | `ff`×128 → `40 41 42 … bf` → `bf be bd … 40`, c'est-à-dire **exactement les octets passés à `glPolygonStipple`, dans l'ordre** |
| `GS+0x3178` | u8 | `GL_POLYGON_STIPPLE` (déjà connu) | `00` → `01` |
| `GS+0x3170` / `0x3172` | u16 ×2 | modes de polygone avant / arrière (déjà connus) | avant `1b02` → `1b01`, arrière `1b02` → `1b00` |
| `GS+0x317b` / `0x317c` | u8 | `GL_POLYGON_OFFSET_POINT` / `_LINE` (déjà connus) | `00` → `01` chacun |
| `GS+0x3168` / `0x316c` | f32 ×2 | facteur et unités de décalage (déjà connus) | `0` → `40200000 40505…` (2,5 et 3,25) |
| `GS+0x317e` | u8 | dérivé « les deux faces diffèrent » | passe à 1 dès que les deux modes de polygone diffèrent |
| `GS+0x4c80` | 3 o | dérivé, suit le mode de polygone | non identifié, **[H]** ; le plugin ne s'en sert pas |

Recoupement par `glGet` dans la même exécution : `GL_BLEND_COLOR = 0.75 0.875 0.0625 0.25`,
`GL_LOGIC_OP_MODE = 0x150e`, `GL_LINE_STIPPLE_REPEAT = 97`, `GL_LINE_STIPPLE_PATTERN = 0x1234`,
`GL_POLYGON_MODE = 0x1b01 0x1b00`, `glGetError = 0`.

### Bloc prêt à recopier

```c
#define GS_BLEND_COLOR    0x2d70   /* float ×4 : R, G, B, A */
#define GS_LOGIC_OP_MODE  0x2e30   /* u16 : GL_CLEAR 0x1500 … GL_SET 0x150F */
#define GS_LINE_STIP_FACT 0x2e26   /* u16 : 1..256 */
#define GS_LINE_STIP_PAT  0x2e28   /* u16 */
#define GS_POLY_STIP_MASK 0x30e8   /* 128 octets, ordre de glPolygonStipple */
```

### Le sens du motif de polygone

Le motif est rangé **tel que `glPolygonStipple` l'a donné** : l'octet 0 est la première ligne du
masque, donc le **bas** de l'image en coordonnée fenêtre OpenGL, et le bit de poids fort de chaque
octet est la colonne `x = 0`. Le PowerPC étant gros-boutiste comme le protocole, une lecture de 32
bits en `GS+0x30e8 + 4k` donne déjà le mot que `QGPU_OP_SET_POLYGON_STIPPLE` attend : **ni
retournement, ni échange d'octets**.

**Mais il faut décaler d'une ligne.** Le protocole (`docs/protocole-v8-pipeline-fixe.md` §4) dit
que la ligne de surface `ys` emploie le mot `(hauteur − ys) mod 32`, alors que la coordonnée
fenêtre OpenGL de cette ligne est `hauteur − 1 − ys`. Le plugin envoie donc `motif[(j−1) mod 32]`
au mot `j`.

> **Mesuré** (scène `stipple`, motif de 16 lignes allumées sur 32, quadrilatère de 64 de large sur
> 96 de haut) : sans le décalage, **exactement 384 composantes** — les six lignes de changement de
> bande, sur 64 pixels, en RGB — diffèrent du rendu d'Apple, avec un écart de 255/255 ; avec, le
> diff tombe à **0/255 sur toute l'image**. Le motif en **colonnes**, lui, était juste dès le
> départ : c'est bien un décalage vertical d'une ligne, et pas une erreur de sens.

---

## 2. Ce que le rendu d'Apple fait de ces états

| Fonction | `GLDriver` d'Apple seul | Chez nous (v8) |
|---|---|---|
| mélange à couleur constante, `GL_MIN`/`GL_MAX` | exact (rastériseur logiciel) | **accéléré**, image identique au rendu d'Apple (0/255 sur toute l'image) |
| opération logique | exact | **accélérée**, 0/255 |
| pointillé de polygone | exact | **accéléré**, 0/255 |
| pointillé de ligne | exact | **accéléré** par le chemin brut, 0/255 |
| modes de polygone | exact | **accélérés par le chemin brut seulement** (§3) |
| requêtes d'occlusion | **rien** : les entrées `gld*` sont des bouchons, l'application lit la valeur qu'elle avait mise dans sa variable (mesuré : 0) | **tenues entièrement par le plugin** |

---

## 3. Modes de polygone : pourquoi le chemin brut seulement

Sur le **chemin brut**, l'hôte reçoit le quadrilatère ou le polygone tel quel (`DRAW_RAW`,
`GL_QUADS`), et `glPolygonMode` y fait ce qu'il faut : seules les arêtes du contour sont tracées.
Sur le **chemin hérité**, GLEngine a déjà décomposé la primitive en triangles avant de nous
l'envoyer : l'information de contour est perdue *avant* d'arriver au plugin, et les diagonales de
la décomposition seraient tracées (`docs/protocole-v8-pipeline-fixe.md` §3, « écart assumé »).

**Décision, prise par le test** : `accel_ok_for()` n'accepte un mode de polygone autre que
`GL_FILL` que pour le chemin brut ; sur l'ancien chemin, le rendu d'Apple reprend la main et
l'image est exacte. Scène `polymode` :

| | témoins | image entière vs rendu d'Apple |
|---|---|---|
| chemin hérité (`POMPPC_GL_GEOM=0`, donc repli d'Apple) | OK | **0/255** |
| chemin brut | OK | **0/255** |

Deux pièges relevés en chemin :

* **La fusion des dessins détruisait le contour.** Le plugin recolle les `DRAW_RAW` consécutifs en
  `GL_TRIANGLES` indexés ; en mode `GL_LINE`, cela faisait tracer les diagonales des
  quadrilatères — 3 % de l'image fausse. Le plugin coupe donc la fusion en triangles quand le mode
  de polygone n'est pas `GL_FILL` (`G.pend_wire`).
* **Une arête posée sur une frontière de pixels** (un `y` entier) peut légitimement tomber sur
  l'une ou l'autre des deux lignes voisines, et GLEngine et l'hôte ne choisissent pas la même :
  GLEngine la ligne du dessus, l'hôte celle du dessous. C'est dans la latitude qu'OpenGL laisse à
  la rastérisation des lignes. Dès que les arêtes sont au **centre** des pixels (coordonnées en
  `.5`), les deux images coïncident au bit près — c'est ce que vérifie la scène `polymode`. La
  scène `mixte`, qui pose son triangle en fil de fer sur des coordonnées entières, regarde donc
  les **deux** lignes possibles.

---

## 4. Requêtes d'occlusion : comment elles atteignent le pilote

Relevé par **lecture** (`GLEngine`, adresses du binaire 10.4.6 PPC), puis confirmé dans l'invité.

### 4.1 Les cinq points d'entrée

GLEngine tient l'objet de requête lui-même — `_gleCreateQueryObject` (0xe1450) alloue 0x30 octets,
`+0x04` le nom, `+0x08` le compte de références, `+0x0c` le destructeur, et
**`+0x10 + 4·i` la poignée du renderer `i`**. Il ne demande au pilote que :

| Appel de GLEngine | Adresse | Entrée du pilote | Signature relevée |
|---|---|---|---|
| `_gleCreateQueryObject` | `0xe14e0 : lwz r12, 0x1c8(r2)` | `plugin+0x1c8` = **`gldCreateQuery`** (n° 45) | `(drvctx, unsigned long *poignée)` — écrit la poignée |
| `_gleFreeQueryObject` | `0xe1414 : lwz r12, 0x1cc(r2)` | `plugin+0x1cc` = **`gldDestroyQuery`** (n° 46) | `(drvctx, poignée)` |
| `_glBeginQuery_Exec` | `0x78cfc : lwz r12, 0x4700(r29)` | `ctx+0x4698 + 0x68` = **procédure +0x68** | `(drvctx, poignée)` |
| `_glEndQuery_Exec` | `0x78fd0 : lwz r12, 0x4704(r30)` | `ctx+0x4698 + 0x6c` = **procédure +0x6c** | `(drvctx, poignée)` |
| `_glGetQueryObjectuiv_Exec` | `0x79390 : lwz r12, 0x4810(r30)` | `ctx+0x4754 + 47·4` = **`gldGetQueryInfo`** (n° 47) | `(drvctx, poignée, nom, unsigned long *sortie)`, en saut terminal |

(`ctx+0x4754 + 4n` est la copie locale de la table `gld*`, `docs/re/capacites-glengine.md` §4.1 ;
`ctx+0x4698` est la table des procédures de rastérisation, §5.3. `0x1c8 = 0x114 + 45·4` et
`0x4810 = 0x4754 + 47·4` : les numéros tombent juste, ce n'est pas une coïncidence.)

### 4.2 Ce que GLEngine vérifie lui-même

`_glBeginQuery_Exec` (0x78a18) refuse avant d'appeler le pilote :

* `GL_INVALID_OPERATION` (0x502) si l'on est entre `glBegin` et `glEnd` (`ctx+0x4a10 ≥ 0`), si une
  requête court déjà (`ctx+0x7444 ≠ 0`) ou si l'identifiant est 0 ;
* `GL_INVALID_ENUM` (0x500) si la cible n'est pas `GL_SAMPLES_PASSED` (0x8914).

`_glGetQueryObjectuiv_Exec` (0x79324) n'accepte que `GL_QUERY_RESULT` (0x8866) et
`GL_QUERY_RESULT_AVAILABLE` (0x8867), et refuse la requête encore ouverte. **Il n'y a aucune
garde sur le bit d'extension** : les points d'entrée fonctionnent que `GL_ARB_occlusion_query`
soit annoncée ou non.

### 4.3 Ce que le rendu d'Apple en fait : rien

```
_gldCreateQuery:   li r0,0 / li r3,0 / stw r0,0(r4) / blr
_gldDestroyQuery:  li r3,0 / blr
_gldGetQueryInfo:  li r3,0 / blr
```

et `gldInitDispatch` du `GLDriver` **n'installe rien** en `+0x68` ni `+0x6c` (0x2f78-0x3198 : la
liste des emplacements écrits ne les contient pas).

**Mesuré dans l'invité** (scène `qprobe`, `POMPPC_GL_DISABLE=1`) : `glGenQueries`, `glIsQuery`,
`glBeginQuery`, `glEndQuery`, `glGetQueryObjectuiv` et `glDeleteQueries` existent tous dans
`libGL`, ne rendent **aucune erreur GL**, et `glGetQueryObjectuiv` **n'écrit pas dans la variable
de sortie** : l'application lit ce qu'elle y avait mis (0 pour nous), pour un triangle qui couvre
pourtant la moitié de l'image. C'est le seul cas du lot où le repli logiciel ne tient rien du tout.

### 4.4 Ce que le plugin en fait

Les identifiants du protocole sont pris dans la plage du client
(`query_base = index × QGPU_CLIENT_QUERY_IDS`), et la **poignée rendue à GLEngine est
`identifiant + 1`** — GLEngine range 0 quand il n'y a pas de requête, et le premier client a
justement l'identifiant 0. `QUERY_RESULT` écrit deux mots big-endian dans la fenêtre partagée et
le plugin lit le mot « disponible » plutôt que de le supposer, comme le protocole le demande pour
le jour où le device deviendra asynchrone.

Les trois entrées `gld*` ne pouvaient pas être ajoutées à `gld_tramp.s`, qui est **engendré** par
`tools/gld/gen_tramp.py` : le plugin les réalise en rendant l'adresse de sa propre fonction depuis
le crochet `pomppc_pre`, que le trampoline appelle déjà pour savoir où sauter. Les deux
procédures, elles, sont installées comme les autres — avec une exception : `pomppc_hook_procs`
accepte d'écrire par-dessus un emplacement **vide**, puisqu'il n'y a ici rien à quoi se replier.

**Preuve** (scène `occl`, 128×128, les deux chemins) : un rectangle de 64×64 entièrement visible
rend **4 096** échantillons, le même coupé de moitié par les ciseaux **2 048**, le même caché par
le test de profondeur **0**, et « disponible » vaut 1 dans les trois cas — tandis que le rendu
d'Apple seul rend 0 partout. L'image, elle, est identique au rendu d'Apple (0/255).

### 4.5 Exactitude : la seule direction sans danger

L'hôte ne compte que les fragments **qu'il a rastérisés**. Si un dessin retombe sur le rendu
d'Apple pendant qu'une requête court, ses fragments manqueraient. Le plugin ajoute alors l'aire de
la surface au compte : **sur-estimer est la seule direction sans danger**, puisqu'une requête
d'occlusion se lit « si le compte est nul, je peux sauter l'objet » — un objet déclaré visible est
dessiné, donc l'image reste exacte et seule la vitesse souffre. Le bilan
(`POMPPC_GL_STATS`) compte le cas sous le motif `requete:repli-logiciel` ; il est resté à **zéro**
sur toutes les scènes et sur Marble Blast.

---

## 5. Ce qui reste à établir

- `GS+0x4c80` (3 octets dérivés du mode de polygone). **[H]**
- Le motif de polygone est-il rangé *après* application de `GL_UNPACK_LSB_FIRST` et de
  l'alignement de `glPixelStore` ? La sonde n'a employé que l'état par défaut. **[H]**
- `GL_POINT_SMOOTH`, `GL_LINE_SMOOTH`, `GL_POLYGON_SMOOTH` : hors périmètre de la v8, non relevés.
- L'atténuation de la taille des points (`GL_POINT_DISTANCE_ATTENUATION`) n'est pas portée par le
  protocole : le chemin brut la refuse, le chemin hérité l'**ignore** (il envoie la taille de base).
  À corriger avant d'annoncer `GL_ARB_point_parameters` (cf. `docs/re/version-extensions.md`).
