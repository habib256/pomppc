# GPU sous Tiger PPC — kext, paravirtualisation et le cas de la GeForce 4060 Ti

Document d'implémentation — POMPPC, août 2026.
Cible : Mac OS X 10.4.11 PPC invité sur QEMU `mac99` (voir `run_tiger.sh`), hôte Linux x86-64 équipé d'une **GeForce RTX 4060 Ti (AD106, Ada Lovelace)**.

---

## 0. Verdict en une page

**Ce qui est demandé** — un kext Tiger qui « connecte » la 4060 Ti — se décompose en trois choses très différentes :

| Lecture de la demande | Faisable ? | Coût |
|---|---|---|
| **A.** La 4060 Ti est *passée en direct* (vfio-pci) à l'invité Tiger, qui la pilote avec un kext maison | **Non.** Verrous matériels cumulés, dont deux rédhibitoires (voir §2) | ∞ |
| **B.** Tiger reçoit un **GPU paravirtualisé** (device QEMU) piloté par un kext maison ; l'hôte fait le rendu final **sur la 4060 Ti** | **Oui**, c'est le seul chemin réaliste | ~2–6 semaines par palier (§4/§6) |
| **C.** Tiger utilise ses **drivers d'époque** sur un GPU émulé qu'il connaît déjà (`ati-vga` = Radeon RV100) | **Peut-être**, à tester en 1 jour ; l'émulation ATI de QEMU est incomplète | 1–2 jours d'essai |

**Recommandation** : faire **C** comme expérience de reconnaissance (une journée, réponse binaire), puis **B** comme projet — device paravirtuel `qfb-pci` + `POMPPCQFB.kext` (sous-classe `IOFramebuffer`), avec le scanout composité sur la 4060 Ti par le frontend ImGui déjà en place.

**Attente de perf à calibrer tout de suite** : le profil GTK déjà mesuré sur ce projet donne *0,65 % du temps hôte* dans l'affichage. Un GPU paravirtuel n'accélérera donc **pas** Tiger de façon spectaculaire : il apporte des résolutions/profondeurs libres, le changement de mode à chaud, un **curseur matériel** (vrai gain de CPU invité), des dirty-rects, et surtout un chemin propre vers le rendu hôte (upscale, shaders, vsync, capture NVENC) — pas un facteur 2 sur le bureau. Le facteur 2 est dans le JIT.

---

## 0 bis. État au 18 août 2026 — ce qui est implémenté

L'option B1 est **faite côté hôte**, en adoptant un protocole existant plutôt qu'un protocole maison :
le framebuffer paravirtualisé **« qfb1 »** de Solra Bizna ([`mac_qfb_driver`](https://github.com/SolraBizna/mac_qfb_driver),
NuBus/68k) a été porté sur PCI pour `mac99`.

| Livrable | Où | État |
|---|---|---|
| Device QEMU `qfb-pci` | `patches/qfb/qfb-pci.c` + `scripts/build_qemu_qfb.sh` | **fait, testé** (QEMU 9.2.0 reconstruit dans `~/src/qemu`) |
| Test de bout en bout sans invité | `tests/qfb_smoke.py` | **passe** : mode programmé en Forth depuis Open Firmware, `rowbytes` relu, pixels vérifiés sur `screendump` |
| Non-régression Tiger | boot headless avec le device attaché | **OK** : bureau intact sur l'écran VGA, second écran QFB présent |
| Kext `POMPPCQFB` (Tiger) | `kext/POMPPCQFB/` | **écrit**, reste à compiler dans l'invité (toolchain gcc 4.0) |
| Lancement | `QFB=1 ./run_tiger.sh` | ajoute l'écran QFB en second moniteur |

Détails et protocole : `kext/POMPPCQFB/README.md`.

---

## 1. Contexte matériel et logiciel (ce sur quoi on s'appuie)

- Machine émulée : `mac99,via=pmu`, CPU G4, OpenBIOS (build unifié `patches/smp-mac99/openbios-smp-screamer.elf`), QEMU **9.2.0 reconstruit depuis les sources** (`scripts/build_qemu_qfb.sh` → `$HOME/src/qemu/build/`, le binaire par défaut de `config.env`) — `vfio-pci`, `ati-vga`, `virtio-gpu-pci` y sont **présents** (`qemu-system-ppc -device help`). Le paquet distro (8.2) les a aussi, mais lui manque le Screamer, l'OpenBIOS unifié et `qfb-pci`.
- Affichage actuel : device `VGA` (std) + `qemu_vga.ndrv` chargé par OpenBIOS → l'invité passe par `IONDRVFramebuffer`, ce qui explique le catalogue de modes figé (1920x1080 OK, 1440x900 retombe en 800x600 — cf. commentaires de `run_tiger.sh`).
- Frontend : `frontend/` (CMake + ImGui + `QemuBridge` GDBus, `-display dbus,p2p=on`, fast-path `Unix.Map`/mmap déjà prouvé). C'est **là** que la 4060 Ti travaille aujourd'hui, et là qu'elle travaillera demain.
- Côté invité, Tiger PPC embarque `NVDAResmanPPC.kext` + `NVDANV40HalPPC.kext` (NV4x/G70 = GeForce 6/7, Quadro FX 4500) et `ATIRage128` / `ATIRadeon`. C'est le plafond absolu du support NVIDIA sur PPC : **cinq générations avant Ada**.

---

## 2. Pourquoi la 4060 Ti en passthrough est un cul-de-sac

Le passthrough vers un invité `mac99` **existe** : un utilisateur a fait fonctionner un affichage basique sous Mac OS X 10.4.11 avec une **ATI Rage 128 GL « Mac Edition » PCI** en `vfio-pci,rombar=1,romfile=…` + un OpenBIOS patché (`…-pci-map-in-rage128.elf`), sans accélération, la carte Mac apportant son propre FCode/NDRV. C'est la borne haute réelle du procédé : carte PCI 32 bits, petits BARs, ROM Mac, driver présent dans l'OS.

La 4060 Ti échoue sur chacun de ces points, et sur d'autres :

| # | Verrou | Détail | Gravité |
|---|---|---|---|
| 1 | **Firmware GSP** | Ada ne s'initialise pas sans la chaîne `booter`/FWSEC signée puis **GSP-RM** (cœur RISC-V, blobs de dizaines de Mo, API RM propriétaire versionnée). Nouveau y a consacré des années et ne fait que *piloter* le blob par RPC. Rien de tout cela n'est réimplémentable dans un kext PPC. | **Rédhibitoire** |
| 2 | **Endianness** | Le switch MMIO big-endian des GPU NVIDIA (`NV_PMC_BOOT_1_ENDIAN`) **s'arrête à Pascal**. Ada est little-endian only — d'où le patch noyau explicite « nova-core: require little endian ». Tout accès depuis un kext PPC devrait être byte-swappé à la main, et les structures RPC/GSP restent LE. | **Rédhibitoire** |
| 3 | **Fenêtre PCI** | `uninorth` (mac99) expose un trou mémoire PCI de **256 Mio à 0x8000_0000** (variante AGP/U3 : ~1,75 Gio). La 4060 Ti demande BAR0 16 Mio + BAR1 ≥ 256 Mio + BAR3 32 Mio → ça ne rentre pas dans le pont principal. | Bloquant |
| 4 | **Espace de config** | `unin-pci-conf-idx/data` = mécanisme CFA classique, **pas d'espace de configuration étendu (> 256 octets)** → aucune capability PCIe (link, AER, ReBAR, MSI-X) visible par l'invité. | Bloquant |
| 5 | **Pas de ROM Open Firmware** | Aucune carte PC n'embarque de FCode/NDRV Apple ; OpenBIOS ne peut ni initialiser la carte ni s'en servir comme console de boot. Le VBIOS x86 est inutilisable (et Ada est de toute façon UEFI/GOP). | Bloquant |
| 6 | **Pile driver inexistante** | Aucun driver NVIDIA Ada n'a jamais existé pour OS X, PPC ou Intel. Il faudrait écrire *ex nihilo* l'équivalent de `NVDAResman` + HAL + RM client. | Bloquant |
| 7 | **Interruptions** | Pas de MSI/MSI-X sur uninorth → repli INTx, et adressage DMA 32 bits (le noyau PPC 10.4 travaille en `IOPhysicalAddress` 32 bits). | Sévère |
| 8 | **TCG** | Sans KVM, chaque accès MMIO est un aller-retour interprété. Un driver GPU moderne en fait des millions à l'init. | Sévère |

**Ne pas tenter naïvement** : lier la 4060 Ti à `vfio-pci` sur l'hôte lui retire l'affichage (il faudrait un second GPU/iGPU). Si tu veux la trace écrite de l'échec pour archive, fais-le avec une **vieille carte PCI/AGP secondaire**, pas avec la 4060 Ti.

**Ce que la 4060 Ti peut faire, elle le fait déjà côté hôte** : compositer/upscaler le framebuffer invité (GL/Vulkan), appliquer des shaders (nearest/CRT/FSR), tenir la vsync, encoder la capture (NVENC). C'est le seul sens dans lequel « Tiger utilise ta 4060 Ti », et le plan §4 est bâti pour l'exploiter.

---

## 3. Les trois architectures candidates côté invité

```
   ┌──────────────── INVITÉ Tiger 10.4 PPC ────────────────┐   ┌──── HÔTE ────┐
C  │ ATIRadeon.kext (Apple, d'époque)  ← ati-vga (RV100)   │──▶│              │
B1 │ POMPPCQFB.kext (nous) ← device paravirtuel « qfb-pci » │──▶│  QEMU  ──▶   │  frontend ImGui
B2 │ POMPPCVirtioGPU.kext (nous) ← virtio-gpu-pci (amont)  │──▶│              │  ──▶ GL/Vulkan
A  │ (impossible) ← vfio-pci 4060 Ti                       │   │              │  ──▶ RTX 4060 Ti
   └───────────────────────────────────────────────────────┘   └──────────────┘
```

| Critère | **C** `ati-vga` | **B1** `qfb-pci` (retenu) | **B2** `virtio-gpu` 2D |
|---|---|---|---|
| Code invité à écrire | **zéro** | kext `IOFramebuffer` simple | kext + pile virtio (files, LE) |
| Patch QEMU | non (device amont) | **oui** (device de ~700 lignes de C, fait) | non |
| Endianness | géré par QEMU | **choisie par nous** (regs `DEVICE_BIG_ENDIAN` → aucun swap dans le kext) | files virtio **little-endian** → swaps partout |
| Modes/profondeurs libres | limité par le driver Apple | **oui** | oui (+EDID) |
| Curseur matériel | oui (si émulé correctement) | oui | oui (curseur queue) |
| Accélération 2D exploitable par Quartz | non | non (voir §4.6) | non |
| Voie 3D future | non | non | virgl (nécessiterait un driver GL invité → hors périmètre, cf. README) |
| Risque principal | l'émulation ATI est **incomplète** et « MacOS ne trouve pas la carte » (device tree manquant) | il faut maintenir un device QEMU | complexité virtio sur BE + spec plus lourde pour un gain nul à court terme |
| Verdict | **expérience J+1** | **projet recommandé** | plan B si on refuse de patcher QEMU |

Note : `classicvirtio` (elliotnunn) avait un driver virtio-gpu et l'a **retiré** au profit du framebuffer paravirtuel « QFB » de SolraBizna (aujourd'hui `nubus-qfb` dans QEMU, côté m68k/Quadra 800, ROM auto-patchée par QEMU, résolutions jusqu'à 3840x2160 + gamma + multi-écran). Autrement dit : la communauté PPC/68k a déjà tranché dans le sens de **B1**. `nubus-qfb` est le modèle à copier, en version PCI.

---

## 4. Spécification — option B1 (`qfb-pci` + `POMPPCQFB.kext`)

### 4.1 / 4.2 Device QEMU — protocole qfb1 (implémenté)

Le device retenu est `qfb-pci` (`patches/qfb/qfb-pci.c`), portage PCI du `nubus-qfb`
d'origine — plutôt que le protocole maison esquissé dans les premières versions de
ce document. BAR0 = VRAM 32 Mio (RAM avec dirty-logging), BAR1 = 4 Kio de registres
déclarés `DEVICE_BIG_ENDIAN`, donc **aucun byte-swap dans le kext**. Signature
`'qfb1'` en 0x00, mode en 0x04–0x14 (`WIDTH`, `HEIGHT`, `DEPTH`, `BASE`, `STRIDE`
recalculé par le device), palette et rampes gamma par paires index/valeur, VBL
60 Hz masquable. Profondeurs 1/2/4/8/16/24, où **24 = 32 bits par pixel xRGB**.

Tableau complet des registres : `kext/POMPPCQFB/README.md`.

Vérifié : OpenBIOS crée le nœud `/pci@f2000000/pci1234,fb1@f` et **assigne les BARs**
(VRAM 0x82000000, registres 0x84000000, `interrupts 1`) bien qu'il affiche
« Cannot manage 'misc display controller' » — il ne sait pas s'en servir comme
console, ce qui est exactement le partage des rôles voulu (§4.4).

### 4.3 Le kext `POMPPCQFB.kext` (invité)

> Cette section était la **spécification écrite avant le code**. Le kext existe désormais
> (`kext/POMPPCQFB/`) : en cas de désaccord, **c'est la source qui fait foi**, et le tableau
> de registres de référence est `kext/POMPPCQFB/qfb_regs.h`. Les identifiants ci-dessous ont
> été réalignés sur l'implémentation ; les « points d'implémentation » gardent leur valeur de
> notes de conception, avec les écarts assumés signalés.

Arborescence réelle (pas de sous-dossier `src/`) :

```
kext/POMPPCQFB/
  POMPPCQFB.h  POMPPCQFB.cpp     (sous-classe IOFramebuffer)
  qfb_regs.h                     (le tableau §4.2, miroir du device QEMU)
  Info.plist  Makefile  go.sh
→ produit POMPPCQFB.kext/Contents/{Info.plist,MacOS/POMPPCQFB}
```

`Info.plist` — points sensibles pour 10.4 (Darwin 8) :

```xml
<key>OSBundleLibraries</key>
<dict>
  <key>com.apple.kpi.iokit</key>       <string>8.0.0</string>
  <key>com.apple.kpi.libkern</key>     <string>8.0.0</string>
  <key>com.apple.kpi.mach</key>        <string>8.0.0</string>
  <key>com.apple.iokit.IOPCIFamily</key>      <string>1.4</string>
  <key>com.apple.iokit.IOGraphicsFamily</key> <string>1.4</string>
</dict>
<key>IOKitPersonalities</key>
<dict><key>POMPPCQFB</key><dict>
  <key>CFBundleIdentifier</key>   <string>net.pomppc.POMPPCQFB</string>
  <key>IOClass</key>              <string>POMPPCQFB</string>
  <key>IOProviderClass</key>      <string>IOPCIDevice</string>
  <key>IOPCIPrimaryMatch</key>    <string>0x0fb11234</string>
  <key>IOProbeScore</key>         <integer>60000</integer>
</dict></dict>
```

`IOPCIPrimaryMatch` attend `0xDDDDVVVV` — **device dans les 16 bits de poids fort, vendor dans
ceux de poids faible**. D'où `0x0fb11234` : device `0x0fb1` (`PCI_DEVICE_ID_QEMU_QFB`) sous le
vendor QEMU `0x1234`, tous deux définis dans `patches/qfb/qfb-pci.c`. Inverser les deux moitiés
est la façon habituelle de se retrouver avec un kext qui charge sans jamais matcher.

**Attention ABI 10.4** : les attributs `IOFramebuffer` se passent en `UInt32 *` sur Darwin 8 ;
`uintptr_t` n'arrive qu'en 10.5. Copier une signature d'un exemple moderne empêche le
`start()` d'être appelé. Voir l'en-tête de `POMPPCQFB.h`, qui liste les méthodes réellement
implémentées.

Points d'implémentation qui font gagner ou perdre une semaine :

1. **Mapping** : `provider->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)` → **VRAM**
   (BAR0), renvoyée par `getApertureRange()`/`getVRAMRange()` ; `…BaseAddress1` → les
   registres, qu'on `map()`. `IOFramebuffer` mappera l'ouverture pour le WindowServer avec le
   cache-mode annoncé dans `getPixelInformation` (`kIOMapWriteCombineCache` inadapté sur PPC ;
   rester en **cacheable write-back** — la région est de la RAM côté QEMU et le dirty-logging
   fonctionne malgré le cache, mais valider visuellement des zones « rémanentes »).
2. **Catalogue de modes** : le protocole qfb1 **n'a pas de registre `MODE_COUNT`** — il ne
   publie que le mode courant et le mode demandé sur la ligne de commande
   (`CUSTOM_WIDTH`/`HEIGHT`/`DEPTH`). Le kext porte donc un catalogue fixe (640×480 →
   1920×1200), filtré par ce qui tient dans les 32 Mio de VRAM, auquel s'ajoute le mode
   `CUSTOM_*` s'il n'y figure pas déjà. `IODisplayModeID` = index+1 (jamais 0). Renseigner
   `IODisplayModeInformation.flags` avec `kDisplayModeValidFlag | kDisplayModeSafeFlag`
   (+ `kDisplayModeDefaultFlag` sur un seul).
3. **`getPixelInformation`** : `bytesPerRow` = `QFB_MODE_STRIDE` relu **après** application du
   mode, `bitsPerPixel` 32, `componentCount` 3, masques `0xFF0000/0x00FF00/0x0000FF`,
   `pixelType = kIORGBDirectPixels`, `pixelFormat = IO32BitDirectPixels`. Toute incohérence
   ici = bureau strié ou panic du WindowServer.
4. **Ordre PPC 32 bpp** : l'invité est big-endian, la mémoire contient `xRGB` ; côté QEMU
   déclarer le format `PIXMAN_x8r8g8b8` en BE. Se tromper d'ordre est le bug n°1 de ce genre
   de driver (canaux permutés / bleu-rouge inversés).
5. **Curseur : non implémenté, et c'est définitif** — le protocole qfb1 n'a aucun registre de
   curseur (§4.6). Le kext répond donc `kIOHardwareCursorAttribute` = 0 et laisse 10.4
   compositer un curseur logiciel. Ne pas ressortir `setCursorImage`/`convertCursorImage`
   sans avoir d'abord étendu le device — ce qui casserait la compatibilité avec le `nubus-qfb`
   d'origine.
6. **Interruptions** : `kIOFBVBLInterruptType` via `registerForInterruptType`, servi par un
   `IOFilterInterruptEventSource` sur l'INTx du device. Le kext ne démasque `IRQ_MASK` que
   lorsqu'un client s'abonne (`setInterruptState`) : une IRQ 60 Hz coûte cher sous TCG.
7. **Pas de 64 bits** : `IOPhysicalAddress` fait 32 bits sur PPC 10.4 (à confirmer dans le
   SDK 10.4 avant d'écrire du code qui suppose autre chose).

### 4.4 Boot et cohabitation avec la console Open Firmware

Le device `qfb-pci` **n'est pas** une console de boot : OpenBIOS ne sait pas l'utiliser. Deux étapes :

- **Phase 1–3** : garder `-device VGA` + `qemu_vga.ndrv` comme écran de boot/console, ajouter `qfb-pci` en **second écran**. Tiger affiche alors deux moniteurs ; on valide `qfb-pci` dans *Préférences → Moniteurs*, puis on le passe en écran principal (glisser la barre de menus). Aucun risque de perdre l'affichage si le kext casse.
- **Phase 4+** (optionnel) : écrire un petit **NDRV** pour `qfb-pci` sur le modèle de `QemuMacDrivers`/`qemu_vga.ndrv` (et de la ROM auto-patchée de `nubus-qfb`) pour avoir un affichage dès Open Firmware et supprimer le device VGA. Bonus : le même NDRV sert **Mac OS 9** (`run_os9.sh`), où l'accélération 2D QuickDraw est, elle, documentée par Apple — contrairement au chemin Quartz (§4.6).

### 4.5 Côté hôte : c'est ici que la 4060 Ti travaille

Le chemin existant (`-display dbus,p2p=on` → `QemuBridge` → texture ImGui) reste valable : le scanout `qfb-pci` est une région RAM, donc le fast-path mmap déjà en place s'applique. À ajouter dans `frontend/` :

- upload en `GL_UNSIGNED_INT_8_8_8_8` (ordre BE natif) → **zéro conversion CPU** ;
- upscale au choix (nearest entier / bilinéaire / shader CRT) exécuté sur la 4060 Ti ;
- présentation vsync, indicateur de FPS invité vs FPS hôte (deux compteurs distincts : le nombre de flushes invité ≠ le nombre de présentations) ;
- capture/enregistrement via NVENC (optionnel, `ffmpeg`/`nvenc` sur la texture) ;
- réutiliser la voie D-Bus déjà validée pour le curseur (position/visibilité) afin d'afficher le curseur matériel sans le compositer dans la texture.

### 4.6 Ce que ce kext ne fera pas (à assumer par écrit)

- **Pas de Quartz Extreme / Core Image.** QE exige un pilote 3D + les bundles `…GLDriver`/accélérateur dont l'interface (CoreGraphics/IOAccelerator) est privée et non documentée, sans aucun exemple pour PPC. Hors périmètre — cohérent avec la ligne du README (« pont OpenGL = piège from-scratch, gelé »).
- **Pas d'accélération 2D utilisable par le WindowServer.** Un blitter matériel (`BLIT2D`) ne serait consommé ni par Quartz ni par CoreGraphics sans plugin accélérateur privé. Il servirait à la console noyau et à OS 9 (QuickDraw), pas au bureau Tiger. Ne pas l'implémenter avant d'avoir un besoin mesuré.
- **Pas de multi-écran ni DDC/EDID** en phase 1 (`getConnectionCount()` = 1, `hasDDCConnect()` = false).

---

## 5. Toolchain, build et debug

**Compiler un kext PPC 10.4.** Deux voies :

1. *Historique, sûre* : machine/VM Tiger avec **Xcode 2.5** (gcc 4.0.1), template « IOKit Driver », SDK `MacOSX10.4u`. C'est la voie de moindre surprise pour l'ABI C++ du noyau (le kernel 10.4 est gcc-4.0 ; un kext C++ compilé par un compilateur d'ABI différente échoue au chargement de façon obscure).
2. *Cross moderne* : cctools/ld64 + un gcc PPC ciblant `-arch ppc`, en réutilisant les headers du SDK 10.4u. Faisable (cf. la reconstruction de gcc récents sur Tiger), mais **ne pas** commencer par là : garder l'option 1 comme référence pour trancher « c'est mon code ou mon toolchain ».

Flags typiques (gcc 4.0, kext C++) : `-arch ppc -fapple-kext -fno-exceptions -fno-rtti -fcheck-new -fno-builtin -fno-common -msoft-float -static -nostdinc -nostdlib -lkmod`, includes depuis `Kernel.framework/Headers` du SDK 10.4u. `-mkernel` n'existe pas dans ce compilateur : ne pas le copier depuis une recette moderne.

**Installer / charger.**

```sh
sudo cp -R POMPPCQFB.kext /System/Library/Extensions/
sudo chown -R root:wheel /System/Library/Extensions/POMPPCQFB.kext
sudo chmod -R 755        /System/Library/Extensions/POMPPCQFB.kext
sudo kextload -t /System/Library/Extensions/POMPPCQFB.kext     # -t = valide dépendances/ABI
sudo touch /System/Library/Extensions && sudo kextcache -e -v # régénère Extensions.mkext
```

**Filet de sécurité obligatoire** : un kext graphique fautif = boucle de panic. Travailler **toujours** avec `SNAPSHOT=1 ./run_tiger.sh` pendant le bring-up, et ne promouvoir dans le disque persistant qu'une version chargée deux fois de suite. En cas de panic persistant : boot en single-user (`Cmd-S` → ici, via `-prom-env 'boot-args=-s'`) puis `mv` du kext hors de `/System/Library/Extensions`.

**Debug — l'avantage QEMU.** Deux outils que le matériel réel n'offre pas :

- `IOLog()` → `/var/log/system.log`, et `kprintf()` → port série : lancer avec `-serial mon:stdio` + `-prom-env 'boot-args=-v debug=0x8'` pour voir les traces avant que le système de fichiers soit monté ;
- **gdbstub QEMU** (`-s -S`) : point d'arrêt sur le device et sur le kext (charger les symboles du mach-o à l'adresse rapportée par `kextstat`), plus les traces côté device (`trace-events` QEMU) — on voit les deux côtés du bus dans le même run.

**Vérifications rapides côté invité** :

```sh
kextstat | grep -i pomppc
ioreg -l -w0 | grep -iE "POMPPCQFB|IOFramebuffer|display"    # nub trouvé ? matché ?
ioreg -c IOPCIDevice -l | grep -A5 -i 1234                    # BARs mappés (0x1234:0x0fb1)
```

---

## 6. Plan par phases, critères d'acceptation, charge

| Phase | Contenu | Critère d'acceptation (mesurable) | Charge |
|---|---|---|---|
| **P0 — reconnaissance** | (a) `-device ati-vga,model=rv100` sous Tiger : est-ce que `ATIRadeon*` matche ? (b) inventaire des kexts graphiques réellement présents dans l'image (`ls /System/Library/Extensions | grep -iE "nvda\|ati"`) (c) baseline d'affichage chiffrée : temps de scroll plein écran Finder, redraw 1920x1080, FPS hôte | ioreg/kextstat concluants ou non ; 3 chiffres notés dans `bench/` | **1–2 j** |
| **P1 — « dumb FB » de bout en bout** ✅ device fait | device `qfb-pci` (protocole qfb1 complet : modes, CLUT, gamma, VBL) + kext écrit ; VGA gardé comme écran de boot | Bureau Tiger visible sur le **second écran** ; `kextstat` OK ; aucune corruption de couleurs ; capture d'écran hôte pixel-exacte vs invité | **1–2 sem.** |
| **P2 — modes & profondeurs** | catalogue de modes (option `modes=`), `setDisplayMode` à chaud, 8 bpp + CLUT, 16 bpp, gamma | changement de résolution **sans reboot** depuis Préférences Moniteurs, sur 5 modes dont 1920x1080 ; 256 couleurs correctes | **1 sem.** |
| **P3 — curseur & vblank** | curseur matériel (`convertCursorImage`), IRQ vblank + INTx, dirty-rects explicites (mesurés) | curseur non compositée (prouvé : bouger la souris ne réveille pas le WindowServer — comparer le %CPU invité avant/après) ; dirty-rects gardés seulement si gain > 5 % | **1 sem.** |
| **P4 — intégration frontend / 4060 Ti** | scanout → texture GL, upscale + shaders, vsync, compteurs FPS invité/hôte, curseur via D-Bus, capture optionnelle | 1920x1080 présenté à 60 Hz hôte avec upscale, sans copie CPU supplémentaire ; menu du frontend pour choisir le filtre | **1 sem.** |
| **P5 — optionnels, à ne lancer que sur besoin mesuré** | NDRV `qfb-pci` (boot + OS 9), blitter 2D, multi-écran/EDID | NDRV : affichage dès Open Firmware et OS 9 fonctionnel avec le même device | **2–4 sem.** |

### 6.1 Commandes exactes de la phase P0

`run_tiger.sh` expose le crochet nécessaire : `EXTRA_ARGS` est éclaté en tableau
(`read -r -a USER_EXTRA`) et passé tel quel à QEMU. Rien à retoucher.

```sh
# (a) Tiger voit-il l'ati-vga émulée ? (ajoutée en second écran, disque jetable)
SNAPSHOT=1 EXTRA_ARGS="-device ati-vga,model=rv100" ./run_tiger.sh
#   puis, dans l'invité :
#     ioreg -l -w0 | grep -iE "ATY|ATI|Radeon|IOFramebuffer"
#     kextstat | grep -i ati

# (b) inventaire réel des drivers graphiques de l'image Tiger (dans l'invité)
ls /System/Library/Extensions | grep -iE "nvda|ati|IOGraphics|IONDRV"

# (c) baseline chiffrée, protocole déterministe déjà validé sur ce projet
SNAPSHOT=1 ./run_tiger.sh            # pas de fsck, chrono comparable (réseau déjà off par défaut)
#   pour un chiffre exploitable, préférer l'outil de mesure :
EXTRA_ARGS="-snapshot" scripts/measure-boot.sh   # -> bench/last-boot.txt
```

Total réaliste pour un affichage paravirtuel propre et exploité par la 4060 Ti : **4 à 6 semaines** de travail effectif, P1–P4.

---

## 7. Risques et pièges connus

1. **`IONDRVFramebuffer` vole le device** → d'où la classe PCI `0x038000` et pas VGA ; vérifier dans `ioreg` que le nub `qfb-pci` est matché par `POMPPCQFB` et non par `IONDRVFramebuffer`.
2. **Ordre des composantes / stride** : les deux sources de « bureau en diagonale » ou « rouge et bleu inversés ». Tester avec une mire générée côté hôte (barres RGB) avant de croire à un bug de mode.
3. **Cache** : la VRAM est de la RAM hôte ; si des zones restent figées, tester un mapping non-caché pour l'ouverture (au prix des perfs) afin d'isoler le problème.
4. **10.4 et les modes ajoutés à chaud** : le WindowServer n'aime pas voir le catalogue changer sous lui. Publier un catalogue **fixe** à `enableController()`, et n'utiliser `IOFramebuffer::connectChangeInterrupt` que si un besoin réel apparaît (P5).
5. **ABI C++ du kext** (gcc 4.0 vs autre) : symptôme = `kextload` réussit mais `start()` ne s'exécute jamais, ou panic à la première méthode virtuelle. D'où la voie Xcode 2.5 en référence.
6. **IRQ en TCG** : une IRQ vblank à 60 Hz coûte cher sous TCG mono-cœur ; la rendre désactivable dès le début (`vblank=off`) et la mesurer.
7. **SMP** : le kext doit être propre en verrouillage (`IOSimpleLock` autour des registres) — le projet tourne à 2 cœurs MTTCG par défaut.
8. **Ne pas déclarer `kIOFBHardwareCursorAttribute`** avant que le curseur matériel fonctionne réellement : sinon curseur invisible, régression difficile à diagnostiquer.
9. **Régression silencieuse du chrono de boot** : garder le protocole déjà établi (`EXTRA_ARGS="-snapshot" scripts/measure-boot.sh`, hôte au repos, runs interleavés) pour comparer les temps de boot avant/après ajout du device.

---

## 8. Décisions à prendre avant d'écrire du code

1. **Objectif réel** : qualité/souplesse d'affichage (résolutions, upscale 4060 Ti, curseur) — ou performance du bureau ? Si c'est la performance, ce document dit que l'effort utile est dans le JIT, pas ici.
2. **Patcher QEMU (B1) ou rester sur l'amont (B2/virtio-gpu)** ? B1 est plus simple côté kext et déjà validé par la communauté via `nubus-qfb` ; B2 évite un patch à maintenir. Recommandation : B1, avec le device isolé dans `patches/qfb/` comme les patches existants.
3. **OS 9 dans le périmètre ?** Si oui, prévoir le NDRV (P5) dès la conception des registres — la ROM `nubus-qfb` montre qu'un seul device peut servir les deux OS.

---

## 9. Sources

- QEMU `uninorth` (fenêtres PCI mac99, config space, endianness) : [hw/pci-host/uninorth.c](https://raw.githubusercontent.com/qemu/qemu/master/hw/pci-host/uninorth.c)
- `IOFramebuffer` — API et rôle de `IONDRVFramebuffer` : [IOFramebuffer.h (apple-oss-distributions/IOGraphics)](https://github.com/apple-oss-distributions/IOGraphics/blob/main/IOGraphicsFamily/IOKit/graphics/IOFramebuffer.h), [IOFramebuffer — Apple Developer](https://developer.apple.com/documentation/kernel/ioframebuffer?language=objc), [IONDRVFramebuffer.cpp](https://github.com/mattl/opensource.apple.com/blob/master/src/IOGraphics/IOGraphics-123/IONDRVSupport/IONDRVFramebuffer.cpp)
- Retour d'expérience « écrire un framebuffer kext pour une carte PC sur PPC » (endianness, ROM, absence d'accélération, toolchain) : [Radeon HD cards on PowerPC Leopard — osx86-driver-radeonhd](https://forums.macrumors.com/threads/radeon-hd-cards-on-powerpc-leopard-using-osx86-driver-radeonhd-framebuffer-driver.2344940/)
- Passthrough réel vers un invité `mac99` Tiger (Rage 128 Mac Edition, `rombar/romfile`, OpenBIOS patché, pas d'accélération) : [Qemu-system-ppc VGA passthrough](https://forums.macrumors.com/threads/qemu-system-ppc-vga-passthrough.2229861/)
- Émulation `ati-vga` (RV100/Rage 128 Pro) : état, 2D partielle, « MacOS ne trouve pas la carte » : [ATI VGA Emulation — Qmiga Wiki](https://osdn.net/projects/qmiga/wiki/SubprojectAti), [patch d'origine](https://lists.gnu.org/archive/html/qemu-devel/2019-02/msg02471.html)
- Framebuffer paravirtuel `nubus-qfb` (modèle à copier) : [SolraBizna/mac_qfb_driver](https://github.com/SolraBizna/mac_qfb_driver), [annonce/discussion](https://www.emaculation.com/forum/viewtopic.php?p=74771)
- Drivers paravirtuels PPC/68k existants et retrait du virtio-gpu : [elliotnunn/classicvirtio](https://github.com/elliotnunn/classicvirtio), [Qemu virtio drivers for Mac OS 9.x](https://www.emaculation.com/forum/viewtopic.php?t=11973)
- NDRV QEMU pour Mac OS/OS X : [QemuMacDrivers](https://github.com/qemu/QemuMacDrivers), [intégration `qemu_vga.ndrv`](https://lists.gnu.org/archive/html/qemu-devel/2017-05/msg00039.html)
- NVIDIA : BARs et switch d'endianness MMIO : [envytools — PCI BARs](https://envytools.readthedocs.io/en/latest/hw/bus/bars.html), [envytools — PMC](https://github.com/envytools/envytools/blob/master/docs/hw/bus/pmc.rst) ; fin du support big-endian après Pascal : [« gpu: nova-core: require little endian »](https://lkml.iu.edu/2604.0/10620.html)
- Ada = chemin GSP obligatoire (booter signé, FWSEC, GSP-RM) : [nouveau — initial support for GSP-RM 535.54.04 (and Ada GPUs)](https://lists.freedesktop.org/archives/nouveau/2023-September/043193.html), [Phoronix](https://www.phoronix.com/news/Nouveau-Patches-Run-On-GSP-Blob), [état du support GSP (airlied)](https://airlied.blogspot.com/2023/11/nouveau-gsp-firmware-support-current.html), [Falcon — doc noyau](https://docs.kernel.org/gpu/nova/core/falcon.html)
- Plafond driver NVIDIA sur PPC (NV4x/G70, Quadro FX 4500 flashée) : [Most Powerful GPU for Quad G5 (68kMLA)](https://68kmla.org/bb/threads/most-powerful-gpu-for-quad-g5.5980/)
- Toolchain PPC/Tiger : [GCC sur Tiger PPC (B. Callahan)](https://briancallahan.net/blog/20250329.html), [Developing for Tiger and Leopard / PowerPC](https://forums.macrumors.com/threads/developing-for-tiger-and-leopard-powerpc-a-devlog.2383838/)
