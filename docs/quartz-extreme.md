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

## 2. Plan

| Étape | Contenu | Preuve |
|---|---|---|
| **P0** | Le kext publie `AccelCaps = 1` si la personnalité porte `POMPPCQuartzExtreme` (`install.sh QE=1`), éteint par défaut. Sur le disque de dev : journal du WindowServer (« Accel caps », repli éventuel), journal du plugin dans le WindowServer, `CGDisplayUsesOpenGLAcceleration`. | ce que le WindowServer demande, et où il s'arrête |
| P1 | Le bureau composé par le plugin vers le scanout (le contexte du WindowServer est un plein écran : `SURF_PRESENT`), sans application GL. | bureau identique à celui sans Quartz Extreme (captures), défilement et ombres justes |
| P2 | Fenêtres des applications GL sous Quartz Extreme : surfaces partagées entre processus, client `kIOAccelSurfaceClientType` du nub (aujourd'hui refusé). | matrice en fenêtre verte avec Quartz Extreme |
| P3 | Vitesse (WindowServer, jeux en fenêtre : l'attente du WindowServer mesurée dans les jeux), endurance, allumage par défaut. | A/B sur la matrice entière, endurance |

## 3. Journal

- **28/10** : relevé statique ci-dessus. P0 codé : `kext/POMPPCGPU/POMPPCGPU.cpp`
  (`publishAccelerator`), `Info.plist`, `guest/gldriver/install.sh` (`QE=1`). Il n'a pas
  encore été essayé dans l'invité : la nuit d'endurance du M4 occupait l'hôte.
