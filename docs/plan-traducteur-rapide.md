# Plan — un traducteur PowerPC beaucoup plus optimisé (second niveau de TCG)

État : rédigé le 25/09/2026, mis de côté, **relancé le 21/10/2026** : phase 0 (mesures, §8) et phase 1 (traces sans compilateur, §9). Point de départ :
`docs/tcg-g4.md` (relevés, profils, patches `x-sr-tlb` et flottants).

---

## 1. Contexte : ce que fait TCG et ce qu'on peut y gagner

### 1.1 Comment QEMU exécute le G4

TCG traduit le code PowerPC **par bloc de base** (un « TB » : jusqu'au premier
branchement, 512 instructions au plus). Il procède en deux étages :

1. le **frontend PowerPC** (`target/ppc/translate.c` et `translate/*.c.inc`) décrit
   chaque instruction en opérations intermédiaires TCG ;
2. le **backend de l'hôte** (`tcg/aarch64/` sur le Mac, `tcg/i386/` sur le PC)
   transforme ces opérations en code machine, avec un optimiseur local
   (`tcg/optimize.c` : propagation de constantes, simplifications) et une allocation
   de registres **limitée au bloc**.

Les blocs s'enchaînent directement quand la cible est connue (`goto_tb`) ; sinon
(`blr`, `bctr`, retour d'interruption) ils passent par `helper_lookup_tb_ptr`, qui
cherche le bloc suivant dans une table.

Ses limites de nature, qu'aucun patch local ne lève :

- **les registres du PowerPC vivent en mémoire** (`CPUPPCState`) : chaque bloc les
  recharge en entrée et les réécrit en sortie, et avant chaque helper qui peut les lire ;
- **aucune optimisation au-delà d'un bloc** : pas de sortie d'invariants de boucle,
  pas d'élimination des calculs de `CR`/`XER` que personne ne lit, pas d'allocation
  de registres sur une boucle entière ;
- **chaque accès mémoire** refait la recherche dans le TLB logiciel (`cputlb.c`),
  même dix fois de suite sur la même page ;
- **aucun niveau supérieur** : un bloc exécuté un milliard de fois garde le code
  produit à la première exécution.

### 1.2 Trois niveaux d'amélioration

| Niveau | Quoi | Exemples chez nous | Gain par chantier | Coût |
|---|---|---|---|---|
| 1 | Traduire une instruction en ligne au lieu d'un helper | `lfs`/`stfs`, AltiVec (agent en cours) | 1-10 % | jours |
| 2 | Supprimer un aller-retour coûteux de l'exécutif | TLB par segments `x-sr-tlb` (+10 % DOOM 3), retours prédits, verrou global | 5-20 % | semaines |
| 3 | **Changer la nature du traducteur** : un second niveau qui recompile les régions chaudes avec un vrai compilateur | **ce plan** | ×1,3 à ×2 sur le code de l'invité (à mesurer, §6) | mois |

Les projets de recherche qui ont fait le niveau 3 sur QEMU (HQEMU, Hong et al.,
CGO 2012 : régions chaudes recompilées par LLVM dans un fil à part) ont rapporté
des gains de l'ordre de ×2 à ×3 sur des bancs de calcul en mode utilisateur — chiffre
à relire dans l'article avant de s'y fier ; un système complet avec son noyau, ses
helpers et ses E/S gagne moins.

### 1.3 Pourquoi l'hôte x86 est moins favorable (le PC du projet)

- **Registres** : PowerPC 32 entiers + 32 flottants ; ARM64 31 + 32 ; x86-64 16 + 16.
  Sur x86 le second niveau aura bien plus de débordements en mémoire.
- **Multiplication-addition fusionnée** (`fmadds`, `vmaddfp`) : native en ARM64 ; sur
  x86-64 seulement avec FMA3, que QEMU n'exige pas.
- **Drapeaux flottants** : chaque hôte a ses différences avec le PowerPC ;
  `x-fast-fp` a été validé sur Apple Silicon, pas encore sur le PC.
- **Point en faveur du x86** : son modèle mémoire (TSO) est plus fort que celui du
  PowerPC, les barrières `sync`/`lwsync` y coûtent peu ; ARM a besoin de vraies
  barrières.

Et dans l'autre sens, émuler **un x86** (ce que fait Rosetta 2) est bien plus dur
qu'émuler un PowerPC : instructions de 1 à 15 octets à décoder, drapeaux posés par
presque chaque instruction, modèle mémoire fort à garantir sur un hôte faible (Rosetta
s'appuie sur un mode TSO matériel des puces Apple), code auto-modifiant sans
avertissement. Le PowerPC sur ARM64 est un cas favorable : instructions fixes,
beaucoup de registres des deux côtés, mêmes opérations flottantes.

---

## 2. Où part le temps aujourd'hui

Relevés de `docs/tcg-g4.md` (Marble Blast, `sample` des fils vCPU, SMP=2, avant et
après `x-sr-tlb`) :

| Poste | Stock | `x-sr-tlb` | Le second niveau le réduit ? |
|---|---|---|---|
| Code généré (self) | 36,8 % | 30,6 % | **oui** : c'est sa cible |
| `helper_lookup_tb_ptr` (branchements indirects) | 22,1 % | 18,4 % | **oui** : régions + retours prédits |
| Remplissage du TLB | 8,6 % | 5,2 % | en partie (moins d'accès vérifiés) |
| `lmw`/`stmw` | 7,0 % | 10,0 % | oui (accès groupés sur une page) |
| Verrou global (BQL) | 2,9 % | 9,2 % | non : chantier de niveau 2 à part |
| Flottant, AltiVec (helpers) | quelques % | | niveau 1 d'abord |

DOOM 3 exécute ~600 M instructions PowerPC par seconde (profil `ppcmix`, §6 bis), dont
80 % traduites en ligne. **Le plafond du second niveau est la part « code généré +
recherche de bloc + TLB »**, soit 50 à 65 % du temps vCPU : même infiniment rapide,
il ne ferait pas mieux que ×2 à ×3 sur le vCPU, et moins sur l'image, où comptent
aussi le GPU, le plugin et le fil d'E/S. D'où la phase 0.

---

## 3. Architecture visée

### 3.1 Principe

Un **second niveau** dans QEMU, à côté de TCG, qui ne remplace rien :

1. **Détecter** les blocs chauds (compteur par TB) ;
2. **former une région** à partir d'un bloc chaud : le graphe des blocs réellement
   enchaînés (boucles comprises), borné en taille et en pages ;
3. **traduire la région** en une seule fonction optimisée, dans un **fil de
   compilation à part** (le vCPU continue au niveau 1 pendant ce temps) ;
4. **installer** le résultat : l'entrée du bloc chaud saute vers le code de niveau 2 ;
5. **revenir au niveau 1** à chaque cas qui sort de l'ordinaire (faute, interruption,
   sortie de la région, invalidation).

### 3.2 Quelle entrée, quel compilateur

| Option | Entrée | Compilateur | Pour | Contre |
|---|---|---|---|---|
| A. Superblocs TCG | ops TCG de plusieurs TB | TCG lui-même, allocateur étendu | pas de dépendance ; reste « du QEMU » | l'allocateur et l'optimiseur de TCG sont locaux par conception : il faudrait les réécrire, c'est déjà un compilateur neuf |
| **B. Ops TCG → LLVM** | ops TCG de la région (le frontend PowerPC est **réutilisé tel quel**) | LLVM ORC JIT, fil à part | allocation globale, boucles, CSE, vectorisation ; toute la sémantique PowerPC déjà décrite en ops TCG ; la voie d'HQEMU | dépendance LLVM (Homebrew sur Mac, paquets sur le PC) ; latence de compilation (cachée par le fil) ; taille |
| C. Frontend PowerPC neuf → IR à nous | instructions PowerPC | IR SSA maison | contrôle total | réécrire 6 700 lignes de sémantique : exclu |

**Recommandation : B.** Traduire les **ops TCG** (et non les instructions PowerPC)
garde un seul endroit où la sémantique du G4 est décrite : tout correctif du frontend,
et nos patches (`x-fast-fp`, `x-sr-tlb`, `lfs` en ligne), profitent aux deux niveaux.
Les helpers restent des appels (déclarés `readnone`/`readonly` quand leurs drapeaux
TCG le permettent), les ops vectorielles `gvec` deviennent des vecteurs LLVM.

### 3.3 Ce que le niveau 2 optimise

- **Registres de l'invité en registres de l'hôte** sur toute la région ; écrits dans
  `CPUPPCState` seulement aux sorties (et avant les helpers qui les lisent).
- **Champs de `CR`, `XER[CA/OV/SO]`, `FPSCR`** : calculés seulement si lus avant
  d'être réécrits dans la région ou vivants à une sortie.
- **Accès mémoire** : chemin rapide du TLB en ligne (comme TCG) ; dans une boucle, la
  vérification d'une page déjà vérifiée est sortie de la boucle **si** aucune
  instruction de la région ne peut changer la traduction (`mtsr`, `tlbie`, `mtmsr`,
  helpers qui vident le TLB : ces instructions ferment la région).
- **Branchements indirects** : cache en ligne des cibles observées (`blr` vers les 1 à
  3 retours vus) avant de retomber sur `lookup_tb_ptr`.
- Tout le reste vient de LLVM : constantes, invariants de boucle, CSE, déroulage,
  fusion `fmadd`.

### 3.4 Invariants à garder (ce qui rend le projet difficile)

1. **Exceptions précises.** Une faute de page au milieu de la région doit laisser
   exactement l'état PowerPC de l'instruction fautive. Méthode : chaque accès qui peut
   fauter a un **point de sortie** qui réécrit l'état vivant, pose `nip`, et reprend au
   niveau 1 (qui refera l'accès et lèvera la faute normalement). Pas de restauration
   à partir de l'adresse hôte comme TCG le fait (`insn_start`) : le niveau 2 sort
   toujours proprement.
2. **Interruptions et `exit_request`** : vérifiées à chaque arête arrière de boucle
   et à l'entrée de la région, comme TCG en tête de bloc.
3. **Code modifié** : une région est inscrite sur toutes les pages qu'elle couvre ;
   l'invalidation d'une page (mécanisme existant de `tb-maint.c`) la jette et remet
   le saut d'entrée vers le niveau 1. Idem sur `tb_flush`.
4. **MTTCG** : un seul code de niveau 2 partagé par les vCPU (comme les TB) ;
   `lwarx`/`stwcx.` et barrières gardent la sémantique des ops TCG ; l'installation
   du saut d'entrée est atomique.
5. **Drapeaux de bloc** (`tb->flags` : MSR, mode, `hflags`) font partie de la clé de
   la région, comme pour un TB.
6. **Débogage** : `-d in_asm`, `gdbstub` en pas à pas et le greffon `ppcmix`
   désactivent le niveau 2 (`CF_SINGLE_STEP`, greffons présents).

---

## 4. Épreuves (comment on sait que c'est juste)

Le niveau 2 n'est allumé qu'avec ses preuves, comme `x-fast-fp` et `x-sr-tlb` :

- **Mode vérificateur** (`x-tier2-verify=N`) : une région sur N s'exécute aussi au
  niveau 1 depuis le même état, sur une copie ; on compare `CPUPPCState` et la liste
  des écritures mémoire à la sortie. Toute divergence est notée avec l'adresse de la
  région et son code.
- **Bancs en mode utilisateur** : QEMU `qemu-ppc` (Linux utilisateur, même
  `target/ppc`) sur des programmes PowerPC compilés en croisé — suite de tests
  d'instructions, SPEC-like, boucles flottantes et AltiVec. Itérations en secondes,
  sans démarrer Tiger.
- **Tiger** : démarrage complet, `gltest`, Marble Blast, DOOM 3, Prey ; vérificateur
  allumé, zéro divergence ; images identiques.
- **Mesures** : A/B entrelacés à scène égale (`tools/tcg/d3run.sh`,
  `tools/tcg/mbab.sh`), médiane, même binaire à une propriété près.

---

## 5. Phases

Chaque phase a une porte : si l'épreuve échoue ou si le gain mesuré n'y est pas, on
s'arrête là avec un rapport chiffré.

| # | Phase | Livrable | Porte |
|---|---|---|---|
| **0** | **Mesurer le plafond** | compteurs par TB (exécutions, instructions), reconstruction des régions chaudes hors ligne (enchaînements `goto_tb` et cibles indirectes) ; part du temps vCPU dans les 50 / 200 / 1000 régions les plus chaudes sur Marble Blast et DOOM 3 ; part « code généré + lookup + TLB » | **go si** ≥ 70 % du temps vCPU tient dans ≤ 1 000 régions **et** le plafond théorique dépasse ×1,3 sur l'image |
| 1 | Niveau 2 « sans compilateur » | l'infrastructure seule : compteurs, formation de régions, fil de compilation, installation et invalidation — la région est d'abord re-générée **par TCG en superbloc** (option A minimale : blocs concaténés, un seul prologue) | Tiger démarre, vérificateur à zéro ; mesure du gain des seuls superblocs + retours prédits |
| 2 | LLVM en mode utilisateur | ops TCG → LLVM IR, un fil ORC ; régions sans accès faillibles d'abord, puis accès avec points de sortie | suite d'instructions et bancs `qemu-ppc` identiques ; ×1,5 sur les bancs de calcul |
| 3 | LLVM en mode système | TLB logiciel en ligne, sorties précises, interruptions, invalidation, MTTCG | Tiger démarre et joue, vérificateur à zéro sur une heure de jeu |
| 4 | Optimisations propres au PowerPC | `CR`/`XER`/`FPSCR` morts, vérifications de page sorties des boucles, caches de cibles indirectes, `lmw`/`stmw` groupés | chacune avec sa propriété et son A/B |
| 5 | Intégration POMPPC | `patches/tcg/` + dépendance LLVM dans `build_qemu_qfb.sh`, `TIER2=1 ./run_tiger.sh`, `docs/tcg-g4.md` | matrice de jeux verte avec le niveau 2, défaut allumé après une semaine de jeu |
| 6 | Hôte x86 (le PC) | construction et épreuves sur le PC | mêmes portes ; gain probablement moindre (§1.3) |

Ordre de grandeur honnête : la phase 0 prend quelques jours ; les phases 1 à 3 font un
projet de plusieurs mois ; 4 à 6 s'étalent ensuite. La phase 1 seule peut déjà
rapporter (`lookup_tb_ptr` pèse 18-21 %).

### 5 bis. Évaluation du temps et de la difficulté (25/09/2026)

Difficulté de 1 (routine) à 5 (le plus dur du projet). Durées en **travail effectif**
pour une personne qui connaît QEMU ; avec des agents, l'écriture du code va plus vite,
mais les épreuves dans Tiger restent en série (une seule VM de jeu) et le débogage
des défauts rares domine.

| # | Phase | Durée | Difficulté | Ce qui est dur |
|---|---|---|---|---|
| 0 | Mesurer le plafond | 2 à 4 jours | 2/5 | rien de neuf : greffon TCG (comme `ppcmix`) + `sample` ; le plus long est de jouer les scènes |
| 1 | Infrastructure + superblocs + retours prédits | 3 à 6 semaines | 4/5 | toucher au cœur de l'exécutif (`cpu-exec.c`, `tb-maint.c`) : chaînage, invalidation, verrous MTTCG ; les retours prédits seuls : 1 à 2 semaines, 3/5 |
| 2 | LLVM en mode utilisateur | 4 à 8 semaines | 4/5 | traduire ~150 ops TCG et les `gvec` en LLVM IR, appels de helpers, fil ORC ; monter `qemu-ppc` et une chaîne croisée PowerPC sur le Mac |
| 3 | LLVM en mode système | 2 à 4 mois | **5/5** | exceptions précises, TLB logiciel, interruptions, code modifié, deux vCPU : les défauts n'apparaissent qu'après des minutes de Tiger, le vérificateur est indispensable |
| 4 | Optimisations PowerPC | 3 à 6 semaines | 3/5 | chacune est locale, mais doit prouver qu'elle ne change rien (drapeaux morts, pages sorties des boucles) |
| 5 | Intégration POMPPC | 1 à 2 semaines | 2/5 | LLVM comme dépendance de `build_qemu_qfb.sh`, lanceur, doc |
| 6 | Hôte x86 (le PC) | 2 à 4 semaines | 3/5 | un autre backend LLVM (sans surprise), mais d'autres cas flottants et moins de registres ; dépend de l'accès au PC |

**Total** : de l'ordre de **6 à 10 mois** de travail effectif ; en calendrier, avec
des agents sur le code et l'utilisateur sur les parties jouées, plutôt **3 à 6
mois**, sans garantie : la phase 3 peut à elle seule doubler si un défaut de
cohérence mémoire en SMP résiste.

**Points de décision** : après la phase 0 (quelques jours : on sait si le jeu en
vaut la chandelle) ; après la phase 1 (quelques semaines : gain déjà encaissé, sans
LLVM) ; après la phase 2 (on sait ce que LLVM apporte avant d'affronter le mode
système). Le risque est concentré dans la phase 3, et on n'y entre qu'avec deux
mesures favorables en main.

---

## 6. Risques

- **Le plafond est bas** si le temps vCPU est surtout dans le noyau Tiger, les helpers,
  le verrou global et les E/S : la phase 0 tranche avant d'écrire le compilateur.
- **Latence de compilation** : LLVM met des millisecondes par région ; le fil à part
  et un seuil de chaleur élevé la cachent, mais les régions qui changent souvent
  (code JIT de l'invité, rare sous Tiger) ne doivent jamais passer au niveau 2.
- **Invalidations fréquentes** : le noyau Tiger réécrit-il du code (commpage à
  `0xffff8000`, chargement de bibliothèques) ? À compter en phase 0.
- **Divergence de sémantique** entre niveaux : réduite par l'entrée commune (ops TCG)
  et gardée par le vérificateur.
- **Maintenance** : un second compilateur à suivre lors d'une montée de version de
  QEMU. On reste en 9.2 tant que le projet est jeune ; l'amont n'a rien de tel.
- **Le PC x86** gagnera moins, et LLVM y est une dépendance de plus à installer.

---

## 7. Ce que ce plan ne fait pas

- Pas de réécriture du frontend PowerPC ni de TCG : le niveau 1 reste la référence.
- Pas de traduction à l'avance (AOT) des binaires du jeu : impossible proprement en
  mode système (le code vit à des adresses virtuelles qui changent avec le noyau).
- Pas d'accélération matérielle (KVM PowerPC) : aucun hôte n'a de G4.
- Les chantiers de niveau 1 et 2 (flottants, AltiVec, retours prédits, verrou
  global) continuent en parallèle : ils profitent aussi au niveau 2.

---

## 8. Phase 0 : mesures du 21/10

PC Linux x86-64 (i7-10700F), binaire rapide PGO, défauts du 20/10. Sources : le profil hôte DOOM 3
du 21/10 (`bench/tcg/ab/x86-hprof-2110/fast-1/d3-fen/perf-hote-mesure.data`, carte
`/tmp/perf-547754.map`, 30 s, 32 928 échantillons des fils vCPU), et les blocs DOOM 3 retraduits
du journal de la session flottante (`tcglog.txt`, 127 blocs, §37.5 de `docs/tcg-g4.md`).

### 8.1 Les outils

- `tools/tcg/tbedges.c` (greffon TCG) : par bloc (adresse, page physique, longueur) exécutions,
  instructions, mot de la dernière instruction ; par arête bloc → bloc (même vCPU) nombre de
  passages, toutes transitions comprises (chaînées, cache de sauts, boucle). Instantanés
  `snap=` ou en anneau `interval=S,keep=K` (en-tête `t_wall`). Coûte un appel par bloc : forme,
  jamais vitesse. Construction : `QEMU_SRC=~/src/qemu tools/tcg/build.sh tbedges`.
- `tools/tcg/regions.py` : `--perf DATA --map PERFMAP` range chaque échantillon dans son bloc
  (la carte `-perfmap` a une ligne par instruction invité : découpage en blocs et octets de code
  hôte par instruction) ; parts des N blocs / fenêtres les plus chauds, **sans biais
  d'échantillonnage** (classement sur une moitié des échantillons, part mesurée sur l'autre) ;
  `--tb PREFIXE [--tb-base]` ou `--ring PREFIXE --t0 --t1` forme les régions sur les arêtes du
  greffon (graines par poids décroissant, croissance en largeur, boucles comprises, bornes
  `--max-insns 1024 --max-pages 4 --max-blocks 128`) et donne la part des 50/200/1000 régions,
  les transitions restées dans une région et la prédictibilité des sorties indirectes.
- `tools/tcg/codeanat.py JOURNAL [--perf --map]` : anatomie du code x86 émis (capstone), par
  instruction invité : rechargements/rangements de `CPUPPCState`, état négatif (sortie, TLB),
  chemin court du TLB, sauts, calcul.

### 8.2 Chiffres (DOOM 3, scène fixe)

| mesure | valeur |
|---|---|
| code généré | **83,7 %** du temps vCPU (helpers 16,3 %) |
| traductions dans la carte | 1 061 447 |
| code hôte par instruction invité, pondéré par le temps | **105 octets** (médiane 92), soit **~22 instructions x86** (4,74 o/instruction) |
| bloc moyen pondéré | 17,2 instructions invité |

| N plus chauds | blocs (TB) | fenêtres 256 o | fenêtres 1 Kio | pages 4 Kio |
|---|---|---|---|---|
| 50 | 17,0 % | 24,4 % | 34,8 % | 49,2 % |
| 200 | 28,9 % | 45,0 % | 61,4 % | 75,0 % |
| 1000 | 48,5 % | **72,1 %** | **81,1 %** | **82,8 %** |

(part du temps vCPU total, sans biais ; le plafond est 83,7 %, le code généré.)

Anatomie (127 blocs DOOM 3, plutôt flottants) : 32,9 instructions x86 par instruction invité —
rechargements de registres 3,7 %, rangements 5,9 %, état négatif 3,9 %, TLB 7,7 %, sauts
10,5 %, calcul 65,9 %. Par classe : flottant 63 instructions hôte, chargement/rangement 31,
comparaison 17, entier 2,8. Le « calcul » du flottant est la sémantique PowerPC (porte du FPSCR,
drapeaux, conversions), pas des allers-retours.

### 8.3 Plafond

Ce qu'une région compilée (niveau 2 complet, LLVM) retirerait du code couvert : 80 % des
rechargements, 70 % des rangements (registres gardés, rangés aux sorties), la moitié de l'état
négatif et des sauts (une seule tête, pas de sortie entre blocs, recherches prédites), 30 % du
TLB (vérifications sorties des boucles), 20 % du calcul (CR/XER morts, sélection d'instructions) :
**~30 % des instructions hôte** du code couvert, de l'ordre de 20 à 35 % de son temps (les
accès à `env` retirés sont bon marché). Code couvert : ~0,85 × 83,7 % ≈ 71 % du vCPU (1000
régions d'une page). Soit **−15 à −25 % de temps vCPU**, ×1,17 à ×1,33 sur le processeur émulé,
amplifié par les tics de DOOM 3 (§3.1 de `docs/vitesse-doom3-x86.md`, ~×1,15) : **×1,2 à ×1,4
sur l'image, centre ×1,25-1,3**. Borne absolue (code couvert gratuit) : ×3 et plus.

### 8.4 Décision de la porte

- **≥ 70 % du temps vCPU dans ≤ 1000 régions : oui** (approximation par fenêtres : 72 % en
  256 o, 81 % en 1 Kio, 83 % en pages ; des régions formées sur les arêtes couvrent au moins
  autant que des fenêtres contiguës de même taille). La mesure sur arêtes de DOOM 3 reste à
  jouer (§8.5, commande 1).
- **Plafond > ×1,3 sur l'image : à la limite** — haut de fourchette ×1,4, centre ×1,25-1,3.

**Go pour la phase 1** (bon marché, sans compilateur, et qui donne une mesure réelle du gain
« frontières de blocs ») ; **la phase 2 (LLVM) reste conditionnée** aux arêtes de DOOM 3 et au
gain de la phase 1 sur DOOM 3.

### 8.5 Commandes pour la session principale (VM quotidienne, DOOM 3)

    W=~/src/pomppc/.claude/worktrees/agent-t2-phase0   # ou le dépôt après fusion
    # 1. arêtes (greffon tbedges, instantanés toutes les 30 s ; mesure de forme, jeu ralenti)
    mkdir -p ~/src/pomppc/bench/tcg/t2/d3-edges
    EXTRA_ARGS="-plugin $W/tools/tcg/libtbedges.so,out=$HOME/src/pomppc/bench/tcg/t2/d3-edges/e,interval=30,keep=16" \
      tools/tcg/matab.sh t2-d3-edges 1 "edges:QEMU_FAST=1" d3 fen
    python3 $W/tools/tcg/regions.py --ring ~/src/pomppc/bench/tcg/t2/d3-edges/e \
      --perf bench/tcg/ab/x86-hprof-2110/fast-1/d3-fen/perf-hote-mesure.data --map /tmp/perf-547754.map
    # 2. code x86 des 400 blocs les plus chauds + nouveau profil
    EXTRA_ARGS="-perfmap -d in_asm,out_asm -dfilter $(cat ~/src/pomppc/bench/tcg/t2/d3-dfilter.txt) -D $HOME/src/pomppc/bench/tcg/t2/d3-asm.log" \
      MATRICE_OPTS="--sample-hote 30" tools/tcg/matab.sh t2-d3-asm 1 "asm:QEMU_FAST=1" d3 fen
    python3 $W/tools/tcg/codeanat.py ~/src/pomppc/bench/tcg/t2/d3-asm.log --perf <perf-hote-mesure.data> --map /tmp/perf-<pid>.map --top 20
    # 3. A/B des traces (phase 1), même binaire -O2 des deux côtés
    QEMU_BIN=$HOME/src/qemu-t2/build/qemu-system-ppc tools/tcg/matab.sh t2-d3-ab 3 \
      "ref:CPU_OPTS=x-tier2=0" "t2:CPU_OPTS=x-tier2=4096" d3 fen
    # 4. une partie vérifiée (pas pour la vitesse) : bilan « x-tier2-verify » dans run_tiger.log
    QEMU_BIN=$HOME/src/qemu-t2/build/qemu-system-ppc tools/tcg/matab.sh t2-d3-verif 1 \
      "v:CPU_OPTS=x-tier2=4096,x-tier2-verify=64" d3 fen
    tools/tcg/matab.sh --restore

(Après fusion : `TIER2=4096` / `TIER2VERIFY=64` au lieu de `CPU_OPTS`.)

## 9. Phase 1 : traces sans compilateur (`tcg/0048`, `x-tier2`), 21/10

### 9.1 Ce qui est fait

`patches/tcg/0048-tcg-tier2.patch` (appliqué par `scripts/build_qemu_qfb.sh`, sondé
`x-tier2` dans les capacités, `TIER2=N` dans `run_tiger.sh`, **éteint par défaut**) :

- **chaleur** : chaque TB compte ses exécutions en tête (6 instructions hôte) ; à la N-ième,
  `helper_t2_hot` le note et demande une sortie de chaîne ;
- **formation** (`t2_form`, boucle d'exécution, fil du vCPU) : on suit les sorties directes
  chaînées par TCG (`jmp_dest[0]` pris / `[1]` non pris, le successeur le plus exécuté), dans la
  page du bloc chaud, sans revisiter un bloc (une boucle se referme sur la tête), ≤ 16 blocs,
  ≤ 384 instructions ;
- **traduction** : le chemin retraduit en UN TB (« trace ») ; le frontal PowerPC continue à
  travers `b`/`bl`/`bc` du chemin (condition en une valeur, un `brcond` vers la sortie latérale,
  aucune étiquette sur le chemin : TCG garde les registres invité d'un bloc à l'autre), sorties
  latérales émises après la dernière instruction (emplacement `goto_tb` libre, sinon cache de
  sauts) ;
- **installation / invalidation** : le bloc chaud est invalidé, la trace prend sa clé (même pc,
  drapeaux, cflags) ; elle couvre toute sa page pour l'invalidation ; `tb_flush` oublie la
  demande en cours ;
- **pas de fil de compilation** : la retraduction TCG coûte des dizaines de µs, elle est faite
  dans le fil du vCPU (écart au plan §3.1, assumé pour un niveau sans compilateur) ;
- **retours prédits** : non refaits ici — `x-ret-inline` (0008) sonde déjà le cache de sauts en
  ligne à chaque `blr`/`bctr`, `x-jc-2way` (0043) en double la capacité ; une trace ne traverse
  pas encore un `blr` (même pas un retour apparié à un `bl` de la trace) ;
- **vérificateur** `x-tier2-verify=N` : une entrée de trace sur N exécute sa copie
  « journalisée » (chaque accès invité noté, accroché dans `tcg-op-ldst.c` ; une marque à chaque
  entrée de bloc du chemin), remet les registres de l'entrée, rejoue les TB d'origine (lectures
  servies par le journal, écritures comparées, mémoire intacte, aucun second accès aux
  périphériques) et compare tous les registres (`ppc_t2_regs` : GPR, CR, XER, LR, CTR, nip,
  MSR, réservation, FPSCR, VSR, VSCR, statuts flottants, SPR) ; atomiques et base de temps : non
  vérifiables. Dans ce mode les traces ne sont ni chaînées ni dans le cache de sauts.
  Un accès invité que le journal ne voit pas (celui d'un helper : `cpu_ld*/st*_mmu`,
  `probe_access*` — lswx/stswx, lmw/stmw hors page, dcbz par helper…) rend la trace non
  vérifiable et n'est **jamais rejoué** (§9.5).
- **parité des hôtes** : code commun (`accel/tcg`, `tcg/tcg-op-ldst.c`) et frontal PowerPC
  seulement, **aucun émetteur touché** : le même patch vaut pour arm64 ; il reste à le
  construire et le prouver sur le M4 (même niveau de preuve).

### 9.2 Preuves

- Mutant (sortie latérale à l'envers sur une trace sur deux, `POMPPC_T2_MUTANT=1`) :
  **attrapé** dès la première trace touchée (« lecture en trop »).
- Ce que le vérificateur a trouvé en route : un **vrai défaut** (le compteur d'instructions PMU
  d'une sortie latérale comptait toute la trace) et trois défauts du vérificateur lui-même
  (`hflags` non recalculés après restauration, marque d'entrée posée avant la sortie latérale,
  fin de trace en bout de page) ; plus un défaut amont croisé : `gen_lookup_and_goto_ptr` lisait
  `CF_NO_GOTO_PTR` dans `tb->flags` au lieu des cflags (corrigé, sans effet hors de ce mode).
- OpenBIOS SMP (2 vCPU, `x-tier2=64`, `x-tier2-verify=2`) : **6,1 millions de traces
  vérifiées, 0 divergence**.
- Tiger (VM de dev, `x-tier2=4096`, `x-tier2-verify=16`) : démarre et joue le banc, image
  identique ; avec le binaire d'avant les deux dernières corrections : 272 millions de traces
  vérifiées, divergences d'une seule classe (artefact de fin de page, corrigé) ; **le binaire
  final n'a pas encore fait de partie vérifiée complète sur Tiger** (une partie vérifiée
  intermédiaire, verif3, s'est enlisée — job jamais rendu en 30 min, 835 millions de traces
  vérifiées, 10 divergences du même artefact ; cause de l'enlisement non établie : lenteur du
  mode ou défaut).

### 9.3 Banc (VM de dev, `t2bench` : gltest d3inter, 300 images, plugin + GLEngine + kext)

Même binaire `~/src/qemu-t2/build` (-O2, série + 0048), toutes les options de production :

| réglage | passes (s) | médiane |
|---|---|---|
| `x-tier2=0` (base2) | 17,04 / 17,42 / 17,05 | 17,05 |
| `x-tier2=4096` (t2a) | 15,65 / 15,60 / 15,52 | **15,60 (−8,5 %)** |

md5 de l'image identique (`ff0b952c…`). 35 176 traces formées (133 013 blocs, 3,8 blocs et 16,7
instructions par trace en moyenne), 18 916 demandes sans successeur chaîné. **Un démarrage par
réglage** : à confirmer en entrelacé (et sur DOOM 3, commande 3 du §8.5).

### 9.4 Ce qui reste

1. ~~Une partie vérifiée complète sur Tiger avec le binaire final, et comprendre verif3.~~
   Fait le 21/10 (§9.5) ; reste la partie vérifiée DOOM 3 sur la VM quotidienne.
2. A/B entrelacé sur la VM de dev, puis DOOM 3 sur la VM quotidienne (§8.5).
3. Arêtes et code x86 de DOOM 3 (§8.5, 1 et 2) pour trancher la phase 2.
4. Pendant arm64 : construire et prouver sur le M4.
5. Leviers suivants du même niveau : traverser les retours appariés (`bl` … `blr` dans la
   trace), traces sur deux pages, soupape pour les traces dont une sortie latérale est chaude.

### 9.x Défaut trouvé sur la VM quotidienne (21/10, 15 h) — corrigé au §9.5

Partie `t2-d3-verif` (binaire rapide reconstruit avec 0048, `TIER2=4096 TIER2VERIFY=64`) : Tiger ne
démarre pas le bureau (console texte, `open`/`osascript` : « On-demand launch of the Window
Server is allowed for root user only »). Le vérificateur note **31 divergences**, dont 28 sur la
trace `0xa66cc` (noyau, boucle de copie, 139 instructions, sortie `0xa67c8`) : « écriture en
trop : accès 58/58 … memop 0x12 » — la trace fait une écriture de plus que les blocs d'origine
(fin de boucle ou sortie latérale mal placée) ; 2 sur `0xffff88d4` (page commune) et 2 sur une
adresse aberrante `0x4672616d` (« Fram » en ASCII : clé de trace lue dans une mémoire qui n'en est
pas une). QEMU coupé au moniteur ; au redémarrage par défaut, HFS a retiré 2 fichiers orphelins,
bureau revenu. Journal : `bench/tcg/t2/d3-verif-2110/run_tiger.log`. (Cause et correctif : §9.5.)

### 9.5 Partie vérifiée de Tiger : le bureau ne venait pas (21/10, corrigé)

**Symptôme** (VM quotidienne, `TIER2=4096 TIER2VERIFY=64`,
`bench/tcg/t2/d3-verif-2110/run_tiger.log`) : pas de bureau, 31 divergences — 28 « écriture en
trop : accès 58/58 … memop 0x12 » sur la trace noyau 0xa66cc, 2 « registres : r6 trace
0x4672616d » sur la page commune 0xffff88d4 (`0x4672616d` n'est pas une clé : c'est « Fram »,
le contenu de r6). Reproduit sur la VM de dev (même noyau, binaire de `qemu-t2` sans
changement) : arrêt au journal de démarrage après loginwindow, 4 divergences r6 à
0xffff88d4, 26 « écriture en trop » sur une trace d'une bibliothèque (0x91b4e8c8), aucune
réponse de l'agent (extinction forcée).

**Causes : deux défauts du vérificateur, aucun des traces elles-mêmes.**

1. *Accès hors journal rejoués* — la cause du bureau absent. Le journal n'accroche que les
   accès émis par `tcg_gen_qemu_ld/st` ; ceux d'un helper (`cpu_ld*/st*_mmu`, `probe_access`)
   lui échappent. 0xffff88d4 est l'en-tête du `bcopy` de la page commune : `mtxer` ;
   `lswx r6,0,r4` ; `stswx r6,0,r12` (helpers `helper_lswx`/`helper_stsw`). Le rejeu des TB
   d'origine, parti des registres de l'entrée, refaisait ces accès **en vrai** : `lswx`
   relisait une source que la trace avait déjà réécrite (copie qui se recouvre : r6 « ks/D »
   au lieu de « Fram »), et `stswx` **réécrivait la mémoire de l'invité** avec ces octets,
   après la trace. Une copie de chaîne sur 64 rejouée faux : chemins de fichiers abîmés,
   bureau jamais lancé. (Même risque pour lmw/stmw hors page, dcbz par helper, et tout
   helper qui touche la mémoire.)
2. *Copie journalisée coupée par TCG* — les « écriture en trop ». 0xa66cc est un bloc droit
   de 139 instructions (copie d'un état de thread 32 → 64 bits : `lwz` ; `stw 0` ; `stw`, en
   boucle déroulée). Les accroches du journal prenaient deux temporaires TCG par accès sans
   les rendre ; à plus de 512 (`TCG_MAX_TEMPS`) `tb_gen_code` recommence avec moitié moins
   d'instructions : la copie journalisée s'arrêtait à 0xa67c8 (63 instructions), au milieu
   du bloc, alors que le rejeu joue des TB entiers et écrivait au-delà. Exécution juste (la
   copie s'arrête proprement), vérification fausse ; idem pour une trace elle-même trop
   longue pour les temporaires.

**Correctif** (`tcg/0048`, toujours sans émetteur, rien quand `x-tier2=0`) :

- `accel/tcg/ldst_common.c.inc` (`cpu_ld*/st*_mmu`) et `cputlb.c` (`probe_access_internal`,
  hors lecture d'instructions) appellent `t2_check_foreign` : si une vérification est en
  cours (`cpu->t2_vrun`), dans le passage journalisé (le vrai) l'accès est fait et la trace
  déclarée non vérifiable ; dans le rejeu il n'est **pas** fait : `cpu_loop_exit`, le
  nettoyage de `cpu_exec` remet l'état laissé par la trace. Même chose pour atomiques et base
  de temps (`helper_t2_unverif`), qui ne s'exécutent plus jamais dans un rejeu ;
- `tcg-op-ldst.c` : les temporaires des accroches sont rendus ;
- `t2_form` : une trace coupée par TCG **au milieu d'un TB** est retraduite avec les TB du
  chemin qui tiennent (« raccourcies » au bilan) ; une trace qui s'arrête là où s'arrête un
  de ses TB (isync, mtmsr…) est gardée telle quelle et vérifiable ; une copie journalisée
  plus courte que sa trace est jetée (« copies coupées », la trace n'est pas vérifiée) ;
- mutants : `POMPPC_T2_MUTANT=2` (toutes les sorties latérales « pris » à l'envers),
  `POMPPC_T2_MUTANT=3` (le garde des accès hors journal coupé : le défaut du 21/10).

**Non-régression** : `tools/tcg/t2proof.sh QEMU_BIN` — banc nu `tools/tcg/t2proof.S`
(`-M mac99 -bios`, mode réel, 1 vCPU, ~10 s par passe) : une trace qui fait glisser un
tampon sur lui-même avec lswx/stswx et réécrit la source, un bloc droit de 144 accès, une
trace de ~300 instructions. Passes ref / t2 / verif (`x-tier2-verify=1`) / mutant 2 /
mutant 3.

| binaire | somme ref / t2 / verif | verif | mutant 2 | mutant 3 |
|---|---|---|---|---|
| avant (qemu-t2, 20b3c5d) | 0x9780e5cd / 0x9780e5cd / **0x3bb2c16a** | **48 958 divergences** (r6), invité altéré | — | — |
| après | 0x9780e5cd ×3 | 0 divergence, 32 641 identiques, 16 319 non vérifiables, 1 raccourcie | attrapé (lecture en trop) | attrapé (r6, somme 0x3bb2c16a) |

**Preuves** (binaire `~/src/qemu-t2fix/build`, branche `t2fix` d51fb1e de `qemu-t2`) :

- OpenBIOS SMP (2 vCPU, `x-tier2=64,x-tier2-verify=2`, 120 s) : 77,7 millions de traces
  vérifiées, **0 divergence** (211 traces, 0 raccourcie).
- Tiger, VM de dev **au bureau** (`start --gui`, 2 vCPU, MTTCG, toutes les options de
  production, `x-tier2=4096,x-tier2-verify=64`), session de **28 min 30 s** (15:51 → 16:20) :
  démarrage, jeux (Zenerchi, Marble Blast Gold en deux réglages, 150 s chacun, deux fois),
  job t2bench (compilation du kext, du plugin et de gltest dans l'invité ; ses passes
  gltest ne tournent pas en mode bureau), extinction propre : **163,9 millions de traces
  vérifiées, 0 divergence**, 154,2 M identiques, 9,5 M non vérifiables, 1,6 M sans copie
  journalisée (169 copies coupées), 911 traces raccourcies, 885 M accès rejoués
  (`bench/tcg/t2/t2fix-verif-gui/`).
- t2bench en single-user vérifié (`t2dev.sh t2fix-verif-su x-tier2=4096,x-tier2-verify=64`) :
  3 passes, md5 identique, 66,2 M traces vérifiées, **0 divergence**.
- verif3 (§9.2) : son enlisement est très probablement le même défaut 1 (invité altéré par un
  rejeu), pas une lenteur.

**Banc entrelacé 3 contre 3** (après la correction ; VM de dev single-user, `t2dev.sh`,
binaire `qemu-t2fix` -O2, t2bench FR=150, 3 passes par démarrage, ordre base/t2 ×3,
`bench/tcg/t2/t2fix-ab-*`) :

| réglage | démarrage 1 | démarrage 2 | démarrage 3 | médiane des 9 passes |
|---|---|---|---|---|
| `x-tier2=0` | 8,51 / 8,72 / 8,57 | 8,51 / 8,23 / 8,43 | 8,79 / 8,26 / 8,11 | 8,51 s |
| `x-tier2=4096` | 7,88 / 7,62 / 7,73 | 7,75 / 7,56 / 7,73 | 8,80 / 8,03 / 8,16 | **7,75 s (−8,9 %)** |

md5 identique partout (`7ff741d6…`, FR=150 ; le `ff0b952c…` du §9.3 est à FR=300).
~35 000 traces par démarrage, ~100 raccourcies. Le gain du §9.3 (−8,5 %, un démarrage par
réglage) tient en entrelacé.

**Commandes pour la VM quotidienne** (binaire `~/src/qemu-t2fix/build`, même série + 0048
corrigé) :

    # 1. partie vérifiée DOOM 3 (pas pour la vitesse) : 0 DIVERGENCE attendu dans run_tiger.log
    QEMU_BIN=$HOME/src/qemu-t2fix/build/qemu-system-ppc tools/tcg/matab.sh t2-d3-verif 1 \
      "v:TIER2=4096 TIER2VERIFY=64" d3 fen
    # 2. A/B entrelacé des traces
    QEMU_BIN=$HOME/src/qemu-t2fix/build/qemu-system-ppc tools/tcg/matab.sh t2-d3-ab 3 \
      "ref:TIER2=0" "t2:TIER2=4096" d3 fen
    tools/tcg/matab.sh --restore

Un arbre où l'ancien 0048 est déjà appliqué est refusé par `scripts/build_qemu_qfb.sh`
(marqueur `t2_check_foreign`) : repartir d'un arbre propre.

### 9.6 DOOM 3 sur la VM quotidienne (21/10, soir) : juste, mais plus lent

Binaire `~/src/qemu-t2fix/build` (-O2, série + 0048 corrigé, sans PGO), mêmes options de
production dans les deux bras, hôte au repos (barrière `chargehote.py attendre`).

- **Partie vérifiée** (`bench/tcg/ab/t2fix-d3-verif`, `TIER2=4096 TIER2VERIFY=64`) : bureau
  présent, **218,7 millions de traces vérifiées, 0 divergence** (208,2 M identiques, 10,3 M non
  vérifiables, 5,9 M sans copie journalisée dont 162 coupées, 1 090 raccourcies). Pas de
  mesure de vitesse : la scène n'est pas atteinte en 1 515 s (image 3003), le rejeu d'une
  trace sur 64 ralentit trop le jeu.
- **A/B entrelacé 3 contre 3** (`bench/tcg/ab/t2fix-d3-ab`, `d3-fen`) :

| réglage | parties (ms/image) | médiane |
|---|---|---|
| `TIER2=0` | 101,1 / 98,8 / 100,5 | 100,5 |
| `TIER2=4096` | 103,8 / 104,0 / 103,6 | **103,8 (+3,3 %)** |

Les trois parties `TIER2` sont plus lentes que les trois de référence. ~44 000 traces par
partie (3,5 blocs, 18,5 instructions en moyenne), dont près de la moitié (~21 000) sans suite
chaînée. **Le gain de t2bench (−8,9 %) ne se retrouve pas en jeu** : `x-tier2` reste éteint par
défaut et n'est pas à allumer sur DOOM 3. Les arêtes de DOOM 3 (§8.5) décident de la suite
(traces sur plusieurs pages, phase 2, ou arrêt pour ce jeu).

### 9.7 Arêtes de DOOM 3 (21/10, soir) : la porte de la phase 2

Greffon `tools/tcg/libtbedges.so` sur le binaire rapide (`bench/tcg/t2/d3-edges`, instantanés
toutes les 30 s ; `regions.py` sur r9 − r8, temps pris du profil hôte `x86-hprof-2110`). Le
greffon a planté vers l'image 2353 (segfault **dans libtbedges.so**, pas dans QEMU ; défaut du
greffon non cherché) : les arêtes sont celles des ~5 premières minutes de jeu, avant la scène
mesurée.

- 70 529 blocs exécutés, 6,6 instructions par bloc ; transitions : direct 87,3 %, bclr 8,2 %,
  bcctr 2,1 % ; sorties indirectes prédites par 1/2/3 cibles : 76 / 89 / 93 %.
- Régions sur arêtes (≤ 1024 instructions, ≤ 4 pages, ≤ 128 blocs), poids = temps vCPU :

| N régions | part du vCPU | instr. moy. | pages moy. | transitions internes |
|---|---|---|---|---|
| 50 | 39,2 % | 302 | 3,40 | 50,2 % |
| 200 | 62,4 % | 193 | 2,98 | 76,9 % |
| 1000 | **76,5 %** | 94 | 2,39 | 87,5 % |

**Porte de couverture passée sur les vraies arêtes** (≥ 70 % dans ≤ 1000 régions ; plafond du
code généré 83,7 %). Les traces de la phase 1 (3,5 blocs, 18 instructions, une page) sont bien
plus courtes que ces régions (~14 blocs, 94 instructions, 2,4 pages) et ne retirent que les
sauts entre blocs : d'où le +3,3 % du §9.6. Le gain du §8.3 (registres gardés, TLB sorti des
boucles) ne peut venir que d'une région **compilée** (phase 2) ; centre ×1,25-1,3, soit DOOM 3
vers ~75 ms/image, pas 50.

Contrôle du binaire rapide reconstruit avec 0048 corrigé (`bench/tcg/ab/fast-2110b`, 3
parties, non entrelacé) : 92,3 / 96,9 / 92,6 ms/image, médiane **92,6** (95,5 le matin) : pas de
régression du garde `t2_check_foreign` x-tier2 éteint.

## 10. Phase 2, essai : régions compilées par LLVM (`tcg/0049`, `x-tier3`), 21-22/10

### 10.1 Ce qui est fait

`patches/tcg/0049-tcg-tier3-llvm.patch` (arbre `~/src/qemu-t3`, base `~/src/qemu-t3-base` =
série + 0048 ; `TIER3=N` dans `run_tiger.sh` ; **éteint par défaut**) :

- **formation** (`accel/tcg/tier3-exec.c.inc`, `t3_form`) : là où 0048 formerait une trace, une
  région de TB chauds en largeur le long des sorties vues dans les ops capturées (nip constant
  d'un `goto_tb` ou d'une recherche, même page ou non) et des retours des `bl` de la région ;
  ≤ 64 TB, 1024 instructions, 8 pages ; une région sans boucle est écartée (sauf
  `POMPPC_T3_ANY`). Seuil `x-tier2` lu comme un **débit** : compteurs remis à zéro chaque seconde ;
- **compilation** (`tcg/tier3.c`, LLVM 21 ORC par l'API C, `default<O2>`, fil à part) : les ops
  TCG capturées après `tcg_optimize` (`tcg/tier3-capture.c.inc`) traduites une pour une ; sorties
  vers un TB de la région = branchements ; registres invités en valeurs LLVM, rangés seulement
  s'ils sont sales (flot de données) avant un helper, au chemin lent et aux sorties ; chemin
  rapide du TLB en ligne ; accès invités volatils ; `cpu->t3_pc` pour `cpu_restore_state` ;
  retours prédits par un `switch` sur le nip ; TB refusé s'il contient une op non traduite
  (vecteurs, `ppc_fp32`) ;
- **installation** : le TB chaud remplacé par un TB d'entrée (`gen_t3_stub`) qui appelle
  `helper_t3_run` ; invalidation par liens vers les TB des blocs, code comparé à l'installation,
  `tb_flush` ; garde de la page aux arêtes inter-pages ;
- **preuve** : `x-tier2-verify` étendu aux régions (copie journalisée, rejeu des TB d'origine) ;
- **change aussi 0048** : compteurs d'exécution sortis du `TranslationBlock` vers une table par
  vCPU (`CPUState.t2_ctrs`). À côté du code, la rangée du compteur déclenchait la détection de
  code automodifié de l'hôte : +80 % sur t2bench ; table partagée entre vCPU : +32 %.
- LLVM optionnel (`llvm-config`, sinon `tcg/tier3-stub.c`) ; aucun émetteur touché, mais non
  construit sur arm64.

### 10.2 Preuves

- `tools/tcg/t3proof.sh` (banc nu `t3proof.S` : boucles imbriquées, appel/retour d'une autre
  page, exception précise dans une boucle, code modifié, accès hors journal) : **CONFORME**,
  trois mutants attrapés (branchements inversés, registre sale oublié aux sorties, garde hors
  journal coupé). Rejoué le 22/10 au matin.
- Tiger single-user (`t3-verif-su2`, 2000 régions) : 19,1 M entrées vérifiées, **0 divergence**.
- Tiger au bureau + banc (`t3-verif-gui`, 254 régions) : 18,8 M vérifiées, **0 divergence**.
- md5 de l'image de t2bench identique dans tous les bras.

### 10.3 Banc (t2bench, VM de dev, entrelacé 3 × 3, `bench/tcg/t2/t3ab.bilan`)

| bras | médiane | écart |
|---|---|---|
| `x-tier2=0` | 8,50 s | — |
| `x-tier2=4096` (traces) | 7,79 s | **−8,4 %** |
| `x-tier2=131072,x-tier3=200` + `POMPPC_T3_NOFORM` (témoin : compteurs + époques) | 9,96 s | +17,1 % |
| `x-tier2=131072,x-tier3=200` (régions) | 10,02 s | **+17,8 %** |

Couverture (`POMPPC_T3_STATS`, 102 régions) : **21,9 %** des instructions dans les régions ;
avec 1952 régions, 92,9 %, mais plus lent (15,6 s) : la compilation coûte **150 ms par région en
moyenne** (de 25 à 580 ms selon les essais, jusqu'à 16 s pour une région), 550 s de processeur
pour 1952 régions.

Sorties des régions : branchement calculé **58,2 %**, retour prédit manqué **39,8 %**, direct
hors région 1,4 %. Les deux régions les plus entrées (0xfff086b0, 0xfff0e360 : 1,5 G entrées)
font **6 et 9 instructions par entrée**. Les régions de t2bench sont entrées et quittées comme
des TB : chaque entrée paie l'appel de `helper_t3_run`, et chaque sortie paie le rangement des
registres sales et une recherche (`helper_lookup_tb_ptr`). Ce que les registres gardés
rapportent dans la région ne couvre pas ce surcoût.

### 10.4 Le coût des compteurs (22/10, `bench/tcg/t2/t3cnt.bilan`, entrelacé 2 × 3)

| bras | médiane | écart |
|---|---|---|
| `x-tier2=0` | 7,91 s | — |
| `x-tier2=2000000000` (compteurs qui ne se déclenchent jamais) | 9,25 s | **+17,0 %** |
| témoin `x-tier3` (compteurs + époques) | 9,53 s | +20,6 % |

Le compteur ajoute 6 instructions en tête de chaque TB (charger la table, `inc`, ranger,
comparer, sauter), plus l'appel du helper en ligne, soit ~40 octets de code par TB. Cela coûte
**17 %** quand tous les TB comptent. Le témoin de 10.3 ne mesurait donc pas « l'infrastructure
de la phase 2 », mais surtout ce compteur. Les régions ne font pas mieux que lui (+0,6 %), alors
qu'elles retirent le compteur du code qu'elles couvrent.

Conséquence pour la phase 1 : dans 0048, un TB chaud qui ne donne pas de trace (« sans suite
chaînée » : 18 916 sur 35 176 demandes au §9.3) **compte à vie**. Sur DOOM 3, le compteur
d'origine était en plus rangé à côté du code (le +80 % ci-dessus). C'est un suspect sérieux pour
le +3,3 % du §9.6.

### 10.5 Retirer le compteur d'un TB chaud sans trace (22/10, neutre)

Essai `patches/tcg/essais/0049b-t2-retire.patch` (`POMPPC_T2_RETIRE=1`) : quand `t2_form` ne
trouve pas de suite, le pc physique du TB est noté, puis le TB est invalidé. Il est retraduit sans
compteur (`T2_DONE`), et son compte se lit comme le seuil, pour que les traces voisines le voient
chaud. `t2proof.sh` reste CONFORME. t2bench, entrelacé 3 × 3 (`bench/tcg/t2/t2ret2.bilan`) :

| bras | médiane | écart |
|---|---|---|
| `x-tier2=0` | 7,97 s | — |
| `x-tier2=4096` | 7,29 s | −8,5 % |
| `x-tier2=4096` + retrait (73 003 TB retirés) | 7,32 s | −8,1 % |

Une fois les traces formées, le code chaud qui compte encore pèse peu : le +17 % de 10.4 vient
d'un régime où **tous** les TB comptent (époques de `x-tier3`). Le retrait n'est pas gardé dans
0049. (Une première série, `t2ret.bilan`, avait tourné par erreur sur l'ancien
`qemu-system-ppc64` : `run_tiger.sh` lance la cible ppc64, il faut reconstruire les deux.)

### 10.6 Verdict

**Pas de gain des régions compilées dans cette forme**, alors que les preuves passent
(0 divergence). Ce qui manque n'est pas l'optimiseur : LLVM garde bien les registres. Le
problème est la **forme des régions et le prix de leurs frontières** :

1. sur t2bench, 98 % des sorties sont des branchements calculés ou des retours vers un appelant
   hors région, et les régions chaudes font 6 à 9 instructions par entrée ;
2. chaque entrée passe par un TB d'entrée et `helper_t3_run`, et chaque sortie range l'état puis
   fait une recherche : une région ne se chaîne ni à une autre ni à un TB ;
3. les compteurs à débit (époques) coûtent 17-20 % s'ils restent dans tout le code ;
4. la compilation (150 ms par région, `O2`) limite le nombre de régions utiles.

Pour qu'une phase 2 rapporte, il faudrait des régions chaînées entre elles et aux TB comme des
`goto_tb`, une entrée directe sans helper, des caches de cibles en ligne pour `bctr`/`blr` à
l'intérieur de la région, et une détection des points chauds sans compteur dans le code (par
échantillonnage). C'est le gros du travail des phases 3-4, sans garantie : le plafond de DOOM 3
reste ×1,25-1,3 (§9.7).

DOOM 3 n'a pas été mesuré avec 0049. Ses arêtes sont plus favorables (87 % de transitions
directes, régions de ~14 blocs, §9.7). Le binaire `~/src/qemu-t3` porte aussi les compteurs en
table, ce qui permettrait de savoir si le +3,3 % des traces sur DOOM 3 (§9.6) venait du compteur
rangé à côté du code.

### 10.7 Commandes pour la VM quotidienne (jouées le 22/10, §10.8)

    # A/B DOOM 3, même binaire -O2 (base + 0048 + 0049) : témoin, traces, régions
    QEMU_BIN=$HOME/src/qemu-t3/build/qemu-system-ppc tools/tcg/matab.sh t3-d3-ab 3 \
      "ref:CPU_OPTS=x-tier2=0" "t2:CPU_OPTS=x-tier2=4096" \
      "t3:CPU_OPTS=x-tier2=131072,x-tier3=200" d3 fen
    # partie vérifiée (pas pour la vitesse) : bilans x-tier2-verify / x-tier3 dans run_tiger.log
    QEMU_BIN=$HOME/src/qemu-t3/build/qemu-system-ppc tools/tcg/matab.sh t3-d3-verif 1 \
      "v:CPU_OPTS=x-tier2=131072,x-tier3=200,x-tier2-verify=64" d3 fen
    tools/tcg/matab.sh --restore

### 10.8 DOOM 3 sur la VM quotidienne (22/10, matin) : la phase 2 s'arrête là

`bench/tcg/ab/t3-d3-ab` (même binaire -O2 `~/src/qemu-t3`, base + 0048 + 0049, 3 parties par
bras entrelacées, barrière `chargehote.py attendre`) :

| bras | médiane (ms/image) | min..max | écart |
|---|---|---|---|
| `x-tier2=0` | 99,4 | 98,8..99,4 | — |
| `x-tier2=4096` (traces) | 102,1 | 101,8..107,6 | **+2,7 %** |
| `x-tier2=131072,x-tier3=200` (régions) | 116,9 | 116,6..117,6 | **+17,6 %** |

Régions : 243 formées (2495 blocs, 12 596 instructions), 685 M entrées, compilation 79 ms en
moyenne (19 s en tout). Traces : 36 719.

- **Les traces perdent toujours sur DOOM 3**, compteurs en table compris : le +3,3 % du §9.6
  ne venait donc pas du compteur rangé à côté du code.
- **Les régions LLVM perdent 17,6 %**, comme sur t2bench : le coût de leurs frontières (entrée par
  helper, sorties par recherche) et des compteurs à débit dépasse ce qu'elles rapportent, même
  avec des arêtes à 87 % directes.

**Décision proposée** : arrêter ici le second niveau sous cette forme (phases 1 et 2 éteintes,
0048 gardé éteint, 0049 laissé sur sa branche, non fusionné). Le reprendre demanderait de refaire
les frontières : chaînage direct entre régions, entrée sans helper, caches de cibles en ligne,
chaleur par échantillonnage (§10.6). C'est le travail des phases 3-4, pour un plafond de
×1,25-1,3 qui ne mène pas à 50 ms/image.
