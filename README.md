# POMPPC — Macs PowerPC (Mac OS 9.2.2 & Mac OS X 10.4) sur QEMU

Objectif : émulateur PowerPC (Power Mac G4) faisant tourner **Mac OS X 10.4 (Tiger)**
et **Mac OS 9.2.2** de façon fluide, à terme sur **Apple Silicon**, en modifiant **QEMU**
— pas en repartant de zéro.

Deux couches se superposent dans ce dépôt, et il faut les distinguer pour ne pas s'y perdre :

- une **couche baseline/métrologie** (`scripts/`, `config.env`), volontairement minimale,
  qui sert à mesurer et à profiler un boot Tiger ;
- une **couche usage quotidien** (`run_tiger.sh`, `run_os9.sh`, `frontend/`, `mount`), qui
  ajoute le SMP, le son, la manette, le dossier partagé et l'écran paravirtuel QFB.

Les deux partagent `config.env` mais **pas** les mêmes défauts (SMP, réseau, affichage) :
voir « Lanceurs » ci-dessous.

## Décisions actées

- **Base : QEMU / TCG**, machine `mac99` (G4). Seul moteur viable (PearPC = JIT x86 mort ;
SheepShaver = pas de MMU, s'arrête à OS 9).
- **Portable d'abord** : code hôte-agnostique ; le branchement Metal ne vient qu'à la toute fin.
Le dev se fait ici (Linux x86-64, TCG pur) ; ce sera identique sur Mac aux perfs hôte près.
- **La baseline est acquise et chiffrée** (23,32 s de CPU QEMU pour un boot Tiger, protocole
en « Métrologie » plus bas). Règle maintenue : on n'optimise rien sans A/B mesuré — tout
« axe d'optimisation » sans baseline est de la spéculation.



## Réalités techniques (corrections vs. l'analyse de départ)

- **Pas de « G4 quasi-natif ».** Aucune virtualisation matérielle PPC sur ARM
(`Hypervisor.framework` = same-arch seulement). C'est du **pur JIT TCG**. Le fait que PPC et
ARM64 partagent un *weak memory model* évite des barrières en **MTTCG** — gain réel mais borné,
pas un miracle.
- **GPU paravirtualisé (kext OpenGL→Metal) = piège *from-scratch*.** C'est un projet séparé de
plusieurs années (OS mort, aucun SDK). Gelé. Gain graphique réaliste à court terme :
framebuffer 2D — c'est exactement ce qui a été fait (device `qfb-pci` + kext `POMPPCQFB`,
section « Écran paravirtuel QFB »), pas un pont OpenGL.
- **SMP** : fait, en deux morceaux (`patches/smp-mac99/qemu-mac99-cpus-v2.patch`, d'après la série
de BALATON Zoltan). D'abord le garde-fou `« Only UP supported today »` d'`hw/intc/openpic.c` est
neutralisé ; ensuite le **GPIO 4 de KeyLargo** — la ligne de reset du CPU1 sur un vrai bi-G4 — est
câblé à un `cpu_kick()` qui `cpu_reset()` puis relâche le cœur secondaire. `run_tiger.sh` démarre
Tiger sur **2 cœurs MTTCG** par défaut ; OS 9 reste mono-cœur (SMP buggé côté invité).



## Prérequis

1. **QEMU.** `sudo apt-get install -y qemu-system-ppc qemu-utils` suffit pour la baseline
   (`scripts/*`). `config.env` cherche d'abord le build source
   (`$HOME/src/qemu/build/qemu-system-ppc`, celui que produit `scripts/build_qemu_qfb.sh`) et
   **retombe sur le paquet distro** s'il est absent ; `QEMU_BIN=/chemin/qemu-system-ppc` force
   un binaire précis. En revanche `run_tiger.sh` / `run_os9.sh` **exigent le build source** :
   son (Screamer), OpenBIOS unifié et `qfb-pci` n'existent pas dans le paquet distro.
2. **Une image d'installation Tiger PPC que tu possèdes** → à déposer dans `images/`
   (dossier à créer, gitignoré), nom ajusté dans `config.env` (`INSTALL_MEDIA`). Elle est
   passée à QEMU en `format=raw` : un ISO convient tel quel, un **DMG compressé (UDIF) doit
   d'abord être converti** (`dmg2img`, ou `hdiutil convert -format UDRO` côté Mac). Aucune ROM
   Apple nécessaire : OpenBIOS (fourni avec QEMU) suffit à booter.
3. Pour Mac OS 9 : **ton CD 9.2.2** dans `disks/os9-install.iso`, ou
   `OS9_CD=/chemin/os9.iso ./run_os9.sh install`.



## Déroulé

```bash
scripts/00-create-disk.sh   # crée disks/tiger.qcow2 (16G à la demande)
scripts/10-install.sh       # boote le média, installe OS X sur le disque
scripts/20-run.sh           # boote le système installé, chronomètre -> bench/run-*.log
```



## Piège OpenFirmware (le point où tout le monde cale)

Si QEMU s'arrête sur une invite `0 >` au lieu de booter :

```
boot cd:,\\:tbxi      # à l'installation (fichier bootinfo "blessé" du CD Apple)
boot hd:,\\:tbxi      # pour booter le disque installé si l'auto-boot échoue
```



## Configuration

Tout est dans `config.env` (binaire QEMU, machine, CPU, RAM, résolution, chemins des disques et
des médias). Ne pas éditer les scripts.

Trois surcharges **volontaires** de `config.env` par les lanceurs quotidiens, à connaître avant de
s'étonner d'un écart :

| `config.env` | `run_tiger.sh` | `run_os9.sh` |
| --- | --- | --- |
| `SMP=1` | 2 cœurs MTTCG (`SMP=1` pour revenir en mono) | 1 (SMP OS 9 buggé) |
| `RAM_MB=1024` | 768 quand le son est actif (le Screamer exige < 1 Go) | 512 |
| réseau selon `NONET` | coupé sauf `NET=1` (build sans slirp) | coupé |

## Recette de boot qui MARCHE (durement acquise)

Le disque installé ne bootait pas : `BootX` était **absent** de `/System/Library/CoreServices/`
et OpenBIOS ne gère pas bien le bless `\\:tbxi`. Solution :

1. `scripts/inject-bootx.sh` (sudo) — injecte dans le volume, via `qemu-nbd` + montage HFS+, un
  `BootX` que tu as **préalablement extrait du CD d'install** dans `work/BootX` (le script ne
  fait pas l'extraction : il échoue si le fichier manque). Détails coriaces gérés par le script :
  - volume HFS+ **encapsulé dans un wrapper HFS** (`BD` à l'offset 1024) → parsing du wrapper
  - noyau refuse l'écriture → patch des attributs du VH HFS+ (bit « démonté proprement »)
2. Boot par **chemin explicite** (pas le bless) : `boot-device=hd:10,\System\Library\CoreServices\BootX`
  (backslashes **simples**). C'est ce que fait `scripts/boot.sh`.



## Lanceurs

Deux familles, défauts différents — c'est voulu.

**Baseline / mesure** (`scripts/`, mono-cœur, pas de son, réseau selon `config.env`) :

```bash
scripts/boot.sh                      # fenêtre GTK (défaut), chronométré, socket moniteur
POMPPC_DISPLAY=none scripts/boot.sh  # headless — pilotage par le moniteur seul
scripts/measure-boot.sh              # mesure le boot jusqu'à l'écran bleu (wall + CPU_qemu)
scripts/profile-boot.sh              # perf record sur un boot complet
```

**Usage quotidien** (SMP, son, manette, partage — nécessite le build source) :

```bash
./run_tiger.sh              # Tiger : 2 cœurs MTTCG + son, fenêtre GTK, disque persistant
SNAPSHOT=1 ./run_tiger.sh   # disque jetable (writes annulés → pas de fsck) : à utiliser en debug
QFB=1 ./run_tiger.sh        # + écran paravirtuel QFB en second moniteur
./run_os9.sh                # Mac OS 9 : boote le disque installé, sinon le CD en live
./run_os9.sh install        # force le boot CD pour (ré)installer
./run_frontend.sh           # frontend ImGui (affichage QEMU embarqué via D-Bus)
./mount <nom>               # insère un CD à chaud dans l'OS 9 en cours (lecteur 'gamecd')
./add-pad                   # enregistre la manette branchée dans la règle udev
```

Chaque lanceur documente ses variables d'environnement dans son en-tête (`head -20 run_tiger.sh`).
Le socket moniteur QEMU est dans `.run/mon.sock` (Tiger) ou `.run/os9-mon.sock` (OS 9) ;
`scripts/moncmd.py <socket> "<commande HMP>"` l'interroge (screendump, sendkey, info block, quit…).

## Écran paravirtuel QFB (kext Tiger)

Portage PCI/PowerPC du framebuffer paravirtualisé « qfb1 » de Solra Bizna
([mac_qfb_driver](https://github.com/SolraBizna/mac_qfb_driver), à l'origine NuBus/68k) :
device QEMU `qfb-pci` + kext `POMPPCQFB` côté Tiger.

```bash
./scripts/build_qemu_qfb.sh        # QEMU 9.2.0 + SMP mac99 + device qfb-pci
python3 tests/qfb_smoke.py         # test de bout en bout (Open Firmware, sans invité)
QFB=1 ./run_tiger.sh               # Tiger + écran QFB en second moniteur
./scripts/make_kext_iso.sh         # sources du kext sur un CD, à compiler dans l'invité
```

Détails : `kext/POMPPCQFB/README.md` ; contexte et alternatives (dont pourquoi une
RTX 4060 Ti ne peut pas être passée en direct) : `docs/gpu-tiger-4060ti.md`.

## Optimisations (guidées par le profiling)

Méthode : `scripts/profile-boot.sh` (perf record) pour **localiser** un hotspot → patcher `../qemu`
→ recompiler → **valider par A/B déterministe** (voir ci-dessous). ⚠️ **Leçon durement acquise : le
perf self% localise, il ne valide pas.** Baisser le self% d'une fonction chaude ne prouve pas un
gain de débit — le coût peut se déplacer ailleurs. Seul un **A/B stock-vs-patché en** `-snapshot`**,
hôte au repos** tranche.

Validation par **A/B interleave, hôte au repos, garde-fou charge** (médiane sur n=8) :

**État actuel : aucun patch d'optimisation TCG actif.** Après validation, aucune des optimisations
tentées ne survit ; les patches restent dans `patches/01-*.patch` et `patches/02-*.patch` pour
référence. À ne pas confondre avec le binaire utilisé : le build source **est** patché, mais
seulement pour des **fonctionnalités** (SMP mac99, device Screamer, `qfb-pci` — voir
`scripts/build_qemu_qfb.sh`), jamais pour la performance du JIT.


| #     | Cible                                                                                                                       | Médiane boot (CPU_qemu, `-snapshot`)                                               | Statut                                                                                                             |
| ----- | --------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| 01    | Division 128 bits du timebase (`mftb` → `ns_to_tb` → `__divti3`) + indirection wrapper d'horloge (`cpus_get_virtual_clock`) | **23,32s = stock** : **neutre** (aucun gain mesurable).                            | **REVERTÉ** — zéro gain, et l'approximation par réciproque touche le timing (risque gratuit). `patches/01-*.patch` |
| 02/03 | Flush jmp_cache O(N) → **invalidation par génération O(1)** + cache 4× (12→14 bits)                                         | **28,63s (+23%)** ; n'atteint jamais le mode rapide (dispersion 0,25s) ; hang ~3×. | **REVERTÉ** — net-négatif. `patches/02-*.patch`                                                                    |


Patch 01 : l'A/B propre le donne **identique à stock** (23,32 vs 23,32) → il ne gagne rien, et comme
`ns_to_tb_cached` remplace la division exacte par une multiplication-réciproque approchée (≤1 tick),
il ajoute un risque de timing pour rien → reverté.

Patch jmp_cache : **régression réelle et reproductible de ~23%** (confirmée sur A/B propre après une
première campagne faussée par la charge hôte). Sous-résultat : la taille 14 seule (avec effacement
stock) est neutre → le cache 4× n'est pas en cause, c'est la **logique de génération**. Deux modes de
boot (~23,3 rapide / ~28,6 lent) ; le patch force quasi toujours le mode lent. Le self% avait menti :
`tcg_flush_jmp_cache` disparaissait du profil mais le débit chutait — **le perf self% localise, il ne
valide pas**.

**Métrologie (protocole de mesure, durement acquis).** L'outil est `scripts/measure-boot.sh` : il
boote en headless, détecte l'écran bleu de login à la couleur moyenne des `screendump`, et écrit
`wall=…  cpu=…` dans `bench/last-boot.txt` (les captures intermédiaires vont dans `bench/frames/`).
**C'est `CPU_qemu` — et non le temps mur — qui sert de métrique**, car il est immunisé à la
contention hôte. Le chrono n'est fiable qu'avec les TROIS précautions : (a) **hôte vraiment au
repos** — vérifier `/proc/loadavg` AVANT (une charge de fond, ex. MAME, gonfle même le CPU_qemu car
le guest spin-attend des timers en retard) ; (b) `EXTRA_ARGS="-snapshot"` — sinon un **fsck sur
volume HFS+ sale** (on tue QEMU avant l'arrêt propre) ajoute ~35s de façon intermittente ; (c)
**interleave** stock/patché (alterner run par run) pour annuler tout biais d'ordre/dérive. Le perf
self% reste l'outil de profiling fin.

```bash
cat /proc/loadavg                                            # (a) garde-fou charge
EXTRA_ARGS="-snapshot" scripts/measure-boot.sh               # (b) un run propre
SMP_N=2 EXTRA_ARGS="-snapshot -accel tcg,thread=multi" \
  scripts/measure-boot.sh                                    # variante SMP/MTTCG
```

`clock_gettime` vdso lui-même (~5%) = lecture d'horloge fondamentale, laissée telle quelle
(la battre = rdtsc maison, risqué). Prochain gros poste : dispatch TCG
(`tb_lookup`+`qht_lookup`+`helper_lookup_tb_ptr` ~20%).

## Prochaine étape

Le build source et le profiling sont en place (`scripts/build_qemu_qfb.sh`,
`scripts/profile-boot.sh`) et les deux premières tentatives d'optimisation ont été mesurées puis
revertées. Le prochain gros poste identifié est le **dispatch TCG**
(`tb_lookup` + `qht_lookup` + `helper_lookup_tb_ptr`, ~20 % du temps hôte) : c'est là que se joue
l'optimisation du JIT, avec le même protocole A/B qu'au-dessus.

## Carte du dépôt

| Chemin | Rôle |
| --- | --- |
| `config.env` | configuration unique (binaire QEMU, machine, RAM, résolution, chemins) — ne pas éditer les scripts |
| `scripts/` | baseline : création du disque, install, boot, mesure, profiling, build QEMU, `moncmd.py` |
| `run_tiger.sh` / `run_os9.sh` | lanceurs d'usage quotidien (SMP, son, manette, partage, QFB) |
| `mount` / `add-pad` | CD à chaud dans OS 9 ; enregistrement d'une manette dans la règle udev |
| `frontend/` | frontend Dear ImGui embarquant l'affichage QEMU via D-Bus (`frontend/README.md`) |
| `patches/` | patches QEMU et firmware OpenBIOS. Ce qui est appliqué vs. ce qui n'est là que pour référence : `patches/README.md` |
| `kext/POMPPCQFB/` | pilote Tiger du framebuffer QFB (`kext/POMPPCQFB/README.md`) |
| `tests/qfb_smoke.py` | test de bout en bout du device QFB, sans invité |
| `docs/gpu-tiger-4060ti.md` | étude GPU : passthrough, paravirtualisation, plan par phases |
| `pack/` | règle udev pour le passthrough manette |
| `disks/extras/` | petits pilotes tiers pour OS 9 (virtio, tablette USB) — `disks/extras/README.md` |
| `disks/`, `images/`, `shared/`, `bench/` | données locales, gitignorées (sauf `disks/extras/`) |
