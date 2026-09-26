# Verdict unique : mémoire des unités et des textures, lot 4, lot 5 (26/09/2026)

Suite de `docs/re/etude-court-circuit-glengine.md` (lots 0-5) et de
`docs/re/bloc-changements-r4.md` (lot 3, relevé R4). Trois changements dans le
plugin (`guest/gldriver/pomppc_accel.c`), chacun derrière un drapeau, et le bilan
du lot 5. Mesures sur la VM quotidienne (QEMU de référence avec `x-fp-inline`,
SMP=2), `tools/matrice/matrice.py -m fen --sans-vidage --sample 10` : DOOM 3
`demo_mars_city1` T+50..T+280, Prey « Fuite » L+300..L+600, `sample` de 10 s
dans Tiger juste après la fenêtre de mesure, résumé par `tools/re/sampleplug.py`.

## 1. Profil de départ (plugin de main, a0c19a4)

DOOM 3 62,5 ms/image, Prey 72,7. Fil principal de DOOM 3 : 829 échantillons
(`bench/plugin/depart-d3-1`, hors dépôt).

| poste (inclusif) | éch. | part |
|---|---|---|
| `gleDrawArraysOrElements_VBO_Exec` (tout le dessin VBO) | 287 | 34,6 % |
| └ `pomppc_geom_dispatch` (verdict au dispatch) | 100 | 12,1 % |
| ┊ └ `geom_ok` (dont `geom_texture_ok` : `texture_uploadable` 11, `intern_tex` 7, `texturing_on` 7) | 37 | 4,5 % |
| ┊ └ `geom_format` (`unit_textured` × 8 → `texturing_on`) | 15 | 1,8 % |
| ┊ └ `texture_ok` | 14 | 1,7 % |
| ┊ └ propre (liste blanche, clé) | 13 | 1,6 % |
| └ `geom_draw_client` (dessin) | 144 | 17,4 % |
| ┊ └ `send_state` (dont `compute_state` 32) | 49 | 5,9 % |
| ┊ └ propre | 28 | 3,4 % |
| ┊ └ `geom_draw_native` | 21 | 2,5 % |
| ┊ └ `geom_send_all`, `prog_sync`, `sync_to_host` | 12, 8, 8 | |
| GLEngine lui-même sous `glDrawElements` (propre des `gle*`) | 14 | 1,7 % |

Le dessin ne recalcule plus rien (lot 2 : verdict repris à chaque dessin). Il
reste, au dispatch, le verdict des dispatches qui lient des textures (~45 % des
dispatches de DOOM 3 depuis le lot 3), et au dessin `send_state`. Le jeu
lui-même (`idSIMD_AltiVec::*`, `R_*Cull*`…) domine le propre. Prey : plugin
plus petit encore (`gleDraw…` 23 %, `pomppc_geom_dispatch` 8 %, `send_state`
1,9 %) ; le propre est au jeu (`idSIMD_Generic::TransformJoints`, `__sqrt`,
`idQuat::Slerp` : Prey n'a pas les chemins AltiVec de DOOM 3).

## 2. Mémoire des unités et des textures (`POMPPC_GL_TEXMEMO`)

Un verdict relisait la table des unités de GLEngine une dizaine de fois :
`texturing_on` dans `geom_texture_ok` et dans `texture_ok`, puis une fois **par
unité** dans `geom_format` (`unit_textured` : 8 × `texturing_on`, soit jusqu'à 64
`unit_mask`), et `unit_drvtex` / `intern_tex` / `find_tex` dans chaque boucle.

- **Relevé des unités** (`us_fill`, `us_get`) : masque, objet de GLEngine et
  `PTex` des 8 unités, et le résultat de `texturing_on`, relevés une fois par
  verdict. Valable seulement entre `us_open` et `us_close` (le verdict du
  dispatch, le recalcul du dessin), pour le même contexte, le même programme de
  fragments (`unit_mask` le lit, `prog_sync` peut le refuser), la même table
  d'unités, et tant qu'aucun `PTex` n'a été libéré (`tex_del_epoch`,
  `pomppc_texture_deleted`). `flush()` peut relâcher `G.mu` : le relevé est rouvert
  après. Hors verdict (chemins hérités), tout se relit comme avant.
- **`PTex` par unité entre verdicts** (`p->us_dt`, `p->us_t`) : même objet à la
  même unité qu'au relevé précédent, aucun `PTex` libéré depuis → même `PTex`,
  sans passer par la table de hachage (`find_tex`).
- **Mémoire par texture et par époque** (`texture_uploadable`) : une texture
  déjà dite bonne à cette image (`ok_frame`) et qu'aucun crochet n'a touchée
  depuis (`hook_gen`, avancé partout où la complétude du lot 1 est effacée :
  `pomppc_texture_changed` — niveaux et paramètres, `gldModifyTexture` 0x80
  compris —, sous-images, `glCopyTexSubImage`, mipmaps, vidage de l'état hôte)
  rend « bonne » sans relire le format de base ni `tex_params_ok`. L'empreinte
  des texels (`tex_lv0_sig`, une fois par image) et celle des paramètres
  d'`upload_texture` (`tex_prm_sig`) ne changent pas.

**Contrôle** : sous `POMPPC_GL_VERDICTCHECK=1`, chaque reprise du relevé refait
un relevé complet (`find_tex`, sans mémoire) et le compare, et chaque texture
gardée bonne est revérifiée par le chemin complet ; lignes `TEXMEMO` dans la
note (relevés, repris, textures gardées, écarts).

## 3. Relevé R5 : ce que lit `compute_state` a-t-il un bit dans le bloc ?

Méthode du R4 (`gltest r4`, `POMPPC_GL_BLOCKDUMP=0:1000000`), 50 étapes de plus
(préfixe `R5`) pour tout ce que `compute_state` lit et que R4 n'avait pas
essayé. `compute_state` ne lit **aucune matrice** : les matrices partent par
`geom_send_matrices` (`geom_send_all`), qui compare ses 64 octets à chaque
dessin, indépendamment.

| appel | bits posés |
|---|---|
| `glBlendColor` | `+00 00000004` |
| `glBlendFuncSeparate`, `glBlendFunc` | `+00 00000002` |
| `glClearStencil` | `+00 00000020` |
| `glClearDepth` | `+00 00000010` |
| `glStencilMask` | `+00 00100000` |
| `glStencilFunc` (référence seule, masque seul) | `+00 10000000` |
| `glAlphaFunc` (référence seule) | `+00 00000001` |
| `glScissor` (test allumé ou éteint) | `+00 04000000 +08 00000001` |
| `glColorMaterial` | `+0c 00040000` |
| `glEnable/Disable(GL_RESCALE_NORMAL)` | `+08 80000000` |
| `glLightModeli` `LOCAL_VIEWER`, `COLOR_CONTROL` | `+0c 00000001` (`TWO_SIDE` : plus `+00 00800000`) |
| `glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS)` | `+10 00010000` |
| `glTexEnvfv(GL_TEXTURE_ENV_COLOR)` | `+10 00000100 +14 1 +48 1` |
| `glEnable/Disable(GL_POLYGON_OFFSET_LINE / _POINT)`, `glPolygonOffset` (test éteint) | `+00 00800000` |
| `glPointParameterf` `SIZE_MIN`, `SIZE_MAX`, `FADE_THRESHOLD` | `+00 00400000 +14 1 +48 1` |
| `glFogfv(GL_FOG_COLOR)`, `glFogf` `START`, `END` | `+00 00000800 +14 1 +48 1` |
| `glLineStipple` (motif seul) | `+00 00008000` |
| `glDepthFunc`, `glEnable/Disable(GL_DEPTH_TEST)` | `+00 00000200` |
| `glEnable(GL_DITHER)` | aucun dispatch (non lu) |

**Réponse : oui.** Tout ce que `compute_state` lit de l'état de GLEngine fait
poser un bit des mots `+0x00..+0x10` ; le reste de ce qu'il lit est à nous (le
verdict `ti`, la surface, `p->st`) ou dérivé d'un état qui pose un bit (les
programmes allumés et liés). Le lot 4 est donc fait, pas abandonné.

## 4. Lot 4 : `compute_state` sauté (`POMPPC_GL_STSKIP`)

- `pomppc_geom_dispatch` pose `p->st_dirty` dès qu'un dispatch porte un bit hors
  de `st_mask` (bits dont `compute_state` ne lit pas l'état : effacement couleur
  et profondeur, indications, `glPixelStorei` ; liaisons et paramètres de texture
  `+04` ; fenêtre, matrices, cibles et plans de coupe `+08` sauf normalisation et
  `RESCALE_NORMAL` ; tableaux, env, liaison et allumage des programmes, lumières
  `+0c` ; environnement et combinaison `+10` sauf le biais de LOD ; valeurs
  `+14..+48`). Un bit inconnu fait recalculer. `gldInitDispatch` (sans bloc) aussi.
- `send_state` saute `compute_state` si rien n'est sale et que le reste de ce
  qu'il lit n'a pas bougé : même chemin (brut), même surface (taille, stencil),
  mêmes bits de stencil du drawable, `p->st` valide. Il ne refait alors que les
  clés des unités (depuis le verdict : liaison, environnement, combinaison) et
  des programmes, et n'envoie que celles qui diffèrent, dans l'ordre des plages
  (même flux qu'avec le calcul complet).
- Deux endroits écrivaient `p->st` hors de `send_state` (taille de point par
  point du chemin hérité, `pix_punch_zero` des bitmaps) : ils posent `st_dirty`.
- **Contrôle** `POMPPC_GL_STATECHECK=1` : à chaque saut, calcul complet comparé
  clé par clé sur les plages envoyées ; lignes `STATE` (sautés, calculés,
  écarts, première clé fautive), et c'est le calcul complet qui part.

## 5. Preuves

**`gltest`** (`tools/guest/jobs/gt5.sh`, 27 scènes : les 18 des lots 1-3, `state
clip fogz blendc stencil lit texgen` du lot 4, `r4` avec R5) : sortie et empreinte
de chaque image **identiques** drapeaux éteints, drapeaux allumés, et drapeaux
allumés sous `VERDICTCHECK=1 STATECHECK=1` ; 0 écart `VERDICT`, `TEXMEMO`,
`STATE`. (`clip` 1 échec, `stencil` 3, `tex14` 1 : les mêmes dans les trois
modes, antérieurs à ce lot ; `lit` et `texgen` se jouent en `256 256` comme
`tex13`.)

**Jeux sous contrôle** (`TEXMEMO=1 STSKIP=1 VERDICTCHECK=1 STATECHECK=1`,
`bench/plugin/check-2`) — **zéro écart** :

| | DOOM 3 (images 0-5000) | Prey (images 0-1000) |
|---|---|---|
| relevés d'unités repris (contrôlés) | 18 986 308 par 500 images | 9 203 323 par 500 images |
| `PTex` repris sans la table | 2 466 165 par 500 images | 1 219 676 |
| textures gardées bonnes (contrôlées) | 2 911 704 par 500 images | 1 440 295 |
| `compute_state` sautés / calculés | 284 589 / 362 787 (44 %) | 112 261 / 142 735 (44 %) |

`check-3` : les mêmes contrôles sur Marble Blast, Zenerchi, UT2004, Warcraft III
(§7 plus bas).

**A/B entrelacé**, même binaire, `TEXMEMO=0 STSKIP=0` (A) contre `=1` (B), fenêtre,
deux tours (`bench/plugin/ab1-*`, `ab2-*` ; hôte chargé à 2-3 au second tour,
aucun autre QEMU) :

| | A | B | écart |
|---|---|---|---|
| DOOM 3, tour 1 | 64,6 / 64,6 | 61,4 / 60,9 | −3,5 ms (−5,4 %) |
| DOOM 3, tour 2 | 69,8 / 70,5 | 67,0 / 66,4 | −3,5 ms (−4,9 %) |
| Prey, tours 1 et 2 | 72,7 / 73,1 / 70,9 / 69,4 | 72,1 / 72,1 / 71,9 / 72,0 | +0,5 ms (bruit : ±2) |

Chaque paire DOOM 3 est gagnante (3,2 à 4,1 ms). Prey ne bouge pas : sa scène
avance avec l'horloge (cinématique scriptée) et le plugin y pèse moins (§1).

Profils agrégés (quatre `sample` de 10 s par mode, ~3 400 échantillons chacun) :

| poste (inclusif, part du fil principal) | DOOM 3 A | DOOM 3 B | Prey A | Prey B |
|---|---|---|---|---|
| `gleDrawArraysOrElements_VBO_Exec` | 33,9 % | 31,8 % | 21,4 % | 20,8 % |
| `pomppc_geom_dispatch` | 12,5 % | 10,4 % | 6,6 % | 6,5 % |
| └ `geom_ok` | 4,5 % | 3,3 % | 2,1 % | 2,4 % |
| └ `geom_format` | 1,9 % | 0,9 % | 1,2 % | 0,5 % |
| └ `intern_tex` | 1,4 % | 0 | 0,6 % | 0 |
| `geom_draw_client` | 16,6 % | 16,3 % | 11,9 % | 11,6 % |
| └ `send_state` | 4,1 % | 3,9 % | 2,2 % | 2,0 % |
| └ `compute_state` | 3,0 % | 2,4 % | 1,7 % | 1,4 % |
| GLEngine, propre sous `glDrawElements` | 1,4 % | 1,6 % | 1,3 % | 1,2 % |

Le lot 4 ne rapporte que ce qu'il pouvait : `compute_state` n'est sauté que dans
44 % des dessins (les volumes d'ombre et chaque nouvelle lumière changent pochoir,
faces, découpe) et `send_state` coûtait déjà moins cher que le verdict.

## 6. Lot 5 — bilan

Comparé à `.run/d3/sample-nat.txt` (24/09, avant les lots 1-3, 88 ms/image) :

| poste | `sample-nat` | départ 26/09 (lots 0-3) | après (B) |
|---|---|---|---|
| `gleDrawArraysOrElements_VBO_Exec` | 44,5 % | 34,6 % | 31,8 % |
| `pomppc_geom_dispatch` | 16,5 % | 12,1 % | 10,4 % |
| `geom_draw_client` | 24,2 % | 17,4 % | 16,3 % |
| `geom_ok` + `texture_ok` + `geom_format` (dispatch et dessin) | 25,2 % | 8,1 % | 6,8 % |
| `send_state` | 4,5 % | 5,9 % | 3,9 % |
| GLEngine, propre sous `glDrawElements` | 1,7 % | 1,7 % | 1,6 % |

**Option A classée** (règle de l'étude §3 : GLEngine sous 5 %) : GLEngine reste à
1,1-2,6 % du fil principal sur les neuf profils de DOOM 3 de ce lot, 1,2-1,3 % sur
Prey. Crocheter la table de dispatch ne rapporterait pas plus que ça et ferait
sauter la matrice composée, les masques sales et le bloc de changements (§2 de
l'étude).

Ce qui reste du plugin sous `glDrawElements` de DOOM 3 (~32 %) : le verdict des
dispatches qui lient des textures (~10 %), le propre de `geom_draw_client` et de
`pomppc_geom_dispatch` (clés `vd_key_of`, liste blanche, ~5 %), `geom_draw_native`
(3 %), `send_state` (4 %), `geom_send_all` et `prog_sync`. Aucun poste au-dessus
de 4 % : la suite n'est plus dans le verdict mais dans le déport vers l'hôte (A4 :
bloc d'état lu par le device, `send_state` et `compute_state` hors du profil) et,
pour l'essentiel, dans le jeu lui-même sous TCG (`idSIMD_AltiVec::*`, culling :
plus de la moitié du fil principal).
