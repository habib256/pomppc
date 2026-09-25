# Plan — un traducteur PowerPC beaucoup plus optimisé (second niveau de TCG)

État : **plan**, rédigé le 25/09/2026, **mis de côté par l'utilisateur (« on essaiera plus tard »)**. Rien n'est commencé. Point de départ :
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
