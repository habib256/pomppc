# Publier l'AppImage Linux x86-64

Écrit le 03/11/2026 pour la 0.4.2. **Rien de ceci n'a encore tourné** : lot écrit pendant une campagne
de mesure, sans aucune compilation ; le premier passage de la CI est sa première épreuve (§6). Publier reste
une décision de l'utilisateur : poser le tag est le seul geste qui publie.

Le macOS ne passe pas par ici (`docs/publier-macos.md`, à la main sur le Mac), mais dépose ses paquets sur
la même Release. Le paquet sous forme d'archive, `scripts/package_release_linux.sh`, reste à sa place et n'est
pas publié : il vise la distribution qui le construit.

## 1. Pourquoi cette image

Une AppImage n'embarque jamais la glibc : **son plancher est celui de la machine de construction**.

| | exigé par QEMU 11.1.2 | bionic (les autres Pommes) | bullseye |
|---|---|---|---|
| gcc | ≥ 10.4 (`meson.build:330`) | 7 | 10.2, **d'où gcc 11.5 compilé dans l'image** |
| glib | ≥ 2.66 (`meson.build:1058`) | 2.56 | 2.66.8 |
| python | ≥ 3.9 (`configure:508`) | 3.6 | 3.9.2 |
| glibc | | 2.27 | **2.31** |

D'où le plancher : **glibc 2.31**, soit Debian 11, Ubuntu 20.04, Fedora 32 et suivantes. `check_appimage.sh`
l'exige de chaque binaire et de chaque bibliothèque du paquet.

Bullseye est sortie du support LTS fin août 2026 : ses paquets ne sont plus que sur `archive.debian.org`,
d'où le Dockerfile les prend (constaté le 04/11/2026). L'image se cuit une fois et ne retouche plus apt.

## 2. L'image, à faire une fois

1. Pousser le dépôt, lancer le workflow **Image de construction Linux** (`builder-image.yml`, à la main, compter
   deux à trois heures : gcc s'y compile). Il pousse `ghcr.io/habib256/pomppc-bullseye-builder`.
2. Lire le digest qu'il imprime, le coller dans `release.yml`, job `linux`, `BUILDER_IMAGE`. Tant que c'est vide,
   le job s'arrête à sa première étape avec ce message, la répétition du lundi aussi.
3. Lancer `release.yml` à la main (« Run workflow ») : c'est une répétition, rien n'est publié.

Le paquet GHCR est lié au dépôt par son premier `docker push` depuis un workflow : `packages: read` suffit ensuite.

## 3. Le CD invité, à chaque version

La CI ne peut pas le produire : ses binaires se compilent dans Tiger (`docs/publier-macos.md` §3), et `disks/prebuilt`
est ignoré par git. Un paquet publié ne doit pas porter les pilotes de la version précédente, et un tag refuse donc un
`packaging/guest-iso.lock` d'une autre version. Une répétition accepte le CD déclaré (0.4.1 à ce jour).

```bash
scripts/make_guest_iso.sh pomppc-0.4.2-guest.iso      # depuis les binaires compilés dans l'invité
sha256sum pomppc-0.4.2-guest.iso
# Une URL publique avant le tag : une pré-publication à part, que les motifs de tag ne déclenchent pas.
gh release create guest-iso-0.4.2 --prerelease --title "Guest CD 0.4.2" --notes "CI input." pomppc-0.4.2-guest.iso
```

Puis refaire `version`, `url` et `sha256` dans `packaging/guest-iso.lock`, et commiter **avant** le tag.

## 4. Publier

```bash
# frontend/CMakeLists.txt, docs/notes-version-<v>.md et CHANGELOG.md disent <v> (run-all le vérifie)
git tag 0.4.2 && git push origin 0.4.2
```

`release.yml` : `version` (le tag est la version), `quality` (`tests/run-all.sh`), `linux`, puis `publish`. Celui-ci
crée la Release ou la réutilise, dépose l'AppImage et `SHA256SUMS-ci.txt` et ne touche à rien d'autre : les
paquets du Mac, déposés avant ou après, restent. Les notes viennent de `docs/notes-version-<v>.md` à la révision du tag.

## 5. Ce que la CI ne prouve pas, à faire à la main avant de publier

La CI prouve la **forme** (`check_appimage.sh`) et que **ça se charge et démarre** (`smoke_appimage.sh` : Open Firmware
en SMP, QMP, capacités). Ses machines n'ont ni disque Tiger ni GPU. Restent, sur le PC, depuis l'AppImage et non le
dépôt, en déplaçant `~/POMPPC` ou avec `POMPPC_DATA=` :

- Tiger démarre, le pilote s'installe depuis le CD `POMPPC_GUEST`, un jeu tourne, avec **`qgpu` sur le vrai GPU** :
  `qgpu-pci: backend gl`, et ce n'est pas llvmpipe (`docs/etat-des-hotes.md`, `EGL_PLATFORM`).
- L'**assistant complet** sous Linux : `md5sum`, `zenity`, `xdg-open`, et l'étape du combo, dont la conversion par
  `qemu-img -f dmg` n'a jamais tourné (`SetupFlow::convertDmg`, grade F).
- Le son par PulseAudio ou PipeWire, la fenêtre sous X11 **et** sous Wayland (GLFW 3.4 choisit au lancement).
- **Un hôte ancien et un hôte sans NVIDIA** : le plancher n'est prouvé que par les symboles. Essayer sur un vrai
  Debian 11 ou Ubuntu 20.04, et si possible AMD ou Intel.
- La vitesse : ce binaire est générique, sans `-march=native` ni PGO, et la colonne PC des notes vient du binaire
  rapide. Mesurer pour pouvoir dire combien. Les notes annoncent « environ 10 % » d'après `README.md`.

## 6. Ce qu'il y a dedans

```
AppRun, version, pomppc.desktop, pomppc.svg
usr/share/pomppc/    le dépôt au commit du tag (git archive), plus :
  bin/               qemu-system-ppc, qemu-system-ppc64, qemu-img, pomppc     RUNPATH $ORIGIN/../lib
  lib/               glib, gio, gobject, gmodule, pixman, libslirp, libpulse et ses dépendances,
                     libfdt, libpng, libusb, zlib, bzip2                      RUNPATH $ORIGIN
  share/qemu/        firmwares de QEMU, sans edk2
  pomppc-guest.iso, THIRD-PARTY-LICENSES/
```

**Jamais embarqué** (`EXCLU`, `build_appimage.sh`, revérifié par `check_appimage.sh`) : libGL, libEGL, libGLX,
libOpenGL, libGLdispatch, gbm, drm, la glibc, libstdc++ (statique), libgcc_s (statique), wayland, xkbcommon et les
clients X, dont libxcb que libpulse demande.

- **Pas de `LD_LIBRARY_PATH`** : les enfants (bash, python3, curl) et le Mesa de l'hôte en hériteraient. Le chargeur
  résout `$ORIGIN` par `/proc/self/exe`, donc à travers le lien `~/POMPPC/bin` aussi ; le firmware pareil.
- **Pas de `EGL_PLATFORM` dans AppRun** : `run_tiger.sh` le pose pour QEMU. Le frontal, lancé sans passer par
  `run_tiger.sh`, garde la plateforme de sa session.
- **`~/POMPPC`** a la forme du dépôt parce que tous les scripts déduisent leur racine de leur emplacement, et
  `main.cpp` en déduit `.run/` : données réelles, lanceurs copiés, le reste en liens refaits à chaque lancement,
  puisque le montage change. Un seul lancement à la fois (`.run/app.lock`, tenu jusqu'à la fin du frontal) : une
  seconde instance referait les liens vers son montage, qui mourrait à sa sortie. Les verrous ne se suppriment jamais.
- **Profil `QEMU_PROFIL=paquet-linux`** de `scripts/build_qemu_qfb.sh` : ni GTK, ni SDL, ni epoxy, `pa` seul,
  `-display dbus` et toute la série exigés, QEMU -O2 générique. Sans lui, le script reste celui de tous les jours.
  `--disable-gbm`, prévu à l'origine, n'existe pas : `--disable-opengl` retire déjà gbm (`meson.build:1781-1784`).
- **Pas de fenêtre native** : l'AppImage n'a que le frontal, `POMPPC_FRONTEND=native` n'y a aucun affichage.

## 7. Risques connus, à regarder au premier essai

- **Même SONAME, deux versions.** Le chargeur ne charge qu'une fois un SONAME : le pilote GL de l'hôte reçoit
  donc notre `libz`, plus vieille que la sienne, et s'il demande une version de symboles plus récente (`ZLIB_1.2.12`…),
  l'initialisation d'EGL échoue. Remède si cela arrive : ajouter le nom à `EXCLU`. C'est pour cela que X et zstd sont hors
  du paquet ; `libz` reste dedans sur décision, parce qu'elle manque rarement et casse rarement.
- **Les bibliothèques de Debian 11 sont gelées.** `libslirp` 4.4, `libpulse` 14.2 : pas de correctifs de sécurité
  sans reconstruction de l'image. Le réseau de l'invité passe par slirp.
- **QEMU sans `opengl`** : la 3D passe par `qgpu`, qui n'en dépend pas, et le frontal ne traite que `Scanout` et
  `ScanoutMap`, pas le dma-buf : rien à perdre sur le papier, jamais essayé (`QemuBridge.cpp`).
- **Code du frontal écrit à l'aveugle** : portage de l'assistant (`SetupFlow`) relu, jamais compilé ni lancé à ce jour.
  Le frontal doit aussi compiler contre la glib 2.66, et QEMU patché contre gcc 11 : c'est la CI qui le dira.

## 8. Adapter les outils

`tools/check_workflow_pins.sh` : tout `uses:` sur un commit, toute image sur un digest. Une action se monte de version
en relevant son commit (`gh api repos/<o>/<r>/git/ref/tags/<tag>`), et le commentaire `# vX.Y.Z` suit.
`tools/check_version_strings.sh` : la version vit dans `frontend/CMakeLists.txt`, et nulle part ailleurs.
`POMPPC_QEMU_VERSION` (`config.env`) et `QGPU_PROTO_VERSION` ont leur propre numéro. Tous deux tournent dans
`tests/run-all.sh`.
