# POMPPC — Macs PowerPC (Mac OS X 10.4 Tiger et Mac OS 9.2.2) sur QEMU

Un Power Mac G4 émulé par **QEMU/TCG** (`mac99`), patché pour que Tiger tourne bien sur une
machine d'aujourd'hui, Apple Silicon ou Linux x86-64 : deux cœurs, son, flottant rapide,
réseau, et surtout un **GPU paravirtuel** qui fait rendre les applications OpenGL de Tiger par
le GPU de l'hôte. Rien d'Apple n'est modifié dans l'invité : un plugin, un kext, un device.

Tableau de bord : **`TODO.md`**. Historique : **`CHANGELOG.md`**. Conception et relevés :
`docs/`.

## Où en est le projet (au 01/11/2026)

L'état courant détaillé (vitesse, matrice, défauts allumés, binaires de chaque hôte) est dans
`TODO.md` et `docs/etat-des-hotes.md`.

| Couche | État |
|---|---|
| Boot Tiger et OS 9 | reproductible (`scripts/`), recette de boot ci-dessous |
| SMP 2 cœurs (MTTCG) | fait (`patches/smp-mac99/`) ; le cœur 1 est vérifié vivant |
| Flottant rapide (FPU hôte) | fait, résultats identiques au bit près, ×2 à ×3 sur le flottant (`docs/flottant-rapide.md`) |
| Son (Screamer), réseau, relais web | faits (`patches/screamer/`, `docs/internet-tiger.md`) |
| Écran paravirtuel QFB | fait (`kext/POMPPCQFB/`, `patches/qfb/`) |
| **GPU 3D paravirtuel qgpu** | protocole **v22** : pipeline fixe et combineurs ATI, OpenGL 1.5 annoncé, programmes ARB et GLSL, VBO lus par l'hôte, copies GPU et présentation directe |
| Vitesse du G4 émulé | série `patches/tcg/` (allumée par hôte dans `run_tiger.sh`) ; binaire rapide PGO sur le PC (`docs/binaire-rapide-x86.md`) |
| Bureau accéléré (Quartz Extreme) | allumé par défaut sur les deux hôtes (M4 depuis le 29/10, PC depuis le 30/10 ; `QE=0` l'éteint), `docs/quartz-extreme.md` ; Core Image non fait |

Jeux : chacun est vérifié par une matrice automatique (`tools/matrice/`, `docs/matrice-jeux.md`) :
les images affichées par l'invité sont vidées, **rejouées nativement sur l'hôte et comparées au
pixel** à l'écran de la VM et à une référence validée à l'œil ; le plugin ne doit pas se replier
sur le rendu logiciel d'Apple. Tour du 01/10/2026, hôte Apple Silicon, invité Tiger 10.4.6,
15 cellules automatisées vertes sur 15 (tour `bench/matrice/20261001-tout`, celui de la publication
0.1.0 ; profils du jour dans `docs/vitesse-profil-2026-10-01.md`, sur d'autres tours) :

| Jeu | Chemin GL | ms/image (≈ img/s) |
|---|---|---|
| Zenerchi | pipeline fixe | 4,3 (≈ 230) |
| Marble Blast Gold | pipeline fixe | 8,9-10,3 (≈ 100) |
| Warcraft III | tableaux de sommets (plein écran) | 15,6 (≈ 64) |
| UT2004 Demo | VBO, S3TC | 25,3 (≈ 40) |
| Nexuiz 2.5.2, rendu GLSL | GLSL | 35,3 (≈ 28) |
| Colin McRae Rally 2005 | programmes ARB (plein écran) | 47,8 (≈ 21) |
| DOOM 3 Demo | programmes ARB, VBO, 7 unités, DXT | 56,6 (≈ 18) |
| Prey Demo | programmes ARB, VBO, DXT5 | 59,1 (≈ 17) |
| Nexuiz 2.5.2, rendu ARB | programmes ARB | 80,2 (≈ 12) |

Temps moyen sur la fenêtre de mesure, cellule en fenêtre (plein écran pour Warcraft III et Colin
McRae, qui n'ont que lui ; les deux pour Marble Blast) ; les deux modes diffèrent de 2 % au plus
ailleurs. Return to Castle
Wolfenstein n'a pas encore été lancé avec succès.

Ces temps ont baissé depuis (série TCG et allègement du plugin allumés par défaut sur les deux
hôtes). Au 01/11/2026, hôte au repos, ms/image, M4 / PC x86-64 : DOOM 3 en fenêtre 29 / 78-80,
UT2004 en fenêtre 19,6 / 40, Marble Blast en fenêtre 6,1 / 8,3, Zenerchi en fenêtre 2,8 / 4,6,
Colin McRae en plein écran **en course** 42 / 82. Dernier tour complet de la matrice sur le M4 :
17 vertes sur 17 (28/10/2026) ; tableau courant dans `TODO.md`. La version Linux x86-64 n'est
pas publiée tant que sa vitesse n'est pas acceptable (cible : DOOM 3 à 50 ms/image).

Ce qui borne tout : le PowerPC est émulé (pas de virtualisation PPC sur ARM ni x86). Le projet
consiste donc à **enlever du travail au G4 émulé** : géométrie, textures, présentation, et
maintenant les tampons de sommets sont lus et convertis par l'hôte.

## Architecture

- **QEMU 11.1.2 amont + patches** (`patches/`, appliqués par `scripts/build_qemu_qfb.sh`, qui
  vérifie chaque capacité sur le binaire produit) : SMP mac99, Screamer, `qfb-pci`,
  `qgpu-pci`, flottant rapide `x-fast-fp`. Le firmware OpenBIOS SMP est livré en binaire
  (`patches/smp-mac99/openbios-smp-screamer.elf`) ; sa source exacte (commit OpenBIOS de base
  plus un diff, recompilé à l'identique à l'octet) est
  `patches/smp-mac99/openbios-smp-screamer-source.patch`.
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
- **Vérification** : 1 440 épreuves natives (`tests/qgpu_core_test.c` : soft 660, gl 780 avec les
  permutations GLSL de DarkPlaces ; M4, 01/11, 0 échec), rejeu natif
  des vidages pris dans la VM (`tests/qgpu_replay.c`), scènes `guest/gltest` comparées au rendu
  d'Apple dans l'invité, harnais `tests/run-all.sh`.

Conception détaillée, offsets, mesures : `docs/gpu-3d-tiger.md`. Contrat courant :
[docs/protocole.md](docs/protocole.md) (v22, ABI transport v19) ; historique dans `docs/protocole-v*.md`.
Règle du projet : **pas de repli, étendre le
protocole** — sous programme ARB, un repli vers Apple tue le jeu
(`docs/re/glengine-exit-interpolateur.md`).

## Démarrer

### Prérequis

1. **QEMU du dépôt** : `./scripts/build_qemu_qfb.sh` clone l'amont, applique les patches et
   sonde les capacités (verdict dans `bench/build-capabilities.txt`). `config.env` cherche
   `$HOME/src/qemu/build/qemu-system-ppc` ; `QEMU_BIN=` force un binaire. Le paquet distro
   suffit à la baseline (`scripts/`), pas aux lanceurs quotidiens. Sur un PC Linux x86-64,
   `QEMU_FAST=1 ./scripts/build_qemu_qfb.sh` construit en plus `~/src/qemu/build-fast/`
   (`-O3 -march=native`, LTO, PGO : ~10 % plus rapide sur DOOM 3 que la référence à réglages
   égaux), que `run_tiger.sh` préfère
   alors (`QEMU_FAST=0` pour la référence) : [docs/binaire-rapide-x86.md](docs/binaire-rapide-x86.md).
   Le `configure` de QEMU veut Python ≥ 3.9 avec `tomli` (intégré sous le nom `tomllib` à
   partir de 3.11) et `distlib` ; sur macOS un venv suffit
   (`PYTHON=venv/bin/python3 ./scripts/build_qemu_qfb.sh`). Prérequis par hôte (en-têtes
   Linux, formules Homebrew) : en-tête de `scripts/build_qemu_qfb.sh`. Le frontal ImGui
   (`frontend/`) se construit de lui-même au premier lancement (`run_frontend.sh` : `setup.sh`
   puis `cmake`).
2. **Un média d'installation Tiger PPC que vous possédez**, dans `images/` (gitignoré), nommé
   dans `config.env` (`INSTALL_MEDIA`). ISO tel quel ; un DMG compressé doit être converti
   (`hdiutil convert in.dmg -format UDTO -o out`, puis renommer `out.cdr` en `.iso` ; ou `dmg2img`). Aucune ROM Apple : OpenBIOS suffit.
   **Compatibilité** : testé avec Tiger **10.4.6**, la version dont `OpenGL.framework` a été
   relevé (`docs/re/`), et **10.4.11**, celle des VM quotidiennes des deux hôtes depuis octobre
   2026. Les autres 10.4.x ne sont pas éprouvées : le plugin lit les structures internes de
   GLEngine.
3. Pour OS 9 : le CD 9.2.2 dans `disks/os9-install.iso`, ou `OS9_CD=… ./run_os9.sh install`.

### Paquet publié (macOS sur Apple Silicon)

Sans construire : l'archive `pomppc-<version>-macos-arm64.tar.xz` des
[publications](https://github.com/habib256/pomppc/releases) demande **macOS 26 (Tahoe) ou
plus récent** (les bibliothèques Homebrew embarquées sont construites pour lui). C'est le dépôt
au tag de la publication, plus `bin/` (`qemu-system-ppc`, `qemu-system-ppc64`, `qemu-img`
patchés, frontal `pomppc`), `lib/`, `share/qemu/` et `pomppc-guest.iso` (paquet de l'invité) ;
`config.env` prend `bin/` de lui-même. Binaires signés ad hoc, non notariés :
`xattr -dr com.apple.quarantine .` après extraction. Puis le média Tiger et l'installation
comme ci-dessous. L'archive se produit par `scripts/package_release_macos.sh`
(`scripts/package_release_linux.sh` pour Linux x86-64, non publié).

### Installer Tiger

```bash
scripts/00-create-disk.sh   # crée disks/tiger.qcow2 (16 Go, alloué à la demande)
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

Avec le paquet publié (`pomppc-guest.iso` à la racine), `./run_tiger.sh` insère le CD
`POMPPC_GUEST` de binaires construits par le gcc 4.0 de Tiger ; une fois, dans Tiger,
`sudo sh /Volumes/POMPPC_GUEST/install.sh` (Xcode inutile ; `--remove` désinstalle), puis
redémarrer. Une application OpenGL annonce alors `GL_RENDERER = "POMPPC qgpu (OpenGL host GPU)"`.

Depuis le dépôt, `./run_tiger.sh` grave un CD `POMPPCSRC` avec les sources du kext et du plugin.
Une fois, dans Tiger (gcc 4.0 des Xcode Tools 2.5 requis, SDK 10.4u) :

```sh
cp -R /Volumes/POMPPCSRC /tmp/src && sudo sh /tmp/src/guest/gldriver/install.sh
```

puis redémarrer. **Une modification de `qgpu_proto.h` ne demande que QEMU et le plugin**
(`NORUN=1 tools/guest/cycle.sh`, sans redémarrer) ; seule une modification de `qgpu_abi.h` demande
aussi le kext (`install.sh` + redémarrage). Le plugin refuse un kext d'avant la v19 et un QEMU
d'un autre `qgpu_proto.h` (`docs/protocole-v19-transport.md`).

### Lancer

Le son Tiger sur macOS utilise un [profil audio stable](docs/audio-stabilite.md)
(réserve doublée, timer 5 ms). `POMPPC_AUDIO_PROFILE=default` rétablit les anciens
réglages. Dans ImGuiDock : départ en fenêtre, Alt+Entrée (ou F11, Ctrl+Cmd+F) bascule en plein écran ;
souris absolue en fenêtre, relative capturée en plein écran.

```bash
./run_tiger.sh              # Tiger : 2 cœurs MTTCG + son + flottant rapide + GPU qgpu + réseau
SNAPSHOT=1 ./run_tiger.sh   # disque jetable (debug)
GPU=0 ./run_tiger.sh        # sans GPU paravirtuel ; GPU_BACKEND=soft|gl force le backend
FASTFP=0 ./run_tiger.sh     # flottant exact
QFB=1 ./run_tiger.sh        # + écran QFB en second moniteur
NET=0 ./run_tiger.sh        # sans réseau
./run_os9.sh [install]      # Mac OS 9 (un cœur, sans 3D)
OS9_PROFILE=complet SNAPSHOT=1 ./run_os9.sh disk  # OS 9 : profils CPU expérimentaux (docs/os9-optimisations-cpu.md)
./run_frontend.sh           # frontend ImGui (affichage QEMU par D-Bus)
POMPPC_FRONTEND=native ./run_tiger.sh  # fenêtre QEMU native, sans ImGuiDock
```

`./run_tiger.sh` ouvre maintenant Tiger dans **ImGuiDock**. Le profil maximal
active FASTFP, SRTLB, LFSINLINE, VFPFAST, VPERMFAST, FPINLINE, RETINLINE,
JCIDX, ICBISYNC, MSRNOBQL, JITNEAR, FPNATIVE, TBFAST, FPNATIVE64, VMXINLINE, copies GPU et
GLSL, puis la série propre à l'hôte (branches `Linux:x86_64` et `Darwin:arm64` de
`run_tiger.sh` : PPC32MT, LFSKNOWN, TLBBASE…, zone froide sur le PC) et Quartz Extreme, sous
réserve des capacités sondées.
Les surcharges `…=0` restent respectées ; les vérificateurs coûteux ne sont
pas activés. SMP reste à deux cœurs (profil Tiger), pas au nombre de cœurs hôte.
`HEADLESS=1`, `DBUS_DISPLAY=1` et un `POMPPC_DISPLAY` explicite contournent
l'ouverture du frontend ; le banc de jeux conserve son lancement natif.

Chaque lanceur documente ses variables dans son en-tête (`head -160 run_tiger.sh`). Les lanceurs
**sondent le binaire** (`scripts/caps.sh`) et n'annoncent jamais une capacité absente. Verrou
`flock` sur le disque : il meurt avec QEMU, **ne jamais supprimer `.run/tiger.lock`** (le fichier
resté après un arrêt brutal ne verrouille rien ; le supprimer quand QEMU tourne encore laisse un
second QEMU écrire le même qcow2). Moniteur QEMU de la VM quotidienne : `.run/mon.sock` (chemin
effectif dans `.run/tiger.mon`, port ssh dans `.run/tiger.sshport`),
`scripts/moncmd.py .run/mon.sock "<commande HMP>"` (screendump, sendkey, system_reset…).

Les lanceurs quotidiens surchargent `config.env` : 2 cœurs, RAM 768 Mo si le son est
réellement actif (le Screamer exige moins de 1 Go), réseau actif si slirp.

## Développer le GPU 3D

- **Boucle courte, disque quotidien** : `tools/guest/tssh.sh "cmd"` (ssh vers l'invité, port
  publié dans `.run/tiger.sshport`, 2222 par défaut) et `NORUN=1 tools/guest/cycle.sh`
  (transfert, compilation gcc 4.0 dans l'invité, installation du plugin, ~2 min). Déplacés de
  `.run/cmr/` le 26/09 (`tools/README.md`).
- **Boucle instrumentée, disque de dev** : `tools/guest/devloop.py` (jobs par boîte aux lettres
  sur `tiger-dev.raw`, `tools/README.md`).
- **Profil** : `sample <pid> 10` dans Tiger ; `tools/re/sym.py` traduit un `CRASH pc` de
  `<note>.crash` en symbole ; `tools/re/frames.py` résume `frames.csv`.
- **Vidage et rejeu** : `POMPPC_GL_DUMP=<dossier>` et `POMPPC_GL_DUMP_TRIGGER=<fichier>` (ou `@<image>`) dans
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
| `kext/POMPPCGPU/`, `kext/POMPPCQFB/`, `kext/POMPPCFsqrt/` | kexts Tiger : transport du GPU paravirtuel ; framebuffer QFB ; `fsqrt` |
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
| `docs/architecture.md` | propriété des ressources, threads, barrières, clients morts, resets et limites connues |
| `docs/gpu-3d-tiger.md` | GPU 3D : architecture, rétro-ingénierie d'OpenGL.framework, protocole, mesures, boucle de dev |
| `docs/re/README.md` | index des relevés (GLEngine, accélérateur IOKit, UT2004, programmes ARB, étude GLEngine) |
| `docs/protocole.md` | le contrat courant du protocole qgpu (v22, ABI de transport v19) |
| `docs/protocole-v7…v23-*.md` | le protocole qgpu, version par version (historique) |
| `docs/bilan-2026-09-23-jeux-tiger.md` | bilan raisonné jeu par jeu |
| `docs/bug-hunt-2026-09-22.md` | relecture statique de la chaîne 3D, 90 findings et leurs verdicts |
| `docs/roadmap-opengl15.md` | feuille de route de fond (OpenGL 1.5, Quartz Extreme, Core Image) |
| `docs/flottant-rapide.md` | le FPU de l'hôte pour le flottant PowerPC |
| `docs/metrologie-boot.md` | mesure du boot, A/B, optimisations TCG revertées |
| `docs/internet-tiger.md` | réseau et relais HTTPS→HTTP |
| `docs/gpu-tiger-4060ti.md` | étude GPU 2D : passthrough impossible, paravirtualisation |
| `docs/references-ingenierie.md` | documents externes (kext Tiger, IOGraphics, virtio-gpu) |
| `docs/archive/` | anciens tableaux de bord |

## Licences

- Code propre à POMPPC (lanceurs, kexts, plugin OpenGL, frontal, outils, tests, documentation) :
  **GPL-3.0-or-later**, voir [LICENSE](LICENSE).
- Code compilé dans QEMU (`patches/qgpu/`, `patches/qfb/` et la série de patches contre QEMU) :
  **GPL-2.0-or-later**, comme QEMU, voir [LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt) ;
  `qgpu_abi.h`, partagé avec le kext, aussi.
- Le device Screamer (`patches/screamer/`) est de Mark Cave-Ayland, **MIT** ; les changements de
  POMPPC suivent la même licence.
- `patches/smp-mac99/openbios-smp-screamer.elf` est OpenBIOS (GPL-2.0), source exacte dans
  `patches/smp-mac99/openbios-smp-screamer-source.patch`.
- L'interface de registres `qfb` vient de [mac_qfb_driver](https://github.com/SolraBizna/mac_qfb_driver)
  de Solra Bizna ; le SMP s'appuie sur le travail `mac99` de BALATON Zoltan.

Les archives publiées contiennent aussi les bibliothèques tierces embarquées, avec leurs licences
dans `THIRD-PARTY-LICENSES/`. Aucun logiciel Apple n'est distribué : il faut sa propre copie du
système.
