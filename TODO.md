# TODO — priorités de POMPPC

> **Deux hôtes (PC x86-64 et Mac M4) en alternance** : l'état de chacun et ce qui y reste à faire
> sont dans [docs/etat-des-hotes.md](docs/etat-des-hotes.md), à lire en premier.

État au 02/11/2026. Objectif : Mac OS X Tiger sous QEMU, bureau et jeux OpenGL
sur le GPU de l'hôte, avec une matrice de jeux verte comme preuve.

**Où on en est.** La fiabilité a été tenue (endurance PC 683 démarrages sans incident le 28/10,
M4 342 démarrages et 40 cycles de jeu sans incident le 29/10, puis 70 démarrages et 8 jeux
avec le nouveau bundle dans la nuit du 01 au 02/11 ; Quartz Extreme allumé sur les
deux hôtes). **L'orientation est de nouveau la vitesse, sur les deux hôtes**, car la version x86
reste trop lente pour être publiée (règle du 20/10). Vitesse, ms/image, hôte au repos
(PC du 01/11, M4 du 02/11) :

| jeu | PC x86-64 (binaire rapide, plugin `packbits`) | M4 (02/11, QE) | rapport |
|---|---:|---:|---:|
| DOOM 3, fenêtre | 78-80 | 28,9 | ×2,7 |
| UT2004, fenêtre | 40 | 19,3 | ×2,1 |
| Marble Blast, fenêtre | 8,3 | 6,2 | ×1,3 |
| Zenerchi, fenêtre | 4,6 | 2,6 | ×1,8 |
| Colin McRae, plein écran, **en course** | 82 | 41,7 (1920×1080) | ×2,0 |

M4 : médianes du témoin sans attente précise de l'A/B `m4-pollns-0211`, trois parties,
frontal Cocoa, profils pris après les fenêtres (`docs/tcg-g4.md` §42.2). Ces valeurs
remplacent le relevé sous Time Machine ; elles ne mesurent pas ImGuiDock.

Cible du PC : DOOM 3 à 50 ms/image (`docs/vitesse-doom3-x86.md` : 148,7 au premier relevé du 03/10, §2 ; 111,5 avec le
binaire rapide le 06/10, §13).

**Ce que les profils ont établi** (`docs/vitesse-doom3-x86.md` §18-§25, `docs/tcg-g4.md` §44-§47) :
- Sur le PC, le goulot du code généré est le **chargement des instructions** (frontend bound
  40 %, 100 octets de x86 par instruction PowerPC) et la latence mémoire, pas le nombre
  d'instructions : retirer des tests hors du chemin critique ne rapporte plus rien (plafond
  mesuré 3,5 %). Les leviers restants du JIT x86 sont petits et génériques : recherche de
  blocs (mb 10 %, zen 14 %, cmr 6 % du vCPU), base de temps (ut 14 %), PGO.
- **Le plugin GL de l'invité est le levier commun aux deux hôtes** : 28 % du vCPU de cmr,
  22 % de d3, 16 % de mb, 12 % de zen, 11 % d'ut sur le PC. Chaque pourcent gagné là vaut sur
  x86 et sur arm64 sans portage, et c'est le seul levier qui agit sur cmr (PACKBITS −3,5 %
  quand les leviers TCG le laissent neutre ou le ralentissent).
- Le fil de rendu de l'hôte (`qgpu-render`) occupe 55 % d'un cœur sur mb, 41 % sur zen, 25 %
  sur d3 (≈ 20 ms par image). Sur le PC, l'invité ne l'attend pas (0,1-3 ms/image) ; sur le M4,
  où d3 est à 29 ms, il peut devenir le chemin critique : à mesurer.

## Cinq pistes proposées le 01/11 (à arbitrer par l'utilisateur)

Cinq améliorations qui ne sont pas encore des chantiers : chacune avec le gain attendu, le coût,
et la preuve qui la fermerait. L'ordre est celui que je recommande ; rien n'est lancé sans
décision. Une piste retenue monte dans « Maintenant » (au plus trois chantiers).

1. **Mesurer ce que l'utilisateur voit : ImGuiDock contre le frontal natif.** Tous les A/B passent
   sous le frontal natif de QEMU (`matab.sh` : GTK sur le PC, Cocoa sur le M4) ; l'utilisateur
   joue sous ImGuiDock (`-display dbus,p2p=on`, images
   poussées par D-Bus puis texture, `frontend/README.md`). Ce coût n'a jamais été mesuré et il
   croît avec la résolution (3 Mo par image en 1024×768×32, 8 Mo en 1920×1080) : c'est un
   suspect des « gros ralentissements en 1920×1080 » de cmr. Travail : A/B des cinq cellules
   sous les deux frontaux à scène égale ; si écart, scanout en mémoire partagée (memfd ou
   dmabuf, prévus par `org.qemu.Display1`) et rectangles sales seulement. **Fermeture :** écart
   ImGuiDock − frontal natif < 2 % sur la matrice, cmr 1920×1080 remesuré. Coût : une journée.
   Gain :
   tout l'écart mesuré, sur les deux hôtes, dans l'usage réel.
2. **Régime de compilation du plugin de l'invité.** Le plugin fait 11-28 % du vCPU selon le jeu :
   10 % de mieux sur lui vaut 1-3 % sur chaque jeu, sur les deux hôtes, sans portage. Leviers :
   PGO et LTO du plugin lui-même (le gcc 4.0 de Tiger a `-fprofile-generate/-fprofile-use` ;
   profil pris en jouant la matrice dans la VM de dev), appels internes directs (`static`,
   visibilité cachée, gain à mesurer ; l'étiquette `___stub_getrealaddr` à 1 % sur cmr doit
   d'abord être résolue sur ses adresses, `docs/re/plugin-symboles-tiger.md`),
   configuration lue une fois (A5 : plus de `getenv` dans le chemin chaud). **Fermeture :**
   plugmd5 0 écart, A/B des cinq jeux, tous en gain. Coût : deux à trois jours (agent Opus).
   Risque : la maturité du PGO de gcc 4.0 sur PowerPC.
3. **Fil de rendu de l'hôte : convertir les attributs une fois.** `nat_conv_attr` fait 27 % de
   `qgpu-render` (attributs reconvertis à chaque dessin natif ; à convertir au `BUF_SUBDATA`).
   Sur le PC le fil prend 20 ms par image de d3 sans bloquer l'invité ; il dispute aussi les
   cœurs rapides aux vCPU sur mb et zen. **Profil M4 du 02/11 fait** : sur d3, 0,178 cœur de
   travail estimé et 82,2 % d'attente de condition (`docs/profil-m4-0211.md` §2), ce qui ne
   justifie pas de placer ce levier en tête sur le M4. **Fermeture :** A/B des cinq jeux sur
   les deux hôtes. Gain utile à chiffrer selon le jeu ; réduction de CPU hôte attendue.
   Coût : deux jours. Aucune optimisation de cette piste lancée.
4. **Boîte noire des paniques.** Les plantages en jeu (délai de verrou, `exit 138`/`139`) restent
   « sans cause » faute de relevé au moment même. Levier : QEMU arme un point d'arrêt sur
   `_panic` du `mach_kernel` (adresse connue de `tools/re/kpanic.py`) et, à l'entrée, écrit
   registres, pile, RAM et console dans `.run/panique-<date>/` puis laisse l'invité continuer ;
   côté invité, le rapport CrashReporter d'un `exit 138` est rapatrié par ssh. **Fermeture :**
   une vraie occurrence dépouillée de bout en bout. Coût : une journée. Gain : la condition de
   fermeture du chantier « Plantages en jeu sans cause » existe enfin.
5. **Temps de chargement des jeux.** cmr met ~70 s à charger sa course, DOOM 3 autant pour un
   niveau, le binaire PGO instrumenté n'atteint pas la course en 900 s : jamais mesuré ni
   optimisé, et c'est ce que l'utilisateur subit à chaque partie. Mesurer d'abord (secondes du
   lancement à la scène de mesure, par jeu ; la matrice a déjà l'horloge), puis les pistes :
   mémoire de l'invité 768 Mo → 1,5 Go (le mac99 va à 2 Go ; moins de pagination, les
   `copypv` des `read()` à 1 000-2 800/s), cache disque des shaders traduits sur l'hôte (« Plus
   tard »), disque IDE (DMA, taille des requêtes). **Fermeture :** temps de chargement dans
   `frames.csv` ou le tableau de la matrice, et un levier prouvé. Coût de la mesure : une
   demi-journée.

Les chantiers sont rangés par priorité ; leur domaine figure entre crochets.
**Maintenant** contient au plus trois chantiers, **Ensuite** donne l'ordre de prise,
**À confirmer par l'utilisateur** réunit ce qui n'attend qu'un essai ou une décision de sa part,
**Plus tard** garde les pistes différées, **Écarté sur chiffres** ce qu'il ne faut pas rouvrir sans
fait nouveau, **Bloqué** indique ce qui manque pour reprendre.
Une entrée décrit le travail restant et la preuve qui la fermera. Pour les optimisations :
preuve d'équivalence (`plugmd5` à l'octet, `x-*-verify`, mutants) et A/B entrelacé à scène
égale, hôte au repos, sur **toute la matrice** (mb, zen, ut, d3, cmr) : un levier n'est allumé
que s'il ne perd sur aucun jeu ; un écart sous 3 % sur trois parties ne prouve rien (six parties
ou un bras témoin). Pour les défauts : reproduction avant correction et contrôle après. Les
travaux clos sont dans [CHANGELOG.md](CHANGELOG.md), les études détaillées dans `docs/`.
L'ancien tableau par domaine : `docs/archive/todo-par-domaines-2026-09-27.md`.

## Maintenant

Ordre du 01/11 : la séance de l'après-midi est sur le M4, son chantier passe en tête (même ordre
que `docs/etat-des-hotes.md` §4) ; le plugin est
commun ; les leviers du JIT x86 attendent le retour sur le PC.

- [ ] **[TCG — M4] Mesurer le M4 avant de l'optimiser, puis rattraper le PC.** Profils croisés
  renouvelés le 02/11 (`docs/profil-m4-0211.md`). Dans l'ordre :
  1. **Rattrapage** (01/11, en partie fait) : `~/src/qemu` reconstruit ; kext, plugin et GA
     compilés et installés dans la VM quotidienne, invité redémarré ; mb et d3 fen/pe vertes ;
     nuit du 01 au 02/11 : artefacts locaux absents, six preuves reconstituées et vertes
     (mb/zen/d3/ut fen, mb/cmr pe), références regardées et validées, cmr en course sans le
     faux reflet. Ombre de DOOM 3 confirmée par l'utilisateur à 22 h 39. Restent : curseur net
     en plein écran à l'œil. A/B neuf `m4-bundle-0111n` terminé le 02/11 à 1 h 36 :
     PACKBITS UT −3,19 %, cmr en course −7,08 %, autres cellules sous 3 % et inconclusives
     sur trois parties ; bras non-simple sous 1 %, avis actifs non comptés.
     Détail : `docs/profil-m4-0211.md` §1, `docs/etat-des-hotes.md` §3-§4.
  2. **Profil croisé des 5 jeux** (`sample` du processus QEMU, fils vCPU et `qgpu-render`,
     comme `tools/re/perfpart.py` sur le PC) : part du JIT, recherche de blocs, `mftb`,
     helpers, et **part du fil de rendu hôte** : fait le 02/11, §2 du profil. Sur d3,
     `qgpu-render` travaille environ 0,178 cœur, attend en condition 82,2 % du relevé :
     pas de justification pour faire passer la piste 3 (`nat_conv_attr`) en tête.
  3. **`tcg/0058` délais courts** : l'attente à la microseconde fait zen −18 % mais ut +6,6 % ;
     variante `select` pour les seuls délais sous la milliseconde, profil d'UT (verrou).
     Contrat de `g_poll` corrigé le 01/11 nuit (`tcg/fixes/0058`, 42 cas et deux mutants) ;
     les chiffres du 30/10 précèdent cette correction. Variante `tcg/0065` préparée le
     02/11 (`POLLNS=2`, strictement sous 1 ms), 56 cas et deux mutants de routage. **A/B
     des cinq jeux clos à 4 h 30 : écartée**, zen −8,20 % mais UT +10,06 % (mode 1
     corrigé : −9,60 / +10,59 %) ; autres écarts sous 3 % et inconclusifs, cmr inclus.
     `archive/m4-pollns-court-0211`, `tcg-g4.md` §42.2. Le levier reste éteint. Sur UT,
     travail d'horloge 0,013→0,18 cœur et charge vCPU 1,07→1,63 : **appelant identifié**
     sur le M4 dans `delay_idle` (noyau xnu-792.24.17, attente active SMP),
     `docs/re/ut-attente-m4.md`. Répartir la perte entre réveils, attente et verrou avant
     un nouveau levier ; aucune attente du noyau n'est supprimée.
  4. Parité de la zone froide (`tcg/0056`, saut long aarch64) en dernier : gain PC de 1-2 %
     sur un goulot L1i propre au PC. **Revue et preuve préparatoire faites le 02/11** :
     36 portes longues exécutées sur le M4, deux mutants détectés ; familles mémoire,
     FP et tlbb à couvrir, régions SMP=1 et pools littéraux à traiter. Implantation et
     A/B restent à faire (`docs/tcg-zone-froide-arm64.md`), levier toujours sans effet.
  5. Tranche d'endurance du kext du 01/11 **faite dans la nuit** : 15 démarrages de contrôle
     et 4 cycles, puis 55 démarrages et 4 cycles avec le binaire final : **70 démarrages
     et 8 jeux sains** (`endurance.md` §16–17). Restauration détachée corrigée et vérifiée.
  **Fermeture :** doc de profil M4, PACKBITS mesuré, A/B de chaque levier, `etat-des-hotes.md`.

- [ ] **[Plugin — les deux hôtes] Alléger le plugin de l'invité, suite de PACKBITS**
  (`docs/plugin-allegement.md` §8, profil `bench/tcg/ab/xprof-0111-cmr-pb`). Le coût est
  maintenant étalé : `geom_draw_client` 3,7 % du vCPU de cmr, `geom_draw_native` 1,5,
  `send_state` 1,3, `prog_sync` 1,1, `geom_dispatch_locked` 1,1, **`___stub_getrealaddr` 1,0**
  (étiquette à résoudre avant d'en tirer un levier : sur le M4, résolveur du stub de
  `fprintf`, suivi des 82 stubs d'importation ; `docs/re/plugin-symboles-tiger.md`),
  `texture_unit_ok` 0,9, `geom_format` 0,8, `us_fill`/`us_get` 1,3,
  `prog_params_arena`/`prog_state` 1,4 ; dans libSystem, le bloc `0x90131200` fait 1,8 % sur
  cmr et 0,7 % sur d3/mb (identifié sur le M4 dans `memcmp`/`bcmp`, libSystem MD5
  `80719800…` ; appelants et empreinte du PC à vérifier). Méthode du §2 : un levier par
  variable `POMPPC_GL_*`, mode `=2` de contrôle, `plugmd5` 0 écart, A/B des cinq jeux sur le
  PC ; le M4 a le bundle depuis le 01/11 (VM quotidienne), A/B PACKBITS neuf terminé le
  02/11 : UT −3,19 %, cmr −7,08 %, autres écarts inconclusifs (`profil-m4-0211.md` §1).
  S'y rattache l'**avis borné** des
  surfaces non simples (QE, 01/11) : le
  coût de l'avis d'ombre (mb-fen +6 % mesuré sur le PC quand une fenêtre est posée sur le jeu)
  vient d'un avis sur toute la surface ; le relevé M4 du 02/11 n'a pas d'avis actifs en régime
  stable, son coût reste à mesurer sous une fenêtre posée. Le kext connaît le rectangle
  englobant de la partie non visible : le renvoyer par la méthode 6 et n'aviser que lui, puis
  remonter la cadence (fermeture : mb-fen sous fenêtre posée au niveau de
  `POMPPC_GL_QE_NONSIMPLE=0`, bande toujours juste). **Fermeture :** plugin sous 20 % du vCPU
  sur cmr et d3, aucun jeu en perte, matrice verte.

- [ ] **[TCG — PC] Les leviers génériques restants du JIT x86** (`docs/tcg-g4.md` §44-§47) :
  1. **PGO entraîné avec cmr** : `pgo-train.sh --plein-ecran cmr` (01/11), réentraînement
     fait, A/B neuf contre ancien (`bench/tcg/ab/pgo-0111-*`) : neutre en fenêtre, cmr non
     prouvé (+6 % sur 2 parties, l'une polluée par une charge de l'hôte, `neuf-1` quittée
     `exit 138`). **Ancien binaire remis**, nouveau gardé dans `~/src/qemu/build-fast-pgo0111cmr/` :
     recontrôle cmr 3 parties au repos strict, et entraînement de cmr sur un binaire qui
     atteint la course (l'instrumenté n'y arrive pas en 901 s).
  2. **`tcg/0063` `x-dbat-lite`** : témoin cmr sous `TLBPVERIFY=16 RETVERIFY=1` **sans** 0063,
     pour savoir si la panique de délai de verrou du 01/11 vient des vérificateurs ; puis
     rejouer l'A/B en fenêtre à 3 parties (2 parties : d3 −3 %, mb −2,2 %, ut +3,7 %).
  3. **Recherche de blocs** (mb 10 %, zen 14 %, cmr 6 %) : `tcg/0060` x8 (effacement exact, 256
     cases) gagne mb −8,7 % et ut −1,9 % mais **cmr +4,2 %** en course, seule perte propre à
     cmr qui reste inexpliquée. À instruire : compter ce que coûte le parcours exact sous les
     ~2 000 écritures de BAT par seconde de cmr (32 pages chacune), puis essayer **x8 +
     `tcg/0063`** (plus de vidage du cache de sauts sur un DBAT) ; l'un sans l'autre a été
     mesuré, pas les deux.
  4. **Base de temps** : `helper_load_tbu/tbl` 14 % du vCPU sur ut, 3 % sur zen. La boucle
     `0x33f3c-0x33fec` est identifiée le 02/11 **sur le M4** dans `delay_idle`, attente
     active SMP de xnu-792.24.17 (noyau MD5 `def99c6e…`, `docs/re/ut-attente-m4.md`).
     **Vérifier l'empreinte et les octets du noyau sur le PC** avant d'y transférer ce
     résultat (ancien profil : 11,7 % du vCPU d'ut). Instruire alors un sommeil hôte
     respectant les réveils (ut occupe 1,9 cœur sur le PC), puis juger `mftb` en ligne :
     accélérer cette boucle n'est pas en soi un gain utile au jeu (−2,4 % mesuré pour
     helper lent → rapide).
  **Fermeture de chaque levier :** preuve, A/B des cinq jeux, allumé ou archivé avec le chiffre.

## Ensuite

Dans l'ordre. Chaque entrée passe par le banc d'endurance quand elle s'y prête.

- [ ] **[Matrice — PC] Un tour complet avec le kext et le plugin du 01/11** : les tours du 01/11
  étaient `--sans-vidage` (cellules rouges par construction) ; il faut des cellules de preuve
  vertes en fenêtre et en plein écran avec le cœur corrigé du 31/10 (dernier tour complet du PC :
  07/10). Le bug de l'ombre est clos (`docs/quartz-extreme.md` journal du 01/11, CHANGELOG) ; sa
  suite, l'avis borné, est dans le chantier plugin.

- [ ] **[Jeux] Performances selon la résolution** — gros ralentissements signalés par
  l'utilisateur dans **Colin McRae en 1920 × 1080** (27/10 ; sur le M4 la cellule y est à +11 %
  contre 1024×768). Étendre les essais aux autres résolutions, jeux et modes ; relever les temps
  d'image et les replis sur une même scène. Les cellules vertes aux résolutions de la matrice ne
  valent pas pour toutes. Premier suspect : le frontal ImGuiDock (piste 1 ci-dessus), dont le
  coût par image croît avec la résolution ; dans les captures du 31/10
  (`bench/tcg/ab/cmr-res-3110/`), la 1920×1080×16 regardée le 01/11 est juste ; les autres
  restent à regarder.

- [ ] **[Outils] Endurance sur la configuration par défaut** — banc `tools/endurance/`,
  `docs/endurance.md`. Fait : PC 683 démarrages sans incident (§10), M4 342 démarrages et 40
  cycles de jeu sans incident (§11), QE PC 48 démarrages et 16 cycles (§14), zone froide
  (§15). **Reste pour fermer :** (1) voir le gardien `POMPPCSshGarde` agir sur un vrai « pas
  de ssh » (série longue ou hôte chargé, §8.6) ; (2) la panique #76 (`kalloc.16` corrompue,
  §8.4) : `pmemsave` à la collecte, reproduction par stress de la console, A/B
  `-accel tcg,thread=single` ; (3) une tranche après chaque allumage (PACKBITS compris).

- [ ] **[Système] Plantages en jeu sans cause** — paniques par délai de verrou
  (`LockTimeOut` 250 ms, `_panic+0x254`), `exit 139`/`138` de DOOM 3 et cmr au chargement.
  Nouveaux cas du 01/11 : panique cmr sous vérificateurs avec `tcg/0063` (`tcg-g4.md` §47,
  `bench/tcg/ab/dbat-0111-verif-cmr/v-1/panique-*`), cmr `exit 138` sous le nouveau PGO.
  **Au prochain gel :** `python3 tools/re/kpanic.py mach_kernel` AVANT tout reset.
  **Fermeture :** texte d'une vraie occurrence lu, cause établie, correction et série sans
  incident ; des lancements réussis ne suffisent pas.

- [ ] **[OS 9] Mesurer les gains CPU et élargir les essais** — profils expérimentaux livrés
  le 27/10 (`docs/os9-optimisations-cpu.md`), défaut `g4` conservé. Reste : A/B de vitesse sur
  une charge reproductible, autres jeux et application AltiVec.

- [ ] **[Plugin] `POMPPC_GL_PRESENTQ` rouvert (01/11, demande de l'utilisateur)** : image n mise
  en file avant d'attendre la n−1 quand l'hôte présente (`SURF_PRESENT`) ; « non tranché » depuis
  le 22/10 (neutre en VM de dev, mb −0,2 ms). Sous QE, les jeux en fenêtre présentent par le
  kext (`present_qe`) : le levier n'agit qu'en **plein écran** (`present_direct`). A/B au repos
  lancé le 01/11 à 13 h 28 (`bench/tcg/ab/presentq-0111-{cmr,mb,ut}`, 3 parties par bras, pe) :
  **raté**, toutes les parties « pas de ssh » (binaire rapide refusé, empreinte de la série
  changée par un commentaire de `qgpu_proto.h`, remis à l'identique) ; à rejouer entièrement
  au retour sur le PC par `bench/tcg/ab/presentq-0111-chaine.sh`, **après** la reconstruction
  de `build/` et `build-fast/` qu'impose `tcg/0064` (`etat-des-hotes.md` §2).
  **Fermeture :** allumé si aucune cellule pe ne perd et qu'une gagne, sinon classé « écarté sur
  chiffres » ; variante pour le chemin QE (ne pas attendre la n−1 avant la présentation par le
  kext) à instruire si le gain en plein écran est net.
- [ ] **[Métrologie] A/B de l'attente** : `QGPU_GL_FLUSH` et `POMPPC_GL_QFLUSH` allumés le 01/10
  sans l'A/B prévu. `tools/matrice/ab-attente.sh 6 qflush` sur hôte au repos ; éteindre si une
  cellule régresse.
- [ ] **[Métrologie] Troisième facteur de lenteur** (`docs/tcg-g4.md` §14.7) : une partie DOOM 3
  sur huit lente de bout en bout ; la matrice relève la charge avant et après la fenêtre.
- [ ] **[TCG] Les « sans traduction » de `x-ret-verify` en fin de session** (`tcg-g4.md` §44) :
  peut-être parents de la rafale « sans PTE » de `tcg/0061` (§45, non reproduite).

### Jeux

- [ ] **[Jeux] DOOM 3 et Prey en plein écran** : changement de mode par le jeu (`r_mode 3`,
  `CGDisplaySwitchToMode`, `SURF_PRESENT` sur l'écran redimensionné) et mesure en combat.
- [ ] **[Jeux — session] Marble Blast en fenêtre, bureau 800×600×16** : en session la fenêtre
  échoue sur un bureau trop petit. **Fermeture :** cellules vertes après démarrage frais,
  réglages utilisateur préservés.
- [ ] **[Jeux] Warcraft III** : texte des menus (chemin tableaux, un sommet sans couleur reste
  blanc) ; menu principal juste au 26/09, à revoir en partie.
- [x] **[Jeux] UT2004 : armes noires dans la démo de test : clos sur les deux hôtes** (PC le
  07/10 ; M4 le 26/10, `5f25498`, défaut propre au GL d'Apple : éclairage à spot calculé par le
  device ; démo revidée sur le M4, images ~166-195 justes ; `docs/re/ut2004-arme-noire.md`).
  Entrée fermée le 01/11 à la relecture.
- [ ] **[Outils] A3, suites** : Zenerchi en plein écran et Warcraft III en fenêtre.
- [ ] **[Outils] Rejeu : `SURF_READBACK` d'une surface jamais liée** — corrigé le 07/10 sur le
  PC ; à confirmer sur le tour `20260926-2156` du M4.
- [ ] **[Plugin] Mode immédiat sous programme de sommets** : revu sur DOOM 3 (PC, 07/10) ;
  reste Colin McRae au M4 (`POMPPC_GL_VPIMM=0` pour l'A/B).
- [ ] **[Backend GL] `gltest tex14` « λ=2 sans biais »** : défaut du GL de l'hôte macOS.
  Décision : ne réappliquer que sur changement, ou passer le biais dans l'échantillonneur.

## À confirmer par l'utilisateur

Le travail est fait ; il ne manque que l'essai ou la décision de l'utilisateur.

- [ ] **[Frontend] Curseur unique sous ImGuiDock** (`50a9801`, 01/11) : à voir à l'œil au prochain
  `run_tiger.sh` en fenêtre (un seul curseur sur l'image, flèche de l'hôte de retour sur les
  menus et bandeaux, rien de collé après un aller-retour en plein écran).
- [ ] **[Plugin] Replis à retirer** : `POMPPC_GL_VERDICT=0`, `POMPPC_GL_WHITELIST=0`,
  `POMPPC_GL_TEXMEMO=0`, `POMPPC_GL_STSKIP=0`. **Reprise :** mesure en jeu par l'utilisateur ;
  ensuite retirer les options devenues inutiles et vérifier la matrice.
- [ ] **[Outils] Captures, dix tours consécutifs verts** : à jouer sur le banc d'endurance quand
  l'utilisateur le demande.
- [ ] **[TCG — PC] Gouverneur `performance`** (`docs/vitesse-doom3-x86.md` §5.4) : un A/B à
  faire, il demande la racine.

## Plus tard

### Vitesse

- [ ] **[TCG] `tcg/0064` x-tb-head : A/B en jeu** (fusionné le 01/11, éteint par défaut,
  `TBHEAD=3 ./run_tiger.sh` ; `docs/anatomie-ppc32.md`). Mesuré sur banc seulement (−5,0 / −3,1 %
  d'instructions, −2 % de cycles). Binaires à reconstruire d'abord (empreinte de série changée).
  **Fermeture :** A/B des cinq jeux au repos sur les deux hôtes, allumé ou archivé avec le chiffre.
- [ ] **[Plugin] Suites A4** : GLEngine déroule les tableaux clients en Begin/End (Nexuiz ARB
  14-16 %, Warcraft III 4,5 %) — RE du canal `gldCreateVertexArray`
  (`docs/protocole-v23-geometrie.md`) ; clés chaudes par une table « objet GLEngine → qtex »
  côté device ; `geom_send_all`.
- [ ] **[Protocole] Nexuiz : les ~2 ms restantes d'attente des requêtes d'occlusion** :
  soumettre par tranches pendant les dessins, ou résultats écrits par l'hôte.
- [ ] **[Plugin] Transmission paresseuse** : mesure honnête et décision du défaut.
- [ ] **[Plugin] Verdict GLSL : mémoire par texture et par époque** (suite de `WLUNIF`).
- [ ] **[Plugin] Indices en VBO** (`docs/vitesse-doom3-x86.md` §10, point 4, et §5.6) :
  `r_useIndexBuffers 1` retombe sur l'empaquetage ; comprendre le refus de `raw_ensure` pour le
  tampon d'éléments.
- [ ] **[TCG] Flottant natif, angles non vérifiés** : exception FP levée depuis le chemin
  natif, formes Rc=1, cible 32 bits en VM (`docs/tcg-g4.md` §22).
- [ ] **[TCG] `vperm` en ligne sur arm64** (TBL/TBX, pendant de `tcg/0059`) : gain plafonné
  vers 0,3 % ; seulement si un profil M4 montre `helper_VPERM_FAST` haut.
- [ ] **[TCG] `tcg/0052`, rangements** (piste par compteur de génération, `tcg-g4.md` §41.7).
- [ ] **[qgpu] Cache disque des shaders traduits** sur l'hôte (démarrage des jeux, pas la
  vitesse en jeu).
- [ ] **[Leopard, plus tard] Correctif de la fenêtre PCI d'OpenBIOS** (BAR au-delà de
  `0x90000000`), utile à la VRAM de qgpu.

### Maintenabilité du plugin

- [ ] **[Plugin] A2 — Découper `pomppc_accel.c` en modules à frontières écrites** : lecteur
  d'état, textures, géométrie, programmes, transport, diagnostic (`docs/architecture.md` §10).
  Épreuve : mêmes scènes `gltest` à l'octet, mêmes `frames.csv`.
- [ ] **[Plugin] A5 (plugin) — Configuration lue une fois** (plus de `getenv` dans le chemin
  chaud) et purge des drapeaux `POMPPC_GL_*` dont le repli est mort.
- [ ] **[Plugin] Gardes de faute ramenées à la cause** : chaque `faute de lecture` et niveau
  envoyé noir devient un compteur observé à zéro sur la matrice.

### Distribution 1.0 et hôtes

- [ ] **[Distribution] 1.0 = installation reproductible** : CD ou paquet, `install.sh` qui
  reconstruit kext et plugin, disque quotidien recréable depuis l'ISO, matrice verte à chaque
  commit. **Pas de publication x86 avant la vitesse** (règle du 20/10).
- [ ] **[Distribution] Firmware reproductible** : `openbios-smp-screamer.elf` est livré en binaire.
- [ ] **[Hôtes] Un seul disque, deux hôtes** : `tiger.raw` partagé par USB entre le Mac et le PC.
- [ ] **[TCG] `tlbie` en SMP stock** : défaut de QEMU 9.2, **toujours présent dans 11.1.2**
  (vérifié le 01/11 : `ppc_tlb_invalidate_one`, cas `POWERPC_MMU_32B`, ne pose que
  `TLB_NEED_LOCAL_FLUSH`), corrigé chez nous par `x-sr-tlb` ; à signaler en amont. Aussi : `helper_store_[id]batu` vide l'ancien BAT avec le masque de la nouvelle
  valeur (`tcg-g4.md` §45, `tcg/0062` le relève).

### Divers

- [ ] **[Système] Quartz Extreme** : allumé sur les deux hôtes (`docs/quartz-extreme.md`).
  Restes : avis borné (chantier plugin), `install.sh QE=1` et personnalités, Core Image.
- [ ] **[Frontend] Frontend F1, restes non joués** : la vraie touche Ctrl+Cmd+F, écran Retina,
  plusieurs moniteurs.
- [ ] **[Son] Arrêt de la boucle principale de 186 ms** en fin de cinématique de DOOM 3
  (`docs/audio-stabilite.md`).
- [ ] **[Métrologie] Métrologie boot** (`docs/metrologie-boot.md`) : baseline de 23,32 s à
  refaire ; A/B du coût du Screamer jamais lancé.
- [ ] **[Système] « Panique cpu 1 » du 24/09** : jamais reproduite depuis le correctif OpenPIC.

## Écarté sur chiffres

Ne pas rouvrir sans fait nouveau ; la mesure est dans la doc citée.

| piste | mesure | doc |
|---|---|---|
| Traducteur de second niveau par traces (`x-tier2`, `0048`, `0055`) | DOOM 3 +2,7 % (PC), +5,7 % (M4) | `plan-traducteur-rapide.md` §9 |
| Régions chaudes recompilées par LLVM | +17,6 % sur DOOM 3 | idem, en-tête (§10 sous `archive/tier3-llvm`) ; arrêté le 22/10 |
| Mémoire directe (« fastmem ») | net ≤ 5 % pour un chantier entier | `vitesse-doom3-x86.md` §21 |
| Vérification TLB compacte | extrapolé de la zone froide : 0-4 % | §21 |
| « Opérande flottant connu partout » | plafond 3,5 %, même sans aucun test | §18.1 |
| TLB logiciel en pages énormes | les deux bras exécutaient le même code | §18.3 |
| `tcg/0053` AltiVec flottant connu | neutre en jeu | `tcg-g4.md` §41, `archive/0053-vfp-known` |
| `tcg/0057` QoS des vCPU (M4) | neutre au repos | §42 |
| `tcg/0059` `vperm` en PSHUFB | exact, neutre | §43 |
| `tcg/0060` x8 tel quel | cmr +4,2 % en course | §46 (voir « Maintenant » pour x8 + 0063) |
| `tcg/0061` `x-bat-skip` | neutre partout en course, rafale non expliquée | §46 |
| `tcg/0063` `x-dbat-lite` tel quel | cmr neutre, ut +3,7 % | §47 (témoin sous vérificateurs et x8 + 0063 : voir « Maintenant ») |
| Appel MMIO synchrone « plugin vers l'hôte » | natif ×0,6-1,5 sur le travail réel | `plan-plugin-hote.md` (code sous `archive/plugin-hote-phase0`) |
| 3-4 vCPU | 0 gain sur les jeux | Bloqué |

## Bloqué

- [ ] **[Jeux] RTCW** quitte après 8 s sans rien dessiner ; absent du disque quotidien.
      **Reprise :** jeu réinstallé sur le disque quotidien, puis diagnostic du lancement.
- [ ] **[TCG] 3-4 vCPU** : Tiger démarre sur 3 et 4 avec
      `patches/smp-mac99/essais/qemu-mac99-4cpus.patch` ; 0 gain sur les jeux.
      **Reprise :** demande explicite d'un mode 3–4 vCPU.
- [ ] **[TCG] Traducteur de second niveau** (`docs/plan-traducteur-rapide.md`, mis de côté par
      l'utilisateur le 25/09, relancé le 21/10, arrêté sur chiffres le 22/10 : « Écarté sur
      chiffres »). **Reprise :** décision explicite de rouvrir ce chantier.

## Référence validée

L'état exact de chaque hôte (binaires, défauts allumés, plugin installé, VM) est tenu dans
`docs/etat-des-hotes.md`, section 2 (PC) et 3 (M4). Repères au 01/11 :

| Élément | État |
|---|---|
| Protocole / ABI | GL **v22**, transport kext **v19** ; capacités ajoutées sans changer de version (`TEX_PRESENT`, `TEX_READBACK_ROWS`, …) |
| QEMU | 11.1.2 + série `patches/` ; PC : `~/src/qemu/build-fast` (PGO du 31/10, -O3, -march=native ; le profil avec cmr du 01/11 attend son recontrôle dans `build-fast-pgo0111cmr/`) et `build/` (référence) ; M4 : `~/src/qemu/build` (reconstruit le 01/11 sur `a0e028e` : `qgpu-core.c` du 31/10, `tcg/0059`, `tcg/0064`) |
| Défauts TCG | PC : série jusqu'à `0056` (zone froide) ; M4 : série du PC jusqu'à `0052` (avec `0037`-`0039`) ; `0057`-`0059` et `0064` compilés, éteints sur les deux hôtes (PC : `0064` à reconstruire) ; `0060`-`0063` éteints, non nommés |
| Invité | Tiger 10.4.11 ; PC : kext du 01/11 (méthode 6, surface non simple) et plugin `20261101-ombre` (`disks/prebuilt`) ; M4 : les mêmes depuis le 01/11 à 16 h 57 (VM quotidienne, compilés dans l'invité, redémarrée ; anciens dans `/var/tmp/avant-0111/` de l'invité) |
| Matrice | PC : `references-linux.csv`, 8 références toutes validées à l'œil (d3-fen et d3-pe le 30/10, `a89515b` ; cmr-pe le 01/11), 5 cellules dans les A/B (mb, zen, ut, d3 en fenêtre, cmr plein écran **en course** depuis `6054367`) ; M4 : `references.csv` (jamais touché par le PC), 17/17 vertes le 28/10 ; le 01/11, mb et d3 fen/pe vertes avec le nouveau bundle, cmr-pe reprise (faux reflet, `docs/re/cmr-rendu-vers-texture.md` §7), nouvelle référence à valider à l'œil |
| Tests natifs | PC 245 OK / 0 échec (14/10, `docs/binaire-rapide-x86.md`), M4 287 OK / 0 échec (27/10) |

## Matrice de jeux

Une cellule verte exige une image juste (rejeu = VM et référence validée), zéro repli
hors rafraîchissements admis (2 par 90 images en fenêtre), et une mesure à scène fixe
sous le seuil du jeu. Une scène `gltest` comparée au rendu d'Apple éprouve chaque notion
nouvelle. Les modes fenêtre et plein écran sont requis quand le jeu les propose.

**M4, dernier tour complet : `bench/matrice/20261028-1854`** (28/10, QE, série du PC) : **17
vertes sur 17 automatisées**. **PC, dernier tour complet : `bench/matrice/20261007-0709`** (07/10) ;
depuis, les cellules sont rejouées par les A/B (`bench/tcg/ab/*`), un tour complet avec le cœur
corrigé du 31/10 reste à faire.

| Jeu / chemin | Fenêtre | Plein écran | M4 01/10 (ms/image fen. / pe) | Travail restant |
|---|---|---|---|---|
| Marble Blast Gold | vert | vert | 9,0 / 9,6 | fenêtre sur bureau 800×600 en session |
| Zenerchi | vert | non automatisé | 4,0 / — | automatiser plein écran |
| DOOM 3 | vert | vert | 56,1 / 56,2 | changements de mode, combat |
| Prey | vert | vert | 50,5 / 49,5 | changements de mode, combat |
| UT2004 | vert | vert | 25,0 / 25,4 | — |
| Warcraft III | non automatisé | vert | — / 15,4 | fenêtre, texte des menus |
| Colin McRae | non applicable | vert | — / 48,1 | 1920×1080 (ralentissements signalés, « Ensuite ») ; mesuré **en course** sur les deux hôtes |
| Nexuiz ARB (`+r_glsl 0`) | vert | vert | 72,4 / 72,8 | tableaux clients déroulés (suites A4) |
| Nexuiz GLSL (`+r_glsl 1`) | vert | vert | 33,4 / 33,5 | attente des requêtes d'occlusion |
| RTCW | non automatisé | non automatisé | — / — | réinstallation puis diagnostic |

Ces scènes ne prouvent pas le jeu entier. Sources et critères : `docs/matrice-jeux.md`.

## Règles de travail

Avant et après un lot touchant le plugin, le device ou le cœur : `tools/matrice/matrice.py`
(ou `-j … -m …`), résultats dans `bench/matrice/dernier/tableau.md`.
À la reprise : `pgrep -x qemu-system-ppc`, `tools/guest/tssh.sh uptime`. Mesurer au premier
plan ; DOOM 3 se mesure en parties réelles (`timedemo` refusé par la démo).

- **Vitesse générique** : un levier se juge sur toute la matrice (mb, zen, ut, d3, cmr), jamais
  sur DOOM 3 seul ; il n'est allumé que s'il ne perd sur aucun jeu.
- **A/B au repos** : aucune construction, aucun autre QEMU, aucun agent parallèle pendant une
  mesure ; `MATAB_AVANT="python3 tools/tcg/chargehote.py attendre JOURNAL 1800"`. Trois parties
  par bras ne prouvent pas un écart sous 3 % (bruit mesuré, `vitesse-doom3-x86.md` §18.3).
- **Parité** : une accélération x86 a son pendant arm64 au même niveau de preuve avant
  d'être allumée là-bas, et inversement. Le plugin de l'invité est commun : rien à porter.
- **Patch TCG** : une propriété de CPU ou d'accélérateur, éteinte par défaut, une preuve
  d'équivalence, un A/B entrelacé ; **jamais nommé dans `build_qemu_qfb.sh` pendant une
  campagne** (l'empreinte de la série change, `build-fast` est refusé). Les essais écartés
  restent sous `archive/*`.
- **Pas de repli : étendre le protocole.** Sous programme ARB, tout repli vers le rendu d'Apple
  finit dans `gleBuildInterpolateFunc` → `exit(1)`.
- **`qgpu_proto.h` ⇒ QEMU + plugin** ; **`qgpu_abi.h` ⇒ QEMU + kext (redémarrage) + plugin**,
  et c'est rare.
- **Mesurer avant d'optimiser** : `perf` + `-perfmap` + `tools/re/perfpart.py` sur le PC,
  `sample` dans l'invité et sur le M4 ; `frames.csv` à scène égale. Le self % localise, il ne
  valide pas.
- **Un lot = un changement + une épreuve + un commit**, une ligne ici tant qu'il est ouvert, une
  entrée dans `CHANGELOG.md` quand il est fini.
- **Un seul agent sur la VM à la fois** ; les autres travaillent en copie isolée. L'implémentation
  non triviale passe par des sous-agents Opus ; la session principale orchestre, mesure et
  committe (demande de l'utilisateur, 01/11).
- **Après un `killall` de DOOM 3, redémarrer l'invité.** Ne jamais supprimer `.run/tiger.lock`.
  Jamais `pgrep -f`/`pkill -f` avec un motif présent dans sa propre ligne de commande.

---

## Où lire la suite

| Sujet | Fichier |
|---|---|
| Ce qui est fini (lots, versions du protocole, mesures) | `CHANGELOG.md` |
| Vitesse sur le PC x86-64 (profils, leviers, chiffres) | `docs/vitesse-doom3-x86.md`, `docs/binaire-rapide-x86.md` (PGO, empreinte de la série) |
| Parité et vitesse sur le M4 | `docs/parite-m4-2510.md`, `docs/parite-arm64-0031-0035.md`, `docs/jit-m4-2026-10-05.md`, `docs/etat-des-hotes.md` §3 |
| Allègement du plugin de l'invité | `docs/plugin-allegement.md`, `docs/d3-plugin-x86.md` |
| Processeur émulé : relevés, patches, A/B | `docs/tcg-g4.md`, `docs/flottant-rapide.md`, `docs/ppc32-mttcg.md`, `docs/anatomie-ppc32.md`, `docs/smp-coeurs.md` |
| Matrice de jeux (A3) : lancer, preuves, références, pièges | `docs/matrice-jeux.md` |
| Banc d'endurance, gels et épreuves de fiabilité | `docs/endurance.md`, `docs/gel-doom3-baddisplay.md`, `docs/epreuve-gpu-bloque.md`, `docs/metrologie-boot.md` |
| Quartz Extreme | `docs/quartz-extreme.md` |
| Architecture, rétro-ingénierie, offsets, boucle de dev | `docs/architecture.md`, `docs/gpu-3d-tiger.md`, `docs/re/README.md` |
| Protocole : contrat courant v22 ; historique v7 → v21 (pas de document propre à la v22, ATI combine3) ; les volets A4 `protocole-v23-*` sont entrés en capacités, sans v23 | `docs/protocole.md`, `docs/protocole-v*.md` |
| Backend GL et attente de l'invité ; textures | `docs/backend-gl-attente.md`, `docs/backend-gl-unites-fixes.md`, `docs/pagination-textures.md` |
| Son ; réseau ; tablette ; OS 9 | `docs/audio-stabilite.md` ; `docs/internet-tiger.md` ; `docs/tablette-tiger-10.4.11.md` ; `docs/os9-optimisations-cpu.md` |
| Traducteur de second niveau (arrêté sur chiffres le 22/10) ; plugin vers l'hôte (arrêté le 23/10) | `docs/plan-traducteur-rapide.md`, `docs/plan-plugin-hote.md` |
| Bug hunts (22/09, 29/09 passes 1-4) | `docs/bug-hunt-2026-09-*.md` |
| Feuille de route de fond (OpenGL 1.5, QE, Core Image) | `docs/roadmap-opengl15.md` |
| Références externes (kext Tiger, IOGraphics, virtio-gpu) ; étude d'origine du GPU | `docs/references-ingenierie.md`, `docs/gpu-tiger-4060ti.md` |
