# Textures 3D, niveaux et LOD : ce que GLEngine donne au pilote (Tiger 10.4.6)

Relevé du 19/09/2026, **par l'expérience dans l'invité** (VM de dev Linux, device qgpu v10,
RTX 4060 Ti) : scène `t3dprobe` de `guest/gltest` et sonde de cible du plugin
(`POMPPC_GL_T3DDUMP=1` avec `POMPPC_GLTRACE`), qui vide à l'échange d'image l'état GL, la table
des textures liées de l'unité 0 et chaque objet texture. Job : `tools/guest/jobs/t3dprobe`.
Méthode : **dimensions toutes distinctes** (8×2×4 puis 4×1×2) et paramètres à valeurs
distinctives, comparés à un vidage aux valeurs initiales. Une première sonde de 4×2×3 avait
échoué pour une raison sans rapport : OpenGL 1.2 exige des puissances de 2
(`glTexImage3D` → `GL_INVALID_VALUE`).

## 1. Où est la texture 3D

`TU_ENABLE` (état GL `+0x31c4 + u·0x7c + 0x10`) : bit 1<<k = cible active, et **la cible k
occupe l'emplacement k** de la table des textures liées du contexte du GLDriver (`ctx+0x10`,
`unité·0x14 + emplacement·4`) :

| Bit | Emplacement | Cible |
|---|---|---|
| 0x01 | 0 | carte de cube |
| 0x02 | **1** | **3D** (vérifié : seul emplacement dont les niveaux portent des données) |
| 0x04 | 2 | rectangle |
| 0x08 | 3 | 2D |
| 0x10 | 4 | 1D |

## 2. L'objet texture de GLEngine (`DT_PARAMS`, `dt+0x00`)

C'est l'objet de GLEngine lui-même, que le GLDriver pointe. Relevé par différence :

| Offset | Type | Champ | Valeur initiale |
|---|---|---|---|
| `+0x01` | u8 | **cible** = emplacement (1 3D, 3 2D, 4 1D) | — |
| `+0x02` | u16 | format interne demandé | — |
| `+0x04` | f32 | `GL_TEXTURE_PRIORITY` | 1 |
| `+0x10` `+0x12` | u16 | `WRAP_S`, `WRAP_T` (déjà connus) | `REPEAT` |
| **`+0x14`** | u16 | **`WRAP_R`** | `REPEAT` |
| `+0x16` `+0x18` | u16 | `MIN_FILTER`, `MAG_FILTER` (déjà connus) | |
| **`+0x30`** | f32 | **`GL_TEXTURE_MIN_LOD`** | −1000 |
| **`+0x34`** | f32 | **`GL_TEXTURE_MAX_LOD`** | 1000 |
| **`+0x38`** | f32 | **`GL_TEXTURE_LOD_BIAS`** | 0 |
| **`+0x3c`** | u16 | **`GL_TEXTURE_BASE_LEVEL`** | 0 |
| **`+0x3e`** | u16 | **`GL_TEXTURE_MAX_LEVEL`** | 1000 |
| `+0x5e`, `+0x64..0x6b` | | champs dérivés (bougent avec niveau de base et niveau max) | |
| **`+0xa4 + 0x18·l`** | | **tableau des niveaux** (ci-dessous) | |

Entrée du tableau des niveaux (0x18 octets) :

| Offset | Type | Champ |
|---|---|---|
| `+0x00` `+0x02` `+0x04` | u16 | largeur, hauteur, **profondeur** |
| `+0x06` | u16 | 0x0030 (drapeaux, non relevé) |
| `+0x08` | u16 | pixels par ligne des données |
| `+0x0a` | u16 | **lignes par tranche** des données |
| `+0x0c` `+0x0e` | u16 | format, type de l'application |
| `+0x10` | u32 | données (le même pointeur que `LV_DATA` du GLDriver) |
| `+0x14` | u32 | log2 de la profondeur (4 → 2, 2 → 1) **[H]** |

**La profondeur n'est que là.** La structure de niveau du GLDriver (`dt+0x78 + l·0x74`,
`LV_*`) est remplie par le rendu d'Apple, qui ne connaît pas la 3D : elle porte largeur et
hauteur (en u16, en u32 en `+0x1c`/`+0x20`, en flottants en `+0x24`/`+0x28`, en log2 en
`+0x2c`/`+0x30`), jamais la profondeur. `gldCreateTextureLevel(ctx, texture, ?, 0, niveau,
0xf, 0, 1)` ne transmet aucune dimension non plus. Les tranches sont rangées à la suite dans
les données, au format de l'application.

## 3. Ce que le rendu d'Apple fait (et ne fait pas)

* **`CTX_TEXTURING` (`ctx+0x6f4`) reste à 0** quand la seule texture active est 3D : c'est le
  verdict du GLDriver d'Apple, qui ne sait pas la 3D. Il ignore aussi le niveau de base et le
  niveau max (scène `texlod` : une chaîne partielle rendue complète par `MAX_LEVEL` sort
  blanche). Le plugin calcule désormais la complétude lui-même (`tex_complete`).
* **Niveaux et bornes de LOD : non tenus par Apple.** Scène `texlod` sous le rendu d'Apple
  seul : `BASE_LEVEL`, `MAX_LEVEL`, `MIN_LOD`, `MAX_LOD` sont tous ignorés (5 échecs sur 8).
  Sous le plugin avec un device v10 : **8 sur 8**.
* **Aucune 3D** : `glTexImage3D` → `GL_INVALID_VALUE` tant que `cfg+0xbe` vaut 0, et même
  posé, rien n'est échantillonné. Il n'y a donc **pas de repli** : une texture 3D hors du
  domaine accéléré sort fausse.

## 4. Le chemin brut ne transmet pas la géométrie 3D

Avec une texture 3D active et le verrou de géométrie posé (bit 0 du dispatch, descripteur
publié en `cfg+0x11c`, 4 composantes par unité de texture), **GLEngine n'appelle jamais
`BeginPrimitiveBuffer`** : la géométrie disparaît (0 `DRAW_RAW`, image noire). Pourquoi : non
relevé. Le plugin sort donc un tel état du chemin brut (motif `brut:texture-3d`) ; par le
chemin hérité, GLEngine transforme et remet **s, t, r, q** dans ses sommets, et tout est exact
(scène `tex3d`, 9 cas sur 9, par les deux réglages de `POMPPC_GL_GEOM`).

## 5. La couleur spéculaire séparée dans les sommets hérités

Scène `sepspec` : sous la spéculaire séparée, GLEngine met la couleur primaire (sans la
spéculaire) en **`+0x30`** du sommet et la **spéculaire en `+0x50`** (RGB). En couleur unique,
`+0x50` est nul et la spéculaire est déjà dans `+0x30`. Le rendu d'Apple ignore `+0x50` : la
spéculaire séparée n'est **pas tenue** sous lui. Le chemin brut la tient (l'hôte éclaire) ; le
chemin hérité ne la tient pas encore (le protocole n'a pas de dessin hérité avec couleur
secondaire).

## 6. Ce qui en découle dans le plugin

`TP_TARGET`, `TP_WRAP_R`, `TP_MIN_LOD`, `TP_MAX_LOD`, `TP_BASE_LEVEL`, `TP_MAX_LEVEL`,
`TP_LEVEL0`/`PL_D`/`PL_IMGH` dans `guest/gldriver/pomppc_accel.c` ; `GL_MAX_3D_TEXTURE_SIZE` =
256 annoncé seulement si le device est v10 avec `QGPU_CAP_GL14` (`cfg+0xc0`, voisine, n'est
pas touchée) ; niveaux et LOD relayés en v10 et, **avant la v10, refusés hors de leurs valeurs
initiales** (ils étaient ignorés en silence) ; le biais de LOD n'est pas relayé : GLEngine
annonce `GL_MAX_TEXTURE_LOD_BIAS = 0`, il est donc sans effet selon la spécification.
