# Protocole qgpu v21 — programmes GLSL (27/09/2026)

## Pourquoi

Nexuiz 2.5.2 (DarkPlaces) ne prend son chemin de rendu GLSL (éclairage par
pixel, reliefs, eau, post-traitement) que si `GL_ARB_fragment_shader` est
annoncée. Le rendu logiciel d'Apple ne l'annonce pas : GLEngine sait compiler
un shader de fragments GLSL, mais pas l'exécuter en logiciel. L'hôte (macOS 26
sur M4, contexte hérité « 2.1 Metal », GLSL 1.20, 16 unités d'image) le sait.

GLEngine compile et lie **lui-même** (compilateur 3Dlabs de
`libGLProgrammability`) : il répond à l'application (statuts, journaux,
emplacements). Le texte ne va jamais au pilote ; le plugin le relit dans les
objets de GLEngine et le fait recompiler par l'hôte. Relevé :
`docs/re/glsl-glengine.md`.

## Le contrat (`patches/qgpu/qgpu_proto.h`, section v21)

| élément | valeur |
|---|---|
| version | `QGPU_PROTO_VERSION` 21 |
| capacité | `QGPU_CAP_GLSL` = `0x1000` (backend GL : points d'entrée d'OpenGL 2.0, 16 unités d'image, programme d'essai lié à l'init ; `QGPU_GLSL=0` dans l'environnement de QEMU l'éteint) |
| cible | `QGPU_PT_GLSL` = `0x8B40` (`PROG_CREATE`, `PROG_BIND`) ; même espace d'identifiants que la v16 |
| identifiants | `QGPU_MAX_PROG` 64 → **256** par contexte (DarkPlaces compile une permutation par combinaison d'effets) |
| unités d'image | `QGPU_MAX_IMAGE_UNITS` 16 ; clés `QGPU_SK_UNIT(8..15)` = 130..161 (texturage, texture ; mode et couleur d'environnement acceptés, sans effet) ; `QGPU_SK_COUNT` 162 |

Opcodes (tous par contexte, sur le programme `id` créé par `PROG_CREATE [id, QGPU_PT_GLSL]`) :

| op | arguments | sens |
|---|---|---|
| `GLSL_SOURCE` `0x76` | `[id, étage, len, off]` | un texte (shader object) de l'étage `0x8B31` sommets / `0x8B30` fragments ; ASCII imprimable et blancs, ≤ 256 Kio, 8 textes au plus. Sur un programme déjà lié : **nouvelle définition** (tout est oublié) |
| `GLSL_ATTRIB` `0x77` | `[id, emplacement, len, off]` | nom d'attribut lié à l'emplacement générique 0..15 avant l'édition des liens |
| `GLSL_UNIFORM` `0x78` | `[id, emplacement, type, n, len, off]` | uniform actif : nom sans `[0]`, type GL, taille de tableau, emplacement de base dans la table de valeurs (1 `vec4` par élément ; 2, 3, 4 colonnes pour mat2, mat3, mat4). Pas de nom en `gl_` |
| `GLSL_LINK` `0x79` | `[id]` | compilation et édition des liens par l'hôte ; refus = `BAD_ARG` **non fatal**, programme cassé |
| `GLSL_UNIFORMS` `0x7A` | `[id, premier, n, off]` | n emplacements × 4 mots bruts, lus selon le type déclaré (flottant : ni NaN ni infini ni \|v\| > 1e9 ; sampler : unité < 16) ; tout ou rien |
| `GLSL_INFO_LOG` `0x7B` | `[id, max, off]` | journal de l'hôte du dernier `GLSL_LINK`, ASCII + NUL, tronqué à `max` |

Sémantique (OpenGL 2.0) :

* un programme GLSL lié **prime** sur les programmes ARB (les clés
  `QGPU_SK_VERTEX_PROGRAM` / `_FRAGMENT_PROGRAM` ne sont pas lues) ; un étage
  sans shader reste au pipeline fixe ; lié mais cassé, ou jamais lié : dessin
  jeté (`BAD_ARG` non fatal) ;
* il agit sur `DRAW_RAW`, `DRAW_RAW_BUF`, `DRAW_NATIVE` ; pas sur les opcodes
  hérités ;
* l'état intégré (`gl_ModelViewMatrix`, `gl_TextureMatrix[u]`, `gl_LightSource`,
  `gl_Fog`…) est celui des clés, matrices et lumières du contexte ;
* attributs intégrés : champs conventionnels du format ; attributs nommés :
  `QGPU_VF_GEN(k)` de leur emplacement ;
* `sampler* = u` lit la texture de l'unité u (texturage à 1, texture
  complète) ; une unité échantillonnée sans texture rend du noir ;
* l'axe y (le cœur rend la ligne 0 en haut) : le backend réécrit les textes —
  `main` → `qgpu_main_`, un `main` ajouté retourne `gl_Position.y` après
  l'appel ; `gl_FragCoord` → `qgpu_FragCoord_ = (x, H − y, z, w)` avec
  l'uniform `qgpu_fh_` = hauteur de la surface. La projection de l'hôte n'est
  alors pas retournée (elle l'est pour le pipeline fixe et pour un programme
  sans shader de sommets). Les déclarations ajoutées vont après la dernière
  ligne `#version` / `#extension`.

Un flux v20 ignore les opcodes (`BAD_OPCODE`, fatal) et les clés 130..161
(`BAD_ARG`) ; le plugin n'émet rien de v21 sans version ≥ 21 et la capacité.
`qgpu_abi.h` ne change pas : **le kext n'est pas à reconstruire**.

## Réalisation

### Cœur (`qgpu-core.c`, `qgpu-core.h`)

`QgpuProgram.glsl` (`QgpuGlsl`) : textes, attributs, déclarations, table de
valeurs (1024 emplacements), carte emplacement → déclaration (refus des
recouvrements), cibles échantillonnées par unité (recalculées quand une valeur
de sampler change, `glsl_samples_update`). `bound[QGPU_PROG_GLSL]`. Au dessin
(`raw_finish`) : programme GLSL lié → unités 0..15 par `unit_texture`, étage
sommets actif ou non, dessin jeté si cassé. Backend : `glsl_link` (nouveau),
`prog_destroy` (libère aussi l'objet GLSL).

### Backend GL (`qgpu-gl.c`)

`gl_glsl_probe` (points d'entrée 2.0, `GL_MAX_TEXTURE_IMAGE_UNITS` ≥ 16,
programme d'essai avec `gl_FragCoord` passé par la réécriture),
`glsl_rewrite` (renommage hors commentaires, insertion après les directives),
`gl_glsl_build` (compilation, `glBindAttribLocation`, liaison, journal,
emplacements hôte de chaque déclaration, `qgpu_fh_`), `gl_glsl_uniforms`
(seules les déclarations dont une valeur a changé, `glUniform*v` sur l'élément
0 avec `count`), `gl_glsl_use` (unités 8..15, texture 0 sur les cibles
échantillonnées sans texture de cette cible, `glUseProgram`, `qgpu_fh_`). Sous
GLSL, les matrices de texture et les coordonnées des unités 0..7 sont posées
même sans texture (DarkPlaces y passe ses tangentes) et les paramètres de
brouillard même brouillard éteint (`gl_Fog`). Le backend logiciel n'annonce
pas la capacité : refus propres (`BACKEND`).

### Plugin (`guest/gldriver/pomppc_accel.c`)

`G.glsl` (v21 + capacité, `POMPPC_GL_GLSL=0` l'éteint) ; `GProg` par (contexte,
objet programme) ; `glsl_state` au dispatch (étages, unités, entrées),
`glsl_sync` au lot (définition si l'empreinte change, liaison, valeurs) ;
16 unités d'image dans le verdict (`TexInfo`, relevé des unités, clés
130..161) sous fragments GLSL ; sous shader de sommets GLSL, les coordonnées
de texture suivent le texte (`gl_MultiTexCoord<u>`) et non les textures ;
`GL_ARB_fragment_shader` (bit 16) et `GL_MAX_TEXTURE_IMAGE_UNITS` = 16
(`cfg+0xb6`) annoncés sous `G.glsl`. `GL_VERSION` reste « 1.5 POMPPC-1.0 » :
OpenGL 2.0 demande aussi les textures non puissances de deux, les
`point sprites`, plusieurs tampons de dessin et le stencil séparé, que la
chaîne ne tient pas tous.

## Épreuves

* `tests/qgpu_core_test.c`, `run_v21` : constantes ; backend logiciel : refus
  propres ; backend GL : sommets + fragments (couverture identique au pipeline
  fixe au pixel près), valeur changée, état intégré (`gl_LightSource`,
  `gl_TextureMatrix[1]` × valeur courante d'une unité sans texture), fragments
  seuls, sommets seuls (uniform posé avant la liaison), float/vec2/vec3,
  int/bool/ivec2, mat2/3/4 par colonnes, tableaux, samplers sur les unités 0
  et 9, unité sans texture (noir), sampler hors bornes refusé, attribut nommé
  en 3, `gl_FragCoord` (y depuis le bas), refus de l'hôte non fatal avec
  journal, dessin jeté puis redéfinition, GLSL prime sur ARB, destruction qui
  délie, validations (NaN, `gl_`, recouvrement, hors table, emplacement 16,
  `PROG_STRING` / `PROG_BIND` croisés, programme jamais lié).
* `run_v21_dp` : **596 permutations de DarkPlaces** (celles que son code
  choisit, `tests/dp_glsl_extract.py`, textes tirés de ses sources hors git)
  compilées et liées par l'hôte à travers la réécriture — 0 refus.
* `guest/gltest` : scènes `glsl`, `glslvs` (référence : rendu d'Apple),
  `glslfs`, `glsldp` (mode lightmap de DarkPlaces : unités 1 et 9,
  coordonnées 0 et 4).
* Nexuiz `+r_glsl 1 -benchmark demos/demo1` : cf. `CHANGELOG.md` et ci-dessous.

Validation finale de la branche (27/09) : `tests/run-all.sh`, avec
`QGPU_DP_ZIP` pointant vers `enginesource20091001.zip` du dépôt principal :
**145 OK, 0 échec, 4 ignorés** (shellcheck absent et trois épreuves `--slow`
non lancées). Le harnais inclut le cœur qgpu, le backend, le préprocesseur GLSL
et les permutations de DarkPlaces lorsque le backend GL est disponible.

## En VM (27/09, QEMU v21 de `~/src/qemu-glsl`, plugin `20260927-glsl`)

* Scènes `glsl`, `glslvs`, `glslfs`, `glsldp` : toutes justes sous le plugin ;
  `glslvs` identique au rendu d'Apple (référence) sur ses trois cas ; sous
  Apple, `glslfs` rend le pipeline fixe (pas de `GL_ARB_fragment_shader`).
  Deux défauts trouvés et corrigés en route : `gl_MultiTexCoord<u>` mal
  reconnu dans le texte (coordonnées de carte de lumière perdues), paramètres
  des lumières éteintes et matrices de texture sans texture non envoyés
  (`gl_LightSource`, `gl_TextureMatrix`).
* **Le préprocesseur GLSL de GLEngine 10.4.6 est faux** (#if imbriqués dans
  un groupe sauté) : toutes les permutations de DarkPlaces refusées par
  GLEngine lui-même. Le plugin fait les conditions à sa place
  (`guest/gldriver/pomppc_glslpp.h`, `docs/re/glsl-glengine.md` §5 bis).
* Nexuiz : 23 programmes compilés par GLEngine, **tous liés par l'hôte**, zéro
  repli hors rafraîchissements de fenêtre ; `demo1` en 72 s, **26,5 img/s**
  (≈ 40 ms/image) contre ≈ 107 ms/image par le chemin ARB (`+r_glsl 0`) ;
  rejeu natif = capture de la VM (0,00 %) en fenêtre et plein écran ;
  `VERDICTCHECK` / `STATECHECK` / TEXMEMO : 0 écart.
* Coût restant : 154 298 dispatches recalculés sur 173 572 dans la démo. Le
  bit `0x04000000` de `glUniform` est neutre depuis le 30/09
  (`POMPPC_GL_WLUNIF`) ; il ne rapporte que ~4 400 dispatches par 500 images,
  les autres lient des textures (`+04`) ou changent une matrice ou cible de
  texture (`+08`) — gain dans le bruit (`CHANGELOG.md`).
