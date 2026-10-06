# Le binaire rapide du PC (`build-fast/`, 06/10/2026)

PC Linux x86-64 seulement. macOS (le M4) n'est pas concerné : rien n'y change.

## 1. Pourquoi

La compilation de QEMU est le plus gros levier mesuré sur le PC, sans une ligne de code de
l'émulateur (`docs/vitesse-doom3-x86.md` §5.5, §9, §11) : `-O3 -march=native`, sans les
durcissements de QEMU, LTO et PGO font passer DOOM 3 de ~150 à **115 ms/image (−23 %)**, et les
trois autres jeux de la matrice gagnent 18-19 %, images justes. Jusqu'ici ce binaire restait une
« variante » (`QEMU_OPT`) dans un arbre séparé (`~/src/qemu-d3tcg/build-pgo3`), que le lanceur
ne prenait que par `QEMU_BIN=…`. Il devient le binaire quotidien du PC :

| dossier | compilation | rôle |
|---|---|---|
| `~/src/qemu/build/` | `-O2`, durcissements de QEMU, x86-64 générique | **binaire de référence** : A/B, preuves, bancs, paquet publié |
| `~/src/qemu/build-fast/` | `-O3 -march=native`, sans durcissements, LTO, PGO | **binaire rapide** : ce que `run_tiger.sh` lance au quotidien |

Les deux viennent du même arbre et de la même série de patches, posée une seule fois. La
justesse ne dépend pas des options (ni `-ffast-math` ni rien qui touche au flottant ; les
épreuves de `docs/vitesse-doom3-x86.md` §7 ont tourné sur les deux).

## 2. Construire

    QEMU_FAST=1 ./scripts/build_qemu_qfb.sh      # (ou --fast) build/ PUIS build-fast/
    QEMU_FAST=only ./scripts/build_qemu_qfb.sh   # build-fast/ seulement

`build-fast/` est construit en `QEMU_OPT=native,nohard,lto[,pgo]` (`-Doptimization=3` : un `-O3`
dans les `CFLAGS` serait écrasé par le `-O2` de meson), avec le profil PGO s'il existe :

- **profil** : `PGO_DIR`, par défaut `~/src/qemu/pgo-fast/` (stable, hors du dossier de build ;
  `pomppc-profil.txt` y dit d'où il vient). Absent ⇒ construction **sans PGO, avec un
  avertissement** (−8 % au lieu de −13 % en `-O2` le 03/10, §5.5) ; `QEMU_FAST_PGO=use` l'exige,
  `=non` s'en passe ;
- les `.gcda` portent le chemin de l'objet (`#home#…#qemu#build-fast#…`) : un profil d'un autre
  arbre ou d'un autre dossier serait ignoré en silence, le script refuse et renvoie à
  `tools/tcg/pgo-train.sh --importer` ;
- `-Wno-error=coverage-mismatch -fprofile-partial-training` : un profil pris sur des sources un
  peu différentes sert aux fichiers inchangés, les autres sont compilés sans profil ;
- **une seule édition de liens à la fois** (`-Dbackend_max_links=1`, `LTO_JOBS=6` processus
  `ltrans`), et refus si une autre liaison LTO tourne déjà sur la machine (`lto1` ;
  `QEMU_FAST_FORCE=1` pour passer outre).

Le contrôle de capacités (`scripts/caps.sh`) est fait sur `build-fast/` comme sur `build/` :
`bench/build-capabilities-build-fast.txt`, et un **relevé à côté du binaire**,
`build-fast/pomppc-build.txt` (variante, profil, processeur, `-march` résolu, empreinte de la
série de patches, `complet=oui|non`), que lit le lanceur. Les firmwares sont trouvés comme pour
`build/` : chaque dossier de build meson a son `qemu-bundle/`.

**Piège mémoire** (gel du PC le 03/10) : deux liaisons LTO+PGO simultanées, 16 `ltrans`
chacune, ont saturé 40 Go de RAM et le swap. Le script ne lie plus qu'un binaire à la fois, mais
construire sous plafond reste prudent — le noyau tue alors la construction au lieu de geler la
machine :

    systemd-run --user --scope -p MemoryMax=20G -p MemorySwapMax=1G \
        nice -n 10 env QEMU_FAST=1 ./scripts/build_qemu_qfb.sh

(`tools/tcg/pgo-train.sh` le fait tout seul.)

## 3. Ce que lance `run_tiger.sh`

Sur Linux x86-64 (`scripts/qemu_fast.sh`, appelé avant tout sondage) :

1. `QEMU_BIN=…` explicite prime toujours (« binaire imposé (QEMU_BIN) ») ;
2. `QEMU_FAST=0` : binaire de référence, sans rien sonder ;
3. sinon `QEMU_FAST_BIN` (`config.env`, défaut `~/src/qemu/build-fast/qemu-system-ppc`, et son
   `…64` dès SMP ≥ 2) est pris s'il passe **tous** ces contrôles : relevé présent et
   `complet=oui` ; pas l'instrumenté (`pgo-gen`) ; construit sur **ce** processeur (un binaire
   `-march=native` meurt sur SIGILL ailleurs) ; même empreinte de série que les `patches/` du
   dépôt (sinon la référence a été reconstruite sans lui) ; QEMU `POMPPC_QEMU_VERSION` ;
   Screamer, `qgpu-pci` et `-smp N` sondés sur le binaire qui sera lancé ;
4. écarté ⇒ avertissement et binaire de référence ; avec `QEMU_FAST=1`, refus net.

La bannière dit lequel : `▶ Tiger (QEMU 11.1.2, binaire rapide (PGO, -O3, -march=native)) : …`
(ou `binaire rapide (-O3, -march=native, LTO, sans PGO)`, `binaire de référence`, `binaire
imposé (QEMU_BIN)`), suivie de `binaire : <chemin>`.

Restent sur la référence : tous les autres scripts qui lisent `config.env` (bancs, preuves,
`run_os9.sh`, `tests/run-all.sh` §4), et **chaque bras de `tools/tcg/matab.sh`**, qui part avec
`QEMU_FAST=0` : un bras `"ref:"` est la référence, un bras rapide se demande explicitement
(`"fast:QEMU_BIN=~/src/qemu/build-fast/qemu-system-ppc"` ou `"fast:QEMU_FAST=1"`). Un paquet
publié (`bin/` à la racine) garde ses binaires : `scripts/package_release_linux.sh` empaquette
toujours `build/` et refuse un `build/` compilé en natif ou avec un profil.

## 4. Entraîner le profil

    tools/tcg/pgo-train.sh --dry-run    # montre les commandes, ne lance rien
    tools/tcg/pgo-train.sh              # gen, jouer, use

1. **gen** : profil précédent archivé (`pgo-fast.<date>`), `build-fast/` instrumenté
   (`QEMU_FAST=only QEMU_FAST_PGO=gen` : `-fprofile-generate`, `-fprofile-update=prefer-atomic`
   pour MTTCG), puis profil vidé (le contrôle de capacités a lancé l'instrumenté) ;
2. **jouer** : charge MIXTE sur la VM quotidienne, par `tools/tcg/matab.sh` (bras
   `train:QEMU_BIN=<build-fast>`, matrice sans vidage) : Marble Blast, Zenerchi, UT2004 et DOOM 3
   en fenêtre (`--jeux`, `--mode`, `--parties N`). Les `.gcda` s'écrivent quand QEMU quitte :
   matab éteint l'invité proprement. Contrôle : au moins 500 `.gcda` (le 03/10 : 1 356 pour 25 min
   de DOOM 3 seul, la scène jamais atteinte, l'instrumenté étant lent : sans importance pour le
   profil). Hôte au repos : le script refuse cette étape si une autre construction tourne
   (`--force`) ;
3. **use** : `build-fast/` reconstruit avec le profil (`QEMU_FAST_PGO=use`).

Les constructions tournent en `nice -n 10` sous `systemd-run --user --scope -p MemoryMax=20G
-p MemorySwapMax=1G` (`PGO_MEM`), journaux et durée/mémoire max (`/usr/bin/time -v`) dans
`bench/vitesse/pgo-fast/<date>/`.

**Profil d'un arbre voisin** (§5.5 de `docs/vitesse-doom3-x86.md` : réentraîner n'avait rien
apporté de mesurable sur la copie TCG, −0,5 %) :

    tools/tcg/pgo-train.sh --importer ~/src/qemu-d3tcg/pgo-data3   # renomme les .gcda
    tools/tcg/pgo-train.sh --etapes use                            # reconstruit avec

(`--de <préfixe>` si la devinette du dossier de build d'origine se trompe.)

**Quand réentraîner** : à chaque changement notable de la série (nouveau patch du traducteur,
de `target/ppc`, de `tcg/` ou des devices chauds), et au plus tard quand la construction
prévient « profil pris sur une autre série de patches ». Un profil vieilli ne casse rien : les
fichiers changés sont compilés sans profil, le gain s'effrite.

## 5. Après un changement de la série

    QEMU_FAST=1 ./scripts/build_qemu_qfb.sh   # référence puis rapide, avec l'ancien profil

Sans cette reconstruction, le lanceur écarte `build-fast/` (« autre série de patches ») et
retombe sur la référence : jamais un binaire rapide en retard sur la série en silence.

## 6. Mesures

**Validation de la mécanique (06/10, arbre voisin `~/src/qemu-opt`, sans toucher à
`~/src/qemu`)** : profil `~/src/qemu-d3tcg/pgo-data3` importé (`--importer`, 1 418 `.gcda`
renommés `#qemu-d3tcg#build-pgo3#` → `#qemu-opt#build-fast#`), série 0021-0028 posée sur l'arbre
par le script, puis `QEMU_SRC=~/src/qemu-opt tools/tcg/pgo-train.sh --etapes use` : **15 min 18 s**
(hôte chargé par deux autres constructions), **1,75 Gio de mémoire max** pour toute la
construction (cgroup, plafond 20 Gio), 1 212 avertissements `-Wcoverage-mismatch` (fichiers
changés depuis le profil, compilés sans lui) et l'avertissement « autre série ». Capacités
toutes présentes (`x-lmw-vector`, `x-jc-word` compris, backend GL), `-M none` démarre, relevé
`complet=oui`, `march=skylake`, et `pomppc_pick_qemu 2` choisit ce binaire en 0,25 s
(« binaire rapide (PGO, -O3, -march=native) »).

**Phase 2, 06/10 (hôte au repos, `~/src/qemu`)** : premier entraînement réel par
`tools/tcg/pgo-train.sh` sur la configuration retenue le jour même (nouveaux défauts de Linux
x86-64 : côté mémoire, `x-jit-rel32`, `x-fp-native-cmp`, `x-vfp-native-cmp`, `x-lmw-vector`,
`x-jc-word`, que portait la bannière du bras d'entraînement). Journaux :
`bench/vitesse/pgo-fast/20261006-1911/`, partie `bench/tcg/ab/pgo-train-20261006-1911`.

| étape | durée | mémoire max (cgroup) | détail |
|---|---|---|---|
| gen | 8 min 04 s | 1 811 Mio | instrumenté complet, profil remis à zéro |
| jouer | 35 min | — | mb, zen, d3, ut en fenêtre sur l'instrumenté (40,2 / 13,2 / 156,1 / 157,5 ms/image), les quatre scènes atteintes ; **1 356 `.gcda`** |
| use | 7 min 46 s | 1 649 Mio | 3 avertissements `-Wcoverage-mismatch` seulement (profil pris sur la série même) |

Relevé `build-fast/pomppc-build.txt` : `variante=native,nohard,lto,pgo`, `complet=oui`,
`march=skylake`, `serie=c9e8afe33871fc49` (celle du dépôt), `pomppc=9e67226` ; profil
`pgo-fast/pomppc-profil.txt` du 06/10 19:55. Capacités (`bench/build-capabilities-build-fast.txt`)
identiques à celles de la référence, 0031-0035 comprises.


**Vérification du lanceur** (06/10, après la reconstruction) : `POMPPC_FRONTEND=native
./run_tiger.sh` sans `QEMU_BIN` ni `QEMU_FAST` prend `build-fast/qemu-system-ppc64` (« binaire
rapide (PGO, -O3, -march=native) »), avec les nouveaux défauts de Linux x86-64 dans la bannière.

**A/B contre la référence : à refaire hôte au repos.** Une seule partie DOOM 3 propre sur
`build-fast` (124,1 ms/image, `bench/tcg/ab/x86-fast-pollue/fast-1`) avant qu'une autre session
de travail n'occupe l'hôte (`docs/vitesse-doom3-x86.md` §13.5) ; commandes de l'A/B et de la
matrice finale au §13.7 de ce document.
