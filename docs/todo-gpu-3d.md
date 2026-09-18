# TODO — Tiger en OpenGL 1.5, le plus vite possible, avec le maximum de travail sur le GPU hôte

**Objectif unique** : que Mac OS X Tiger sous QEMU annonce et tienne OpenGL 1.5, et que tout ce
qui peut s'exécuter sur le GPU de l'hôte s'y exécute. Chaque tâche ci-dessous est jugée à cette
aune : *combien de travail quitte le PowerPC émulé ?*

Ce fichier est le tableau de bord ; il est tenu à jour à chaque lot. Le contexte long est dans
`docs/roadmap-opengl15.md`, la conception et les offsets relevés dans `docs/gpu-3d-tiger.md`,
les relevés de rétro-ingénierie nouveaux dans `docs/re/`.

## État (19/09/2026)

- **Lot 4, côté hôte (19/09/2026) : protocole v10, ce qui manquait à 1.2–1.4.** Cibles 1D,
  3D, cube et rectangle (`TEX_CREATE3`), données **au format de l'application converties par
  l'hôte** (18 couples format/type, profondeur, S3TC décompressé par le cœur, `TEX_IMAGE3`),
  sous-images (`TEX_SUBIMAGE`), `GL_MIRRORED_REPEAT`, `GL_CLAMP_TO_BORDER` et couleur de
  bordure, niveaux de base et max, bornes et biais de LOD (texture et unité), textures de
  profondeur et comparaison d'ombre, mipmaps automatiques calculés par le cœur ; couleur
  secondaire (`QGPU_SK_COLOR_SUM`) et paramètres de point. `run_v10` :
  **0 échec sur le backend logiciel et sur le GPU hôte** (RTX 4060 Ti), cube compris — son
  orientation est vérifiée contre le pilote. `QGPU_REG_CAPS` publie enfin les bits résolus à
  chaud (occlusion, et le nouveau `QGPU_CAP_GL14`). **Rien n'est encore visible des
  applications** : le plugin doit suivre, et cela demande la VM de développement.
  `docs/protocole-v10-textures.md`.

- Protocole **v6** : rastérisation sur l'hôte, 4 unités de texture, GL_COMBINE, brouillard,
  lignes, points, **stencil**. Sous-ensemble d'OpenGL 1.3.
- Zenerchi ~50 img/s (plein écran). Marble Blast Gold, en fenêtre 800x600 : **28 img/s sur une
  scène de 2 600 triangles, 63 sur 650** depuis la présentation directe en fenêtre (16-17 avant).
  ⚠ Les « 20-80 img/s » notés le 17/09 pour ce jeu étaient **doublés** : le compteur comptait
  aussi le `glFinish` que Marble Blast fait à chaque image. Corrigé ; les chiffres de Zenerchi,
  qui n'en fait pas, étaient justes.
- **Dans les deux jeux c'est le PowerPC émulé qui limite** : GLEngine transforme, éclaire et
  découpe chaque sommet sur l'invité ; le débit de Marble Blast suit le nombre de triangles.
- Le renderer annonce **« 1.1 POMPPC-1.0 » et 42 extensions** (18/09/2026, tâche 4.1) : la plus
  haute version dont *toutes* les fonctions sont tenues. Pas 1.2, parce que les **textures 3D**
  manquent dans toute la chaîne. Le relevé fonction par fonction est dans
  `docs/re/version-extensions.md`.
- **Fusion des dessins faite (18/09/2026)** : le plugin recolle les `DRAW_RAW`
  **consécutifs** en `GL_TRIANGLES` **indexés** (rubans, éventails, quads, bandes de quads,
  polygones), sans déplacer un seul sommet — seuls des indices u16 sont écrits. Marble Blast :
  **1 065 → 65-85 `DRAW_RAW` par image**, 7 → 90-125 sommets par dessin, soumission 3,5 → 1,6 ms
  par image, **+10,7 % d'images par seconde en moyenne** (jusqu'à +23 % sur une fenêtre lourde).
  Image **identique au pixel près** à celle sans fusion sur les 23 scènes de `gltest`.
  Interrupteur `POMPPC_GL_MERGE` (défaut : activé). Voir `docs/gpu-3d-tiger.md` §4.7.
- **Lot 2 fait (18/09/2026)** : protocole **v7** et **chemin brut** dans le plugin — GLEngine ne
  transforme, n'éclaire, ne découpe ni n'élimine plus les faces tant que l'état est dans le
  domaine ; tout cela s'exécute sur le GPU de l'hôte. `gltest spin` ×2,2, `gltest game` ×1,25,
  Marble Blast **×1,6 à ×1,96 sur les scènes lourdes** (24 → 48 img/s au plus lourd), +48 % en
  moyenne. Le PowerPC émulé reste le facteur limitant, mais pour une autre raison : le nombre de
  dessins (voir ci-dessous).

## Règles de travail (plusieurs agents)

- **Une seule VM de développement** (`disks/tiger-dev.raw`, `tools/guest/devloop.py`) : un seul
  agent à la fois y lance des jobs. Le travail hôte (protocole, backends, tests natifs) et la
  lecture des désassemblages se font en parallèle, sans VM.
- Un lot = une fonction, avec sa **preuve** : cas dans `tests/qgpu_core_test.c` (mêmes pixels sur
  les backends logiciel et OpenGL), scène `guest/gltest` comparée au rendu d'Apple
  (`POMPPC_GL_DISABLE=1`), et mesure avant/après sur un jeu quand c'est une tâche de vitesse.
- Hors domaine, le rendu d'Apple reprend la main. **Rien n'est annoncé qui ne soit tenu.**
- Tout relevé dans `OpenGL.framework` s'écrit dans `docs/re/`, avec l'adresse et la méthode.

---

## Axe 1 — Sortir la géométrie de l'invité (le plus gros gain de vitesse)

| # | Tâche | Statut |
|---|---|---|
| 1.1 | ✅ *relevé fait (`docs/re/capacites-glengine.md`) et **vérifié dans l'invité le 18/09/2026** (`docs/re/verification-tcl.md`, V1 et V6) : le bloc de configuration est confirmé à l'octet près, mais le verrou réel est le **bit 0 du retour de `gldInitDispatch`/`gldUpdateDispatch`**, relu à chaque changement d'état (donc repli possible par lot d'état), et il faut en plus publier un descripteur de sortie de sommet en `cfg+0x11c` sans quoi GLEngine jette la géométrie. Avec les deux, les sommets arrivent en `BeginPrimitiveBuffer`/`EndPrimitiveBuffer` en **coordonnées d'objet**. Restent V2–V5 et V7.* **Codes du descripteur relevés le 18/09/2026** (`docs/re/descripteur-de-sommet.md`) : une entrée vaut `(code << 10) | ((composantes − 1) << 8) | décalageEnMots`, les codes sont les indices d'attribut d'entrée (0 position, 1 normale, 2 couleur, 3 brouillard, 4 couleur secondaire, 5 poids, 8..15 coordonnées de texture, 16..31 attributs génériques, 32..39 matériau ; **le code 6, drapeau d'arête, plante GLEngine**). Confirmé dans l'invité : on reçoit les attributs **bruts** — ni transformation, ni éclairage, ni texgen, ni matrice de texture, ni découpage, ni élimination de face — et **aucun code ne donne de sortie transformée ou éclairée** ; `glDrawArrays`, `glDrawElements` et les listes d'affichage passent tous par `Begin`/`EndPrimitiveBuffer`, les modes de primitive (rubans, éventails, quads) sont transmis tels quels. Aucun pilote de 10.4.6 ne publie `cfg+0x11c` : le GeForce3 et le Radeon annoncent la T&L matérielle mais passent par `RenderVertexArray`/`RenderVertexBuffer`, le Rage 128 reçoit des sommets **déjà transformés** dans le sommet interne de GLEngine. **Décision : option A (attributs bruts, DRAW_RAW v7)** ; borne supérieure du gain mesurée sur `gltest spin` à rastérisation négligeable : 415 → 788 img/s (×1,9).* Le verrou est **l'octet `+0x79` du bloc de configuration passé à `gldCreateContext`** : à 1, GLEngine envoie la géométrie brute (`BeginPrimitiveBuffer` +0x50, `RenderVertexBuffer` +0x4c, `RenderVertexArray` +0x70) ; il est figé à la création du contexte. **Relever la négociation des capacités** : ce qui décide GLEngine à appeler les entrées « hautes » du pilote (`CreateVertexArray`, `RenderVertexArray` +0x70, `AllocVertexBuffer`, `CreatePipelineProgram`) plutôt que de transformer lui-même. Lire comment les pilotes ATI/NVIDIA de 10.4.6 répondent à `GetRendererInfo`, `GetInteger`, `GetString`. **Verrou de tout l'axe.** | relevé ✅, **vérifié** ✅ |
| 1.2 | ✅ **Fait le 18/09/2026** : relevé par lecture (`docs/re/tableaux-de-sommets.md`) **puis établi par l'expérience dans l'invité** (`docs/re/etat-tcl.md`, sondes `lightprobe`, `matprobe`, `mtxprobe`, `tgprobe`, `xformprobe` de `guest/gltest`, diff des vidages par `tools/re/diffstate.py`). Sont confirmés offset par offset : les 24 matrices (`GS+0x1860 + m·0x40`, index de mode `16+u` pour la texture de l'unité `u`), le viewport et `glDepthRange` avec la formule exacte échelle/biais, l'élimination des faces, la normalisation, l'ombrage, les 8 lumières (`GS+0x24c0 + i·0x80`, position et direction de spot **en coordonnées œil**, seuil de spot stocké en **cosinus**), le modèle d'éclairage, les **deux matériaux qui sont DANS le bloc** (`GS+0x28c0`/`GS+0x2b00`, et non hors de lui), `GL_COLOR_MATERIAL`, le TexGen (pas `0x94` par unité, `0x24` par coordonnée, plan œil transformé), les 6 plans de découpe (coordonnées œil), le brouillard complet (densité/début/fin/source de coordonnée) et les paramètres de point. Les **valeurs courantes** (couleur, normale, couleur secondaire, coordonnée de brouillard, coordonnées de texture) sont **avant** le bloc, en `GS − 0x360 + x`. Le bloc `#define GS_…` prêt à recopier est en §10 de `docs/re/etat-tcl.md`. | ✅ **fait** |
| 1.3 | ✅ **Fait** : protocole **v7** — `SET_MATRIX`, `VIEWPORT`, `DEPTH_RANGE`, `SET_LIGHT`, `SET_MATERIAL`, `SET_LIGHT_MODEL`, `SET_TEXGEN`, `SET_CLIP_PLANE`, `SET_CURRENT`, `DRAW_RAW` (10 modes, indexé), 17 clés d'état de géométrie ; étage géométrique complet dans le backend de référence et dans le backend OpenGL ; `run_v7` de `tests/qgpu_core_test.c` sur les deux backends. `docs/protocole-v7-geometrie.md`. | ✅ **fait** |
| 1.4 | ✅ **Fait le 18/09/2026** : le plugin pose le verrou par le **bit 0 du retour de `gldInitDispatch`/`gldUpdateDispatch`**, publie son **descripteur de sortie de sommet** en `cfg+0x11c`, et reçoit les attributs bruts dans `Begin`/`EndPrimitiveBuffer` — `BeginPrimitiveBuffer` rend un pointeur **dans la fenêtre partagée**, à la disposition exacte de `DRAW_RAW` : **zéro recopie de sommet**. L'état T&L (matrices, viewport, 8 lumières, 2 matériaux, texgen, 6 plans de découpe, valeurs courantes, clés v7) ne part que quand il change. Hors domaine, le retour d'Apple est rendu tel quel et GLEngine reprend tout : le repli est **par lot d'état**, vérifié exact. Interrupteur `POMPPC_GL_GEOM` (défaut : **activé**). **Mesures** : `gltest spin` 406 → **900 img/s** (16×16), 420 → **822** (256×256), 323 → **481** (640×480) ; `gltest game` 684 → **854** ; **Marble Blast Gold, scènes lourdes 25-30 → 43-48 img/s (×1,6 à ×1,96)**, moyenne des fenêtres de jeu 42,4 → **62,6 img/s (+48 %)**. Voir `docs/gpu-3d-tiger.md` §4.7. | ✅ **fait** |

## Axe 2 — Ne plus recopier, ne plus attendre

| # | Tâche | Statut |
|---|---|---|
| 2.1 | **Objets tampon** (`CreateBuffer`, `FlushBuffer`, `BufferSubData`) sur tampons hôte : les maillages statiques ne retraversent plus la fenêtre partagée. (OpenGL 1.5.) | à faire |
| 2.2 | ✅ **Fait le 18/09/2026**, de bout en bout. **Hôte** : protocole **v9** — file de 16 soumissions, thread de rendu, `QGPU_DOORBELL_ASYNC`, `FENCE_SUBMITTED`, `SUBMIT_ST`, `QUEUE_FREE`, `ERRORS`, `QUEUE_DEPTH`, IRQ `DONE` posée par un *bottom half* (`docs/protocole-v9-asynchrone.md`). **Kext** : le drapeau voyage dans les bits hauts de `len` de `QGPU_UC_SUBMIT` — l'ABI de Darwin 8 compare le *nombre* d'arguments scalaires au bit près, donc ajouter un scalaire aurait cassé tous les appelants existants ; `QGPU_UC_WAIT_FENCE` ne scrute plus, il dort sur la command gate et `irqAction` le réveille, avec un `IOTimerEventSource` comme base de temps du délai maximal (Tiger n'a pas `commandSleep(event, deadline, …)`, arrivé en 10.5) ; `destroyClientObjects` draine la file avant de rendre la tranche. **Plugin** : `POMPPC_GL_ASYNC` (défaut **activé**), tranche coupée en **deux moitiés** alternées à chaque soumission, relectures **différées** jusqu'au moment où l'invité en a besoin, présentation directe de l'image *n−1* au début de l'échange suivant (**une image de latence, jamais plus**). **Mesures Marble Blast** : temps de soumission **1,6 → 0,06 ms/image** (÷26), attente restante **0,1 ms/image**, **79,4 → 88,1 img/s (+11 %)** sur les fenêtres de jeu. Sur `gltest` (hors écran, relecture à chaque image) : **aucun gain**, c'est attendu. | ✅ **fait** |
| 2.3 | **Zero-copy à la présentation** : le device écrit lui-même dans la VRAM (plage déclarée par le kext) ; plus de relecture ni de recopie par l'invité. | à faire |
| 2.4 | **Présentation en fenêtre sans attendre le WindowServer** : écriture directe dans le rectangle de la surface à l'écran, tant que rien ne la recouvre et que le curseur n'y bouge pas ; un échange normal toutes les 90 images rafraîchit la fenêtre. Marble Blast : +70 %. | ✅ fait |
| 2.5 | Téléversement de textures sans conversion invité quand le format est connu de l'hôte (BGRA, 565, 1555…) : la conversion passe sur l'hôte. | **hôte fait (v10, 19/09/2026)** : `TEX_IMAGE3` prend le couple (format, type) de l'application et son pas de ligne ; `TEX_SUBIMAGE` pour les textures qui changent. Reste le plugin : envoyer `LV_FORMAT`/`LV_TYPE`/`LV_ROWPIX` au lieu de `convert_level` |
| 2.6 | Opérations de pixels sur l'hôte (`DrawPixels`, `CopyPixels`, `Bitmap`, `ReadPixels`, `CopyTexSubImage`) : chacune force aujourd'hui une relecture complète. | à faire |

## Axe 3 — Compléter le pipeline fixe jusqu'à 1.5 (condition pour annoncer 1.5)

| # | Tâche | Statut |
|---|---|---|
| 3.1 | **Stencil** (protocole v6) : tampon hôte combiné profondeur+stencil, 9 clés d'état, transferts, backend de référence, offsets GLEngine (`docs/re/stencil.md`), plugin. Scène `stencil` identique au rendu d'Apple à l'octet près. | ✅ fait |
| 3.2 | Modes de polygone (ligne, point), pointillés de ligne et de polygone, lissage. | ✅ **fait le 18/09/2026** sauf le **lissage** (hors périmètre v8). Offsets relevés par la sonde `v8probe` (`docs/re/etat-v8.md`) : motif de ligne `GS+0x2e26`/`0x2e28`, motif de polygone **128 octets en `GS+0x30e8`**, dans l'ordre de `glPolygonStipple`. Modes de polygone **réservés au chemin brut** (le chemin hérité reçoit des triangles déjà décomposés : le contour est perdu avant l'hôte) et **la fusion des dessins est coupée** quand le mode n'est pas `GL_FILL`. Le motif de polygone demande un **décalage d'une ligne** (le protocole indexe par `hauteur − ys`, OpenGL par `hauteur − 1 − ys`). Scènes `polymode` et `stipple` : **0/255 sur l'image entière** par les deux chemins. |
| 3.3 | Opérations logiques ; mélange à couleur constante, équations minimum et maximum. | ✅ **fait le 18/09/2026**. Couleur de mélange en `GS+0x2d70`, opération logique en `GS+0x2e30` ; les facteurs `0x8001`-`0x8004` et les équations `GL_MIN`/`GL_MAX` passent par les clés existantes. Clés v8 envoyées pour **les deux chemins**. Scènes `blendc` et `logicop` : témoins exacts au bit près, **0/255**. Le motif de refus « stencil/logicop/stipple » ne couvre plus que le lissage de polygone. |
| 3.4 | Textures 3D, cube, rectangle ; bordures ; compressées (S3TC passé tel quel à l'hôte). | **hôte fait (v10, 19/09/2026)** : 3D, cube, rectangle, 1D, `CLAMP_TO_BORDER` et couleur de bordure, `MIRRORED_REPEAT`, S3TC (DXT1/3/5, décompressé par le cœur pour que les backends voient les mêmes texels). Les **texels de bordure** (`border = 1` de `glTexImage`) ne sont pas portés : l'invité doit les refuser. Reste le plugin : `cfg+0xbe`, `cfg+0xc2`, relevé des niveaux 3D et des faces de cube dans GLEngine (`docs/protocole-v10-textures.md` §7) |
| 3.5 | Lignes et points texturés ; sprites de points ; couleur secondaire. | **hôte fait pour la couleur secondaire et les paramètres de point (v10, 19/09/2026)** : `QGPU_SK_COLOR_SUM` (trois valeurs, la règle v7–v9 par défaut), atténuation, bornes et seuil de fondu, calculés par le cœur. Restent : lignes et points texturés, sprites de points, et tout le plugin (octet de `GL_COLOR_SUM` à relever ; attention au `POINT_SIZE_MAX` initial de GLEngine, `docs/protocole-v10-textures.md` §7) |
| 3.6 | Textures de profondeur et comparaison d'ombre ; génération automatique de mipmaps (`GenerateTexMipmaps` repérée). | **hôte fait (v10, 19/09/2026)** : `GL_DEPTH_COMPONENT` en `FLOAT`, `UNSIGNED_INT`, `UNSIGNED_SHORT`, comparaison texel par texel avant filtrage, `DEPTH_TEXTURE_MODE` ; niveaux de base et max, bornes et biais de LOD ; mipmaps générés par le cœur à chaque image du niveau de base. Reste le plugin |
| 3.7 | **Requêtes d'occlusion** (`CreateQuery`, `GetQueryInfo`). (OpenGL 1.5.) | ✅ **fait le 18/09/2026**. Interface relevée par lecture et vérifiée dans l'invité (`docs/re/etat-v8.md` §4) : `gldCreateQuery` (n° 45), `gldDestroyQuery` (46), `gldGetQueryInfo` (47) et **les procédures `+0x68` / `+0x6c`** = `glBeginQuery` / `glEndQuery`. Le `GLDriver` d'Apple ne tient **rien** (bouchons, et rien d'installé en `+0x68`/`+0x6c`) : sous lui, `glGetQueryObjectuiv` n'écrit même pas dans la variable de sortie. Le plugin tient la fonction entièrement ; un repli logiciel pendant une requête **majore** le compte (seule direction sans danger). Scène `occl` : 4 096 / 2 048 / 0 échantillons exacts. |
| 3.8 | Multiéchantillonnage. | à faire |
| 3.9 | Brouillard par fragment (`GL_NICEST`) ; niveau de détail des mipmaps par fragment dans le backend de référence. | à faire |

## Axe 4 — Annoncer 1.5, et brancher le système

| # | Tâche | Statut |
|---|---|---|
| 4.1 | **Annoncer version et extensions** telles que tenues (dépend de 1.1) : `GL_VERSION`, liste d'extensions, limites (`GetInteger`). | ✅ **fait le 18/09/2026** — mais le verdict n'est pas celui qu'on espérait. Relevé fonction par fonction dans **`docs/re/version-extensions.md`** (scène `v15` : un sous-test par fonction, joué sous le rendu d'Apple seul **et** sous le plugin). **La plus haute version entièrement tenue est 1.1** : OpenGL 1.2 exige les **textures 3D**, que GLEngine refuse (`GL_MAX_3D_TEXTURE_SIZE = 0`) et que le protocole ne porte pas. Annoncé : `GL_VERSION = "1.1 POMPPC-1.0"`, plus **trois** extensions ajoutées au tableau de bits (`GL_ARB_occlusion_query`, `GL_ARB_vertex_buffer_object`, `GL_EXT_blend_func_separate`) → 42 au lieu de 39. **Les limites d'Apple sont laissées telles quelles** (8 unités, 4096) : au-delà du chemin accéléré, le repli tient, c'est vérifié. Au passage, l'expérience **V3** a montré que la table bit → extension de `docs/re/capacites-glengine.md` §3.2 est **décalée d'un cran à partir du bit 24** ; elle est corrigée. |
| 4.2 | **Accélérateur IOKit** : nœud `IOAccelerator` + `IOGLBundleName`, chargement comme un vrai pilote de carte. Préalable de Quartz Extreme. | à faire |
| 4.3 | Programmes ARB de sommets et de fragments (`CreatePipelineProgram`) — au-delà de 1.5 strict, mais condition de Core Image. | à faire |
| 4.4 | Quartz Extreme (surfaces de fenêtre sur l'hôte), puis Core Image, puis Quartz 2D Extreme. Objectif visible : « QE/CI géré » dans Informations Système. | à faire |

### Ce que le lot 3 a laissé derrière lui (côté invité)

- **Le pipeline fixe est complet, sauf le lissage.** Restent hors domaine : `GL_LINE_SMOOTH`,
  `GL_POINT_SMOOTH`, `GL_POLYGON_SMOOTH` (3.2, fin), le multiéchantillonnage (3.8) et les sprites
  de points (3.5).
- **Deux inexactitudes trouvées en chemin et corrigées**, toutes deux invisibles jusqu'ici :
  *(a)* un `glTexParameteri(GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT)` — une valeur que le cœur
  refuse — faisait **rejeter toute la soumission** et retombait le processus entier sur le rendu
  logiciel ; le plugin valide désormais filtres et modes de répétition avant de les envoyer
  (motif `param-texture`). *(b)* Avec une **atténuation de la taille des points par la distance**,
  le chemin hérité dessinait les points à la taille de base : GLEngine calcule une taille par
  sommet que la clé d'état unique du protocole ne peut pas rendre. Le plugin refuse maintenant
  (motif `taille-de-point-attenuee`).
- **Ce qui manque pour annoncer 1.2** (et donc pour espérer 1.5) est nommé précisément dans
  `docs/re/version-extensions.md` §7 : textures 3D d'abord, puis cartes de cube et compression,
  puis couleur secondaire, `GL_MIRRORED_REPEAT`, textures de profondeur et paramètres de point.
- **La couleur secondaire n'est pas tenue du tout**, même par le rendu d'Apple
  (`GL_COLOR_SUM` + `glSecondaryColor3f` n'ajoutent rien) : ce n'est donc pas seulement le chemin
  brut qui la perd, comme le lot 2 le croyait.
- **La compression de texture est le pire cas rencontré** : `glCompressedTexImage2D` ne rend
  aucune erreur et l'image est fausse, silencieusement, des deux côtés.
- **Le doorbell asynchrone rapporte moins que l'attente qu'il supprime ne le laissait croire.**
  `t_submit` tombe de 1,6 ms à 0,06 ms par image (÷26), mais la file du device est vue **vide**
  à chaque échantillon (`0,00 en vol`) : l'hôte n'a jamais de retard, il n'y a donc presque rien à
  *recouvrir*. Le 1,5 ms gagné était le coût de l'aller-retour MMIO et du BQL, pas du dessin. Le
  gain final est **+11 %** sur Marble Blast, et **zéro** sur `gltest`, qui relit son image à chaque
  échange et doit donc attendre de toute façon.
- **`QGPU_REG_ERRORS` ne peut pas servir de verdict par client.** Il est global au device et le
  balayage de fermeture du kext (148 identifiants détruits par client, la plupart inexistants) le
  fait monter de ~148 à chaque application GL qui s'en va : **8 877** relevés sur une VM saine. Le
  plugin s'en sert donc comme d'un *signal*, pas d'un verdict — il repasse en synchrone 120 images
  pour que l'erreur, si elle est la sienne, se renomme d'elle-même avec son statut et son `pc`. Un
  vrai verdict par client demanderait que le device publie le statut **par barrière**, ce que la v9
  ne fait pas.
- **Bogue latent trouvé en chemin** (il ne s'était jamais déclenché, et le double tampon le rendait
  probable) : quand `geom_begin` vide le flux *après* avoir envoyé l'état — parce que la place des
  sommets manque — le `DRAW_RAW` suivant partait en **première commande d'une soumission neuve**,
  sans `CTX_BIND`, ce que l'hôte refuse (`QGPU_ST_NO_CTX`). `close_raw` lie maintenant le contexte
  s'il ne l'est plus. Même famille : `arena_alloc` réserve désormais aussi la place de flux de la
  commande qui désignera l'arène, sinon `reserve()` pouvait vider le flux entre les deux et
  envoyer la commande dans une **autre moitié** que la mémoire qu'elle désigne.

### Premier passage sur l'hôte Linux + RTX 4060 Ti (18/09/2026)

Les lots 1 à 3 ont été développés et mesurés sur un hôte macOS (CGL, Apple Silicon). Le
premier passage sur l'hôte Linux (EGL, pilote NVIDIA) a trouvé deux bogues, **tous deux
invisibles sous CGL** et tous deux côté hôte :

- **Le device v9 ne rendait plus rien** : `qgpu_smoke.py` en backend GL, 12 échecs, statut
  `QGPU_ST_BACKEND` dès la première scène. `gl_init` laissait le contexte EGL courant sur le
  thread qui réalise le device, et EGL refuse de le rendre courant sur le thread de rendu tant
  qu'un autre thread le tient (`EGL_BAD_ACCESS`). CGL ne l'interdit pas. Corrigé : le contexte
  naît libre, et le **reset du cœur** et la **libération du backend** passent eux aussi par le
  thread de rendu (le reset fait sur le vCPU y laissait le contexte). `tests/qgpu_core_test.c`
  initialise désormais sur un thread et exécute sur un autre, comme le device : il reproduit le
  bogue sans QEMU (177 échecs avant la correction).
- **Profondeur relue différente selon la présence d'un stencil** : un cran de 24 bits d'écart
  entre surface combinée (téléversée par `glDrawPixels`) et surface à profondeur seule (par
  `glTexSubImage2D`) — le pilote n'arrondit pas pareil dans les deux chemins. Les deux passent
  maintenant par `glDrawPixels`.

Après correction, sur cet hôte : `qgpu_core_test` 0 échec (logiciel + GL), `qgpu_smoke.py` 0 échec
(GL et logiciel), `run-all.sh` 47 OK. **Non refait ici** : tout ce qui demande l'invité
(`gltest`, jeux) — la VM de développement et `devloop.py prepare` (qui passe par `hdiutil`)
n'existent que sur l'hôte macOS.

## Points ouverts

- **Zenerchi, fin de partie** : ralentissement quand les cristaux brillent, non diagnostiqué
  (il faut jouer une partie jusqu'au bout ; mis de côté au profit de Marble Blast). Le bilan
  `POMPPC_GL_STATS=<fichier>` donne les motifs de refus et les replis par procédure.
- Le tampon profondeur+stencil du rendu d'Apple n'est alloué qu'à son premier usage : si un
  repli logiciel survient alors que seul l'hôte a dessiné, il part d'un tampon vide (limite
  partagée avec la profondeur depuis le début).
- Plus de 4 clients GL accélérés (tranches du kext) — à lever avant 4.4.

## Lot 2 en cours (18/09/2026) — la géométrie sur l'hôte

| Partie | Qui | Où | État |
|---|---|---|---|
| Vérifier dans l'invité l'octet `+0x79` et observer ce que GLEngine envoie (`POMPPC_GL_TCL=1`, procédures traceuses) → `docs/re/verification-tcl.md` | agent Opus, **seul sur la VM** | `guest/gldriver`, `guest/gltest` | en cours |
| Protocole **v7** côté hôte : matrices, viewport, lumières, matériaux, texgen, plans de découpe, brouillard calculé par l'hôte, `DRAW_RAW` (10 modes, indexé), étage géométrique complet dans le backend de référence, tests → `docs/protocole-v7-geometrie.md` | agent Opus, copie de travail isolée | `patches/qgpu`, `tests` | en cours |
| Plugin : recevoir la géométrie brute, l'envoyer en v7, repli hors domaine ; mesurer sur Marble Blast | agent Opus, seul sur la VM | `guest/gldriver`, `guest/gltest` | ✅ **fait** (18/09/2026) |
| Plugin : **fusion des dessins consécutifs** en `GL_TRIANGLES` indexés ; scène `fusion` de `guest/gltest` (ombrage plat et sommet provoquant, alternance des rubans sous `GL_CULL_FACE`, ordre sous mélange, coupures par changement d'état) ; mesure sur Marble Blast | agent Opus, seul sur la VM | `guest/gldriver`, `guest/gltest` | ✅ **fait** (18/09/2026) |
| Annoncer version et extensions tenues (4.1) | après le plugin | `guest/gldriver` | à faire |

### Ce que le lot 2 a laissé derrière lui

- ✅ **Le goulot des ~1 065 `DRAW_RAW` par image est levé (18/09/2026).** Rubans, éventails,
  quads, bandes de quads et polygones **consécutifs** sont convertis en `GL_TRIANGLES` indexés
  côté invité : un seul dessin par lot d'état, 65 à 85 par image au lieu de 1 065. Le coût hôte
  d'un `DRAW_RAW` a été **chiffré à 2,0 µs** (le temps de soumission est affine en nombre de
  dessins) : avant la fusion il pesait 2,1 ms sur une image de 26 ms, après il pèse moins de
  0,2 ms. **Il n'y a donc plus de gain à prendre dans `qgpu-gl.c`** en mémorisant l'état de
  `gl_target` entre deux dessins.
- **Le nouveau goulot** (profil `sample`, 18/09/2026) : `__memcpy` **15,8 %** — les sommets que
  GLEngine écrit dans la fenêtre partagée — et `mach_msg_trap` **13,9 %** — l'attente du device et
  du WindowServer. Le coût par appel du plugin est retombé de 17 % à 8 %. Les deux tâches qui
  attaquent ce qui reste sont **2.1** (objets tampon : les maillages statiques ne retraversent
  plus la fenêtre) et **2.2** (doorbell asynchrone : l'invité n'attend plus l'hôte).
  **2.2 est faite** (18/09/2026) : le temps de soumission tombe de 1,6 ms à 0,06 ms par image et
  Marble Blast gagne 11 %. Ce qui reste de `mach_msg_trap` après elle est l'attente du
  **WindowServer**, pas celle du device — c'est 2.3 (le device écrit lui-même dans la VRAM) qui
  l'attaquera ; `__memcpy` (les sommets que GLEngine écrit dans la fenêtre partagée) reste pour
  2.1.
- **Bogue hôte contourné** : `QGPU_OP_STENCIL_UPLOAD` sur une surface combinée
  profondeur+stencil **abîme la profondeur déjà posée** (reproduction : `gltest mixte` avec
  `GLTEST_STENCIL=1` ; sauter ce seul téléversement rend l'image exacte, avec comme sans le chemin
  brut). Contournement dans le plugin : le stencil n'est échangé avec l'hôte que si le contexte
  s'en sert vraiment (test de stencil activé, ou effacement du stencil demandé) — ce qui est en
  plus un gain, beaucoup d'applications demandant un stencil sans jamais s'en servir.
- **Couleur secondaire par sommet** non portée par le chemin brut : la mettre dans le format de
  sommet allumerait `GL_COLOR_SUM` sur l'hôte, et l'état GL relevé ne dit pas si l'application l'a
  demandé. Reste à trouver l'octet de `GL_COLOR_SUM` dans le bloc d'état de GLEngine.
- **Mode de rendu `GL_FEEDBACK`/`GL_SELECT`** : non relevé dans le bloc d'état, donc non testé
  explicitement dans le domaine. GLEngine emploie alors un autre étage de sommets
  (`gctx+0x4e1c ≠ 0x1c00`), que le prédicat rejette — mais cela n'a pas été vérifié par
  l'expérience.

## Lot 3 en cours (18/09/2026) — annoncer 1.5, et ne plus attendre le GPU

| Partie | Qui | Où | État |
|---|---|---|---|
| Plugin : brancher les fonctions v8 (mélange constant, min/max, opérations logiques, modes de polygone, pointillés, requêtes d'occlusion via `gldCreateQuery`) — relevé des offsets par sondes, scènes comparées à Apple | agent Opus, **seul sur la VM** | `guest/` | ✅ **fait** (18/09/2026) |
| Annoncer `GL_VERSION` et la liste d'extensions **tenues** (4.1), par la chaîne de `gldGetString` et le tableau de bits du bloc de configuration | même agent, ensuite | `guest/gldriver` | ✅ **fait** (18/09/2026) — **1.1**, pas 1.5 : voir 4.1 |
| Device asynchrone (2.2) côté hôte : soumissions exécutées par un thread de rendu, file d'attente, `FENCE`/IRQ `DONE`, sémantique documentée | agent Opus, copie isolée | `patches/qgpu`, `tests` | ✅ **fait** (18/09/2026) |
| Kext et plugin asynchrones (2.2) : `QGPU_UC_SUBMIT` avec drapeau, attente de barrière sur la command gate, double tampon de la tranche, relectures différées, présentation de l'image *n−1* ; `guest/qgpu-test` étendu ; mesures sur Marble Blast | agent Opus, **seul sur la VM** | `kext/POMPPCGPU`, `guest/` | ✅ **fait** (18/09/2026) |

## Ordre d'attaque

**Au 19/09/2026**, ce qui débloque le plus est côté invité, sur la moitié hôte que la v10 vient de
poser (`docs/protocole-v10-textures.md` §7) :

1. **Formats convertis par l'hôte** dans le plugin (2.5) : pure vitesse, aucun relevé nouveau.
2. **Textures 3D** dans le plugin (`cfg+0xbe`, niveaux 3D de `gldCreateTextureLevel`) : avec les
   vérifications [H] de `docs/re/version-extensions.md`, c'est l'annonce de **1.2**.
3. **Compression** : fermer d'abord le cas « silencieusement faux » (`version-extensions.md` §2),
   puis l'annoncer avec les cartes de cube.
4. Côté hôte seul : **2.1** (objets tampon) et **2.3** (zero-copy) demandent aussi le kext et le
   plugin ; la couleur secondaire et les paramètres de point (3.5) ont chacun une moitié hôte
   possible sans VM.

Historique :

1. ✅ **Vérifié dans l'invité** le 18/09/2026 (`docs/re/verification-tcl.md`). Prochaine étape de
   l'axe : relever les **codes d'entrée du descripteur `cfg+0x11c`** (§6.4 de ce relevé), pour
   obtenir couleur, normale et coordonnées de texture à côté de la position — puis 1.3.
2. **4.1** est débloquée : `GL_VERSION` sort tel quel de `gldGetString`, les extensions d'un
   tableau de 79 bits du bloc de configuration. À n'annoncer que ce qui est tenu.
3. Puis 1.3 → 1.4 (géométrie), 2.1 (tampons), 2.2 (asynchrone), 3.x par lots.
