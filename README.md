# POMPPC — Mac OS X PPC basée sur QEMU

Objectif : émulateur PowerPC (Power Mac G4) faisant tourner **Mac OS X 10.4 (Tiger)**
de façon fluide, à terme sur **Apple Silicon**, en modifiant **QEMU** — pas en repartant de zéro.

## Décisions actées

- **Base : QEMU / TCG**, machine `mac99` (G4). Seul moteur viable (PearPC = JIT x86 mort ;
SheepShaver = pas de MMU, s'arrête à OS 9).
- **Portable d'abord** : code hôte-agnostique ; le branchement Metal ne vient qu'à la toute fin.
Le dev se fait ici (Linux x86-64, TCG pur) ; ce sera identique sur Mac aux perfs hôte près.
- **Phase actuelle : BASELINE bootable, chronométrée.** On n'optimise rien avant d'avoir
un boot stable mesuré. Tout « axe d'optimisation » sans baseline est de la spéculation.



## Réalités techniques (corrections vs. l'analyse de départ)

- **Pas de « G4 quasi-natif ».** Aucune virtualisation matérielle PPC sur ARM
(`Hypervisor.framework` = same-arch seulement). C'est du **pur JIT TCG**. Le fait que PPC et
ARM64 partagent un *weak memory model* évite des barrières en **MTTCG** — gain réel mais borné,
pas un miracle.
- **GPU paravirtualisé (kext OpenGL→Metal) = piège *from-scratch*.** C'est un projet séparé de
plusieurs années (OS mort, aucun SDK). Gelé. Gain graphique réaliste à court terme :
framebuffer 2D, pas un pont OpenGL.
- **SMP OpenPIC** : vrai bon sujet, réaliste, mais après la baseline mono-CPU.



## Prérequis

1. `sudo apt-get install -y qemu-system-ppc qemu-utils`
2. **Une image d'installation Tiger PPC que tu possèdes** (ISO/DMG) → déposée dans `images/`,
  nom ajusté dans `config.env` (`INSTALL_MEDIA`). Aucune ROM Apple nécessaire : OpenBIOS
   (fourni avec QEMU) suffit à booter.



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

Tout est dans `config.env` (machine, CPU, RAM, résolution, chemins). Ne pas éditer les scripts.

## Recette de boot qui MARCHE (durement acquise)

Le disque installé ne bootait pas : `BootX` était **absent** de `/System/Library/CoreServices/`
et OpenBIOS ne gère pas bien le bless `\\:tbxi`. Solution :

1. `scripts/inject-bootx.sh` (sudo) — extrait `BootX` du CD (`work/BootX`) et l'injecte dans le
  volume via `qemu-nbd` + montage HFS+. Détails coriaces gérés par le script :
  - volume HFS+ **encapsulé dans un wrapper HFS** (`BD` à l'offset 1024) → parsing du wrapper
  - noyau refuse l'écriture → patch des attributs du VH HFS+ (bit « démonté proprement »)
2. Boot par **chemin explicite** (pas le bless) : `boot-device=hd:10,\System\Library\CoreServices\BootX`
  (backslashes **simples**). C'est ce que fait `scripts/boot.sh`.



## Lancer le système

```bash
scripts/boot.sh                    # headless (défaut) — je pilote via le socket moniteur
POMPPC_DISPLAY=gtk scripts/boot.sh # fenêtre à l'écran (interaction directe)
```

Le socket moniteur QEMU est dans `.run/mon.sock` (screendump, sendkey, quit…).

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

**État actuel : QEMU stock (aucun patch actif).** Après validation, aucune des optimisations tentées
ne survit. Les patches restent dans `patches/` pour référence.


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

**Métrologie (protocole de mesure, durement acquis).** Le chrono de boot n'est fiable qu'avec les
TROIS précautions : (a) **hôte vraiment au repos** — vérifier `/proc/loadavg` AVANT (une charge de
fond, ex. MAME, gonfle même le CPU_qemu car le guest spin-attend des timers en retard) ; (b)
`EXTRA_ARGS="-snapshot"` — sinon un **fsck sur volume HFS+ sale** (on tue QEMU avant l'arrêt
propre) ajoute ~35s de façon intermittente ; (c) **interleave** stock/patché (alterner run par run)
pour annuler tout biais d'ordre/dérive. Le perf self% reste l'outil de profiling fin.

`clock_gettime` vdso lui-même (~5%) = lecture d'horloge fondamentale, laissée telle quelle
(la battre = rdtsc maison, risqué). Prochain gros poste : dispatch TCG
(`tb_lookup`+`qht_lookup`+`helper_lookup_tb_ptr` ~20%).

## Après la baseline

Build QEMU depuis les sources (`ninja` requis), pointer `QEMU_BIN` dessus, puis profiler
`tcg/` pour voir où l'hôte perd ses cycles sur un boot 10.4. C'est là que commence
l'optimisation du JIT.

```

```

