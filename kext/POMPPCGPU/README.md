# POMPPCGPU.kext — transport du GPU paravirtuel « qgpu » pour Tiger PPC

Pilote IOKit (Mac OS X 10.4, PowerPC, gcc 4.0) du device QEMU `qgpu-pci`
(`patches/qgpu/qgpu-pci.c`). Conception complète, état et plan :
`docs/gpu-3d-tiger.md`. Contrat hôte/invité : `qgpu_proto.h` (copie identique de
`patches/qgpu/qgpu_proto.h`, vérifiée par `tests/run-all.sh`).

## Ce que fait le kext (et rien d'autre)

- matche le device PCI `1234:0fb2` (classe coprocesseur : aucun pilote graphique
  d'Apple ne le revendique) ;
- vérifie la signature `'qgp1'` et la version du protocole, publie dans `ioreg`
  `QGPUVersion`, `QGPUCaps`, `QGPUShmemSize`, `QGPUBackend` ;
- expose un `IOUserClient`, jusqu'à 4 clients simultanés (une application
  OpenGL = un client), chacun avec sa tranche de fenêtre et sa plage
  d'identifiants d'objets (`qgpu_proto.h`, « Interface du kext ») :
  - `IOConnectMapMemory(type 0)` → la tranche du client (un quart de la fenêtre,
    moins la page de service : ≈ 16 Mio sur 64, à lire dans `QGPU_UC_GET_INFO`) ;
  - `QGPU_UC_GET_INFO` → version, caps, taille de tranche, fence ;
  - `QGPU_UC_GET_SLOT` → index, base de la tranche dans BAR0, premiers ids ;
  - `QGPU_UC_SUBMIT(off, len)` (relatif à la tranche) → fence, statut, index fautif ;
  - `QGPU_UC_WAIT_FENCE(fence, ms)` ; `QGPU_UC_RESET` (objets du client) ;
- détruit les objets d'un client qui se ferme ou meurt ;
- sert l'interruption `DONE` (filtre + acquittement), et s'en sert pour dormir
  pendant une attente de barrière.

## 4.2 : l'accélérateur publié

Un vrai pilote de carte se fait connaître d'OpenGL.framework par deux propriétés que **son kext
pose sur le framebuffer** : `IOAccelTypes` (chemin d'un objet de classe `IOAccelerator`) et
`IOAccelIndex`. CGL retrouve l'accélérateur de chaque écran (`IOAccelFindAccelerator`), et
GLEngine charge le bundle nommé par son `IOGLBundleName` depuis `/System/Library/Extensions`,
avant les `GLDriver*` d'OpenGL.framework (`docs/re/accelerateur-iokit.md`).

Le kext fait de même :

- il publie un **nub enfant** `POMPPCAccelerator` (classe `IOAccelerator`, d'où la dépendance à
  `com.apple.iokit.IOGraphicsFamily`) portant `IOGLBundleName = GLDriver-POMPPC`. Pas
  `POMPPCGPU` lui-même : ses clients l'ouvrent en type 0, qui est aussi
  `kIOAccelSurfaceClientType`, le type qu'ouvriraient CGL (pbuffers) et le WindowServer. Le nub
  **refuse toute ouverture** tant qu'il n'y a pas de surfaces (Quartz Extreme, tâche 4.4) ;
- il pose `IOAccelTypes`/`IOAccelIndex` sur chaque `IOFramebuffer` publié (notification, donc
  aussi en chargement à chaud), sauf sur un framebuffer qui désigne déjà un autre accélérateur,
  et les retire au déchargement ;
- il ne publie **pas** `AccelCaps` : le WindowServer tenterait alors Quartz Extreme.

`ioreg -c POMPPCAccelerator` montre le nub ; `guest/gltest/accelprobe` montre ce qu'en voit CGL.
En single-user il n'y a aucun framebuffer (IONDRVSupport n'est chargé que par `kextd`) : rien à
lier, c'est normal.

## v9 : le doorbell asynchrone

Le device exécute désormais les soumissions sur un **thread de rendu** et les met
en **file** (`docs/protocole-v9-asynchrone.md`). Le kext sait poser les deux
doorbells, **par soumission**.

**Le drapeau voyage dans les bits hauts de `len`.** `qgpu_proto.h` fige
`QGPU_UC_METHOD_COUNT` et les sélecteurs : pas de méthode nouvelle. Et pas
d'argument nouveau non plus — l'ABI de Darwin 8 compare le **nombre**
d'arguments scalaires au bit près (`is_io_connect_method_scalarI_scalarO`
refuse dès que `inputCount ≠ IOExternalMethod::count0`), donc passer `count0`
de 2 à 3 ferait rendre `kIOReturnBadArgument` à **tous** les appelants
existants. `len` est un multiple de 4 borné par la tranche : ses trois bits
hauts sont libres, et un appelant qui ne les connaît pas les laisse à zéro,
c'est-à-dire le comportement v8 exact. **Les deux formes d'appel sont donc le
même appel** — `guest/qgpu-test` le vérifie en mêlant les deux dans la même
session. Les macros sont dans `POMPPCGPU.h`, copie identique dans
`guest/gldriver/pomppc_qgpu.h` :

| drapeau dans `len` | effet | sorties |
|---|---|---|
| — | doorbell 1, synchrone (v1–v8, inchangé) | fence, statut du rendu, `pc` fautif |
| `POMPPC_SUB_ASYNC` | doorbell 3 : mise en file, retour immédiat | **barrière de cette soumission**, **acceptation** (`OK` / `QUEUE_FULL`), **`QGPU_REG_ERRORS`** |
| `POMPPC_SUB_PEEK` | ne soumet rien | `ERRORS`, `STATUS`, `STATUS_PC` |
| `POMPPC_SUB_QUEUE` | ne soumet rien | en vol, places libres, profondeur |

Le kext ne pose le drapeau que si le device annonce `QGPU_CAP_ASYNC`
(`ioreg` : `QGPUAsync`, `QGPUQueueDepth`). Sur `QGPU_ST_QUEUE_FULL` il
**répond** au lieu de boucler : attendre une place dans la command gate
bloquerait les trois autres clients et le gestionnaire d'interruption avec eux.

**`QGPU_UC_WAIT_FENCE` ne scrute plus.** Il dort sur la command gate
(`commandSleep`) et `irqAction` le réveille. Deux pièges :
l'`IRQ DONE` se **coalesce**, donc on relit `FENCE` à chaque réveil au lieu de
compter les interruptions ; et Tiger **n'a pas** `commandSleep(event, deadline,
…)`, arrivé en 10.5 — un `IOTimerEventSource` bat donc toutes les 10 ms tant
qu'il y a des dormeurs, ce qui donne la base de temps du délai maximal **et** un
réveil de secours si une interruption se perd. Dormir sans réveil garanti dans
un kext, c'est figer la VM, et `-x` n'est pas disponible pour la dépanner.

**Fermeture d'un client.** `destroyClientObjects` soumet les destructions par
**paquets asynchrones** (profondeur de la file du device), séparés par une
barrière qui **dort sur la command gate** : le verrou du work loop est relâché
pendant l'attente, les autres clients continuent, et le BQL de QEMU n'est plus
tenu 212 fois de suite (le bureau se figeait à chaque sortie d'application 3D).
La barrière finale remplace le drainage actif d'avant : la file est FIFO, donc
attendre la dernière destruction, c'est attendre aussi toutes les soumissions
en vol qui lisaient la tranche du client — rendue juste après. Sur un device
sans `QGPU_CAP_ASYNC`, on garde le drainage puis le doorbell synchrone.

Le flux de destruction est écrit dans une **page de service** rognée sur la
fenêtre partagée, hors de toute tranche (chaque tranche y a son quart) : la
tranche du client, elle, peut être encore mappée et vivante (`clientDied`
pendant un dessin, `QGPU_UC_RESET`).

Une tranche n'est rendue que **par son propriétaire** (`fClients[slot] ==
client`, vérifié dans la gate), et `stop()` du user client se contente d'une
comptabilité silencieuse : entrer dans la gate depuis le fil de terminaison,
c'est une panic (la gate a pu être retirée du work loop, et `runAction` la
ferme avant tout test).

Le kext ne connaît des opcodes que ceux de destruction (nettoyage d'un client) : le flux
est produit en userland (`guest/gldriver`, le plugin OpenGL ; `guest/qgpu-test`) et
exécuté par l'hôte.

## Compiler et charger (dans l'invité)

Normalement par `guest/gldriver/install.sh`, qui installe kext et plugin. À la main :

```sh
cd /pomppc/POMPPCGPU        # ou le CD produit par scripts/make_kext_iso.sh
make                        # gcc-4.0 + SDK MacOSX10.4u (Xcode Tools du DVD Tiger)
sudo make load              # kextload -t : valide dépendances et ABI avant tout
kextstat | grep POMPPCGPU ; ioreg -c POMPPCAccelerator -w 0   # (ni -r ni -d sous Tiger)
cd ../qgpu-test && make && ./qgpu_test     # scène de référence, pixels vérifiés, out.ppm
```

`go.sh` fait la même chose depuis un CD en single-user, et `guest/verify.sh` enchaîne
tout avec un verdict dans `/pomppc/result.txt` (piloté depuis l'hôte par
`scripts/verify-kext-in-guest.py`).

## Notes ABI Darwin 8

- Méthodes du user client via `IOExternalMethod` / `getTargetAndMethodForIndex`
  (l'`externalMethod()` moderne date de 10.5) ; scalaires 32 bits.
- `clientMemoryForType` doit **retenir** le descripteur : `IOUserClient::mapClientMemory`
  le relâche après le mapping.
- `IOPCIPrimaryMatch` = `0xDDDDVVVV` (device en poids fort) : `0x0fb21234`.
- Compilé et lié par gcc 4.0 uniquement (ABI C++ du noyau 10.4).
- Chaîne de build PPC, quatre pièges rencontrés et corrigés dans le `Makefile` :
  pas de `-nostdlib` pour `ld` ; `libcc_kext.a` via `gcc -print-file-name` (hors
  `-syslibroot`) ; `POMPPCGPU_info.c` fournit `kmod_info` (sinon « doesn't contain
  kernel extension code ») ; **`-mlong-branch`** (sinon « relocation overflow »).

## État

Vérifié dans Tiger 10.4.6 (protocole v9) : chargé au démarrage depuis
`/System/Library/Extensions`, `qgpu_test` vert (y compris la section v9 :
soumission asynchrone, barrière, relecture visible seulement après elle, rafale
de 64 dont 3 refusées file pleine, soumission fautive comptée dans `ERRORS`,
puis un `SUBMIT` **synchrone** qui marche toujours), quatre applications GL
accélérées en parallèle, un cinquième client refusé proprement, objets détruits
à la fermeture. Détails : `docs/gpu-3d-tiger.md` §5.
