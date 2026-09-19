# Ce que la chaîne TIENT vraiment — version et extensions annoncées (tâche 4.1)

Règle du projet : **rien n'est annoncé qui ne soit tenu.** Ce document établit, fonction par
fonction, ce qu'une application voit réellement quand elle tourne sur la chaîne complète
(plugin POMPPC + protocole qgpu v8 + repli exact sur le rendu logiciel d'Apple), et en déduit la
chaîne `GL_VERSION`, la liste d'extensions et les limites.

Trois statuts, jamais supposés :

* **(i) accéléré** — exécuté par le GPU de l'hôte, image vérifiée identique au rendu d'Apple ;
* **(ii) tenu par le repli** — le plugin refuse proprement, le rendu logiciel d'Apple le fait, et
  c'est exact ; lent, mais tenu ;
* **(iii) absent ou faux** — ni l'hôte ni GLEngine ne le font correctement : **non annonçable**.

Méthode : scène **`v15`** de `guest/gltest` — un sous-test par fonction, qui fait l'appel, note
l'erreur GL, dessine une case de 24×24 et compare le pixel du centre à ce qu'OpenGL exige. Jouée
sous le rendu d'Apple seul (`POMPPC_GL_DISABLE=1`) **et** sous le plugin. Les scènes `caps`,
`entry`, `qprobe`, `blendc`, `logicop`, `polymode`, `stipple`, `occl` complètent.

Date : **18/09/2026**, Tiger 10.4.6 PPC sous QEMU, device qgpu v8, backend OpenGL.

---

## 1. Le verdict, en trois lignes

* Les **points d'entrée** de 1.2, 1.3, 1.4 et 1.5 existent tous dans `libGL` et se laissent tous
  appeler sans erreur GL (scène `entry` : 21/21 présents). **Cela ne prouve rien** : GLEngine est
  un moteur complet, mais ce qui rend est le pilote, et il en refuse une partie.
* La plus haute version dont **toutes** les fonctions sont tenues est **OpenGL 1.1**. Ce qui
  l'empêche d'être 1.2 tient en un point : **les textures 3D sont absentes** et rien, dans la
  chaîne, ne peut les fournir.
* On annonce donc `GL_VERSION = "1.1 POMPPC-1.0"`, et on **ajoute trois extensions** au tableau de
  bits d'Apple : `GL_ARB_occlusion_query`, `GL_ARB_vertex_buffer_object`,
  `GL_EXT_blend_func_separate`. Soit **42 extensions** au lieu de 39.

---

## 2. Fonction par fonction

### OpenGL 1.1 — tenu par construction

Le `GLDriver` d'Apple annonce « 1.1 APPLE-1.1 » et fait tout 1.1 en logiciel ; le plugin
n'accélère qu'un sous-ensemble et se replie sur lui pour le reste, avec une image vérifiée
identique sur 28 scènes de `gltest`. Il n'y a donc rien à démontrer ici.

### OpenGL 1.2

| Fonction | Statut | Preuve |
|---|---|---|
| Textures 3D | **(i) sous le plugin v10** *(19/09/2026 : scène `tex3d` 9/9, `v15` TENU ; par le chemin hérité seulement, cf. `docs/re/textures-3d.md`)* ; **(iii) sous Apple** — ancien relevé : | `GL_MAX_3D_TEXTURE_SIZE = 0` ; `glTexImage3D` → `GL_INVALID_VALUE`. En forçant `cfg+0xbe = 256` (`POMPPC_GL_TRY3D=256`), l'appel **passe** mais rien n'est échantillonné : le quadrilatère sort blanc. Le protocole qgpu ne porte pas non plus les textures 3D (`TEX_IMAGE` est 2D). |
| BGRA (`GL_EXT_bgra`) | (i) | annoncée par Apple ; `convert_level` traduit BGRA et BGR ; scène `texfmt` |
| Pixels compactés | (i) | `GL_APPLE_packed_pixels` (liste fixe de GLEngine) ; 8888, 8888_REV, 1555_REV, 565, 4444 accélérés ; scène `texpack` |
| `GL_CLAMP_TO_EDGE` | (i) | `v15` « GL_CLAMP_TO_EDGE » TENU sous Apple **et** sous le plugin ; transmis en `QGPU_TP_WRAP_*` |
| `glDrawRangeElements` | (ii) | `v15` TENU ; `GL_EXT_draw_range_elements` déjà annoncée |
| Rescale normal | (i) | `GL_EXT_rescale_normal` déjà annoncée ; clé `QGPU_SK_RESCALE_NORMAL` (v7) |
| Couleur spéculaire séparée | **(i) par le chemin brut, (iii) sinon** *(19/09/2026)* | scène `sepspec` : blanc attendu, rendu par le chemin brut (l'hôte éclaire) ; noir par le chemin hérité ET sous le rendu d'Apple seul, qui ignore la spéculaire que GLEngine range en `+0x50` du sommet (`docs/re/textures-3d.md` §5) |
| Niveaux et LOD de texture (`BASE_LEVEL`, `MIN_LOD`…) | **(i) en v10 ; (iii) sous Apple** *(19/09/2026)* | scène `texlod` : 8/8 sous le plugin, 3/8 sous le rendu d'Apple seul, qui ignore les quatre paramètres (`docs/re/textures-3d.md` §3) |
| Sous-ensemble *imaging* | (ii) revendiqué par Apple (bit 0) | non vérifié de notre côté **[H]** |

→ **1.2 n'est pas tenu.**

### OpenGL 1.3

| Fonction | Statut | Preuve |
|---|---|---|
| Multitexture, 4 unités | (i) | `v15` « multitexture 4 unites » TENU (0,5⁴ = 16/255, exact) |
| Multitexture, 5 à 8 unités | (ii) | `v15` « multitexture 8 unites » TENU **avec le plugin** : au-delà de `QGPU_MAX_UNITS`, le plugin refuse (`unites>2`) et Apple rend juste. C'est ce qui autorise à laisser `GL_MAX_TEXTURE_UNITS = 8`. |
| `GL_COMBINE`, `GL_ADD`, dot3 | (i) | scène `comb` ; `QGPU_CB_*` (v5) |
| Cartes de cube | **(iii)** | `GL_MAX_CUBE_MAP_TEXTURE_SIZE = 0` ; `glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, …)` → `GL_INVALID_VALUE` |
| Compression de texture | **(iii), et silencieusement fausse** | `GL_NUM_COMPRESSED_TEXTURE_FORMATS = 0`, mais `glCompressedTexImage2D` avec un bloc DXT1 **ne rend aucune erreur** et l'image est fausse (gris au lieu de rouge). C'est le pire des cas : une application n'a aucun moyen de s'en apercevoir. |
| Multiéchantillonnage | **(iii)** | `GL_SAMPLE_BUFFERS = 0`, `GL_SAMPLES = 0` ; `glSampleCoverage` passe sans erreur et sans effet |
| Matrices transposées | (ii) | `v15` TENU ; `GL_ARB_transpose_matrix` déjà annoncée |
| `GL_CLAMP_TO_BORDER` | **non vérifié [H]** | — |

→ **1.3 n'est pas tenu.**

### OpenGL 1.4

| Fonction | Statut | Preuve |
|---|---|---|
| Mélange à couleur constante | (i) **nouveau (v8)** | scène `blendc`, témoins exacts, **0/255 sur toute l'image** par les deux chemins |
| Équations `GL_MIN` / `GL_MAX` | (i) **nouveau (v8)** | idem ; les facteurs sont bien ignorés |
| Mélange à facteurs séparés | (i) | `v15` TENU ; les quatre facteurs partent depuis la v2 |
| Soustraction et soustraction inverse | (i) | v2 |
| Coordonnée de brouillard | (i) | `v15` « glFogCoord » TENU ; `QGPU_VF_FOG` (v7), scène `fogz` |
| Mipmaps automatiques (`GL_GENERATE_MIPMAP`) | (ii) | `v15` TENU : un damier 16×16 très minifié rend bien le gris moyen `7f7f7f` ; `GL_SGIS_generate_mipmap` déjà annoncée |
| `glWindowPos` | (ii) | `v15` « glWindowPos + glDrawPixels » TENU ; `GL_ARB_window_pos` déjà annoncée |
| `glMultiDrawArrays` | (ii) | `GL_EXT_multi_draw_arrays` déjà annoncée ; appel sans erreur |
| Enveloppement du stencil (`INCR_WRAP`) | (i) | `QGPU_SOP_INCR_WRAP` (v6), `stencil_op_ok` — mais **Apple n'annonce pas** `GL_EXT_stencil_wrap` et nous ne l'avons pas vérifié au pixel : non annoncé **[H]** |
| Biais de LOD de texture | **[H]** | annoncé par Apple, mais `GL_MAX_TEXTURE_LOD_BIAS = 0` : sans effet |
| **Couleur secondaire** | **(iii)** | `v15` : `glEnable(GL_COLOR_SUM)` + `glSecondaryColor3f(0,5, 0, 0)` sur une couleur primaire `(0, 0,5, 0)` rend `008000` — la couleur secondaire n'est **pas** ajoutée. (Le chemin brut ne la porte pas non plus : elle passe en valeur courante.) |
| Paramètres de point | **(iii) partiel** | les appels passent ; mais l'atténuation par la distance n'est pas portée par le protocole (une seule `QGPU_SK_POINT_SIZE` pour toute la primitive). Le plugin la **refuse** désormais sur les deux chemins ; le comportement exact de GLEngine n'a pas été vérifié au pixel **[H]** |
| Textures de profondeur, comparaison d'ombre | **(iii)** | bits absents du tableau d'Apple ; le protocole n'a pas de format de profondeur pour les textures |
| `GL_MIRRORED_REPEAT` | **(iii)** | `v15` : `s = 1,25` rend le texel 0 (rouge) au lieu du texel 1 (bleu) — c'est un `GL_REPEAT` |

→ **1.4 n'est pas tenu.**

### OpenGL 1.5

| Fonction | Statut | Preuve |
|---|---|---|
| Objets tampon (VBO) | (ii) | `v15` « VBO » TENU sous Apple **et** sous le plugin, par les deux chemins : GLEngine les réalise lui-même et la géométrie arrive normalement dans `Begin`/`EndPrimitiveBuffer`. |
| **Requêtes d'occlusion** | (i) **nouveau (v8), tenu par NOUS** | scène `occl` : 4 096 / 2 048 / 0 échantillons exacts ; sous Apple seul, **0 partout** (bouchons). Détail : `docs/re/etat-v8.md` §4 |
| `glMapBuffer`, `glGetBufferSubData` | **non vérifié [H]** | le point d'entrée existe |

→ **1.5 n'est pas tenu** — il hérite de tout ce qui manque en 1.2, 1.3 et 1.4.

---

## 3. Ce qui est annoncé, et pourquoi

### `GL_VERSION` = « 1.1 POMPPC-1.0 »

`GL_VERSION` sort **tel quel** de `gldGetString` : GLEngine ne le recoupe ni avec les bits
d'extensions ni avec les limites (`docs/re/capacites-glengine.md` §2). C'est un `strcpy`, donc une
promesse que personne ne vérifie — raison de plus pour ne pas mentir. On y met la plus haute
version entièrement tenue, **1.1**, et le suffixe dit qui rend.

`GL_VENDOR` et `GL_RENDERER` sont inchangés (`POMPPC`, `POMPPC qgpu (OpenGL host GPU)`).

### Extensions ajoutées au tableau de bits

| bit | extension | pourquoi elle est tenue |
|---|---|---|
| 17 | `GL_ARB_occlusion_query` | tenue **par le plugin** sur le device v8 (scène `occl`). N'est posée **que si le device est un v8** : sur un device plus ancien, on ne l'annonce pas. |
| 20 | `GL_ARB_vertex_buffer_object` | tenue par GLEngine lui-même, vérifiée au rendu sur les deux chemins |
| 39 | `GL_EXT_blend_func_separate` | tenue et **accélérée** : les quatre facteurs partent au device depuis la v2 |

Résultat mesuré : **42 extensions** au lieu de 39, la liste étant exactement celle d'Apple plus ces
trois noms.

### Ce qui est volontairement **non** annoncé

`GL_ARB_texture_cube_map`, `GL_ARB_texture_compression`, `GL_EXT_texture_compression_s3tc`,
`GL_ARB_multisample`, `GL_ARB_shadow`, `GL_ARB_depth_texture`, `GL_EXT_secondary_color`,
`GL_ARB_texture_mirrored_repeat`, `GL_EXT_texture_rectangle`, `GL_ARB_point_parameters`,
`GL_ARB_texture_env_crossbar` (le plugin n'accepte une source croisée que pour sa propre unité),
`GL_EXT_stencil_wrap` et `GL_EXT_separate_specular_color` (faute de mesure). Et bien sûr aucune
version supérieure à 1.1.

### Limites : celles d'Apple, laissées telles quelles

| Limite | Valeur | Tenue par |
|---|---|---|
| `GL_MAX_TEXTURE_UNITS` (`cfg+0xb4`) | 8 | 4 accélérées, 5–8 par le repli — **vérifié** (`v15`) |
| `GL_MAX_TEXTURE_SIZE` (`cfg+0xbc`) | 4096 | ≤ 2048 accéléré (`QGPU_MAX_TEX_DIM`), au-delà par le repli — **vérifié** (`v15`, texture de 4096 de large) |
| `GL_MAX_3D_TEXTURE_SIZE`, `_CUBE_MAP_`, `_RECTANGLE_` | 0 | exact : ces cibles sont absentes |
| tailles de point et de ligne | 0,1–50 et 0,1–10 | le device va jusqu'à 64 ; le plugin borne et arrondit comme le fait OpenGL pour les points et lignes non lissés |
| `GL_MAX_LIGHTS`, `GL_MAX_CLIP_PLANES` | 8, 6 | `QGPU_MAX_LIGHTS` = 8, `QGPU_MAX_CLIP_PLANES` = 6 : exactement ce que l'hôte tient |

**Aucune limite n'a donc eu besoin d'être abaissée** : partout où le chemin accéléré s'arrête, le
repli sur le rendu d'Apple prend le relais et l'image est exacte. C'est le résultat le moins
attendu de cette tâche — `docs/re/capacites-glengine.md` §10(a).3 recommandait d'abaisser
`cfg+0xb4` à 4, ce qui aurait **retiré** une capacité que la chaîne tient.

---

## 4. Correction du relevé : la table bit → extension

`docs/re/capacites-glengine.md` §3.2 donnait la correspondance bit → nom par alignement des
longueurs de chaînes. **Elle est juste jusqu'au bit 23 et décalée ensuite.** L'expérience V3 du
§9 de ce même document a été faite le 18/09/2026 (`POMPPC_GL_ALLEXT=1` : les 79 bits allumés d'un
coup, la liste lue dans `gltest caps`) — GLEngine émettant les noms **dans l'ordre des bits**, la
lecture donne la table sans aucune déduction. 107 noms sortent : 25 fixes, puis 82 pour 79 bits,
trois bits émettant **deux** noms.

| bit | extension | bit | extension |
|---|---|---|---|
| 0 | `GL_ARB_imaging` | 40 | `GL_EXT_shadow_funcs` |
| 1 | `GL_ARB_point_parameters` | 41 | `GL_EXT_stencil_two_side` |
| 2 | `GL_ARB_texture_env_crossbar` | 42 | `GL_EXT_depth_bounds_test` |
| 3 | `GL_ARB_texture_border_clamp` | **43** | `GL_EXT_texture_compression_s3tc` **+** `_dxt1` |
| 4 | `GL_ARB_multitexture` | 44 | `GL_EXT_blend_equation_separate` |
| 5 | `GL_ARB_texture_env_add` | 45 | `GL_EXT_texture_mirror_clamp` |
| 6 | `GL_ARB_texture_cube_map` | 46 | `GL_APPLE_ycbcr_422` |
| 7 | `GL_ARB_texture_env_dot3` | 47 | `GL_APPLE_vertex_array_range` |
| 8 | `GL_ARB_multisample` | 48 | `GL_APPLE_texture_range` |
| 9 | `GL_ARB_texture_env_combine` | **49** | `GL_APPLE_float_pixels` **+** `GL_ATI_texture_float` |
| 10 | `GL_ARB_texture_compression` | 50 | `GL_APPLE_pixel_buffer` |
| 11 | `GL_ARB_texture_mirrored_repeat` | 51 | `GL_NV_point_sprite` |
| 12 | `GL_ARB_shadow` | 52 | `GL_NV_register_combiners` |
| 13 | `GL_ARB_depth_texture` | 53 | `GL_NV_register_combiners2` |
| 14 | `GL_ARB_shadow_ambient` | 54 | `GL_NV_blend_square` |
| 15 | `GL_ARB_fragment_program` | 55 | `GL_NV_texture_shader` |
| 16 | `GL_ARB_fragment_shader` | 56 | `GL_NV_texture_shader2` |
| **17** | **`GL_ARB_occlusion_query`** | 57 | `GL_NV_texture_shader3` |
| 18 | `GL_ARB_point_sprite` | 58 | `GL_NV_fog_distance` |
| 19 | `GL_ARB_texture_non_power_of_two` | 59 | `GL_NV_depth_clamp` |
| **20** | **`GL_ARB_vertex_buffer_object`** | 60 | `GL_NV_multisample_filter_hint` |
| 21 | `GL_ARB_pixel_buffer_object` | 61 | `GL_NV_fragment_program_option` |
| 22 | `GL_ARB_draw_buffers` | 62 | `GL_NV_fragment_program2` |
| 23 | `GL_EXT_compiled_vertex_array` | 63 | `GL_NV_vertex_program2_option` |
| 24 | `GL_EXT_framebuffer_object` | 64 | `GL_NV_vertex_program3` |
| **25** | `GL_EXT_texture_rectangle` **+** `GL_ARB_texture_rectangle` | 65 | `GL_ATI_point_cull_mode` |
| 26 | `GL_EXT_texture_env_add` | 66 | `GL_ATI_texture_mirror_once` |
| 27 | `GL_EXT_blend_color` | 67 | `GL_ATI_text_fragment_shader` |
| 28 | `GL_EXT_blend_minmax` | 68 | `GL_ATI_blend_equation_separate` |
| 29 | `GL_EXT_blend_subtract` | 69 | `GL_ATI_blend_weighted_minmax` |
| 30 | `GL_EXT_texture_lod_bias` | 70 | `GL_ATI_texture_env_combine3` |
| 31 | `GL_EXT_abgr` | 71 | `GL_ATI_separate_stencil` |
| 32 | `GL_EXT_bgra` | 72 | `GL_ATI_array_rev_comps_in_4_bytes` |
| 33 | `GL_EXT_stencil_wrap` | 73 | `GL_ATI_pn_triangles` |
| 34 | `GL_EXT_texture_filter_anisotropic` | 74 | `GL_ATI_texture_compression_3dc` |
| 35 | `GL_EXT_paletted_texture` | 75 | `GL_ATIX_pn_triangles` |
| 36 | `GL_EXT_shared_texture_palette` | 76 | `GL_SGIS_texture_edge_clamp` |
| 37 | `GL_EXT_separate_specular_color` | 77 | `GL_SGIS_texture_lod` |
| 38 | `GL_EXT_secondary_color` | 78 | `GL_SGI_color_matrix` |
| **39** | **`GL_EXT_blend_func_separate`** | | |

Contrôle : le `GLDriver` d'Apple pose `+0x124 = 0xFC0002B1`, `+0x128 = 0x00004001`,
`+0x12c = 0x00001000`, soit les bits {0, 4, 5, 7, 9, 26…31, 32, 46, 76}. Avec la table ci-dessus,
cela donne exactement les 14 noms lus : `ARB_imaging`, `ARB_multitexture`, `ARB_texture_env_add`,
`ARB_texture_env_dot3`, `ARB_texture_env_combine`, `EXT_texture_env_add`, `EXT_blend_color`,
`EXT_blend_minmax`, `EXT_blend_subtract`, `EXT_texture_lod_bias`, `EXT_abgr`, `EXT_bgra`,
`APPLE_ycbcr_422`, `SGIS_texture_edge_clamp`. **L'ancienne table donnait `EXT_stencil_wrap` à la
place de `EXT_bgra` et perdait `EXT_texture_env_add` : elle était fausse.**

> Ce décalage n'est pas anecdotique : la première version du code d'annonce, écrite d'après
> l'ancienne table, annonçait `GL_EXT_texture_rectangle` et `GL_EXT_secondary_color` — deux
> extensions que la chaîne **ne tient pas** — en croyant poser `GL_EXT_texture_env_add` et
> `GL_EXT_blend_func_separate`. C'est la scène `caps`, qui imprime la liste telle qu'une
> application la lit, qui l'a montré. Aucun bit d'extension ne doit être posé sans que cette
> scène l'ait confirmé.

---

## 5. Preuves

| Preuve | Résultat |
|---|---|
| `gltest caps` (version, extensions, limites, sous le plugin) | 1.1 POMPPC-1.0, 42 extensions, limites ci-dessus |
| `gltest entry` (un appel par fonction de chaque version, liaison normale) | 21/21 points d'entrée présents, aucun plantage ; seule erreur GL : `glTexImage3D` → 0x501, **identique sous le rendu d'Apple seul** ; image à 0/255 hors arêtes des trois côtés (Apple, chemin hérité, chemin brut) |
| `gltest v15` | tableau du §2 ; sous le plugin, la seule case qui diffère du rendu d'Apple est la texture **compressée** — cas sans résultat défini des deux côtés (l'un rend `747474`, l'autre `000000`), et non annoncé |
| `gltest occl`, `gltest qprobe` | comptes exacts avec le plugin, 0 sous Apple seul |
| 28 scènes de `gltest`, trois rendus chacune | toutes « OK (0 échec) », écart max hors arêtes ≤ 1/255 |
| Marble Blast Gold | démarre, lit `Version: 1.1 POMPPC-1.0`, joue ; extensions activées **inchangées** (voir §6) |

## 6. Ce que Marble Blast en fait

`~/Library/MarbleBlast/console.log`, avec et sans l'annonce (`POMPPC_GL_ANNOUNCE=0`) :

```
OpenGL driver information:
  Vendor: POMPPC
  Renderer: POMPPC qgpu (OpenGL host GPU)
  Version: 1.1 POMPPC-1.0          (sans l'annonce : 1.1 APPLE-1.1)
OpenGL Init: Enabled Extensions
  ARB_multitexture (Max Texture Units: 8)
  EXT_texture_env_combine
  EXT_packed_pixels
  EXT_fog_coord
  (ARB|EXT)_texture_env_add
OpenGL Init: Disabled Extensions
  EXT_paletted_texture, EXT_compiled_vertex_array, NV_vertex_array_range,
  ARB_texture_compression, EXT_texture_compression_s3tc,
  3DFX_texture_compression_FXT1, EXT_texture_filter_anisotropic,
  WGL_EXT_swap_control, NPatch tessellation, ATI_FSAA
```

**Les deux listes sont identiques avec et sans l'annonce** : Marble Blast ne teste ni les requêtes
d'occlusion, ni les objets tampon, ni le mélange à facteurs séparés, et il voyait déjà
`ARB_texture_env_add`. Seule la chaîne de version change. C'est le résultat voulu : l'annonce
n'ouvre aucune porte que la chaîne ne tiendrait pas.

---

## 7. Ce qu'il faudrait pour monter d'une version

Pour **1.2**, dans l'ordre de difficulté :

1. **Textures 3D** — c'est le seul vrai verrou. *(Protocole fait le 19/09/2026 : v10,
   `TEX_CREATE3` / `TEX_IMAGE3`, cf. `docs/protocole-v10-textures.md` ; reste le plugin.)*
   Il faut les ajouter au protocole (`TEX_IMAGE_3D`,
   coordonnée `r` déjà présente dans le format de sommet), puis poser `cfg+0xbe` : GLEngine
   accepte alors `glTexImage3D` (vérifié par la sonde `POMPPC_GL_TRY3D`) et nous remet les niveaux
   par `gldCreateTextureLevel`. Il n'y a **aucun repli possible** : le rendu d'Apple ne sait pas
   les échantillonner, donc tout ce qui les emploie doit passer par l'hôte.
2. Vérifier la couleur spéculaire séparée et les niveaux/LOD de texture, qui sont probablement
   déjà tenus par GLEngine.

Puis, pour **1.3** : cartes de cube (même mécanique que les textures 3D) et compression S3TC
(passée telle quelle à l'hôte, tâche 3.4) — le multiéchantillonnage (3.8) reste le plus lourd.
Pour **1.4** : la couleur secondaire (il faut trouver l'octet de `GL_COLOR_SUM` dans l'état de
GLEngine, cf. `docs/todo-gpu-3d.md`), les textures de profondeur et l'ombre (3.6),
`GL_MIRRORED_REPEAT` (une valeur de plus dans `QGPU_TP_WRAP_*`), et les paramètres de point
(taille par sommet, 3.5). Pour **1.5**, il ne restera alors que les objets tampon côté hôte
(2.1) — déjà tenus par GLEngine, donc une affaire de vitesse et non d'exactitude.
