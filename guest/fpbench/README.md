# `fpbench` — le flottant PowerPC sous QEMU : preuve d'équivalence et vitesse

Deux outils qui tournent **dans l'invité** Tiger 10.4.6 (PowerPC, G4 7400) :

| outil | rôle |
|---|---|
| `fpcheck` | test **différentiel** déterministe : hache tous les résultats et tous les bits du FPSCR, une ligne par section, pour que deux exécutions se comparent par `diff` |
| `fpbench` | **banc de vitesse** : un noyau par ligne, itérations fixes, somme de contrôle sur les bits des résultats |

Ils servent à vérifier et à chiffrer le mode « flottant rapide » de QEMU
(`-cpu g4,x-fast-fp=on`), qui fait passer les calculs par la FPU de l'hôte quand
`FPSCR[XX]` vaut déjà 1, `FPSCR[XE]` vaut 0 et l'arrondi est au plus proche.
La promesse du patch : **tous les résultats et tous les bits du FPSCR identiques
au mode exact, sauf FI et FR** (« inexact » et « arrondi » de la *dernière*
opération, non collants).

## Compiler et lancer

Dans l'invité (Xcode Tools installés, cf. `tools/guest/jobs/xcode`) :

```sh
make                 # /usr/bin/gcc-4.0 -arch ppc -mcpu=7400 -O2 -isysroot 10.4u -faltivec
./fpcheck 4000       # 4000 itérations aléatoires par section
./fpbench            # mesures ; ./fpbench --calibrer pour recalibrer les itérations
```

Depuis l'hôte, par la boucle de développement :

```sh
export DEVDISK=disks/tiger-dev.raw
python3 tools/guest/devloop.py start                  # single-user
tools/guest/jobs/stage.sh fpbench /tmp/j
python3 tools/guest/devloop.py run /tmp/j --timeout 900
python3 tools/guest/devloop.py shutdown               # arrêt SÛR
```

Pour rejouer avec un autre binaire QEMU et le mode rapide, seul le démarrage
change :

```sh
QEMU_BIN=~/src/qemu-fastfp/build/qemu-system-ppc CPU_OPTS=x-fast-fp=on \
    python3 tools/guest/devloop.py start
```

## `fpcheck` — lire la sortie

Une ligne par section :

```
section                    n_ops  h_res             h_fpscr_sans_FI_FR  h_fpscr_sans_FI_FR_FX  h_fpscr_strict
```

Quatre hachages pour localiser un écart sans avoir à rejouer :

| ce qui bouge entre deux exécutions | verdict |
|---|---|
| `h_res` | **grave** : le calcul diffère (bits de résultat, CR d'une comparaison, VSCR) |
| `h_fpscr_sans_FI_FR` **et** `h_fpscr_sans_FI_FR_FX` | **grave** : un bit collant du FPSCR diffère (XX, OX, UX, ZX, VX…) |
| `h_fpscr_sans_FI_FR` seul (pas `..._FX`) | seul **FX** diffère — *toléré*, voir la nuance ci-dessous |
| `h_fpscr_strict` seul | seuls **FI/FR** diffèrent — *toléré*, c'est la promesse du patch |

### La nuance FX

Le QEMU d'aujourd'hui pose **FX à chaque opération inexacte**, même quand XX
valait déjà 1 ; le vrai matériel ne pose FX que sur la transition 0→1 d'un bit
d'exception, et le mode rapide ne touchera pas FX dans ce cas. Un programme qui
**efface FX seul en laissant XX** (`mtfsb0 0`) peut donc voir les deux modes
diverger sur FX. La section `fx-efface-seul` est là pour ça, **isolée** : un
écart sur cette ligne-là ne contamine aucune autre.

### Sections `-info`

Elles portent sur des bits ou des résultats **architecturalement indéfinis** :
un écart y est admissible, il ne prouve rien.

* `fctiw-64b-info` — les 32 bits **hauts** du FPR rendu par `fctiw`/`fctiwz`
  sont indéfinis ; toutes les autres sections `fctiw-*` ne hachent que les
  32 bits bas, seuls définis.
* `s-bin-*-info`, `s-fma-*-info` — les instructions simple précision
  (`fadds`…) ont un résultat indéfini quand un opérande n'est pas représentable
  en simple (PEM). Les sections simple précision *normatives* n'utilisent que
  des opérandes représentables (`G_SING`).
* `lfs-stfs-bits-info` — `stfs` d'une valeur hors du domaine du simple est
  également indéfini (et QEMU, contrairement au matériel, peut y toucher au
  FPSCR).

### Couverture

**Instructions.** `fadd fsub fmul fdiv` et `fadds fsubs fmuls fdivs` ;
`fmadd fmsub fnmadd fnmsub` et leurs formes `s` ; `fres frsqrte` ; `frsp` ;
`fctiw fctiwz` ; `fcmpu fcmpo` (champ CR **et** FPSCR) ; `fsel` ;
`fabs fneg fnabs fmr` ; `lfs stfs` ; `mffs mtfsf mtfsfi mtfsb0 mtfsb1 mcrfs`.
AltiVec : `vaddfp vsubfp vmaddfp vnmsubfp vrefp vrsqrtefp vmaxfp vminfp
vcmpeqfp vcmpgtfp vcmpgefp vcmpbfp vcfsx vcfux vctsxs vctuxs mfvscr mtvscr`.

> Le G4 7400 n'a **pas** `fsqrt`/`fsqrts` (sqrt est dans libm), ni `fcfid`/`fctid`
> (64 bits) : ils ne sont pas testés parce qu'ils n'existent pas.

**Opérandes.** PRNG déterministe (xorshift 64 bits, graine dérivée du nom de la
section — ajouter une section ne décale pas les autres) et six familles :
motifs de bits 64 bits quelconques (beaucoup de NaN, d'infinis et de dénormaux),
normaux d'exposant borné, valeurs exactement représentables en simple, cas de
mi-arrondi vers le simple (et leurs voisins immédiats), voisinage des bornes de
`fctiw`, dénormaux double et simple. Plus une table de **46 cas limites**
(±0, ±inf, QNaN/SNaN avec charges utiles, plus petit/plus grand dénormal,
`DBL_MIN`/`DBL_MAX`, `FLT_MIN`/`FLT_MAX` en double, 2^-149, 2^128, `1+2^-24`…)
jouée en **produit croisé complet** (46 × 46) pour les binaires, les `fma` et
les comparaisons — c'est là que se trouvent 0/0, inf−inf, x/0, débordements et
soupassements.

**Le chemin rapide du patch.** Sans ces sections-là, le test ne prouverait rien
du mode rapide (`docs/flottant-rapide.md` §7) : le générateur `G_SINGN` ne rend
que des **float32 zéro ou normaux** — exactement ce qu'un `lfs` charge dans du
code compilé en `float`, et la seule forme d'opérande qui emprunte le chemin
rapide `float64r32_*`. Sections `*-singn` avec XX = 1 et arrondi au plus proche
(amorçage armé), avec XX = 0 (jamais armé) et avec RN ≠ 0 (jamais armé) ;
`*-oe1`, `*-ue1`, `*-oe1ue1` (le re-biaisage interdit l'amorçage) ;
`transition-xx` (suite délibérément exacte — puissances de deux — puis la
première opération inexacte : XX doit basculer au même rang dans les deux
modes) ; `xx-bascule` (`mtfsb0 6` désarme, `mtfsb1 6` ré-arme, en pleine
rafale) ; `ox-ux-*` (produit croisé de 16 valeurs extrêmes : `FLT_MAX*FLT_MAX`
→ OX, `FLT_MIN/2^30` → UX, résultats dénormaux, qui doivent tous repartir au
logiciel).

**États du FPSCR.** Les 4 modes d'arrondi (champ RN) ; des sections partant de
XX = 0 ; des sections avec XX déjà à 1 (le chemin rapide) dans les 4 modes
d'arrondi ; XE = 1 avec XX = 0 et avec XX = 1 (sans risque : Mac OS X laisse
MSR[FE0/FE1] à 0, aucune trappe n'est prise) ; NI = 1 ; XX effacé **au milieu**
d'une rafale par chacune des quatre instructions possibles (`mtfsb0`, `mtfsf`,
`mtfsfi`, `mcrfs`) ; la section isolée « FX effacé seul » ; une section qui
manipule le FPSCR pour lui-même (`mtfsb0/1`, `mtfsfi`, `mtfsf`, `mcrfs` sur deux
champs). Le FPSCR est lu par `mffs` **après chaque opération** dans la plupart
des sections, et **seulement en fin de rafale** dans les sections `rafale-*` et
`*-rafale-*` (le cas des jeux).

**AltiVec.** Mode Java (`VSCR[NJ]=0`) et non-Java (`NJ=1`), avec des opérandes
normaux, des motifs de bits quelconques et des dénormaux ; VSCR (dont SAT) haché
après chaque opération, et FPSCR vérifié immobile (les opérations vectorielles
ne doivent pas y toucher).

### Pourquoi de l'assembleur en ligne partout

gcc 4.0 replie les constantes, fusionne `mul`+`add` en `fmadd`, transforme une
division par une constante en multiplication et réordonne librement. Chaque
instruction sous test passe donc par un `__asm__ __volatile__` qui nomme le
mnémonique : c'est la seule façon de savoir ce qui s'exécute, et l'attribut
`volatile` garantit en plus l'ordre relatif des opérations et des `mffs`. Les
opérandes sont fabriqués **avant** la boucle mesurée, pour qu'aucun `lfs` ni
aucune conversion du générateur ne s'intercale entre une opération et la lecture
de son FPSCR.

## `fpbench` — lire la sortie

```
noyau              secondes   iterations  somme_de_controle
entier-temoin         2.104      6000000   a1b2c3d4e5f60718
```

Les itérations sont **fixes** (table `K[]` en tête de `main`) : les secondes de
deux modes sont donc directement comparables, et la somme de contrôle — un
hachage des **bits** des résultats — prouve que les deux modes ont calculé la
même chose. `fpbench --calibrer` remesure et propose les itérations donnant ~2 s
par noyau (utile si l'hôte ou le binaire QEMU changent beaucoup) ;
`fpbench 50` divise toutes les itérations par deux.

Les noyaux :

| noyau | ce qu'il exerce |
|---|---|
| `entier-temoin` | **aucun flottant** — témoin : ne doit pas bouger d'un mode à l'autre ; mesure la charge de l'hôte |
| `mdct-papillons` | papillons MDCT en `float` sur tableaux, à la `mdct_butterfly_generic` de libvorbis |
| `fft-inverse` | FFT inverse radix-2 de 128 points complexes en `float` + post-rotation MDCT |
| `mat4-double` | produit de matrices 4×4 en `double` (moteur 3D) |
| `horner-double` | Horner, polynôme de degré 15, `double` |
| `div-double`, `div-float` | `fdiv` / `fdivs` |
| `fmadd-double`, `fmadd-float` | `fmadd` / `fmadds` (gcc fusionne sur PowerPC) |
| `conv-fp-entier` | conversions flottant ↔ entier (`fctiwz`/`stfiwx`, et la ruse 0x43300000 pour entier → double) |
| `lfs-stfs` | recopie par `lfs`/`stfs`, instructions imposées par assembleur en ligne |
| `altivec-vmadd` | `vec_madd` (`vmaddfp`) sur 64 vecteurs |
| `libm-melange` | `sin`, `cos`, `sqrt`, `pow` |

Les données restent **bornées** par construction (récurrences à point fixe,
recharges, facteurs d'échelle) : ni infini, ni NaN, ni dérive vers les
dénormaux, qui fausseraient les temps sans rien prouver de plus. Les lignes
`# fpscr…` donnent l'état du FPSCR à l'entrée, après le noyau entier et à la
sortie : le chemin rapide ne s'arme que lorsque XX vaut 1, ce qui arrive dès la
première opération inexacte.

## Ce que le job range dans `out/`

`tools/guest/jobs/fpbench/job.sh` : `build.txt`, `env.txt`, `fpcheck-1.txt`,
`fpcheck-2.txt`, `fpcheck-diff.txt` (vide = déterministe), `fpbench-1..3.txt`.
Variables : `FPCHECK_N`, `FPCHECK_RUNS`, `FPBENCH_RUNS`, `FPBENCH_PCT`,
`FPBENCH_CAL=1` (passe de calibrage), `NOALTIVEC=1`.

## Isoler un écart : `fpcheck --vidage SECTION`

Quand une colonne de hachage diffère entre deux modes, le hachage ne dit pas
*où*. `fpcheck --vidage <section>` rejoue tout mais détaille la section nommée,
opération par opération, avec `N = 6` :

```
D <section> OPER    0 <a> <b> <c>     les opérandes (les 24 premiers triplets)
D <section>     0 R 3ff0000000000000  un résultat (rang = n_ops courant)
D <section>     1 F 82004000          le FPSCR lu juste après
```

Un `diff` des deux vidages donne le **rang de la première opération fautive** ;
les lignes `OPER` donnent ses opérandes ; les lignes `F` encadrantes donnent le
FPSCR avant et après. L'ordre des opérations dans une itération est celui du
`run_*` correspondant dans la source.

## Ce que le mode rapide change, et ce qu'il ne change pas

Mesuré dans l'invité le 19/09/2026 (voir `bench/fpbench/`), 120 sections,
2 402 208 opérations, `-cpu g4` contre `-cpu g4,x-fast-fp=on` sur le **même**
binaire QEMU :

| | sections |
|---|---|
| identiques sur les 4 colonnes | 50 |
| seul FI diffère (`h_fpscr_strict`) | 45 |
| FI **et** FX diffèrent | 25 |
| **résultats qui diffèrent** | **0** |
| **bits collants du FPSCR qui diffèrent** | **0** |

Les 25 sections à FX sont exactement celles où l'amorçage est armé (XX = 1,
XE = OE = UE = 0, arrondi au plus proche). Les sections `*-xe1`, `*-oe1`,
`*-ue1` sont **identiques sur les 4 colonnes** : l'amorçage ne s'y arme pas,
comme annoncé. Les sections `av-*` (AltiVec) aussi. En `SMP=2` (MTTCG, binaire
`ppc64`) la sortie est identique au bit près à celle d'un seul cœur, dans les
deux modes.

`--vidage fx-efface-seul` donne le mécanisme à l'opération près : après
`mtfsb0 0` (FX effacé, XX laissé à 1), l'opération inexacte suivante rend
`82028000` en exact et `02028000` en rapide — **même résultat, FX en moins**,
jamais l'inverse.

Vitesse (médiane de 6 passes, hôte au repos) : le témoin entier ne bouge pas
(×1,00), le gain va de **×1,34** (conversions) à **×2,95** (`fdivs`), ×2,1 à
×2,6 sur les noyaux qui ressemblent à libvorbis. Le **mode exact du binaire
patché n'égale pas le binaire d'origine en vitesse** : −25 % sur les noyaux
simple précision, mais **+2 à +7 % (plus lent)** sur `mat4-double`,
`horner-double`, `div-double`, `fmadd-double` et `altivec-vmadd` (intervalles
disjoints sur 6 passes).

## Repères de la phase 1 (QEMU actuel, mode exact, 19/09/2026)

Référence rangée dans `bench/fpbench/stock/` (gitignoré). `fpcheck 4000` :
**98 sections, 2 002 688 opérations**, deux exécutions **identiques au bit près**
(`fpcheck-diff.txt` vide). `fpbench` : 13 noyaux entre 1,34 s et 1,97 s, sommes
de contrôle identiques sur trois passes, temps reproductibles à ~1 %. Le job
entier (compilation comprise) dure 77 s.

Trois régularités de la référence, qui valent contrôle de cohérence :

* `edge-bin-rn0`, `edge-bin-xx1` et `edge-bin-xe1` partagent le même
  `h_res` — XX et XE ne changent pas les résultats, seulement le FPSCR ;
* `fsel-fabs-fneg`, `lfs-stfs-*` et toutes les sections `av-*` ont leurs
  **trois** colonnes FPSCR égales : ces instructions n'y touchent pas ;
* dans `fcmp-*` et `edge-cmp-rn0`, `h_fpscr_strict` = `h_fpscr_sans_FI_FR` :
  les comparaisons ne posent ni FI ni FR.

## Et sur les vrais jeux : `tools/guest/jobs/fpgames`

`fpbench` mesure des noyaux ; `tools/guest/jobs/fpgames/job.sh` mesure Marble
Blast Gold et Zenerchi en mode bureau (`devloop.py start --gui`, `SMP=2
SND=none`, les conditions de l'utilisateur). Trois différences avec le job
`games`, imposées par ce qu'on mesure (le processeur émulé, pas le rendu) : un
**lancement de chauffe** d'abord (le premier lancement de Marble Blast compile
ses scripts `.cs` en `.dso` — il mesure l'installation, pas le jeu), une
**durée longue** (`DUR=300`) pour atteindre la démo qui se joue toute seule, et
pour Zenerchi une mesure du **temps de chargement** — le jeu décode toutes ses
musiques Ogg Vorbis avant d'afficher son menu.

Le temps de chargement se reconstruit **sans toucher au plugin**, à partir du
seul bilan `POMPPC_GL_STATS` : chaque fenêtre dure `images / (img/s)` secondes,
exactement, et la première ligne est écrite à la fin de la première fenêtre.
D'où la première image rendue (`t_première_ligne − durée_fenêtre_1`) puis, en
cumulant les durées, l'instant de la première fenêtre dont le nombre de
triangles par image est celui du menu (26 pour Zenerchi, contre 4 pour le logo
d'ouverture). Les images/s de Marble Blast se comparent par **fenêtres
appariées par triangles/image à ±2 %** (`docs/gpu-3d-tiger.md` §4.7) : ce sont
alors les mêmes images des deux côtés.

## Comparer deux modes

```sh
diff bench/fpbench/stock/fpcheck-1.txt bench/fpbench/rapide/fpcheck-1.txt
```

Aucune ligne de différence, ou seulement la colonne `h_fpscr_strict` (et au pire
`h_fpscr_sans_FI_FR` sur la seule ligne `fx-efface-seul`) : la promesse tient.
Toute autre différence est un écart de calcul.
