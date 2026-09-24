# Protocole qgpu v16 — programmes ARB et attributs génériques (projet, 23/09/2026)

Colin McRae Rally Mac (port Feral : IndirectX + ZonicLib) ne dessine sa course
qu'avec des **programmes ARB de sommets** (vs.1.1 convertis en `!!ARBvp1.0`,
11 programmes de 2,3 à 9,5 Ko), des **programmes ARB de fragments** (ps.1.1
convertis en `!!ARBfp1.0`, 8 programmes) et trois **attributs génériques**
(`glVertexAttribPointerARB` 0 = position 3f, 1 = couleur 4ub normalisée,
2 = texcoord 2f, pas 24). Relevé : `tools/guest/gltrap` (interposition dyld
des appels GL), journal `.run/cmr/gltrap.txt`.

Le protocole n'a aucune de ces notions. GLEngine 10.4.6, ne voyant pas de
pilote capable, **émule le programme de sommets en logiciel** (`PPEmulatorProgram`,
`docs/re/tableaux-de-sommets.md` §4) et ne transmet jamais le texte au pilote :
`gldModifyPipelineProgram` n'arrive qu'avec le masque 2 (paramètres). Sous notre
verrou T&L (bit 0 de `gldUpdateDispatch`), GLEngine déroule les tableaux dans
notre tampon **avec les valeurs courantes des attributs conventionnels** — les
attributs génériques n'y sont pas — d'où les maillages effondrés (0,0,0,1) et
la « géométrie éclatée ». C'est le constat du vidage rejoué
(`.run/cmr/dump-20260923-181852`).

L'hôte sait tout faire directement : le profil hérité d'OpenGL sur macOS 26
(M4, « 2.1 Metal - 90.5 ») expose `GL_ARB_vertex_program`,
`GL_ARB_fragment_program` et `GL_ARB_texture_cube_map`, et compile les deux
familles (essai `glcaps`, 262 144 instructions max). Pas de traduction GLSL.

## 1. Ce que v16 ajoute

Capacité `QGPU_CAP_PROGRAMS` (backend gl seulement ; le backend logiciel et le
rejoueur en `soft` refusent tout dessin sous programme par `BAD_ARG`).

### Objets programme (par contexte)

| Op | Arguments | Sens |
|---|---|---|
| `PROG_CREATE` | `[id, cible]` | cible `0x8620` (sommets) ou `0x8804` (fragments) ; `id` < `QGPU_MAX_PROG` (64) |
| `PROG_STRING` | `[id, len, off]` | texte ASCII dans l'arène (`off` absolu BAR0) ; l'hôte compile ; refus → `BAD_ARG` **sans** couper la session (le plugin note et retombe sur Apple pour ce contexte) |
| `PROG_DESTROY` | `[id]` | |
| `PROG_BIND` | `[cible, id]` | `id` 0 = aucun |
| `PROG_ENV` | `[cible, premier, n, off]` | `n` × 4 flottants (`program.env`), 96 pour les sommets suffisent à CMR, borner à 256 / 128 |
| `PROG_LOCAL` | `[id, premier, n, off]` | `program.local` de l'objet |

Clés d'état (`SET_STATE`) : `QGPU_SK_VERTEX_PROGRAM`, `QGPU_SK_FRAGMENT_PROGRAM`
(0/1 = `glEnable(GL_*_PROGRAM_ARB)`). Sous programme de sommets, l'hôte n'applique
ni éclairage, ni texgen, ni matrices (le programme lit `state.matrix.*` et
`program.env` lui‑même) ; sous programme de fragments, les combineurs sont ignorés.

### Format de sommet

`QGPU_VF_GEN(k)` pour `k` = 0..7 (bits 10..17), 4 flottants chacun, après les
coordonnées de texture dans l'ordre fixe du protocole. `QGPU_VF_ALL` passe à
`0x3FFFF`, `QGPU_VF_MAX_WORDS` à 63. Le cœur valide comme aujourd'hui ;
`qgpu-gl.c` pose `glVertexAttribPointerARB(k, 4, GL_FLOAT, …)` et
`glEnableVertexAttribArrayARB(k)`. L'attribut 0 et la position conventionnelle
sont exclusifs (aliasing ARB) : si `QGPU_VF_GEN(0)` est présent, il **est** la
position.

### Version

`QGPU_PROTO_VERSION` 16 ; un flux v15 ne connaît pas ces opcodes (`BAD_OPCODE`,
inchangé). Le plugin n'émet rien de v16 sous `version < 16` ou sans la capacité.

## 2. Côté plugin : ce qu'il faut apprendre de GLEngine

1. **Négociation.** GLEngine ne délègue les programmes qu'à un pilote qui
   remplit les **limites de programmes** du bloc de configuration
   (`+0xec..+0x11c`, `docs/re/capacites-glengine.md` : nulles chez le rendu
   d'Apple, remplies chez GeForce3/Radeon). À relever champ par champ dans le
   listing (GeForce3GLDriver `gldCreateContext`), puis vérifier dans l'invité que
   `gldModifyPipelineProgram` arrive avec le **masque 1** (texte) et que
   `gctx+0x4e1c` reste un étage que nous acceptons. Sonde en place :
   `pp_create`/`pp_modify` (note `PIPELINE …`).
2. **Texte et paramètres.** Objet : texte `ppobj+0x14`, longueur `+0x18`,
   cible `+0x4c8`, paramètres locaux `+0x4e0` (16 octets × 256/128). Les
   `program.env` restent à localiser (`_glProgramEnvParameter4fvARB_Exec`) —
   à relever au listing, puis sonde.
3. **Attributs génériques sous T&L.** Publier les codes 16..23 dans le
   descripteur `cfg+0x11c` (`docs/re/descripteur-de-sommet.md` §4) quand les
   emplacements 16.. du VAO sont actifs, et vérifier dans l'invité que GLEngine
   y écrit bien les tableaux génériques (le code 16 est déjà l'alias de la
   position ; 17.. à prouver par `gltest` : scène `attrib`).
4. **Prédicat `geom_ok`.** Aujourd'hui `gctx+0x4e1c != 0x1c00` ⇒ refus
   (`raw:program`) et, depuis le 23/09, un emplacement générique actif ⇒ refus
   définitif du contexte (`raw:generic-attribs`, garde-fou à retirer quand v16
   marche). En v16 : programme lié + capacité ⇒ domaine, avec le format qui
   porte les génériques réellement actifs.
5. **Repli.** Un programme que l'hôte refuse (`PROG_STRING` → `BAD_ARG`) ne
   coupe rien : le contexte sort du domaine pour ce programme (Apple émule),
   les autres continuent.

## 3. Épreuves

- `guest/gltest` : scène `arbvp` (position par `vertex.attrib[0]`, couleur
  `attrib[1]` ub normalisé, `attrib[2]` texcoord, `program.env[2..5]` = MVP,
  `program.local[0]` = facteur) et scène `arbfp` (2 textures, `LRP`, `CMP`,
  `ADD_SAT`), comparées au rendu Apple (`POMPPC_GL_DISABLE=1`).
- Rejeu natif : `tests/qgpu_replay.c` en backend `gl` rejoue les nouveaux
  opcodes (le prologue réémet `PROG_*` comme les textures).
- Colin McRae : `.run/cmr/cycle.sh` (compilation, installation, lancement
  jusqu'en course par ssh + AppleScript, vidage, journal) ; critère : course
  jouable, plus aucun `raw:generic-attribs`, `PIPELINE modify masque 1` présent.

## 4. Ordre de travail

A. Device + cœur + backend gl (opcodes, format, état, replay) — vérifiable seul
   par `tests/qgpu_core_test.c` et une scène de rejeu synthétique.
B. Relevé GLEngine (limites `+0xec..`, `program.env`, codes 16.. sous T&L) —
   listing puis sondes dans l'invité.
C. Plugin : négociation, crochets `PipelineProgram` → `PROG_*`, descripteur
   générique, `geom_ok`, clés d'état.
D. `gltest arbvp`/`arbfp`, puis Colin McRae.

Le jeu s'arrête aujourd'hui aussi sur une assertion de ZonicLib
(`COpenGLFragmentProgram.cpp:210`, `glGetError != 0`) dès que le contexte quitte
le domaine T&L pour de bon ; le journal `gltrap` de cette exécution ne montre
pourtant aucune erreur retournée par `glGetError` interposé — l'assertion lit
sans doute l'erreur par un autre point d'entrée (`glGetError` de la table
CGL). À reprendre après C : si GLEngine délègue les programmes, la question
change de nature.

## 5. Réalisé (23/09/2026, soir) — ce qui diffère du projet

**A. Device, cœur, backend GL** — `patches/qgpu/` (copiés dans `~/src/qemu`,
QEMU reconstruit), `tests/qgpu_core_test.c` (jeu v16, 28 épreuves : soft = refus
propres, gl = pixels), `tests/qgpu_replay.c` (prologue `PROG_CREATE` déduit du
texte, `LIST` des opcodes v16). `tests/run-all.sh` : 83 OK.

- Sans `QGPU_CAP_PROGRAMS`, les opcodes `PROG_*` et l'activation (clé à 1)
  répondent **`QGPU_ST_BACKEND`** (la convention des requêtes v8 et de la v10),
  pas `BAD_ARG` ; seul un format à `QGPU_VF_GEN(k)` vaut `BAD_ARG` (dessin
  jeté, non fatal).
- `PROG_STRING` refusé par le compilateur de l'hôte : `BAD_ARG` **non fatal**
  (`draw_op` l'admet), programme marqué cassé ; un dessin qui le trouve lié et
  actif vaut `BAD_ARG` non fatal (jeté). `PROG_BIND` délie par
  `QGPU_PROG_NONE` (0xFFFFFFFF), pas par 0 : 0 est un identifiant valide.
- Objets **par contexte** : rien à découper entre clients dans le kext.
- Paramètres : `PROG_ENV` / `PROG_LOCAL` tout ou rien (validés avant d'écrire),
  `QGPU_MAX_PROG_PARAMS` 256 pour les deux cibles.
- **L'axe y.** Le backend réécrit le texte des programmes de sommets :
  `result.position` → temporaire `qgpu_pos_`, `OUTPUT x = result.position` →
  `ALIAS`, puis `MUL result.position, qgpu_pos_, {1,-1,1,1}` avant `END`. La
  projection n'est alors plus retournée (elle l'est sous
  `OPTION ARB_position_invariant`, qui garde le pipeline fixe). Un programme
  de fragments n'est pas réécrit (`fragment.position` n'est pas retourné).
- `QGPU_VF_GEN(0)` est donné à l'hôte par `glVertexPointer` (4 composantes) :
  sur Apple, `glVertexAttribPointerARB(0)` ne remplace pas le tableau de
  sommets posé avant lui (épreuve (h)). Le plugin ne l'émet jamais : GLEngine
  écrit lui-même le générique 0 dans le champ de position (alias 16).
- Sous programme de fragments, `texture[u]` est la texture **liée** à l'unité,
  allumée ou non (`unit_texture_bound`) ; sous programme de sommets, le test
  `w ≈ 0` du cœur ne s'applique pas.

**B. Relevé GLEngine** — `docs/re/programmes-arb.md`. L'essentiel : le texte ne
va jamais au pilote (masque 1 = « l'ancien texte est mort »), il se lit dans
l'objet ; `program.env` en `*(gctx+0x4668)` / `*(gctx+0x4670)` ; l'activation
ARB en `gctx+0x4664` / `+0x466c` (u8) — **pas** `gctx+0x5434/0x5438`, qui
sont les drapeaux GLSL (`_updateShaderState`) ; les limites `cfg+0xec..` ne
servent qu'à `glGetProgramivARB` (un bloc de 16 octets par cible).

**C. Plugin** — `guest/gldriver/pomppc_accel.c` : `PProg` (poignées
`0x505000xx`, celle d'Apple gardée pour le repli), crochets Create / Modify /
Destroy / GetInfo, `prog_state` (dispatch), `prog_parse` (indices `env`/`local`
lus dans le texte — ZonicLib écrit `program.env  [0..95]`, avec des blancs —,
unités `texture[u], 2D` d'un programme de fragments), `prog_ensure`
(`PROG_CREATE` + `PROG_STRING` dans une **soumission sonde synchrone** : le
verdict du compilateur de l'hôte est connu tout de suite, un refus renvoie ce
programme à Apple sans rien casser), `prog_sync` au lot (`PROG_BIND`, `env`
par plages changées contre un miroir, `local` au masque 2), clés
`QGPU_SK_VERTEX_PROGRAM` / `_FRAGMENT_PROGRAM` (`compute_geom_state`, 4ᵉ plage
de `send_state`), `QGPU_VF_GEN(1..7)` dans le format et codes 17.. dans le
descripteur (`geom_publish`, 16 entrées), `unit_mask` (sous programme de
fragments l'unité est celle que le texte lit : Direct3D n'allume jamais
`GL_TEXTURE_2D`), limites `cfg+0xec..` et bit 15 (`GL_ARB_fragment_program`)
dans `caps_extensions`. **Sous programme, la position est demandée par le code
16** (générique 0) et un attribut conventionnel n'est porté que si son
tableau est actif : GLEngine déroule sinon depuis ses pointeurs résolus
périmés (`gctx+0x48f8`, `docs/re/programmes-arb.md` §3 bis) — c'était la
géométrie éclatée de la course, que `gltest` ne reproduisait pas (ses
pointeurs conventionnels n'avaient jamais été posés). Le chemin **tableaux** (`RenderVertexArray`) refuse
encore les génériques (il packe lui-même) ; Colin McRae ne le prend pas.
`POMPPC_GL_PROG=0` revient à l'émulation par GLEngine.

**Seize génériques (nuit du 23/09).** DOOM 3 Demo (chemin ARB2, choisi
parce que `GL_ARB_fragment_program` est maintenant annoncé) met ses tangentes
dans les attributs génériques **8 à 11** : `QGPU_VF_GEN(k)` va donc de 0 à 15
(bits 10..25, `QGPU_VF_ALL` 0x3FFFFFF, `QGPU_VF_MAX_WORDS` 95). Avec 8, le
plugin refusait ces dessins (`raw:generic-attribs f00/8`), GLEngine les
émulait en logiciel et mourait sur son propre `exit(1)` de
`gleBuildInterpolateFunc` (`docs/re/glengine-exit-interpolateur.md`) — DOOM 3
« plantait ». Sous programme, tout repli vers Apple expose à ce bogue.

**Huit unités de texture — protocole v17 (23/09, nuit).** Avec seize
génériques, DOOM 3 charge sa carte puis meurt de la même façon (segfault dans
les destructeurs statiques de `gameppc.dylib` pendant l'`exit(1)` de
`gleBuildInterpolateFunc`, appelé sous `gleDrawArraysOrElements_VBO_Exec` ←
`RB_ARB2_CreateDrawInteractions`). Cause : `glprogs/interaction.vfp` lit
**`texture[0..6]`** (cube de normalisation, normale, projection et chute de
lumière, diffuse, spéculaire, table spéculaire), le protocole n'en tenait que
quatre (`QGPU_MAX_UNITS`), `text_fp_units` marquait le programme « unités hors
bornes » (`fallback prog:units`) et GLEngine émulait → `exit(1)`. Le protocole
apprend donc les unités 4..7 sans casser un flux v16 : clés
`QGPU_SK_TEXTURE4..` (101..116), `QGPU_SK_COMBINE4` (117..120),
`QGPU_SK_COMBINE_SRC4` (121..124), `QGPU_SK_TEX_LOD_BIAS4` (125..128),
`QGPU_SK_COUNT` 129, adressées par `QGPU_SK_UNIT(u)`, `QGPU_SK_COMBINE(u)`,
`QGPU_SK_COMBINE_SRC(u)`, `QGPU_SK_TEX_LOD_BIAS(u)` ; bits de format
`QGPU_VF_TEX(4..7)` = 26..29 (`QGPU_VF_TEX4`, APRÈS les génériques, ordre sur
le fil : position, normale, couleur, secondaire, brouillard, unités 0..7,
génériques 0..15 ; `QGPU_VF_ALL` 0x3FFFFFFF, `QGPU_VF_MAX_WORDS` 111) ;
`QGPU_MTX_COUNT` 10 et `QGPU_CUR_COUNT` 12 suivent. Les sources croisées de
GL_COMBINE restent aux unités 0..3 (champ source de 3 bits). Le plugin lit le
nombre d'unités du device (`G.units` : 8 en v17, 4 avant) et n'annonce plus
que 4 à GLEngine que sur un device ancien. Épreuves : `run_v17` dans
`tests/qgpu_core_test.c` (constantes, offsets, unité 5 seule, unités 0+6 en
GL_COMBINE, refus de la clé 129 et du bit 30), soft et gl.

**D. Épreuves** — `gltest arbvp` : identique au rendu d'Apple (référence) et
juste sur l'hôte v16 (format `0x80a`, générique 1, env, local, clé). `gltest
arbfp` : juste sur l'hôte ; le rendu d'Apple n'annonce pas
`GL_ARB_fragment_program` et **ignore** `glEnable(GL_FRAGMENT_PROGRAM_ARB)`
sans erreur — sous Apple, les huit programmes de fragments de Colin McRae ne
jouaient donc pas. Colin McRae : voir `docs/todo-gpu-3d.md` (état du jour).
