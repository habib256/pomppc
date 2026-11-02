# Publier le paquet macOS arm64

Recette de la 0.4 (02/11/2026). Le paquet est le commit : arbre propre exigé, tag posé sur ce commit.

## 1. Le macOS minimal

Le paquet embarque ses bibliothèques et n'a pas besoin de Homebrew à l'exécution, mais le macOS
exigé se lit dans chaque binaire : copiées telles quelles, les bouteilles Homebrew exigent le macOS
de la machine. `MIN_MACOS` (15.0 pour la 0.4) fixe donc la cible, et
`package_release_macos.sh` refuse tout binaire au-dessus.

```bash
vtool -show-build <binaire> | grep minos       # ce qu'un binaire déclare
scripts/build_release_deps_macos.sh            # ~/src/pomppc-deps : les 17 formules, aux versions de Homebrew
```

Le script refuse aussi une bibliothèque liée à Homebrew ou à `@rpath`. Une formule ajoutée à
QEMU ou au frontal s'ajoute à sa liste `FORMULES`, dans l'ordre des dépendances.

## 2. Construire, hors des arbres quotidiens

```bash
D=~/src/pomppc-deps; PC=""; for d in "$D"/*/*/lib; do PC="$d/pkgconfig:$PC"; done
GT=$(ls -d "$D"/gettext/*/lib)
export PKG_CONFIG_PATH="$PC" MACOSX_DEPLOYMENT_TARGET=15.0 LDFLAGS="-L$GT"
QEMU_SRC=~/src/qemu-rel04 PYTHON=~/src/qemu-venv/bin/python3 \
  CONFIGURE_EXTRA="--disable-sdl --extra-ldflags=-L$GT" scripts/build_qemu_qfb.sh
cmake -S frontend -B ~/src/pomppc-frontend-rel04 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_PREFIX_PATH="$(ls -d "$D"/glfw/*)"
cmake --build ~/src/pomppc-frontend-rel04 -j
```

La cible ne se rattrape pas sur un arbre déjà construit : effacer `build/` en la changeant. Le
script de QEMU écrase `bench/build-capabilities.txt` : le sauvegarder puis le remettre.
`~/src/qemu/build` reste le binaire de la VM quotidienne.

## 3. Binaires de l'invité

Compilés dans la VM quotidienne par ssh, depuis `git archive HEAD`, dans `~/pomppc-rel<version>` :
`POMPPCGPU.kext`, `POMPPCFsqrt.kext`, `GLDriver-POMPPC.bundle`, `POMPPCGA.plugin`, `gltest`, `glwin`,
`accelprobe`, `Install POMPPC.app` (`osacompile`, job `prebuilt`), plus un fichier `VERSION`. `disks/prebuilt` ne suffit pas : il n'a pas `POMPPCFsqrt.kext`.

## 4. Empaqueter

```bash
PREBUILT=bench/release-0.4/prebuilt FRONTEND=~/src/pomppc-frontend-rel04/pomppc \
  QEMU_SRC=~/src/qemu-rel04 scripts/package_release_macos.sh 0.4
```

Sortie : `dist/pomppc-0.4-macos-arm64.tar.xz` et son `.sha256`. Puis l'app, tirée de ce paquet :

```bash
scripts/package_app_macos.sh 0.4
```

Sortie : `dist/POMPPC.app`, `dist/POMPPC-0.4-macos-arm64.dmg` et son `.sha256`. Pas de certificat
Apple Developer ici : signature ad hoc, sans notarisation, d'où l'accord demandé au premier lancement.
L'icône se refait par `scripts/app-macos/icone.swift`.

L'assistant de premier lancement (`frontend/src/SetupFlow.*`) ouvre les pages de Macintosh Garden et attend le
fichier dans Téléchargements : il ne télécharge pas le système lui-même. Mac OS reste sous droit d'auteur
d'Apple, et le dépôt continue de ne rien distribuer d'Apple : le geste reste celui de l'utilisateur. Si le
site change ses fichiers, mettre à jour noms et MD5 en tête de `SetupFlow.cpp`
(`https://macintoshgarden.org/arch_md5.php?filename=<fichier>`). Le CD seul, `pomppc-guest.iso`, se
publie à part, renommé `pomppc-0.4-guest.iso`.

## 5. Éprouver l'archive

Sur un chemin court, avec un clone du disque, pour ne pas toucher à la VM quotidienne :

```bash
tar xJf dist/pomppc-0.4-macos-arm64.tar.xz -C ~/ && cd ~/pomppc-0.4-macos-arm64
cp -c <dépôt>/disks/tiger.qcow2 disks/tiger.qcow2
SNAPSHOT=1 SSH_FWD=2240 ./run_tiger.sh
```

Dans l'invité : `install.sh` du CD, redémarrage, étape 6 du README, `setmode` (`guest/gltest/`) à
1024×768, 1920×1080 et 2560×1080, puis un jeu. Vérifier que `lsof -p <QEMU>` ne montre que `lib/` du paquet, et si possible essayer
sur le macOS minimal réel. Vérifier aussi `x-tb-fast` au lancement : le paquet
v0.1.0 l'avait perdu en silence (`CHANGELOG.md`, 0.3).

## 6. Publier

Tag sur le commit du paquet, puis `gh release create <version> --notes-file docs/notes-version-<version>.md`
avec l'archive, le CD et leurs sommes. Publier reste une décision de l'utilisateur.
