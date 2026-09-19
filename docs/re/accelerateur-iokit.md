# L'accélérateur IOKit : comment OpenGL.framework trouve le pilote d'une carte (Tiger 10.4.6)

Relevé du 19/09/2026 (tâche 4.2), **par lecture** de `OpenGL` (framework), `GLEngine`,
`IOKit` (framework), `CoreGraphics` et des kexts ATI/NVIDIA de l'image Tiger 10.4.6 PowerPC
(désassemblages de `tools/re/ppcanno.py`, binaires extraits du disque de dev par `7z`), **puis
vérifié dans l'invité** : sonde `guest/gltest/accelprobe.c`, jobs `tools/guest/jobs/accel` et
`tools/guest/jobs/install`. Les adresses sont celles du binaire nommé en tête de section.

Convention : **[É]** = établi par lecture ; **[V]** = vérifié dans l'invité.

## 1. Résumé

1. Un pilote de carte se fait connaître en posant, **sur le framebuffer**, deux propriétés :
   `IOAccelTypes` (le chemin, dans le plan IOService, d'un objet de classe `IOAccelerator`) et
   `IOAccelIndex` (le rang du framebuffer pour cet accélérateur). C'est le kext de la carte qui
   les pose, pas IOGraphicsFamily. **[É][V]**
2. CGL associe à chaque bit du masque d'écrans OpenGL l'accélérateur de son framebuffer ;
   GLEngine lit sur cet accélérateur `IOGLBundleName` et charge
   `/System/Library/Extensions/<nom>.bundle/Contents/MacOS/<nom>` **avant** les `GLDriver*` du
   dossier `Resources` d'OpenGL.framework, avec pour masque les écrans qui le désignent. **[É][V]**
3. `gldInitializeLibrary` d'un plugin est appelé **avant** les contrôles de version et
   d'identifiant ; un plugin rejeté est aussitôt déchargé (`gldTerminateLibrary`, puis
   `NSUnLinkModule`). **[É][V]**
4. Le WindowServer tente Quartz Extreme dès qu'un écran a un accélérateur **et** que
   `AccelCaps` est non nul **ou** qu'un `IOAGPDevice` (autre qu'un Rage 128) existe. QEMU
   `mac99` n'a pas d'`IOAGPDevice` : sans `AccelCaps`, publier un accélérateur ne déclenche pas
   Quartz Extreme. **[É][V]**
5. **En single-user, il n'y a aucun framebuffer** (IONDRVSupport n'est chargé que par `kextd`) :
   aucun écran ne désigne l'accélérateur, et un plugin qui ne vit que dans `Extensions` n'est
   pas chargé. **[V]**

## 2. CGL : `_glcLoadRenderers` (OpenGL, 0x90f9ae14)

`_glc_io_data` (0xa0f98544) : `+0x00` nombre d'écrans, `+0x04 + 4·i` le service accélérateur de
l'écran de bit `i`, `+0x84 + i` (octet) l'index de framebuffer. Remis à zéro à chaque chargement.

- **Avec le WindowServer** : pour chaque écran de `CGSGetDisplayList`, `i` = rang du bit de
  `CGDisplayIDToOpenGLDisplayMask` ; le nombre d'écrans devient `i + 1` **avant** la recherche ;
  `CGSServiceForDisplayNumber` donne le framebuffer, `IOAccelFindAccelerator` l'accélérateur.
- **Sans WindowServer** (`CGSGetDisplayList` échoue, attribut `kCGLPFARemotePBuffer`) : CGL
  énumère les services `IOFramebuffer`, les ouvre (`IOServiceOpen` type 0, sinon type 1) et
  **ne compte que ceux qui mènent à un accélérateur**.
- Puis `GL_RESOURCES`, ou `/System/Library/Frameworks/OpenGL.framework/Resources`, pour les
  bundles `GLEngine*` (`_glcParseFolder`).

`_glcGetIOAccelService(i, &service, &index)` (0x90f9f7e8) relit ce tableau ; `CGLSetPBuffer`
(0x90f9f108) s'en sert pour **ouvrir une surface sur l'accélérateur** (`IOAccelCreateSurface`,
donc `IOServiceOpen` de type 0 = `kIOAccelSurfaceClientType`), et s'en passe si l'appel échoue —
exactement comme avec un accélérateur nul.

## 3. `IOAccelFindAccelerator` (IOKit, 0x9070079c)

Déclarée dans `IOKit/graphics/IOGraphicsInterface.h`, clés dans `IOGraphicsInterfaceTypes.h`
(`kIOAccelTypesKey`, `kIOAccelIndexKey`, `kIOAccelRevisionKey`).

```
IORegistryEntryCreateCFProperties(framebuffer)
  IOAccelTypes (chaîne)  -> IORegistryEntryFromPath(maître, chemin)   sinon 0xe00002bc
  IOObjectConformsTo(entrée, "IOAccelerator")                       sinon 0xe00002bc
  IOAccelIndex (nombre)  -> CFNumberGetValue(kCFNumberSInt32Type)    facultatif
```

Rien ne vérifie que l'accélérateur « pilote » ce framebuffer : la désignation suffit. **[É]**

## 4. GLEngine : `_glepLoadRenderers` (0x1630) et la validation

- Pour chaque `i < _gle_io_data[0]` dont le service est non nul : `IOGLBundleName` →
  `"/System/Library/Extensions/" + nom + ".bundle/Contents/MacOS/" + nom` ; si ce chemin est déjà
  dans la liste, le bit `1 << i` est ajouté à son masque (`plugin+0x110`), sinon un plugin est
  ajouté avec ce masque.
- Puis, seulement si les deux rappels de `_gle_io_data+0xc/+0x10` sont posés (chargement normal
  par CGL) : `scandir` de `Resources` (ou de `GL_RESOURCES`), et ajout de tout `GLDriver*` et de
  `GLRendererFloat`, en disposition **plate** `<dossier>/<nom>.bundle/<nom>`, avec pour masque
  **tous** les bits `1 << i` pour `i < _gle_io_data[0]`.
- CGL garde l'ordre de la liste : **un pilote désigné par un accélérateur passe avant le
  `GLDriver` d'Apple, quel que soit son nom.** [V] : `CGLQueryRendererInfo` rend `0x00027700`
  (POMPPC, accéléré), puis `0x00020200` (GLDriver), puis `0x00020400` (GLRendererFloat).

`_glepPluginConnect` (0x1a40) : `NSCreateObjectFileImageFromFile`, `NSLinkModule` (option 2,
module privé), puis `_glepValidatePlugin` (0x1b58), qui :

1. appelle **`gldInitializeLibrary(services, index, masque, rappel 0xc, rappel 0x10)`** ;
2. appelle `gldGetVersion`, exige 2.4.11 et un identifiant (octet `0xff00`) **absent** des
   plugins déjà chargés ;
3. sinon `_glepPluginDisconnect` (0x559dc) : `gldTerminateLibrary`, `NSUnLinkModule`,
   `NSDestroyObjectFileImage`.

Conséquences pour le plugin, vues en vrai : un second exemplaire (la même binaire dans
`Extensions` et dans `Resources`) passe par `gldInitializeLibrary` avant d'être rejeté. Il ne
doit rien garder : une tranche du kext resterait prise jusqu'à la fin du processus, et un
`atexit` pointerait dans du code déchargé. D'où la marque `POMPPC_GLD_OWNER` (pid du
propriétaire) et `pomppc_backend_fini` (`guest/gldriver`). [V] : trace
`gldInitializeLibrary … : second exemplaire du plugin, inactif`, puis
`gldTerminateLibrary() (second exemplaire)`, image de `glwin` juste.

## 5. Qui pose `IOAccelTypes` : les kexts de carte

`ATIRage128.kext`, `ATIRadeon9700.kext`, `GeForce.kext` contiennent les chaînes
`IOAccelTypes`, `IOAccelIndex`, `IOAccelRevision` et dérivent d'`IOAccelerator`
(`__ZN13IOAccelerator10gMetaClassE`) ; `IOGraphicsFamily` ne contient ni `IOAccelTypes` ni
`IOAccelIndex`. La personnalité d'ATIRage128 (`Info.plist`) :

| Clé | Valeur |
|---|---|
| `IOProviderClass` / `IOPCIMatch` | `IOPCIDevice`, le **même** device PCI que le framebuffer NDRV |
| `IOMatchCategory` | `IOAccelerator` (sinon le framebuffer et l'accélérateur se disputeraient le device) |
| `IOGLBundleName` | `ATIRage128GLDriver` |
| `IOCFPlugInTypes` | `ACCF0000-…` → `ATIRage128GA.plugin` (accélération 2D du WindowServer) |
| `IODVDBundleName` | `ATIRage128DVDDriver` |

## 6. Le WindowServer et Quartz Extreme (CoreGraphics)

`_CGXGLDisplayContextInitialize` (0x92eedef8) :

1. pour chaque écran : `_CGXAcceleratorForDisplayDevice` → `IOAccelFindAccelerator` sur son
   framebuffer ; `_getAccelCapsBits` (0x92eee7a8) lit la propriété **`AccelCaps`** de
   l'accélérateur (journal : « Accel caps: %08lx »), et les masques sont réunis ;
2. `_checkForValidDevice` (0x92eee80c) rend 1 s'il existe un **`IOAGPDevice`** dont le `model`
   ne commence pas par `ATY,Rage128`, 0 sinon — **et 0 s'il n'y en a aucun** ;
3. **si les deux sont nuls, pas de Quartz Extreme.** Sinon `_cglsInitialize` charge les
   renderers dans le WindowServer, et pour chaque écran : `_cglsQueryRendererInfo`, mémoire vidéo
   ≥ `GLCompositorMinimumVRAM` (16 par défaut), `_isAccelUsable` (0x92eef7ec : listes
   d'autorisation de `GLCompositorConfiguration`, **1 par défaut**), formats de pixels accélérés…
   puis `_cglsCreateContext`.

`_MPSetDisplayFlags` (0x92ec2094) lit aussi `IOAccelRevision` **sur le framebuffer** (drapeau
0x80 si > 1) : inutile ici.

[V] Sur QEMU `mac99` : `IOAGPDevice` = 0 instance (`AppleMacRiscAGP` = 0 aussi). Avec le kext
installé, chargé **avant** le WindowServer, le bureau démarre normalement et
`CGDisplayUsesOpenGLAcceleration` rend faux.

## 7. Ce que fait POMPPC (kext `POMPPCGPU`, 19/09/2026)

- Un **nub enfant** `POMPPCAccelerator : IOAccelerator` sous `POMPPCGPU`, avec
  `IOGLBundleName = GLDriver-POMPPC`. Pas `POMPPCGPU` lui-même : le transport s'ouvre en type 0,
  qui est aussi `kIOAccelSurfaceClientType`, et changer ce type casserait tous les clients
  existants. Le nub **refuse toute ouverture** (`kIOReturnUnsupported`) tant qu'il n'y a pas de
  surfaces (tâche 4.4).
- **Pas d'`AccelCaps`**, donc pas de Quartz Extreme (§6).
- Liaison des framebuffers par une notification de publication sur `IOFramebuffer` (appelée
  aussi pour ceux qui existent déjà : le kext se charge à chaud dans la boucle de dev) ; un
  framebuffer qui désigne déjà un **autre** accélérateur est laissé tel quel. Au déchargement,
  les deux propriétés sont retirées. [V] : 0 instance après `kextunload`, propriétés absentes,
  rechargement propre.
- Plugin dans `/System/Library/Extensions/GLDriver-POMPPC.bundle` (disposition standard,
  `Contents/Info.plist`), plus rien dans OpenGL.framework. Sans le device, pas d'accélérateur,
  donc pas de plugin.

## 8. Vérification (19/09/2026, VM de dev Linux, device v12)

| Mode | Résultat |
|---|---|
| single-user | nub publié ; **aucun framebuffer**, donc rien à lier ; 0 `IOAGPDevice` ; déchargement / rechargement propres ; `gltest` par POMPPC avec la copie « à plat » que la boucle de dev pose alors dans `Resources` (`plugin_layout`, `tools/guest/jobs/lib.sh`) |
| bureau, kext chargé depuis `/tmp` | `IONDRVFramebuffer` désigne `POMPPCAccelerator`, index 0 ; `IOServiceOpen` type 0 → `0xe00002c7` ; renderers `0x00027700` (accéléré) puis `0x00020200`, `0x00020400`, masque 0x1 ; Quartz Extreme inactif ; `gltest tri tex lit caps` et `glwin` par POMPPC avec le plugin **seulement** dans `Extensions` |
| bureau, **installé par `install.sh`** puis redémarré | idem : le WindowServer a vu l'accélérateur au démarrage, le bureau est normal, Quartz Extreme inactif |
| second exemplaire | §4 : inactif, puis rejeté par GLEngine ; `glwin` juste |

## 9. Pour Quartz Extreme (tâche 4.4)

- `AccelCaps` sur l'accélérateur (ou un `IOAGPDevice`) ouvre la porte : à ne poser qu'une fois
  la suite tenue.
- Le client de surface (`kIOAccelSurfaceClientType`, méthodes de
  `IOKit/graphics/IOAccelSurfaceConnect.h`) : `IOAccelCreateSurface`, verrous en lecture et en
  écriture, forme de la surface.
- La mémoire vidéo : notre renderer annonce **0** (`kCGLRPVideoMemory`, héritée du GLDriver
  d'Apple), sous le minimum `GLCompositorMinimumVRAM`. Des jeux lisent aussi cette valeur.
