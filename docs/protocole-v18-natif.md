# Protocole qgpu v18 — `DRAW_NATIVE` : l'hôte lit les VBO tels quels (24/09/2026)

## Pourquoi

DOOM 3 (idTech4) donne ses sommets par des VBO : `glVertexPointer` /
`glVertexAttribPointerARB` sur un tampon lié, au format `idDrawVert`
entrelacé (60 octets) :

| octet | attribut | type |
|---|---|---|
| 0 | xyz | 3 × float |
| 12 | st | 2 × float |
| 20 | normale | 3 × float |
| 32 | tangente 0 | 3 × float |
| 44 | tangente 1 | 3 × float |
| 56 | couleur | 4 × ubyte (normalisé) |

Jusqu'en v17, le plugin de l'invité **REPACKE** chaque sommet en flottants
qgpu (`va_pack_planned` → `DRAW_RAW` / `DRAW_RAW_BUF`) : 79 000 sommets par
image dans une scène, 7 Mo par image, sur un PowerPC émulé. C'est le premier
poste de temps. Or le cœur de l'hôte convertissait déjà chaque mot
(grand-boutiste → hôte, NaN) : le travail du G4 était fait deux fois.

Avec v18, l'invité recopie les octets du VBO **tels quels** dans un tampon
hôte (`BUF_SUBDATA`, une fois tant qu'il ne change pas) et décrit leur
disposition ; c'est l'hôte qui convertit.

## Le contrat

| élément | valeur |
|---|---|
| version | `QGPU_PROTO_VERSION` 18 (un flux plus ancien est lu à l'identique) |
| capacité | `QGPU_CAP_NATIVE` = `0x100`, annoncée par le **cœur** pour tout backend qui a `draw_raw` (soft et gl) |
| opcode | `QGPU_OP_DRAW_NATIVE` = `0x005A`, longueur fixe `QGPU_LEN_DRAW_NATIVE` = 9 |

```
DRAW_NATIVE [mode, n, ibuf, ioff, itype, premier, nattr, aoff]
```

- `mode` : `QGPU_PRIM_MODE_*`, comme `DRAW_RAW`.
- `n` : nombre d'indices (indexé) ou de sommets (non indexé), 1..`QGPU_MAX_VERTS`.
- `ibuf`, `ioff` : tampon hôte des indices et offset en octets ;
  `QGPU_BUF_SHMEM` = indices dans BAR0 à `ioff` (multiple de 4, comme
  `DRAW_RAW`). Ignorés sans indices.
- `itype` : `QGPU_IDX_NONE` / `_U16` / `_U32`. Indices **grand-boutistes**.
- `premier` : non indexé, premier sommet (`glDrawArrays`) ; indexé, doit
  valoir 0 (réservé — un futur `basevertex`).
- `nattr` (1..24), `aoff` : table de `nattr` descripteurs de 6 mots dans BAR0
  (`aoff` multiple de 4).

Descripteur (`QGPU_NATIVE_DESC_WORDS` = 6 mots big-endian) :

```
[code, buf, offset, pas, type, taille | drapeaux]
```

| champ | sens |
|---|---|
| `code` | `QGPU_NA_POSITION` 0, `_NORMAL` 1, `_COLOR` 2, `_SEC_COLOR` 3, `_FOG` 4, `QGPU_NA_TEX(u)` = 8 + u, `QGPU_NA_GEN(k)` = 16 + k ; chacun au plus une fois |
| `buf` | tampon hôte (`BUF_CREATE`) ; **jamais** `QGPU_BUF_SHMEM` (des sommets encore dans BAR0 passent par `DRAW_RAW`) |
| `offset` | octet du sommet d'indice 0 dans le tampon |
| `pas` | octets entre deux sommets ; 0 = serré, comme OpenGL ; aucun alignement exigé |
| `type` | énumération OpenGL : `0x1400` BYTE … `0x1406` FLOAT, `0x140A` DOUBLE (`QGPU_NT_*`) ; données **grand-boutistes** |
| `taille` | 8 bits bas : 1..4 composantes ; bit 8 (`QGPU_NA_NORMALIZED`) : entier normalisé, non signé c/(2^b−1), signé (2c+1)/(2^b−1) (règle d'OpenGL 2.1, celle de l'hôte) ; autres bits réservés |

### Sémantique

1. Le cœur lit la table et en déduit le format `QGPU_VF_*` : position à
   `taille` composantes (2..4), normale 3, couleur 4, secondaire 3,
   brouillard 1, texcoord 4, générique 4 (forme remise au backend,
   indépendante de `QGPU_SK_GEN_SIZES`, qui n'est pas lue). Composantes
   manquantes complétées comme OpenGL : (0, 0, 0, 1) ; une taille supérieure
   à la forme ne lit que les premières composantes.
2. La plage `[lo, hi]` : min et max des indices (indexé), sinon
   `premier .. premier + n − 1`. `hi − lo + 1 ≤ QGPU_MAX_VERTS`.
3. Chaque attribut doit tenir : `offset + pas × hi + taille × octets ≤ taille
   du tampon` (arithmétique 64 bits).
4. Conversion dans le tableau de travail du cœur (`c->vbuf`, agrandi à la
   demande, pas d'allocation par dessin) : le bloc `lo..hi` si sa largeur ne
   dépasse pas le nombre d'indices, sinon **seulement les sommets cités** (le
   reste à zéro). Indices rebasés sur `lo`. Les flottants passent par
   `sane_coord` (NaN → 0, saturation à ±1e9), comme `DRAW_RAW`.
5. Puis **exactement** la fin de `DRAW_RAW` (`raw_finish`, factorisée pour
   l'occasion) : programme cassé → jeté, test `w ≈ 0` sur les sommets cités
   (hors programme de sommets), textures, backend inchangé.

Sans code 0, la position est le générique 0 (aliasing ARB, `QGPU_VF_GEN(0)`) ;
le champ de position (2 mots) vaut alors zéro. Ni l'un ni l'autre = refus. Un
générique sans `QGPU_CAP_PROGRAMS` = refus, comme `QGPU_VF_GEN(k)`.

### Refus

**Tout refus vaut `QGPU_ST_BAD_ARG` non fatal** (dessin jeté, la soumission
continue : `draw_op`). C'est plus large que `DRAW_RAW`, qui rend `OOB` —
fatal — pour les bornes : ici `aoff` hors de BAR0, indices hors de leur
tampon ou plage hors d'un tampon ne coûtent que le dessin, parce qu'un VBO mal
dimensionné par le jeu ne doit pas figer l'image (leçon H4). Liste complète
dans `qgpu_proto.h`, section « v18 : DRAW_NATIVE ».

Un device v17 répond `QGPU_ST_BAD_OPCODE`, qui **arrête** la soumission :
l'invité n'émet `DRAW_NATIVE` que si `version >= 18` **et**
`QGPU_CAP_NATIVE`.

### Performance

Boucle **par attribut** (le type est tranché une fois par attribut, pas par
composante), avec trois boucles sans aiguillage : flottants × 3 (position,
normale, tangentes), flottants × 2 complétés à 4 (st), octets × 4 normalisés
(couleur). Pas d'allocation par appel ; la table (≤ 24 × 24 octets) est lue
dans BAR0 à chaque dessin.

## Ce que le plugin doit faire (`guest/gldriver/pomppc_accel.c`, à faire)

1. **Négociation** : `G.native` = `version >= 18` et `QGPU_CAP_NATIVE`
   (et `POMPPC_GL_NATIVE` ≠ `0` pour pouvoir revenir en arrière).
2. **Miroir des VBO** : pour chaque objet tampon de GLEngine lié à un tableau
   actif, un emplacement hôte (tampon qgpu + offset). Recopier les octets
   **tels quels** par `BUF_SUBDATA` à la création et à chaque modification
   (`glBufferData`, `glBufferSubData`, `glUnmapBuffer` — à relever dans
   GLEngine : où lire le contenu et la « génération » du tampon). Aucune
   conversion sur le G4.
3. **Peu d'identifiants** : un client n'a que `QGPU_CLIENT_BUF_IDS` (64)
   tampons de 16 Mo au plus ; DOOM 3 alloue un VBO par surface statique. Le
   plugin **sous-alloue** donc plusieurs VBO dans un même tampon hôte
   (`offset` du descripteur), avec un allocateur simple (arène + libération
   par génération), plutôt qu'un tampon hôte par VBO.
4. **Dessin** : à `glDrawElements` / `glDrawRangeElements` / `glDrawArrays`,
   si **tous** les tableaux actifs sont des VBO miroités, écrire la table
   (code, tampon, offset + décalage du pointeur, pas, type, taille, bit
   normalisé — que GL impose pour `glColorPointer`, `glSecondaryColorPointer`
   et `glNormalPointer` entiers) dans l'arène et émettre `DRAW_NATIVE`. Les
   indices : dans leur propre miroir s'ils viennent d'un
   `GL_ELEMENT_ARRAY_BUFFER`, sinon recopiés dans l'arène
   (`QGPU_BUF_SHMEM`, `ioff` aligné sur 4). Un tableau client (hors VBO) ou
   un type non décrit fait retomber sur le chemin actuel (repack).
5. **Attributs conventionnels absents** : comme `DRAW_RAW`, ils prennent la
   valeur courante (`SET_CURRENT`), à tenir à jour avant le dessin.

## Épreuves (`tests/qgpu_core_test.c`, `run_native`, soft et gl)

- (1) constantes ; 24 refus (commande et descripteur), chacun `BAD_ARG` **non
  fatal** (le `CLEAR` suivant s'exécute) ; indice qui sort du tampon ; dernier
  octet exact accepté ; plage > `QGPU_MAX_VERTS` ; `premier + n` > 32 bits ;
  longueur 8 ; générique avec / sans `QGPU_CAP_PROGRAMS` ; générique 0 sans
  position.
- (2) quatre `idDrawVert` grand-boutistes dans un tampon hôte, indices U16
  (tampon hôte, puis BAR0) et U32, pipeline fixe (position 3f, couleur 4ub
  normalisée, st 2f + texture 2×2 en MODULATE) : **image identique au bit
  près** à celle d'un `DRAW_RAW` construit à la main, et tableau remis au
  backend identique.
- (3) gl : programme ARB de sommets lisant `vertex.attrib[8..10]` (st 2f,
  normale et tangente 3f, composantes complétées lues par le programme) :
  image identique au `DRAW_RAW` à 4 composantes ; puis l'`idDrawVert` complet
  (6 attributs).
- (4) pas de 72 octets, offset 40, sommets NaN non cités (bloc dense
  assaini) ; indices épars 7/40/90 dans 100 sommets NaN (rebasés 0/33/83,
  seuls les cités convertis).
- (5) non indexé, `premier` = 3, `ibuf` / `ioff` quelconques ignorés.
- (6) types : SHORT brut, USHORT / BYTE / UINT / UBYTE normalisés, DOUBLE,
  INT brut, FLOAT NaN → 0, complétion (0, 0, 0, 1), pas 0 serré.

`tests/qgpu_replay.c` : prologue qui crée les tampons inconnus cités par
`DRAW_NATIVE` (indices et table), `QGPU_REPLAY_LIST` (commande + un
descripteur par ligne), `QGPU_REPLAY_SKIPDRAW` et l'avertissement de texture
sans image comptent `DRAW_NATIVE`.

## Pistes suivantes

- **Passage direct au backend GL** : garder sur l'hôte une copie des tampons
  permutée en petit-boutiste (au `BUF_SUBDATA`, ou paresseusement par
  génération), en faire des VBO de l'hôte, et poser
  `glVertexPointer` / `glVertexAttribPointerARB(type, normalisé, pas,
  offset)` directement, sans conversion ni recopie par dessin. Il faut alors
  sortir du cœur la validation `sane_coord` (NaN) et le test `w ≈ 0` — ou les
  faire à la permutation — et le backend logiciel garderait la conversion
  actuelle.
- `premier` indexé = `basevertex` (`glDrawElementsBaseVertex`), si un jeu en a
  besoin.
- Cache de conversion par (tampon, génération, plage) : les maillages
  statiques de DOOM 3 sont redessinés à chaque passe de lumière.
