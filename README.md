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
- **GPU paravirtualisé : pas « un kext OpenGL→Metal », mais trois couches.** Un kext Tiger
ne peut pas appeler Vulkan/OpenGL : le rendu se fait côté hôte. Le gel initial (« piège
from-scratch ») visait un port de pile GL complète dans l'invité ; il est levé pour une
architecture où l'invité ne porte qu'un traducteur : plugin OpenGL `GLDriver-POMPPC`, kext
`POMPPCGPU` et device `qgpu-pci`, **faits et testés dans Tiger**. Voir « GPU 3D paravirtuel
qgpu » et `docs/gpu-3d-tiger.md`. L'écran 2D reste `qfb-pci` + `POMPPCQFB`.
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

   ✅ **`scripts/build_qemu_qfb.sh` reproduit intégralement le binaire de référence** :
   device audio Screamer (`patches/screamer/`, vendu dans le dépôt), `qfb-pci`, SMP mac99,
   slirp et PulseAudio — tout est demandé explicitement à `configure`, jamais laissé à
   l'autodétection. Le script se termine par une **vérification de capacités** et écrit le
   verdict dans `bench/build-capabilities.txt` ; il sort en erreur si une capacité manque.
   Aucun fork tiers n'est récupéré au build : seul l'amont `qemu-project` est cloné.
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
| `RAM_MB=1024` | 768 **si et seulement si** le son est réellement actif (le Screamer exige < 1 Go) | 512 |
| réseau selon `NONET` | actif si slirp (`NET=0` pour couper) | coupé |

**Les lanceurs sondent le binaire, ils ne supposent rien.** Règle du dépôt : *un lanceur
n'annonce jamais une capacité que le binaire n'a pas* (`scripts/caps.sh`). Elle vient d'un
bug coûteux : le binaire de référence a été reconstruit sans le Screamer, et `run_tiger.sh` a
continué d'afficher « + SON », de passer `-global screamer.audiodev=snd0` — que QEMU ignore
avec un **simple warning**, pas une erreur — et surtout de raboter la RAM invité de 1024 à
768 Mo pour un device absent. Désormais : pas de Screamer → message explicite, pas de
plafond RAM, bannière sans « + SON ». Idem pour slirp avec `NET=1`.

Les deux lanceurs prennent aussi un **verrou `flock`** sur leur disque (sauf en `SNAPSHOT=1`,
où plusieurs instances sont légitimes) : deux QEMU sur le même qcow2 le corrompent. Le
`fuser` précédent venait de `psmisc` et, si le paquet manquait, ne gardait plus rien.

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
GPU=1 ./run_tiger.sh        # + GPU paravirtuel qgpu, et CD des sources du plugin GL
FASTFP=1 ./run_tiger.sh     # + flottant rapide : le FPU de l'hôte pour le flottant PowerPC (docs/flottant-rapide.md)
NET=0 ./run_tiger.sh        # sans réseau (actif par défaut, avec le relais web)
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

## Internet sous Tiger

`run_tiger.sh` active le réseau par défaut (NAT utilisateur de QEMU : Tiger reçoit
`10.0.2.15` par DHCP, l'hôte est `10.0.2.2`, le DNS `10.0.2.3`). HTTP fonctionne tel quel,
mais **HTTPS non** : Tiger n'a qu'OpenSSL 0.9.7 et des certificats de 2006, que les sites
actuels refusent.

D'où le relais `scripts/web-proxy.py`, lancé et arrêté avec QEMU : il n'écoute que sur
`127.0.0.1:8080` (vu de Tiger : `10.0.2.2:8080`), va chercher les pages en HTTPS moderne
avec les certificats de l'hôte et les rend en HTTP, liens réécrits. Dans Tiger, une fois :

```sh
sudo sh /Volumes/POMPPCSRC/guest/net/proxy.sh on    # CD présent avec GPU=1 ou GLISO=1
```

(ou Préférences Système → Réseau → Proxys → Proxy web (HTTP) : `10.0.2.2`, port `8080`),
puis taper les adresses en `http://` dans Safari. **Page d'accueil : `http://pomppc/`**
(recherche Google, sites légers, réglages). `WEBPROXY=0` ne lance pas le relais, `NET=0`
coupe le réseau.

Le relais ne s'arrête pas au TLS : Safari 2 ne comprend ni le JavaScript moderne, ni WebP,
AVIF ou SVG. Selon le **mode** du site, réglable par la barre jaune en tête de chaque page :

| mode | ce que reçoit le vieux navigateur |
|---|---|
| **auto** (défaut) | *sans JS*, et *rendu* si la page sort vide ou demande JavaScript (appris par site) |
| sans JS | la page sans ses scripts, `<noscript>` déplié — plus légère de moitié |
| rendu | Chrome, sans affichage, exécute la page sur l'hôte ; l'invité reçoit le document obtenu |
| brut | la page telle quelle |

Dans tous les modes : images WebP, AVIF et SVG converties, grandes images réduites à 1024 px,
bannières de cookies et éléments que Safari 2 afficherait à tort retirés. Des **adaptateurs**
servent les sites fermés à tout autre qu'un navigateur récent : **recherche Google** (rendue
par Chrome, résultats en HTML simple), **Reddit** (lu par ses flux), **Stack Overflow** et
Stack Exchange (par leur API). Dépendances facultatives, détectées au démarrage :
BeautifulSoup + lxml, Pillow, ImageMagick, Google Chrome.

Mesure : `tools/web/banc.py` passe 22 sites par une instance du relais en se présentant comme
Safari 2 — **21 lisibles** contre 15 avec le simple relais TLS, 131 images décodables sur 133,
plus aucun script envoyé (19/09/2026) ; archive.org reste fermé.

## GPU 3D paravirtuel qgpu (plugin OpenGL de Tiger → GPU de l'hôte)

Les applications OpenGL de Tiger rendent sur le GPU de l'hôte. Trois couches : le plugin
`GLDriver-POMPPC` (chargé par `OpenGL.framework` comme un pilote, il traduit la rastérisation de
GLEngine en commandes), le kext `POMPPCGPU` (transport, 4 processus à la fois) et le device QEMU
`qgpu-pci`, qui exécute les commandes par OpenGL sur l'hôte (CGL sur macOS, EGL sur Linux).
Tout ce que le plugin n'accélère pas est rendu par le code d'Apple, qu'il enveloppe : le résultat
reste exact.

| Scène (dans Tiger) | Apple logiciel | POMPPC |
|---|---|---|
| couloir multitexture + brouillard, 640×480 | 3,0 img/s | 574 img/s |
| plan en perspective trilinéaire, 512×384 | 14,2 img/s | 837 img/s |

```bash
./scripts/build_qemu_qfb.sh          # QEMU + qfb-pci + qgpu-pci
./tests/run-all.sh --slow            # dont les tests qgpu (natif soft/GL, bout en bout)
GPU=1 ./run_tiger.sh                 # Tiger + qgpu-pci + CD « POMPPCSRC » (sources du plugin,
                                     # regravé si besoin ; GLISO=0 pour l'omettre). Une fois,
                                     # dans Tiger : cp -R /Volumes/POMPPCSRC /tmp/src &&
                                     #   sudo sh /tmp/src/guest/gldriver/install.sh
```

Domaine accéléré, rétro-ingénierie d'`OpenGL.framework`, protocole, mesures et boucle de
développement dans l'invité : `docs/gpu-3d-tiger.md`. Plugin : `guest/gldriver/README.md`.

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
repos** (une charge de fond, ex. MAME, gonfle même le CPU_qemu car le guest spin-attend des
timers en retard) ; (b) `EXTRA_ARGS="-snapshot"` — sinon un **fsck sur volume HFS+ sale** (on
tue QEMU avant l'arrêt propre) ajoute ~35s de façon intermittente ; (c) **interleave**
stock/patché (alterner run par run) pour annuler tout biais d'ordre/dérive. Le perf self%
reste l'outil de profiling fin.

Les précautions (a) et le nettoyage sont désormais **appliqués par l'outil**, pas laissés à la
discipline :

- **pré-vol** : `measure-boot.sh` refuse de démarrer si un autre `qemu-system-ppc` tourne ou
  si `/proc/loadavg` dépasse `MAXLOAD` (1.0 par défaut). `POMPPC_FORCE=1` passe outre — et le
  dit : *« ce chiffre n'est pas publiable »*.
- **PID via `-pidfile`** : le `pgrep -f qemu-system-ppc.*POMPPC-measure` d'avant pouvait
  s'accrocher à un QEMU orphelin d'un run précédent et lire `/proc/<mauvais_pid>/stat`. Une
  mesure fausse mais plausible est le pire mode de panne pour une campagne A/B.
- **`trap` de nettoyage** : un Ctrl-C laissait tourner la VM `setsid`, c'est-à-dire exactement
  la charge de fond que (a) interdit. `measure-boot.sh` et `profile-boot.sh` tuent la leur en
  sortie, quoi qu'il arrive.
- **outils vérifiés** : sans ImageMagick, la détection d'écran bleu renvoyait du vide, la
  comparaison arithmétique valait 0, et on obtenait un timeout silencieux au lieu d'une erreur.

```bash
EXTRA_ARGS="-snapshot" scripts/measure-boot.sh               # un run propre (pré-vol inclus)
SMP_N=2 EXTRA_ARGS="-snapshot -accel tcg,thread=multi" \
  scripts/measure-boot.sh                                    # variante SMP/MTTCG
scripts/ab-measure.sh <binA> <binB> 4                        # A/B interleavé, médiane
```

`scripts/ab-measure.sh` applique le protocole complet (interleave A/B/A/B, `-snapshot` forcé,
médiane sur n paires) et s'arrête de lui-même si le pré-vol refuse un run.

⚠️ **Le coût du device Screamer sur le temps de boot n'a pas encore été mesuré en A/B.** Il est
instancié dans le macio pour toute machine mac99, donc présent même en mesure headless. Deux
binaires de comparaison sont prêts dans `bench/ab/` (gitignoré) :

```bash
scripts/ab-measure.sh bench/ab/qemu-no-screamer bench/ab/qemu-with-screamer 4
```

À lancer machine au repos. Neutraliser le backend audio (`-audiodev none`) ne change rien, donc
si coût il y a, il ne vient pas du flux audio.

`clock_gettime` vdso lui-même (~5%) = lecture d'horloge fondamentale, laissée telle quelle
(la battre = rdtsc maison, risqué). Prochain gros poste : dispatch TCG
(`tb_lookup`+`qht_lookup`+`helper_lookup_tb_ptr` ~20%).

## Tests

```bash
./tests/run-all.sh          # rapide : syntaxe, shellcheck, doc↔binaire, registres QFB
./tests/run-all.sh --slow   # + qfb_smoke.py et bridge_probe (bootent réellement)
```

Le dépôt est majoritairement du Bash et **les scripts sont l'interface utilisateur** : une
faute dans un lanceur est un bug produit. Le harnais couvre quatre choses qu'aucune relecture
ne garantit :

1. **syntaxe** de tous les scripts shell et Python suivis par git ;
2. **shellcheck** (si installé) ;
3. **cohérence doc ↔ binaire** — on interroge `$QEMU_BIN` (Screamer, qfb-pci, slirp, audio pa)
   au lieu de comparer la doc au dépôt. C'est précisément la classe de bug que deux passes de
   « nettoyage de cohérence » n'avaient pas vue : le README décrivait un binaire qui n'était
   plus celui qui tournait ;
4. **alignement des registres QFB** entre l'hôte (`patches/qfb/qfb-pci.c`) et l'invité
   (`kext/POMPPCQFB/qfb_regs.h`) — une divergence donne un écran corrompu très pénible à
   diagnostiquer ;
5. **GPU paravirtuel** : `qgpu_proto.h` identique au bit près hôte/invité, `qgpu_core_test`
   (cœur + backends soft/gl) compilé et exécuté en natif, trampolines du plugin à jour de leur
   générateur. `--slow` ajoute `qgpu_smoke.py`. Le plugin lui-même se teste dans l'invité
   (`docs/gpu-3d-tiger.md` §5).

## Prochaine étape

Le build source et le profiling sont en place (`scripts/build_qemu_qfb.sh`,
`scripts/profile-boot.sh`) et les deux premières tentatives d'optimisation ont été mesurées puis
revertées. Le prochain gros poste identifié est le **dispatch TCG**
(`tb_lookup` + `qht_lookup` + `helper_lookup_tb_ptr`, ~20 % du temps hôte) : c'est là que se joue
l'optimisation du JIT, avec le même protocole A/B qu'au-dessus.

⚠️ La baseline de 23,32 s a été mesurée avec l'ancien harnais (PID par `pgrep`, sans pré-vol
ni `trap`). **Elle est à refaire** avec `measure-boot.sh` durci avant de servir de référence à
la campagne TCG — c'est le seul chiffre du projet dont la provenance n'est plus vérifiable.

## Carte du dépôt

| Chemin | Rôle |
| --- | --- |
| `config.env` | configuration unique (binaire QEMU, machine, RAM, résolution, chemins) — ne pas éditer les scripts |
| `scripts/` | baseline : création du disque, install, boot, mesure, profiling, build QEMU, `moncmd.py` |
| `run_tiger.sh` / `run_os9.sh` | lanceurs d'usage quotidien (SMP, son, manette, partage, QFB) |
| `mount` / `add-pad` | CD à chaud dans OS 9 ; enregistrement d'une manette dans la règle udev |
| `frontend/` | frontend Dear ImGui embarquant l'affichage QEMU via D-Bus (`frontend/README.md`) |
| `patches/` | patches QEMU et firmware OpenBIOS, **dont le device audio Screamer vendu dans le dépôt** (`patches/screamer/`). Ce qui est appliqué vs. ce qui n'est là que pour référence : `patches/README.md` |
| `kext/POMPPCQFB/` | pilote Tiger du framebuffer QFB (`kext/POMPPCQFB/README.md`) |
| `kext/POMPPCGPU/` | pilote Tiger du GPU paravirtuel qgpu : transport multi-processus (`kext/POMPPCGPU/README.md`) |
| `patches/qgpu/` | device QEMU `qgpu-pci`, cœur d'exécution, backends logiciel et **OpenGL** (rendu sur le GPU hôte), contrat `qgpu_proto.h` |
| `guest/gldriver/` | **plugin OpenGL de Tiger** et son installateur (`guest/gldriver/README.md`) |
| `guest/gltest/`, `guest/qgpu-test/` | programmes de test à compiler dans l'invité (GL hors écran et fenêtré ; transport seul) |
| `guest/verify.sh`, `scripts/verify-kext-in-guest.py` | première vérification du kext à l'aveugle (single-user) |
| `tools/` | rétro-ingénierie PPC, générateur de trampolines, boucle de développement dans l'invité (`tools/README.md`) |
| `tests/qgpu_core_test.c`, `tests/qgpu_smoke.py` | tests du GPU paravirtuel : natif hôte (soft + gl), et bout en bout depuis Open Firmware |
| `docs/gpu-3d-tiger.md` | GPU 3D : architecture, rétro-ingénierie d'OpenGL.framework, protocole, mesures |
| `docs/todo-gpu-3d.md` | GPU 3D : ce qui reste à faire à court terme |
| `docs/roadmap-opengl15.md` | GPU 3D : feuille de route OpenGL 1.5, Quartz Extreme et Core Image |
| `tests/run-all.sh` | **harnais de non-régression** : syntaxe shell/Python, shellcheck, cohérence doc↔binaire, alignement des registres QFB hôte/invité. `--slow` ajoute les tests qui bootent réellement |
| `tests/qfb_smoke.py` | test de bout en bout du device QFB, sans invité (dont la non-régression du scanout débordant) |
| `scripts/web-proxy.py`, `guest/net/proxy.sh` | Internet sous Tiger : relais HTTPS→HTTP sur l'hôte, réglage du proxy dans l'invité |
| `scripts/caps.sh` | sondage des capacités réelles d'un binaire QEMU (QOM), partagé par les lanceurs, le build et les tests |
| `scripts/ab-measure.sh` | A/B interleavé entre deux binaires QEMU, médiane de `CPU_qemu` |
| `docs/gpu-tiger-4060ti.md` | étude GPU 2D : passthrough, paravirtualisation, plan par phases |
| `pack/` | règle udev pour le passthrough manette |
| `disks/extras/` | petits pilotes tiers pour OS 9 (virtio, tablette USB) — `disks/extras/README.md` |
| `disks/`, `images/`, `shared/`, `bench/` | données locales, gitignorées (sauf `disks/extras/`) |
