# État des deux hôtes : PC x86-64 et Mac M4

L'utilisateur travaille **alternativement** sur deux machines, avec le même dépôt GitHub
(`origin/main`). Une séance d'agent se déroule sur **une seule** de ces machines. Avant d'agir,
l'agent doit savoir sur laquelle il se trouve et où en est le travail sur chacune. Cette page
donne cet état. **La mettre à jour à la fin de chaque séance**, dans le même commit que le
travail, avec la date et l'heure.

## 0. Sur quel hôte suis-je ?

| `uname -sm` | hôte | binaire QEMU quotidien | défauts de `run_tiger.sh` |
|---|---|---|---|
| `Linux x86_64` | **PC** (Ubuntu 26.04, Core i7-10700F, RTX 4060 Ti) | `~/src/qemu/build-fast` (PGO), référence `~/src/qemu/build` | branche `Linux:x86_64` |
| `Darwin arm64` | **M4** (macOS, GL d'Apple sur Metal) | `~/src/qemu/build` (reconstruit par `scripts/build_qemu_qfb.sh`) | branche `Darwin:arm64` |

Au début d'une séance :
1. `git pull` : l'autre hôte a peut-être poussé.
2. Lire la section de **cet** hôte ci-dessous, puis « À faire en passant d'un hôte à l'autre ».
3. Si `patches/` ou `patches/qgpu/` ont changé depuis la dernière construction locale,
   reconstruire QEMU. Sinon, on mesure un binaire qui ne correspond pas au dépôt.

## 1. Règles communes

- **Parité** : toute accélération prouvée sur un hôte reçoit son pendant sur l'autre, **au même
  niveau de preuve**, avant d'y être allumée. Si elle ne peut pas l'être, la raison est écrite
  dans la doc du patch.
- **Vitesse générique** : un levier se juge sur **toute la matrice** (mb, zen, ut, d3, cmr),
  jamais sur DOOM 3 seul.
- **A/B seulement l'hôte au repos** : aucune construction, aucun autre QEMU, aucun agent
  parallèle pendant la mesure (`tools/tcg/chargehote.py attendre`).
- **Ne jamais supprimer `.run/tiger.lock`** (flock : deux QEMU écriraient le même disque).
- Le PC n'est **pas publié** (paquets, annonces) tant que sa vitesse n'est pas acceptable.
  Cible : DOOM 3 à 50 ms/image en fenêtre.
- Les commits vont sur `main` puis sont poussés. Un essai non retenu est rangé sous une
  étiquette `archive/*`.

## 2. PC x86-64 : état au 01/11/2026, 14 h 30 (aucune VM ne tourne : A/B PRESENTQ raté et arrêté à 14 h 20 ; `build/` et `build-fast/` à reconstruire depuis `tcg/0064`)

**Fait le 01/11** (matinée close vers 12 h ; l'après-midi : `pgo-train.sh` et PRESENTQ, fin de
cette liste ; la séance suivante se fait sur le M4)
- **Bug de l'ombre QE corrigé** (agent Opus, `docs/quartz-extreme.md` journal du 01/11,
  CHANGELOG) : la bande d'ancien jeu sous l'ombre du Terminal venait du chemin direct, qui
  n'avisait plus le WindowServer ; kext **méthode 6** (`POMPPC_GA_PRESENT2`) + plugin
  `20261101-ombre` (avis cadencé 20 Hz). Prouvé avant/après sur mb et DOOM 3 au chargement ;
  plugmd5 69 scènes 0 écart. Coût seulement sous une fenêtre posée : mb-fen +6,2 %, d3 +0,9 %.
  Installés dans la VM quotidienne (anciens dans `/var/tmp/avant-ombre/` de l'invité) et dans
  `disks/prebuilt` (anciens dans `bench/ombre-0111/prebuilt-avant-ombre-3010.tar`). Le M4 les a
  depuis le 01/11 à 16 h 57 (§3). Suite : avis borné au rectangle non visible (TODO).
- **Frontal ImGuiDock : double curseur corrigé** (`50a9801`, agent Opus) : curseur de l'hôte
  masqué au-dessus de l'image en souris absolue. Reconstruit, tests OK ; **à voir à l'œil au
  prochain `run_tiger.sh`** (un seul curseur sur l'image, flèche de retour sur les menus).
- Référence cmr-pe du PC validée à l'œil (image de course propre, `references-linux.csv`).
- PGO réentraîné avec cmr : neutre en fenêtre, cmr non prouvé (mesure polluée) ; ancien profil
  en production, nouveau dans `~/src/qemu/build-fast-pgo0111cmr/` (`e1eebc2`). **Piège évité à
  12 h 50** : `pgo-train.sh` avait laissé le profil cmr dans `~/src/qemu/pgo-fast/` (une
  reconstruction l'aurait pris) ; dossiers permutés, `pgo-fast/` = profil du 31/10, le profil cmr
  dans `pgo-fast-pgo0111cmr/` (`docs/binaire-rapide-x86.md` §7).
- `~/src/qemu-jcp/` et `~/src/qemu-bat/` effacés par l'utilisateur à 12 h 10 ; `qemu-nuit`
  contient 0060-0063.
- `tools/tcg/pgo-train.sh` cible **ppc32mt** par défaut (`cf6cf61`) : avant, le défaut
  « ppc64 » héritait du `PPC32MT=1` de `run_tiger.sh` et entraînait en fait ppc32mt ; le bras
  ppc64 pose maintenant `PPC32MT=0`. Les profils déjà construits ne changent pas.
- **`POMPPC_GL_PRESENTQ` rouvert** (demande de l'utilisateur) : A/B au repos en plein écran
  lancé à 13 h 28 (`bench/tcg/ab/presentq-0111-{cmr,mb,ut}`, 3 parties par bras) ;
  verdict et suite dans `TODO.md` (« Ensuite », PRESENTQ). Code de l'invité : s'il est allumé,
  le M4 le reçoit avec le bundle. **A/B RATÉ, à rejouer entièrement au retour sur le PC** :
  les 5 parties jouées (13 h 28-14 h 17) ont toutes fini « pas de ssh » parce que le binaire
  rapide a été **refusé** : « construit sur une autre série de patches » (`run_tiger.log` des
  parties). Cause : à 12 h 55 j'avais retouché un **commentaire** de `patches/qgpu/qgpu_proto.h`,
  et l'empreinte de la série (`scripts/qemu_fast.sh`) hache le contenu de chaque fichier de
  `patches/` nommé par `build_qemu_qfb.sh`, commentaires compris. Remis à l'identique à 14 h 20
  (empreinte `5ad32b79ece3943b` retrouvée = `build-fast/pomppc-build.txt`). **Règle apprise : ne
  pas toucher un fichier de `patches/`, même un commentaire, tant que le binaire rapide n'est
  pas reconstruit.** Chaîne arrêtée, aucune VM ne tourne. Au retour : `bench/tcg/ab/presentq-0111-chaine.sh`
  au repos (cmr, mb, ut pe), trancher PRESENTQ (règle dans le TODO), relancer la VM avec
  `./run_tiger.sh`.
- **Dérive de documentation nettoyée en deux passes** (huit agents Opus, `633c444`, `634797b`) :
  voir les messages de commit ; questions laissées ouvertes dans le TODO et les notes datées.
- **x-tb-head fusionné sous `tcg/0064`** (14 h 30, demande de l'utilisateur ; branche
  `worktree-agent-a15011…` renumérotée de 0051 à 0064, worktree et branche supprimés ;
  `docs/anatomie-ppc32.md`, `tools/tcg/codeanat32.py`, `tbhproof.*`). Patch nommé dans
  `build_qemu_qfb.sh`, propriété de l'accélérateur **éteinte par défaut** (`TBHEAD=N`) ; il
  s'applique au bout de la série (essai à blanc sur `~/src/qemu`, 0 rejet). **Conséquence : l'empreinte
  de la série change ; `build/` et `build-fast/` du PC sont à reconstruire avant toute mesure**
  (`./scripts/build_qemu_qfb.sh` puis `QEMU_FAST=1 ./scripts/build_qemu_qfb.sh`, profil
  `pgo-fast/` = 31/10), et le M4 reconstruit de toute façon. A/B en jeu de `TBHEAD=3` à faire
  (TODO, « Plus tard », Vitesse).

**Fait dans la nuit du 31/10 au 01/11** (objectif de l'utilisateur : vitesse x86 jusqu'à 7 h)
- **Fenêtre de cmr** : depuis le 28/10, le PC mesurait une transition d'avant la course, pas la
  course (`6054367`, `vitesse-doom3-x86.md` §25). En course, la production fait ~82 ms/image.
  Le « 16 bits ×2 » du 31/10 n'était que cette différence de phase : **point clos**.
- **0060 et 0061 rejoués en course** (`tcg-g4.md` §46) : x8 cmr +4,2 % (éteint), 0061 neutre
  partout (abandonné). `tcg/0062` `x-bat-old-mask` (diagnostic du masque d'origine) écrit,
  éteint, non nommé.
- **Plugin `POMPPC_GL_PACKBITS` allumé** (révision `20261101-packbits`, `plugin-allegement.md`
  §8) : bornage des flottants sur leurs bits, octets par table ; plugmd5 69 scènes 0 écart ; A/B
  d3 −3,9 %, ut −4,5 %, zen −4,2 %, mb −1,2 %, cmr −3,5 %. Installé dans la VM quotidienne ;
  `disks/prebuilt` mis à jour (l'ancien bundle dans `bench/packbits-0111/`). **Le M4 le reçoit
  avec le bundle** (code de l'invité, rien à porter).
- **Pourquoi cmr ralentit** (question de l'utilisateur, `tcg-g4.md` §47) :
  - le plugin GL fait 31 % de son vCPU, contre 11-22 % ailleurs ;
  - le PGO n'est entraîné que sur mb, zen, ut, d3 ;
  - les écritures de BAT (`copypv` des `read()`) sont communes à tous les jeux.
  `tcg/0063` `x-dbat-lite` (vidage d'un DBAT réduit à son mmu_idx, sans cache de sauts) est
  écrit et prouvé sans divergence. Il est **neutre sur cmr**, mitigé ailleurs (d3 −3 %, ut +3,7 %), et une session cmr sous
  vérificateurs a fini en **panique noyau** (délai de verrou) : éteint, non nommé.
- Arbre de mesure `~/src/qemu-nuit/build-x` : série + 0060-0063, tout éteint, -O2. La source de
  `target/ppc/mmu_helper.c` y est sans le diagnostic `POMPPC_BATSTAT` ; son diff est dans le
  scratchpad de la séance, à remettre au besoin.

**Vitesse** (binaire rapide, plugin packbits) : DOOM 3 ~78-80 ms/image en fenêtre, Colin McRae
~82 ms/image **en course** (plein écran).

**À faire sur le PC, dans l'ordre** (au retour du M4)
0. Vérifier à l'œil le curseur unique du frontal et la bande d'ombre avec le kext du 01/11 ;
   une cellule de preuve verte de la matrice en fenêtre (les tours du 01/11 étaient sans vidage).
1. PGO avec cmr : réentraîné le 01/11 (7 h-8 h 20) ; A/B neuf contre ancien neutre en fenêtre,
   cmr +6 % sur 2 parties (une polluée par mes dépouillements de profils, charge 3,1 ; une
   quittée `exit 138`) : **ancien binaire remis** (`build-fast`), nouveau dans
   `~/src/qemu/build-fast-pgo0111cmr/`. À refaire : A/B cmr 3 parties au repos strict avant de
   trancher (`vitesse-doom3-x86.md` §25). Le binaire instrumenté n'atteint pas la course en
   901 s : allonger `delai_scene` pour l'entraînement, ou entraîner cmr sur le binaire -O2.
2. Plugin sur cmr, suite de PACKBITS : profil de la course refait avec packbits
   (`xprof-0111-cmr-pb`, plugin-allegement §8) : plugin 28,2 % du vCPU, coût étalé
   (`geom_draw_client` 3,7 %, `geom_draw_native`, `send_state`, `prog_sync`, `__stub_getrealaddr`
   1-1,5 % chacun). Piste : `texture_unit_ok` ; qualifier d'abord les adresses de
   `__stub_getrealaddr` avant d'en déduire un coût d'appels par pointeur (relevé M4 du 01/11,
   `docs/re/plugin-symboles-tiger.md`).
3. `tcg/0063` : témoin sous `TLBPVERIFY=16 RETVERIFY=1` **sans** 0063 sur cmr. La panique de
   délai de verrou vient-elle des vérificateurs ? A/B en fenêtre mitigé (d3 −3,0 %, mb −2,2 %,
   ut +3,7 %, 2 parties) : à rejouer à 3 parties si le témoin innocente 0063.
4. x8 (0060) : la seule perte propre à cmr qui reste ; à instruire après le point 1.
5. Base de temps en ligne (`mftb` sans helper), gain modeste attendu.
6. Avis borné pour les surfaces non simples (TODO, chantier plugin) : ramener mb-fen sous
   fenêtre posée au niveau d'avant.
7. Les « sans traduction » de `x-ret-verify` en fin de session (`tcg-g4.md` §44) : peut-être
   parents de la rafale « sans PTE » de `tcg/0061` (§45, non reproduite).

**Défauts du PC** (`run_tiger.sh`, branche `Linux:x86_64`) : Quartz Extreme (depuis le 30/10 au
soir, `QE=0` l'éteint) et la série TCG jusqu'à `tcg/0056`, dont la **zone froide `x-tcg-cold`**
(depuis le 31/10, `TCGCOLD=0` l'éteint). Plugin `20261101-ombre` (PACKBITS, UVWIDE et leviers
A+B allumés, avis d'ombre à 20 Hz), kext du 01/11. `tcg/0057`-`0059` sont compilés et
éteints ; `tcg/0064` (`x-tb-head`, `TBHEAD=N`) est nommé depuis le 01/11 et éteint ;
`tcg/0060`-`0063` sont éteints et **non nommés** dans `build_qemu_qfb.sh` (arbre de
mesure `~/src/qemu-nuit`). Binaire rapide : profil PGO du 31/10 (2 bis), construit avant
`tcg/0064` : refusé par le lanceur jusqu'à sa reconstruction.

**Propres au PC** : `patches/diag/` (auto-profileur, plafonds), `tools/tcg/perfhw.sh` (compteurs
Intel), et l'arbre de mesure `~/src/qemu-ceil` (série et diag, -O2, sans PGO). `perf` exige
`perf_event_paranoid` ≤ 1, que l'utilisateur règle à la main ; la valeur revient à 4 à chaque
redémarrage.

## 2 bis. PC x86-64 : ce qui reste utile des séances du 30 et du 31/10

Réduit le 01/11 : ce qui est repris ou dépassé par la section 2 est retiré ; ses « À faire »
sont fondus dans la liste de la section 2. Le détail est dans les journaux cités.

- **Binaire rapide en production** : profil PGO réentraîné dans la nuit du 30 au 31/10 (cible
  ppc32mt, zone froide, 1 255 `.gcda`, construction à 2 h 43). A/B neuf contre ancien neutre en
  tout (mb, zen, ut −0,6 à −1,4 % ; cmr +0,3 % ; d3 +1,0 %, dans la dispersion) : c'est le
  profil du `build-fast/` d'aujourd'hui (`vitesse-doom3-x86.md` §23), reconstruit avec lui le
  31/10 à 16 h 04 après le correctif des reflets (`binaire-rapide-x86.md` §7). `~/src/qemu/build-fast-pgo2810/` est
  effacé.
- **Zone froide allumée** (31/10, `80aae4e`, `vitesse-doom3-x86.md` §23, `endurance.md` §15) :
  6 parties par bras sans gel de cmr ; A/B d3 −1,8 %, mb −1,2 %, zen −2,2 %, cmr −2,2 %, ut
  neutre. Avec la zone froide, cmr compte 20 parties pilotées et 1 gel (le 28/10), aucun dans
  les 17 suivantes. Endurance : 10 démarrages et 8 cycles de jeu, 0 incident.
- **Profil croisé des 5 jeux** (§23) : postes communs, la recherche de blocs (`qht`,
  `lookup_tb_ptr` : 10 % mb, 13,7 % zen, 6,1 % cmr, 3-4 % d3/ut) et la base de temps (`mftb`
  13,8 % sur ut, 1-3 % ailleurs ; sur ut, surtout la boucle au repos du noyau, `tcg-g4.md` §44).
- **`matab.sh --restore`** retire les variables de binaire héritées et affiche le binaire lancé
  (`6510319`, cause de la VM restée sur `~/src/qemu-ceil/build` le 28/10).
- **Reflets de la carrosserie de cmr** (31/10, `928a5f0`, `docs/re/cmr-rendu-vers-texture.md`
  §7) : défaut du cœur qgpu de l'hôte (`unit_texture_bound` respectait mal l'unité éteinte par
  le plugin sous programme de fragments) ; plus de reflet, comme sur un vrai Tiger/PowerPC qui
  se juge lent. `build/` et `build-fast/` reconstruits ; nouvelle référence cmr-pe (tour
  `cmr-nouvref-3110`) **validée le 01/11**, l'ancienne dans
  `bench/matrice/ref-linux/cmr-pe.faux-reflet-2810/`. Plancher de cmr : `plancher_ms_linux`
  105 depuis la mesure en course (`6054367`).
- `gltrap` : `POMPPC_GLTRAP_BINDS=1` (unité active et liaisons depuis le lancement), déposé
  dans `/Users/tiger/gltrap/` de la VM quotidienne.
- **Premiers essais de `tcg/0060` et `tcg/0061`** (`tcg-g4.md` §44-§45) : repris en course le
  01/11 (§46, section 2). De `0061` reste un défaut d'origine de QEMU : l'ancien BAT est vidé
  avec le masque de la nouvelle valeur (`tcg/0062` le relève ; TODO, « Plus tard »).
- **cmr à plusieurs résolutions** (`bench/tcg/ab/cmr-res-3110/`, sous ImGuiDock) : rejeux
  identiques partout (0,00 %), captures `<mode>/capture.png`. Les temps de ce tour sont pris
  **avant la course** (fenêtre de mesure d'avant `6054367`) : le « 16 bits ×2 » qui en sortait
  est clos (section 2).
- cmr, « couleurs, flous, reflets bizarres » signalés pendant la campagne du 31/10 au matin :
  tour avec preuve d'image vert, identique à la référence d'alors (0,00 %), non reproduit ; un
  premier essai avait gelé au lancement (aucune image GL).

## 3. Mac M4 : état au 01/11/2026, 23 h 37 (A/B en cours ; arrêt prévu le 02/11 à 6 h 40)

**Nuit du 01 au 02/11, reprise à 22 h 31** (demande de l'utilisateur : suivre le TODO jusqu'à
6 h 40, heure Réunion). Dépôt à jour sur `aad7721`. À la reprise, `.run/` et `bench/` sont
absents : anciens A/B, références binaires et script de reprise indisponibles. Le manifeste
versionné des références reste présent ; **ne pas confondre la nouvelle preuve avec un rejeu
des anciennes références**.
- QEMU reconstruit depuis HEAD, capacités complètes (`.run/build-nuit-0111.log`), aucun levier
  supplémentaire allumé. VM quotidienne : plugin `20261101-ombre` et kext chargés ; accès ssh
  du banc rétabli (nouvelle clé locale, jamais versionnée).
- Job `prebuilt` compilé par ssh dans la VM, sans installation : kext, plugin, GA, gltest,
  glwin et accelprobe rapportés dans `disks/prebuilt`, `VERSION=20261101-ombre`. Ancien paquet
  dans `bench/prebuilt-0111n/avant.tar`, journaux de compilation à côté.
- Preuve mb-fen `bench/matrice/preuve-nuit-0111-mb` : 6,2 ms/image, zéro repli, rejeu/VM
  0,03 / 0,02 %. Nouvelle référence regardée (bille au départ de gems.mis) et validée ;
  ancien enregistrement conservé dans l'historique Git.
- Campagnes **neuves prévues** `m4-bundle-0111n-{fen,pe}`, puis `m4-profils-0111n-{fen,pe}` par
  `.run/chaine-nuit-0111.sh`, après les jeux du banc (`.run/nuit-suite-0111.sh`, journal
  `.run/nuit-suite-0111.log`). Trois bras entrelacés :
  `avant` (PACKBITS=0, QE_NONSIMPLE=0), `bits` (1,0), `actuel` (1,1) ; trois parties par bras,
  mb/zen/ut/d3 en fenêtre, mb/cmr en plein écran. PACKBITS est ainsi séparé du coût de l'avis
  d'ombre. Barrière `chargehote.py attendre` et relevé continu de charge ; Time Machine arrêté.
  Ensuite profils hôte 30 s et invité 10 s avec les défauts, sur les cinq jeux.
- À 22 h 39, l'utilisateur confirme l'ombre correcte sur DOOM 3 avec la correction active ;
  le défaut qu'il venait de voir appartenait au bras `avant` (QE_NONSIMPLE=0). Campagne
  interrompue pour lever le doute, début conservé dans `m4-bundle-0111n-fen.interrompu-ombre` ; captures de la
  vérification dans `bench/ombre-m4-0111n/`. Rien à rouvrir dans le correctif.
- Un autre agent compile Swift sur l'hôte : pas d'A/B pendant ce travail. Barrière locale
  `.run/repos-strict-0111.py` avant les parties. En attendant, preuves d'image neuves
  `preuve-nuit-0111-{fen,pe}` (temps non retenus comme preuve de vitesse) ; Zenerchi regardé
  et validé aussi. `threadbusy.py --fonctions N` préparé pour le dépouillement des profils,
  coûts propres par fil et attentes séparées, quatre tests hors VM réussis.
- Tests de départ : 397 OK, 49 signalements ShellCheck préexistants, 5 ignorés. Les campagnes
  de vitesse sans vidage restent rouges par construction ; elles ne prouvent pas les images.
- Symboles du plugin et de libSystem identifiés par les binaires de cette VM
  (`docs/re/plugin-symboles-tiger.md`, empreintes et adresses exactes). Le coût attribué à
  `___stub_getrealaddr` n'établit pas à lui seul un coût d'appels par pointeur : distinguer
  le résolveur des stubs voisins sur les prochains profils. `0x90131200` appartient ici à
  `memcmp`/`bcmp` ; appelants et identité de la bibliothèque sur le PC restent à vérifier.
  `machonm.py --stubs` donne les 82 importations, identiques à `otool -Iv` ; trois tests
  hors VM passent. Aucun code de rendu changé.
- **Preuves neuves terminées à 22 h 54 : six cellules vertes sur six**, après validation à
  l'œil des références et réanalyse : `preuve-nuit-0111-fen` (mb, zen, d3, ut),
  `preuve-nuit-0111-pe` (mb, cmr). Zéro repli partout ; rejeu/VM 0,00/0,00 %, sauf mb-fen
  0,03/0,02 % et mb-pe 0,02/0,01 %. cmr-pe en course, 1920×1080, image sans le faux reflet.
  Les temps (6,6 / 3,1 / 30,3 / 19,9 ms en fenêtre, mb-pe 6,7, cmr-pe 44,6) passent les
  planchers mais **ne servent pas d'A/B** : autre agent actif, contrôles et analyses sur
  l'hôte. Les références des autres jeux/modes restent à reconstituer si leur tour est repris.
- VM éteinte proprement par le banc à 22 h 53 ; première barrière de l'A/B neuf en attente
  de la fin de l'agent externe. Avant la reprise, base d'endurance fraîche créée depuis la
  quotidienne arrêtée : `disks/tiger-endurance-m4-ombre-0111n.qcow2`, clone APFS autonome
  sous verrou, sources et tailles dans `bench/endurance/base-ombre-0111n.json`.
- Contrôle général après les outils : 400 OK, mêmes 49 signalements ShellCheck qu'au départ,
  5 ignorés ; nouveau test Mach-O exécuté et compilé séparément (non encore indexé lors du
  parcours `git ls-files` du harnais).

- **Contrat de 0058 corrigé**, sans allumage : `tcg/fixes/0058-poll-ns-contract.patch`,
  nommé par le constructeur. Quatre écarts reproduits contre `g_poll` de glib 2.90.1 sur
  ce M4 ; 42 cas sans écart après, deux mutants détectés (`pollnsproof.*`, `tcg-g4.md` §42.1).
  QEMU reconstruit, capacités complètes (`.run/build-pollns-contract-0111.log`), fonctions
  réellement construites recontrôlées. L'empreinte de la série change : le PC reconstruit
  toujours `build/` et `build-fast/` à son retour, même si ce chemin n'y est pas compilé.
- **Endurance : 10 démarrages froids sans incident** (22 h 57–23 h 05, binaire de 22 h 31,
  `977c844cd5d8b18b`, bundle ombre). Défaut du banc découvert à l'interruption de la phase
  suivante : Ctrl-C pendant l'écart initial, hors du `try`, laissait sa VM tourner. Instance
  isolée fermée proprement par ssh ; nettoyage du banc corrigé, quatre tests réussis et
  contrôle réel à deux instances prévues (seule la première démarre), aucun QEMU résiduel.
  `docs/endurance.md` §16. **Jeux terminés** : `m4-ombre-0111n-jeux-corr`, 5 démarrages et 4 cycles mb/zen/ut/d3
  sans incident, binaire `b8921ddf205e6b22`, `POLLNS` toujours éteint. Le premier essai `m4-ombre-0111n-jeux`
  interrompu n'est pas compté comme un cycle de jeu réussi.
- **Tranche d'endurance close à 23 h 36** : 15 démarrages de contrôle et 4 cycles de jeu
  sains (contrôle d'interruption séparé), base sans erreur à `qemu-img check`. Sources du
  banc corrigé précisées dans les dossiers de campagnes (lancées avant le commit, HEAD du
  rapport `aca7459`, code du banc ensuite committé `04574f7`). `endurance.md` §16.
- **A/B neuf démarré à 23 h 36**, journal `.run/nuit-suite-0111.log` ; première barrière
  calme, zéro QEMU, 0,48 cœur étranger. Barrière explicite sur agents externes, compilations,
  autres campagnes d'endurance, Time Machine, puis `chargehote.py attendre`. Pilotage conservé
  dans `bench/tcg/ab/pilotage-m4-0111n/`. Le frontal compilé à 18 h est à jour (aucun fichier
  suivi plus récent) ; validation visuelle du curseur encore à faire.
- **Tests après corrections : 405 OK, mêmes 49 signalements ShellCheck, 5 ignorés.**

**Fait le 01/11** (après-midi ; Time Machine et hermes tournaient en parallèle sur l'hôte)
- **QEMU reconstruit** (`a0e028e`, `.run/build-0111.log`) : `qgpu-core.c` du 31/10, `tcg/0059`
  et `tcg/0064` compilent sur arm64 (`✔ x-tb-head`), rien d'allumé de plus.
- **Kext, plugin `20261101-ombre` et GA** compilés **dans la VM quotidienne** (elle a gcc-4.0 ;
  sources par ssh dans `~/pomppc-build`, `install.sh` sans `QE`), invité redémarré :
  `QGPUVersion` 22, 4 clients. Anciens binaires dans `/var/tmp/avant-0111/` de l'invité.
  `disks/prebuilt` n'est pas refait sur le M4 (`run_tiger.sh` le dit plus ancien que les
  sources : job `tools/guest/jobs/prebuilt` à repasser).
- **Matrice de preuve** `bench/matrice/preuve-0111-m4` : mb-fen 6,3, mb-pe 6,5, d3-fen 29,7,
  d3-pe 30,1 ms/image, **vertes** (temps pris sous Time Machine). cmr-pe rouge : sa référence
  du 27/09 portait le **faux reflet** (même cause que le PC ; le jeu se range aussi en
  « machine lente » sur le M4 : `ValLowMachine` 1, `docs/re/cmr-rendu-vers-texture.md` §7).
  Ancienne référence dans `bench/matrice/ref/cmr-pe.faux-reflet-2709/`, nouvelle prise par
  `bench/matrice/cmr-nouvref-m4-0111` (41,8-42,6 ms/image en course), **à valider à l'œil**
  (`matrice.py --valider cmr-pe`). Le M4 mesurait déjà la course avant `6054367`.
- **A/B `m4-bundle-0111`** (`bench/tcg/ab/m4-bundle-0111-chaine.sh`) : comportement d'avant
  (`POMPPC_GL_PACKBITS=0 POMPPC_GL_QE_NONSIMPLE=0`) contre défaut, mb/d3 fen et mb/cmr pe,
  3 parties par bras ; barrière : fin de Time Machine puis hôte au repos ; finit par
  `matab.sh --restore`. Bilans : `bench/tcg/ab/m4-bundle-0111-{fen,pe}/bilan.txt`.

- **Passes de cohérence doc/code (soir)** : six passes de documentation, puis dix passes
  « code ↔ doc » en worktrees fusionnées dans `main` (le code fait foi). Elles ont touché des
  **commentaires** de `patches/` (qgpu-pci/core/gl, `qgpu_proto.h`, `qgpu_abi.h` et sa copie du
  kext, `screamer.c`, en-têtes de `tcg/0037` et `0038`) : l'empreinte de la série change. Le M4
  a reconstruit `~/src/qemu/build` sur `d086f03` (`.run/build-0111b.log`) ; **le PC reconstruit
  `build/` et `build-fast/`** (de toute façon dû à `tcg/0064`). Le kext, le plugin et le GA de la
  VM quotidienne restent valables (seuls des commentaires ont changé ; code du plugin vérifié
  identique hors commentaires). Bugs corrigés dans le code : paquet invité publié et CD des
  sources sans plugin GA (QE), CD des sources non regravé sur les en-têtes du contrat, test
  §5 bis de `run-all.sh` en échec depuis le 29/10 (copies QE du kext, maintenant vérifiées par
  valeur), deux libellés du frontal, deux messages de `run_tiger.sh`. `tests/run-all.sh` :
  397 OK ; les 49 échecs sont des avertissements de shellcheck 0.11, tous présents avant.
- **A/B relancé** après la reconstruction, puis **arrêté** à la demande de l'utilisateur
  (redémarrage de l'hôte) ; VM éteinte proprement, verrou non touché. Joué en fenêtre seulement,
  2 parties par bras (avant-3 interrompue, rangée en `.interrompu`) : **d3-fen 29,1 → 29,0
  (−0,0 %)**, **mb-fen 6,0 → 6,2 (+2,5 %)** (`matabsum.py bench/tcg/ab/m4-bundle-0111-fen`).
  PACKBITS ne se voit pas sur d3 au M4 ; mb-fen paie l'avis d'ombre (Terminal posé par la
  matrice), moins que le +6 % du PC. **À reprendre** : 3ᵉ partie par bras en fenêtre, puis le
  plein écran (mb-pe, cmr-pe) : `bench/tcg/ab/m4-bundle-0111-chaine.sh` (ranger d'abord
  `m4-bundle-0111-fen` en `.partiel`). Puis `matab.sh --restore` si la chaîne ne finit pas.
  Référence cmr-pe du M4 toujours à valider à l'œil.

Ci-dessous, l'état du 30/10 (fusionné le 01/11 avec l'ancienne « 3 bis » du 29/10).

**Défauts sur Darwin:arm64** : depuis le 26/10 (`550b2a8`), la série du PC est allumée et
prouvée : `PPC32MT TLBPRECISE LMWINLINE DCBZINLINE FPNATIVECMP VFPNATIVECMP DIRTYSKIP PTEGMAP
JC2WAY FPKNOWN FPRFLAZY STFSKNOWN LFSKNOWN TLBBASE`. A/B : DOOM 3 −23,9 %, UT2004 −18,6 %,
Marble Blast −22,7 %. Garde : un binaire sans tcg/0037-0039 les laisse éteints. Quartz Extreme
allumé depuis le 29/10 (`c792464` : `run_tiger.sh` pose `qe=on`, `QE=0` l'éteint ; le kext en
personnalité « auto » le suit). `tcg/0057`, `0058` compilés, éteints ; `x-tcg-cold` (`0056`)
compilé, sans effet sur arm64.

**Binaire et VM quotidienne** : `~/src/qemu/build` reconstruit dans la nuit du 01 au 02/11
(série jusqu'à `0059` et `0064`, plus le correctif de contrat de 0058, éteint) ; kext du 01/11
et plugin `20261101-ombre` (voir ci-dessus).

**Fait le 30/10**
- **Émulateurs voisins, A/B au repos** (`docs/tcg-g4.md` §42, 4 parties par bras, mb zen ut
  d3 en fenêtre). `tcg/0057` `x-vcpu-qos` (`VCPUQOS=1`, fils des vCPU en QoS interactive) :
  **neutre**. `tcg/0058` `POMPPC_POLL_NS` (`POLLNS=1`, attente de la boucle principale à la
  microseconde par `select`) : zen **−18 %** mais ut **+6,6 %**. Les deux sont **éteints**
  par défaut, compilés dans `~/src/qemu`. Le PC n'est pas concerné : le code est propre à
  macOS.
- **Bug QE corrigé : bande blanche, noire ou « ancien jeu » sous l'ombre d'une fenêtre posée
  sur un jeu en fenêtre** (signalé par l'utilisateur). Dans le WindowServer, le chemin
  rapide des clés d'état liait la texture de surface vide au lieu de la texture d'export du
  jeu. Correctif : `tex_bind_id` dans le plugin, sans changer le protocole, ni QEMU, ni le
  kext. Vérifié en VM jetable sur Zenerchi, Marble Blast et DOOM 3 ; matrice mb/zen/d3 en
  fenêtre dans `bench/matrice/qe-ombre-3010`. Coins noirs de DOOM 3 et Prey : la fenêtre
  est `NSBackingStoreRetained` (relevé sous gdb), comme sur un vrai Mac ; pas un bug.
  (01/11 : la bande « ancien jeu » sous DOOM 3 avait une seconde cause, corrigée sur le PC par
  le kext du 01/11 et le plugin `20261101-ombre`, section 2.)
- **Fin de séance, 11 h 30** : la VM jetable est arrêtée et `git` est propre. La VM
  quotidienne a été relancée par `matab.sh --restore`, puis s'est fermée ; aucune VM ne tourne.
  Elle repart avec `./run_tiger.sh`.

**Fait du 28 au 29/10** (détail : CHANGELOG, `docs/quartz-extreme.md`, `docs/endurance.md`)
- Vidage autonome par bandes (`403593e`, `QGPU_CAP_TEX_READBACK_ROWS`, sans changer
  `QGPU_PROTO_VERSION`) : cmr-pe en 1920×1080 vert. Une texture hôte sans image est comptée à
  part (« 2/2 (1 sans image) ») ; un refus du device se distingue par l'en-tête prérempli à
  0xFF (`917ae47`).
- **Matrice entière `20261028-1854` : 17 cellules vertes sur 17**, dont Warcraft III. Gain sur
  le 26/10 non attribué (frontal et état changés, aucun A/B) ; cmr +11 % mais passé de 1024×768
  à 1920×1080.
- **Endurance du 28 au 29/10** (`endurance.md` §11) : 342 démarrages et 40 cycles de jeu, aucun
  incident ; QE : §12 (ancien chemin) et §13 (chemin direct), 156 démarrages et 32 cycles de
  jeu en fenêtre, aucun incident.
- **Quartz Extreme P1 à P3** (`quartz-extreme.md` §1 ter et journal) : bureau juste, fenêtres
  GL par l'hôte, puis le jeu présente lui-même son échange par le noyau (méthode 5). A/B au
  repos en fenêtre : QE au niveau de QE éteint ou mieux (mb 6,1-6,3 contre 6,4, zen 2,9-3,1
  contre 3,5, d3 28,8-29,7 contre 29,8, ut 19,6-19,7 contre 19,9 ms/image).
- Outil : `/var/tmp/pomppc-env` passe les variables `POMPPC_*` au WindowServer.

**Pièges du M4**
- `matab.sh --restore` lance la VM sous ImGuiDock, les parties de `matab.sh` sous le frontal
  natif : la résolution des jeux en plein écran en dépend (1920×1080 contre 1024×768).
- Un frontal ImGuiDock resté ouvert après l'arrêt de sa VM tourne à 60 % d'un cœur et double
  les temps en fenêtre ; la fenêtre de mesure d'UT (60 images) demande 3 passes.
- Les traces du plugin (`POMPPC_GLTRACE`, vidages de tampons) ont **rempli le disque de
  l'invité** (16 Go) le 29/10 : les effacer après usage.

**À faire sur le M4** : la liste ordonnée est **au §4, « Venu du PC le 01/11 »** (même ordre que
le chantier M4 de `TODO.md`). Hors de cette liste, seulement si besoin : parité de **tcg/0055**
(si les traces de second niveau reprennent, arrêtées depuis le 22/10) ; `vperm` en ligne
(TBL/TBX, gain plafonné vers 0,3 %).

**Propres au M4** : pas de `qemu-ppc` linux-user (les preuves passent par l'invité), outils
`ppc32mt-bench.sh` et `chargehote.py` portés sur macOS, et `references.csv`, le fichier de
références de la matrice **du M4** (le PC utilise `references-linux.csv` et ne touche jamais
`references.csv`).

## 4. À faire en passant d'un hôte à l'autre

- **Venu du PC le 01/11 (pour le M4), dans l'ordre** — reprend les entrées du 30/10 et du
  31/10, réduites le 01/11 à ce qui n'y figure pas :
  (01/11 : points 1 et 2 faits, la VM **quotidienne** servant de VM de dev ; points 3-4 en
  cours, §3.)
  1. `git pull` ; **reconstruire QEMU** (`qgpu-core.c` du 31/10 : reflets de cmr ; `tcg/0059`
     nommé depuis le 30/10) ; ni le protocole ni l'ABI n'ont changé. Vérifier que la série se
     compile sur arm64 avec `tcg/0059` (l'opcode générique n'y est jamais émis ; rien ne change
     sans `VPERMINLINE=1`).
  2. Dans la VM de dev du M4 : **reconstruire kext, plugin et GA** depuis HEAD (kext du 01/11
     avec la méthode 6 ; plugin `20261101-ombre` qui contient aussi PACKBITS et UVWIDE),
     `install.sh` sans `QE`, puis **redémarrer l'invité**. Les kext du 30/10 (`805f504`, curseur
     immobile) et plugins du 30/10 (`f01d2d7`, `pomppc_qe_same` ; `tex_bind_id`) y sont inclus.
  3. À l'œil : bande sous l'ombre du Terminal sur un jeu en fenêtre pendant qu'il change
     (DOOM 3 au chargement, Marble Blast), curseur net en plein écran, reflets de carrosserie
     de cmr.
  4. A/B au repos, ancien bundle contre nouveau : mb-fen, mb-pe, cmr-pe et d3-fen au moins
     (PACKBITS attendu −1 à −4 % ; l'avis d'ombre coûte sur mb-fen seulement quand une fenêtre
     est posée sur le jeu, +6 % sur le PC) ; cmr-pe rejoué avec la fenêtre de mesure **en
     course** (`6054367` ; le M4 mesurait peut-être déjà la course : le voir sur ses
     `frames.csv`) et sa référence reprise si elle porte le faux reflet. `references.csv` du M4
     ne se touche que là-bas.
  5. Profil croisé des cinq jeux avec `sample` (part du JIT, recherche de blocs, `mftb`,
     helpers, et part du fil `qgpu-render` sur d3) : c'est lui qui ordonne les leviers du M4
     (TODO, chantier M4). Puis `tcg/0058` limité aux délais courts (`select` sous la
     milliseconde, profil d'UT) ; parité de la zone froide **tcg/0056** en dernier (un saut long
     vers le chemin lent sur aarch64 : le `b.cond` imm19 n'a qu'1 Mio de portée, pour une région
     de 64 Mio).
  6. Une tranche d'endurance avec le nouveau kext, puis cette page.

- **Avant de quitter un hôte** : arrêter les campagnes en cours, remettre la VM quotidienne
  (`tools/tcg/matab.sh --restore`), mettre à jour cette page, puis commit et push.
- **En arrivant** : `git pull`, reconstruire si `patches/` a bougé, lire la section de l'hôte.
- Le disque Tiger raw de la VM quotidienne peut circuler par USB entre les hôtes ; il se
  prépare sur le M4 (`docs/gpu-3d-tiger.md` §5.1).
- Ce qui est commun aux deux hôtes : le frontal, le protocole qgpu, le kext et le plugin de
  l'invité, la matrice et ses jeux. Un changement de `patches/qgpu/qgpu_abi.h` impose de
  reconstruire le kext et de redémarrer l'invité sur **les deux** hôtes ; un changement de
  `qgpu_proto.h` impose de reconstruire QEMU et le plugin.
