# Transmission paresseuse des `gldUpdateDispatch` au GLDriver d'Apple

État au 24/09/2026 : **code écrit, compilé et installé dans l'invité, éteint par défaut**
(`POMPPC_GL_LAZYAPPLE=1` l'allume). La mesure de référence (sans) est faite, **la
mesure avec n'a pas été faite** : le travail a été arrêté pour laisser la place à
un correctif prioritaire. Rien n'a été validé dans un jeu avec la variable allumée.

## 1. Ce qui coûte (mesure de référence, sans transmission paresseuse)

DOOM 3, carte `demo_mars_city1`, fenêtre 640×480, révision `20260924-lazyapple`
avec `POMPPC_GL_LAZYAPPLE=0` (identique au comportement d'avant). Profil `sample`
de 10 s, fil principal (827 échantillons), vers t = 200 s (≈ 500 à 800
dessins/image, 118-125 ms/image) :

| fonction (inclusif, fil principal) | échantillons | part |
|---|---|---|
| `gldUpdateDispatch` (plugin + Apple) | 136 | **16,4 %** |
| `gldLoadCurrentTexture` (Apple) | 71 | **8,6 %** |
| `glgProcessPixels` (dont `glgS3TCDecompress`) | 67 | 8,1 % |
| `glrSetFunctions` (module GLRaster) | 9 | 1,1 % |

Chaîne : `gleDrawArraysOrElements_VBO_Exec → gldUpdateDispatch (plugin) →
gldUpdateDispatch (Apple) → gldLoadCurrentTexture → gldLoadTextureStructure →
gldLoadTextureLevelBuffer → glgProcessPixels → glgProcessColor / glgS3TCDecompress`.
Le reste de `gldUpdateDispatch` est le verdict du plugin (`pomppc_geom_dispatch` :
`texture_ok`, `tex_complete`, `unit_mask`…), qui doit tourner de toute façon.

Temps par image de référence (`frames.csv`, fenêtres de 100 images) :

| t | ms/image | dessins/image |
|---|---|---|
| 72-94 s (cinématique d'intro) | 42-46 | 149-164 |
| 115 s | 168 | 806 |
| 162-199 s | 118-133 | 536-736 |
| 276-281 s | 22-35 | 102-187 |
| 285-297 s | 55-78 | 438-600 |

Replis : ~1 par image en jeu (c'est l'échange, compté par `fallback()`), 100 par
100 images pendant le chargement. Image juste (capture à t ≈ 300 s).

## 2. Ce que fait vraiment le `gldUpdateDispatch` d'Apple (10.4.6, désassemblé)

`GLDriver` 0x3198-0x3874, appelé par GLEngine sous la forme
`(ctx, gctx+0x4698 procs, bloc de changements r28)` :

- **Arguments lus** : `r3` ctx, `r4` table de procédures, `r5` bloc de changements.
  Du bloc, il ne lit que **+0x00** (masque principal), **+0x04** et **+0x10**
  (unités de texture : `(c[1] | c[4]) & 0xff` déclenche `gldLoadCurrentTexture`).
  `glrSetFunctions` (module `GLRasterARGB8888D32`, appelé en fin par `ctx+0xbc`,
  posé par `glsGetFunctionsFuncs`) ne lit que +0x00 et +0x04. **Personne n'écrit
  dans le bloc.** Cumuler par OU mot à mot est donc exact.
- **Retour** : **4** toujours, sauf échec d'allocation d'un tampon (`0x2720`).
- **Table de procédures** : il n'écrit **que** `+0x58/+0x5c/+0x60` (échanges),
  et seulement sous le bit **0x80**. (C'est `gldInitDispatch` qui remplit la
  table ; contrairement à ce que disait `verification-tcl.md` §8.3, `UpdateDispatch`
  ne « réécrit pas la table entière ».)
- **Par bit** : `0x80` → `gldSetDrawBufferPtrs` (+ `gldAllocateDrawBuffer`, procs
  d'échange) ; `0x100` → pointeurs de lecture ; `0x200` → alloue la profondeur si
  test actif et `ctx+0x88 == 0` ; `0x10000000` → stencil (alloc + masques
  `ctx+0x2a4/0x2a8`) ; `0x40000000` → tables de couleurs ; `1` → alpha ;
  `0x10040000` → masques couleur/stencil ; `0x400000` points ; `0x4000` lignes ;
  `0x4000000` rectangle de découpe ; `0x800000`/`0x3000000` polygones ; `0x1800`
  brouillard ; puis clé de rastérisation `ctx+0x6fc` et fonctions de pavage
  `ctx+0x27c/0x280`.
- `gldLoadCurrentTexture` remet `ctx+0x6b4..0x6d3` et `ctx+0x6f4` à zéro, puis
  pour chaque unité 1D/2D charge la texture liée (`ctx+0x10`, table de GLEngine).
  `gldLoadTextureLevelBuffer` ne reconvertit un niveau que si `niveau+0x0c` ≠
  format de base (`dt+0x578`) : c'est une texture **modifiée** (vidéos ROQ des
  écrans de Mars City, copies d'écran) qui coûte à chaque dispatch.
- **Qui lit ce qu'il calcule** (`ctx+0x22c..0x2a8`, `0x6b4..0x6fc`, `0x27c/0x280`,
  fonctions de GLRaster) : uniquement ses procédures de rastérisation
  (`gldRender*`, `gldTessellate*`, `gld*TextureFragment`, `gldCompute*Rho`) et
  `gldCreateContext`. **Aucun autre point d'entrée gld\***, ni GLEngine, ni le
  plugin (qui ne lit plus `CTX_TEXTURING` 0x6f4 ; `CTX_TEXUNITS` 0x10 est la table
  de GLEngine).
- Bouchons chez Apple (rien à rattraper avant eux) : `gldNoop`, `gldBufferSubData`,
  `gldBegin/EndPrimitiveBuffer`, `gldRenderVertexBuffer/Array`,
  `gldModifyTexSubImage`, `gldGenerateTexMipmaps`, `gldCopyTexSubImage`,
  `gldFlush`, `gldFinish`. `gldSwapBuffers` ne lit que le tampon de dessin.

Conclusion : la chose est **faisable sans risque de fond** — l'état qu'Apple
dérive ne sert qu'à ses procédures de rastérisation ; il suffit qu'il soit à jour
quand l'une d'elles travaille.

## 3. Conception (`guest/gldriver/pomppc_accel.c`, section « transmission paresseuse »)

- `PCtx` porte `lazy_m[5]` (masques cumulés), `lazy_pending`, `lazy_ret` (4).
- `gldUpdateDispatch` → `pomppc_lazy_update` : cumule le bloc. Si le bit **0x80**
  est là, transmet **tout de suite** le cumul (le plugin lit `ctx+0x94` aussitôt
  après, et `check_draw_buffer` à chaque dessin hôte) ; sinon garde, et rend
  `lazy_ret` (4). `pomppc_after_draw_buffer_change`, le crochetage et
  **`pomppc_geom_dispatch` tournent toujours** : le bit 0 (verrou de géométrie)
  ne dépend pas d'Apple.
- **Rattrapage** (`lazy_sync`, un seul `gldUpdateDispatch` avec le cumul, table
  décrochetée puis recrochetée comme avant) :
  - au début de `fallback()` — donc avant chaque `a_*` qui rend la main à Apple,
    `apple_guard`, et chaque procédure trampolinée non bouchon
    (`pomppc_proc_pre`) — **avant** la relecture `sync_to_sw_locked`, parce que
    c'est ce dispatch qui alloue au besoin le tampon de profondeur ; sauf pour
    les échanges `Swap58/5c/60` ;
  - dans `a_clear` avant le `Clear` partiel d'Apple (accumulation, stencil non suivi) ;
  - avant `gldInitDispatch` et `gldAttachDrawable` (`pomppc_lazy_flush`).
- Toujours sous `G.mu`, dans le fil qui appelle **pour ce contexte** : jamais de
  rattrapage pour un autre contexte (GLEngine ne sérialise qu'au sein d'un contexte).
- Actif seulement si `G.state > 0` et après le premier `gldInitDispatch` du contexte.
- Compteurs : `G.n_lazy_defer`, `n_lazy_eager`, `n_lazy_sync` ; une ligne
  `LAZYAPPLE image N : …` dans la note toutes les 500 images, et une ligne
  `lazy apple:` dans le bilan périodique (`POMPPC_GL_STATS=/chemin`). Un retour
  d'Apple ≠ 4 est noté (`LAZYAPPLE: gldUpdateDispatch d'Apple rend …`).
- Révision du plugin : `20260924-lazyapple`, la note l'annonce avec `lazyapple=0|1`.

Lanceurs dans l'invité : `~/doom3-lazy.command`, `~/doom3-nolazy.command`,
`~/prey-lazy.command`, `~/prey-nolazy.command` (copies des originaux avec
`POMPPC_GL_LAZYAPPLE` posé).

## 4. Défaut

**0 (éteint)**, faute de mesure avec. À passer à 1 (`LAZY_DEFAULT`) seulement
après : DOOM 3 avant/après à scène égale, image juste ; Prey ; un jeu à replis
(Warcraft III) pour vérifier qu'un repli voit un Apple à jour ; Zenerchi.

## 5. Risques et points ouverts

1. **Pas mesuré, pas validé en jeu.** Gain attendu : la part de
   `gldLoadCurrentTexture` (≈ 8,6 % du fil principal) tombe à presque rien quand
   il n'y a pas de repli ; il reste un rattrapage par image (bit 0x80), qui ne
   recharge que les textures liées à ce moment-là.
2. **Rattrapage par image au 0x80** : si le profil montre qu'il coûte encore,
   variante possible — n'envoyer sous 0x80 que `0x80` (et les bits d'allocation)
   et garder le reste en attente. Écartée pour l'instant : la clé de
   rastérisation (`ctx+0x6fc`) relit alors `ctx+0x6b4` (textures chargées au
   dernier rattrapage), qui peut désigner une texture détruite entre-temps.
3. **Échec d'allocation** (`0x2720`) pendant un rattrapage différé : GLEngine ne
   le voit pas (il est noté). En transmission immédiate il le voyait.
4. **Texture détruite pendant l'attente** : `ctx+0x6b4` peut désigner une texture
   d'Apple libérée jusqu'au rattrapage suivant. Même situation qu'en flux normal
   entre deux validations, et rien ne lit `0x6b4` avant un rattrapage, qui
   recharge (GLEngine pose les bits de texture au changement de liaison).
5. `fallback()` compte l'échange comme un repli (1/image en jeu) : il est exclu
   du rattrapage exprès ; un repli réel, lui, rattrape.


## Mesure du 24/09/2026 (après-midi), DOOM 3, cinématique d'intro, images 560-800

Plugin relu (a0935b2), génériques à taille déclarée actifs (caps 0xfe), scène à 165 dessins par image :

| Configuration | ms par image | relectures par image |
|---|---|---|
| A. défaut (LAZYAPPLE=0) | 22,6 | 0,01 |
| B. LAZYAPPLE=1 | 22,1 (« 665 205 dispatch gardés, 2 499 transmis au tampon de dessin, 0 avant une procédure d'Apple » à l'image 2 500) | 0,01 |
| C. plein écran (r_fullscreen 1) | 22,0 | 0,00 |

Le matin, à la même scène, le même plugin sans génériques compacts ni clés de réutilisation
dérivées du plan faisait 43-55 ms. La transmission paresseuse ne rapporte que ~2 % ici (la scène
change peu d'état de texture) ; à remesurer dans le hangar (1 280 dessins) avant de changer le
défaut. Elle reste ÉTEINTE par défaut.
