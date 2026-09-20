# TODO — Tiger à fond : bureau, et Unreal Tournament 2004

**Objectif** : que Mac OS X Tiger sous QEMU tienne le bureau (Quartz Extreme) et les jeux
OpenGL, avec **Unreal Tournament 2004 Mac PPC** comme critère final. OpenGL 1.5 annoncé et
tenu n'est plus le but : c'est le plancher. Chaque tâche est jugée à cette aune : *est-ce
qu'UT2004 ou le WindowServer quitte le PowerPC émulé ?* Marble Blast et Zenerchi restent les
témoins de non-régression.

Ce fichier est le tableau de bord ; il est tenu à jour à chaque lot. Le contexte long est dans
`docs/roadmap-opengl15.md`, la conception et les offsets relevés dans `docs/gpu-3d-tiger.md`,
les relevés de rétro-ingénierie nouveaux dans `docs/re/`. Les amorces de recherche déjà
écrites, à ne pas refaire, sont en *Recherches amorcées*.

## État (20/09/2026)

- **Phase A** : `SURF_PRESENT` + `COPY_TEX` (v13), pixels 2.6, tampons hôte
  v14 (`BUF_*` + `DRAW_RAW_BUF`). Plugin `20260920-vbo`. Device v14 en service.

- **Plan pixels hors G4** : d'abord hisser dans le miroir GL (`SURF_PRESENT`
  v13, puis 2.6 / 2.1), et seulement ensuite casser le protocole. Détail sous
  *Plan — hisse hors du G4*.

- **Canal tableaux GeForce3 (plugin)** : `RenderVertexArray` / `RenderVertexBuffer`
  lisent `GS_VAO` et émettent `DRAW_RAW` **indexé** — GLEngine ne déroule plus
  `glDrawElements` dans Begin/End. Interrupteur `POMPPC_GL_ARRAY` (défaut : auto
  si `GL_VERTEX_ARRAY` est actif). À vérifier dans l'invité (`gltest varray` /
  `varrayvbo`). Voir `docs/gpu-3d-tiger.md` §4.7.

- **UT2004 Demo, premier passage réel** (`docs/re/ut2004-demo.md`) : en
  fenêtre l'image est juste (textures, polices, HUD, DM-Rankin). **Inutilisable** :
  trop lent, même avec le T&L hôte. Portes levées : pixel format (drapeaux
  plein écran / fenêtre / accéléré), textures UI à un niveau (filtre mipmap
  rabattu), `BAD_ARG` non fatal, NaN de `DRAW_TRIANGLES_TEX` (0x31) et de
  `DRAW_RAW` (primitive jetée, plus de triangle à l'origine). Reste :
  **FASTFP éteint** sur le QEMU quotidien (levier déjà prouvé), NaN à traiter
  **côté hôte**, plein écran (`CGLSetFullScreen` → `invalid drawable`),
  `malloc` double-free à la sortie. Contrôle de la VM quotidienne (AZERTY,
  CD, QMP) dans le même relevé.

## État (19/09/2026)

- **Accélérateur IOKit publié (tâche 4.2, 19/09/2026)** : le kext publie un nub
  `POMPPCAccelerator` (classe `IOAccelerator`, `IOGLBundleName = GLDriver-POMPPC`) et pose sur
  chaque framebuffer `IOAccelTypes`/`IOAccelIndex`, comme le kext d'une vraie carte. GLEngine
  charge alors le plugin **depuis `/System/Library/Extensions`**, avant le GLDriver d'Apple, sans
  astuce de nom ni fichier ajouté dans OpenGL.framework ; sans le device, pas de plugin du tout.
  Vérifié dans l'invité, kext installé et chargé avant le WindowServer : bureau normal, **Quartz
  Extreme inactif** (pas d'`AccelCaps`, pas d'`IOAGPDevice` sous QEMU), renderer `0x00027700` en
  tête de `CGLQueryRendererInfo`, `gltest` 40/40, `glwin` 68-69 img/s. Relevé :
  `docs/re/accelerateur-iokit.md`. Deux défauts latents du plugin corrigés en chemin : un plugin
  déchargé par GLEngine gardait sa tranche du kext et laissait un `atexit` dans du code déchargé.

- **OpenGL 1.5 ANNONCÉ et tenu (lot 5, 19/09/2026) — l'objectif de cette feuille de route** :
  `GL_VERSION = « 1.5 POMPPC-1.0 »`, **55 extensions**, device v12. Ce que 1.5 ajoute à 1.4 était
  déjà là ou presque : objets tampon (GLEngine ; `glMapBuffer`/`glGetBufferSubData` vérifiés),
  requêtes d'occlusion (v8), et les huit fonctions d'ombre (hôte v10, `EXT_shadow_funcs`
  annoncée). Scène `gl15` 18/18 par les deux chemins ; `gltest` 40/40.

- **OpenGL 1.4 ANNONCÉ et tenu (lot 5, 19/09/2026)** : `GL_VERSION = « 1.4 POMPPC-1.0 »`,
  **54 extensions**, avec un device **v12** (crossbar, `docs/protocole-v12-crossbar.md`). Fait dans
  le plugin, vérifié au pixel par les deux chemins (`tex14` 33/33, `docs/re/opengl-1.4.md`) :
  biais de LOD de texture et d'unité (`GL_MAX_TEXTURE_LOD_BIAS` = 16), textures de profondeur et
  comparaison d'ombre, couleur secondaire (code 4 du descripteur au chemin brut), paramètres de
  point (l'hôte au chemin brut, le plugin au chemin hérité), crossbar ; stencil à enveloppement
  annoncé après vérification. Au passage, **la perte de géométrie du chemin brut sous 3D et cube
  est élucidée** : une coordonnée r ou q donnée entre `glBegin` et `glEnd` faisait basculer
  GLEngine vers un autre renderer tant que `cfg+0x7a` valait 0 ; posé à 1, la 3D et les cubes
  passent par le chemin brut. `gltest` 39/39, hôte `run-all` 58 OK, mode bureau juste.

- **OpenGL 1.3 ANNONCÉ et tenu (lot 5, 19/09/2026)** : `GL_VERSION = « 1.3 POMPPC-1.0 »`,
  **48 extensions**, avec un device v11 — sans rien changer à l'hôte (tout était en v10). Fait
  dans le plugin et vérifié dans l'invité : **cartes de cube** (`cube` 12/12 par les deux
  chemins, `docs/re/cartes-de-cube.md`), **`CLAMP_TO_BORDER` et couleur de bordure**,
  **`MIRRORED_REPEAT`** (1.4), **niveaux S3TC relayés tels quels** (`tex13` 34/34 par les deux
  chemins, `docs/re/bordure-et-compression.md`). `gltest` 37/37, `v15` : tous les cas 1.2 et 1.3
  TENUS. Réserves : GLEngine ne sert pas les requêtes `GL_TEXTURE_COMPRESSED`/`_IMAGE_SIZE`
  (hors de portée du pilote) ; `GL_EXT_texture_compression_s3tc` non annoncée, parce que le
  rendu d'Apple **plante** sur une texture S3TC à mipmaps et qu'un repli reste possible.

- **OpenGL 1.2 ANNONCÉ et tenu (lot 5, 19/09/2026)** : `GL_VERSION = « 1.2 POMPPC-1.0 »`,
  **44 extensions**, avec un device v11. Ce qui manquait, fait dans le plugin et vérifié dans
  l'invité : textures 3D (`tex3d` 9/9 — jamais sous Apple), niveaux et bornes de LOD (`texlod`
  8/8 — Apple 3/8), spéculaire séparée par les deux chemins (`sepspec` — Apple : non), cette
  dernière grâce au protocole **v11** (`docs/protocole-v11-couleur-secondaire.md`). `gltest`
  35/35. Relevés : `docs/re/textures-3d.md`. Limite assumée : une texture 3D **hors** du domaine
  accéléré sort fausse (le rendu d'Apple n'a pas de 3D, il n'y a pas de repli).

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
| 1.1 | ✅ *relevé fait (`docs/re/capacites-glengine.md`) et **vérifié dans l'invité le 18/09/2026** (`docs/re/verification-tcl.md`, V1 et V6) : le bloc de configuration est confirmé à l'octet près, mais le verrou réel est le **bit 0 du retour de `gldInitDispatch`/`gldUpdateDispatch`**, relu à chaque changement d'état (donc repli possible par lot d'état), et il faut en plus publier un descripteur de sortie de sommet en `cfg+0x11c` sans quoi GLEngine jette la géométrie. Avec les deux, les sommets arrivent en `BeginPrimitiveBuffer`/`EndPrimitiveBuffer` en **coordonnées d'objet**. V3, V4, V5 faites ; restent V2 et V7.* **Codes du descripteur relevés le 18/09/2026** (`docs/re/descripteur-de-sommet.md`) : une entrée vaut `(code << 10) | ((composantes − 1) << 8) | décalageEnMots`, les codes sont les indices d'attribut d'entrée (0 position, 1 normale, 2 couleur, 3 brouillard, 4 couleur secondaire, 5 poids, 8..15 coordonnées de texture, 16..31 attributs génériques, 32..39 matériau ; **le code 6, drapeau d'arête, plante GLEngine**). Confirmé dans l'invité : on reçoit les attributs **bruts** — ni transformation, ni éclairage, ni texgen, ni matrice de texture, ni découpage, ni élimination de face — et **aucun code ne donne de sortie transformée ou éclairée** ; `glDrawArrays`, `glDrawElements` et les listes d'affichage passent tous par `Begin`/`EndPrimitiveBuffer`, les modes de primitive (rubans, éventails, quads) sont transmis tels quels. Aucun pilote de 10.4.6 ne publie `cfg+0x11c` : le GeForce3 et le Radeon annoncent la T&L matérielle mais passent par `RenderVertexArray`/`RenderVertexBuffer`, le Rage 128 reçoit des sommets **déjà transformés** dans le sommet interne de GLEngine. **Décision : option A (attributs bruts, DRAW_RAW v7)** ; borne supérieure du gain mesurée sur `gltest spin` à rastérisation négligeable : 415 → 788 img/s (×1,9).* Le verrou est **l'octet `+0x79` du bloc de configuration passé à `gldCreateContext`** : à 1, GLEngine envoie la géométrie brute (`BeginPrimitiveBuffer` +0x50, `RenderVertexBuffer` +0x4c, `RenderVertexArray` +0x70) ; il est figé à la création du contexte. **Relever la négociation des capacités** : ce qui décide GLEngine à appeler les entrées « hautes » du pilote (`CreateVertexArray`, `RenderVertexArray` +0x70, `AllocVertexBuffer`, `CreatePipelineProgram`) plutôt que de transformer lui-même. Lire comment les pilotes ATI/NVIDIA de 10.4.6 répondent à `GetRendererInfo`, `GetInteger`, `GetString`. **Verrou de tout l'axe.** | relevé ✅, **vérifié** ✅ |
| 1.2 | ✅ **Fait le 18/09/2026** : relevé par lecture (`docs/re/tableaux-de-sommets.md`) **puis établi par l'expérience dans l'invité** (`docs/re/etat-tcl.md`, sondes `lightprobe`, `matprobe`, `mtxprobe`, `tgprobe`, `xformprobe` de `guest/gltest`, diff des vidages par `tools/re/diffstate.py`). Sont confirmés offset par offset : les 24 matrices (`GS+0x1860 + m·0x40`, index de mode `16+u` pour la texture de l'unité `u`), le viewport et `glDepthRange` avec la formule exacte échelle/biais, l'élimination des faces, la normalisation, l'ombrage, les 8 lumières (`GS+0x24c0 + i·0x80`, position et direction de spot **en coordonnées œil**, seuil de spot stocké en **cosinus**), le modèle d'éclairage, les **deux matériaux qui sont DANS le bloc** (`GS+0x28c0`/`GS+0x2b00`, et non hors de lui), `GL_COLOR_MATERIAL`, le TexGen (pas `0x94` par unité, `0x24` par coordonnée, plan œil transformé), les 6 plans de découpe (coordonnées œil), le brouillard complet (densité/début/fin/source de coordonnée) et les paramètres de point. Les **valeurs courantes** (couleur, normale, couleur secondaire, coordonnée de brouillard, coordonnées de texture) sont **avant** le bloc, en `GS − 0x360 + x`. Le bloc `#define GS_…` prêt à recopier est en §10 de `docs/re/etat-tcl.md`. | ✅ **fait** |
| 1.3 | ✅ **Fait** : protocole **v7** — `SET_MATRIX`, `VIEWPORT`, `DEPTH_RANGE`, `SET_LIGHT`, `SET_MATERIAL`, `SET_LIGHT_MODEL`, `SET_TEXGEN`, `SET_CLIP_PLANE`, `SET_CURRENT`, `DRAW_RAW` (10 modes, indexé), 17 clés d'état de géométrie ; étage géométrique complet dans le backend de référence et dans le backend OpenGL ; `run_v7` de `tests/qgpu_core_test.c` sur les deux backends. `docs/protocole-v7-geometrie.md`. | ✅ **fait** |
| 1.4 | ✅ **Fait le 18/09/2026** : le plugin pose le verrou par le **bit 0 du retour de `gldInitDispatch`/`gldUpdateDispatch`**, publie son **descripteur de sortie de sommet** en `cfg+0x11c`, et reçoit les attributs bruts dans `Begin`/`EndPrimitiveBuffer` — `BeginPrimitiveBuffer` rend un pointeur **dans la fenêtre partagée**, à la disposition exacte de `DRAW_RAW` : **zéro recopie de sommet**. L'état T&L (matrices, viewport, 8 lumières, 2 matériaux, texgen, 6 plans de découpe, valeurs courantes, clés v7) ne part que quand il change. Hors domaine, le retour d'Apple est rendu tel quel et GLEngine reprend tout : le repli est **par lot d'état**, vérifié exact. Interrupteur `POMPPC_GL_GEOM` (défaut : **activé**). **Mesures** : `gltest spin` 406 → **900 img/s** (16×16), 420 → **822** (256×256), 323 → **481** (640×480) ; `gltest game` 684 → **854** ; **Marble Blast Gold, scènes lourdes 25-30 → 43-48 img/s (×1,6 à ×1,96)**, moyenne des fenêtres de jeu 42,4 → **62,6 img/s (+48 %)**. Voir `docs/gpu-3d-tiger.md` §4.7. | ✅ **fait** |

## Axe 2 — Ne plus recopier, ne plus attendre

| # | Tâche | Statut |
|---|---|---|
| 2.1 | **Objets tampon** (`CreateBuffer`, `FlushBuffer`, `BufferSubData`) sur tampons hôte : les maillages statiques ne retraversent plus la fenêtre partagée. (OpenGL 1.5.) | **v14 20/09/2026** : `BUF_CREATE` / `DESTROY` / `SUBDATA` + `DRAW_RAW_BUF`. `FlushBuffer` salit ; le premier `DrawArrays`/`DrawElements` emballe au format `DRAW_RAW` et téléverse ; les suivants du même intervalle sautent BAR0 (`QGPU_BUF_SHMEM` pour les indices). `POMPPC_GL_VBO=0` reprend `DRAW_RAW`. Mode immédiat inchangé. Preuve native `run_v14`. |
| 2.2 | ✅ **Fait le 18/09/2026**, de bout en bout. **Hôte** : protocole **v9** — file de 16 soumissions, thread de rendu, `QGPU_DOORBELL_ASYNC`, `FENCE_SUBMITTED`, `SUBMIT_ST`, `QUEUE_FREE`, `ERRORS`, `QUEUE_DEPTH`, IRQ `DONE` posée par un *bottom half* (`docs/protocole-v9-asynchrone.md`). **Kext** : le drapeau voyage dans les bits hauts de `len` de `QGPU_UC_SUBMIT` — l'ABI de Darwin 8 compare le *nombre* d'arguments scalaires au bit près, donc ajouter un scalaire aurait cassé tous les appelants existants ; `QGPU_UC_WAIT_FENCE` ne scrute plus, il dort sur la command gate et `irqAction` le réveille, avec un `IOTimerEventSource` comme base de temps du délai maximal (Tiger n'a pas `commandSleep(event, deadline, …)`, arrivé en 10.5) ; `destroyClientObjects` draine la file avant de rendre la tranche. **Plugin** : `POMPPC_GL_ASYNC` (défaut **activé**), tranche coupée en **deux moitiés** alternées à chaque soumission, relectures **différées** jusqu'au moment où l'invité en a besoin, présentation directe de l'image *n−1* au début de l'échange suivant (**une image de latence, jamais plus**). **Mesures Marble Blast** : temps de soumission **1,6 → 0,06 ms/image** (÷26), attente restante **0,1 ms/image**, **79,4 → 88,1 img/s (+11 %)** sur les fenêtres de jeu. Sur `gltest` (hors écran, relecture à chaque image) : **aucun gain**, c'est attendu. | ✅ **fait** |
| 2.3 | **Zero-copy à la présentation** : le device écrit lui-même dans la VRAM QFB (`SURF_PRESENT`, v13) ; plus de relecture ni de recopie par l'invité. Conversion 1555 sur l'hôte. `docs/protocole-v13-present.md`. | **hôte + plugin 20/09/2026** — preuve native `run_v13`. Device v14 en service. |
| 2.4 | **Présentation en fenêtre sans attendre le WindowServer** : écriture directe dans le rectangle de la surface à l'écran, tant que rien ne la recouvre et que le curseur n'y bouge pas ; un échange normal toutes les 90 images rafraîchit la fenêtre. Marble Blast : +70 %. | ✅ fait |
| 2.5 | Téléversement de textures sans conversion invité quand le format est connu de l'hôte (BGRA, 565, 1555…) : la conversion passe sur l'hôte. | ✅ **fait le 19/09/2026** : hôte (v10, `TEX_IMAGE3`) et plugin. Avec un device v10, le plugin recopie le niveau tel quel (pas de ligne compris : les niveaux alignés ne sont plus refusés) et l'hôte convertit ; `POMPPC_GL_TEX3=0` rend l'ancien chemin. Image identique au pixel près sur les 8 scènes texturées de `gltest`. **Gain mesuré modeste** (scène `texup`, 256×256 renvoyée à chaque image) : RGBA 362 → 388 img/s, RGB 410 → 445 (+7 à +9 %) ; 565 et BGRA inchangés — le temps y est dans GLEngine, pas dans la conversion |
| 2.6 | Opérations de pixels sur l'hôte (`DrawPixels`, `CopyPixels`, `Bitmap`, `ReadPixels`, `CopyTexSubImage`) : chacune force aujourd'hui une relecture complète. | **20/09/2026** : `COPY_TEX` + rectangle couleur. **Bitmap**. **DEPTH/STENCIL** : `ReadPixels` FLOAT/octet, `DrawPixels` / `CopyPixels` via `DEPTH_*` / `STENCIL_*` (test `ALWAYS` ou coupé, masque d'écriture plein). Zoom / transfer maps / test LESS → Apple. `POMPPC_GL_PIXEL=0` reprend Apple. |

## Axe 3 — Compléter le pipeline fixe jusqu'à 1.5 (condition pour annoncer 1.5)

| # | Tâche | Statut |
|---|---|---|
| 3.1 | **Stencil** (protocole v6) : tampon hôte combiné profondeur+stencil, 9 clés d'état, transferts, backend de référence, offsets GLEngine (`docs/re/stencil.md`), plugin. Scène `stencil` identique au rendu d'Apple à l'octet près. | ✅ fait |
| 3.2 | Modes de polygone (ligne, point), pointillés de ligne et de polygone, lissage. | ✅ **fait le 18/09/2026** sauf le **lissage** (hors périmètre v8). Offsets relevés par la sonde `v8probe` (`docs/re/etat-v8.md`) : motif de ligne `GS+0x2e26`/`0x2e28`, motif de polygone **128 octets en `GS+0x30e8`**, dans l'ordre de `glPolygonStipple`. Modes de polygone **réservés au chemin brut** (le chemin hérité reçoit des triangles déjà décomposés : le contour est perdu avant l'hôte) et **la fusion des dessins est coupée** quand le mode n'est pas `GL_FILL`. Le motif de polygone demande un **décalage d'une ligne** (le protocole indexe par `hauteur − ys`, OpenGL par `hauteur − 1 − ys`). Scènes `polymode` et `stipple` : **0/255 sur l'image entière** par les deux chemins. **Lissage : offsets déjà dans le plugin** (`GS_LINE_SMOOTH 0x2e2d`, `GS_POINT_SMOOTH 0x30dc`, `GS_POLY_SMOOTH 0x3179`) — refus seulement, pas de relais. |
| 3.3 | Opérations logiques ; mélange à couleur constante, équations minimum et maximum. | ✅ **fait le 18/09/2026**. Couleur de mélange en `GS+0x2d70`, opération logique en `GS+0x2e30` ; les facteurs `0x8001`-`0x8004` et les équations `GL_MIN`/`GL_MAX` passent par les clés existantes. Clés v8 envoyées pour **les deux chemins**. Scènes `blendc` et `logicop` : témoins exacts au bit près, **0/255**. Le motif de refus « stencil/logicop/stipple » ne couvre plus que le lissage de polygone. |
| 3.4 | Textures 3D, cube, rectangle ; bordures ; compressées (S3TC passé tel quel à l'hôte). | **hôte fait (v10)** ; **plugin : textures 3D faites le 19/09/2026** (`docs/re/textures-3d.md`) — `GL_MAX_3D_TEXTURE_SIZE` = 256 annoncé si le device les tient, profondeur lue dans l'objet texture de GLEngine, `WRAP_R` relayé. Scène `tex3d` : 9 cas sur 9, et le cas « 1.2 texture 3D » de `v15` est **TENU** (jamais sous Apple). La perte de géométrie brute (3D/cube) est **élucidée** : coordonnée r/q entre `glBegin`/`glEnd` + `cfg+0x7a = 0` (`docs/re/opengl-1.4.md` §3) ; le plugin pose `cfg+0x7a = 1`. **Cubes, bordure et compression faits le 19/09/2026** (`docs/re/cartes-de-cube.md`, `docs/re/bordure-et-compression.md`) ; **reste le rectangle** (hôte déjà, emplacement 2, `cfg+0xe8`) |
| 3.5 | Lignes et points texturés ; sprites de points ; couleur secondaire. | **hôte fait (v10)** ; **plugin fait pour la couleur secondaire et les paramètres de point (lot 5)** : octet `GS+0x2e0b` (`docs/re/opengl-1.4.md`), `POINT_SIZE_MAX` initial 1 traité comme « pas de borne » (envoie 64). Restent : lignes et points texturés, sprites de points |
| 3.6 | Textures de profondeur et comparaison d'ombre ; génération automatique de mipmaps (`GenerateTexMipmaps` repérée). | **hôte fait (v10)** ; **plugin fait pour profondeur et ombre (lot 5)** : `DT+0x40/0x42/0x48` (`docs/re/opengl-1.4.md`). Mipmaps : le cœur les génère à chaque image du niveau de base ; `DT+0x4a = 0x8000` avec `GL_GENERATE_MIPMAP` **[H]** ; procédure `GenerateTexMipmaps` en `+0x7c` |
| 3.7 | **Requêtes d'occlusion** (`CreateQuery`, `GetQueryInfo`). (OpenGL 1.5.) | ✅ **fait le 18/09/2026**. Interface relevée par lecture et vérifiée dans l'invité (`docs/re/etat-v8.md` §4) : `gldCreateQuery` (n° 45), `gldDestroyQuery` (46), `gldGetQueryInfo` (47) et **les procédures `+0x68` / `+0x6c`** = `glBeginQuery` / `glEndQuery`. Le `GLDriver` d'Apple ne tient **rien** (bouchons, et rien d'installé en `+0x68`/`+0x6c`) : sous lui, `glGetQueryObjectuiv` n'écrit même pas dans la variable de sortie. Le plugin tient la fonction entièrement ; un repli logiciel pendant une requête **majore** le compte (seule direction sans danger). Scène `occl` : 4 096 / 2 048 / 0 échantillons exacts. |
| 3.8 | Multiéchantillonnage. | à faire |
| 3.9 | Brouillard par fragment (`GL_NICEST`) ; niveau de détail des mipmaps par fragment dans le backend de référence. | à faire |

## Axe 4 — Annoncer 1.5, et brancher le système

| # | Tâche | Statut |
|---|---|---|
| 4.1 | **Annoncer version et extensions** telles que tenues (dépend de 1.1) : `GL_VERSION`, liste d'extensions, limites (`GetInteger`). | ✅ **fait le 18/09/2026** — mais le verdict n'est pas celui qu'on espérait. Relevé fonction par fonction dans **`docs/re/version-extensions.md`** (scène `v15` : un sous-test par fonction, joué sous le rendu d'Apple seul **et** sous le plugin). **La plus haute version entièrement tenue est 1.1** : OpenGL 1.2 exige les **textures 3D**, que GLEngine refuse (`GL_MAX_3D_TEXTURE_SIZE = 0`) et que le protocole ne porte pas. Annoncé : `GL_VERSION = "1.1 POMPPC-1.0"`, plus **trois** extensions ajoutées au tableau de bits (`GL_ARB_occlusion_query`, `GL_ARB_vertex_buffer_object`, `GL_EXT_blend_func_separate`) → 42 au lieu de 39. **Les limites d'Apple sont laissées telles quelles** (8 unités, 4096) : au-delà du chemin accéléré, le repli tient, c'est vérifié. Au passage, l'expérience **V3** a montré que la table bit → extension de `docs/re/capacites-glengine.md` §3.2 est **décalée d'un cran à partir du bit 24** ; elle est corrigée. |
| 4.2 | **Accélérateur IOKit** : nœud `IOAccelerator` + `IOGLBundleName`, chargement comme un vrai pilote de carte. Préalable de Quartz Extreme. | ✅ **fait le 19/09/2026** (`docs/re/accelerateur-iokit.md`) : nub `POMPPCAccelerator` enfant de `POMPPCGPU` (le transport garde son type d'ouverture 0, qui est aussi celui des surfaces), qui refuse toute ouverture ; les framebuffers le désignent par `IOAccelTypes`/`IOAccelIndex`. Plugin dans `Extensions`. Pas d'`AccelCaps` : le WindowServer tenterait Quartz Extreme. Pour 4.4 : `AccelCaps`, client de surface, mémoire vidéo annoncée (0 aujourd'hui) |
| 4.3 | Programmes ARB de sommets et de fragments (`CreatePipelineProgram`) — au-delà de 1.5 strict, mais condition de Core Image. | à faire — **relevé par lecture déjà là** (`docs/re/tableaux-de-sommets.md` §4 ; sonde `ppprobe` écrite, jamais jouée) |
| 4.4 | Quartz Extreme (surfaces de fenêtre sur l'hôte), puis Core Image, puis Quartz 2D Extreme. Objectif visible : « QE/CI géré » dans Informations Système. | à faire — **porte et pièges relevés** (`docs/re/accelerateur-iokit.md` §6 et §9 ; voir *Recherches amorcées*) |

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

- **Après 1.5** (19/09/2026) : l'exactitude du pipeline fixe est atteinte. Le critère n'est
  plus « annoncer 1.5 » : c'est **UT2004 Mac PPC jouable**, et le bureau sous Quartz Extreme.
  Marble Blast et Zenerchi restent les témoins. Restent la vitesse (axes 1 et 2), le 16 bits,
  la VRAM annoncée, les programmes ARB, et les réserves ci-dessous.
- **`glMaterial` entre `glBegin` et `glEnd`, chemin brut** : la primitive est perdue et la
  suivante garde l'ancien matériau (scène `matbegin`) — GLEngine bascule vers un autre renderer
  quel que soit `cfg+0x7a` (`docs/re/opengl-1.4.md` §3.3). Défaut antérieur ; piste : matériau
  par sommet (codes 32..39 du descripteur) porté par `DRAW_RAW`.
- **S3TC en extension** : le relais est exact (`tex13`), mais le rendu d'Apple plante (Bus error)
  sur une texture à mipmaps chargée par `glCompressedTexImage2D`. Pour annoncer le bit 43, il
  faut rendre le repli sûr : décoder dans l'invité pour le rendu d'Apple, ou ne jamais replier
  un dessin qui l'emploie.
- **Requêtes de niveau compressé** (`GL_TEXTURE_COMPRESSED`, `_IMAGE_SIZE`) : GLEngine ne les sert
  pas. Seule voie concevable : surcharger l'entrée `glGetTexLevelParameteriv` de la table de
  dispatch (`gldInitDispatch`) — à relever.

- **`qgpu_smoke.py`, contrôle « dernier doorbell refusé (file pleine) »** : il dépend de la course
  entre la rafale envoyée par la console d'Open Firmware et le thread de rendu. Sur un hôte chargé
  (charge moyenne 25 à 44 pendant une compilation LTO, 19/09/2026), il a échoué 1 fois sur 6, et
  les relectures console ont rendu `None` ou décalé d'un mot 2 fois sur 6. Ce n'est pas une
  régression : relancer à vide avant de conclure.

- **Zenerchi, fin de partie** : ralentissement quand les cristaux brillent, non diagnostiqué
  (il faut jouer une partie jusqu'au bout ; mis de côté au profit de Marble Blast). Le bilan
  `POMPPC_GL_STATS=<fichier>` donne les motifs de refus et les replis par procédure.
- Le tampon profondeur+stencil du rendu d'Apple n'est alloué qu'à son premier usage : si un
  repli logiciel survient alors que seul l'hôte a dessiné, il part d'un tampon vide (limite
  partagée avec la profondeur depuis le début).
- Plus de 4 clients GL accélérés (tranches du kext) — à lever avant 4.4.
- **Mémoire vidéo annoncée : 0** (`kCGLRPVideoMemory`, héritée du GLDriver d'Apple). Le
  WindowServer exige au moins `GLCompositorMinimumVRAM` pour Quartz Extreme, et des jeux lisent
  cette valeur. À poser avec 4.4 (`docs/re/accelerateur-iokit.md` §9).

## Recherches amorcées (récupérées le 19/09/2026)

Relevés commencés, assez avancés pour ne pas les refaire. Rien d'implémenté au-delà de ce que
les tâches disent déjà.

### 2.1 Objets tampon — `docs/re/tableaux-de-sommets.md` §3

- Signatures **[L]** : `gldCreateBuffer(ctx, u32 *poignée, void **ptr, u32 *drapeaux)` ;
  `FlushBuffer(ctx, poignée, ptr, longueur)` ; `BufferSubData` → `0` = refus (le moteur retombe
  sur `memcpy` + `FlushBuffer`).
- Objet GLEngine, taille `0x48 + 4·n_rendus` : poignées en `+0x10+4i`, données `+0x30`, taille
  logique `+0x38`, drapeaux par rendu `+0x48+4i`. Le moteur pose `|= 3` après une écriture CPU ;
  le pilote **efface les bits 0-1 dans `FlushBuffer`** (GeForce3).
- Liaisons : `GL_ARRAY_BUFFER` → `A+0x348`, `ELEMENT` → `A+0x34c`, `PACK`/`UNPACK` →
  `gctx+0x4a68`/`0x4a6c`.
- Apple et Rage 128 : poignée 0. GeForce3 : `malloc(32)`, copie GPU paresseuse.
- Pour nous : poignée non nulle + `FlushBuffer` qui téléverse vers un tampon hôte.

### 4.3 Programmes ARB — `docs/re/tableaux-de-sommets.md` §4

- `CreatePipelineProgram(ctx, *poignée, descripteur)` — **3 arguments**. `Modify` : masque
  **1** = texte, **2** = paramètres locaux. `Relate` / `GetInfo` : **aucun appel** dans
  GLEngine 10.4.6.
- Le descripteur reçu est `ppobj+0x4c8` (premier `u16` = cible). GLEngine crée **deux objets
  par défaut** à l'init du contexte, même sans aucun programme ARB, cible **0** (pipeline fixe).
- Texte ASCII en `+0x14`, forme compilée en `+0x4e4`, locaux en `+0x4e0`. **[H]** : l'état fixe
  n'est *pas* recodé en programme — sonde `ppprobe` (§7.6) écrite, **jamais jouée**.
- Apple : jeton factice 4. Rage 128 : n'écrit même pas `*r4`. GeForce3 : compile au dessin.

### 4.4 Quartz Extreme — `docs/re/accelerateur-iokit.md` §6 et §9

- La porte : propriété **`AccelCaps`** sur l'accélérateur, **ou** un `IOAGPDevice` dont le
  modèle n'est pas Rage 128. QEMU `mac99` n'a **aucun** `IOAGPDevice` : sans `AccelCaps`, pas
  de QE (vérifié). Ensuite : VRAM ≥ `GLCompositorMinimumVRAM` (16), `_isAccelUsable` (1 par
  défaut), formats de pixels accélérés.
- Client de surface : `kIOAccelSurfaceClientType` **est le type 0**, déjà celui du transport
  `POMPPCGPU` — d'où le nub enfant qui refuse toute ouverture (`kIOReturnUnsupported`).
  `CGLSetPBuffer` appelle `IOAccelCreateSurface` et **s'en passe** si ça échoue.
- Les cartes réelles publient aussi `IOCFPlugInTypes` (`ACCF0000-…` → plugin GA 2D) et
  `IODVDBundleName` : pas commencé.
- Mémoire vidéo annoncée : **0** (`kCGLRPVideoMemory`).

### 3.4 Rectangle — hôte fait, plugin muet

- Cœur : `QGPU_TT_RECTANGLE` (coordonnées en texels, pas de mipmap, `CLAMP` seulement).
- GLEngine : `TU_ENABLE` bit **0x04**, emplacement **2** de la table liée (`textures-3d.md` §1).
- Annonce : `cfg+0xe8` = `GL_MAX_RECTANGLE_TEXTURE_SIZE` (`capacites-glengine.md` §4) ; le
  plugin le laisse à 0. Pas de sonde dédiée (il y a `t3dprobe` et `cubeprobe`).

### 2.6 `DrawPixels` — amorce

- `cfg+0x7b` n'est lu que par `_glDrawPixels_Exec` (0x63d8c) et `_glDrawPixels_ListExec`.
  À 1 : tentative `_gleDrawPixelsFast` puis la procédure du plugin.
- Signature de la procédure : `(ctx, sommet, w, h, format, type, pixels, 0)`.
  Le sommet (0x100 octets) porte déjà x/y/z **fenêtre** (`V_X`/`V_Y`/`V_Z`).
  Source : `gctx+0x2e0/0x2e4/0x2e8` × échelle viewport `gctx+0x4a44..`.
- `CopyPixels` : `(ctx, sommet, x, y, w, h, type)` ; `type` = `GL_COLOR` 0x1800.
- Fait dans le plugin `20260920-pix` (RGB/RGBA octet, COLOR). Restent Bitmap et DEPTH/STENCIL.

### 3.2 Lissage — offsets déjà posés

Dans `pomppc_accel.c`, pour le refus seulement : `GS_LINE_SMOOTH 0x2e2d`,
`GS_POINT_SMOOTH 0x30dc`, `GS_POLY_SMOOTH 0x3179` (`0x3179` déjà dans
`tableaux-de-sommets.md`). `etat-v8.md` §5 dit « non relevés » : faux.

### RenderVertexArray « à la GeForce3 »

✅ **Fait le 20/09/2026.** Quand `GL_VERTEX_ARRAY` est actif, le plugin retire `cfg+0x11c`
(`POMPPC_GL_ARRAY=1`, défaut) : GLEngine n'écrit plus dans Begin/End (où il déroulait
`glDrawElements`) et appelle `RenderVertexArray` `+0x70` (VAR) ou `AllocVertexBuffer` /
`RenderVertexBuffer` `+0x4c`. Le plugin lit `GS_VAO`, packe les attributs au format
`DRAW_RAW` et envoie les **indices tels quels**. Le mode immédiat garde le descripteur
tant qu'aucun tableau n'est actif. `cfg+0x78 = 1` et les six identifiants
`cfg+0x7c..0x87` `{3,2,1,6,5,4}` (comme le GeForce3). Le tampon packé de
`AllocVertexBuffer` (plafond 2048) est ignoré : on relit les tableaux clients, le
format matériel n'est pas le nôtre. `POMPPC_GL_ARRAY=0` rétablit Begin/End.
À vérifier dans l'invité : `gltest varray` / `varrayvbo` contre Apple, et
`POMPPC_GL_ARRAY=0` en témoin A/B.

### Restes d'état, bas priorité

- `etat-tcl.md` §12 : `GS+0x24a4` (`GL_RESCALE_NORMAL`, jamais exercé), usage de `GS+0x4d50`
  et `GS+0x450f`, lequel des deux viewports consommer (`GS+0x1810` lu par le GeForce3, vs
  `GS+0x46cc`), modes de matrice 1 et 5..15 (`MODELVIEW1..3_ARB`, `MATRIX0..7_ARB`).
- `cfg+0x78` : lu par `_gleUpdatePolyMode` et `_gleDrawArraysOrElements_Exec` ; Rage 128 le
  pose à 1 (pas de T&L). `cfg+0xc0` : taille max d'une cible non identifiée (le rectangle
  est `+0xe8`). 7ᵉ argument de `gldCreateContext` (`config+0x130`).
- Objet texture : `DT+0x1a = 0x85BD` (constant), `DT+0x2c` probablement anisotropie max **[H]**.
- Sondes 7.1 à 7.6 de `tableaux-de-sommets.md` : préalables 7.0 **faits** dans le plugin
  (vidage de `0x5400` octets, VAO, matériaux, pipeline program) ; les sondes elles-mêmes
  n'ont jamais tourné — plusieurs ont été remplacées par `v8probe` / `v14probe` /
  `t3dprobe` ; **`ppprobe` (7.6) et l'attribut générique (7.5 étape 26) restent utiles**.

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
- ✅ **Couleur secondaire** : octet trouvé le 19/09/2026 en `GS+0x2e0b` (copie `GS+0x4c8a`,
  sonde `v14probe`) ; le plugin le pose et porte le code 4 au chemin brut.
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

## Plan — hisse hors du G4, puis quitte le miroir GL

Le projet reste **intelligent** tant qu'il enlève du travail au PowerPC émulé,
dans le protocole GL actuel. Il devient **autre chose** le jour où plus aucun
pixel n'est fabriqué ni recopié par le G4, et où qgpu n'est plus un flux
OpenGL 1.x.

### Phase A — encore du GL, plus rien d'inutile sur le G4

Le miroir GL 1.x reste le contrat. On hisse, dans cet ordre :

1. **Présentation (tâche 2.3, v13)** — l'hôte écrit dans la VRAM QFB.
   Plus de `SURF_READBACK` + `memcpy` + pack 1555 par image. *Sources
   prêtes ; QEMU v13 à relancer.*
2. **Pixels restants (2.6)** — `CopyTexSubImage2D`, `ReadPixels`
   couleur/profondeur/stencil, `DrawPixels` / `CopyPixels` COLOR/DEPTH/STENCIL,
   `glBitmap`. Zoom et test de profondeur autre que `ALWAYS` restent Apple.
3. **Tampons (2.1)** — v14 `BUF_*` + `DRAW_RAW_BUF`. Les maillages VBO
   statiques ne retraversent plus BAR0 après le premier dessin.
4. **Repli Apple** — le GLDriver logiciel reste le trou : dès qu'on y
   retombe, le G4 rastérise. Réduire le domaine de repli, pas l'élargir.

Critère de fin de phase A : sur le chemin accéléré, **aucun texel n'est
fabriqué ni recopié par l'invité**. `POMPPC_GL_STATS` : `relect = 0` et
`present > 0` à chaque image, `copie ≈ 0 ms`.

### Phase B — qgpu n'est plus du GL

Quand A est tenu : casser le miroir. Le protocole devient un tampon de
commandes de GPU (et, plus tard, Metal). Ce n'est **pas** le lot en cours.
`docs/gpu-3d-tiger.md` reste la conception du miroir jusqu'à ce jour-là.

---

## Ordre d'attaque

**Au 20/09/2026, soir** — pixels hors du G4, première coupe :

1. **v13 `SURF_PRESENT`** (2.3) : hôte → VRAM QFB, pack 1555 sur l'hôte,
   plugin sans copie. Preuve native `run_v13`. Pour en profiter : reconstruire
   QEMU (device v13) puis réinstaller le plugin ; la quotidienne v12 continue
   de marcher (`QGPU_PROTO_MIN`).
2. Géométrie Colin McRae (clip `w≈0`, plugin `20260920-clip`) et texte WC3
   (hash de texels) : suivis, pas le goulot architectural.
3. Ensuite 2.6 (pixels, ✅) et 2.1 (VBO, v14). Repli Apple ensuite.

**Au 20/09/2026** (démo UT lancée, image juste, injouable) — `docs/re/ut2004-demo.md` §4 :
1. bilan `POMPPC_GL_STATS` en fenêtre (DRAW_RAW vivant ou rastérisation) ;
2. **FASTFP=1** sur la quotidienne (arrêt propre) ;
3. NaN dans `do_draw` / `do_draw_raw` côté hôte, retirer le scan G4 du plugin ;
4. `CGLSetFullScreen` / `gldAttachDrawable` ;
5. double-free à la sortie. Ne pas re-splitter `tiger.qcow2` tant que la VM tourne.

**Critère** (19/09/2026, soir) : dès que le DVD MacSoft PPC est dans l'invité, **premier
lancement d'UT2004** avec `POMPPC_GL_STATS` — le bilan dicte l'ordre, pas cette liste.
Attendu sans avoir joué : VRAM lue à 0, 16 bits hors domaine, 4 unités hôte contre
`maxtextureunits=8`, géométrie hors du chemin `AllocVertexBuffer` / VAR. Démo Mac
acceptable pour un premier passage. *(La démo a été lancée le 20/09 ; le bilan
stats n'a pas encore été pris.)*

**Au 19/09/2026, soir** (1.5 annoncé, accélérateur publié) :

1. **Vitesse sur l'hôte Linux** : l'utilisateur trouve Marble Blast et Zenerchi plus lents sur
   le PC (i7-10700F, RTX 4060 Ti) que sur l'hôte Apple Silicon, la musique saccade, et Marble
   Blast en 16 bits « ralentit à mort ». **Diagnostic du 19/09/2026 soir** (job
   `tools/guest/jobs/games`, bilan `POMPPC_GL_STATS`, `top` et `sample` dans l'invité) :
   - **16 bits** : hors domaine du plugin (`CTX_COLOR_BITS != 32` → `NO_BUFFER`, profondeur 16
     → `NO_DEPTH`) ; tout part au rendu logiciel d'Apple. Limite connue (§4.5 de
     `gpu-3d-tiger.md`). Tâche : tampons 16 bits (RGB555, profondeur 16) avec conversion par
     l'hôte à la relecture et au téléversement.
   - **Le rendu n'est pas en cause** : `gltest game` 686 img/s ici (684-854 sur le Mac), aucun
     refus, plugin < 5 ms par image. Dans l'invité, le WindowServer est à < 1 % et le jeu à 91 %.
   - **Zenerchi** : 100 % des échantillons dans `TLoopMusicManager::LoadMusics → ov_read →
     mdct_butterflies` — le jeu décode toutes ses musiques Ogg Vorbis au démarrage.
     **Marble Blast** : fil principal à 96 %, dont ~35 % d'appels au système de fichiers
     (chargements de niveau) et ~15 % de décodage Vorbis (musique en flux, d'où les saccades).
   - **Cause de fond : le flottant PowerPC est entièrement émulé.** `fpu/softfloat.c` de QEMU 9.2
     définit `QEMU_NO_HARDFLOAT 1` pour `TARGET_PPC` (le bit FI du FPSCR n'est pas collant), et
     chaque instruction flottante fait `reset_fpstatus` + l'opération + `float_check_status`.
     Même cause sur le Mac, dont le cœur est ~30 % plus rapide (rendu logiciel d'Apple dans
     `glwin` : 81 contre 57 img/s).
   - ✅ **Mode « flottant rapide » fait et prouvé (19/09/2026, `docs/flottant-rapide.md`)** :
     propriété de CPU `x-fast-fp` (`FASTFP=1 ./run_tiger.sh`, opt-in), patches
     `patches/fastfp/`. Le FPU de l'hôte ne sert que si FPSCR[XX] = 1, XE = OE = UE = 0 et
     arrondi au plus proche ; chemins rapides exacts pour la simple précision. **Résultats
     identiques au bit près, FPSCR identique hors FI/FR/FX** — prouvé dans l'invité par
     `guest/fpbench/fpcheck` (120 sections, 2,4 M d'opérations, mono-cœur et SMP=2, AltiVec
     compris) ; mode exact = QEMU non patché à la ligne près. **Mesures** (`fpbench`, job
     `fpgames`) : ×2,1 à ×2,9 sur les noyaux `float` (MDCT, FFT, fmadd, div), ×1,4 à ×2,5 en
     double, ×2,1 en AltiVec, témoin entier ×1,00 ; **Zenerchi : menu en 55 s au lieu de 68
     (−12 s)** ; **Marble Blast : +20 % d'images/s** (médiane de 72 fenêtres appariées, 2 cœurs),
     physique identique.
   - Restent : la variante de compilation (`-march=native`, LTO, sans `qom-cast-debug`) dont
     l'A/B de boot est à faire, hôte au repos (binaire dans `~/src/qemu/build-opt`) ; le coût
     restant d'une instruction flottante n'est PAS dans les appels de helper (le 0002 ne
     rapporte que 2-4 %) — pistes : FPRF paresseux, `lfs`/`stfs` en ligne ; et les tampons
     16 bits du plugin.
2. **Ce qu'UT2004 va exiger** (à confirmer par le bilan, pas à implémenter à l'aveugle) :
   **2.1** (VBO) et le chemin `AllocVertexBuffer` / `APPLE_vertex_array_range` déjà relevé ;
   tampons **16 bits** ; mémoire vidéo annoncée (le jeu lit `kCGLRPVideoMemory` et `VARSize`) ;
   plus de 4 unités de texture si le bilan le montre. **4.3** (programmes ARB) pour le joli
   chemin d'Unreal Engine 2, et pour Core Image.
3. **Vers Quartz Extreme (4.4)** : plus de 4 clients, mémoire vidéo, client de surface sur
   `POMPPCAccelerator`, puis `AccelCaps` (`docs/re/accelerateur-iokit.md` §9). Ça ne fait pas
   gagner une image en UT plein écran ; ça fait que Tiger *entre les matchs* ne rampe pas.
4. **2.3** (zero-copy), une fois le goulot d'UT2004 nommé.

Au 19/09/2026, matin (fait depuis : 2.5, textures 3D, compression — 1.2 à 1.5 annoncés) :

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
