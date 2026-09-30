# GLSL dans GLEngine 10.4.6 — relevé pour le protocole v21 (27/09/2026)

Ce que le plugin doit lire dans GLEngine pour exécuter les programmes GLSL
(`GL_ARB_shader_objects`, `GL_ARB_vertex_shader`, `GL_ARB_fragment_shader`,
`GL_ARB_shading_language_100`) sur le GPU de l'hôte
(`docs/protocole-v21-glsl.md`). Listing : `bench/devloop/jobs/1789676199209/out/gle-dis.txt`
(GLEngine de Tiger 10.4.6). Tout est **[L]** (lu au listing) sauf mention ;
**[H]** = hypothèse à vérifier dans l'invité.

## 1. Qui compile : GLEngine, pas le pilote

GLEngine embarque le compilateur de 3Dlabs (`libGLProgrammability.dylib`) et
fait tout lui-même :

| Appel | Fonction de GLEngine | Ce qu'elle fait |
|---|---|---|
| `glCreateShaderObjectARB` | `_glCreateShaderObjectARB_Exec 0x86e14` | objet de 0x3c (sommets) / 0x38 (fragments) octets, `ShInitialize` |
| `glShaderSourceARB` | `_glShaderSourceARB_Exec 0x8774c` | concatène les chaînes (`malloc`, `stpcpy`/`strncpy`), `\r\n` → ` \n`, `\r` seul → `\n`, ajoute un `\n` final ; texte en `+0x2c`, longueur `+0x28`, `+0x24 = 1` |
| `glCompileShaderARB` | `_glCompileShaderARB_Exec 0x87a1c` → `_gleShaderParse` | compile ; `+0x25` = statut, `+0x30` = poignée du compilateur |
| `glCreateProgramObjectARB` | `_glCreateProgramObjectARB_Exec 0x86fb8` | objet de 0x50 octets + DEUX blocs d'étage de 0x520 octets ; **pour chaque renderer, `gldCreatePipelineProgram(ctx, &étage+0x4e4[u], étage+0x504)`** (sommets puis fragments) ; `ShConstructLinker` |
| `glLinkProgramARB` | `_glLinkProgramARB_Exec 0x88140` | `ShLink`, puis tables (uniforms, attributs, taille de la table de valeurs), flux compilés des étages (`PPStream`), émulateur logiciel des SOMMETS (`PPEmulatorProgramCreate`), `_evaluateImageUnits`, `_updateShaderState` si courant |
| `glUseProgramObjectARB` | `_glUseProgramObjectARB_Exec 0x8683c` | `gctx+0x5430` = objet, `_updateShaderState` |
| `glUniform*` | `_glUniform4fARB_Exec 0x88dcc`, `_glUniform1iARB_Exec 0x8ad94`, `_glUniformMatrix4fvARB_Exec 0x8d568`… | écrit la table de valeurs, recopie dans les paramètres locaux de chaque étage qui s'en sert, **puis `gldModifyPipelineProgram(étage, masque 2)`** (6 pour un sampler) |
| `glGetUniformLocationARB` | `0x8edbc` | `ShGetUniformLocation(poignée d'édition des liens, nom)` |
| `glGetActiveUniformARB` | `0x8eee4` | `ShGetActiveUniform(poignée, index, max, &long, &taille, &type, nom)` |
| `glBindAttribLocationARB` | `0x8fd14` | `ShAttributeBindingRequest` (emplacement ≤ 15, refus de « gl_ ») |
| `glGetAttribLocationARB` | `0x8fe7c` | `ShGetAttribLocation` |

**Le texte GLSL ne va jamais au pilote**, comme celui des programmes ARB
(`programmes-arb.md` §1) : le pilote ne reçoit que les deux « pipeline
programs » d'étage à la création de l'objet programme (cible **0** à ce
moment-là : le descripteur, `étage+0x504`, reçoit `0x8B31` / `0x8B30` juste
APRÈS l'appel), et des `gldModifyPipelineProgram` à chaque `glUniform`. Un
pilote matériel de 10.4 compile le flux `PPStream` de l'étage ; nous, nous
relisons le TEXTE dans les objets et le faisons recompiler par l'hôte.

Conséquence : **COMPILE_STATUS, LINK_STATUS, le journal, les emplacements
d'uniforms et d'attributs sont ceux de GLEngine** — l'application les tient de
lui. L'hôte n'a pas à répondre ; il doit accepter ce que GLEngine a accepté
(preuve : les 596 permutations de DarkPlaces passent le compilateur de l'hôte,
`tests/qgpu_core_test.c`, `run_v21_dp`). Un refus de l'hôte (non attendu) est
noté par le plugin avec le journal de l'hôte (`GLSL_INFO_LOG`) et renvoie ce
dessin au rendu d'Apple.

## 2. Les objets

### Objet shader (type `0x8B48`, `GL_SHADER_OBJECT_ARB`)

| Offset | Quoi |
|---|---|
| `+0x04` | nom |
| `+0x08` | compte de références |
| `+0x10` | `0x8B48` |
| `+0x20` | étage : `0x8B31` sommets, `0x8B30` fragments |
| `+0x24` | u8 : un texte a été posé |
| `+0x25` | u8 : compilé (lu par `glLinkProgramARB`, `0x883b8`) |
| `+0x28` | longueur du texte |
| `+0x2c` | texte (NUL final, fins de ligne normalisées) |
| `+0x30` | poignée du compilateur (passée à `ShLink`) |

### Objet programme (type `0x8B40`, `GL_PROGRAM_OBJECT_ARB`)

Les champs à partir de `+0x20` forment un bloc « données de shader » (SD) que
les fonctions adressent par `r27 = objet + 0x20`.

| Offset | SD | Quoi | Preuve |
|---|---|---|---|
| `+0x10` | | `0x8B40` | tous les `_Exec` |
| `+0x18` / `+0x1c` | | journal (malloc) / longueur | `0x883cc` |
| `+0x20` | `+0x00` | étage **sommets** (bloc de 0x520 o) | `0x87084` |
| `+0x24` | `+0x04` | étage **fragments** | `0x87098` |
| `+0x28` | `+0x08` | uniforms actifs (`ShGetNumActiveUniforms`) | `0x8849c` ; borne de `glGetActiveUniformARB` |
| `+0x2c` | `+0x0c` | longueur max des noms d'uniform | `0x884a8` |
| `+0x30` | `+0x10` | **emplacements** de la table de valeurs (`ShGetActiveUserUniformsSize`) | `0x884d0` ; borne des `glUniform*` |
| `+0x34` | `+0x14` | attributs actifs | `0x884b4` |
| `+0x38` | `+0x18` | longueur max des noms d'attribut | `0x884c0` |
| `+0x3c` | `+0x1c` | u8 : lié (`ShLink`) — exigé par `glUseProgramObjectARB` et `glGetUniformLocationARB` | `0x88484`, `0x86914` |
| `+0x3e` | `+0x1e` | u8 : unités d'image cohérentes (`_evaluateImageUnits`) | `0x87b50`, `0x87ce4` |
| `+0x40` | `+0x20` | **poignée de l'éditeur de liens** (`ShConstructLinker`), argument de toutes les requêtes `Sh*` | `0x87154`, `0x8eea0` |
| `+0x44` | `+0x24` | tableau des objets shader attachés | `0x883a8` |
| `+0x48` | `+0x28` | leur nombre | `0x88394` |
| `+0x4c` | `+0x2c` | **table des valeurs** : 16 octets par emplacement | `0x88508`, `0x88ea4` |

### La table des valeurs (`objet+0x4c`)

L'**emplacement** d'un uniform est l'index de son premier `vec4` dans la table
(ce que `glGetUniformLocationARB` rend à l'application) ; `glUniform*_Exec`
écrit `table + 16 × emplacement` :

* flottants (`float`, `vec2..4`) : les composantes en IEEE, le reste inchangé ;
* entiers et booléens : mots entiers (`glUniform1iARB` : `stwx r28`), un
  booléen posé par `glUniform*f` devient 0/1 (`0x88ee0..`) ;
* samplers : l'unité, entière (`glUniform1iARB` → `_setImageUnit`) ;
* matrices : une COLONNE par emplacement (mat2 : 2, mat3 : 3, mat4 : 4) —
  `glUniformMatrix4fvARB` recopie tel quel ou transposé (`0x8d65c` / `0x8d704`),
  la table est toujours par colonnes ;
* tableaux : les éléments à la suite (`count` borné par la taille restante).

Le type d'un emplacement se lit par `ShGetUniformTypeInfo(poignée, emplacement,
&n, &type, &étages)` (le dernier argument dit quels étages s'en servent).

### Les blocs d'étage (0x520 octets)

| Offset | Quoi |
|---|---|
| `+0x000` | u8 : l'étage a un flux compilé (posé par la liaison) |
| `+0x02c` | 16 mots : nombre de samplers par unité d'image |
| `+0x06c` | 16 demi-mots : type de sampler par unité |
| `+0x4dc` | émulateur logiciel (sommets seulement) |
| `+0x4e0` | table de correspondance emplacement → paramètre local de l'étage (`ShGetShaderLocalParamRemapTable`), `0xFFFF` = inutilisé |
| `+0x4e4 + 4u` | poignée du pipeline program du renderer u (**la nôtre** pour le plugin) |
| `+0x504` | descripteur passé à `gldCreatePipelineProgram` : u16 cible (`0x8B31`/`0x8B30`), u16 étage (0/1), `+0x18` = paramètres locaux |
| `+0x508` | flux `PPStream` (réalloué à chaque liaison) |
| **`+0x50c`** | **64 bits : 4 bits par unité d'image** (`u` aux bits `4u`) — index de cible `0` cube, `1` 3D, `2` rectangle, `3` 2D, `4` 1D (`_getIndexForGLSLSamplerType 0x8651c`), `0xF` = unité libre. Le bit `1 << index` est exactement celui de `TU_ENABLE`. Mots : `+0x50c` unités 8..15, `+0x510` unités 0..7 |

Le plugin trouve le pipeline program d'un étage par `PProg.obj = étage + 0x3c`
(`obj = descripteur − 0x4c8`, la convention des programmes ARB).

## 3. L'activité (`gctx`)

| Où | Quoi |
|---|---|
| `gctx+0x5430` | objet programme courant (`glUseProgramObjectARB`), 0 sinon |
| `gctx+0x5434` | u32 : étage SOMMETS actif — posé par `_updateShaderState 0x865a8` : étage présent ET unités cohérentes |
| `gctx+0x5438` | u32 : étage FRAGMENTS actif |
| `gctx+0x31c` | bit `0x04000000` posé par tout `glUniform` et `_updateShaderState` : dispatch au dessin suivant. Neutre pour la liste blanche du verdict et pour `compute_state` depuis le 30/09 (`POMPPC_GL_WLUNIF`, `pomppc_accel.c` `WL_UNIF`) : ce qu'il peut changer au verdict — objet courant, étages, unités échantillonnées, entrées de sommet — est dans la clé `vd_key_of` |
| `gctx+0x314` | bit `1 << u` : l'unité u a changé (sampler déplacé, `_setImageUnit`) |
| `gctx+0x18878 + 0x2e0·r` / `+0x1887c` | poignées de pipeline program des étages courants pour le renderer r |

Un étage absent (programme sans shader de fragments, par exemple) fait
appeler `_gleFPDisable` : l'étage est au pipeline fixe. Des unités
incohérentes (deux types de sampler sur une unité, ou une unité ≥ 16 ; un
sampler dans un shader de SOMMETS : 0 unité permise, `0x87c74`) mettent
`+0x3e` à 0 et les DEUX étages à 0 : GLEngine dessine au pipeline fixe.

## 4. Les limites

`_gleGetState` (`0x141b0..`) :

| Requête | Réponse |
|---|---|
| `GL_MAX_TEXTURE_IMAGE_UNITS` (`0x8872`) | `cfg+0xb6` |
| `GL_MAX_TEXTURE_COORDS` (`0x8871`) | `cfg+0xba` |
| `GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS` (`0x8B4D`) | 16 (constante) |
| `GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS` (`0x8B4C`) | 0 |
| `GL_MAX_VERTEX_UNIFORM_COMPONENTS` / `_FRAGMENT_` (`0x8B4A` / `0x8B49`) | 512 |
| `GL_MAX_VARYING_FLOATS` (`0x8B4B`) | 32 |

`glActiveTexture` accepte une unité `< max(cfg+0xb6, cfg+0xba)` (`0x16664`).
La table des textures liées que lit le pilote (`drvctx+0x10` =
`cfg+0x130`, `CTX_TEXUNITS` du plugin) a **16 unités** de 5 cibles
(`_gleInitTextureState 0x57b4` : boucle de 16), comme l'état d'unité de GLEngine
(`gctx+0x3524 + 0x7c·u`, 16 unités parcourues par `glUseProgramObjectARB`).
Le plugin abaissait `cfg+0xb6` à 8 : sous v21 il le remet à **16** — DarkPlaces
lie sa carte de lumière en 9, sa carte de directions en 10, son masque de
brouillard en 8.

## 5. Ce que le plugin en fait (v21)

* **Dispatch** (`prog_state` → `glsl_state`) : objet courant, étages actifs,
  unités échantillonnées (`étage fragments + 0x50c`), entrées de sommet
  (`glsl_need` : attributs intégrés que lisent les textes de sommets, et
  génériques des attributs nommés par `ShGetActiveAttrib` /
  `ShGetAttribLocation`), refait quand l'empreinte de l'objet change
  (`glsl_sig` : étages, table, textes et poignées de compilation des shaders
  attachés, flux réalloués à la liaison).
* **Lot** (`prog_sync` → `glsl_sync`) : définition sur l'hôte si l'empreinte a
  changé (`glsl_define` : `PROG_CREATE`, un `GLSL_SOURCE` par shader attaché,
  `GLSL_ATTRIB`, `GLSL_UNIFORM` depuis `ShGetActiveUniform` /
  `ShGetUniformLocation`, `GLSL_LINK`, dans une soumission sonde synchrone),
  `PROG_BIND`, puis les valeurs changées (`GLSL_UNIFORMS`, par plages, contre
  un miroir, seulement si un `gldModifyPipelineProgram` a été vu sur un des
  étages).
* L'API `Sh*` est résolue dans `libGLProgrammability.dylib` (déjà chargée par
  GLEngine) par `dlsym` ; ses signatures sont celles des `glGetActive*ARB`
  (arguments recopiés tels quels par GLEngine, `0x8efa4`, `0x8ff60`).

## 5 bis. Le préprocesseur de GLEngine 10.4.6 est faux (vu en VM, 27/09)

Nexuiz `+r_glsl 1 +developer 1` : « vertex shader compile log : ERROR: 0:373:
'' : syntax error #else after a #else » (26 fois), puis « GLSL shader …
generic diffuse failed! … OpenGL 2.0 shaders disabled » — **GLEngine** refuse
le texte, avant tout pilote. Sonde dans l'invité (petits textes compilés par
GLEngine, rendu d'Apple comme plugin) :

| texte | GLEngine |
|---|---|
| `#ifdef F / #ifdef S … #else … #endif` ×2 / `#endif` (deux niveaux, F non défini) | accepté |
| `#ifdef F / #ifdef R / #ifdef S … #else … #endif / #endif / #ifdef D / #ifdef S … #else … #endif / #endif / #endif` | **« #else after a #else »** |
| le même, chaque `#else` réécrit en `#endif` + `#if !(…)` | accepté, mais des lignes SAUTÉES sont prises pour actives (texte de DarkPlaces : `gl_FragColor` dans le shader de sommets) |
| « # ifdef » (blancs après `#`) | sans effet (accepté) |

Le préprocesseur de 3Dlabs de Tiger compte donc mal les conditions imbriquées
à plus de deux niveaux sous un groupe sauté. DarkPlaces imbrique tout son texte
ainsi (mode ▸ étage ▸ effet ▸ option) : aucune permutation ne passait.

**Contournement** (`guest/gldriver/pomppc_glslpp.h`) : le plugin interpose
`glShaderSourceARB` dans la table de dispatch du contexte (entrée `+0x94c` de
`GLIFunctionDispatch`, `gliDispatch.h` ; tables `gctx+0x4680` / `+0x4684`,
posée une fois par contexte à `gldGetString` ou au premier dispatch) et fait
lui-même les conditions avant que GLEngine ne range le texte : lignes sautées
et directives de condition remplacées par des lignes vides (numéros inchangés),
`#define` / `#undef` actifs suivis, évaluation à la C (identificateur inconnu =
0 : les extensions absentes de Tiger, `GL_EXT_gpu_shader4`… ; `__VERSION__` =
110) ; au moindre doute (macro à paramètres dans une condition, `#if` non
fermé) le texte part tel quel. Le texte rangé — celui que GLEngine compile ET
celui que le plugin relit pour l'hôte — est le même. `POMPPC_GL_GLSLPP=0`
l'éteint. Épreuves : `tests/glsl_pp_test.c`, et les 596 permutations de
DarkPlaces préprocessées ainsi puis liées par l'hôte (`run_v21_dp`) ; en VM,
23 programmes de Nexuiz compilés par GLEngine et liés par l'hôte.

## 6. Limites connues

* Un texte changé par `glShaderSourceARB` APRÈS la liaison sans nouvelle
  liaison ferait recompiler le nouveau texte (le programme de GLEngine garde
  l'ancien) — l'empreinte suit le texte courant de l'objet shader. Rare ;
  DarkPlaces ne le fait pas.
* Un shader détaché après la liaison n'est plus lisible : l'empreinte change
  et la redéfinition perd ce texte. DarkPlaces détruit ses shaders après les
  avoir attachés (`glDeleteObjectARB`), ce qui les garde attachés.
* Les programmes détruits : l'entrée du plugin est libérée par la destruction
  des pipeline programs d'étage **[H]** (`gldDestroyPipelineProgram` des
  étages à la destruction de l'objet programme n'est pas relevé au listing ;
  à défaut, l'empreinte d'un nouvel objet à la même adresse diffère et le
  programme est redéfini sur le même identifiant hôte).
