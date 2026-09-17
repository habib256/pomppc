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
  - `IOConnectMapMemory(type 0)` → la tranche du client (16 Mio sur 64) ;
  - `QGPU_UC_GET_INFO` → version, caps, taille de tranche, fence ;
  - `QGPU_UC_GET_SLOT` → index, base de la tranche dans BAR0, premiers ids ;
  - `QGPU_UC_SUBMIT(off, len)` (relatif à la tranche) → fence, statut, index fautif ;
  - `QGPU_UC_WAIT_FENCE(fence, ms)` ; `QGPU_UC_RESET` (objets du client) ;
- détruit les objets d'un client qui se ferme ou meurt ;
- sert l'interruption `DONE` (filtre + acquittement) ; l'exécution côté QEMU étant
  synchrone, `SUBMIT` renvoie déjà le résultat.

Le kext ne connaît des opcodes que ceux de destruction (nettoyage d'un client) : le flux
est produit en userland (`guest/gldriver`, le plugin OpenGL ; `guest/qgpu-test`) et
exécuté par l'hôte.

## Compiler et charger (dans l'invité)

Normalement par `guest/gldriver/install.sh`, qui installe kext et plugin. À la main :

```sh
cd /pomppc/POMPPCGPU        # ou le CD produit par scripts/make_kext_iso.sh
make                        # gcc-4.0 + SDK MacOSX10.4u (Xcode Tools du DVD Tiger)
sudo make load              # kextload -t : valide dépendances et ABI avant tout
kextstat | grep POMPPCGPU ; ioreg -c POMPPCGPU -r
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

Vérifié dans Tiger 10.4.6 (protocole v4) : chargé au démarrage depuis
`/System/Library/Extensions`, `qgpu_test` vert, quatre applications GL accélérées en
parallèle, un cinquième client refusé proprement, objets détruits à la fermeture.
Détails : `docs/gpu-3d-tiger.md` §5.
