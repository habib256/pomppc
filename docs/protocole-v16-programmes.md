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
