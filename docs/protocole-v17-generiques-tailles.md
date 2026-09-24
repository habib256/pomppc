# Protocole qgpu v17 + `QGPU_CAP_GEN_SIZES` — génériques à taille déclarée (24/09/2026)

## Pourquoi

DOOM 3 et Prey (idTech4, programmes ARB) passent par le chemin **tableaux** du
plugin (`geom_draw_client` → `va_pack_planned` → `DRAW_RAW` / `DRAW_RAW_BUF`).
Leurs sommets portent `st` en générique 8 (2 flottants), la normale en 9 et les
tangentes en 10 et 11 (3 flottants chacun). Jusqu'ici un générique occupait
**toujours 4 flottants** sur le fil : 16 mots par sommet pour les génériques
quand 11 suffisent. Un profil `sample` de DOOM 3 attribue environ 20 % du fil
principal à l'empaquetage des sommets, et le transfert BAR0 → hôte croît avec
le nombre de mots.

## Le contrat

**Une clé d'état, pas d'opcode ni de nouvelle version.**

| élément | valeur |
|---|---|
| capacité | `QGPU_CAP_GEN_SIZES` = `0x80` |
| clé | `QGPU_SK_GEN_SIZES` = 129 (`QGPU_SK_COUNT` passe à 130) |
| valeur | 2 bits par générique k (bits 2k..2k+1) : **0 = 4 composantes** (valeur initiale, la forme v16/v17), 1..3 = 1..3 composantes |
| portée | état **du contexte**, lu par chaque `DRAW_RAW` / `DRAW_RAW_BUF` qui suit ; les opcodes de dessin hérités ne le lisent pas |

- Seuls comptent les champs des génériques **présents** dans le format
  (`QGPU_VF_GEN(k)`). Les autres sont ignorés : on peut poser la clé une fois
  pour plusieurs formats.
- Dans le sommet, un générique déclaré à n composantes occupe **n flottants à sa
  place dans l'ordre fixe** (position, normale, couleur, secondaire, brouillard,
  unités 0..7, génériques 0..15). Taille du sommet sur le fil :
  `QGPU_VF_WORDS_GS(format, clé)` ; offsets : `qgpu_vf_offset_gs(format, clé,
  bit)` (cœur). Avec une clé nulle, ce sont `QGPU_VF_WORDS` et `qgpu_vf_offset`.
- Le **pas** (0 = serré) et les bornes (BAR0, tampon hôte v14) se comptent en
  mots déclarés : un pas explicite inférieur à `QGPU_VF_WORDS_GS` vaut
  `QGPU_ST_BAD_ARG` ; lire le même tampon avec une clé qui dit 4 composantes
  peut donc déborder (`QGPU_ST_OOB`), comme tout format trop large.
- **Le cœur complète** les composantes absentes comme OpenGL (y = 0, z = 0,
  w = 1) en remettant les sommets à plat (`conv_raw_vertex_gs`). Les backends
  reçoivent toujours la forme serrée à 4 flottants par générique : ni
  `qgpu-gl.c` (qui donne les génériques par `glVertexAttribPointerARB(k, 4, …)`
  sur le tableau du cœur) ni `qgpu-soft.c` n'ont changé. Coût hôte : nul, le
  cœur convertissait déjà chaque mot (big-endian → hôte, NaN).
- **Toutes les valeurs 32 bits sont valides** (chaque code de 2 bits a un
  sens). Le refus porte sur la capacité : poser une valeur **non nulle** sans
  `QGPU_CAP_GEN_SIZES` vaut `QGPU_ST_BACKEND`, rien n'est écrit (la règle de
  l'activation d'un programme sans `QGPU_CAP_PROGRAMS`) ; poser 0 est toujours
  accepté.
- Le cœur annonce la capacité **dès que `QGPU_CAP_PROGRAMS` l'est** (les
  génériques n'existent que sous elle) : aujourd'hui, backend GL oui, backend
  logiciel non.

### Pourquoi une capacité et pas une v18

Rien d'existant ne change de sens : la clé vaut 0 au départ, et un flux qui ne
la pose jamais est lu **bit pour bit** comme avant (épreuve (h)). Un device qui
ne connaît pas la clé la refuse (clé ≥ son `QGPU_SK_COUNT` → `BAD_ARG`), et il
n'annonce pas le bit : l'invité, qui teste le bit, ne l'émet jamais chez lui.
`QGPU_PROTO_VERSION` reste 17 ; c'est le mécanisme de `QGPU_CAP_SCANOUT`,
`QGPU_CAP_GL14`, `QGPU_CAP_PROGRAMS`. Le kext transmet `QGPU_REG_CAPS` sans le
filtrer (seul `QGPU_CAP_ASYNC` est masqué) : il n'a pas à être reconstruit pour
que le plugin voie le bit (sa copie de `qgpu_proto.h` est tout de même tenue
identique).

### Pourquoi une clé et pas un opcode ou des bits du format

Les 32 bits de la clé couvrent exactement 16 × 2 bits ; `SET_STATE` existe,
est validé, suivi par contexte, rejoué et vidé par les outils (`qgpu_replay`).
Le mot de format n'a plus que les bits 30-31 de libres.

## Côté plugin (`guest/gldriver/pomppc_accel.c`)

- `G.gensizes` = `G.prog` **et** `QGPU_CAP_GEN_SIZES` annoncé **et**
  `POMPPC_GL_GENSIZES` ≠ `0`. Sinon la clé n'est jamais posée : le fil est celui
  d'avant.
- **Chemin tableaux** : `va_gen_sizes` lit, pour chaque générique k ≥ 1 du
  format dont le tableau est actif et lisible, sa taille (`U16(ent, 0xa)`) ;
  1..3 sont déclarées, 4 (et les tableaux remplacés par la valeur courante)
  restent au code 0. `words = QGPU_VF_WORDS_GS(fmt, gs)` ; `va_plan_build`
  donne `dst_n = QGPU_GS_COUNT(gs, k)` à chaque générique — l'empaqueteur copie
  donc exactement ce que le tableau contient, sans compléter. Le tampon hôte
  (v14) retient `pack_gs` : un emballage à une autre taille n'est pas
  réutilisé.
- **Émission** : `emit_draw_client` pose `SET_STATE(QGPU_SK_GEN_SIZES, gs)`
  juste avant le `DRAW_RAW` / `DRAW_RAW_BUF`, après `close_raw()`, si le miroir
  `p->c_gs` ne dit pas déjà ces tailles pour les génériques du format
  (`gs_stale`). **Begin/End** (descripteur de GLEngine, `DESC_ENT` à 4
  composantes) : `geom_begin` remet à 0 les champs de ses génériques s'ils ne
  le sont pas. Le miroir est invalidé avec les autres (`invalidate_mirrors`,
  soumission refusée) et naît invalide avec le contexte.
- Journal de démarrage : « génériques à taille déclarée » dans la ligne
  `POMPPC: qgpu actif (…)`.

Gain attendu sur DOOM 3 (génériques 8 : 2f, 9..11 : 3f) : 5 mots de moins par
sommet, soit 16 → 11 mots de génériques, et autant d'écritures en moins dans
`va_pack_planned` (les compléments y = 0 / w = 1 n'y sont plus écrits).

## Épreuves (`tests/qgpu_core_test.c`, `run_gensizes`)

Soft (sans capacité) : constantes, capacité absente, valeur non nulle refusée
(`BACKEND`, rien d'écrit), 0 accepté. GL (capacité annoncée) : programme de
sommets qui lit `vertex.attrib[8..10]` y compris les composantes complétées
(`z` et `w`) ; (a) référence à 4 composantes ; (b) mêmes sommets déclarés
2/3/3 → **image identique au bit près** et sommets remis au backend complétés
(0, 1) ; (c) codes des génériques absents ignorés ; (d) pas explicite = / < /
> taille déclarée ; (e) indexé épars (conversion qui suit les indices) ;
(f) bornes de BAR0 en mots déclarés ; (g) `DRAW_RAW_BUF` d'un tampon hôte ;
(h) non-régression 4 composantes et clé propre au contexte ; (i) format sans
générique : clé sans effet. Le rastériseur logiciel ne tenant pas les
programmes, la preuve « mêmes pixels » est faite sur le backend GL entre la
forme déclarée et la forme 4 composantes.

## Pistes suivantes

- La position du chemin tableaux part toujours en `QGPU_VF_POS(4)` ; le champ
  de position sait déjà 2 ou 3 composantes (`QGPU_VF_POS(n)`) : un mot de plus
  par sommet pour DOOM 3 (position 3f), à condition de vérifier ce qui dépend
  de `POS(4)` dans le plugin (test `w ≈ 0`, `raw_scan_nan`).
- Coordonnées de texture conventionnelles (toujours 4) : même idée, autre clé.
- Descripteur de GLEngine (`DESC_ENT(16 + k, off, nc)`) : il sait demander nc
  composantes, mais le comportement de GLEngine quand la source en a plus ou
  moins n'est pas relevé.
