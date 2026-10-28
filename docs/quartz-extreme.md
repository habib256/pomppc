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

## 2. Plan

| Étape | Contenu | Preuve |
|---|---|---|
| **P0** | Le kext publie `AccelCaps = 1` si la personnalité porte `POMPPCQuartzExtreme` (`install.sh QE=1`), éteint par défaut. Sur le disque de dev : journal du WindowServer (« Accel caps », repli éventuel), journal du plugin dans le WindowServer, `CGDisplayUsesOpenGLAcceleration`. | ce que le WindowServer demande, et où il s'arrête |
| **P0 bis** | Plugin GA `POMPPCGA.plugin` (CFPlugIn, `IOGraphicsAcceleratorInterface`) : blitters de copie et de remplissage en VRAM, d'abord par le processeur de l'invité ; `IOCFPlugInTypes` posé sur le framebuffer par le kext en mode QE. | drapeaux d'écran posés sans gdb, `AttachDrawable` du compositeur |
| **P1 (en cours)** | Le bureau composé par le plugin vers le scanout (le contexte du WindowServer est un plein écran : `SURF_PRESENT`), sans application GL. | bureau identique à celui sans Quartz Extreme (captures), défilement et ombres justes |
| P2 | Fenêtres des applications GL sous Quartz Extreme : surfaces partagées entre processus, client `kIOAccelSurfaceClientType` du nub (aujourd'hui refusé). | matrice en fenêtre verte avec Quartz Extreme |
| P3 | Vitesse (WindowServer, jeux en fenêtre : l'attente du WindowServer mesurée dans les jeux), endurance, allumage par défaut. | A/B sur la matrice entière, endurance |

## 3. Journal

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
