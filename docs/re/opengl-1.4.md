# OpenGL 1.4 : ce que GLEngine donne au pilote, et pourquoi le chemin brut perdait la géométrie

Relevé du 19/09/2026, **par l'expérience dans l'invité** (VM de dev Linux, device qgpu v12,
RTX 4060 Ti) et **par lecture** de `GLEngine` 10.4.6, extrait de la VM et désassemblé par
`tools/re/ppcanno.py`. Sondes de `guest/gltest` : `v14probe` (état GL vidé à chaque
`glClear` sous `POMPPC_GL_DISABLE=1`, objet texture vidé à l'échange, job
`tools/guest/jobs/v14probe`), `tcprobe`, `ptprobe`, `matbegin` ; vérification au pixel :
`tex14`. Convention : `GS` = bloc d'état GL de GLEngine (`drvctx+0x0c`), `gctx = GS − 0x360`.

## 1. Où GLEngine range l'état 1.4

| Offset | Type | Champ | Initial | Preuve |
|---|---|---|---|---|
| `GS+0x2e0b` | u8 | `glEnable(GL_COLOR_SUM)` ; copie en `GS+0x4c8a` | 0 | diff 1→2, 5→6 |
| `GS−0xa0` | f32 ×3 | couleur secondaire courante (déjà connue) | 0 | diff 2→3 |
| `GS+0x31c4 + u·0x7c + 0x3c` | f32 | biais de LOD de l'**unité** (`GL_TEXTURE_FILTER_CONTROL`) | 0 | 1,5 en `+0x3200`, 2,5 en `+0x327c` |
| `GS+0x30c0` | f32 | `GL_POINT_SIZE_MIN` | 0 | diff 6→7 |
| `GS+0x30c4` | f32 | `GL_POINT_SIZE_MAX` | **1** (voir §4) | diff 7→8 |
| `GS+0x30c8` | f32 | `GL_POINT_FADE_THRESHOLD_SIZE` | 1 | diff 8→9 |
| `GS+0x30cc` | f32 ×3 | `GL_POINT_DISTANCE_ATTENUATION` (déjà connu) | 1, 0, 0 | diff 9→10 |

Objet texture de GLEngine (`DT_PARAMS`, suite de `docs/re/textures-3d.md` §2) :

| Offset | Type | Champ | Initial |
|---|---|---|---|
| `+0x38` | f32 | `GL_TEXTURE_LOD_BIAS` (déjà connu) | 0 |
| **`+0x40`** | u16 | `GL_TEXTURE_COMPARE_FUNC` | `GL_LEQUAL` |
| **`+0x42`** | u16 | `GL_TEXTURE_COMPARE_MODE` (`0x884E` = `COMPARE_R_TO_TEXTURE`) | 0 |
| **`+0x48`** | u16 | `GL_DEPTH_TEXTURE_MODE` | `GL_LUMINANCE` |
| `+0x4a` | u16 | 0x8000 avec `GL_GENERATE_MIPMAP` **[H]** (GLEngine fabrique les niveaux lui-même) | 0 |

**Texture de profondeur** : `glTexImage2D(…, GL_DEPTH_COMPONENT, …, GL_FLOAT, …)` arrive au
pilote **convertie** en `GL_DEPTH_COMPONENT` / `GL_UNSIGNED_INT` (niveau et `LV_*`), format de
base `0x1902` en `DT_BASE_FORMAT`.

**GLEngine accepte tout**, erreur GL nulle, avec notre annonce, avec les 79 bits d'extensions
allumés, et sous Apple : `INCR_WRAP`/`DECR_WRAP`, une source croisée, la texture de profondeur
et ses trois paramètres, le biais de texture, `GENERATE_MIPMAP`. `GL_MAX_TEXTURE_LOD_BIAS`
est `cfg+0xb0` (`docs/re/capacites-glengine.md` §4) : 0 chez Apple.

## 2. Ce que le rendu d'Apple fait (scène `tex14` sous `POMPPC_GL_DISABLE=1`)

Juste : stencil à enveloppement, mélange au carré (`GL_SRC_COLOR` en source, `GL_DST_COLOR` en
destination), `glMultiDrawArrays`, biais de LOD **d'unité**, crossbar en `REPLACE` seul.
Faux : couleur secondaire (jamais ajoutée), biais de LOD **de texture**, comparaison d'ombre et
modes de profondeur (il rend `D` en luminance quoi qu'il arrive), crossbar dès qu'une source
croisée entre dans une opération (`MODULATE(TEXTURE0, TEXTURE1)` rend la seconde texture),
paramètres de point (taille de base, ni atténuation ni borne), `GL_MAX_TEXTURE_LOD_BIAS = 0`
alors qu'OpenGL 1.4 exige au moins 2. **Il n'y a donc pas de repli exact** pour 1.4 : tout doit
passer par l'hôte.

## 3. Le chemin brut perdait la géométrie à coordonnée r ou q

### 3.1 Le constat

`docs/re/textures-3d.md` §4 et `cartes-de-cube.md` §5 attribuaient la perte à la cible (3D,
cube). La sonde `tcprobe` (texture 2D blanche, chemin brut) montre que **la cible n'y est pour
rien** : la géométrie disparaît dès qu'une coordonnée à 3 ou 4 composantes est donnée **entre
`glBegin` et `glEnd`** (`glTexCoord3f`, `glTexCoord4f`, `glMultiTexCoord3f`, même avec
r = 0). Les tableaux de taille 3 ou 4 passent, `glTexCoord3f` posé avant `glBegin` aussi.
Trace : `BeginPrimitiveBuffer` est bien appelé, puis `EndPrimitiveBuffer` arrive avec
**n = −999480** ; à ce moment, `gctx+0x4858` (curseur d'écriture) vaut `0x287620`, dans le
tampon interne de GLEngine (`gctx+0x4854 = 0x287220`, + 4 sommets × 0x100), et non plus dans le
nôtre.

### 3.2 La lecture

* `_glTexCoord3f_Exec` (0x55adc) pose le **bit 0 de `gctx+0x486c`** (« coordonnée r/q
  employée ») ; `_glTexCoord2f_Exec` n'y touche pas. Toutes les variantes 3/4 composantes font de
  même.
* `_glEndRDirty_Exec` (0x440c8) et les `_gleFlush*TCLRDirtyFunc` appellent
  **`_gleForceToSoftwareTCL`** si le chemin T&L du pilote est actif (`gctx+0x7580`), si ce bit est
  posé, et si **`cfg+0x7a` vaut 0** (gardes `gctx+0x4664` et `gctx+0x5434` nulles).
* `_gleForceToSoftwareTCL` (0x103b38) recopie les sommets déjà écrits vers son format interne
  (`gctx+0x4858 = gctx+0x4854 + n·0x100`), clôt la primitive du pilote, puis
  **`_gleSwitchToNonRevertRenderer`** : il **change de renderer** (le rendu générique d'Apple)
  pour finir la primitive. Avec notre pilote, le compte rendu à `EndPrimitiveBuffer` est calculé
  entre deux tampons, et le dessin logiciel se fait hors de la surface de l'hôte : perdu.

`cfg+0x7a` est le « second verrou » de `docs/re/capacites-glengine.md` §5.3, que le pilote
GeForce3 pose à 1. **Le plugin le pose à 1** (`pomppc_geom_context`, `POMPPC_GL_RDIRTY=0` pour
revenir) : GLEngine reste sur nous, et le descripteur (4 composantes par unité) porte s, t, r, q.
`tcprobe` passe entièrement, et **les textures 3D et les cartes de cube passent désormais par
le chemin brut** (`tex3d`, `cube`, `tex13` : 0 échec, aucun refus).

### 3.3 Reste ouvert : `glMaterial` entre `glBegin` et `glEnd`

`_glMaterialfv_Exec` appelle `_gleForceToSoftwareTCL` **sans consulter `cfg+0x7a`** (seulement
`gctx+0x7580`, `gctx+0x4a10 ≥ 0`, `gctx+0x4664 == 0`). Au chemin brut, une primitive qui change
de matériau en cours de route est donc perdue, et la suivante garde l'ancien matériau (scène
`matbegin` : noir puis vert au lieu de rouge ; exact au chemin hérité et sous Apple). Défaut
antérieur à ce lot. Pistes : les codes 32..39 du descripteur (matériau par sommet,
`docs/re/descripteur-de-sommet.md`), qu'il faudrait porter dans `DRAW_RAW`.

## 4. La taille de point maximale initiale

GLEngine rend `GL_POINT_SIZE_MAX = 1` avant tout réglage — la valeur écrite dans la table
d'OpenGL 1.4, alors qu'`ARB_point_parameters` et les pilotes réels partent de la plus grande
taille. Le plugin lit le couple intact (MIN 0, MAX 1) comme « pas de borne haute » et envoie 64,
le maximum de l'hôte (`point_max`).

## 5. Le sommet hérité porte les coordonnées œil

Sonde `ptprobe` (points sous le rendu d'Apple, sommets vidés) : `+0x10` = coordonnées de
découpe, **`+0x20` = coordonnées œil** (x, y, z, w), `+0x30` = couleur. La taille dérivée n'y est
pas (sommets identiques avec et sans atténuation). Le plugin calcule donc l'atténuation des
points hérités lui-même (`pt_size`, règle de `qgpu_point_size`).

## 6. Ce qui en découle dans le plugin (`G.tex14`, `G.xbar`)

* Clés `QGPU_SK_TEX_LOD_BIAS0..3`, `QGPU_SK_COLOR_SUM` et `QGPU_SK_POINT_*` (v10), paramètres
  de texture `QGPU_TP_LOD_BIAS`, `_COMPARE_MODE`, `_COMPARE_FUNC`, `_DEPTH_MODE` ; biais bornés
  à ±16 chacun (OpenGL borne la somme, l'écart ne joue qu'au-delà de 12 niveaux).
* Textures de profondeur : base `0x1902`, formats `(0x1902, FLOAT/UINT/USHORT)`, 1D et 2D
  seulement, jamais un format de profondeur sur une base qui ne l'est pas (l'hôte refuserait
  la soumission).
* Couleur secondaire : au chemin brut, code 4 du descripteur (3 composantes) quand la somme est
  allumée hors éclairage, et `QGPU_SK_COLOR_SUM` explicite ; au chemin hérité,
  `DRAW_TRIANGLES_SEC` (v11) avec la secondaire lue en `+0x50`.
* Crossbar : `QGPU_CS_TEXTURE0 + n` (protocole v12, `docs/protocole-v12-crossbar.md`).
* Paramètres de point : l'hôte dérive la taille au chemin brut ; au chemin hérité, `pt_size`.
* Annonce : `cfg+0xb0 = 16`, bits 1 (`ARB_point_parameters`), 2 (`ARB_texture_env_crossbar`),
  12 (`ARB_shadow`), 13 (`ARB_depth_texture`), 33 (`EXT_stencil_wrap`), 38
  (`EXT_secondary_color`) ; `GL_VERSION = « 1.4 POMPPC-1.0 »`.

## 7. Preuve

Scène `tex14` : couleur secondaire (courante, en tableau, après la texture, coupée, ignorée
sous éclairage), biais de LOD (texture, unité, somme, négatifs), profondeur (luminance, trois
comparaisons, modes `ALPHA` et `INTENSITY`), stencil à enveloppement, crossbar (deux cas),
paramètres de point (atténuation, `MAX`, `MIN`), mélange au carré, `glMultiDrawArrays` :
**33 cas sur 33, plus `GL_MAX_TEXTURE_LOD_BIAS` ≥ 2, par le chemin brut et par le chemin hérité**, 15 échecs sous le rendu d'Apple
seul. `gltest` 39/39 (suite `gpu`), `v15` sans aucun cas non tenu, mode bureau (`glwin`) juste.
