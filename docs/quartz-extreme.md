# Quartz Extreme sous POMPPC

Chantier ouvert le 28/10/2026 sur le M4 (TODO « Divers », `docs/roadmap-opengl15.md` étape E).
But : le WindowServer de Tiger compose les fenêtres sur le GPU de l'hôte, à travers notre
plugin et qgpu, comme sur un Power Mac à carte accélérée. Informations Système doit alors
afficher « Quartz Extreme : géré ».

Préalables déjà relevés : `docs/re/accelerateur-iokit.md` §1, §6 et §9.

## 1. Ce que fait le WindowServer (CoreGraphics 10.4.11, relevé statique du 28/10)

Binaire : `ApplicationServices.framework/.../CoreGraphics.framework/Versions/A/CoreGraphics`
du disque de dev 10.4.11. Désassemblé par `tools/re/ppcanno.py` (symboles présents).

`_CGXGLDisplayContextInitialize` (0x903a8bc4) :

1. Pour chaque écran, `_CGXAcceleratorForDisplayDevice`, puis `_getAccelCapsBits`
   (0x903a9454). Cette fonction lit 4 octets de la propriété **`AccelCaps`** de
   l'accélérateur (`IORegistryEntryGetProperty`) et l'écrit au journal
   (« Accel caps: %08lx »). Le masque de chaque écran est gardé, et les masques sont réunis.
2. `_checkForValidDevice` : un `IOAGPDevice` autre que Rage128. Il n'y en a pas sous QEMU.
3. Si l'union des masques et ce test valent 0 tous les deux, pas de Quartz Extreme.
4. Sinon, `_cglsInitialize` (0x903a9630), sous les droits root. Elle charge **GLEngine**
   (`/System/Library/Frameworks/OpenGL.framework/Resources/GLEngine.bundle/GLEngine`) par
   `NSLinkModule`, avec la liste des services accélérateurs et les index de framebuffer, puis
   prend sa table `gli*` (`_cgls_gli_names`). GLEngine charge ensuite le GLDriver de
   l'accélérateur (`IOGLBundleName`) : **notre plugin est chargé dans le WindowServer** par le
   même chemin que dans une application.
5. `_cglsChoosePixelFormat` (attributs `_attribs.0`, 0x90724330). Puis, pour chaque écran,
   `_cglsQueryRendererInfo` et ses exigences :
   - mémoire vidéo `rinfo+0x30` ≥ `GLCompositorMinimumVRAM` (16 par défaut, clé de la
     configuration CoreGraphics) ;
   - `_isAccelUsable` (listes de `GLCompositorConfiguration`, autorisé par défaut), **ou**
     bit `0x1000` des drapeaux `rinfo+0x08`, **ou** `AccelCaps` non nul pour cet écran (le
     masque de l'écran court-circuite donc la liste) ;
   - un format de pixels du masque de l'écran : drapeaux `+0x08 & 0x111` (accéléré), couleur
     `+0x10` = `0x8000` ou `0x4000`, `+0x18 == 1`, `+0x1c == 1`, `+0x22 == 0` (à
     identifier sur le format réel de notre plugin) ;
   - `_glMinimumDisplayBitDepth` = 32 par défaut.
6. `_cglsCreateContext`, puis `_CGXGLDisplayContextCapabilities`. Cette dernière lit les
   préférences, les fournisseurs exclus (`_isVendorExcluded`), `_isDisplayContextCapable` et
   `_CGXGPUCapability` (extensions, dont `GL_ARB_fragment_program` pour Core Image et Quartz
   2D Extreme).

L'écran est ensuite attaché par `_cglsAttachSurface`, qui appelle l'`AttachDrawable` de
GLEngine avec le type `0x50` (surface), comme pour une fenêtre d'application. Les fenêtres
deviennent des textures (`_CGXGLCreateBackingTexture`, `_CGXGLCreateSurfaceTexture` pour les
surfaces GL), composées par `_CGXGLAccelCompositeLayer`.

Ce que notre plugin donne déjà : mémoire vidéo de 64 Mio (`pomppc_patch_renderer_info`),
format 32 bits xRGB, identifiant de renderer propre.

## 1 bis. Ce que l'expérience a montré (28/10, VM jetable `SNAPSHOT=1`, kext `QE=1`)

Outil : gdb de l'invité attaché au WindowServer (`/tmp/wsprobe.sh` : on lit dans un point
d'arrêt, puis `detach` dans ses commandes. Un `INT` sans point atteint détache par la suite
du script. Un script qui échoue après un `INT` **tue le WindowServer**, que loginwindow
relance). Trace du plugin dans le WindowServer : `/var/tmp/pomppc-gltrace` contient un
dossier, et chaque processus écrit dans `<dossier>/<nom>-<pid>/trace.txt`
(`pomppc_tracing`).

Les portes, dans l'ordre où le WindowServer les franchit :

1. **`AccelCaps` seul ne suffit pas.** `_CGXAcquireDisplayDeviceSurface` (0x903a6dfc)
   n'essaie le compositeur GL (`_CGXGLAccelForDisplayDevice`) que si les drapeaux d'écran
   `dev+8→+0xac` (lus `lwbrx`) portent **0x80 et 0x08**, puis l'octet **`+0xe9`**. Le bit
   `0x2000` de `__CGSDebugOptionFlags` y mène aussi directement.
2. Ces drapeaux viennent de `__MPSetDisplayFlags` (0x9037ccb0). Elle pose `0x58` si le
   champ `+0xa0` du pilote d'écran porte `0x20`, et `0x80` si **`IOAccelRevision` > 1**
   **sur le framebuffer**. Le kext pose désormais `IOAccelRevision = 2` en mode QE.
3. `+0xa0 |= 0x60` n'est posé par `_CGXMappedDisplayStart` que si
   **`IOPSAllocateBlitEngine`** (IOKit) réussit. Cette fonction charge par
   `IOCreatePlugInInterfaceForService` un **plugin GA** (CFPlugIn de type
   `ACCF0000-0000-0000-0000-000a2789904e`, interface `IOGraphicsAcceleratorInterface`,
   `IOKit/graphics/IOGraphicsInterface.h`). Elle appelle `SetDestination` (+0x50), puis
   `GetBlitter` (+0x54), types et sources (1, 0), (0, 0x6000), puis, facultatifs, (4, 0x1000)
   et (1, 0x2000). C'est ce que font `ATIRadeonGA.plugin` et `GeForceGA.plugin` du système.
   **Sans plugin GA, pas de Quartz Extreme.** Nous n'en avons pas : c'est le prochain
   morceau.
4. **Drapeaux forcés par gdb** (`+0xac = 0xde` gros-boutiste inversé, `+0xa0 |= 0x60`,
   `+0xe9 = 1`) : le WindowServer charge GLEngine, puis **notre plugin** (tranche qgpu 0).
   Il s'arrêtait ensuite à `_cglsChoosePixelFormat`, la liste des formats revenant vide :
   - la liste du WindowServer (`_attribs.0` = `80, 20 8, 21 8, 22 8, 12 0, 13 0`) est en
     attributs **GLI**, que GLEngine passe tels quels : `gliChoosePixelFormat` n'a que deux
     arguments ;
   - le GLDriver logiciel d'Apple (`switch` de 91 cas de `_gldChoosePixelFormat`) exige
     **4** (RGBA, sinon aucun format) et **53** ou un masque d'écrans (84). Il met **4
     tampons auxiliaires** par défaut (`+0x22`, attribut 7), alors que le compositeur
     n'accepte que `+0x22 == 0` ;
   - corrigé dans `pomppc_translate_attribs` : 20..22 portent une valeur, et pour une liste
     du WindowServer (80) on ajoute 4, 53 et `7 0`.
5. Avec ces corrections, **`_cglsCreateContext` réussit**, par `gldCreateContext` de notre
   plugin. Informations Système affiche alors « **Core Image : géré** ». Le bureau reste
   juste. Aucun `AttachDrawable` ni dessin pour autant : les drapeaux n'étaient forcés
   qu'à la main. La suite passe par le plugin GA.

## 1 ter. Fenêtres GL des applications (relevé du 29/10, P2 étape 1)

Aujourd'hui, sous QE, une fenêtre GL passe par le magasin logiciel : AppKit
(`-[NSSurface _createSurface]`) lie la surface en **type 1** (`CGSBindSurface`). Le
GLDriver logiciel d'Apple y écrit, et le compositeur la téléverse
(`_CGXGLCreateSurfaceTexture`). Le journal du kext le confirme : aucune `IOAccelSurface`
d'application, seulement celle de l'écran (id 1). D'où 16 à 17 ms/image pour Marble Blast
et Zenerchi en fenêtre, contre 6,3 et 3,6 ms avec la présentation directe, coupée sous QE.

Le chemin des cartes :

1. GLEngine charge le GLDriver par `gldInitializeLibrary(services, index_fb, n,
   doNonSimpleFlush, bindSurface)` (`_glepValidatePlugin`). Les deux derniers sont
   `_glcDoNonSimpleFlush` et `_glcBindSurface` d'OpenGL.framework (`_glcInitializePlugin`).
2. Le GLDriver matériel lie lui-même la surface de la fenêtre : `glcBindSurface(cid, wid,
   sid, mode, service)` appelle `CGSBindSurface(cid, wid, sid, 2, mode, écran)`. Le
   WindowServer (`_CGXBindSurface`, type 2) crée alors l'`IOAccelSurface` `sid` sur
   l'accélérateur de l'écran (client de surface de notre kext). Le pilote NVIDIA, lui,
   rend dans cette surface par son noyau (`io_connect_method_*`).
3. À l'échange, `glcDoNonSimpleFlush(cid, wid, sid, rect|0, h)` → `CGSFlushSurface` :
   le WindowServer recompose.
4. Le compositeur fait de la surface une texture **sans données** :
   `_CGXGLCreateSurfaceTextureReference` crée une texture `GL_TEXTURE_RECTANGLE` dont le
   pointeur vaut `0xdeadbeef`, puis appelle `cglsSetInteger(ctx, 997, {sid, 0x84F5,
   GL_RGBA|GL_RGB, l, h, type, 0x400|œil, 0})`. Le type vaut `0x8367` en 8888 et `0x8366`
   en 1555. Le GLDriver logiciel d'Apple ne connaît pas 997 (`gldSetInteger` →
   `0x271a`) : le compositeur détruit la texture et retombe sur le magasin.

Sur l'hôte, toutes les tranches partagent un cœur, un contexte GL et un fil de rendu, et
les identifiants de surface et de texture ne sont pas vérifiés par tranche
(`qgpu-core.c`, tranches v19). La tranche du WindowServer peut donc lire une texture du
jeu, et la file unique ordonne les soumissions. **P2 se fait sans changer le protocole** :

- jeu : liaison en type 2 par `bindSurface`, rendu sur sa surface hôte comme aujourd'hui.
  À l'échange, `SURF_TEX` vers une texture d'export (une copie GPU, au lieu de la relecture
  vers l'invité), publication `sid → texture` au kext, puis `doNonSimpleFlush` ;
- kext : table d'export par `sid` (méthodes 2 et 3 du client 2D), purgée à la fermeture
  du client qui a publié ;
- WindowServer : `gldSetInteger(997)` marque la texture rectangle liée ; au dessin,
  c'est la texture d'export du jeu qui est liée sur l'hôte.

## 2. Plan

| Étape | Contenu | Preuve |
|---|---|---|
| **P0** | Le kext publie `AccelCaps = 1` si la personnalité porte `POMPPCQuartzExtreme` (`install.sh QE=1`), éteint par défaut. Sur le disque de dev : journal du WindowServer (« Accel caps », repli éventuel), journal du plugin dans le WindowServer, `CGDisplayUsesOpenGLAcceleration`. | ce que le WindowServer demande, et où il s'arrête |
| **P0 bis** | Plugin GA `POMPPCGA.plugin` (CFPlugIn, `IOGraphicsAcceleratorInterface`) : blitters de copie et de remplissage en VRAM, d'abord par le processeur de l'invité ; `IOCFPlugInTypes` posé sur le framebuffer par le kext en mode QE. | drapeaux d'écran posés sans gdb, `AttachDrawable` du compositeur |
| **P1 (bureau juste, 29/10)** | Le bureau composé par le plugin vers le scanout (le contexte du WindowServer est un plein écran : `SURF_PRESENT`), sans application GL. | bureau identique à celui sans Quartz Extreme (captures), défilement et ombres justes |
| **P2 (29/10)** | Fenêtres des applications GL sous Quartz Extreme : surface accélérée liée par le plugin, présentée par le noyau au Flush, texture d'export pour le compositeur. | matrice en fenêtre verte avec Quartz Extreme |
| **P3 (29/10)** | Vitesse (WindowServer, jeux en fenêtre), endurance, allumage par défaut. Fait : TEX_PRESENT, présentation directe par le noyau, A/B (QE au niveau d'éteint ou mieux), endurance M4 sans incident ; **allumé par défaut sur le M4** (`QGPU_CAP_QE_DEFAULT`), le PC après sa preuve. | A/B sur la matrice entière, endurance |

## 3. Journal

- **30/10, 6 h 05** : **BUG OUVERT : bande blanche à la place d'une ombre sur une fenêtre GL**
  (signalé par l'utilisateur, capture : Terminal au-dessus de Zenerchi en fenêtre, VM de la
  matrice, QE allumé par défaut). Sous l'ombre portée du Terminal (une bande d'environ 15 px à
  droite et en dessous), la fenêtre du jeu est blanche au lieu de montrer le jeu assombri.
  Lecture du code (`present_qe`, `qa_present`) : sur le chemin direct, le kext présente
  l'image dans la région visible de `SetShape`. Cette région exclut le rectangle de l'ombre,
  que le WindowServer se réserve pour y mélanger l'ombre et le contenu de la fenêtre dessous.
  Chaque échange copie bien l'image dans la texture d'export (`SURF_TEX`), mais le
  WindowServer compose la zone d'ombre depuis une autre source, blanche. Hypothèse : le tampon
  de la surface côté invité, jamais rempli puisque l'image ne vit que sur l'hôte. Le
  compositeur ne lierait la texture d'export que pour ses transformations (Génie). C'est
  vraisemblablement le même blanc que celui de l'avis `CGSFlushSurface` pour une fenêtre qui
  n'est pas au premier plan (P3, point 2 du 29/10, 19 h 30). À faire : reproduire, comparer
  avec `POMPPC_GL_QE_DIRECT=0`, tracer la source lue par le WindowServer (lock et lecture de
  la surface, ou liaison de texture), corriger. Deux voies : relire l'image dans le tampon
  invité, limitée aux rectangles hors `SetShape` de la fenêtre ; ou faire composer le
  WindowServer depuis la texture d'export. Ensuite, recontrôler l'avis pour les fenêtres qui
  ne sont pas au premier plan.
  **6 h 23, deuxième capture (Marble Blast sous le Terminal)** : même bande, mais **noire**.
  La couleur dépend du jeu (Zenerchi blanc, Marble Blast noir), et aucun dégradé d'ombre ne
  se voit. Le WindowServer recopie donc dans cette zone le **tampon invité de la surface**,
  qui garde son contenu initial (fond de fenêtre, ou effacement fait par Apple) ; il ne prend
  ni la texture d'export ni l'image de l'hôte. L'hypothèse est confortée, et la voie de
  correction serait de remplir ce tampon, au moins dans les rectangles hors `SetShape`.
  **6 h 40, DOOM 3** : la bande garde même **la trace d'anciens jeux**. La source est donc une
  mémoire qui persiste d'un jeu à l'autre, et non un tampon neuf par fenêtre : soit la VRAM
  de l'écran à la position de la fenêtre (ce que rendent `ReadLock`/`WriteLock` du kext,
  `sLock0`), soit le tampon de forme passé par `SetShapeBacking`. Le chemin direct ne remplit
  ni l'une ni l'autre. Lecture du kext : `Read` (méthode 5 du client de surface, `sRead`) rend
  `kIOReturnUnsupported`. Premier contrôle : `surface N : méthode …` dans le journal de
  l'invité (les 6 premiers appels de chaque méthode y sont notés), avec une ombre posée sur
  le jeu.

- **29/10, 21 h 15** : **curseur sur une fenêtre GL.** Le chemin direct renvoyait au
  WindowServer quand le curseur logiciel était visible sur la fenêtre ; pour une fenêtre qui
  n'est pas au premier plan, l'avis la faisait peindre en blanc. Le kext retire maintenant le
  curseur (`IOFramebuffer::hideCursor`, qui rend son fond), présente en synchrone, puis le
  remet (`showCursor`, qui sauve le nouveau fond) : plus de repli, plus de fond périmé sous le
  curseur (l'ancien Flush l'écrasait aussi). Vérifié : Marble Blast derrière le Terminal,
  curseur sur sa fenêtre, image juste, curseur dessiné, aucune traînée en le déplaçant ;
  matrice mb-fen 6,1, zen-fen 3,1, d3-fen 30,7 ms/image, vertes.

- **29/10, 19 h 30** : **P3 fini : chemin direct, et allumé par défaut sur le M4.**
  1. **Le jeu présente lui-même** son échange par le noyau (méthode 5 du client 2D,
     `0ba5221`) : le kext garde la région visible de chaque surface à `SetShape` et
     présente l'instantané (TEX_PRESENT, soumission asynchrone) sans passer par le
     WindowServer. Repli vers l'avis `CGSFlushSurface` si le curseur est visible sur la
     fenêtre (le WindowServer le redessine alors) ; `POMPPC_GL_QE_DIRECT=0` rend l'ancien
     chemin.
  2. **Plus d'avis au WindowServer sur ce chemin.** Pour une fenêtre qui n'est pas au
     premier plan, `CGSFlushSurface` lui fait peindre le fond blanc de la fenêtre au lieu
     du Flush noyau : DOOM 3 clignotait au chargement derrière le Terminal (un avis par
     seconde), et restait blanc avec l'ancien chemin (captures toutes les 0,15 s). C'est
     aussi l'écran blanc de Marble Blast signalé l'après-midi.
  3. **A/B au repos, même VM, 3 passes par bras** (ms/image, en fenêtre) : QE éteint mb 6,4,
     zen 3,5, d3 29,8, ut 19,9 ; QE asynchrone par le WindowServer 6,2 / 3,55 / 31,6 /
     21,2 ; **QE direct 6,1-6,3 / 2,9-3,1 / 28,8-29,7 / 19,6-19,7**. Les « +16 à +25 % »
     du matin venaient d'une passe unique (UT : fenêtre de 60 images) et d'un frontal
     ImGuiDock orphelin qui chargeait l'hôte. Matrice entière (`20261029-1842`) : tout vert
     sauf cmr-pe (49 replis comptés après la fenêtre, jusqu'à la fin du jeu ; rejouée seule :
     vert, 0 repli, 45,0 ms/image).
  4. **Allumage par défaut, sur le M4 seulement** (règle de parité) : capacité
     `QGPU_CAP_QE_DEFAULT` (propriété `qe` du device), posée par `run_tiger.sh` sur
     Darwin:arm64 (`QE=0` l'éteint, `QE=1` la pose ailleurs). La personnalité du kext
     vaut « auto » par défaut (`install.sh` sans `QE`) et suit ce bit ; `QE=1` / `QE=0` à
     l'installation forcent. Sur le PC, rien ne change tant qu'il n'a pas fait sa preuve
     (matrice entière et tranche d'endurance avec `QE=1`).

- **29/10, 17 h** : **P3, Flush asynchrone dans le noyau** (`0e26278`). Le kext soumet la
  présentation du Flush sans attendre l'hôte (deux moitiés de la zone de 4 Kio, barrière
  attendue avant de réécrire une moitié). Il reste synchrone quand le curseur logiciel est
  visible sur la fenêtre (relevé : le WindowServer ne le masque pas autour du Flush ; Marble
  Blast le laisse visible, Zenerchi et DOOM 3 le cachent), ou avec `POMPPCQESync` dans la
  personnalité. Le d3-fen à 17,7 ms du matin était un artefact (fenêtre de mesure dans la
  cinématique) : la matrice attend maintenant un palier du niveau. A/B au repos, même VM,
  tours `qe13a` (asynchrone) et `qe13b` (synchrone), ms/image : mb 6,7 / 6,4, zen 3,3 / 3,3,
  **d3 31,6 / 36,4 (−13 %)**, ut 24,3 / 24,5, nx 49,5 / 49,8. QE éteint (`0936`) : mb 6,2,
  zen 3,2, d3 28,3, ut 19,5, nx 46,9. Reste en fenêtre : ut +25 %, d3 +12 %, mb +8 %.
  Piège de mesure : un frontal ImGuiDock resté ouvert sans VM (60 % d'un cœur) doublait
  tous les temps en fenêtre.

- **29/10, 12 h** : **P3, A/B et endurance.**
  1. Avis au WindowServer cadencés à 60 Hz au plus (`POMPPC_GL_QE_HZ`) : au-delà, le
     compositeur n'en montre pas plus et chaque avis lui coûte un Flush.
  2. **A/B au même binaire et au même disque**, matrice entière QE éteint (`20261029-0936`,
     16 vertes) contre QE allumé (`20261029-0905`, 16 vertes) puis fenêtres cadencées
     (ms/image, QE contre sans) : mb-fen 6,5 / 6,2 (+5 %), zen-fen 3,7 / 3,2 (+16 %),
     ut-fen 23,5 / 19,5 (+20 %), nx-fen 49,4 / 46,9 (+5 %), d3-fen 17,7 / 28,3 (−37 %, une
     seule passe, à confirmer). Plein écran neutre (−4 à +3 %). prey-fen ne compte pas
     (boucle de l'écran de chargement).
  3. **Endurance avec QE** (`docs/endurance.md` §12) : 30 démarrages froids, 60 reboots,
     16 cycles de jeu en fenêtre (mb, zen, ut, d3), **aucun incident**.

  **Allumage par défaut : pas encore.** QE est sûr (endurance, matrice entière verte),
  mais il coûte en fenêtre (ut +20 %, zen +16 %) et n'a jamais tourné sur le PC (parité).
  Reste avant de l'allumer : le coût par image en fenêtre (copie SURF_TEX + avis), une
  seconde passe de d3-fen, la matrice et l'endurance sur le PC.

- **29/10, 9 h 30** : **P3, vitesse.** Sous P2, le jeu attendait dans `CGSFlushSurface` que
  le noyau ait présenté, donc que l'hôte ait fini son image : aucun recouvrement entre deux
  images (mb-fen 8,9 ms contre 6,2 hors QE). Trois changements :
  1. `QGPU_OP_TEX_PRESENT` (`QGPU_CAP_TEX_PRESENT`, sans changer la version) : le kext
     présente au Flush la **texture d'export**, instantané copié à l'échange, au lieu de la
     surface vivante que le jeu redessine déjà. Lecture par un FBO dans le backend GL
     (`tex_readback`) : `glGetTexImage` (`tex_fetch`) coûtait environ 8 ms par image de
     320×240 sur le GL d'Apple ;
  2. l'avis au WindowServer part d'un **fil à part** (`qn_post`), et un avis en retard est
     remplacé par le suivant : une image peut sauter, aucune ne se déchire.
     `POMPPC_GL_QE_ASYNC=0` rend l'ancien. Synchrone pendant un vidage de preuve ;
  3. fenêtres hors de l'écran (cibles cachées d'IndirectX) : ni copie ni avis
     (`qe_hidden`). Leurs textures de surface retrouvent leur source par `qe_sid` :
     Colin McRae plantait chez Apple (`glrPolyRGB000`) faute de source.

  `glwin` : 1 128 img/s (789 en présentation directe hors QE). **Matrice entière sous QE
  (`20261029-0905`) : 16 cellules vertes sur 16 automatisées**. En plein écran, la vitesse
  est celle de la référence (−4 à +3 %). En fenêtre, il reste +6 à +26 % : mb +10 %,
  zen +23 %, d3 +15 %, ut +26 %, nx +6 %, nxg +8 %. prey-fen (7,1 ms) mesure la boucle de
  son écran de chargement, qui n'attend plus la présentation : ce n'est pas un gain de
  rendu.

- **29/10, 8 h** : **P2, fenêtres GL des applications par l'hôte.** Ce qui marche, dans
  l'ordre où le relevé (§1 ter) l'a dicté :
  1. le plugin reçoit `glcBindSurface` et `glcDoNonSimpleFlush` de GLEngine
     (`gldInitializeLibrary`, arguments 5 et 4 ; le service est cherché dans la liste
     d'OpenGL.framework, l'argument 3 n'en est pas l'index). À l'attache d'une fenêtre
     (type 80, descripteur `{cid, wid, sid}`), il lie la surface en type 2
     (`pomppc_qe_attach`, mode `0x24`) puis rend dans un tampon privé (descripteur 53,
     comme le plein écran, `attach_buffer`). Le pas du contexte est recalé : sous un
     bureau en milliers de couleurs, Apple tirait 2 w pixels par ligne des w × 4 octets
     (Marble Blast plantait dans `upload_bands`) ;
  2. à l'échange (`present_qe`), une copie GPU de la surface hôte vers une texture
     d'export (`SURF_TEX`), publiée au kext (méthode 2 du client 2D : `sid`, texture,
     taille, surface hôte, zone de commandes), puis `glcDoNonSimpleFlush` ;
  3. **le WindowServer ne recompose pas une surface accélérée à chaque vidage** (vidage :
     la texture du jeu liée une seule fois, à l'apparition) : il appelle
     `IOAccelFlushSurfaceOnFramebuffers`, et c'est le **noyau** qui recopie la surface à
     l'écran, dans sa région visible (méthodes 10 et 16 du client de surface, relevées au
     journal du kext). `POMPPCSurfaceClient::sFlush` le fait : un `SURF_PRESENT` de la
     surface hôte du jeu par rectangle visible (région gardée à `SetShape`), écrit dans
     4 Kio réservés en fin de tranche du jeu et soumis en synchrone (`kernelSubmit`). Le
     curseur logiciel est retiré à ce moment-là, et l'image du jeu est déjà en file ;
  4. le compositeur, lui, lie la texture d'export quand il transforme la fenêtre
     (apparition, Génie) : `cglsSetInteger(997)` marque la texture comme `aglSurfaceTexture`
     (GLEngine traite 997 à 999 de même), `upload_remote_surftex` la sert par
     `ws_lookup` (méthode 3), jamais Apple (qui lirait la surface comme de la VRAM).

  `glwin` : image juste, animation et image témoin, 346 img/s (80 par le chemin d'Apple,
  789 en présentation directe hors QE). Matrice en fenêtre sous QE : Marble Blast
  9,6 ms/image (plancher 13), Zenerchi 5,5 (plancher 6), contre 6,2 et 3,5 hors QE.
  L'écart est l'aller-retour synchrone par échange (jeu → WindowServer → noyau, qui attend
  l'image du jeu sur l'hôte) : premier chantier de P3. Preuve d'image de la matrice :
  pendant un vidage seulement, le jeu écrit aussi la présentation dans son flux (position
  au dernier Flush, méthode 4), et `capture_presented` suit le Flush.

- **29/10, 6 h** : **bureau juste sous Quartz Extreme** (VM jetable, `QE=1`). Barre des
  menus, icônes et libellés, Dock, ombres, effet Génie et vignette du Dock, fenêtre GL
  (`glwin`, pixels témoins bons) composée par-dessus le Terminal. Trois causes, trouvées
  par un **vidage du WindowServer rejoué en natif** :
  1. **outil** : `/var/tmp/pomppc-env` (lignes `POMPPC_*=valeur`) pose les variables du
     plugin dans le WindowServer au chargement (`ws_env`) ; une passe du compositeur
     compte pour une image (`stats_frame` au `glFinish`/`glFlush`). Le vidage
     (`POMPPC_GL_DUMP`, `_TRIGGER`, `_FRAMES`) marche alors comme pour un jeu ;
  2. **profondeur** : le rejeu donnait des couleurs justes, l'écran non. La session
     repasse l'écran en **milliers de couleurs** après le démarrage du WindowServer.
     Le plugin GL et le plugin GA gardaient les 32 bpp lus au démarrage : l'image partait
     en XRGB8888 dans un écran 1555 (pixel `FF4A 8C42` lu `f8d050` puis `181010` : les
     colonnes jaunes et violettes, l'image doublée). Géométrie relue au nub à chaque
     vérification d'écran (`ws_main_display`) et à chaque blit (`ga_geometry`) ;
  3. **zone de la surface** : le compositeur attache l'écran (`_CGXGLAccelAccess`,
     descripteur `{0, 0, id de surface}`) puis dessine chaque zone sale dans
     `glViewport(0, 0, l, h)` (passes de 52×281, 57×318, 44×41 au vidage). Sur une carte,
     c'est la **forme de la surface** (`IOAccelSetSurfaceShape`) qui place cette zone à
     l'écran. Le kext garde le rectangle de chaque surface (`setSurfaceBounds`) et le rend
     au client 2D (méthode 1, `POMPPC_GA_SURF_BOUNDS`). Le plugin
     (`pomppc_ws_region`) décale viewport et ciseaux de l'origine de la zone, borne tout
     dessin et tout effacement à la zone, et ne présente qu'elle.

  Au passage, la surface rattachée à chaque passe gardait sa relecture complète puis son
  renvoi complet (2 × 3 Mio par passe) : le tampon gardé reste « hôte plus récent »
  (`ws_kept`). Reste pour P1 : comparaison chiffrée au bureau sans QE, vitesse d'une passe.
- **28/10** : relevé statique ci-dessus. P0 codé : `kext/POMPPCGPU/POMPPCGPU.cpp`
  (`publishAccelerator`), `Info.plist`, `guest/gldriver/install.sh` (`QE=1`).
- **29/10, 0 h 15** : **Quartz Extreme engagé.** Informations Système affiche « Quartz
  Extreme : géré » et « Core Image : géré », sans gdb. Le WindowServer compose par notre
  plugin (`CGXGLAccelCompositeLayer` → `gleBeginPrimitive` → `geom_begin`), et la première
  image composée arrive à l'écran. Session ouverte, Dock et Finder présents. Ce qu'il a
  fallu, dans l'ordre :
  1. le client `IOAccelSurface` du nub (`POMPPCSurfaceClient`, mode QE seulement). Surface
     d'écran id 1, mode `0x24`, forme à l'écran. Version d'exploration : la surface est la
     VRAM au rectangle de sa forme ;
  2. dans le WindowServer, **pas de glsAssignDrawable d'Apple** : il appelle
     `CGSGetSurfaceBounds`, donc le serveur, donc lui-même (interblocage, relevé par
     `sample`). La surface d'écran (type `0x50` = 80) est attachée en plein écran
     (`pomppc_attach_fullscreen`), et l'écran est lu dans le client 2D du nub (`WS.*`,
     `pomppc_ws_process`) au lieu de `CGMainDisplayID`/`CGDisplay*` ;
  3. `pomppc_drawable_attached` traite 80 comme 54. Sinon il libérait le tampon arrière
     que GLEngine venait de recevoir : faute de lecture dans `sync_to_host`, puis reprise
     d'erreur du WindowServer par `setjmp` avec `G.mu` resté pris, et interblocage au
     `fork` suivant (`atfork_prepare`). Trouvé en notant la ligne du dernier verrou ;
  4. le compositeur est à **tampon unique** : il appelle `glFinish`/`glFlush`, jamais
     d'échange. Présentation au `Swap58`/`Swap5c` du contexte plein écran du WindowServer.
     Il rattache la surface avant chaque passe : le tampon est gardé à taille égale.

  **Reste faux** (photo du 29/10) : couleurs jaune et violet en colonnes alternées, et
  géométrie agrandie d'environ ×2 vers le bas à gauche. Textures du compositeur (trace
  `TEX_IMAGE3`) : tuiles **rectangle 256×256**, `GL_BGRA` +
  `UNSIGNED_INT_8_8_8_8_REV` (rangée de 4 096 octets) et **`GL_BGRA` +
  `UNSIGNED_SHORT_1_5_5_5_REV`** (`0x8366`, rangée de 2 048 octets), qu'aucun jeu
  n'emploie, plus une texture `GL_ALPHA`. Suspects : la conversion 1555 REV du cœur,
  et la projection ou le viewport du contexte. Outil à faire : vidage des images du
  WindowServer (`POMPPC_GL_DUMP` n'a pas d'environnement là ; repli par fichier comme
  `/var/tmp/pomppc-gltrace`), puis rejeu natif.
- **28/10, nuit** : **P0 bis fait.** `guest/ga/` (`POMPPCGA.plugin`) : blitters de copie et
  de remplissage exécutés par le processeur de l'invité, sur la VRAM du framebuffer mappée
  par le client 2D du nub (`POMPPC_GA_CLIENT_TYPE`, `POMPPCGAClient` du kext). Le kext pose
  `IOCFPlugInTypes` sur le framebuffer en mode QE. Vérifié **sans gdb** : le WindowServer
  charge le plugin (VRAM `0xb0015000`, 1024×768×32), obtient les deux blitters, et les
  drapeaux d'écran valent `0xd7` (`0x80`, `0x40`, `0x10` posés). **Porte suivante** :
  `_MPAccessSurfaceForDisplayDevice` (0x903a864c) appelle `IOAccelCreateSurface(nub, id,
  0x24 = fenêtré | 8888)`. Le nub refuse le client de surface, et le WindowServer efface
  alors `0x08` : il faut le **client `IOAccelSurface`** (méthodes de
  `IOAccelSurfaceConnect.h`), donc P2 avant P1.
- **28/10, soir** : essais dans une VM jetable (§1 bis), en parallèle de la nuit
  d'endurance. `IOAccelRevision = 2` est posé sur le framebuffer en mode QE, et les
  attributs du WindowServer sont corrigés dans le plugin. Contexte GL du compositeur créé
  (drapeaux forcés). Prochaine étape : le **plugin GA** (P0 bis).
