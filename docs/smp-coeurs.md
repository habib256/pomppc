# Combien de cœurs ? — pourquoi deux, ce que 3 ou 4 rapportent, et où sont les vrais leviers

Question de l'utilisateur (25/09/2026) : « pourquoi on est limité à 2 cœurs, et pourquoi on ne
pourrait pas paralléliser plus en passant sur 4, 6, 8 cœurs hôte ou plus ? ». Ce document
répond avec des faits vérifiés (code source d'Apple et de QEMU, essais sur une seconde
instance, mesures), puis dit ce qui est faisable et ce que ça rapporte.

**En une phrase** : la limite de 2 n'est **ni dans Tiger ni dans le noyau** ; c'est notre
patch QEMU qui ne câble qu'une ligne de reset de CPU. En câblant les deux suivantes
(une quinzaine de lignes), **Tiger 10.4.6 démarre sur 3 et 4 processeurs** (`hw.ncpu: 4`, `hostinfo` :
« 4 processors are physically available ») ; au-delà de 4, le pilote d'interruptions
d'Apple (AppleMPIC) n'a plus de canal IPI. **Mais les jeux n'en profitent pas** : quel que
soit le nombre de vCPU, la somme de leur travail reste **≈ 1,0 cœur hôte** pendant Marble
Blast, parce que le jeu est un seul fil et que tout le reste de Tiger pèse quelques
pour cent. Le temps d'image est celui d'**un** fil PowerPC émulé : ce qui le raccourcit, c'est
de rendre ce fil plus rapide (TCG) ou de lui retirer du travail (déporter vers l'hôte), pas de
lui donner des voisins.

Journaux : `bench/reg/res/{a,b,c}{1,2,3,4}-*` du worktree (non versionné, comme tout
`bench/`), `bench/reg/campagne-smp.log`. Outils : `tools/tcg/smpab.sh` (campagne),
`tools/tcg/threadbusy.py` (occupation par fil d'un `sample` hôte), `tools/tcg/psmsum.py`
(CPU par fil depuis `ps -M`). Patch d'essai : `patches/smp-mac99/essais/qemu-mac99-4cpus.patch`
(**non appliqué** par le build).

---

## 1. Pourquoi deux aujourd'hui

La chaîne a cinq maillons : la machine émulée (QEMU `mac99`), le contrôleur d'interruptions
(OpenPIC/MPIC), le firmware (OpenBIOS : l'arbre `/cpus`), la plate-forme expert de Tiger
(`AppleMacRISC2PE`, qui démarre les CPU secondaires) et le noyau (xnu). Versions de 10.4.6
PowerPC d'après le manifeste d'Apple : `xnu-792.6.70`, `AppleMacRISC2PE-180.0.12`,
`AppleMacRISC4PE-185.0.0`, `AppleMPIC-1.5.2`
(<https://raw.githubusercontent.com/apple-oss-distributions/distribution-macOS/mac-os-x-1046ppc/release.json>) ;
`uname -a` de l'invité confirme `xnu-792.6.70.obj~1/RELEASE_PPC`.

| Maillon | Limite | Source |
|---|---|---|
| QEMU 9.2 amont, `mac99` | **1** : `mc->max_cpus = 1` (« SMP is not supported currently »), et `openpic_realize` refuse `nb_cpus != 1` (« Only UP supported today ») ; toujours vrai dans la branche principale (11.1) | `hw/ppc/mac_newworld.c`, `hw/intc/openpic.c` |
| Notre patch (`patches/smp-mac99/qemu-mac99-cpus-v2.patch`, d'après BALATON Zoltan) | **2** : `mc->max_cpus = 2`, **une seule ligne de reset câblée** (GPIO 3/4 de KeyLargo → `cpus[1]`) ; commentaire : « le MacIO ne porte qu'UNE ligne de reset » — c'est inexact, voir plus bas | `patches/smp-mac99/` |
| OpenPIC de QEMU | 4 pour KeyLargo (`KEYLARGO_MAX_CPU 4`), 32 en générique (`MAX_CPU`) ; **4 canaux IPI** (`OPENPIC_MAX_IPI 4`) | `include/hw/ppc/openpic.h` |
| OpenBIOS livré (`openbios-smp-screamer.elf`, binaire sans source dans le dépôt) | **N** : crée autant de nœuds `/cpus` que `FW_CFG_NB_CPUS`, `reg` = 0..N−1, `state = stopped` hors CPU 0 (boucle du `openbios.patch`) ; pas de propriété `soft-reset`. OpenBIOS amont, lui, n'en crée qu'un | `patches/smp-mac99/openbios.patch` ; `arch/ppc/qemu/init.c` amont |
| `AppleMacRISC2PE` (plate-forme des G4) | **pas de limite codée** : compte les enfants de `/cpus`, `cpus=` en boot-arg ; numéro du CPU = `reg` ; relâche un CPU secondaire en écrivant son offset `soft-reset` dans KeyLargo, **repli codé en dur 0x5B (CPU 0) ou 0x5C (tous les autres)** | `MacRISC2CPU.cpp` l. 121-188, 785-816 (tag 180.0.12) |
| `AppleMPIC` (le pilote d'OpenPIC) | **4** : un canal IPI par CPU cible (`STWBRX(1 << cnt, ipiBase + kPnIPImDispStride * cnt)`) ; OpenPIC n'en a que 4, un 5ᵉ CPU écrirait dans CTPR/SPVE | `AppleMPIC.cpp` l. 597, 618-624 (tag 1.5.2) ; `hw/intc/openpic.c` |
| xnu-792.6.70 PowerPC | **256** : `#define MAX_CPUS 256`, `PerProcTable[MAX_CPUS-1]` ; `ml_processor_register` échoue proprement au-delà de `max_ncpus` (boot-arg `cpus=`) | `osfmk/ppc/exception.h:552`, `osfmk/ppc/cpu.c:56-57, 267-276`, `osfmk/ppc/model_dep.c:216-218` |
| Les vrais Mac G4 | **2** (PowerMac3,4 Digital Audio, 3,5 QuickSilver, 3,6 MDD en bi-processeur) ; aucun Power Mac G4 à plus de 2 | Wikipédia « Power Mac G4 » ; modèles exacts à vérifier |

Le noyau de 10.4 sait donc faire bien plus que 2 — le Power Mac G5 Quad (PowerMac11,2,
10.4.2 et suivants : **à vérifier**) en a 4, par une autre plate-forme expert
(`AppleMacRISC4PE`, qui exige une propriété `soft-reset` par CPU et une fonction de
plate-forme pour la base de temps) mais **le même AppleMPIC** : 4 est la limite de tout
Mac PowerPC sous 10.4.

**KeyLargo a quatre lignes de reset, pas une.** Linux les nomme
(`arch/powerpc/include/asm/keylargo.h`) : `KL_GPIO_RESET_CPU0..3` = `EXTINT_0 (0x58)` +
0x03, 0x04, **0x0F, 0x10**, soit les offsets 0x5B, 0x5C, **0x67, 0x68** (GPIO 3, 4, 15, 16).
Et Tiger les utilise sans qu'on lui dise rien : avec `-smp 3` sur un QEMU qui accepte 3 CPU
mais ne câble que le GPIO 4, la trace `macio_gpio_write` montre (`bench` : `smp3-trace.log`)

    macio_gpio_write addr: 0xc  value: 0x4 / 0x5     # GPIO 4  : CPU 1 relâché
    macio_gpio_write addr: 0x17 value: 0x4 / 0x5     # GPIO 15 : CPU 2 — ligne non câblée

puis le démarrage reste bloqué sur « Still waiting for root device » (le CPU 2 enregistré
auprès du noyau ne répond jamais). Le repli codé en dur d'`AppleMacRISC2PE-180.0.12`
(0x5C pour tout CPU non nul) ne suffit donc pas à décrire le binaire livré dans 10.4.6 :
celui-ci choisit 0x67 pour le CPU 2 (et 0x68 pour le CPU 3, §1.1) — cohérent avec la table
de Linux ; le code exact du binaire n'a pas été désassemblé (non nécessaire).

### 1.1 L'essai : 3 et 4 CPU démarrent

Copie isolée de `~/src/qemu-tcg19` (branche `regime15`, `qgpu` v15, `x-jit-near`), trois
changements (`patches/smp-mac99/essais/qemu-mac99-4cpus.patch`, 80 lignes de diff) :
`mc->max_cpus = 4` ; GPIO 15 et 16 déclarés lignes de reset (actives bas) dans
`hw/misc/macio/gpio.c` ; `cpu_kick` câblé sur `cpus[2]` (GPIO 15) et `cpus[3]` (GPIO 16).
**Firmware inchangé** (l'`openbios-smp-screamer.elf` livré crée déjà N nœuds). Disque : copie
APFS de `tiger-dev.raw`.

| `-smp` | démarrage | `sysctl hw.ncpu` / `hostinfo` | GPIO écrits par Tiger |
|---|---|---|---|
| 3 (GPIO 15 non câblé) | **bloqué** (« Still waiting for root device ») | — | 4, 15 |
| 3 | bureau (4 démarrages sur 4) | `hw.ncpu: 3`, « Processors active: 0 1 2 » | 4, 15 |
| 4 | bureau (5 démarrages sur 5 ; un 6ᵉ s'est éteint proprement juste après le bureau, lancé sans vider la boîte aux lettres de `devloop` où restait vraisemblablement un job d'arrêt — non reproduit une fois la boîte vidée) | `hw.ncpu: 4`, « Kernel configured for up to 4 processors », « Processors active: 0 1 2 3 », `Load average 0.51, Mach factor: 3.48` | 4, 16, 15 |

Travail dans l'invité (job `smppar`, un démarrage par mode, même binaire : K copies
simultanées d'une boucle de calcul, puis 48 `gcc -O2 -c` en série et répartis sur N fils) :

| | SMP=1 | SMP=2 | SMP=4 |
|---|---|---|---|
| boucle, 1 copie (s) | **3,27** | 3,66 | 3,82 |
| 2 copies (s par copie) | 6,63 | 3,83 | 3,97 |
| 3 copies | — | 5,75 | 4,05 |
| 4 copies | — | — | 5,03 |
| 5 copies | — | — | 6,32 |
| 48 `gcc`, en série (s) | **10** | 11 | 13 |
| 48 `gcc`, sur N fils (s) | 10 | 7 | **6** |

- **Un fil seul va plus vite en SMP=1** : +12 % de temps en SMP=2, +17 % en SMP=4 sur la
  boucle, +10 % et +30 % sur `gcc` en série. C'est le prix de MTTCG (binaire `ppc64`, accès
  atomiques et barrières du code généré, TLB et `tlb_flush` synchronisés entre vCPU, BQL
  partagé) et des vCPU au repos que Tiger réveille. Sur Marble Blast il ne se voit presque
  pas (§2.1 : ~1,5 %), le jeu passant une grande part de son temps hors du code pur.
- **Le travail parallèle passe à l'échelle jusqu'à 3 vCPU** (3 copies à 4,0-4,1 s en SMP=4) ;
  la 4ᵉ copie ralentit tout (5,0 s) : l'hôte n'a que **4 cœurs P** (M4 : 4 P + 6 E), dont un
  sert déjà au fil principal de QEMU, au son et à la VM quotidienne au repos ; le 4ᵉ vCPU
  tombe sur un cœur E ou se partage. Débit à 4 copies : 4 × 3,27 / 5,03 = **2,6×** SMP=1.
- `gcc` réparti : 10 → 7 → 6 s. Compiler dans l'invité est **le** cas où 4 vCPU servent.

Ce qui resterait pour en faire un mode propre (≈ 1-2 jours) : `max_cpus = 4` et le câblage
sous une garde (les GPIO 15/16 n'existent que sur la variante PMU, comme 3/4) ; un
commentaire juste dans le patch v2 (« une seule ligne » est faux) ; `run_tiger.sh` qui
accepte `SMP=3|4` ; la panique AppleUSBOHCI au démarrage (~1/10 en SMP=2) à re-mesurer à 4 ;
optionnellement publier `soft-reset` dans OpenBIOS (source absente du dépôt : c'est le vrai
coût). **Au-delà de 4** : réécrire AppleMPIC (kext d'Apple) ou émuler un contrôleur à plus de
canaux IPI — hors de portée, et sans intérêt au vu du §2.

## 2. Ce que plus de cœurs invités rapportent

### 2.1 Marble Blast, 1-2-3-4 vCPU, placement du JIT forcé

`tools/tcg/smpab.sh` : un processus QEMU neuf par démarrage (`regab.sh`, `NGUEST=1`), bureau,
`regbench`, Marble Blast 110 s de chauffe + 150 s de mesure (bilan `POMPPC_GL_STATS` toutes
les 5 s), `sample` hôte de 10 s dans la passe de mesure, relevés `ps -M` toutes les 20 s.
**Même binaire** pour tous les modes (copie « 4 CPU » ci-dessus ; SMP=1 prend
`qemu-system-ppc` en `thread=single` comme `run_tiger.sh`, SMP≥2 `qemu-system-ppc64` en
MTTCG) ; `x-jit-near=on` et placement relevé à chaque démarrage (« même fenêtre » partout) ;
toutes les propriétés de CPU de `run_tiger.sh` ; entrelacement
`1 2 4 3 | 3 4 2 1 | 1 2 4 3`. Indice : `tools/tcg/regidx.py` contre les sept démarrages
« près » des campagnes du §14 de `tcg-g4.md` (1,00 = cette référence). VM quotidienne au
repos pendant toute la campagne (7 % d'un cœur).

| vCPU | démarrages (img/s ; indice) | moyenne img/s | indice moyen |
|---|---|---|---|
| 1 (`thread=single`) | a1 72,5 (1,000) · b1 74,8 (1,022) · c1 72,9 (1,006) | **73,4** | 1,009 |
| 2 | a2 72,0 (0,992) · c2 72,6 (0,989) · ~~b2 56,9 (0,758)~~ ¹ | **72,3** | 0,991 |
| 3 | a3 73,2 (0,998) · b3 72,6 (0,995) · c3 73,3 (1,001) | **73,0** | 0,998 |
| 4 | a4 71,1 (0,976) · b4 73,2 (1,002) · c4 71,9 (0,983) | **72,1** | 0,987 |

¹ Écartée : le « troisième facteur » du §14.7 de `tcg-g4.md`. Tampon du JIT bien placé,
vCPU aussi occupés que dans c2 (101 % d'un cœur au total), mais tout est plus lent —
dès le premier tour de `regbench` (`call` 761 ms contre 620-630 dans les autres
démarrages, `mem` 712 contre 469) : le micro-banc, inutile pour les deux régimes du JIT
(§14.1), l'a signalé ici avant le jeu (un seul cas : à confirmer comme détecteur). 1 démarrage sur 12 ici, 1 sur 8 sur DOOM 3.

**1, 2, 3 et 4 vCPU donnent le même temps d'image** : l'écart entre modes (≤ 1,8 %) est
de l'ordre de l'écart entre démarrages d'un même mode (2-3 %). Si une tendance existe, elle
va vers **SMP=1** (+1,5 % sur SMP=2, trois démarrages contre deux : non significatif).

### 2.2 Pourquoi : le travail ne se partage pas

`sample` hôte de 10 s pendant la passe de mesure, `tools/tcg/threadbusy.py` (un échantillon
est « occupé » quand son sommet de pile n'est pas un appel bloquant du noyau) :

| vCPU | somme des fils vCPU | fil principal (E-S, BQL) | `qgpu-render` | processus entier |
|---|---|---|---|---|
| 1 | 93,0 · 93,6 · 92,7 % | 12 % | 9 % | **1,15 cœur** |
| 2 | 101,3 · 101,0 · 101,0 % (≈ 54 + 48) | 20-22 % | 6-7 % | **1,29** |
| 3 | 101,9 · 101,8 · 102,7 % | 19-20 % | 7-8 % | **1,30** |
| 4 | 102,1 · 103,2 · 102,6 % (≈ 32 + 24 + 24 + 23) | 19-20 % | 7 % | **1,30** |

Détail d'un vCPU (SMP=1, a1, % des échantillons du fil) : code généré 89 % (self), dont
`helper_lookup_tb_ptr` 23 %, tous les helpers 44 %, remplissage du TLB 6,4 %, BQL 6,8 %,
**traduction (`tb_gen_code`) 0,01 %**. En SMP=4, chaque vCPU passe 1,5-2 % de son temps
dans le BQL, et le fil principal monte de 12 à ~20 % dès le second vCPU (réveils croisés,
IPI, minuteries par vCPU).

- **La somme des vCPU reste ≈ 1 cœur hôte**, quel que soit leur nombre : l'invité n'a
  qu'un cœur de travail à donner. À 2 ou 4 vCPU, ce cœur est simplement réparti (le fil du
  jeu migre d'un vCPU à l'autre, WindowServer et le noyau se glissent à côté).
- En SMP=1, le vCPU est occupé ~93 % : les ~7 % restants sont des attentes (barrières du
  GPU, fin d'image), pas du travail qu'un second vCPU pourrait prendre.
- Le prix de MTTCG est faible sur le jeu avec `x-sr-tlb` (BQL 2-3 % par vCPU, vidages de TLB
  < 1 %), plus net sur un fil de calcul pur (+12 à +17 %, §1.1), et il n'achète rien :
  SMP=2 et SMP=1 sont à égalité aux erreurs de mesure près. Le
  « −7 % à SMP=1 » du §5.2 de `tcg-g4.md` mêlait les régimes du JIT (confirmé par §14.4).

### 2.3 Les jeux sont-ils multifils sous Tiger ?

`sample` dans l'invité (`.run/d3/sample*.txt`, `.run/prey/sample.txt`, VM quotidienne,
parties déjà enregistrées ; 10 s, 1 échantillon/10 ms) :

| Jeu | fils | occupés | les autres |
|---|---|---|---|
| DOOM 3 | 5 | **1** : `Thread_100f` (fil principal : `idCommonLocal::Frame`, rendu et jeu) | `BackgroundDownloadThread` (attente), 2 fils CoreAudio (HAL, I/O : ~11 % d'un fil dans `PerformIO`), `Sys_AsyncThread` (98,7 % dans `usleep`, 1,3 % dans `idCommonLocal::Async` : c'est le « Async thread started » du journal, le tic à 60 Hz du son et de l'entrée) |
| Prey | 5 | **1** (même moteur, même structure) | idem |
| Marble Blast (Torque) | — | 1 fil de jeu (§2.2 : la somme vCPU ≈ 1 cœur) | son |

La version Mac PowerPC de DOOM 3 (1.3) n'a pas le « SMP » de la version Windows 1.3.1
(`r_useSMP`, fil de rendu séparé) : **à vérifier** dans le binaire, mais le `sample` ne montre
aucun second fil de rendu. Aucun des jeux de la matrice ne peut occuper un second vCPU plus
de quelques pour cent.

### 2.4 Et en dehors des jeux

- **Compilation dans l'invité, `make -j`, encodage, tout travail vraiment parallèle** : là,
  oui : 48 `gcc` en 10 s (SMP=1), 7 s (2), 6 s (4) ; débit ×2,6 à 4 vCPU sur une boucle
  de calcul (§1.1), plafonné par les 4 cœurs P de l'hôte.
- **Bureau, Finder, Safari** (non mesuré) : un second vCPU ne peut aider que quand une tâche
  de fond tourne à côté (Spotlight `mds` au premier démarrage, `update_prebinding`) ; au
  repos, rien à partager.
- **Quartz Extreme** : n'existe pas ici (le WindowServer compose en logiciel ; `qgpu` ne sert
  qu'OpenGL) ; la composition tourne sur le vCPU du WindowServer — c'est justement ce que le
  second vCPU absorbe en SMP=2 (le jeu garde le sien).

## 3. Les leviers de parallélisme côté hôte, à cœurs invités constants

Ce que le processus QEMU fait déjà en parallèle pendant le jeu (SMP=1, a1) : le fil vCPU à
93 %, le **fil principal** à 12-20 % (boucle d'E-S sous BQL : minuteries, rafraîchissement
de l'écran, audio, `qgpu` IRQ), le **fil de rendu `qgpu-render`** à 7-9 % (appels OpenGL de
l'hôte), le fil CoreAudio. Somme ≈ 1,15-1,3 cœur. Le goulot est **le seul fil vCPU**.

Classés par gain attendu :

| # | Levier | État | Gain attendu | Coût |
|---|---|---|---|---|
| L1 | **Rendre le fil vCPU plus rapide** (TCG : retours prédits `lookup_tb_ptr` 18-23 %, flottant scalaire ~19 % de DOOM 3, BQL à chaque `mtmsr`/`rfi`) | en cours, `TODO.md` §4 | 10-25 % par poste traité | jours à semaines par poste |
| L2 | **Déporter du travail invité vers l'hôte** (A4 : état GL, textures par DMA, empaquetage minimal — `send_state`/`compute_state` sortent du vCPU) | planifié, `TODO.md` §3 « A4 » | ce que le pilote GL de l'invité coûte au fil du jeu : dans DOOM 3, 57 % du fil principal est dans le rendu (`RB_ExecuteBackEndCommands`, `.run/d3/sample-nat.txt`), dont une part diffuse dans le plugin (`tex_complete`, `geom_draw_client`, `compute_state`, `send_state`…) — à chiffrer au lot 5 | semaines |
| L3 | **Rendu `qgpu` sur son fil** | **fait** (v9, `qgpu-render`, file bornée) ; le plugin soumet en asynchrone (« submit async : 197 fences waited (26 ms), 0 sync fallback », DOOM 3) | déjà acquis ; le fil ne pèse que 7-9 % d'un cœur | — |
| L4 | **Doorbell synchrone sans BQL** : aujourd'hui un doorbell synchrone attend le rendu BQL pris (`qgpu-pci.c` : « D2 — le BQL est conservé pendant cette attente ») | ouvert (`TODO.md` §3 « doorbell asynchrone côté invité ») | faible en jeu (le chemin courant est déjà asynchrone) ; supprime des gels du fil principal | 1-2 jours + preuve de non-réentrance |
| L5 | **Traducteur de second niveau dans un fil à part** (`docs/plan-traducteur-rapide.md`, option B, LLVM) | mis de côté | ×1,3 plafond visé sur l'image, c'est **le seul** usage massif de cœurs hôte supplémentaires | 6-10 mois |
| L6 | **Traduction TCG de premier niveau dans un fil à part** | non (l'amont ne le fait pas : chaque vCPU traduit dans son propre contexte, `docs/devel/multi-thread-tcg.rst`) | **nul en jeu** : `tb_gen_code` = 1 échantillon sur 7 272 (0,01 %) ; 10-30 traductions/s (`tcg-g4.md` §2.3). Utile seulement au démarrage et aux chargements | gros (TB provisoires, invalidation) ; **à ne pas faire** |
| L7 | **Plus de vCPU** (3-4, §1) | essai fait, patch d'essai | 0 sur les jeux (et −12 à −17 % sur un fil seul) ; ×1,7 à ×2,6 sur du travail parallèle dans l'invité | 1-2 jours |
| L8 | **BQL moins pris** (`ppc_maybe_interrupt` sans verrou si l'état ne change pas) | ouvert (`TODO.md` §4) | BQL : 6,8 % du vCPU en SMP=1, 3 % par vCPU en SMP=2 (profils du §2.2) ; `tcg-g4.md` §2.4 donnait 9-10 % avant les patches flottants | 1-2 jours |

Ce qui **ne sert à rien** : plus de cœurs hôte au-delà de ~2 pour la VM telle qu'elle est —
6, 8 ou plus de cœurs de M4 ne changent pas le temps d'image, puisque ~1,3 cœur est tout ce
que le processus consomme.

## 4. Recommandation

1. **SMP par défaut : pas de raison de performance de garder 2 pour les jeux** —
   Marble Blast est à égalité (SMP=1 73,4, SMP=2 72,3 img/s, §2.1) et SMP=1 consomme 0,14 cœur
   hôte de moins (fil principal 12 % contre 20 %), et un fil de calcul seul y va 12 % plus vite
   (§1.1). Ce que SMP=2 apporte est ailleurs : le bureau
   quand une tâche de fond tourne, la compilation dans l'invité (§2.4). Ce qu'il coûte : la
   panique AppleUSBOHCI au démarrage (~1/10) et le binaire `ppc64` en MTTCG. Proposition :
   **garder 2 par défaut pour le bureau et passer les lanceurs de jeux à SMP=1** — *après*
   les parties DOOM 3 ci-dessous (seul jeu où SMP=1 n'a pas été mesuré à placement forcé, et
   sa partie SMP=1 du §6 bis de `tcg-g4.md` était un tirage de régime). Décision de
   l'utilisateur.
2. **Pas de chantier « 4 cœurs »** pour les jeux. Garder le patch d'essai
   (`patches/smp-mac99/essais/`) pour qui voudrait compiler dans l'invité ; le sortir de
   l'essai seulement sur demande (1-2 jours, §1.1).
3. **Le parallélisme utile est dans l'hôte et passe par le fil vCPU** : L1 (TCG) et L2
   (déporter vers l'hôte, A4) restent les chantiers ; L5 (second niveau dans un fil à part)
   est la seule façon d'employer vraiment des cœurs hôte en plus, et reste « plus tard ».
4. Corriger le commentaire de `qemu-mac99-cpus-v2.patch` (« une seule ligne de reset » : il
   y en a quatre) au prochain passage sur le patch.

### 4.1 Parties DOOM 3 à jouer (VM quotidienne, à jouer par l'utilisateur)

Même protocole que le §14.6 de `tcg-g4.md` ; binaire de référence (`tcg/0006` compris,
`JITNEAR` allumé par défaut dans `run_tiger.sh`), trois parties par mode, entrelacées,
depuis le worktree de cette branche :

    Q=~/src/qemu/build/qemu-system-ppc
    for i in 1 2 3; do
      QEMU_BIN=$Q bash tools/tcg/d3run.sh smp1-$i 1 1
      QEMU_BIN=$Q bash tools/tcg/d3run.sh smp2-$i 2 1
    done
    bash tools/tcg/d3run.sh --restore

Attendu si le §2 vaut pour DOOM 3 : les deux modes à ~74 ms/image, écart < 2 % ; chaque
`info.txt` dit « même fenêtre ». Écarter une partie dont la cinématique est lente (témoin du
troisième facteur, §14.7) et la rejouer. Si SMP=1 ≤ SMP=2 : lanceurs de jeux en SMP=1.
