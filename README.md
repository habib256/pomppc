# POMPPC — Macs PowerPC (Mac OS X 10.4 Tiger et Mac OS 9.2.2) sur QEMU

Un Power Mac G4 émulé par **QEMU/TCG** (`mac99`), patché pour que Tiger tourne bien sur une
machine d'aujourd'hui, Apple Silicon ou Linux x86-64 : deux cœurs, son, flottant rapide,
réseau, et surtout un **GPU paravirtuel** qui fait rendre les applications OpenGL de Tiger par
le GPU de l'hôte. Rien d'Apple n'est modifié dans l'invité : un plugin, un kext, un device.

Tableau de bord : **`TODO.md`**. Historique : **`CHANGELOG.md`**. Conception et relevés :
`docs/`.

## Où en est le projet (24/09/2026)

| Couche | État |
|---|---|
| Boot Tiger et OS 9 | reproductible (`scripts/`), recette de boot ci-dessous |
| SMP 2 cœurs (MTTCG) | fait (`patches/smp-mac99/`) ; le cœur 1 est vérifié vivant |
| Flottant rapide (FPU hôte) | fait, résultats identiques au bit près, ×2 à ×3 sur le flottant (`docs/flottant-rapide.md`) |
| Son (Screamer), réseau, relais web | faits (`patches/screamer/`, `docs/internet-tiger.md`) |
| Écran paravirtuel QFB | fait (`kext/POMPPCQFB/`, `patches/qfb/`) |
| **GPU 3D paravirtuel qgpu** | protocole **v18** : pipeline fixe complet, OpenGL 1.5 annoncé et tenu, programmes ARB, VBO lus par l'hôte, présentation directe |
| Bureau accéléré (Quartz Extreme) | pas commencé |

Jeux, sur le disque quotidien (trois preuves visées : image juste, zéro repli, mesure) :

| Jeu | Chemin GL | État |
|---|---|---|
| Marble Blast Gold, Zenerchi | pipeline fixe | justes, ~88 et ~50 img/s |
| DOOM 3 Demo | programmes ARB, VBO, 7 unités, DXT | **image parfaite**, ~22 img/s |
| Prey Demo | programmes ARB, VBO, DXT5 | image juste, lent (117 ms/image) |
| UT2004 Demo | tableaux, VBO, S3TC | jouable ~20 img/s, arme en main noire |
| Warcraft III, Colin McRae | tableaux ; ARB via IndirectX | jouable, texte des menus ; géométrie éclatée en course |
| Return to Castle Wolfenstein | idTech3 | pas encore lancé avec succès |

Ce qui borne tout : le PowerPC est émulé (pas de virtualisation PPC sur ARM ni x86). Le projet
consiste donc à **enlever du travail au G4 émulé** : géométrie, textures, présentation, et
maintenant les tampons de sommets sont lus et convertis par l'hôte.

## Architecture

- **QEMU 9.2 amont + patches** (`patches/`, appliqués par `scripts/build_qemu_qfb.sh`, qui
  vérifie chaque capacité sur le binaire produit) : SMP mac99, Screamer, `qfb-pci`,
  `qgpu-pci`, flottant rapide `x-fast-fp`. Le firmware OpenBIOS SMP est livré en binaire
  (`patches/smp-mac99/openbios-smp-screamer.elf`), non reproductible aujourd'hui.
- **GPU 3D en trois couches**, contrat en deux en-têtes (`patches/qgpu/`) : `qgpu_abi.h`, le
  **transport** (registres, doorbell, tranches de clients — la seule chose que le kext connaît,
  copie identique vérifiée par le harnais) et `qgpu_proto.h`, la **sémantique** (opcodes, clés,
  formats — device et plugin seulement) :
  1. **plugin `GLDriver-POMPPC`** (`guest/gldriver/`) : chargé par `OpenGL.framework` comme le
     pilote d'une carte ; il lit l'état de GLEngine (offsets relevés par rétro-ingénierie,
     `docs/re/`) et émet des commandes qgpu ; hors domaine, il rend la main au rendu d'Apple
     qu'il enveloppe ;
  2. **kext `POMPPCGPU`** (`kext/POMPPCGPU/`) : accélérateur IOKit, fenêtre partagée découpée
     en autant de clients que le device en publie, doorbell asynchrone ; à la mort d'un client
     il demande au device de détruire ses objets (`QGPU_REG_CLIENT_RESET`) et laisse le plugin
     lire les registres (`QGPU_UC_READ_REG`) — il ne compile aucun opcode ;
  3. **device `qgpu-pci`** (`patches/qgpu/`) : cœur qui valide et décode tout (`qgpu-core.c`),
     backend OpenGL hôte (CGL sur macOS, EGL sur Linux) et rastériseur logiciel de référence
     (`qgpu-soft.c`) qui rend le protocole testable sans VM.
- **Vérification** : 883 épreuves natives (`tests/qgpu_core_test.c`, soft et gl), rejeu natif
  des vidages pris dans la VM (`tests/qgpu_replay.c`), scènes `guest/gltest` comparées au rendu
  d'Apple dans l'invité, harnais `tests/run-all.sh`.

Conception détaillée, offsets, mesures : `docs/gpu-3d-tiger.md`. Protocole : un fichier par
version dans `docs/protocole-v*.md` (v7 → v18). Règle du projet : **pas de repli, étendre le
protocole** — sous programme ARB, un repli vers Apple tue le jeu
(`docs/re/glengine-exit-interpolateur.md`).

## Démarrer

### Prérequis

1. **QEMU du dépôt** : `./scripts/build_qemu_qfb.sh` clone l'amont, applique les patches et
   sonde les capacités (verdict dans `bench/build-capabilities.txt`). `config.env` cherche
   `$HOME/src/qemu/build/qemu-system-ppc` ; `QEMU_BIN=` force un binaire. Le paquet distro
   suffit à la baseline (`scripts/`), pas aux lanceurs quotidiens.
2. **Un média d'installation Tiger PPC que vous possédez**, dans `images/` (gitignoré), nommé
   dans `config.env` (`INSTALL_MEDIA`). ISO tel quel ; un DMG compressé doit être converti
   (`dmg2img`, ou `hdiutil convert -format UDRO`). Aucune ROM Apple : OpenBIOS suffit.
3. Pour OS 9 : le CD 9.2.2 dans `disks/os9-install.iso`, ou `OS9_CD=… ./run_os9.sh install`.

### Installer Tiger

```bash
scripts/00-create-disk.sh   # crée disks/tiger.qcow2
scripts/10-install.sh       # boote le média, installe
scripts/20-run.sh           # boote le disque installé, chronomètre
```

Deux pièges, durement acquis :

- **Invite `0 >` d'OpenFirmware** au lieu de booter : `boot cd:,\\:tbxi` à l'installation,
  `boot hd:,\\:tbxi` ensuite.
- **`BootX` absent** du disque installé : `scripts/inject-bootx.sh` (sudo) l'injecte depuis
  `work/BootX`, extrait du CD au préalable ; le boot se fait par chemin explicite
  (`boot-device=hd:10,\System\Library\CoreServices\BootX`, ce que fait `scripts/boot.sh`).

### Installer le GPU 3D dans l'invité

`./run_tiger.sh` grave un CD `POMPPCSRC` avec les sources du kext et du plugin. Une fois, dans
Tiger (gcc 4.0 des Xcode Tools requis) :

```sh
cp -R /Volumes/POMPPCSRC /tmp/src && sudo sh /tmp/src/guest/gldriver/install.sh
```

puis redémarrer. **Une modification de `qgpu_proto.h` ne demande que QEMU et le plugin**
(`.run/cmr/cycle.sh NORUN=1`, sans redémarrer) ; seule une modification de `qgpu_abi.h` demande
aussi le kext (`install.sh` + redémarrage). Le plugin refuse un kext d'avant la v19 et un QEMU
d'un autre `qgpu_proto.h` (`docs/protocole-v19-transport.md`).

### Lancer

```bash
./run_tiger.sh              # Tiger : 2 cœurs MTTCG + son + flottant rapide + GPU qgpu + réseau
SNAPSHOT=1 ./run_tiger.sh   # disque jetable (debug)
GPU=0 ./run_tiger.sh        # sans GPU paravirtuel ; GPU_BACKEND=soft|gl force le backend
FASTFP=0 ./run_tiger.sh     # flottant exact
QFB=1 ./run_tiger.sh        # + écran QFB en second moniteur
NET=0 ./run_tiger.sh        # sans réseau
./run_os9.sh [install]      # Mac OS 9
./run_frontend.sh           # frontend ImGui (affichage QEMU par D-Bus)
```

Chaque lanceur documente ses variables dans son en-tête (`head -30 run_tiger.sh`). Les lanceurs
**sondent le binaire** (`scripts/caps.sh`) et n'annoncent jamais une capacité absente. Verrou
`flock` sur le disque : `rm -f .run/tiger.lock` si un arrêt brutal l'a laissé. Moniteur QEMU :
`scripts/moncmd.py .run/mon.sock "<commande HMP>"` (screendump, sendkey, system_reset…).

Les lanceurs quotidiens surchargent `config.env` : 2 cœurs, RAM 768 Mo si le son est
réellement actif (le Screamer exige moins de 1 Go), réseau actif si slirp.

## Développer le GPU 3D

- **Boucle courte, disque quotidien** : `.run/cmr/tssh.sh "cmd"` (ssh vers l'invité, port 2222)
  et `.run/cmr/cycle.sh NORUN=1` (transfert, compilation gcc 4.0 dans l'invité, installation du
  plugin, ~2 min). Ces scripts sont à verser dans `tools/guest/` (`TODO.md` A5).
- **Boucle instrumentée, disque de dev** : `tools/guest/devloop.py` (jobs par boîte aux lettres
  sur `tiger-dev.raw`, `tools/README.md`).
- **Profil** : `sample <pid> 10` dans Tiger ; `tools/re/sym.py` traduit un `CRASH pc` de
  `<note>.crash` en symbole ; `tools/re/frames.py` résume `frames.csv`.
- **Vidage et rejeu** : `POMPPC_GL_DUMP=<dossier>` et `POMPPC_GL_DUMP_TRIGGER=<fichier>` dans
  l'invité, `tools/re/dumpdec.py` pour lire, `tests/qgpu_replay.c` pour rejouer sur l'hôte et
  bisecter en réécrivant le vidage.
- **Drapeaux** `POMPPC_GL_*` (dans `guest/gldriver/pomppc_accel.c`) : `NOTE`, `STATS`, `DUMP`,
  `NATIVE=0`, `LAZYAPPLE=0`, `S3TC=0`, `PROG=0`, `ARRAY`, `TRIFILTER=0`… chacun rétablit le
  comportement d'avant un lot.
- **Rétro-ingénierie** : `tools/re/ppcanno.py` (désassembleur PowerPC annoté) ; les relevés
  sont dans `docs/re/`, un fichier par sujet, avec la méthode de vérification.

## Tests

```bash
./tests/run-all.sh          # syntaxe, shellcheck, doc ↔ binaire, registres QFB, qgpu natif
./tests/run-all.sh --slow   # + qfb_smoke.py, qgpu_smoke.py, bridge_probe (bootent réellement)
```

Le harnais interroge le **binaire** QEMU (Screamer, qfb-pci, qgpu-pci, slirp, SMP, audio,
`x-fast-fp`) au lieu de comparer la doc au dépôt, vérifie `qgpu_abi.h` identique hôte/kext et
le kext vierge de toute sémantique,
compile et exécute `qgpu_core_test` (soft + gl), et contrôle l'alignement des registres QFB.
Le plugin lui-même se teste dans l'invité (`docs/gpu-3d-tiger.md` §5).

## Mesurer

Le protocole A/B (interleave, `-snapshot`, hôte au repos, médiane de `CPU_qemu`) et l'histoire
des optimisations TCG tentées puis revertées sont dans `docs/metrologie-boot.md`. Leçon
conservée : le self % du profil localise, il ne valide pas. En jeu, la mesure est `frames.csv`
(ms/image, replis, relectures) à scène égale, et `sample` dans l'invité.

## Carte du dépôt

| Chemin | Rôle |
|---|---|
| `TODO.md`, `CHANGELOG.md` | tableau de bord ; historique par jour et par version du protocole |
| `config.env` | configuration unique (binaire QEMU, machine, RAM, résolution, chemins) |
| `run_tiger.sh`, `run_os9.sh`, `run_frontend.sh`, `mount`, `add-pad` | lanceurs d'usage quotidien |
| `scripts/` | baseline : disque, install, boot, mesure, profil, build QEMU, `caps.sh`, `moncmd.py`, `web-proxy.py` |
| `patches/` | patches QEMU et firmware ; `patches/README.md` dit ce qui est appliqué et ce qui est là pour référence |
| `patches/qgpu/` | device `qgpu-pci`, cœur, backends soft et GL, contrat `qgpu_proto.h` |
| `patches/qfb/`, `patches/screamer/`, `patches/fastfp/`, `patches/smp-mac99/` | écran QFB, audio, flottant rapide, SMP |
| `kext/POMPPCGPU/`, `kext/POMPPCQFB/` | kexts Tiger : transport du GPU paravirtuel ; framebuffer QFB |
| `guest/gldriver/` | plugin OpenGL de Tiger et son installateur (`guest/gldriver/README.md`) |
| `guest/gltest/`, `guest/qgpu-test/`, `guest/fpbench/` | programmes de test à compiler dans l'invité |
| `guest/net/` | réglage du proxy web dans l'invité |
| `tools/` | rétro-ingénierie, générateur de trampolines, boucle de dev, décodage de vidages (`tools/README.md`) |
| `tests/` | harnais, tests natifs du cœur, rejoueur, tests de bout en bout |
| `frontend/` | frontend Dear ImGui (`frontend/README.md`) |
| `docs/` | conception, protocole par version, relevés (`docs/re/`), bilans, archives (`docs/archive/`) |
| `disks/`, `images/`, `shared/`, `bench/`, `.run/` | données locales et éphémères, gitignorées (sauf `disks/extras/`) |

## Documentation

| Fichier | Contenu |
|---|---|
| `docs/gpu-3d-tiger.md` | GPU 3D : architecture, rétro-ingénierie d'OpenGL.framework, protocole, mesures, boucle de dev |
| `docs/re/README.md` | index des relevés (GLEngine, accélérateur IOKit, UT2004, programmes ARB, étude GLEngine) |
| `docs/protocole-v7…v18` | le protocole qgpu, version par version (fusion prévue en un seul `protocole.md`) |
| `docs/bilan-2026-09-23-jeux-tiger.md` | bilan raisonné jeu par jeu |
| `docs/bug-hunt-2026-09-22.md` | relecture statique de la chaîne 3D, 90 findings et leurs verdicts |
| `docs/roadmap-opengl15.md` | feuille de route de fond (OpenGL 1.5, Quartz Extreme, Core Image) |
| `docs/flottant-rapide.md` | le FPU de l'hôte pour le flottant PowerPC |
| `docs/metrologie-boot.md` | mesure du boot, A/B, optimisations TCG revertées |
| `docs/internet-tiger.md` | réseau et relais HTTPS→HTTP |
| `docs/gpu-tiger-4060ti.md` | étude GPU 2D : passthrough impossible, paravirtualisation |
| `docs/references-ingenierie.md` | documents externes (kext Tiger, IOGraphics, virtio-gpu) |
| `docs/archive/` | anciens tableaux de bord |
