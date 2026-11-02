# Publier le paquet macOS arm64

Recette de la 0.4 (02/11/2026). Le paquet est le commit : arbre propre exigé, tag posé sur ce commit.

## 1. Choisir le macOS minimal

Le macOS exigé se lit dans les binaires et monte tout seul quand l'hôte change : les bouteilles
Homebrew visent le macOS de la machine, le compilateur aussi. `MIN_MACOS` (26.0 pour la 0.4) fixe
la cible, et `package_release_macos.sh` refuse tout binaire au-dessus.

```bash
vtool -show-build <binaire> | grep minos       # ce qu'un binaire déclare
scripts/build_release_deps_macos.sh            # ~/src/pomppc-deps : pcre2, libpng, libunistring, glib
```

Les formules à recompiler sont celles que le refus de l'empaquetage nomme (`FORMULES="…"`).
Le script reprend les versions et sommes de Homebrew. Les binaires compilés contre les bouteilles
gardent la même ABI, et l'empaquetage remplace chaque bibliothèque par sa version recompilée.

## 2. Construire, hors des arbres quotidiens

```bash
export MACOSX_DEPLOYMENT_TARGET=26.0
QEMU_SRC=~/src/qemu-rel04 PYTHON=~/src/qemu-venv/bin/python3 CONFIGURE_EXTRA=--disable-sdl \
  scripts/build_qemu_qfb.sh
cmake -S frontend -B ~/src/pomppc-frontend-rel04 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0
cmake --build ~/src/pomppc-frontend-rel04 -j
```

Le script de QEMU écrase `bench/build-capabilities.txt` : le sauvegarder puis le remettre.
`~/src/qemu/build` reste le binaire de la VM quotidienne.

## 3. Binaires de l'invité

Compilés dans la VM quotidienne par ssh, depuis `git archive HEAD`, dans `~/pomppc-rel<version>` :
`POMPPCGPU.kext`, `POMPPCFsqrt.kext`, `GLDriver-POMPPC.bundle`, `POMPPCGA.plugin`, `gltest`, `glwin`,
`accelprobe`, plus un fichier `VERSION`. `disks/prebuilt` ne suffit pas : il n'a pas `POMPPCFsqrt.kext`.

## 4. Empaqueter

```bash
PREBUILT=bench/release-0.4/prebuilt FRONTEND=~/src/pomppc-frontend-rel04/pomppc \
  QEMU_SRC=~/src/qemu-rel04 scripts/package_release_macos.sh 0.4
```

Sortie : `dist/pomppc-0.4-macos-arm64.tar.xz` et son `.sha256`. Le CD seul, `pomppc-guest.iso`, se
publie à part, renommé `pomppc-0.4-guest.iso`.

## 5. Éprouver l'archive

Sur un chemin court, avec un clone du disque, pour ne pas toucher à la VM quotidienne :

```bash
tar xJf dist/pomppc-0.4-macos-arm64.tar.xz -C ~/ && cd ~/pomppc-0.4-macos-arm64
cp -c <dépôt>/disks/tiger.qcow2 disks/tiger.qcow2
SNAPSHOT=1 SSH_FWD=2240 ./run_tiger.sh
```

Dans l'invité : `install.sh` du CD, redémarrage, étape 6 du README, `setmode` (`guest/gltest/`) à
1024×768, 1920×1080 et 2560×1080, puis un jeu. Vérifier aussi `x-tb-fast` au lancement : le paquet
v0.1.0 l'avait perdu en silence (`CHANGELOG.md`, 0.3).

## 6. Publier

Tag sur le commit du paquet, puis `gh release create <version> --notes-file docs/notes-version-<version>.md`
avec l'archive, le CD et leurs sommes. Publier reste une décision de l'utilisateur.
