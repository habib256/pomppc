# État de transformation et d'éclairage de GLEngine — relevé expérimental

Relevé le **18/09/2026** dans l'invité (Mac OS X 10.4.6 PPC), rendu d'Apple seul
(`POMPPC_GL_DISABLE=1`), par les sondes `lightprobe`, `matprobe`, `mtxprobe`, `tgprobe` et
`xformprobe` de `guest/gltest/gltest.c` : **un réglage GL par `glClear`**, le traceur du plugin
vide l'état à chaque effacement (`POMPPC_GLTRACE=dossier POMPPC_GLTRACE_STATE=1`), on diffe les
vidages successifs avec `tools/re/diffstate.py`. Toutes les valeurs des sondes sont des multiples
distincts de 1/32, de sorte que chaque champ est retrouvé **aussi** par recherche de flottant
big-endian (`diffstate.py find`).

**Tout ce qui suit est observé**, pas déduit ; les rares déductions sont signalées.

Conventions :

- `GS` = bloc d'état GL vu par le pilote, pointé par `ctx+0x0c` (= `gctx + 0x360`) ;
- les offsets **négatifs** (`GS−0x360 + x`, c'est-à-dire `gctx + x`) désignent la zone qui
  précède le bloc : elle est accessible au plugin, mais **hors** du bloc annoncé ;
- le traceur vide `clear-glstate` (0x5400 octets à partir de `GS+0`), `clear-gctxlow`
  (0x360 octets à partir de `gctx+0`, **ajouté pour ce relevé**), plus les objets pointés.

`0x5400` suffit : le champ le plus haut employé ici est `GS+0x50c0` (pointeur du pipeline
program), et le plus haut **lu** est `GS+0x5074` (unité de texture active). Le vidage
`clear-gctxlow` a en revanche été indispensable : les valeurs courantes (couleur, normale,
couleur secondaire, coordonnée de brouillard, coordonnées de texture) sont **avant** le bloc et
n'apparaissaient dans aucun vidage.

Sondes et vidages de référence : `bench/devloop/jobs/1789679323973/out/{light,mat,mtx,tg,xform}`
et `bench/devloop/jobs/1789679667213/out/tg`.

---

## 1. Éclairage

### 1.1 Interrupteurs et modèle d'éclairage

| Offset | Type | Champ | Appel qui l'a fait bouger | Valeur observée |
|---|---|---|---|---|
| `0x2d40` | u32 | masque des lumières allumées, bit `i` = `GL_LIGHT0+i` | `glEnable(GL_LIGHT0)` puis `LIGHT5`, `LIGHT7` | `0x00` → `0x01` → `0x21` → `0xa1` |
| `0x2d44` | u16 | face de `glColorMaterial` | `glColorMaterial(GL_FRONT, …)` | défaut `0x0408`, → `0x0404`, `0x0405` |
| `0x2d46` | u16 | mode de `glColorMaterial` | `glColorMaterial(…, GL_SPECULAR)` | défaut `0x1602`, → `0x1202`, `0x1600` |
| `0x2d48` | u16 | `GL_LIGHT_MODEL_COLOR_CONTROL` | `glLightModeli(COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR)` | `0x81f9` ⇄ `0x81fa` |
| `0x2d4a` | u8 | `GL_LIGHTING` | `glEnable(GL_LIGHTING)` | `0` → `1` |
| `0x2d4b` | u8 | `GL_COLOR_MATERIAL` | `glEnable(GL_COLOR_MATERIAL)` | `0` → `1` |
| `0x2d4c` | u8 | `GL_LIGHT_MODEL_TWO_SIDE` | `glLightModeli(TWO_SIDE, 1)` | `0` → `1` |
| `0x2d4d` | u8 | `GL_LIGHT_MODEL_LOCAL_VIEWER` | `glLightModeli(LOCAL_VIEWER, 1)` | `0` → `1` |
| `0x24b0` | float[4] | `GL_LIGHT_MODEL_AMBIENT` (ambiante de scène) | `glLightModelfv(GL_LIGHT_MODEL_AMBIENT, …)` | défaut `(0.2, 0.2, 0.2, 1)` → `(.8125 .875 .9375 1)` |
| `0x2e0b` | u8 | dérivé : « couleur secondaire produite » | `glLightModeli(COLOR_CONTROL, SEPARATE_SPECULAR)` | `0` ⇄ `1` |
| `0x317e` | u8 | dérivé : « les deux faces diffèrent » | `TWO_SIDE`, `glPolygonMode` dissymétrique, `glEnable(GL_CULL_FACE)` | `0` ⇄ `1` |

`0x2d4a`, `0x2d40` et `0x24b0` confirment `tableaux-de-sommets.md` §5.7 ; `0x2d4c`/`0x2d4d`
**corrigent** son hypothèse « `0x2d4d` = matériau de couleur / deux faces ».

### 1.2 Bloc d'une lumière : `GS + 0x24c0 + i·0x80`

Le pas de `0x80` est **prouvé** : `glLightfv(GL_LIGHT5, GL_DIFFUSE, …)` écrit en `0x2750`
(= `0x24c0 + 5·0x80 + 0x10`) et `glLightfv(GL_LIGHT7, GL_DIFFUSE, …)` en `0x2850`
(= `0x24c0 + 7·0x80 + 0x10`).

| Offset (lumière) | Type | Champ | Appel qui l'a fait bouger | Valeur observée |
|---|---|---|---|---|
| `+0x00` | float[4] | ambiante | `glLightfv(L0, GL_AMBIENT, …)` | défaut `(0,0,0,1)` → `(.0625 .125 .1875 .25)` |
| `+0x10` | float[4] | diffuse | `glLightfv(L0, GL_DIFFUSE, …)` | défaut `(1,1,1,1)` pour L0, `(0,0,0,1)` pour L1..L7 |
| `+0x20` | float[4] | spéculaire | `glLightfv(L0, GL_SPECULAR, …)` | défaut `(1,1,1,1)` pour L0, `(0,0,0,1)` sinon |
| `+0x30` | float[4] | **position, en coordonnées ŒIL** | `glLightfv(L0, GL_POSITION, …)` | défaut `(0,0,1,0)` ; `(1.5,2.5,3.5,1)` sous identité, **`(11.5,22.5,33.5,1)`** sous `glTranslatef(10,20,30)` |
| `+0x40` | float[3] | **direction de spot, en coordonnées ŒIL** (transformée comme une normale : inverse-transposée de la modèle-vue, **non normalisée**) | `glLightfv(L0, GL_SPOT_DIRECTION, …)` | défaut `(0,0,−1)` ; `(.25,.5,.75)` sous identité, **`(.125,.125,.09375)`** sous `glScalef(2,4,8)` |
| `+0x4c` | float | **cosinus** du seuil de spot ; `< 0` ⟺ pas un spot | `glLightf(L0, GL_SPOT_CUTOFF, 60)` | défaut `−1` ; `60°` → `0.49999997`, `30°` → `0.8660254`, `180°` → `−1` |
| `+0x50` | float | atténuation constante | `glLightf(L0, GL_CONSTANT_ATTENUATION, 3)` | `1` → `3` |
| `+0x54` | float | atténuation linéaire | `glLightf(…, GL_LINEAR_ATTENUATION, 5)` | `0` → `5` |
| `+0x58` | float | atténuation quadratique | `glLightf(…, GL_QUADRATIC_ATTENUATION, 7)` | `0` → `7` |
| `+0x5c` | float | exposant de spot | `glLightf(…, GL_SPOT_EXPONENT, 12)` | `0` → `12` |
| `+0x60` | float[4] | **dérivé** : vecteur demi-chemin normalisé (`normalize(L + (0,0,1))`, `w = 1`) | toute écriture de position | `(0.2553, 0.3120, 0.9151, 1)` pour `L = (4.5,5.5,6.5,0)` |
| `+0x70` | float | seuil de spot en **degrés** — **non fiable** | `glLightf(…, GL_SPOT_CUTOFF, …)` | défaut `180` ; `60` → `60`, `30` → `30`, mais `180` → **`0`** |

> Le champ utile est `+0x4c` (le cosinus). `+0x70` vaut 180 à l'initialisation et 0 après un
> retour explicite à 180 : il ne distingue pas les deux états, ne pas s'en servir.

L'état **dérivé** par lumière reste hors du bloc, en `GS+0x4710 + i·0x6c` (= `gctx+0x4a70`) :
direction/position normalisée (`+0x0c`), table d'exponentielle du spot (`+0x24`, 15 flottants),
`1/atténuation constante` (`+0x64`), drapeaux (`+0x68..0x6b`). Le plugin n'en a pas besoin :
l'hôte recalcule.

---

## 2. Matériau, matériau de couleur, valeurs courantes

### 2.1 Les deux matériaux sont **dans** le bloc

`tableaux-de-sommets.md` §5.8 les donnait « probablement hors du bloc transmis » : c'est **faux**.
Les pointeurs `GS+0x4a70` et `GS+0x4a74` visent `GS+0x28c0` et `GS+0x2b00` — mesuré :
`GS+0x4a70 = 0x000c6c40`, `GS+0x4a74 = 0x000c6e80`, base du bloc `0x000c4380` (déduite du
pointeur de matrice courante `GS+0x4c8c` et de l'index de mode `GS+0x4c94`), et les vidages
`clear-matf`/`clear-matb` sont **octet pour octet** identiques à `glstate[0x28c0…]` et
`glstate[0x2b00…]`. Taille d'un objet : `0x240`.

| Offset | Type | Champ | Appel qui l'a fait bouger | Valeur observée |
|---|---|---|---|---|
| `0x28c0` | float[4] | matériau **avant** : ambiante | `glMaterialfv(GL_FRONT, GL_AMBIENT, …)` | défaut `(0.2,0.2,0.2,1)` → `(.03125 .0625 .09375 .125)` |
| `0x28d0` | float[4] | avant : diffuse | `glMaterialfv(GL_FRONT, GL_DIFFUSE, …)` | défaut `(0.8,0.8,0.8,1)` → `(.15625 .1875 .21875 .25)` |
| `0x28e0` | float[4] | avant : spéculaire | `glMaterialfv(GL_FRONT, GL_SPECULAR, …)` | défaut `(0,0,0,1)` → `(.28125 .3125 .34375 .375)` |
| `0x28f0` | float[4] | avant : émission | `glMaterialfv(GL_FRONT, GL_EMISSION, …)` | défaut `(0,0,0,1)` → `(.40625 .4375 .46875 .5)` |
| `0x2900` | float | avant : brillance | `glMaterialf(GL_FRONT, GL_SHININESS, 37)` | `0` → `37` |
| `0x2af4` | u16 | avant : masque des composantes écrites | chaque `glMaterial*` | `0x1f00` → `0x1f04`, `0x1f0c`, `0x1f1c`, `0x1f1e`, `0x1f1f` |
| `0x2b00` | float[4] | matériau **arrière** : ambiante | `glMaterialfv(GL_BACK, GL_AMBIENT, …)` | `(.53125 .5625 .59375 .625)` |
| `0x2b10` | float[4] | arrière : diffuse | `glMaterialfv(GL_BACK, GL_DIFFUSE, …)` | `(.65625 .6875 .71875 .75)` |
| `0x2b20` | float[4] | arrière : spéculaire | `glMaterialfv(GL_BACK, GL_SPECULAR, …)` | `(.78125 .8125 .84375 .875)` |
| `0x2b30` | float[4] | arrière : émission | `glMaterialfv(GL_BACK, GL_EMISSION, …)` | `(.90625 .9375 .96875 1)` |
| `0x2b40` | float | arrière : brillance | `glMaterialf(GL_BACK, GL_SHININESS, 23)` | `0` → `23` |
| `0x2d34` | u16 | arrière : masque des composantes écrites | chaque `glMaterial*(GL_BACK, …)` | idem |

Bits de l'octet **bas** du masque `+0x234` : brillance `0x01`, émission `0x02`, ambiante `0x04`,
diffuse `0x08`, spéculaire `0x10`. L'octet haut vaut `0x1f` **dès le premier vidage** et ne bouge
pas (`tableaux-de-sommets.md` §5.8 le lisait comme un couple `0x404`/`0x808`/`0x1010`/`0x202` :
seul l'octet bas est mis à jour ici).

`glEnable(GL_COLOR_MATERIAL)` **écrase immédiatement** dans l'objet matériau les composantes
suivies par `glColorMaterial` avec la couleur courante (observé : `0x28c0…0x28dc` et
`0x2b00…0x2b1c` repassent à `(1,1,1,1)`), puis chaque `glColorMaterial` réécrit la nouvelle
composante. Conséquence pour le plugin : **il suffit de lire l'objet matériau**, GLEngine y tient
déjà le résultat du matériau de couleur ; il faut seulement savoir que la couleur courante
continuera de l'écraser à chaque `glColor*`.

### 2.2 Valeurs courantes — **hors du bloc**, en offsets `gctx` (= `GS − 0x360 + x`)

Aucun de ces appels ne bouge le moindre octet de `GS+0…0x5400` (seul effet visible :
`GS+0x450f` passe à `3` au premier `glTexCoord`). Ils vivent tous **avant** le bloc :

| Offset `gctx` | Offset depuis `GS` | Type | Champ | Appel | Valeur observée |
|---|---|---|---|---|---|
| `0x0120 + u·0x10` | `GS − 0x240 + u·0x10` | float[4] | coordonnée de texture courante de l'unité `u` | `glTexCoord4f`, `glMultiTexCoord4fARB(GL_TEXTURE3)` | u0 `(.28125 .46875 .59375 .71875)` ; u3 en `0x0150` |
| `0x02a0` | `GS − 0x0c0` | float[4] | couleur courante | `glColor4f(.0625 .1875 .3125 .4375)` | défaut `(1,1,1,1)` |
| `0x02b0` | `GS − 0x0b0` | float[3] | normale courante | `glNormal3f(.09375 .15625 .21875)` | défaut `(0,0,1)` |
| `0x02c0` | `GS − 0x0a0` | float[3] | **couleur secondaire courante** | `glSecondaryColor3fvEXT(.5625 .6875 .8125)` | défaut `(0,0,0)` |
| `0x02cc` | `GS − 0x094` | float | **coordonnée de brouillard courante** | `glFogCoordfEXT(.34375)` | défaut `0` |

`gctx+0x2c0` et `gctx+0x2cc` sont **nouveaux** (absents de `tableaux-de-sommets.md` §5.9).
Le pas de `0x10` entre unités de texture est prouvé par l'unité 3 (`gctx+0x150`).

---

## 3. Normalisation, ombrage, élimination des faces

| Offset | Type | Champ | Appel qui l'a fait bouger | Valeur observée |
|---|---|---|---|---|
| `0x24a4` | float | facteur d'échelle des normales (`GL_RESCALE_NORMAL`) | — (non exercé ; défaut) | `1.0` |
| `0x24ad` | u8 | `GL_NORMALIZE` | `glEnable(GL_NORMALIZE)` | `0` → `1` |
| `0x24ae` | u8 | `GL_RESCALE_NORMAL` | `glEnable(GL_RESCALE_NORMAL)` | `0` → `1` |
| `0x3170` | u16 | `glPolygonMode` face avant | `glPolygonMode(GL_FRONT, GL_LINE)` | `0x1b02` → `0x1b01` |
| `0x3172` | u16 | `glPolygonMode` face arrière | `glPolygonMode(GL_BACK, GL_POINT)` | `0x1b02` → `0x1b00` |
| `0x3174` | u16 | `glFrontFace` | `glFrontFace(GL_CW)` | `0x0901` ⇄ `0x0900` |
| `0x3176` | u16 | `glCullFace` | `glCullFace(GL_FRONT)`, `GL_FRONT_AND_BACK` | `0x0405` → `0x0404` → `0x0408` |
| `0x317a` | u8 | `GL_CULL_FACE` | `glEnable(GL_CULL_FACE)` | `0` → `1` |
| `0x3194` | u32 | `glShadeModel` | `glShadeModel(GL_FLAT)` | `0x1d01` ⇄ `0x1d00` |

`0x3174`/`0x3176`/`0x317a` et `GS_SHADE_MODEL` sont **confirmés** ; `0x24a4` n'a pas été mis à
l'épreuve (aucune modèle-vue à échelle non uniforme avec `GL_RESCALE_NORMAL` actif).

---

## 4. Matrices

Deux tableaux parallèles de 24 matrices `float[16]`, pas `0x40`, indexés par un index de mode
interne. Confirmé de deux façons indépendantes : par les écritures observées, et par le pointeur
de matrice courante `GS+0x4c8c`, dont la valeur suit exactement `base + index·0x40`.

| Offset | Type | Champ | Appel qui l'a fait bouger | Valeur observée |
|---|---|---|---|---|
| `0x1860 + m·0x40` | float[16] | matrice du mode `m` | voir table ci-dessous | — |
| `0x1e60 + m·0x40` | float[16] | inverse du mode `m` | recalculée paresseusement | — |
| `0x1860` | float[16] | **MVP** (mode 0), cache dérivé | `glLoadMatrixf` sur modèle-vue ou projection | recalculée à chaque fois |
| `0x1f60` | float[16] | **inverse de la modèle-vue** (mode 4) | `glPushMatrix`, `glClipPlane`, `glEnable(GL_LIGHTING)`, `glTexGenfv(EYE_PLANE)` | recalculée **à la demande** |
| `0x2460` | float[16] | MVP × transformation de viewport | idem MVP | — |
| `0x4c8c` | ptr | → matrice courante (dans le bloc) | `glMatrixMode` | `0x000c5ce0` pour le mode 4 |
| `0x4c90` | ptr | → inverse courante | `glMatrixMode` | `0x000c62e0` pour le mode 4 |
| `0x4c94` | i32 | **index de mode courant** | `glMatrixMode` | `4`, `3`, `2`, `16`, `17`, `18` |
| `0x4c9c + m·4` | i32 | profondeur de pile du mode `m` | `glPushMatrix`/`glPopMatrix` | `0x4cac` (modèle-vue) : `0 → 1 → 2 → 1 → 0` |
| `0x4d48` | u32 | masque « matrice modifiée » (bit = index de mode) | `glLoadMatrixf` | `0x11` (MVP+MV), `0x09` (MVP+proj), `0x10000`, `0x20000`, `0x40000`, `0x04` |
| `0x4318` | ptr | tampon des piles (hors bloc) | `glPushMatrix` | `0x02813600` |
| `0x5074` | u32 | **unité de texture active** | `glActiveTextureARB` | `0`, `1`, `2` |
| `0x30b2` | u8 | dérivé : matrice de couleur ≠ identité | `glMatrixMode(GL_COLOR); glLoadMatrixf` | `0` → `1` |

Correspondance index de mode → matrice, **vérifiée** :

| Index | Offset | Contenu | Preuve |
|---|---|---|---|
| 0 | `0x1860` | MVP (dérivé) | recalculée à chaque `glLoadMatrixf` sur MV ou projection |
| 2 | `0x18e0` | `GL_COLOR` | `glMatrixMode(GL_COLOR); glLoadMatrixf(1..16)` écrit en `0x18e0` |
| 3 | `0x1920` | `GL_PROJECTION` | `glMatrixMode(GL_PROJECTION); glLoadMatrixf(N)` |
| 4 | `0x1960` | `GL_MODELVIEW` | `glMatrixMode(GL_MODELVIEW); glLoadMatrixf(1..16)` |
| **16 + u** | **`0x1c60 + u·0x40`** | `GL_TEXTURE` de l'unité `u` | unité 0 → `0x1c60`, unité 1 → `0x1ca0`, unité 2 → `0x1ce0` |

---

## 5. TexGen — bloc propre, pas `0x94` par unité, `0x24` par coordonnée

Base `GS+0x3988` ; unité `u` à `+u·0x94` (prouvé : l'unité 2 est en `0x3ab0`) ;
coordonnée `c` (S, T, R, Q) à `+c·0x24` (prouvé pour les quatre).

| Offset (unité 0) | Type | Champ | Appel qui l'a fait bouger | Valeur observée |
|---|---|---|---|---|
| `0x3988` | u16 | mode **S** | `glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, …)` | défaut `0x2400` ; `0x2401`, `0x2402`, `0x8512` |
| `0x398c` | float[4] | plan **œil** S (transformé en coordonnées œil) | `glTexGenfv(GL_S, GL_EYE_PLANE, …)` | défaut `(1,0,0,0)` ; `(.5,.25,.125,.0625)` sous `glTranslatef(10,20,30)` donne **`(.5,.25,.125,−13.6875)`** |
| `0x399c` | float[4] | plan **objet** S (brut) | `glTexGenfv(GL_S, GL_OBJECT_PLANE, …)` | défaut `(1,0,0,0)` → `(.0625 .125 .1875 .25)` |
| `0x39ac` / `0x39b0` / `0x39c0` | u16 / float[4] / float[4] | mode T, plan œil T, plan objet T | `glTexGen*(GL_T, …)` | défauts `0x2400`, `(0,1,0,0)`, `(0,1,0,0)` |
| `0x39d0` / `0x39d4` / `0x39e4` | u16 / float[4] / float[4] | mode R, plan œil R, plan objet R | `glTexGen*(GL_R, …)` | défauts `0x2400`, `(0,0,0,0)`, `(0,0,0,0)` |
| `0x39f4` / `0x39f8` / `0x3a08` | u16 / float[4] / float[4] | mode Q, plan œil Q, plan objet Q | `glTexGen*(GL_Q, …)` | défauts `0x2400`, `(0,0,0,0)`, `(0,0,0,0)` |
| `0x3a18` … `0x3a1b` | u8 ×4 | `GL_TEXTURE_GEN_S/T/R/Q` | `glEnable(GL_TEXTURE_GEN_S)` … | `0` → `1` chacun |

Codes de mode observés : `0x2400` `GL_EYE_LINEAR` (défaut), `0x2401` `GL_OBJECT_LINEAR`,
`0x2402` `GL_SPHERE_MAP`, `0x8511` `GL_NORMAL_MAP`, `0x8512` `GL_REFLECTION_MAP` —
c'est-à-dire **l'énumération GL telle quelle**.
Unité 2 : mode S `0x3ab0`, plan objet S `0x3ac4`, activation S `0x3b40`.

`GL_SPHERE_MAP` sur R et `GL_REFLECTION_MAP` sur Q sont des erreurs GL (rien n'est écrit) : la
sonde ne peut pas les employer pour situer les champs. Les offsets des modes R et Q ont été
obtenus avec `GL_OBJECT_LINEAR`.

---

## 6. Plans de découpe

| Offset | Type | Champ | Appel qui l'a fait bouger | Valeur observée |
|---|---|---|---|---|
| `0x3e28` | u32 | masque d'activation, bit `i` = `GL_CLIP_PLANE0+i` | `glEnable(GL_CLIP_PLANE0)`, `…3`, `…5` | `0x00` → `0x01` → `0x09` → `0x29` |
| `0x3e2c + i·0x10` | float[4] | plan `i`, **en coordonnées ŒIL** | `glClipPlane` | plan 0 en `0x3e2c`, plan 3 en `0x3e5c`, plan 5 en `0x3e7c` |

Preuve des coordonnées œil : `glClipPlane(GL_CLIP_PLANE5, (.5, .25, .125, .0625))` posé sous une
modèle-vue `glTranslatef(10, 20, 30)` est stocké **`(.5, .25, .125, −13.6875)`**
(`0.0625 − (0.5·10 + 0.25·20 + 0.125·30) = −13.6875`) : le plan est multiplié par l'inverse de la
modèle-vue au moment de l'appel, comme le veut la spécification.

---

## 7. Brouillard

| Offset | Type | Champ | Appel qui l'a fait bouger | Valeur observée |
|---|---|---|---|---|
| `0x2de0` | float[4] | `GL_FOG_COLOR` | `glFogfv(GL_FOG_COLOR, …)` | `(0,0,0,0)` → `(.0625 .1875 .3125 .4375)` |
| `0x2df0` | float | **`GL_FOG_DENSITY`** | `glFogf(GL_FOG_DENSITY, .1875)` | `1` → `0.1875` |
| `0x2df4` | float | **`GL_FOG_START`** | `glFogf(GL_FOG_START, .3125)` | `0` → `0.3125` |
| `0x2df8` | float | **`GL_FOG_END`** | `glFogf(GL_FOG_END, .4375)` | `1` → `0.4375` |
| `0x2dfc` | float | dérivé : `1 / (fin − début)` | idem | `1` → `1.454545` → `8` |
| `0x2e04` | u16 | `GL_FOG_MODE` | `glFogi(GL_FOG_MODE, GL_EXP2)` puis `GL_LINEAR` | `0x0800` → `0x0801` → `0x2601` |
| `0x2e06` | u16 | **`GL_FOG_COORDINATE_SOURCE`** | `glFogi(FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE)` | `0x8452` ⇄ `0x8451` |
| `0x2e0a` | u8 | `GL_FOG` | `glEnable(GL_FOG)` | `0` → `1` |
| `0x2e14` | u16 | `GL_FOG_HINT` | — (défaut) | `0x1100` |

Attention : `GS+0x2e04` est un `u16` dont l'octet haut porte l'énumération (`0x0800 GL_EXP`,
`0x0801 GL_EXP2`, `0x2601 GL_LINEAR`). `GS_FOG_MODE 0x2e04` de `pomppc_accel.c` est donc juste.

---

## 8. Viewport et plage de profondeur

| Offset | Type | Champ | Appel qui l'a fait bouger | Valeur observée |
|---|---|---|---|---|
| `0x1810` | float[4] | **échelle** de viewport `(x, y, z, w)` | `glViewport(3,5,17,19)` | `(8.4995747, 9.4995251, 5.36870880e8, 1)` |
| `0x1820` | float[4] | **biais** de viewport | idem | `(11.5, 14.5, 5.36870880e8, 0)` |
| `0x1830` | double | `glDepthRange` near | `glDepthRange(0.25, 0.75)` | `0.25` |
| `0x1838` | double | `glDepthRange` far | idem | `0.75` |
| `0x1840`/`0x1844`/`0x1848`/`0x184c` | i32 ×4 | viewport `x, y, largeur, hauteur` | `glViewport(3,5,17,19)` | `3, 5, 17, 19` |
| `0x1850`/`0x1854`/`0x1858` | float ×3 | near, far, far − near | `glDepthRange(0.25,0.75)` | `0.25, 0.75, 0.5` |
| `0x46cc`/`0x46d0`/`0x46d4`/`0x46d8` | float ×4 | échelle x, biais x, échelle y, biais y (**sans correction**) | `glViewport(3,5,17,19)` | `8.5, 11.5, 9.5, 14.5` |
| `0x46dc`/`0x46e0` | float ×2 | échelle z, biais z **en `[0,1]`** | `glDepthRange(0.25,0.75)` | `0.25, 0.5` |

Relation exacte, mesurée sur `glViewport(3,5,17,19)` dans un tampon 64×64 et
`glDepthRange(0.25, 0.75)` :

```
échelle.x = (largeur / 2) · 0.99995      biais.x = x + largeur / 2      (exact)
échelle.y = (hauteur / 2) · 0.99995      biais.y = y + hauteur / 2      (exact)
échelle.z = ((far − near) / 2) · D       biais.z = ((far + near) / 2) · D
```

avec `D = ctx+0x14` (`CTX_DEPTH_SCALE`) = **1073741760.0** sur ce contexte :
`0.25·D = 268435440` et `0.5·D = 536870880`, exactement les valeurs relevées.
Le facteur `0.99995` ne porte que sur x et y ; `GS+0x46cc…0x46d8` donne les mêmes grandeurs
**sans** ce facteur, et `GS+0x46dc/0x46e0` les donne normalisées dans `[0,1]`.
`échelle.w = 1`, `biais.w = 0` (constants).

---

## 9. Points, lignes, polygones

| Offset | Type | Champ | Appel qui l'a fait bouger | Valeur observée |
|---|---|---|---|---|
| `0x2e20` | float | `glLineWidth` | `glLineWidth(3.25)` | `1` → `3.25` |
| `0x30bc` | float | `glPointSize` | `glPointSize(5.5)` | `1` → `5.5` |
| `0x30c0` | float | `GL_POINT_SIZE_MIN` | `glPointParameterf(POINT_SIZE_MIN, .75)` | `0` → `0.75` |
| `0x30c4` | float | `GL_POINT_SIZE_MAX` | `glPointParameterf(POINT_SIZE_MAX, 19.5)` | `1` → `19.5` |
| `0x30c8` | float | `GL_POINT_FADE_THRESHOLD_SIZE` | `glPointParameterf(FADE_THRESHOLD, 3.25)` | `1` → `3.25` |
| `0x30cc` | float[3] | **`GL_POINT_DISTANCE_ATTENUATION`** | `glPointParameterfv(DISTANCE_ATTENUATION, …)` | `(1,0,0)` → `(.5,.25,.125)` |
| `0x3170`/`0x3172` | u16 ×2 | `glPolygonMode` avant / arrière | `glPolygonMode` | voir §3 |

Les entrées `glPointParameterf/fv` sont résolues par `dlsym` dans la sonde : la variante `ARB`
existe bien dans le `libGL` de 10.4.6.

---

## 10. Bloc `#define` prêt à recopier dans `guest/gldriver/pomppc_accel.c`

```c
/* État T&L de GLEngine — relevé par les sondes de docs/re/etat-tcl.md (18/09/2026).
   Tous les offsets sont relatifs au bloc d'état GL (CTX_GLSTATE, noté GS). */
/* Matrices : 24 matrices float[16], pas 0x40, index de mode interne.
   0 = MVP (dérivé), 2 = GL_COLOR, 3 = GL_PROJECTION, 4 = GL_MODELVIEW,
   16+u = GL_TEXTURE de l'unité u. */
#define GS_MATRIX(m)     (0x1860 + (m) * 0x40)   /* float[16] */
#define GS_MATRIX_INV(m) (0x1e60 + (m) * 0x40)   /* float[16] */
#define GS_MAT_MVP       0x1860
#define GS_MAT_COLOR     0x18e0
#define GS_MAT_PROJ      0x1920
#define GS_MAT_MODELVIEW 0x1960
#define GS_MAT_TEXTURE(u) (0x1c60 + (u) * 0x40)
#define GS_MAT_MV_INV    0x1f60                  /* inverse de la modèle-vue */
#define GS_MAT_MVP_VIEW  0x2460                  /* MVP × transformation de viewport */
#define GS_MAT_CURRENT   0x4c8c                  /* ptr → matrice courante */
#define GS_MAT_MODE      0x4c94                  /* i32 : index de mode 0..23 */
#define GS_MAT_DEPTH(m)  (0x4c9c + (m) * 4)      /* i32 : profondeur de pile */
#define GS_MAT_DIRTY     0x4d48                  /* u32 : bit = index de mode modifié */
#define GS_ACTIVE_TEXUNIT 0x5074                 /* u32 : unité de texture active */

/* Viewport et profondeur : échelle/biais = (l/2)·0.99995 et x + l/2 en x, y ;
   ((far−near)/2)·D et ((far+near)/2)·D en z, avec D = ctx+CTX_DEPTH_SCALE. */
#define GS_VP_SCALE      0x1810   /* float ×4 (x, y, z, w) */
#define GS_VP_BIAS       0x1820   /* float ×4 */
#define GS_DEPTH_NEAR_D  0x1830   /* double : glDepthRange near */
#define GS_DEPTH_FAR_D   0x1838   /* double : glDepthRange far */
#define GS_VIEWPORT      0x1840   /* i32 ×4 : x, y, largeur, hauteur */
#define GS_DEPTH_NEAR_F  0x1850   /* float ×3 : near, far, far−near */
#define GS_WIN_SCALE     0x46cc   /* float ×4 : échelle x, biais x, échelle y, biais y */
#define GS_WIN_Z         0x46dc   /* float ×2 : échelle z, biais z dans [0,1] */

/* Normalisation et faces. */
#define GS_NORMAL_SCALE  0x24a4   /* float : facteur de GL_RESCALE_NORMAL */
#define GS_NORMALIZE     0x24ad   /* u8 */
#define GS_RESCALE_NORMAL 0x24ae  /* u8 */
#define GS_FRONT_FACE    0x3174   /* u16 : GL_CW 0x900 / GL_CCW 0x901 */
#define GS_CULL_FACE_MODE 0x3176  /* u16 : 0x404 / 0x405 / 0x408 */
#define GS_CULL_FACE     0x317a   /* u8 */
#define GS_TWO_SIDE_DRV  0x317e   /* u8 : dérivé « les deux faces diffèrent » */

/* Éclairage. */
#define GS_LIGHT_MASK    0x2d40   /* u32 : bit i = GL_LIGHT0+i allumée */
#define GS_COLORMAT_FACE 0x2d44   /* u16 : GL_FRONT/BACK/FRONT_AND_BACK */
#define GS_COLORMAT_MODE 0x2d46   /* u16 : GL_AMBIENT … GL_AMBIENT_AND_DIFFUSE */
#define GS_COLOR_CONTROL 0x2d48   /* u16 : 0x81f9 SINGLE / 0x81fa SEPARATE_SPECULAR */
#define GS_LIGHTING      0x2d4a   /* u8 */
#define GS_COLOR_MATERIAL 0x2d4b  /* u8 */
#define GS_TWO_SIDE      0x2d4c   /* u8 : GL_LIGHT_MODEL_TWO_SIDE */
#define GS_LOCAL_VIEWER  0x2d4d   /* u8 : GL_LIGHT_MODEL_LOCAL_VIEWER */
#define GS_SCENE_AMBIENT 0x24b0   /* float ×4 : GL_LIGHT_MODEL_AMBIENT */
#define GS_LIGHT(i)      (0x24c0 + (i) * 0x80)
#define GL_MAX_LIGHTS    8
#define LT_AMBIENT       0x00     /* float ×4 */
#define LT_DIFFUSE       0x10
#define LT_SPECULAR      0x20
#define LT_POSITION      0x30     /* float ×4, coordonnées ŒIL */
#define LT_SPOT_DIR      0x40     /* float ×3, coordonnées ŒIL, non normalisée */
#define LT_SPOT_COS      0x4c     /* float : cos(seuil) ; < 0 = pas un spot */
#define LT_ATT_CONST     0x50     /* float */
#define LT_ATT_LINEAR    0x54
#define LT_ATT_QUAD      0x58
#define LT_SPOT_EXP      0x5c     /* float */

/* Matériaux : objets de 0x240 octets DANS le bloc (GS+0x4a70/0x4a74 y pointent). */
#define GS_MATERIAL_FRONT 0x28c0
#define GS_MATERIAL_BACK  0x2b00
#define GS_MATERIAL_PTR_F 0x4a70  /* ptr → GS+0x28c0 */
#define GS_MATERIAL_PTR_B 0x4a74  /* ptr → GS+0x2b00 */
#define MT_AMBIENT       0x00     /* float ×4 */
#define MT_DIFFUSE       0x10
#define MT_SPECULAR      0x20
#define MT_EMISSION      0x30
#define MT_SHININESS     0x40     /* float */
#define MT_DIRTY         0x234    /* u16 : 1 brill., 2 émi., 4 amb., 8 dif., 0x10 spéc. */

/* Valeurs courantes : elles sont AVANT le bloc (offsets gctx = GS − 0x360). */
#define GS_CUR_TEXCOORD(u) (-0x240 + (u) * 0x10) /* float ×4 */
#define GS_CUR_COLOR     (-0x0c0)                /* float ×4 */
#define GS_CUR_NORMAL    (-0x0b0)                /* float ×3 */
#define GS_CUR_SECCOLOR  (-0x0a0)                /* float ×3 */
#define GS_CUR_FOGCOORD  (-0x094)                /* float */

/* TexGen : bloc propre, pas 0x94 par unité, 0x24 par coordonnée (S, T, R, Q). */
#define GS_TEXGEN(u)     (0x3988 + (u) * 0x94)
#define TG_COORD(c)      ((c) * 0x24)
#define TG_MODE          0x00     /* u16 : énumération GL telle quelle */
#define TG_EYE_PLANE     0x04     /* float ×4, coordonnées ŒIL */
#define TG_OBJ_PLANE     0x14     /* float ×4, brut */
#define TG_ENABLE        0x90     /* u8 ×4 : S, T, R, Q */

/* Plans de découpe utilisateur. */
#define GS_CLIP_MASK     0x3e28   /* u32 : bit i = GL_CLIP_PLANE0+i */
#define GS_CLIP_PLANE(i) (0x3e2c + (i) * 0x10)   /* float ×4, coordonnées ŒIL */
#define GL_MAX_CLIP_PLANES 6

/* Brouillard (compléments à GS_FOG_COLOR / GS_FOG_MODE / GS_FOG / GS_FOG_HINT). */
#define GS_FOG_DENSITY   0x2df0   /* float */
#define GS_FOG_START     0x2df4   /* float */
#define GS_FOG_END       0x2df8   /* float */
#define GS_FOG_INV_SPAN  0x2dfc   /* float : 1 / (fin − début), dérivé */
#define GS_FOG_COORD_SRC 0x2e06   /* u16 : 0x8451 FOG_COORD / 0x8452 FRAGMENT_DEPTH */

/* Points (compléments à GS_POINT_SIZE / GS_POINT_SMOOTH). */
#define GS_POINT_SIZE_MIN 0x30c0  /* float */
#define GS_POINT_SIZE_MAX 0x30c4  /* float */
#define GS_POINT_FADE     0x30c8  /* float */
#define GS_POINT_ATT      0x30cc  /* float ×3 : constante, linéaire, quadratique */
```

---

## 11. Écarts avec `docs/re/tableaux-de-sommets.md` (relevé par lecture)

| Point | Ce que disait le relevé par lecture | Ce qui est observé |
|---|---|---|
| Matériau | objet séparé, « probablement **hors** du bloc transmis » **[H]** | **dans** le bloc, `GS+0x28c0` (avant) et `GS+0x2b00` (arrière) ; `GS+0x4a70/0x4a74` y pointent |
| Masque du matériau `+0x234` | `0x404` ambiante, `0x808` diffuse, `0x1010` spéculaire, `0x202` émission | octet **bas** seul : `0x04`, `0x08`, `0x10`, `0x02`, brillance `0x01` ; l'octet haut vaut `0x1f` dès le départ |
| Ambiante de lumière `+0x00` | **[H]** par élimination | **confirmée** (défaut `(0,0,0,1)`) |
| Direction de spot `+0x40` | **[H]** | **confirmée**, et **en coordonnées œil** (inverse-transposée, non normalisée) |
| Seuil de spot `+0x4c` | **[H]** pour « cosinus » | **cosinus confirmé** (`60°` → `0.5`) ; il existe en plus le seuil en degrés en `+0x70`, **non fiable** |
| `gctx+0x30ad` (`GS+0x2d4d`) | « matériau de couleur / deux faces » **[H]** | `0x2d4b` = `GL_COLOR_MATERIAL`, `0x2d4c` = `TWO_SIDE`, `0x2d4d` = `LOCAL_VIEWER`, `0x2d48` u16 = contrôle de couleur |
| Ambiante de scène `GS+0x24b0` | **[H]**, défaut annoncé `(0,0,0,1)` | **confirmée**, défaut `(0.2, 0.2, 0.2, 1)` |
| Masques de matrices | `GS+0x4d4c` / `GS+0x4d50` | le masque « matrice modifiée » est en **`GS+0x4d48`** ; `0x4d4c` reste nul ; `0x4d50` mélange d'autres bits |
| Valeurs courantes | couleur `gctx+0x2a0`, normale `gctx+0x2b0`, texcoords `gctx+0x120` | **confirmées**, plus **couleur secondaire `gctx+0x2c0`** et **coordonnée de brouillard `gctx+0x2cc`** |
| TexGen | activations en `+0x90..0x93`, plans en `+0x04`/`+0x14` | **confirmé**, pas `0x94` par unité **prouvé** sur l'unité 2, pas `0x24` par coordonnée **prouvé** sur les quatre |
| Matrices de texture | `16 + u` **[H]** | **prouvé** (unités 0, 1, 2 en `0x1c60`, `0x1ca0`, `0x1ce0`) |
| Échelle de viewport | « échelle/biais » sans formule | formule exacte donnée en §8, dont le facteur `0.99995` sur x et y |

## 12. Ce qui reste à établir

- `GS+0x24a4` (facteur de `GL_RESCALE_NORMAL`) : jamais vu bouger, la sonde n'a pas posé de
  modèle-vue à échelle non uniforme avec `GL_RESCALE_NORMAL` actif.
- L'usage exact de `GS+0x4d50` (masque d'état dérivé) et de `GS+0x450f` (mis à `3` au premier
  `glTexCoord`).
- Pourquoi `GS+0x1810` porte le facteur `0.99995` et `GS+0x46cc` non : les deux jeux coexistent,
  reste à savoir lequel un pilote à T&L doit consommer (le `GeForce3GLDriver` lit `GS+0x1810`,
  `tableaux-de-sommets.md` §5).
- Les modes de matrice 1, 5..15 (`GL_MODELVIEW1..3_ARB`, `GL_MATRIX0..7_ARB`) : non exercés.
