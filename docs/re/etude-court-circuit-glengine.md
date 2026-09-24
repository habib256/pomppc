# Court-circuiter GLEngine pour les dessins VBO ? Étude du 24/09/2026

Question : peut-on recevoir les `glDrawElements` / `glDrawArrays` d'un jeu à VBO (idTech4)
**avant** la machinerie de GLEngine, et émettre `DRAW_NATIVE` directement depuis l'état que le
plugin sait déjà lire, en ne rendant la main à GLEngine que hors domaine ?

Étude en lecture seule : profil `.run/d3/sample-nat.txt` (DOOM 3 en jeu, après DRAW_NATIVE),
listing de GLEngine 10.4.6 (`bench/devloop/jobs/1789676199209/out/gle-dis.txt`, `otool` ; mêmes
adresses que `tools/re/ppcanno.py` sur le binaire extrait), sources du plugin à la révision
`e9f0ff4`. **[L]** = lu dans le code, **[P]** = compté dans le profil, **[H]** = hypothèse,
**non relevé** = rien dans le dépôt ne l'établit.

---

## 0. Réponse courte

**La prémisse ne tient pas : les ~100 + 91 échantillons ne sont pas dans GLEngine.** Sur les
369 échantillons (45 % du fil principal) passés sous `glDrawElements`, **GLEngine lui-même en
consomme ~13 (1,6 %)**. Le reste, **~356, c'est le plugin** : `pomppc_geom_dispatch` (135,
appelé par *notre* `gldUpdateDispatch`) et `geom_draw_client` (198). Et **207 d'entre eux
sont le même verdict calculé deux fois** : `geom_ok` + `texture_ok` + `geom_format` au dispatch,
puis encore au dessin.

Court-circuiter GLEngine (options A et B) rapporterait au mieux ~1,5 % et ferait sauter des
mises à jour d'état dont le plugin dépend (matrice composée, masques sales des tableaux,
validation, bloc de changements). **Recommandation : ne pas court-circuiter. Rester dans
`RenderVertexArray` et faire calculer le verdict une seule fois (options C et D).** Gain
estimé : 15 à 25 % du fil principal.

---

## 1. Faits relevés

### 1.1 Le profil, décomposé (fil principal, 818 échantillons) **[P]**

Somme de tous les sites d'appel du fil principal (`sample-nat.txt` lignes 3-1654 ; le premier
site, ligne 35, en porte 214 sur 370) :

| nœud (inclusif) | éch. | part | enfants principaux |
|---|---|---|---|
| `my_glDrawElements` (interposeur gltrap) | 369 | 45,1 % | `gleDrawArraysOrElements_VBO_Exec` 362 |
| └ `gleDrawArraysOrElements_VBO_Exec` | 364 | 44,5 % | `gleExecuteVertexArrayRange` 208, `gldUpdateDispatch` 146, **lui-même 5**, `gleUpdateMatrixFunc` 2, `gleUpdateTextureTransform` 1 |
| ┊ └ `gldUpdateDispatch` (**le nôtre**, `pomppc_gld.c:599`) | 154 | 18,8 % | `pomppc_geom_dispatch` 135, `pomppc_lazy_update` 8, lui-même 4 |
| ┊ ┊ └ `pomppc_geom_dispatch` | 135 | 16,5 % | **`geom_ok` 51, `texture_ok` 41, `geom_format` 30** |
| ┊ └ `gleExecuteVertexArrayRange` → `geom_render_array` | 207 | 25,3 % | `geom_draw_client` 198 |
| ┊ ┊ └ `geom_draw_client` | 198 | 24,2 % | `send_state` 36, **`texture_ok` 32, `geom_ok` 32, `geom_format` 21**, `geom_draw_native` 17, lui-même 17, `geom_send_all` 11, `prog_sync` 9, `sync_to_host` 8 |

Temps propre (hors enfants) de GLEngine sous `my_glDrawElements` : `gleExecuteVertexArrayRange`
5, `gleDrawArraysOrElements_VBO_Exec` 3, `gleUpdateMatrixFunc` + `gleApplyViewScissorTransform`
3, `gleUpdateTextureTransform` 1, `glDrawElements_VBO_Exec` 1 : **≈ 13 échantillons, 1,6 %**.

Coûts propres notables du plugin sous `glDrawElements` : `tex_complete` 31, **`__pthread_self`
26**, `compute_state` 14, `unit_mask` 14, `find_tex` 13, `upload_texture` 13, **`getenv` 9**.

Biais de mesure : ce profil a été pris **avec l'interposeur `gltrap`** (`my_glDrawElements`
est dans la pile) ; son coût propre est de 1 échantillon, négligeable. `sample4.txt` (avant
DRAW_NATIVE) n'est pas comparable : les 660 échantillons du fil principal sont tous dans un
seul `geom_draw_client` → `0x1b014034` (processus figé ou fautif pendant la prise).

### 1.2 Le chemin d'un `glDrawElements` VBO dans GLEngine **[L]**

1. `libGL.dylib` `glDrawElements` → entrée `draw_elements` de la table de dispatch du contexte
   CGL. **Le code de `libGL` / `OpenGL.framework` n'est pas relevé** (aucun listing dans le
   dépôt). La pile du profil passe directement de `my_glDrawElements` à GLEngine : le saut de
   `libGL` est terminal (`bctr`), sans cadre.
2. L'entrée vaut `_glDrawElements_VBO_Exec` (0xa8a24, 34 instructions) : vérifie le type
   d'indices (sinon `GL_INVALID_ENUM` 0x500 en `gctx+0x7574`), met en cache le type
   (`gctx+0x48dc`, remet à zéro `+0x488c/+0x4890`), ajoute la base du VBO d'indices
   (`V+0x35c`, `+0x30`) et range le pointeur en `gctx+0x48d8` et `+0x4978`, puis
   `b _gleDrawArraysOrElements_VBO_Exec` (0xa8aa8).
3. `_gleDrawArraysOrElements_VBO_Exec` (0xa79e0-0xa80cc) :
   - validation : mode ≤ 9 (0xa7a14), nombre ≥ 0 (0xa7a30), programme de sommets valide si
     `GL_VERTEX_PROGRAM_ARB` (`gctx+0x4664`, `*(gctx+0x5420)+0x4cc`, 0xa7a4c) → erreurs
     0x500/0x501/0x502 ;
   - « tous les tableaux actifs sont dans des VBO » : `A+0x320/0x324` (actifs) comparé à
     `A+0x328/0x32c` (servis par un VBO) (0xa7aa8-0xa7acc), sinon chemin mixte 0xa7d90 ;
   - **masques sales des tableaux** : `A+0x340/0x344` versés dans chaque bloc par rendu
     (`A+0x450+4`, pas `0x8c`) puis remis à zéro, et `gctx+0x31c |= 0x00100000` (0xa7ad0-0xa7b44) ;
   - **matrices** : si `gctx+0x50ac | (gctx+0x505c & gctx+0x50b0)`, `_gleUpdateMatrixFunc`
     (0xa7b48-0xa7b64) — c'est là que la matrice composée que le plugin relit est recalculée ;
   - si `gctx+0x4691` (validation en attente) : complétude du framebuffer (0xa7b74-0xa7bac),
     sommes de couleurs des lumières (`_gleUpdateLightRGBASums`, 0xa7bec-0xa7c34), puis calcul
     du masque de changements sur le **bloc `gctx+0x310`** (0xa7c38-0xa7c8c) et, s'il est non
     nul, **appel de `gldUpdateDispatch`** : `bctrl` sur `gctx+0x4788` avec
     `(ctx_pilote = gctx+0x4688, procs = gctx+0x4698, bloc = gctx+0x310)` (0xa7ca4-0xa7cb8) ;
     comparaison du retour avec `gctx+0x7580/0x7581/0x759d` et, s'il diffère,
     `_gleUpdateDispatchCodeChange` (0xa7cbc-0xa7d14) ; puis **remise à zéro des 19 mots du
     bloc** (0xa7d40-0xa7d58) ;
   - si le verrou T&L `gctx+0x7580` est posé : `_gleExecuteVertexArrayRange` (0xa7d84) →
     `RenderVertexArray` (`gctx+0x4708`) ; retour non nul → fini (0xa7d8c) ;
   - sinon (ou refus) : chemin logiciel / `Begin/EndPrimitiveBuffer` par
     `_gleDrawArraysOrElements_Core` (0xa8084) ;
   - en sortie, si plus aucun tableau n'est dans un VBO : `b _gleUpdateDrawArraysFuncs` (0xa80b4).

Le travail propre de GLEngine sur ce chemin est donc **court et déjà « paresseux »** : il ne
valide que si `gctx+0x4691` est posé et n'appelle le pilote que si le bloc de changements
est non nul.

### 1.3 La table de dispatch (GLIFunctionDispatch) **[L]**

- Disposition : `docs/re/apple-headers/gliDispatch.h` (686 pointeurs, `0xab8` octets).
  Décalages calculés dans l'en-tête : `array_element` **+0x00c**, `draw_arrays` **+0x104**,
  `draw_elements` **+0x10c**, `draw_range_elements` **+0x654**, `draw_element_array_APPLE`
  +0x8ac, `draw_range_element_array_APPLE` +0x8b0, `multi_draw_arrays` +0x8dc,
  `multi_draw_elements` +0x8e0. **Confirmé par GLEngine** : `_gleUpdateDrawArraysFuncs`
  écrit `_glDrawArrays_Exec` en +0x104 (0x1cfb8), `_glDrawElements_Exec` en +0x10c (0x1cfe4),
  `_glDrawRangeElements_Exec` en +0x654 (0x1d010).
- **Deux tables par contexte** : `gliCreateContext(GLIContext *sortie, pixfmt, partage,
  r6, r7, r8)` range `r6` en **`gctx+0x4680`** et `r7` en **`gctx+0x4684`** (0x2e8c/0x2e90).
- **Règle d'écriture** (toutes les écritures de GLEngine, p. ex. 0x1cdc0-0x1cde8) :
  ```
  t = (T4684[k] != 0) ? T4684 : T4680 ;  t[k] = f
  ```
  (`addic r0,r0,-1 ; subfe r0,r0,r0` fait un masque « entrée de `T4684` nulle ».)
  **Qui est quoi n'est pas relevé** : l'en-tête public `CGLContext.h` du SDK 10.4 décrit
  `struct _CGLContextObject { GLIContext rend; GLIFunctionDispatch disp; … void *stak; }`
  **[H]** (à vérifier dans `/Developer/SDKs/MacOSX10.4u.sdk/…/OpenGL.framework/Headers/CGLContext.h`
  de l'invité). La règle ressemble à un mécanisme d'interception : une table « aval » (`T4684`)
  dont une entrée non nulle détourne la mise à jour, l'autre (`T4680`) étant la table vive
  **[H]**. Il se peut aussi que les deux pointeurs soient égaux.
- **GLEngine réécrit lui-même ces entrées en marche** :
  - `_glDrawElements_Exec` (0xa8c44), l'entrée générique, finit par
    `_gleEvalDrawArraysOrElements_Entires_Exec` (0x4d570), qui **choisit la variante**
    (VBO si `A+0x328/0x32c & A+0x320/0x324` ≠ 0, sinon VAR, CVA, IMM), **l'écrit dans la
    table** pour +0x104, +0x10c et +0x654 (0x4d5c8-0x4d648) puis y saute ;
  - `_gleUpdateDrawArraysFuncs` (0x1cd8c) **remet l'entrée générique** (ou les `_NoopExec` si
    `gctx+0x759c` et pas `+0x759d`) ; il a **13 appelants** : `_gleSetClientEnableFlag`
    (`glEnableClientState`, `glEnableVertexAttribArrayARB`), `_gleSetCurrentVertexObject`,
    `_glLockArraysEXT_Exec`, `_glVertexArrayRangeEXT_Exec`, `_glVertexArrayParameteriEXT_Exec`,
    `_gleUpdateDispatchCodeChange` (donc chaque changement du verrou T&L), `_gleSwitchToNonRevertRenderer`,
    `_gleUpdateFramebufferOperationFuncs`, `_gliSetCurrentPluginDispatchTable`, `_gliSwitchPlugin`,
    `_updateShaderState`, `_gleDrawArraysOrElements_IMM_Exec`, `_gleDrawArraysOrElements_VBO_Exec`.
  - À la création, **`gldInitDispatch` est appelé avant que les tables soient remplies** :
    `_gliSetCurrentPluginDispatchTable` (0x312c, qui appelle `gldInitDispatch`, 0x9f00), puis
    `_gliInitDispatchTable` (0x3134) et `_gliExecDispatchTable` (0x313c), qui écrivent tout,
    dont +0x104/+0x10c/+0x654 (0xcafc, 0xcb50, 0xf840). Une entrée posée par le plugin dans
    `gldInitDispatch` **serait écrasée aussitôt**.
  - Listes d'affichage : `_gliCompExecDispatchTable` (0x33b30) et `_gliCompDispatchTable`
    (0x3d6d4) existent (compilation, `glNewList`).

### 1.4 Le bloc de changements `gctx+0x310` **[L]**

19 mots (`li r2, 0x13`, 0xa7d40), remis à zéro après chaque dispatch. Mots connus :
- `+0x00` masque principal (bits d'Apple, `dispatch-paresseux.md` §2 : 0x80 tampon de dessin,
  0x100 lecture, 0x200 profondeur, 0x10000000 stencil, 0x4000000 découpe, 0x1800 brouillard…) ;
  bit 31 posé par la complétude du framebuffer (0xa7ba8) ;
- `+0x04` et `+0x10` : unités de texture (bits 0-7) ;
- `+0x08` : lu avec le bit 1 masqué si `cfg+0x79` (0xa7c70) ;
- `+0x0c` (= `gctx+0x31c`) : `0x00800000` env de sommets, `0x02000000` env de fragments,
  `0x00400000`/`0x01000000` texte des programmes (`programmes-arb.md:52`), `0x00100000`
  tableaux salis (0xa7b40), `0x10000000` verrou T&L perdu (0xa7d04), `0x15555` lumières
  (0xa7bf4) ; retenu avec le masque `0x17ffffff` si `cfg+0x79`, `0x17000000` sinon (0xa7c50-0xa7c60).

Conséquence : **sous `cfg+0x79` (notre cas), un simple `glProgramEnvParameter4fvARB` (bit
0x00800000) suffit à déclencher `gldUpdateDispatch`** au dessin suivant. DOOM 3 pose des
paramètres env à chaque interaction : il y a vraisemblablement **un dispatch par dessin**
**[H]**, ce que le rapport 135/198 (dispatch/dessin) rend plausible. À mesurer (lot 0).

### 1.5 Ce que fait le plugin, et deux fois **[L]**

- `gldUpdateDispatch` (`pomppc_gld.c:599-625`) : transmission paresseuse à Apple
  (`pomppc_lazy_update`, `pomppc_accel.c:12060`, allumée par défaut, `LAZY_DEFAULT 1`,
  `pomppc_accel.c:517`), crochetage, puis **`pomppc_geom_dispatch`** (`pomppc_accel.c:9254`).
- `pomppc_geom_dispatch` : `geom_ok` (9263), **`texture_ok`** avec téléversement (9269-9271),
  et sous `ARRAY=2` **`geom_format`** rangé dans `p->geom_fmt` (9294). Le 3ᵉ argument (le bloc
  de changements) **n'est pas lu** par le verdict.
- `geom_draw_client_unsafe` (`pomppc_accel.c:8889`) : **refait `geom_ok`, `texture_ok`**
  (8912) **et `geom_format`** (8916) — `p->geom_fmt` n'est pas réutilisé —, puis `prog_sync`,
  `va_gen_sizes`, `va_sources_ok`, `va_scan_idx` (8926), `check_draw_buffer`, `sync_to_host`,
  **`send_state` → `compute_state`** (8943, vecteur d'état complet recalculé puis comparé clé
  par clé, `pomppc_accel.c:4491-4540`), `geom_send_all` (8944), `va_plan_build`, puis
  `geom_draw_native` (8950).
- Coûts parasites :
  - `pthread_self()` à **chaque** `tex_lv0_sig` (`pomppc_accel.c:3156`) et à chaque dessin
    (`pack_thr`, 8871) : 26 échantillons de `__pthread_self` (appel coûteux sous QEMU) ;
  - `getenv("POMPPC_GL_T3DDUMP")` à chaque `target_probe` (`pomppc_accel.c:3594`), appelé pour
    toute unité dont la cible est cube/3D/rectangle — DOOM 3 lie une carte de cube de
    normalisation — et `getenv("POMPPC_GL_DUMP_TRIGGER")` à chaque `draw_probe` (6943),
    appelé à la fin de chaque `texture_ok` : 9 échantillons de `getenv` ;
  - `texturing_on` + `texture_unit_ok` appellent `tex_complete` plusieurs fois par unité
    (31 échantillons propres).

---

## 2. Les quatre options

### (A) Réécrire les entrées +0x104 / +0x10c / +0x654 de la table de dispatch

**Mécanisme.** Le plugin connaît `gctx` (`gctx_of`, `pomppc_accel.c:5584`). Il lirait
`T = *(gctx+0x4680)` (ou `+0x4684`, §1.3) et y poserait ses propres `draw_elements`,
`draw_arrays`, `draw_range_elements`, qui recevraient `(GLIContext = gctx, mode, count, type,
indices)`, feraient le verdict et `DRAW_NATIVE`, et appelleraient l'entrée d'origine hors domaine.
- **Pas dans `gldInitDispatch`** à la création : écrasé juste après (0x3134, 0x313c).
- **Écrasé en marche** par `_gleUpdateDrawArraysFuncs` (13 appelants, dont chaque
  `glEnableClientState`) et `_gleEvalDrawArraysOrElements_Entires_Exec`. Deux parades :
  réinstaller à chaque `gldUpdateDispatch` (des dessins passeraient par GLEngine entre-temps),
  ou, **si `T4684` est une table distincte**, poser l'entrée d'origine dans `T4684[k]` (non
  nulle) et la nôtre dans `T4680[k]` : GLEngine écrirait alors ses variantes dans `T4684`, que
  notre crochet appellerait en aval. **Non relevé** : il faut d'abord savoir ce que sont les deux
  pointeurs (§4, R1).

**Gain.** Le temps propre de GLEngine : ≈ 13 échantillons (1,6 %). Il faudrait de toute façon
refaire la mise à jour des matrices (3 échantillons) : **net ≈ 10, soit ~1,2 %**. Les 135
échantillons de `pomppc_geom_dispatch` ne sont pas « gagnés » par A : c'est notre code, qu'on
peut alléger sans rien court-circuiter (C).

**Pertes et risques** (tout ce que `VBO_Exec` fait et que le crochet sauterait, §1.2) :
- la **matrice composée** et ses dérivées (`_gleUpdateMatrixFunc`, fonction interne non
  exportée) : le plugin les lit (`GS_MAT_MVP`, `GS_MAT_NORMAL`) → géométrie fausse ;
- les **masques sales** `A+0x340/0x344` et `gctx+0x31c |= 0x00100000` : ils alimentent
  `gldModifyVertexArray` et nos propres réserves VBO ;
- le **bloc `gctx+0x310`** : jamais consommé, jamais remis à zéro, `gctx+0x4691` jamais
  retombé ; `gldUpdateDispatch` n'est plus appelé → **le verrou `gctx+0x7580`, le descripteur
  `cfg+0x11c` et la transmission paresseuse à Apple ne sont plus réévalués** ; au premier dessin
  hors domaine, GLEngine rattraperait tout, mais le verdict du domaine lui-même vient de là ;
- la validation GL (`GL_INVALID_ENUM/VALUE/OPERATION`) et le cache du type d'indices
  (`gctx+0x48dc`, `+0x48d8`, `+0x4978`) ;
- les appelants qui ne passent pas par `libGL` (CGL macros, listes d'affichage en compilation)
  et `_gliSwitchPlugin` (changement de rendu) : le crochet disparaît en silence.

**Coût.** Élevé : relevé CGL (R1), réimplémentation partielle de `VBO_Exec`, appels à des
fonctions internes de GLEngine par adresse (fragile, 10.4.6 seulement), gestion de la
réinstallation. **Rapport gain/risque défavorable.**

### (B) Interposition dyld installée par le plugin

**Mécanisme.** `gltrap` s'appuie sur la section `__DATA,__interpose`
(`tools/guest/gltrap/gltrap.c:217-259`), lue par dyld **au lancement** pour les images de
`DYLD_INSERT_LIBRARIES`. Le plugin est chargé bien plus tard, par `libGLSystem` (`glsLoadRenderers`, mécanisme de
chargement exact non relevé) : **[H] la section n'y est pas appliquée** (dyld de 10.4 ne traite
l'interposition que des bibliothèques insérées ; à vérifier, R3). Reste la réécriture à la main
des pointeurs de symboles du jeu (`__la_symbol_ptr`/`__nl_symbol_ptr` de `_glDrawElements`,
via `_dyld_image_count`, `_dyld_get_image_header` et la table des symboles indirects), façon
« fishhook ».

**Gain.** Au-delà de A, seulement le saut de `libGL` (`CGLGetCurrentContext` + indirection),
invisible au profil (< 0,5 %). Le crochet arrive **sans `gctx`** : il faudrait
`CGLGetCurrentContext()->rend` **[H]**, ou revenir dans GLEngine. **B cumule les risques de A**
et en ajoute : pointeurs paresseux pas encore liés au chargement du plugin, jeux qui appellent
par CGL macros ou par `dlsym`, modification de l'image du jeu.

**Coût.** Élevé, pour un gain qui est un sous-ensemble de A. **Dominée.**

### (C) Rester dans `RenderVertexArray`, alléger ce qui se passe avant

**Mécanisme.** Ce qui précède `RenderVertexArray` est aujourd'hui, pour 90 %, notre propre
`gldUpdateDispatch`. Les réglages de canal sont déjà les bons : `cfg+0x78 = 1` et `ARRAY=2`
(`cfg+0x11c = 0`, `pomppc_geom_dispatch` 9281-9295) envoient `glDrawElements` VBO droit sur
`_gleExecuteVertexArrayRange` ; `cfg+0x7a = 1` évite `_gleForceToSoftwareTCL`
(`capacites-glengine.md` §5.3). Les leviers restants :
1. **Verdict par liste blanche du bloc de changements** : `gldUpdateDispatch` reçoit le bloc
   (`c`, `pomppc_gld.c:599`). Quand **seuls** des bits neutres pour le verdict sont posés (env
   de programmes `0x00800000|0x02000000` en `+0x0c`, puis ceux que le relevé R4 aura prouvés),
   `pomppc_geom_dispatch` rend le verdict précédent sans rien recalculer.
2. **Verdict unique** : ce que le dispatch calcule (`TexInfo`, `fmt`, `gs`) est gardé dans le
   `PCtx` avec une clé (voir lot 2) et le dessin le reprend au lieu de le refaire (c'est le
   point commun avec D).
3. GLEngine : rien à gagner d'utile (≈ 13 échantillons, déjà paresseux).

**Gain.** Jusqu'à ~100 des 135 échantillons de `pomppc_geom_dispatch` (**~12 %**), selon la
proportion de dispatches « env seulement » (à mesurer, lot 0).

**Risques.** Un bit mal classé laisse un verdict périmé : texture non téléversée, format faux.
Les parades : liste blanche (on ne court-circuite que ce qui est prouvé neutre), mode de
contrôle qui recalcule et compare (`POMPPC_GL_VERDICTCHECK=1`), clé de validité (image,
époque des textures).

**Coût.** Faible à moyen : quelques dizaines de lignes, pas de nouveau relevé en dehors de R4.

### (D) Réduire nos coûts dans `geom_draw_client_unsafe`

**Mécanisme.**
1. **Reprendre le verdict du dispatch** (`geom_ok`, `texture_ok`, `geom_format` : 85
   échantillons au dessin) quand la clé est valide ; sinon le recalculer comme aujourd'hui.
2. **`send_state`/`compute_state`** (36) : ne recalculer le vecteur d'état que si un dispatch
   a eu lieu depuis le dernier envoi pour ce contexte, ou si `ti`/le mode raw ont changé. **À
   prouver d'abord** (R5) : tout ce que lit `compute_state` passe-t-il par le bloc de
   changements ? Les matrices (`glLoadMatrixf`) en particulier.
3. **Parasites** : `pthread_self()` une fois par entrée (dispatch ou dessin) dans une variable
   globale sous `G.mu`, relue par `tex_lv0_sig` et `geom_draw_client` (≈ 20 éch.) ;
   `getenv` de `target_probe` et `draw_probe` mis en cache statique comme les autres
   (`trifilter_on`, R1 du 24/09) (≈ 9 éch.) ; `tex_complete` mémorisé par texture et par image
   comme `ok_frame` (partie des 31).
4. `va_scan_idx` n'apparaît pas au profil : rien à faire. `geom_send_all` (11) et `prog_sync`
   (9) ont déjà leurs clés de changement.

**Gain.** Verdict repris : ~85 (10 %) ; parasites : ~30 (4 %) ; `compute_state` sauté : jusqu'à
~30 (4 %). **Total ~15-18 %.**

**Risques.** Ceux de C pour le verdict repris ; aucun pour les parasites (même comportement).

**Coût.** Faible.

### Tableau comparatif

| option | gain estimé (fil principal, 818 éch.) | risque | coût | preuve du mécanisme |
|---|---|---|---|---|
| **A** table de dispatch | **~1,2 %** (≈ 10 éch.) | **élevé** : matrices, masques sales, bloc de changements, verrou T&L jamais réévalués ; crochet écrasé par 13 appelants | élevé (relevé CGL, fonctions internes) | [L] offsets et écrivains ; **non relevé** : identité de `T4680`/`T4684` |
| **B** interposition depuis le plugin | ≤ A (+ < 0,5 %) | élevé + chargement tardif, CGL macros | élevé | [H] dyld 10.4 n'interpose pas un bundle chargé tard |
| **C** alléger l'avant-`RenderVertexArray` | **~8-12 %** | moyen, borné par liste blanche et mode de contrôle | faible-moyen | [L] bloc `gctx+0x310` passé en 3ᵉ argument ; bits partiels |
| **D** alléger `geom_draw_client_unsafe` | **~12-18 %** | faible (parasites : nul) | faible | [L] duplication 8912/8916 vs 9263-9294 ; [P] 85 + 30 éch. |

C et D se recouvrent (le verdict unique sert aux deux) : **C + D ≈ 20-25 %** du fil
principal, soit, tant que le fil principal borne l'image, un gain du même ordre en ms/image.

---

## 3. Recommandation

Ne pas court-circuiter GLEngine. Sa part propre est de 1,6 % ; ce qui coûte, c'est le plugin,
qui calcule le même verdict au `gldUpdateDispatch` puis au `RenderVertexArray` de chaque dessin.
Le chemin actuel (dispatch → `RenderVertexArray` → `DRAW_NATIVE`) est le bon ; il faut qu'il
ne fasse qu'**un** verdict par changement d'état, et un verdict **presque gratuit** quand le
changement ne concerne que des paramètres de programme. Garder A comme piste documentée,
à rouvrir seulement si, après C et D, `gleDrawArraysOrElements_VBO_Exec` et ses enfants GLEngine
dépassent 5 % du fil principal.

---

## 4. Plan par lots

Chaque lot : un changement, une épreuve, et un repli par variable d'environnement tant que la
mesure en jeu n'est pas faite.

**Lot 0 — compter (aucun changement de comportement).**
Compteurs dans `pomppc_geom_dispatch` et `geom_render_array` : dispatches par image, dessins
par image, dessins sans dispatch depuis le précédent, et **histogramme des motifs du bloc de
changements** (les 5 premiers mots, masqués ; les 8 motifs les plus fréquents par tranche de
500 images, dans la note). Épreuve : note de DOOM 3 à `demo_mars_city1` et de Prey ; on sait
alors quelle part des dispatches est « env seulement » et si l'estimation de C tient.

**Lot 1 — parasites (sans risque).**
`pthread_self` une fois par entrée ; `getenv` de `target_probe` et `draw_probe` en statique ;
`tex_complete` mémorisé par image. Épreuves : `gltest texup texcache texdelmid cube tex3d
arbvp arbfp varrayvbo` inchangés à l'octet par rapport à avant ; profil `sample` : `__pthread_self`
et `getenv` < 3 échantillons ; `frames.csv` à scène égale.

**Lot 2 — verdict unique (D1, et C2).**
`pomppc_geom_dispatch` range dans le `PCtx` : `ok`, `TexInfo`, `fmt`, `gs`, et une clé
(`G.n_frames`, époque globale des textures incrémentée par toute création, modification,
destruction, éviction ou téléversement de texture, `p->geom_lost`, `p->qctx`, pointeur du VAO,
`VA_EN_HI/LO`, `vp_rec`/`fp_rec`, `G.state`). `geom_draw_client_unsafe` reprend le verdict si la
clé est identique, le recalcule sinon (et le range). `POMPPC_GL_VERDICTCHECK=1` recalcule
toujours et note les écarts (`VERDICT écart : …`). Épreuves : la suite `gltest` ci-dessus plus
`mixte game dlist gl15 vbocolor arbvp0vbo` justes, **zéro écart** sous `VERDICTCHECK=1` sur
DOOM 3 (cinématique + Mars City) et Prey ; `sample` : `geom_ok`+`texture_ok`+`geom_format` sous
`geom_draw_client` < 10 échantillons ; `frames.csv`.

**Lot 3 — liste blanche du bloc de changements (C1).**
Après R4, `pomppc_geom_dispatch(ctx, bloc)` rend le verdict gardé si le bloc ne contient que
des bits prouvés neutres (au départ : `+0x0c & ~(0x00800000|0x02000000) == 0` et les autres
mots nuls). Le crochetage et `lazy_update` restent inchangés. Épreuves : `VERDICTCHECK=1`
étendu au dispatch (zéro écart) ; `gltest arbvp arbfp arbvp0vbo` (qui changent des env entre
dessins) justes ; `sample` : `pomppc_geom_dispatch` divisé par deux au moins ; `frames.csv`.

**Lot 4 — `compute_state` sauté (D2).**
Seulement si R5 prouve que tout ce que lit `compute_state` fait poser un bit du bloc (sinon,
abandonner). Épreuves : mode de contrôle qui compare le vecteur recalculé à `p->st`
(`POMPPC_GL_STATECHECK=1`), zéro écart sur DOOM 3/Prey et sur `gltest state lit texgen clip
fogz blendc stencil` ; `sample` : `compute_state` < 5.

**Lot 5 — bilan.** Nouveau profil `sample` de DOOM 3 à la même scène que `sample-nat.txt`, et
décision sur A : si la part propre de GLEngine sous `glDrawElements` reste sous 5 %, A est
classée.

---

## 5. Relevés GLEngine et CGL encore à faire en VM

| n° | question | méthode |
|---|---|---|
| **R1** | Que sont `gctx+0x4680` et `gctx+0x4684` ? Égaux ? L'un vaut-il `(char*)CGLGetCurrentContext() + 4` (`&ctx->disp`) ? Contenu de `T4684` (tout à zéro ?) | sonde dans le plugin au premier `RenderVertexArray` : noter les deux pointeurs, `CGLGetCurrentContext()`, et un vidage de `T4684` (0xab8 octets) ; lire `CGLContext.h` du SDK 10.4 dans l'invité |
| **R2** | `libGL` `glDrawElements` : comment trouve-t-il le contexte (TLS, `CGLGetCurrentContext`) et quelle table appelle-t-il ? | `otool -tV /System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGL.dylib` par un job devloop (comme `gle-dis.txt`), ou désassemblage par le stub GDB de QEMU (`-s`, `x/40i` sur l'adresse de `_glDrawElements` résolue dans le processus) |
| **R3** | dyld 10.4 applique-t-il `__interpose` d'une image chargée par `NSAddImage` après le lancement ? | bundle d'essai avec une section `__interpose` sur `glClear`, chargé par `gltest` ; compter les appels interceptés |
| **R4** | Quels bits du bloc `gctx+0x310` posent : `glProgramEnvParameter*`, `glProgramLocalParameter*`, `glBindProgramARB`, `glBindTexture`, `glActiveTexture`, `glEnable/Disable` (texture, blend, profondeur, stencil…), `glEnableClientState`, `glBindBufferARB`, `glVertexAttribPointerARB`, `glLoadMatrixf`/`glMatrixMode` ? | sonde dans `gldUpdateDispatch` (vidage des 19 mots) + scène `gltest` dédiée qui fait **un seul** changement entre deux dessins pour chaque appel de la liste ; tableau bit → appel |
| **R5** | Tout ce que lit `compute_state` et `send_state` a-t-il un bit dans le bloc ? Les matrices en particulier (`gctx+0x50ac`, `0x505c`, `0x50b0` sont des drapeaux séparés, §1.2) | même scène que R4, avec `POMPPC_GL_STATECHECK=1` : dessins sans dispatch dont le vecteur d'état a changé |
| **R6** | Nombre réel de dispatches par dessin dans DOOM 3 et Prey | lot 0 (compteurs du plugin), pas de désassemblage |
| **R7** | `_gleUpdateMatrixFunc` : quels champs `GS_*` recalcule-t-il (MVP, inverses, normale) ? Utile seulement si A est rouverte | listing `gle-dis.txt` (0x7e48-0x8674) et sonde avant/après dans le plugin |
