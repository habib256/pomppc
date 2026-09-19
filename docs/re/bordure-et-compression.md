# Bordure, miroir et compression S3TC : ce que GLEngine donne au pilote (Tiger 10.4.6)

Relevé du 19/09/2026, **par l'expérience dans l'invité** (VM de dev Linux, device qgpu v11,
RTX 4060 Ti) : sonde `wrapprobe` de `guest/gltest` (job `tools/guest/jobs/wrapprobe`, modes
`PROBE_MODE=border|dxt|generic|genmip`), vidage de l'objet texture à l'échange
(`POMPPC_GL_T3DDUMP=1`). Suite de `docs/re/textures-3d.md` et `docs/re/cartes-de-cube.md`.

## 1. Répétitions et couleur de bordure

`glTexParameteri(WRAP_S, GL_CLAMP_TO_BORDER)`, `(WRAP_T, GL_MIRRORED_REPEAT)` et
`glTexParameterfv(GL_TEXTURE_BORDER_COLOR, {0,25 ; 0,5 ; 0,75 ; 1})` passent **sans erreur**, et
GLEngine les range tels quels dans son objet texture (`DT_PARAMS`) :

| Offset | Type | Champ | Valeur initiale |
|---|---|---|---|
| `+0x10` `+0x12` `+0x14` | u16 | `WRAP_S/T/R` : `0x812D` et `0x8370` y arrivent | `REPEAT` |
| `+0x1a` | u16 | `0x85BD` (constant, non relevé) | |
| **`+0x1c`** | 4 × f32 | **`GL_TEXTURE_BORDER_COLOR`** r, g, b, a | 0, 0, 0, 0 |
| `+0x2c` | f32 | 1,0 dans tous les vidages (probablement l'anisotropie max.) **[H]** | 1 |

Le **rendu d'Apple ignore les deux répétitions** : `CLAMP_TO_BORDER` y rend le texel du bord
(blanc au lieu de la couleur de bordure), `MIRRORED_REPEAT` y est un `REPEAT` (`v15`). Il n'y a
donc pas de repli exact ; comme pour la 3D, c'est l'hôte qui doit les tenir.

## 2. Textures compressées

### 2.1 Ce qu'annonce GLEngine

`GL_NUM_COMPRESSED_TEXTURE_FORMATS` = **0**, liste vide — avec le tableau d'Apple, avec le
nôtre, et même avec les 79 bits d'extensions allumés (`POMPPC_GL_ALLEXT=1`). OpenGL 1.3 le
permet : `ARB_texture_compression` n'exige aucun format particulier.

### 2.2 `glCompressedTexImage2D(…, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, …)`

Accepté **sans erreur** même sans l'extension S3TC. Le pilote reçoit :

* dans l'objet de GLEngine, l'entrée de niveau (`+0xa4`) avec le format **`0x83F0`**, le type
  **0** et le pointeur des **blocs tels quels** ; `+0x02` (format interne) vaut `GL_RGB` ;
* dans le GLDriver, `LV_FORMAT = 0x83F0`, `LV_TYPE = 0`, `LV_DATA` = les blocs, et un second
  pointeur en `LV+0x14` vers un tampon décodé à part (quand rien n'est à décoder, `LV+0x14` =
  `LV_DATA`) ;
* **`DT_BASE_FORMAT` (`dt+0x578`) = `0x83F0`** — le format compressé, pas un format de base.

Le rendu d'Apple ne rend pas ces textures : noir sur la sonde, noir aussi sur la scène `tex13`, et
**il plante (Bus error) dès qu'il dessine une texture à mipmaps chargée ainsi** (scène `tex13`,
rangée 3, reproduit sous `POMPPC_GL_DISABLE=1` et sous `POMPPC_GL_TEX13=0`).

### 2.3 `glTexImage2D(…, GL_COMPRESSED_RGB, …)` (format générique)

**GLEngine compresse lui-même** : le niveau arrive en DXT1 (`0x83F0`, type 0, 8 octets pour
2×2), mais `DT_BASE_FORMAT` vaut **`GL_RGB`**. Le rendu d'Apple décode ce cas-là correctement, y
compris avec des mipmaps (`genmip`, pas de plantage), à la précision près : il développe le
565 **sans répliquer les bits hauts** (blanc → `f8fcf8`). L'hôte, lui, rend `ffffff`.

### 2.4 Les requêtes

`glGetTexLevelParameteriv` rend `GL_TEXTURE_INTERNAL_FORMAT` = **`GL_RGBA8` pour toute
texture**, compressée ou non, et **ne répond pas** à `GL_TEXTURE_COMPRESSED` ni à
`GL_TEXTURE_COMPRESSED_IMAGE_SIZE` (la variable reste intacte, aucune erreur GL). C'est pareil
sous Apple et sous nous, avec ou sans nos bits, avec les 79 bits : **c'est GLEngine**, et aucune
de ces requêtes ne passe par le pilote (rien dans la trace, et `0x8058` ne figure dans aucun
vidage de l'objet texture). `glGetCompressedTexImage` fonctionne (le bloc exact, 8 octets pour
2×2).

## 3. Ce qui en découle dans le plugin (`G.tex13`, v10 + `QGPU_CAP_GL14`)

* `tex_wrap_ok` accepte `GL_CLAMP_TO_BORDER` et `GL_MIRRORED_REPEAT` ; la couleur de bordure
  (`TP_BORDER`) part en `QGPU_TP_BORDER_COLOR` (0xAARRGGBB). Sans `G.tex13`, une couleur de
  bordure non nulle sous `GL_CLAMP` est refusée (l'hôte y mettrait le noir transparent).
* Les niveaux S3TC partent **tels quels** en `TEX_IMAGE3` (format `0x83F0..0x83F3`, type 0, pas
  0, `ceil(w/4)·ceil(h/4)` blocs de 8 ou 16 octets), en 2D et sur les faces de cube ; l'hôte les
  décode (`qgpu-core.c`, `dxt_block`). Le format de base est ramené à `GL_RGB` (DXT1) ou
  `GL_RGBA` (les trois autres), et **tout autre couple est refusé** avant l'envoi : l'hôte
  rejetterait la soumission entière.
* Annoncées (bits de `docs/re/version-extensions.md` §4) : `GL_ARB_texture_border_clamp` (3),
  `GL_ARB_texture_compression` (10), `GL_ARB_texture_mirrored_repeat` (11).
* **Non annoncée : `GL_EXT_texture_compression_s3tc` (43).** Le relais est exact, mais le moindre
  repli sur le rendu d'Apple avec une texture à mipmaps de `glCompressedTexImage2D` fait planter
  l'application. Il faudrait d'abord que ce repli soit impossible ou rendu sûr (décodage dans
  l'invité, ou refus du dessin).

## 4. Preuve

Scène `tex13` : `CLAMP_TO_BORDER` (bordure, intérieur, linéaire au bord), `GL_CLAMP` linéaire au
bord avec la couleur de bordure, `CLAMP_TO_EDGE` en témoin, `MIRRORED_REPEAT` (s = 1,25 ;
−0,25 ; 2,25), DXT1 (les quatre couleurs), DXT1 à alpha (trois couleurs et le transparent, test
d'alpha), DXT3 et DXT5 (alphas explicites et interpolés, mélange), mipmaps DXT1 8×8 → 1×1,
format générique, sous-image compressée, carte de cube DXT1 : **34 cas sur 34** par le chemin brut
**et** par le chemin hérité ; le rendu d'Apple seul plante (§2.2). Dans `v15`, « 1.3 compression
S3TC » et « 1.4 GL_MIRRORED_REPEAT » sont **TENUS**.
