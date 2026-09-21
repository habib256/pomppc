# Références — kext, framebuffer, plugin, hôte

Ce que les textes publics couvrent, et où ils s'arrêtent. Le contrat
`GLEngine` / `gld*` n'est dans aucun d'eux : il est dans `docs/re/`.

À lire en variante **Tiger 10.4**. Les pages Léopard décrivent
`IOUserClient::externalMethod` et `IOConnectCallMethod`. Tiger appelle
`getTargetAndMethodForIndex` et `IOConnectMethodScalarIScalarO`. Un kext
écrit d'après Léopard ne parle pas au plugin.

## 1. Kext

Le transport (`POMPPCGPU`) est un user client : table `IOExternalMethod`,
mémoire publiée par `clientMemoryForType`, interruption vers le plugin.

| Texte | À quoi il sert |
|---|---|
| [I/O Kit Fundamentals](https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/Introduction/Introduction.html) | familles, personnalités, registre, appariement |
| [Making Hardware Accessible to Applications](https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/WritingDeviceDriver/MakingHWAccessible/MakingHWAccessible.html) | `IOServiceOpen`, `IOConnectMethod*`, `clientMemoryForType`, IRQ |
| [SimpleUserClient](https://developer.apple.com/library/archive/samplecode/SimpleUserClient/Introduction/Intro.html) et [User Client Info](https://developer.apple.com/library/archive/samplecode/SimpleUserClient/Listings/User_Client_Info_txt.html) | l'exemple Apple, avec une cible **10.4** séparée |
| [IOUserClient.h](https://leopard-adc.pepas.com/documentation/Darwin/Reference/KernelIOKitFramework/IOUserClient_h/index.html) | `kIOUCScalarIScalarO`, `kIOUCVariableStructureSize`. Lire `IOExternalMethod` (cinq champs), pas `IOExternalMethodDispatch` |

## 2. Écran et accélérateur

Apple sépare l'image linéaire et la 3D. Le framebuffer ne dessine pas.
L'accélération est un bundle en espace utilisateur. Le noyau ne fait que
le nommer (`IOGLBundleName`).

| Texte | À quoi il sert |
|---|---|
| [Familles I/O Kit, Graphics](https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/Families_Ref/Families_Ref.html) | « there is no standard way to implement » la 3D. `IOGLBundleName` n'est pas une API |
| [IOFramebuffer.h](https://github.com/apple-oss-distributions/IOGraphics/blob/main/IOGraphicsFamily/IOKit/graphics/IOFramebuffer.h) | framebuffer bête ; 2D/3D par une autre classe `IOAccelerator` |
| [IOGraphics, arbres 10.4](https://opensource.apple.com/source/IOGraphics/) | `IONDRVFramebuffer`, `IOFramebufferUserClient`. Pas la branche actuelle |
| [IOFramebufferShared.h](https://opensource.apple.com/source/IOGraphics/IOGraphics-517.17/IOGraphicsFamily/IOKit/graphics/IOFramebufferShared.h.auto.html) | contrat curseur / VRAM du WindowServer (`kIOFBVRAMMemory`). Pas une API du plugin |
| `docs/re/accelerateur-iokit.md` | comment CGL trouve le plugin, et pourquoi Quartz Extreme ne part pas sans `AccelCaps` |

`IOAccelerator` dans le SDK 10.4 est un `IOService` sans méthode. Le nub
`POMPPCAccelerator` est déjà dans cette forme.

## 3. Plugin OpenGL

Les notes Apple s'arrêtent au-dessus de `gldInitDispatch`. [QA1188](https://developer.apple.com/library/archive/qa/qa2001/qa1188.html)
dit comment un jeu obtient un point d'entrée, pas comment un pilote en
fournit un.

La spécification de la couche privée est `docs/re/` (`accelerateur-iokit.md`,
`descripteur-de-sommet.md`, `etat-tcl.md`, `tableaux-de-sommets.md`) et
`docs/gpu-3d-tiger.md`. Le sens des appels traduits est la spécification
OpenGL 1.5, pas un en-tête de pilote.

## 4. Hôte et protocole

virtio-gpu a déjà tranché les mêmes choix : ressources chez l'hôte, file
de commandes, scanout, mémoire partagée. La 3D n'est pas une relecture de
`glBegin` : c'est un flux vers un interpréteur.

| Texte | À quoi il sert |
|---|---|
| [virtio-gpu](https://github.com/oasis-tcs/virtio-spec/blob/master/device-types/gpu/description.tex) | ressources, scanout, blob, synchronisation |
| [virtio-gpu dans QEMU](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html) | `virglrenderer` : langage intermédiaire, pas des appels GL bruts |
| [pci.h](https://github.com/qemu/qemu/blob/master/include/hw/pci/pci.h) | `pci_register_bar` |
| [memory.h](https://github.com/qemu/qemu/blob/master/include/system/memory.h) | `MemoryRegion`. Un bus PCI est petit-boutiste ; le PowerPC ne l'est pas. L'endianness se choisit |

## Ce que ces textes ne justifient pas

- Réécrire le user client en API Léopard.
- Annoncer Quartz Extreme (`AccelCaps`, VRAM, client de surface) tant que
  `mac99` n'a pas d'`IOAGPDevice` et que les jeux quittent encore le chemin
  brut. Porte relevée : `docs/re/accelerateur-iokit.md` §6 et §9.
- Chercher dans la doc Apple les offsets de `GLEngine`. Ils n'y sont pas.
