# Négociation des capacités entre GLEngine et un pilote (Tiger 10.4.6)

Relevé en lecture seule, sans VM. Binaires : `GLEngine.bundle/GLEngine`, `GLDriver.bundle/GLDriver`
(rendu logiciel d'Apple, identifiant `0x0200`), `GLRendererFloat.bundle/GLRendererFloat` (`0x0400`),
`GeForce3GLDriver`, `ATIRage128GLDriver`, tous de l'image Tiger 10.4.6 PowerPC. Désassemblages
annotés produits par `tools/re/ppcanno.py`. Les adresses sont des adresses virtuelles du binaire
nommé en tête de section.

Convention : **[É]** = établi par lecture du code ; **[H]** = hypothèse à vérifier.

---

## 1. Résumé

1. **`GL_VERSION`, `GL_VENDOR`, `GL_RENDERER` viennent mot pour mot de `gldGetString` du pilote.**
   GLEngine ne les filtre pas et n'impose aucun plafond. **[É]**
2. **`GL_EXTENSIONS` est fabriqué par GLEngine**, pas par le pilote : une liste fixe de 25 chaînes
   (extensions réalisées par GLEngine lui-même), puis un **tableau de 79 bits** que le pilote pose
   dans un bloc de configuration. `gldGetString(GL_EXTENSIONS)` du pilote n'est **jamais appelé**. **[É]**
3. Ce bloc de configuration n'est **pas** `RendererInfo`. C'est le **5ᵉ argument (r7) de
   `gldCreateContext`**. Il porte les bits d'extensions (`+0x124`), toutes les limites
   (`GL_MAX_TEXTURE_SIZE`, `GL_MAX_TEXTURE_UNITS`, tailles de points et de lignes, anisotropie…)
   et **quatre octets de capacités en `+0x78..+0x7b`**. **[É]**
4. **Le verrou de l'axe 1 est l'octet `+0x79` de ce bloc.** `_gleInitGLIState` le recopie dans
   `ctx+0x7580` ; plus de 140 procédures de GLEngine s'y branchent. À 0, GLEngine transforme,
   éclaire et découpe lui-même (chemin actuel) ; à 1, il remet la géométrie **non transformée** au
   pilote par `BeginPrimitiveBuffer`/`EndPrimitiveBuffer` (+0x50/+0x54), `RenderVertexBuffer`
   (+0x4c) et `RenderVertexArray` (+0x70). **[É]**
5. `RendererInfo` ne contient **aucun** bit de fonctionnalité : identifiant, drapeaux (dont
   `0x100` accéléré), masques de formats de pixel, mémoire. Il sert au choix du renderer, pas à la
   négociation des capacités. **[É]**
6. `gldGetInteger` ne décrit **aucune** capacité : ce sont des paramètres CGL d'état
   (rectangle d'échange, intervalle d'échange, stockage client…). **[É]**

---

## 2. D'où viennent les chaînes — `_glGetString_Exec` (GLEngine, 0x11bc0-0x12338)

Aiguillage sur le nom :

```
00011c00  cmpwi   cr7, r4, 0x1f03     ; GL_EXTENSIONS -> 0x11d1c (fabrication locale)
00011c10  cmpwi   cr7, r4, 0x1f01     ; GL_RENDERER   -> 0x11c60
00011c1c  bgt     cr7, 0x11cf8        ; GL_VERSION (0x1f02) -> 0x11cf8
00011c20  cmpwi   cr7, r4, 0x1f00     ; GL_VENDOR     -> 0x11cf8
...
00011cf8  lwz     r12, 0x47bc(r29)    ; ctx+0x47bc = gldGetString du pilote
00011cfc  mr      r4, r2
00011d00  lwz     r3, 0x4688(r29)     ; ctx+0x4688 = contexte du pilote
00011d08  bctrl
0001230c  b       0x12324             ; la chaîne du pilote est rendue telle quelle
```

`ctx+0x4754` est la copie locale de la table `gld*` (§4) ; l'entrée 26 (`gldGetString`) tombe donc
bien en `ctx+0x4754 + 26*4 = ctx+0x47bc`. **[É]**

`GL_RENDERER` subit un seul traitement : si le nom contient `"FX"` et que le type du renderer
courant n'est pas `0x2402`, GLEngine recopie le nom dans `_new_rend_str.0` et écrit `'x'` (0x78)
juste après le `"FX"` (0x11ca0-0x11cec). Rustine pour les Voodoo ; sans effet pour nous. **[É]**

`GL_SHADING_LANGUAGE_VERSION` (0x8b8c) rend la constante `_gl_shader_lang_version` = `"1.10"`,
jamais demandée au pilote (0x11d10). **[É]**

### Chaînes rendues par les pilotes (`gldGetString`, sélecteur 0x1f02)

| Pilote | `GL_VERSION` | `GL_RENDERER` |
|---|---|---|
| `GLDriver` (0x0200) | `1.1 APPLE-1.1` | `Apple` (…) |
| `GLRendererFloat` (0x0400) | `1.2.1 APPLE` | `Apple Software Renderer` |
| `ATIRage128GLDriver` | `1.1 ATI-1.4.4` | `ATI Rage 128 OpenGL Engine` |
| `GeForce3GLDriver` | `1.3 NVIDIA-1.4.18` | `NVIDIA %s OpenGL Engine` |

(`GLDriver` 0x124a0-0x12518 : quatre constantes `_gl_vender` / `_gl_renderer` / `_gl_version` /
`_gl_extensions` ; noter que `_gl_extensions` existe mais que **GLEngine ne demande jamais 0x1f03
au pilote**.) **[É]**

> Conséquence directe pour la tâche 4.1 : **la version annoncée est un `strcpy`**. Rien, dans
> GLEngine, ne recoupe la chaîne de version avec les bits d'extensions ni avec les limites —
> `GLRendererFloat` annonce « 1.2.1 » tout en posant le bit `GL_ARB_fragment_shader`. **[É]**

---

## 3. `GL_EXTENSIONS` : liste fixe + 79 bits

### 3.1 La partie fixe (25 chaînes)

```
00011d1c  addi  r3, r3, 0x759e       ; tampon de sortie = ctx+0x759e
00011d24  addi  r2, r2, -0x18a8      ; = 0x110328  _engine_extensions
00011d34  addi  r2, r2, 0x60         ; = 0x110388  dernière entrée (incluse)
00011d38  lwz   r9, 0(r10)           ; boucle de concaténation, séparateur 0x20 (espace)
```

25 pointeurs (0x110328..0x110388 inclus), copiés **sans condition**. Les 25 chaînes, dans l'ordre
du `__TEXT,__const` (juste avant `0x109344`, première chaîne du tableau à bits) **[É pour le
contenu, H pour l'ordre exact]** :

`GL_ARB_transpose_matrix`, `GL_ARB_vertex_program`, `GL_ARB_vertex_blend`, `GL_ARB_window_pos`,
`GL_ARB_shader_objects`, `GL_ARB_vertex_shader`, `GL_EXT_multi_draw_arrays`,
`GL_EXT_clip_volume_hint`, `GL_EXT_rescale_normal`, `GL_EXT_draw_range_elements`,
`GL_EXT_fog_coord`, `GL_APPLE_client_storage`, `GL_APPLE_specular_vector`,
`GL_APPLE_transform_hint`, `GL_APPLE_packed_pixels`, `GL_APPLE_fence`,
`GL_APPLE_vertex_array_object`, `GL_APPLE_vertex_program_evaluators`, `GL_APPLE_element_array`,
`GL_APPLE_flush_render`, `GL_NV_texgen_reflection`, `GL_NV_light_max_exponent`,
`GL_IBM_rasterpos_clip`, `GL_SGIS_generate_mipmap`, `GL_ARB_shading_language_100`.

Ces extensions sont **réalisées par GLEngine sur le processeur émulé** : les annoncer ne coûte et
ne rapporte rien au plugin.

### 3.2 Le tableau de bits (79 entrées)

```
00011d70  li      r10, 0                 ; i = 0
00011d74  lwz     r9, 0x468c(r29)        ; r9 = bloc de configuration du renderer courant
00011d78  srawi   r2, r10, 5             ; i/32
00011d7c  slwi    r2, r2, 2
00011d80  add     r2, r2, r9
00011d84  clrlwi  r9, r10, 0x1b          ; i%32
00011d88  lwz     r0, 0x124(r2)          ; mot = bloc[0x124 + 4*(i/32)]
00011d8c  srw     r0, r0, r9
00011d90  andi.   r2, r0, 1              ; bit i
00011d94  beq     0x122ec                ; absent -> suivant
00011d98  cmplwi  cr7, r10, 0x4e         ; i > 78 ?
00011dac  addi    r2, r2, 0x1f0          ; table de sauts à 0x11dc0 (79 mots)
000122f0  cmpwi   cr7, r10, 78           ; borne de boucle
```

Le tableau de sauts (0x11dc0, 79 mots) précède 79 corps de cas à partir de 0x11efc, chacun
`addis r9,r31,15 / addi r9,r9,<imm> / b <concat>`. Les adresses de chaînes calculées
(`r31 = 0x11bd0`, base `0x101bd0`) vont de `0x109344` à `0x109b44` et **s'alignent exactement, à
4 octets près, sur les 79 chaînes consécutives du binaire** : la correspondance ci-dessous est donc
vérifiée par les longueurs, pas devinée. **[É]**

| bit | extension | bit | extension |
|---|---|---|---|
| 0 | `GL_ARB_imaging` | 40 | `GL_EXT_stencil_two_side` |
| 1 | `GL_ARB_point_parameters` | 41 | `GL_EXT_depth_bounds_test` |
| 2 | `GL_ARB_texture_env_crossbar` | 42 | `GL_EXT_texture_compression_s3tc` + `_dxt1` |
| 3 | `GL_ARB_texture_border_clamp` | 43 | `GL_EXT_blend_equation_separate` |
| 4 | `GL_ARB_multitexture` | 44 | `GL_EXT_texture_mirror_clamp` |
| 5 | `GL_ARB_texture_env_add` | 45 | `GL_EXT_framebuffer_object` |
| 6 | `GL_ARB_texture_cube_map` | 46 | `GL_APPLE_ycbcr_422` |
| 7 | `GL_ARB_texture_env_dot3` | 47 | `GL_APPLE_vertex_array_range` |
| 8 | `GL_ARB_multisample` | 48 | `GL_APPLE_texture_range` |
| 9 | `GL_ARB_texture_env_combine` | 49 | `GL_APPLE_float_pixels` + `GL_ATI_texture_float` |
| 10 | `GL_ARB_texture_compression` | 50 | `GL_APPLE_pixel_buffer` |
| 11 | `GL_ARB_texture_mirrored_repeat` | 51 | `GL_NV_point_sprite` |
| 12 | `GL_ARB_shadow` | 52 | `GL_NV_register_combiners` |
| 13 | `GL_ARB_depth_texture` | 53 | `GL_NV_register_combiners2` |
| 14 | `GL_ARB_shadow_ambient` | 54 | `GL_NV_blend_square` |
| 15 | `GL_ARB_fragment_program` | 55 | `GL_NV_texture_shader` |
| 16 | `GL_ARB_fragment_shader` | 56 | `GL_NV_texture_shader2` |
| 17 | `GL_ARB_occlusion_query` | 57 | `GL_NV_texture_shader3` |
| 18 | `GL_ARB_point_sprite` | 58 | `GL_NV_fog_distance` |
| 19 | `GL_ARB_texture_non_power_of_two` | 59 | `GL_NV_depth_clamp` |
| **20** | `GL_ARB_vertex_buffer_object` — *émis seulement si l'octet `ctx+0x7584` vaut 0* (0x11fec) | 60 | `GL_NV_multisample_filter_hint` |
| 21 | `GL_ARB_draw_buffers` | 61 | `GL_NV_fragment_program_option` |
| 22 | `GL_ARB_pixel_buffer_object` | 62 | `GL_NV_fragment_program2` |
| 23 | `GL_EXT_compiled_vertex_array` | 63 | `GL_NV_vertex_program2_option` |
| 24 | `GL_EXT_texture_rectangle` + `GL_ARB_texture_rectangle` | 64 | `GL_NV_vertex_program3` |
| 25 | `GL_EXT_texture_env_add` | 65 | `GL_ATI_point_cull_mode` |
| 26 | `GL_EXT_blend_color` | 66 | `GL_ATI_texture_mirror_once` |
| 27 | `GL_EXT_blend_minmax` | 67 | `GL_ATI_text_fragment_shader` |
| 28 | `GL_EXT_blend_subtract` | 68 | `GL_ATI_blend_equation_separate` |
| 29 | `GL_EXT_texture_lod_bias` | 69 | `GL_ATI_blend_weighted_minmax` |
| 30 | `GL_EXT_abgr` | 70 | `GL_ATI_texture_env_combine3` |
| 31 | `GL_EXT_bgra` | 71 | `GL_ATI_separate_stencil` |
| 32 | `GL_EXT_stencil_wrap` | 72 | `GL_ATI_array_rev_comps_in_4_bytes` |
| 33 | `GL_EXT_texture_filter_anisotropic` | 73 | `GL_ATI_pn_triangles` |
| 34 | `GL_EXT_paletted_texture` | 74 | `GL_ATI_texture_compression_3dc` |
| 35 | `GL_EXT_shared_texture_palette` | 75 | `GL_ATIX_pn_triangles` |
| 36 | `GL_EXT_separate_specular_color` | 76 | `GL_SGIS_texture_edge_clamp` |
| 37 | `GL_EXT_secondary_color` | 77 | `GL_SGIS_texture_lod` |
| 38 | `GL_EXT_blend_func_separate` | 78 | `GL_SGI_color_matrix` |
| 39 | `GL_EXT_shadow_funcs` | | |

### 3.3 Ce que pose chaque pilote

Les pilotes font un **`ori` / `oris`** : ils ajoutent des bits, ils n'en retirent jamais (le bloc
est mis à zéro par GLEngine avant l'appel, §4.1).

| Pilote | `+0x124` | `+0x128` | `+0x12c` |
|---|---|---|---|
| `GLDriver` (logiciel) `_gldSetConfigData` 0x1ce8-0x1d24 | `0xFC0002B1` | `0x00004001` | `0x00001000` |
| `GLRendererFloat` 0x2b50-0x2bf8 | `0xFE0183B9` | `0x004250C3` | `0x00001000` |
| `ATIRage128GLDriver` 0xa66c-0xa730 | `0xC4800230` | `0x00044041` | `0x00005000` |
| `GeForce3GLDriver` 0x3ab64-0x3ac08 | `0xFEB23FFF` | (par étapes, cf. ci-dessous) | (idem) |

Extrait (`GLDriver`, 0x1ce8) :

```
00001ce8  lwz   r9, 0x124(r29)
00001cec  lwz   r10, 0x128(r29)
00001cf0  lwz   r2, 0x12c(r29)
00001cf4  ori   r9, r9, 0x2b1
00001cfc  oris  r9, r9, 0xfc00
00001d04  ori   r10, r10, 0x4001
00001d08  ori   r2, r2, 0x1000
00001d20  stw   r9, 0x124(r29)
```

Décodage du rendu logiciel d'Apple (14 bits) : `ARB_imaging`, `ARB_multitexture`,
`ARB_texture_env_add`, `ARB_texture_env_dot3`, `ARB_texture_env_combine`, `EXT_blend_color`,
`EXT_blend_minmax`, `EXT_blend_subtract`, `EXT_texture_lod_bias`, `EXT_abgr`, `EXT_bgra`,
`EXT_stencil_wrap`, `APPLE_ycbcr_422`, `SGIS_texture_edge_clamp`. Avec les 25 fixes : 39
extensions — c'est exactement ce que voit aujourd'hui une application sous notre plugin, puisque
nous transmettons l'appel à `GLDriver`. **[É]**

`GeForce3` construit son mot `+0x128` en plusieurs passes, dont certaines conditionnées par un
drapeau de l'adaptateur (`lwz r0, 16660(r25); andi. r2, r0, 8`, 0x3abb0/0x3abe0/0x3abf4) : le
tableau complet des extensions NVIDIA dépend du modèle. **[H]** — non dépouillé en détail, non
nécessaire pour nous.

---

## 4. Le bloc de configuration (5ᵉ argument de `gldCreateContext`)

### 4.1 Comment GLEngine le fabrique — `_gliCreateContext` (GLEngine, 0x2d18-0x3170)

GLEngine tient, dans le contexte, un enregistrement de **0x2e0 octets par renderer** :
`B(i) = ctx + 0x85ac + i*0x2e0` (les accès se font sous la forme `ctx + 0x10000 + i*0x2e0 - 0x7a54`).

```
00002ea0  addi   r25, r21, -0x7a54      ; B(i)
00002eb8  bl     _memset                ; B(i) mis à zéro sur 0x2e0 octets
...
00002fc4  addi   r3, r9, -0x7a54        ; arg1 : &B+0x00  (sortie : contexte du pilote)
00002fc8  lwz    r11, -0x777c(r9)       ; B+0x2d4 = structure du plugin
00002fe8  lwz    r12, 0x12c(r11)        ; plugin+0x12c = gldCreateContext (entrée 6)
00002ff4  addi   r9, r7, -0x7920        ; arg7 : B+0x134
00002ffc  addi   r7, r7, -0x7a50        ; arg5 : B+0x04  <-- LE BLOC DE CONFIGURATION
00003004  bctrl
...
00003070  lwz    r0, -0x7a54(r21)
00003074  addi   r2, r21, -0x7a50
00003078  stw    r2, 0x468c(r26)        ; ctx+0x468c = &bloc de configuration
00003080  stw    r0, 0x4688(r26)        ; ctx+0x4688 = contexte du pilote
00003088  lwz    r4, -0x777c(r21)
0000308c  addi   r3, r26, 0x4754
00003098  addi   r4, r4, 0x114
0000309c  bl     _memcpy                ; 0xf4 = 61*4 octets : plugin+0x114.. -> ctx+0x4754..
```

Signature relevée : **`gldCreateContext(void **outCtx, pixelformat, shared, sharedCtx,
void *config /* = B+0x04 */, void *glstate /* = ctx+0x360 */, void *x /* = B+0x134 */)`**. **[É]**

- `ctx+0x4754 + 4*n` = entrée `gld*` n° n (d'où `gldGetString` en `ctx+0x47bc`). **[É]**
- `ctx+0x360` est la base de l'état GL déjà documentée (`docs/gpu-3d-tiger.md` §4.3, offsets
  relatifs à `drvctx+0x0c`) : `GLDriver` range cet argument en `drvctx+0x0c` (0x1e3c). **[É]**
- Le 7ᵉ argument (`B+0x134`, c'est-à-dire `config+0x130`) est rangé par `GLDriver` en
  `drvctx+0x10` (0x1e44). Rôle non établi. **[H]**

Le bloc étant **mis à zéro avant l'appel**, tout ce que le pilote n'écrit pas vaut 0.

### 4.2 Contenu du bloc — offsets confirmés par `_gleGetState` (GLEngine, 0x12b80-0x15f6c)

`_gleGetState` est le `glGetXxxv` interne ; chaque cas fait `lwz r2, 0x468c(r3)` puis lit le bloc.
Mise en correspondance automatique des constantes d'énumération avec les offsets :

| offset | type | signification | GLDriver | GLRendererFloat | Rage128 | GeForce3 |
|---|---|---|---|---|---|---|
| `+0x00` | u32 | masque de classe du renderer (bits 0 et 1 comparés en 0x9530) | 4 | – | 2 | 10 |
| `+0x04` | f32 | échelle de profondeur (recopie de `drvctx+0x14`) | 1.0 | 1.0 | 1.0 | 1.0 |
| `+0x08` | u32 | `GL_SUBPIXEL_BITS` (0x0D50) | 3 | – | 3 | 3 |
| `+0x0c`/`+0x10` | u32 | dimensions maximales (`GL_MAX_RENDERBUFFER_SIZE_EXT` = `+0x10`) | 2048 | – | 2048 | 2048 |
| `+0x14` | u32 | `GL_MAX_DRAW_BUFFERS` (0x8824) | 1 | – | 1 | 1 |
| `+0x1c`/`+0x20` | u32 | (lus par 0x8A10) | 0 | – | 32 | 32 |
| `+0x24`/`+0x25`/`+0x26`/`+0x27` | u8 | drapeaux de tampon (double, stéréo…) recopiés de `drvctx+0xe0/0xe1` | | | | |
| `+0x28` | u32 | `GL_INDEX_BITS` (0x0D51) | | | | |
| `+0x2c`..`+0x38` | u32 ×4 | `GL_RED/GREEN/BLUE/ALPHA_BITS` | | | | |
| `+0x3c` | u32 | `GL_DEPTH_BITS` | | | | |
| `+0x40` | u32 | `GL_STENCIL_BITS` | | | | |
| `+0x44`..`+0x50` | u32 ×4 | `GL_ACCUM_*_BITS` (0x0D58..0x0D5B) | | 64 | | |
| `+0x54` | u32 | `GL_AUX_BUFFERS` (0x0C00) | | | | |
| `+0x58`/`+0x5c` | u32 | `GL_SAMPLE_BUFFERS` / `GL_SAMPLES` (0x80A8/0x80A9) | 0 | 0 | 0 | |
| `+0x60`/`+0x64` | f32 ×2 | `GL_POINT_SIZE_RANGE` (0x0B12, 0x846D) | 1.0 / 50.0 | 1.0 / 50.0 | 1.0 / 10.0 | 1.0 / ? |
| `+0x68` | f32 | `GL_POINT_SIZE_GRANULARITY` (0x0B13) | | | 1.0 | 0.125 |
| `+0x6c`/`+0x70` | f32 ×2 | `GL_LINE_WIDTH_RANGE` (0x0B22, 0x846E) | 1.0 / 10.0 | 1.0 / 10.0 | 1.0 / 10.0 | 0.5 / 10.0 |
| `+0x74` | f32 | `GL_LINE_WIDTH_GRANULARITY` (0x0B23) | | | 1.0 | 0.125 |
| **`+0x78`** | u8 | capacité (§5.3) | **0** | **0** | **1** | **1** |
| **`+0x79`** | u8 | **« le pilote fait la T&L »** (§5) | **0** | **0** | **0** | **1** |
| **`+0x7a`** | u8 | capacité (§5.3) | **0** | **1** | **0** | **1** |
| **`+0x7b`** | u8 | capacité (§5.3) | **0** | **0** | **0** | **1** |
| `+0x7c`..`+0x87` | u16 ×6 | table de 6 valeurs {3,2,1,6,5,4} ; le Rage128 pose en plus le bit 0x8000. Rôle non établi **[H]** | 0 | 0 | 0x8003… | 3,2,1,6,5,4 |
| `+0x88` | u32 | `GL_MAX_ELEMENTS_VERTICES` (0x80E8) | 1000 | 1000 | 2048 | 2048 |
| `+0x8c` | u32 | `GL_MAX_VERTEX_ARRAY_RANGE_ELEMENT` (0x8520) | 0 | 0 | 0 | 0xFFFFF |
| `+0x94` | u32 | (lu en 0x8bd8, 0x1f260) | 3 | 7 | 0 | |
| `+0xa0`/`+0xa4` | u32 | `GL_MAX_SHININESS_NV` / `GL_MAX_SPOT_EXPONENT_NV` (0x8504/0x8505) | 128 | 128 | 128 | 1024 |
| `+0xac` | f32 | `GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT` (0x84FF) | 1.0 | 1.0 | 1.0 | 8.0 |
| `+0xb0` | f32 | `GL_MAX_TEXTURE_LOD_BIAS` (0x84FD) | 0 | 16.0 | 1.0 | 16.0 |
| **`+0xb4`** | u16 | **`GL_MAX_TEXTURE_UNITS` (0x84E2)** | **8** | **8** | **2** | **4** |
| `+0xb6` | u16 | `GL_MAX_TEXTURE_IMAGE_UNITS` (0x8872) | 8 | 8 | 2 | 4 |
| `+0xb8` | u16 | (0) | 0 | 0 | 0 | 0 |
| `+0xba` | u16 | `GL_MAX_TEXTURE_COORDS` (0x8871) | 8 | 8 | 2 | 4 |
| **`+0xbc`** | u16 | **`GL_MAX_TEXTURE_SIZE` (0x0D33)** | **4096** | **16384** | **1024** | **4096** |
| `+0xbe` | u16 | `GL_MAX_3D_TEXTURE_SIZE` (0x8073) | 0 | 16384 | 0 | 4096 |
| `+0xc0` | u16 | taille maximale (cible non identifiée) **[H]** | 0 | 16384 | 1024 | 4096 |
| `+0xc2` | u16 | `GL_MAX_CUBE_MAP_TEXTURE_SIZE` (0x851C) | 0 | 0 | 0 | 4096 |
| `+0xc6`/`+0xc7` | u8 | valeurs par défaut d'indications (`GL_FOG_HINT`…) | 0 | 0 | 0 | 3 / 1 |
| `+0xe8` | u32 | `GL_MAX_RECTANGLE_TEXTURE_SIZE_ARB` (0x84F8) | 0 | 16384 | 0 | 4096 |
| `+0xec`..`+0x11c` | u16/u32 | limites de programmes (instructions, paramètres, indirections) | 0 | remplis | 0 | remplis |
| `+0x124`/`+0x128`/`+0x12c` | u32 ×3 | **bits d'extensions** (§3) | | | | |

`GL_MAX_TEXTURE_UNITS` et `GL_MAX_TEXTURE_SIZE`, extraits (GLEngine) :

```
000159f0  lwz  r2, 0x468c(r3)     ; cas 0x84E2 (GL_MAX_TEXTURE_UNITS)
000159f4  lhz  r0, 0xb4(r2)
000144a4  lwz  r2, 0x468c(r3)     ; cas 0x0D33 (GL_MAX_TEXTURE_SIZE)
000144a8  lhz  r0, 0xbc(r2)
```

Et la validation de `glActiveTexture` (0x16664) borne l'unité par `max(+0xb6, +0xba)` :

```
0001666c  lwz  r2, 0x468c(r3)
00016670  lhz  r0, 0xb6(r2)
00016674  cmpw cr7, r4, r0
00016678  blt  cr7, 0x166a0
0001667c  lhz  r0, 0xba(r2)
```

**Conséquence** : c'est ce bloc, et lui seul, qui décide de toutes les limites annoncées. Notre
plugin hérite aujourd'hui de celles du `GLDriver` d'Apple (8 unités de texture, 4096 de côté),
qu'il ne sait pas tenir au-delà de 4 unités — GLEngine reprend alors la main (repli documenté
dans `docs/gpu-3d-tiger.md` §4.5).

---

## 5. Le point clé : qui transforme les sommets

### 5.1 Le drapeau

`_gleInitGLIState` (GLEngine, 0x51cc-0x54d4) :

```
0000544c  lwz  r10, 0x468c(r30)   ; bloc de configuration
00005498  lbz  r0, 0x79(r10)      ; <-- octet de capacité « T&L matérielle »
000054a0  stb  r0, 0x7580(r30)    ; ctx+0x7580
000054a8  lbz  r2, 0x7580(r30)
000054b0  stb  r2, 0x7581(r30)    ; ctx+0x7581
```

**C'est le seul endroit où `ctx+0x7580` naît.** Il n'y a pas d'autre condition : ni l'identifiant du
renderer, ni le drapeau « accéléré » de `RendererInfo`, ni la version annoncée n'entrent en compte.
Le seul endroit de GLEngine qui traite spécialement les identifiants d'Apple est le rapport CGL
(§5.4), pas la décision elle-même. **[É]**

### 5.2 Ce que `ctx+0x7580` commande

Plus de 140 procédures s'y branchent. Les deux plus parlantes :

```
_gleCompileVertexArray:  ; 0xdad24-0xdad38
  000dad24  lbz  r0, 0x7580(r3)
  000dad2c  beq  cr7, 0xdad34
  000dad30  b    0xda498       ; _gleCompileTCLVertexArray   (pilote)
  000dad34  b    0xda8b4       ; _gleCompileClippedVertexArray (GLEngine)

_gleExecuteVertexArray:  ; 0xdc08c-0xdc0a0
  000dc08c  lbz  r0, 0x7580(r3)
  000dc098  b    0xdad38       ; _gleExecuteTCLVertexArray
  000dc09c  b    0xdae98       ; _gleExecuteClippedVertexArray
```

La liste (extraite par recherche des `lbz rX, 0x7580(...)`) couvre tout le pipeline :
`_glBegin_Exec`, `_glFlush_Exec`, `_glClear_Exec`, `_glSwap_Exec`, `_glMaterial*_Exec`,
tous les `_glRasterPos*_Exec`, `_glDrawPixels_Exec`, `_glReadPixels_Exec`, `_glCopyTex*_Exec`,
`_glBeginQuery_Exec`, `_gleDrawArraysOrElements_{Exec,IMM,VBO,VAR,CVA}_Exec`,
`_gleSelectVertexSubmitFunc`, `_gleUpdatePrimitiveData`, `_gleSetColorMaterialEnable`,
`_gleBuildVertexFuncNO`, `_gleCheckFramebufferStatus`, `_gliSwapBuffers`, et toute la famille
`_gleFlush*TCLRDirtyFunc` / `_gleVPFlush*TCLRDirty`. **[É]**

### 5.3 Les entrées « hautes » réellement utilisées

Table de procédures de rastérisation = `ctx+0x4698` (2ᵉ argument de `gldInitDispatch` :
`00009f04  addi r4, r3, 0x4698`). Donc `ctx+0x4698 + k` = procédure `+k`.

| appelant (GLEngine) | lit | procédure |
|---|---|---|
| `_gleBeginPrimitiveTCLFunc` 0x1d160, `_gleBeginPrimitiveTCLRDirtyFunc` 0xf38e8 | `ctx+0x46e8` | **+0x50 `BeginPrimitiveBuffer`** |
| `_gleRenderPrimitiveTCLFunc` 0x1ee28, `_gleForceToSoftwareTCL` 0x103ba4 | `ctx+0x46ec` | **+0x54 `EndPrimitiveBuffer`** |
| `_gleExecuteTCLVertexArray` 0xdada8 / 0xdae38, `_gleExecuteClippedVertexArray` 0xdaed8 | `ctx+0x46e4` | **+0x4c `RenderVertexBuffer`** |
| `_gleExecuteVertexArrayRange` 0x44fe0 | `ctx+0x4708` | **+0x70 `RenderVertexArray`** |

Mode immédiat en T&L matérielle (`_gleBeginPrimitiveTCLFunc`, 0x1d120) :

```
0001d160  lwz   r12, 0x46e8(r30)      ; BeginPrimitiveBuffer
0001d164  addi  r5, r1, 0x40          ;   arg3 : &pointeur de sortie
0001d168  lwz   r3, 0x4688(r30)       ;   arg1 : contexte du pilote
0001d16c  lha   r4, 0x4a10(r30)       ;   arg2 : mode de primitive
0001d174  bctrl
0001d184  mullw r0, r29, r0           ; r29 = pas d'un sommet (ctx+0x4880 ou +0x4882)
0001d1a4  stw   r0, 0x485c(r30)       ; fin du tampon
0001d1b0  stw   r3, 0x4858(r30)       ; curseur d'écriture
```

GLEngine écrit ensuite les sommets **bruts** directement dans le tampon rendu par le pilote, puis
`_gleRenderPrimitiveTCLFunc` appelle `+0x54`. Le pas de sommet est calculé dans
`_gleSelectVertexSubmitFunc` (0x9e2c) à partir de l'objet de tableau de sommets courant :
`lbz r0, 2(r2); slwi r0, r0, 2` — le format du sommet vient donc d'un descripteur, non d'une taille
fixe de 0x100 octets comme dans le chemin actuel. **[É]** (Le détail de ce descripteur relève de la
tâche 1.2.)

**`+0x7a` est un second verrou.** `_gleFlushAtomicTCLRDirtyFunc` (0xf2814) :

```
000f2828  lbz  r0, 0x7580(r3)      ; T&L matérielle ?
000f2834  lwz  r0, 0x486c(r3)      ; état de rastérisation « sali » en cours de primitive ?
000f2840  lwz  r2, 0x468c(r3)
000f2844  lbz  r0, 0x7a(r2)
000f284c  bne  cr7, 0xf288c        ; +0x7a != 0 : on reste sur le pilote
000f2868  bl   0x103b38            ; _gleForceToSoftwareTCL : repli logiciel
```

Autrement dit : **avec `+0x79 = 1` et `+0x7a = 0`, le moindre changement d'état en cours de
primitive renvoie GLEngine au chemin logiciel** (c'est le cas du Rage 128). Les six variantes
`_gleFlush{Atomic,LineLoop,LineStrip,TriQuadStrip,TriangleFan,Polygon,Primitive}TCLRDirtyFunc` et
leurs jumelles `_gleVPFlush*` font toutes ce test. **[É]**

`+0x78` est lu par `_gleUpdatePolyMode` (0x9508-0x9518 : si `+0x78 == 0`, un drapeau `ctx+0x34de`
est forcé à 1) et par `_gleDrawArraysOrElements_Exec` (0xa7434) et `_gleDrawArraysOrElements_CVA_Exec`
(0xa848c). `+0x7b` n'est lu que par `_glDrawPixels_Exec` (0x642bc) et `_glDrawPixels_ListExec`
(0x63a78). Sémantique précise non établie. **[H]**

### 5.4 Ce que CGL en rapporte

`_gliGetInteger` (GLEngine, 0x2238-0x29a4), sélecteurs 310 et 311 — soit
`kCGLCPGPUVertexProcessing` et `kCGLCPGPUFragmentProcessing` :

```
00002320  cmpwi cr7, r4, 0x136     ; 310
00002324  beq   cr7, 0x2914
00002914  lbz   r0, 0x7580(r30)    ; -> rend (ctx+0x7580 == 1)
...
00002328  cmpwi cr7, r4, 0x137     ; 311
0000232c  beq   cr7, 0x2928
0000293c  lwz   r9, -0x777c(r2)    ; structure du plugin du renderer courant
00002940  lwz   r0, 0x104(r9)      ; identifiant du renderer
00002944  rlwinm r0, r0, 0, 0x10, 0x17   ; & 0xff00
00002948  cmpwi cr7, r0, 0x200     ; GLDriver d'Apple
00002950  cmpwi cr7, r0, 0x400     ; GLRendererFloat
00002958  li    r0, 0              ;   -> forcé à 0
00002960  lbz   r0, 0x7581(r30)    ; sinon : ctx+0x7581
```

C'est **la sonde la moins chère** pour vérifier tout ce document depuis l'invité : notre
identifiant est `0x7700`, donc non masqué. **[É]**

### 5.5 Ce que fait le pilote logiciel d'Apple des entrées « hautes »

GLEngine **appelle toujours** `gldCreateVertexArray` et `gldCreatePipelineProgram`, pour **tous les
renderers du contexte**, T&L matérielle ou pas — ce que confirme notre trace :

```
_gleCreatePluginVertexArray:  ; GLEngine 0x65e0-0x6664
  00006614  lwz   r2, -0x777c(r30)   ; structure du plugin (renderer i)
  0000661c  lwz   r3, -0x7a54(r30)   ; contexte du pilote (renderer i)
  00006624  lwz   r12, 0x1a4(r2)     ; plugin+0x1a4 = gldCreateVertexArray (entrée 36)
  00006640  bctrl                    ; boucle sur les ctx+0x85a4 renderers

_gleCreatePluginPipelineProgram:  ; GLEngine 0x62b8-0x6330
  000062f8  lwz   r12, 0x190(r2)     ; plugin+0x190 = gldCreatePipelineProgram (entrée 31)
```

Côté `GLDriver` (logiciel), ce sont des bouchons :

```
_gldCreateVertexArray:      00019bcc  li r0,4 / li r3,0 / stw r0,0(r4) / blr
_gldModifyVertexArray:      00019bdc  li r3,0 / blr
_gldFlushVertexArray:       00019be4  li r3,0 / blr
_gldCreatePipelineProgram:  00011d80  li r0,4 / li r3,0 / stw r0,0(r4) / blr
_gldModifyPipelineProgram:  00011d90  li r3,0 / blr
_gldAllocVertexBuffer:      00004d1c  li r0,0 / li r3,0 / stw r0,0(r5) / blr
_gldCreateBuffer:           000017e8  li r0,0 / li r3,0 / stw r0,0(r4) / blr
_gldRenderVertexArray:      00019bf4  li r3,0 / blr
_gldRenderVertexBuffer:     00004d34  blr
_gldBeginPrimitiveBuffer:   00004d10  li r3,0 / blr
_gldEndPrimitiveBuffer:     00004d18  blr
```

Et pourtant `gldInitDispatch` **installe quand même** ces procédures dans la table (0x30d0-0x3108 :
`+0x4c` `_gldRenderVertexBuffer`, `+0x50` `_gldBeginPrimitiveBuffer`, `+0x54`
`_gldEndPrimitiveBuffer`, `+0x70` `_gldRenderVertexArray`, `+0x80` `_gldBufferSubData`). Elles ne
sont jamais appelées, puisque `+0x79 = 0`. **[É]**

> Traduction pratique : **le fait que notre plugin voie passer `gldCreateVertexArray` et
> `gldCreatePipelineProgram` ne veut rien dire sur la T&L.** Le seul signe qui compte est
> `BeginPrimitiveBuffer` / `RenderVertexBuffer` / `RenderVertexArray`.

---

## 6. `gldGetRendererInfo` : ce que la structure contient (et ne contient pas)

`GLDriver` 0x123c8-0x124a0, `GeForce3GLDriver` 0x6d8c, `ATIRage128GLDriver` 0x6268.

| offset | `GLDriver` (logiciel) | Rage128 | GeForce3 | lecture |
|---|---|---|---|---|
| `+0x00` | 0 | objet d'accélérateur | objet d'accélérateur | `stw r29, 0(r28)` |
| `+0x04` | `0x200` | `id \| (unité<<24)` | `id \| (unité<<24)` | |
| `+0x08` | `0x65D` | `0x2513` | `0xA513` | drapeaux ; **bit `0x100` = accéléré**, absent du logiciel |
| `+0x0c` | 13 | 13 | 13 | constante identique partout |
| `+0x10` | OU de `glsGetRasterSpecs[i]+0x100` | `0x8400` | `0x8400` | modes couleur |
| `+0x14` | `0xC0C000` | `0x00808000` | `0x00808000` | modes d'accumulation |
| `+0x18` | OU de `glsGetRasterSpecs[i]+0x104` | `0x1C01` | `0x1C01` | modes de profondeur |
| `+0x1c` | `0x81` | `0x80` | `0x80` | modes de stencil |
| `+0x20`..`+0x28` | 0 (u16 ×5) | recopie de `r29` | recopie de `r29` | rôle non établi **[H]** |
| `+0x24` | 4 (u16) | — | — | |
| `+0x2a` | 0 (u8) | 0 | 0 | |
| `+0x2c` | 0 | objet d'accélérateur | objet d'accélérateur | |
| `+0x30`/`+0x34` | 0 | mémoires rendues par IOAccelerator | idem | mémoire vidéo / texture |

Extrait (`GLDriver` 0x123d4-0x12404) :

```
000123d4  li   r0, 0x200
000123e0  stw  r0, 4(r3)          ; identifiant
000123e4  li   r2, 0x65d
000123e8  li   r0, 0xd
000123f0  stw  r0, 0xc(r3)
000123f4  stw  r2, 8(r3)          ; drapeaux (pas de 0x100)
```

**Aucun champ de fonctionnalité** : pas de bits d'extension, pas de nombre d'unités de texture, pas
de taille maximale de texture, pas de drapeau T&L. Taille utile ≥ `0x38`. `RendererInfo` sert à
`CGLQueryRendererInfo` et au choix du renderer ; la négociation des capacités se fait ailleurs. **[É]**

---

## 7. `gldGetInteger` : aucun rapport avec les capacités

Un seul appelant : `_gliGetInteger` (GLEngine 0x2978, transmission du sélecteur non traité au
pilote) :

```
00002974  mr   r5, r29
00002978  lwz  r12, 0x477c(r30)   ; ctx+0x4754 + 10*4 = gldGetInteger
0000297c  lwz  r3, 0x4688(r30)
00002984  bctrl
```

Sélecteurs reconnus (le 2ᵉ argument est le numéro de paramètre CGL) :

| sélecteur | `GLDriver` (0x4c40-0x4d10) | Rage128 (0x42f4) | GeForce3 (0x420c) |
|---|---|---|---|
| 200 `kCGLCPSwapRectangle` | 4 mots `drvctx+0x50..0x5c` | `+0x40..0x4c` | `+0xc0..0xcc` |
| 201 | octet `drvctx+0x70` | `+0x5c` | `+0xe0` |
| 203 | octet `drvctx+0x6fb` | oui | `+0xde` |
| 221 | octet `drvctx+0x702` | — | — |
| 222 `kCGLCPSwapInterval` | octet `drvctx+0x6fa` | oui | `+0x21` |
| 294 | 0 | oui | oui |
| 306 | — | — | oui |
| 666 | 0 | — | — |
| 995 | — | oui | oui |
| autre | `0x271a` (paramètre inconnu) | idem | idem |

Rien là-dedans ne décrit une capacité : ce sont des paramètres de dessin et de présentation. **[É]**

---

## 8. Récapitulatif : les trois canaux

| canal | ce qu'il fixe | quand |
|---|---|---|
| `gldGetRendererInfo` | identifiant, « accéléré », formats de pixel possibles, mémoire | choix du renderer (CGL) |
| **5ᵉ argument de `gldCreateContext`** | **extensions (79 bits), toutes les limites, `+0x78..+0x7b` dont la T&L** | création du contexte |
| `gldGetString` | `GL_VENDOR`, `GL_RENDERER`, **`GL_VERSION`** | à chaque `glGetString` |

---

## 9. Vérifications à faire dans l'invité

Toutes se font avec une seule reconstruction du plugin (`guest/gldriver`) ; aucune n'exige le
protocole hôte.

### V1 — Le bloc de configuration est bien le 5ᵉ argument

Dans `gldCreateContext` (`guest/gldriver/pomppc_gld.c:340`), **après** `FWD8(GLD_CreateContext)`,
ajouter `pomppc_dump("cfg", (void *)e, 0x140)`. Attendu dans `POMPPC_GLTRACE` :
`+0x08 = 00000003`, `+0x0c = +0x10 = 00000800`, `+0xb4 = +0xb6 = +0xba = 0008`, `+0xbc = 1000`,
`+0x124 = FC0002B1`, `+0x128 = 00004001`, `+0x12c = 00001000`, et `+0x78..+0x7b = 00 00 00 00`.
Si c'est bien ce qu'on lit, tout le §4 est confirmé d'un coup.

### V2 — Les bits d'extension commandent bien `GL_EXTENSIONS`

Toujours après la transmission, poser un bit isolé et sans effet ailleurs, par exemple le bit 34
(`GL_EXT_paletted_texture`) : `GLD_U32(e, 0x128) |= 1u << 2;`. Puis, dans `guest/gltest`,
`printf("%s", glGetString(GL_EXTENSIONS))`. Attendu : la chaîne contient `GL_EXT_paletted_texture`
et rien d'autre n'a bougé. Reprendre avec le bit 20 en posant aussi le paramètre `ctx+0x7584` pour
vérifier la condition particulière de `GL_ARB_vertex_buffer_object`.

### V3 — Le tableau de correspondance bit → extension

Poser `+0x124 = +0x128 = +0x12c = 0xFFFFFFFF` (uniquement dans une version de sonde, jamais
livrée), lire `GL_EXTENSIONS` et comparer mot à mot au tableau du §3.2 (79 noms attendus après les
25 fixes). C'est la vérification complète et bon marché du décodage.

### V4 — `GL_VERSION` vient du pilote

Ajouter `case 0x1f02: return "1.5 POMPPC-1.0";` dans `pomppc_override_string`
(`guest/gldriver/pomppc_accel.c:2296`) et lire `glGetString(GL_VERSION)` depuis `gltest`. Attendu :
la chaîne sort inchangée, et `atof` d'une application voit 1.5. **À ne pas livrer tant que 1.5
n'est pas tenu** (règle « rien n'est annoncé qui ne soit tenu »).

### V5 — Les limites

Poser `+0xb4 = +0xb6 = +0xba = 4` et `+0xbc = 2048`, puis lire dans `gltest`
`glGetIntegerv(GL_MAX_TEXTURE_UNITS)`, `(GL_MAX_TEXTURE_IMAGE_UNITS)`, `(GL_MAX_TEXTURE_SIZE)`, et
vérifier que `glActiveTexture(GL_TEXTURE4)` rend `GL_INVALID_ENUM`. C'est aussi la façon d'aligner
enfin l'annonce sur ce que le plugin tient réellement (4 unités aujourd'hui).

### V6 — Le verrou T&L (l'expérience décisive)

1. Sans rien changer : `CGLGetParameter(ctx, 310 /* kCGLCPGPUVertexProcessing */, &v)` doit rendre
   **0**, et `311` doit rendre **0** (mais pour la raison du §5.4 il faudrait un identifiant Apple ;
   avec `0x7700` c'est `ctx+0x7581` qui sort, donc 0 aussi).
2. Poser `GLD_U8(e, 0x79) = 1; GLD_U8(e, 0x7a) = 1;` après la transmission, relancer :
   - `CGLGetParameter(ctx, 310)` doit rendre **1** ;
   - la trace doit montrer des appels aux procédures `+0x50` / `+0x54` (mode immédiat) et `+0x4c`
     (tableaux de sommets) — c'est-à-dire les emplacements `PROC_BeginPrimitiveBuffer`,
     `PROC_EndPrimitiveBuffer`, `PROC_RenderVertexBuffer` de `pomppc_gld.h` — **et plus aucun appel
     à `RenderTriangles` & co.**
   - Attention : nos bouchons doivent être branchés avant l'essai, sinon `BeginPrimitiveBuffer`
     rendra le pointeur nul d'Apple et GLEngine écrira à l'adresse 0. **Faire l'essai avec un
     `BeginPrimitiveBuffer` qui rend un tampon de 64 Kio à nous, et un `EndPrimitiveBuffer` qui se
     contente de vider le tampon en hexadécimal** : c'est du même coup la première sonde de la
     tâche 1.2 (disposition des sommets non transformés).
3. Variante `+0x79 = 1`, `+0x7a = 0` : vérifier que le repli `_gleForceToSoftwareTCL` apparaît dès
   qu'on change un état entre `glBegin` et `glEnd` (la trace doit alors montrer à nouveau
   `RenderTriangles`).

### V7 — Ce qui ne sert à rien

Vérifier, en modifiant `RendererInfo` dans `pomppc_patch_renderer_info`, que **ni** le drapeau
`0x100`, **ni** `+0x0c`, **ni** `+0x10/+0x18` ne changent quoi que ce soit à `GL_EXTENSIONS` ni à
`CGLGetParameter(310)`. Confirme le §6 par la négative.

---

## 10. Recommandation pour le plugin

### (a) Annoncer une version et des extensions supérieures

1. **Version** : `pomppc_override_string`, cas `0x1f02` → `"1.5 POMPPC-1.0"`. Rien d'autre à faire ;
   GLEngine transmet. Le cas `0x1f03` est inutile (jamais demandé).
2. **Extensions** : dans `gldCreateContext`, après la transmission à `GLDriver`, faire un `|=` sur
   les trois mots du 5ᵉ argument. Le bloc est déjà rempli par Apple ; **ne jamais l'écraser**, seulement
   ajouter, et seulement les bits que le plugin tient (le reste retomberait sur le rastériseur
   logiciel, plus lent). Pour l'état actuel (sous-ensemble d'OpenGL 1.3, 4 unités, `GL_COMBINE`),
   les bits légitimes sont déjà posés par Apple sauf `GL_ARB_texture_env_crossbar` (2),
   `GL_ARB_texture_mirrored_repeat` (11) et `GL_EXT_texture_env_add` (25) — à n'ajouter qu'après
   preuve.
3. **Limites** : corriger `+0xb4`, `+0xb6`, `+0xba` à 4 (au lieu des 8 d'Apple) tant que le plugin
   n'a que 4 unités ; c'est une correction **vers le bas** qui supprime des replis coûteux. Ajuster
   `+0xbc` à ce que l'hôte accepte réellement.
4. Pour 1.5 il faudra en plus : `GL_ARB_vertex_buffer_object` (bit 20, axe 2.1),
   `GL_ARB_occlusion_query` (bit 17, axe 3.7), `GL_ARB_multisample` (8),
   `GL_ARB_texture_compression` (10), `GL_ARB_shadow`/`GL_ARB_depth_texture` (12/13),
   `GL_ARB_point_sprite` (18) — c'est la liste de contrôle de l'axe 3.

### (b) Faire arriver la géométrie non transformée

Une seule ligne décide : **`config[0x79] = 1`** (et `config[0x7a] = 1` pour ne pas retomber en
logiciel au premier changement d'état). Mais elle engage tout le reste :

1. `gldInitDispatch` : installer nos procédures en **`+0x50` `BeginPrimitiveBuffer`**,
   **`+0x54` `EndPrimitiveBuffer`**, **`+0x4c` `RenderVertexBuffer`**, **`+0x70`
   `RenderVertexArray`** (les bouchons d'Apple rendent 0 / ne font rien : les laisser en place avec
   `+0x79 = 1` fait écrire GLEngine à l'adresse 0).
2. `BeginPrimitiveBuffer(drvctx, mode, &ptr)` doit rendre un tampon **à nous**, assez grand ;
   GLEngine y écrit lui-même les sommets avec un pas lu dans le descripteur de tableau de sommets
   (`ctx+0x48d0`, octet +2, en mots de 4 octets). `EndPrimitiveBuffer(drvctx, drapeau, n, …)` clôt
   la primitive.
3. Tant que la disposition exacte des sommets n'est pas relevée (tâche 1.2), garder un repli :
   `EndPrimitiveBuffer` peut toujours retransformer côté plugin ou refuser — mais **il n'existe pas
   de moyen de refuser primitive par primitive** : `+0x79` est fixé une fois pour toutes à la
   création du contexte. Le repli doit donc être décidé **avant** `gldCreateContext` (par exemple
   sur variable d'environnement `POMPPC_GL_TCL=1`), pas en cours de route.
4. Ne pas se fier aux appels à `gldCreateVertexArray` / `gldCreatePipelineProgram` déjà observés :
   GLEngine les émet aussi pour le rendu logiciel (§5.5).

### (c) Ce qu'il ne faut pas toucher

`RendererInfo` (§6) et `gldGetInteger` (§7) ne portent aucune capacité. La réécriture
`0x02xx → 0x77xx` reste nécessaire pour le rattachement des pixel formats, et le bit `0x100`
(« accéléré ») pour `kCGLPFAAccelerated` — mais ni l'un ni l'autre n'influence les extensions ni la
T&L.

---

## 11. Restes à établir

- Rôle exact de `config+0x78` et `config+0x7b`, et de la table `config+0x7c..+0x87`. **[H]**
- Rôle du 7ᵉ argument de `gldCreateContext` (`config+0x130`). **[H]**
- Signification du masque `config+0x00` (logiciel 4, Rage128 2, GeForce3 10) : les bits 0 et 1 sont
  comparés en `_gleUpdatePolyMode` (0x9530-0x9540). **[H]**
- Signature exacte de `BeginPrimitiveBuffer` / `EndPrimitiveBuffer` / `RenderVertexBuffer` /
  `RenderVertexArray` et disposition du sommet non transformé : c'est la tâche 1.2, et V6 en est la
  première sonde.
- `config+0xc0` (taille maximale d'une cible non identifiée ; 1024 sur Rage128, 4096 sur GeForce3,
  0 sur le rendu logiciel). **[H]**
