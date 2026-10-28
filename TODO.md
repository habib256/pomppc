# TODO — priorités de POMPPC

> **Deux hôtes (PC x86-64 et Mac M4) en alternance** : l'état de chacun et ce qui y reste à faire
> sont dans [docs/etat-des-hotes.md](docs/etat-des-hotes.md), à lire en premier.

État au 01/10/2026. Objectif : Mac OS X Tiger sous QEMU, bureau et jeux OpenGL
sur le GPU de l'hôte, avec une matrice de jeux verte comme preuve.

**Où on en est.** L'orientation « vitesse » (29/09) a livré ses lots : cache de sauts,
`mtmsr`/`rfi` sans verrou, `x-icbi-sync`, A4 (état et géométrie par l'hôte), flottant
scalaire natif, attente des requêtes d'occlusion — tout est **allumé par défaut** et la
matrice `20261001-tout` est verte (15/15, images justes ; DOOM 3 61 → 57, Colin McRae
71 → 48, Nexuiz ARB 110 → 80 ms/image depuis le 30/09). **Orientation suivante : revenir à
la fiabilité** — des plantages en jeu restent sans cause (paniques `LockTimeOut`, DOOM 3 plein
écran `exit 139`), et beaucoup de leviers viennent d'être allumés d'un coup : le banc
d'endurance doit tourner sur la configuration par défaut avant de reprendre la vitesse.
Ensuite : jeux, maintenabilité du plugin, puis 1.0.

Les chantiers sont rangés par priorité ; leur domaine figure entre crochets.
**Maintenant** contient au plus trois chantiers, **Ensuite** donne l'ordre de prise,
**À confirmer par l'utilisateur** réunit ce qui n'attend qu'un essai ou une décision de sa part,
**Plus tard** garde les pistes différées, **Bloqué** indique ce qui manque pour reprendre.
Une entrée décrit le travail restant et la preuve qui la fermera. Pour les défauts de
session : reproduction avant correction et contrôle après correction ; un incident qui
ne se reproduit plus n'est pas une correction. Pour les défauts de rendu : scène
reproduisant le défaut, image corrigée et matrice verte. Pour les optimisations :
preuve d'équivalence et mesure A/B à scène égale. Les travaux clos sont dans
[CHANGELOG.md](CHANGELOG.md), les études détaillées dans `docs/`.

L'ancien tableau par domaine est conservé dans
[docs/archive/todo-par-domaines-2026-09-27.md](docs/archive/todo-par-domaines-2026-09-27.md) :
les anciens renvois « TODO §0…§10 » se lisent dans cette archive.

## Maintenant

- [ ] **[Outils] Endurance sur la configuration par défaut** — banc livré le 29/09
  (`tools/endurance/`, `docs/endurance.md`), porté sur Linux et joué une nuit sur le PC le 07/10
  (`docs/endurance.md` §7 : défauts du 06/10 + binaire rapide, **0 panique sur 305 démarrages,
  0 incident sur 62 parties** (15 par jeu, borne ~20 % par jeu), 5 « pas de ssh » après
  `shutdown -r` ; témoin anciens défauts 1 panique « 0x400 Inst access » dans l'appariement
  ATAPI sur 238). **Reste pour fermer :** (1) une nuit sur le **M4** avec les nouveaux défauts
  (build de référence du 26/10 ; première tranche terminée le 27/10 : **78 démarrages sains**,
  dont 24 froids et 54 reboots, **16 cycles de jeux sains**, quatre par jeu ; Tiger 10.4.11
  isolé, gardien SSH absent, `docs/endurance.md` §9 ; reste une série plus longue) ;
  (2) sur le PC, les « pas de ssh » : **cause trouvée le 07/10**
  par la console série du banc (`--serie`, `docs/endurance.md` §8) — course dans l'invité :
  l'IPv6 de lo0 ne s'attache pas (`in6_ifattach_loopback … errno=17/55`), `launchctl` échoue au
  `bind()` IPv6 de `ssh.plist` et abandonne aussi l'IPv4 ; rien de QEMU. **Correction faite le 13/10**
  (`docs/endurance.md` §8.5) : gardien `POMPPCSshGarde` du paquet invité (recharge `ssh.plist`
  une fois une IPv6 présente) ; défaut reproduit à la demande dans la VM et réparé par le
  gardien. Série `pc-reboot-garde` (14/10, §8.6) : 450 démarrages, 0 « pas de ssh », mais la course
  ne s'est pas produite (gardien jamais sollicité) : **reste** à voir le gardien agir sur un vrai
  cas (série plus longue, ou hôte chargé comme le 07/10) ; la panique `CheckSubTree` de #160
  revient sur la configuration par défaut (#98, 1 sur 450) ; (2 bis) la panique #76 : relue le 13/10 (§8.4) — tombée **pendant un
  relevé par la console**, liste libre de kalloc.16 corrompue (probable), CPU 1 en 0x700 ;
  suite : savearea (décodage restant) ; **`pmemsave` ajouté à la collecte le 20/10**
  (`docs/endurance.md` §8.7, transfert éprouvé sur QEMU sans invité), reproduction par stress de la console, puis A/B
  `-accel tcg,thread=single` ; (3) ~~une tranche de démarrages à froid sur le PC~~ **faite la nuit
  du 27-28/10** (`docs/endurance.md` §10 : binaire rapide du 27/10, 400 froids + 283 reboots,
  **0 incident sur 683**, borne 0,6 %).

- [ ] **[Système] Plantages en jeu sans cause** — trois défauts, peut-être un seul mécanisme ;
  le banc ci-dessus doit les faire sortir (nuit du 07/10 sur le PC : aucun `LockTimeOut`, aucun
  `exit 139` en 15 parties de DOOM 3 et 543 démarrages).
  - **Paniques par délai de verrou (`LockTimeOut` = 250 ms).** Le « gel au chargement de
    DOOM 3 » (26/09, 1 lancement sur ~12) est une panique : `0x268b4` = `_panic+0x254`, vCPU 1
    au repos ; sans kdp, Tiger n'en garde ni écran ni `panic.log`. Mécanisme probable : un arrêt
    de vCPU côté QEMU (BQL, doorbell synchrone, hôte chargé) compté par `mftb` dans un délai de
    verrou tournant. Arrêts mesurés : 95 ms au plus (`stallmeter`). Non reproduit le 29/09
    (0 sur 39 chargements). `docs/gel-doom3-baddisplay.md` §2.
  - **Panique pendant le diagnostic UT2004 (27/09, 15:50)** : CPU 1, `Lock timeout` dans
    `_fpu_switch`, PC `0x000AA010`, LR `0x00003820`, pile désalignée. Ne pas l'attribuer sans
    preuve à AppleUSBOHCI (corrigé le 29/09). `bench/utweapon-priorites/panic.png`,
    `docs/matrice-jeux.md`.
  - **DOOM 3 plein écran mort au chargement** (`exit 139`, tas du jeu corrompu) : 3 fois de
    suite dans une même session de la VM pendant le volet état d'A4, dont une fois leviers
    éteints ; 0 sur les ~20 parties suivantes. `bench/a4/etat/rouges`.
  **Au prochain gel :** `python3 tools/re/kpanic.py mach_kernel` AVANT tout reset (texte,
  appelant, pile). **Fermeture :** texte d'une vraie occurrence lu, cause établie, correction
  (relever `LockTimeOut` par le kext, ou borner les arrêts de vCPU) et série sans incident ;
  des lancements réussis ne suffisent pas.

- [ ] **[Jeux] Performances selon la résolution** — les jeux fonctionnent très bien dans
  l'ensemble, mais pas dans toutes les résolutions : gros ralentissements signalés par
  l'utilisateur dans **Colin McRae en 1980 × 1024** (27/10). Étendre les essais aux autres
  résolutions, jeux et modes fenêtre/plein écran ; ne pas limiter le diagnostic à Colin
  McRae. Relever les temps d'image et les éventuels replis de rendu sur une même scène pour
  comprendre les chutes de vitesse. Les cellules déjà vertes aux résolutions de la matrice
  ne valent pas validation de toutes les résolutions.

- [ ] **[Validation] Restes des épreuves en VM des bug hunts** (tout le reste est tenu le
  07/10 sur le PC : `docs/bug-hunt-2026-09-29-passe4.md`, dernière section).
  (1) ~~**R5 sous refus réel**~~ **tenu le 14/10** : pagination des textures dans le plugin
  (`20261014-r5`, `docs/pagination-textures.md`) — plus aucun dessin rendu à Apple pour la
  mémoire ; à 128 Mio DOOM 3 essuie 833 refus, apprend un budget de 78,5 Mio et joue jusqu'au
  bout de la cellule, image juste (rejeu/VM 0,00 %) ; témoin `POMPPC_GL_TEXPAGE=0` : `exit` de
  `gleBuildInterpolateFunc` à l'image 288 ; à 512 Mio rien ne bouge (0 `NOMEM`, image juste).
  Ligne de preuve : `docs/bug-hunt-2026-09-29-passe4.md`, dernière section. Mis au jour en
  route (à traiter à part) : QEMU mourait par `double free` (Mesa, `DeleteProgramsARB` sous
  `qgpu_core_client_reset`) à la fin de toute cellule DOOM 3 avec vidage, ancien plugin
  compris — sur llvmpipe seulement : 0 sur 2 sur la RTX le 14/10 (`docs/pagination-textures.md` §4) ; le backend GL tournait sur llvmpipe (corrigé le 14/10 : `EGL_PLATFORM=device`, `run_tiger.sh`) et QEMU ne se reconstruit plus (`glib-2.0`
  absent) depuis Ubuntu 26.04 (`docs/pagination-textures.md` §4). (2) **GPU hôte bloqué : tenu le 14/10 sur le PC**
  (`docs/epreuve-gpu-bloque.md`, propriétés `x-test-stall-ms` et `x-test-refuse-reset` du
  device, preuves dans `bench/stall-20261014/`) :
  - **GL3** : GPU bloqué, job en vol, kext rechargé → « reset non terminé après 5000 ms »,
    le kext lit CAPS = 0 et refuse de démarrer, moniteur servi (pire 5,01 s) ; débloqué,
    `kextload` répare (`QGPUClients = 4`, `qgpu_test` vert) ; même chose par `system_reset`
    (rendu en 7,0 s, CAPS = CLIENTS = 0 après le redémarrage jusqu'au reset suivant).
  - **KT4** : mode de l'écran POMPPCQFB changé pendant la panne en 0,001 s, aucune réponse du
    moniteur au-delà de 0,25 s, CAPS et CLIENTS relus à 0.
  - **K6** : lien 1024x768 au pas 128 avant la panne, aucun lien pendant, « écran relié de
    nouveau (qfb-pci, 640x480, pas 1280) » à la réparation ; et, après un correctif trouvé par
    l'épreuve (géométrie qfb perdue quand le drainage échoue sur un device encore sain,
    `system_reset` avec job bloqué), relié aussi à 1024x768 au pas 128 sans autre changement.
  - **L2** : voir A6 (`CHANGELOG.md`) — avec le kext d'avant, 4 `QGPU_ST_LIMIT` sur 5
    scénarios ; avec celui du dépôt, 0 sur 5, autre tranche ou attente, journal du kext qui
    nomme la tranche sale puis sa libération. KT5 en prime : `quit` bloqué sort en 5,06 s.
  **Reste du point (2) :** reconstruire `build/` et `build-fast/` (PC) puis le M4 avec ce
  device, et installer le kext A6 dans les VM (paquet invité ; ABI inchangée, plugin intact).

## Ensuite

- [ ] **[OS 9] Mesurer les gains CPU et élargir les essais** — profils expérimentaux livrés
  le 27/10, défaut `g4` conservé. Sept passages DOOM II avec image fixe exacte et PCM présent,
  plus Barrack en partie sur témoin et complet ; vérificateurs sans divergence
  (`docs/os9-optimisations-cpu.md`). Reste : A/B de vitesse sur une charge reproductible,
  autres jeux et application AltiVec (très peu exercé par cette première tranche).

Dans l'ordre. Chaque entrée passe par le banc d'endurance quand elle s'y prête.

- [x] **[TCG — M4] Séance de parité du 25-26/10** (`docs/parite-m4-2510.md` §5) : 0049-0050, série
  0031-0039, 0045-0047, 0051, 0052, 0040-0043 prouvés sur le M4 au niveau du PC (disque de dev
  10.4.11), **tous allumés sur `Darwin:arm64`** après A/B, hôte au repos : DOOM 3 −23,9 %
  (42,3 → 32,2 ms/image), UT2004 −18,6 %, Marble Blast −22,7 %. **0037 corrigé** en route (diviseur
  nul sous `PFP_K_B`) ; 0037-0039 posés par `build_qemu_qfb.sh` après 0052. Talon de `vcf*`
  couvert le 26/10 par `tcg/0054` (`x-vfp-native-cmp-slow`, les deux hôtes ; mutants 21/21, §5.9).
  Reste : le mutant n° 14 de 0052, équivalent sur arm64 (§5.5), rien à faire.
- [x] **[TCG — PC] 0052 TLB par base, 0053 AltiVec flottant connu** (25/10) : 0052 fusionné et
  allumé (DOOM 3 −2,2 %) ; 0053 juste mais neutre en jeu, non fusionné (`archive/0053-vfp-known`).
  Rangements de 0052 (piste par compteur de génération, §41.7) : ouvert.

- [x] **[TCG — M4] Parité arm64 de 0031-0035** : faite le 25/10 (`docs/parite-m4-2510.md` §5.3) ;
  `m4-serie-0031-0035-proofs.sh` `host`, `guest`, `mut` sur une copie de l'arbre (`prep` est caduc :
  `build_qemu_qfb.sh` pose désormais 0037-0039 lui-même).

- [x] **[TCG — M4] Finir la matrice du 06/10** : 17/17 cellules automatisées couvertes
  sous les nouveaux défauts (`docs/parite-m4-2510.md` §5.10–5.11 : tour à 16/17 puis
  rejeu Warcraft III vert, images justes). Le dernier rouge venait du montage du CD,
  corrigé avec `-noautofsck` et un contrôle explicite du volume.

- [x] **[TCG — M4] Traces `0048`, pendant arm64** : séance du 26/10
  (`docs/plan-traducteur-rapide.md` §9.8). Condition de lancement trouvée : `split-wx=on`
  pour les compteurs dans les pages JIT ; choix automatique par le lanceur quand
  `x-tier2` est demandé. Banc nu et mutants portés. `0055` corrige le rejeu de TB dont
  la longueur change sous journalisation : compte exact des instructions, référence
  rejouée une instruction par TB ; comparaison des stores à leur largeur réelle.
  Banc nu et mutants verts, OpenBIOS SMP 438,1 M traces vérifiées sans divergence,
  Tiger + Marble Blast 32,6 M sans divergence, image exacte. DOOM 3 vérifié à son tour
  (§9.9 : 293,5 M contrôles, zéro divergence, image synchronisée exacte).
  A/B entrelacé 3×3 DOOM 3 (§9.9), hôte au repos : 32,086 ms/image au défaut,
  32,184 split-wx seul, 33,929 avec traces (**+5,74 %**, les trois parties avec traces
  au-dessus de tous les témoins). **Éteint par défaut**, comme sur le PC ; mode mémoire
  par défaut conservé, VM rendue dans ImGuiDock.

- [x] **[TCG — PC] Preuves de `0055`** (27/10, `docs/plan-traducteur-rapide.md` §9.10) :
  `t2proof.sh` conforme avec et sans les leviers FP natifs (0 divergence, mutants vus) ;
  Tiger + Marble Blast vérifiés, 26,5 M traces, 0 divergence, image juste. `x-tier2` éteint.

- [ ] **[Métrologie] A/B de l'attente** : `QGPU_GL_FLUSH` et `POMPPC_GL_QFLUSH` ont été allumés
  le 01/10 à la demande de l'utilisateur sans l'A/B prévu (un seul tour : Nexuiz GLSL 36,1 → 35,3,
  ARB 81,2 → 80,2). `tools/matrice/ab-attente.sh 6 qflush` sur hôte au repos ; éteindre si
  une cellule régresse.

- [ ] **[TCG] Marble Blast sous `x-msr-nobql`** : A/B non concluant (cellule trop bruitée), à
  refaire par `tools/tcg/mbab.sh`.

- [ ] **[Métrologie] Troisième facteur de lenteur** (`docs/tcg-g4.md` §14.7) : une partie DOOM 3
  sur huit lente de bout en bout ; cause inconnue (cœurs P/E, autre processus, thermique ?).
  La matrice relève désormais la charge avant et après la fenêtre de mesure.

### Jeux

- [ ] **[Jeux] DOOM 3 et Prey en plein écran** : changement de mode par le jeu (`r_mode 3`,
  `CGDisplaySwitchToMode`, `SURF_PRESENT` sur l'écran redimensionné) et mesure en combat.
- [ ] **[Jeux — session] Marble Blast en fenêtre, bureau 800×600×16** : la cellule de la matrice
  est verte, mais en session la fenêtre échoue sur un bureau trop petit (surface rognée, replis,
  pas de présentation capturable). Identifier le choix du mode vidéo et garantir un bureau assez
  grand, puis refaire Nexuiz fenêtre/plein écran. **Fermeture :** cellules vertes après
  démarrage frais, réglages utilisateur préservés.
- [ ] **[Jeux] Warcraft III** : texte des menus (chemin tableaux, un sommet sans couleur
  reste blanc) ; menu principal juste au 26/09, à revoir en partie.
- [ ] **[Jeux] UT2004 : armes noires dans la démo de test — reste le M4** (constaté par
  l'utilisateur le 01/10 sur le M4). PC corrigé le 07/10 (backend GL sans l'extension ATI
  → `NV_texture_env_combine4`, `docs/re/ut2004-arme-noire.md`) : image juste dans la démo
  sur un QEMU reconstruit ; reconstruire `~/src/qemu` (référence) avec ce `qgpu-gl.c`. Le M4
  a l'extension ATI, le correctif n'y change rien : y revider toute la démo (`@1`, 450
  images), rejouer les images ~166-195 et lire les présentoirs avec `QGPU_REPLAY_UNUSED` /
  `QGPU_REPLAY_STATE`. Fermeture : image juste dans la démo sur le M4.
- [ ] **[Outils] A3, suites** : Zenerchi en plein écran et Warcraft III en fenêtre (réglage à
  trouver, sinon clic par System Events) ; Colin McRae fenêtre non applicable.
- [ ] **[Outils] Rejeu : `SURF_READBACK` d'une surface jamais liée** — corrigé le 07/10 sur le
  PC (`docs/matrice-jeux.md` §5 : reproduit sur le vidage DOOM 3 fenêtre du PC privé de son
  `surfaces.txt`, 2 `NO_SURF` → 0) ; à confirmer sur le tour `20260926-2156` du M4 (rejeu sans
  `NO_SURF`, image inchangée).
- [ ] **[Plugin] Mode immédiat sous programme de sommets : revue des jeux** : tous les
  attributs tenus entre `glBegin`/`glEnd` sous programme ARB (`gltest vpimm` 44/44, identique
  au rendu d'Apple ; `docs/protocole-v16-programmes.md` §7 et §7 bis : coordonnées, couleur le
  07/10 à l'aube, normale, couleur secondaire, brouillard, génériques `vertex.attrib[k]`
  l'après-midi). Le texte du programme décide désormais seul de ce qui est porté : reste à
  revoir en jeu Colin McRae (M4), `POMPPC_GL_VPIMM=0` pour l'A/B. **DOOM 3 revu sur le PC
  (07/10, paquet `20261007c`, `bench/matrice/20261007-vpimm2`)** : `d3-fen` image juste (rejeu =
  VM 0,04, référence 0,00, à valider à l'œil comme avant), 15 replis, pas de plantage ; `ut-fen`
  juste et validée. Vitesse non jugée (trois VM d'endurance sur l'hôte : 232,7 et 167,0 ms/image ;
  A/B UT2004 sans vidage, même charge : défaut 60,2 puis 101,5, `VPIMM=0 TCLLOST=0` 56,4 — la
  dispersion suit la charge). Fermeture : Colin McRae au M4, vitesse hôte au repos.
- [ ] **[Backend GL] `gltest tex14` « λ=2 sans biais »** : défaut du GL de l'hôte macOS (le
  biais d'unité du dessin précédent reste appliqué ; un `glFlush` le corrige mais coûte).
  Décision : ne réappliquer que sur changement, ou passer le biais dans l'échantillonneur.

## À confirmer par l'utilisateur

Le travail est fait ; il ne manque que l'essai ou la décision de l'utilisateur.

- [ ] **[Plugin] Replis à retirer** : `POMPPC_GL_VERDICT=0`, `POMPPC_GL_WHITELIST=0`,
  `POMPPC_GL_TEXMEMO=0`, `POMPPC_GL_STSKIP=0`. **Reprise :** mesure en jeu par l'utilisateur ;
  ensuite retirer les options devenues inutiles et vérifier la matrice.
- [ ] **[Matrice — PC] Valider à l'œil la référence `d3-fen` du PC** (`validee=non` depuis le
  04/10 ; DOOM 3 sort rouge pour ce seul motif une fois sous son plancher). Images :
  `bench/matrice/ref-linux/d3-fen/image.png` (référence, image 5013) contre
  `bench/tcg/ab/x86-final/final-1/d3-fen/capture.png` (binaire rapide, image 5079) et
  `…/ref-1/d3-fen/capture.png` (anciens défauts, image 5074) : même scène, écart moyen 3,2 et
  3,9 dû aux ~60 images d'écart (`docs/vitesse-doom3-x86.md` §13.6). **Reprise :**
  `tools/matrice/matrice.py --valider d3-fen`.
- [ ] **[Outils] Captures, dix tours consécutifs verts** : le rendez-vous `POMPPC_GL_CAPTURE`
  remplace `dump_attente` ; premier contrôle DOOM 3/Prey fenêtre vert. Reporté le 27/09, à jouer
  sur le banc d'endurance quand l'utilisateur le demande.

## Plus tard

### Vitesse — reprise après la fiabilité

Une mesure A/B exige un hôte au repos (aucune autre VM, charge relevée avant et après).
Dans l'ordre des gains estimés. Profil du 01/10 et lecture : `docs/vitesse-profil-2026-10-01.md`
(moteur multifil d'Apple absent : `docs/re/moteur-multifil.md`).

- [ ] **[TCG] `x-fp-native64` (tcg/0016) et `x-tb-fast` (tcg/0015) allumés par défaut le 01/10**
  sans tour de matrice, à la demande de l'utilisateur (prouvés exacts, A/B Prey −1,7 %, DOOM 3
  −0,6 %, Nexuiz −0,8 % ; `docs/tcg-g4.md` §23-§24). **Reste :** l'essai de l'utilisateur, puis
  un tour de matrice complet sur le binaire de référence reconstruit.

- [ ] **[Plugin] Suites A4** : GLEngine déroule les tableaux clients en Begin/End (Nexuiz ARB
  14-16 %, Warcraft III 4,5 %) — RE du canal `gldCreateVertexArray`
  (`docs/protocole-v23-geometrie.md`) ; clés chaudes (unités) par une table « objet GLEngine →
  qtex » côté device (0,3-0,6 ms/image estimés sur DOOM 3) ; `geom_send_all` (1,5-3,9 %).
  Contribution de chaque levier A4 séparément non mesurée (la campagne les allumait ensemble).
- [ ] **[Protocole] Nexuiz : les ~2 ms restantes d'attente des requêtes d'occlusion** :
  soumettre par tranches pendant les dessins, ou résultats de requête écrits par l'hôte.
- [ ] **[Plugin] DOOM 3 sur le PC, étape 3** (`docs/d3-plugin-x86.md`) : trois leviers écrits,
  éteints, **pas encore compilés dans l'invité** — indices sans balayage quand les miroirs
  sont propres et balayage AltiVec (`POMPPC_GL_IDXLAZY`, `POMPPC_GL_IDXVEC`), verdict refait
  par unité (`POMPPC_GL_UNITVD`), dispatch en une passe (`POMPPC_GL_DISPONE`). Reste : gltest
  51 scènes et DOOM 3 sous contrôle, matrice avec vidage, A/B sur le PC (§6-§7), puis défauts.
- [ ] **[Plugin] Transmission paresseuse** : mesure honnête et décision du défaut.
- [ ] **[Plugin] Verdict GLSL : mémoire par texture et par époque** (suite de `WLUNIF`, dont le
  gain était dans le bruit : ce sont les liaisons de texture qui font recalculer).
- [ ] **[TCG] Flottant natif, angles non vérifiés** : exception FP levée depuis le chemin
  natif, formes Rc=1, cible 32 bits en VM, hôtes non arm64 (`docs/tcg-g4.md` §22).
- [ ] **[Cœur] DOOM 3 : `nat_conv_attr`** = 27 % du fil de rendu hôte (attributs reconvertis
  à chaque dessin natif) ; convertir une fois au `BUF_SUBDATA`. Utile seulement si l'hôte
  devient la limite (attente ≤ 1,3 %).

### Maintenabilité du plugin

- [ ] **[Plugin] A2 — Découper `pomppc_accel.c` en modules à frontières écrites** : lecteur
  d'état (table d'offsets `gctx+…` vérifiée par une empreinte de GLEngine), textures,
  géométrie, programmes, transport, diagnostic. Plan : `docs/architecture.md` §10. Épreuve :
  mêmes scènes `gltest` à l'octet, mêmes `frames.csv`. Motif : la plupart des bugs des
  passes 3 et 4 venaient de correctifs de la passe précédente dans ce fichier.
- [ ] **[Plugin] A5 (plugin) — Configuration lue une fois** (plus de `getenv` dans le chemin
  chaud) et purge des drapeaux `POMPPC_GL_*` dont le repli est mort.
- [ ] **[Plugin] Gardes de faute ramenées à la cause** : chaque `faute de lecture` et niveau
  envoyé noir devient un compteur observé à zéro sur la matrice.
- [ ] **[Outils] A5 (scripts) — reste** : `.run/cmr/` n'a plus que des données (sa copie
  locale de `tssh.sh` lit le port publié depuis le 29/09).

### Distribution 1.0 et hôtes

- [ ] **[Distribution] 1.0 = installation reproductible** : CD ou paquet, `install.sh` qui
  reconstruit kext et plugin, disque quotidien recréable depuis l'ISO, matrice verte à chaque commit.
- [ ] **[Distribution] Firmware reproductible** : `openbios-smp-screamer.elf` est livré en binaire.
- [ ] **[Hôtes] Un seul disque, deux hôtes** : `tiger.raw` partagé par USB entre le Mac et le PC.
- [ ] **[TCG] `tlbie` en SMP stock** : défaut de QEMU 9.2, corrigé par `x-sr-tlb` ; à signaler en amont.

### Divers

- [ ] **[Système] Quartz Extreme et Core Image** : **engagé le 29/10** dans une VM jetable
  (`docs/quartz-extreme.md`). « QE / CI : géré », composition par notre plugin, mais image
  fausse (textures 1555 REV, projection). Éteint par défaut (`install.sh QE=1`). Reste : P1
  (bureau juste), P2 (fenêtres GL sous QE), P3 (vitesse, endurance, allumage).
- [ ] **[Frontend] Frontend F1, restes non joués** : la vraie touche Ctrl+Cmd+F, écran Retina,
  plusieurs moniteurs.
- [ ] **[Son] Arrêt de la boucle principale de 186 ms** en fin de cinématique de DOOM 3
  (1 fois en 13 parties, 93 ms de silence hôte ; doorbell synchrone exclu,
  `docs/audio-stabilite.md`).
- [ ] **[Métrologie] Métrologie boot** (`docs/metrologie-boot.md`) : baseline de 23,32 s à
  refaire avec le harnais durci ; A/B du coût du Screamer jamais lancé.
- [ ] **[Système] « Panique cpu 1 » du 24/09** : jamais reproduite depuis le correctif OpenPIC
  (hypothèse : verrou tenu par le CPU 0 pris dans la tempête d'IRQ) ; à revoir si le banc la
  fait réapparaître.

## Bloqué

Chaque entrée indique la condition de reprise ; une cause encore inconnue reste
un travail de diagnostic dans « Ensuite », pas un blocage.

- [ ] **[Jeux] RTCW** quitte après 8 s (`~/rtcw.command`, LaunchCFMApp) sans rien dessiner ;
      stdout dans `~/rtcw-dump`. Absent du disque quotidien (`~/Desktop/Wolfenstein` manque).
      **Reprise :** jeu réinstallé sur le disque quotidien, puis diagnostic du lancement.

- [ ] **[TCG] 3-4 vCPU** : Tiger démarre sur 3 et 4 (`hw.ncpu 4`) avec
      `patches/smp-mac99/essais/qemu-mac99-4cpus.patch` (GPIO 15/16 de KeyLargo) ; 0 gain sur
      les jeux, ~linéaire sur du travail parallèle dans l'invité jusqu'aux 4 cœurs P de l'hôte.
      Pas de chantier sans demande (1-2 jours pour en faire un mode ; au-delà de 4 : AppleMPIC).
      **Reprise :** demande explicite d'un mode 3–4 vCPU.

- [ ] **[TCG] Traducteur de second niveau** (`docs/plan-traducteur-rapide.md`, **mis de côté par
      l'utilisateur, 25/09**) : régions chaudes recompilées par LLVM depuis les ops TCG.
      6-10 mois de travail effectif. Première étape si repris : phase 0 (2-4 jours), go si
      ≥ 70 % du temps vCPU tient dans ≤ 1 000 régions et le plafond dépasse ×1,3 sur l'image.
      **Reprise :** décision explicite de rouvrir ce chantier.

## Référence validée

État au 01/10/2026 (A4, flottant natif et attente allumés ; tout par défaut).

| Élément | État |
|---|---|
| Protocole / ABI | GL **v22**, transport kext **v19** (`QGPU_ST_NO_MEM`, `QGPU_REG_NOMEM`, `QGPU_CAP_GLSL_PATHS`, `QGPU_CAP_STATE_BLOCK`, `QGPU_CAP_GEOM_HOST` ajoutés sans changer d'ABI) |
| QEMU de référence | `~/src/qemu/build/qemu-system-ppc64`, reconstruit le 01/10 au soir (device A4, TCG 0010-0016, Screamer corrigé) ; précédents en `*.avant-tbfp`, `*.avant-a4`. **PC Linux** : `build/` et `build-fast/` (binaire rapide, que prend le lanceur) reconstruits le 07/10 à 07:00 avec le device de la nuit (erreurs par client, combineurs ATI par NV combine4, `vpimm`), série `3d7534b0f2217d54` ; précédents en `*.avant-0707` |
| Invité quotidien | `tiger.qcow2`, kext v19, plugin `20261001-tout` (gcc-4.0 dans l'invité), SMP=2 (M4) ; **PC** : Tiger 10.4.11, paquet `disks/pomppc-guest-20261007c.iso` (plugin de main 97fad0a : nuit du 06-07/10, `glPixelStore` lu sur le contexte de GLEngine, suites de `vpimm`, « T&L perdu » rendu à GLEngine ; chaîne `20261004-d3x` ; précédents dans `~/pomppc-sauvegarde-20261007`, `-20261007b`, `-20261007c`), kext `caps 0xefffe` ; `POMPPCFsqrt.kext` chargé au démarrage par `/Library/StartupItems/POMPPCFsqrt` (`kHasFsqrt` : la libm prend `fsqrt` ; `sudo sh install.sh --retirer` l'enlève) |
| TCG | `0001–0004`, `0006–0008`, `0010–0012`, `0014–0016` activés par défaut (`JCBITS=14`) ; `SRTLB=0`, `LFSINLINE=0`, `VFPFAST=0`, `VPERMFAST=0`, `JITNEAR=0`, `FPINLINE=0`, `RETINLINE=0`, `JCIDX=0`, `ICBISYNC=0`, `MSRNOBQL=0`, `FPNATIVE=0`, `FPNATIVE64=0`, `TBFAST=0` les désactivent ; `0013` (`FPFLAT=1`) éteint |
| Plugin et device | `POMPPC_GL_STATEBLK`, `POMPPC_GL_RAWSANE`, `POMPPC_GL_NATSHM`, `POMPPC_GL_WLUNIF`, `POMPPC_GL_QFLUSH`, `QGPU_GL_FLUSH`, `POMPPC_GL_VPIMM` (07/10) allumés (`=0` éteint chacun) ; `disks/prebuilt` du 07/10 09:05 (précédents : `bench/devloop/prebuilt-20261004`, `prebuilt-20261007-0709`, `prebuilt-20261007b`) |
| Tests natifs et scripts | M4 (01/10) : **190 OK, 0 échec, 4 ignorés** ; frontend ctest 3/3. PC Linux (07/10 au matin, RTX 4060 Ti, backend GL `FIXED4`, binaires reconstruits) : **240 OK, 0 échec, 5 ignorés** ; job `gpu` : **56 OK, 0 échec** (07/10 09:05, `vpimm` 44 témoins, `pixstore`, `matbegin`, `matsonde`) ; ces quatre scènes vertes dans la VM quotidienne |
| Travaux clos | Voir `CHANGELOG.md` et `docs/bug-hunt-2026-09-29*.md` |
| Profils | Plugin : `bench/plugin/ab2-B*/{d3,prey}-fen/mesure/sample.txt`, `bench/a4/depart/LISEZMOI.md` ; TCG : `bench/tcg/` (hors git) |

## Matrice de jeux

Une cellule verte exige une image juste (rejeu = VM et référence validée), zéro repli
hors rafraîchissements admis (2 par 90 images en fenêtre), et une mesure à scène fixe
sous le seuil du jeu. Une scène `gltest` comparée au rendu d'Apple éprouve chaque notion
nouvelle. Les modes fenêtre et plein écran sont requis quand le jeu les propose.

**PC Linux, dernier tour complet : `bench/matrice/20261007-0709/tableau.md`** (binaires et paquet
du 07/10 au matin, avec vidage) : Marble Blast 15,5 / 15,2, Zenerchi 7,4, UT2004 48,8 / 48,5
**vertes**, images justes ; DOOM 3 123,4 / 121,5 rouge tant que `d3-fen` et `d3-pe` (créée ce
tour) ne sont pas validées à l'œil (`matrice.py --valider`). Détail : `docs/matrice-jeux.md`
§6 quater.

**Dernier tour complet : `bench/matrice/20261001-fsqrt/<jeu>/tableau.md`** (configuration du
01/10 plus `POMPPCFsqrt` chargé ; Nexuiz dans `nx-bis`/`nxg-bis`) : **15 vertes sur 15
automatisées**, images toutes justes. Tours précédents : `20261001-tout` (15/15, sans `fsqrt`),
`20260930-wlunif` (16/16), `20260930-0128` (16/16, tour de référence au repos d'avant A4).

| Jeu / chemin | Fenêtre | Plein écran | ms/image fenêtre / plein écran (01/10, `fsqrt`) | 30/09 (`0128`) | Travail restant |
|---|---|---|---|---|---|
| Marble Blast Gold | vert | vert | 9,0 / 9,6 | 9,9 / 9,7 | fenêtre sur bureau 800×600 en session |
| Zenerchi | vert | non automatisé | 4,0 / — | 4,4 / — | automatiser plein écran |
| DOOM 3 | vert | vert | 56,1 / 56,2 | 61,7 / 61,6 | plantages au chargement, changements de mode, combat |
| Prey | vert | vert | 50,5 / 49,5 | 70,9 / 70,3 | changements de mode, combat |
| UT2004 | vert | vert | 25,0 / 25,4 | 27,2 / 27,5 | — |
| Warcraft III | non automatisé | vert | — / 15,4 | — / 17,8 | fenêtre, texte des menus |
| Colin McRae | non applicable | vert | — / 48,1 | — / 71,5 | — |
| Nexuiz ARB (`+r_glsl 0`) | vert | vert | 72,4 / 72,8 | 110,7 / 109,4 | tableaux clients déroulés (suites A4) |
| Nexuiz GLSL (`+r_glsl 1`) | vert | vert | 33,4 / 33,5 | 41,7 / 41,1 | attente des requêtes d'occlusion |
| RTCW | non automatisé | non automatisé | — / — | — / — | réinstallation puis diagnostic |

Ces scènes ne prouvent pas le jeu entier. Sources et critères : `docs/matrice-jeux.md`.
Nexuiz est installé dans `~/Nexuiz` ; sources hôte : `.run/jeux/Nexuiz/sources/`.
Son lanceur précharge `premierplan.dylib` pour éviter les replis de premier plan.

## Règles de travail

Avant et après un lot touchant le plugin, le device ou le cœur : `tools/matrice/matrice.py`
(ou `-j … -m …`), résultats dans `bench/matrice/dernier/tableau.md`.
À la reprise : `pgrep -fl qemu-system`, `tools/guest/tssh.sh uptime`. Mesurer au premier
plan ; DOOM 3 se mesure en parties réelles (`timedemo` refusé par la démo).
Les redémarrages de la VM sont autorisés ; un seul intervenant dessus à la fois.

- **Pas de repli : étendre le protocole.** Sous programme ARB, tout repli vers le rendu d'Apple
  finit dans `gleBuildInterpolateFunc` → `exit(1)` (`docs/re/glengine-exit-interpolateur.md`).
- **`qgpu_proto.h` ⇒ QEMU + plugin** (`cycle.sh NORUN=1`, sans redémarrer) ; **`qgpu_abi.h` ⇒
  QEMU + kext (`install.sh` dans l'invité, redémarrage) + plugin**, et c'est rare. Le plugin
  refuse un kext d'avant la v19 et un QEMU d'un autre `qgpu_proto.h` ; le kext refuse un device
  sans `QGPU_CAP_CLIENTS`.
- **Patch TCG** : une propriété de CPU, éteinte par défaut, une preuve d'équivalence, un A/B
  entrelacé ; allumée par défaut seulement après DOOM 3 (six parties par mode).
- **Mesurer avant d'optimiser** : `sample <pid> 10` dans Tiger, `frames.csv` à scène égale ; le
  self % localise, il ne valide pas (`docs/metrologie-boot.md`).
- **Un lot = un changement + une épreuve + un commit**, une ligne ici tant qu'il est ouvert, une
  entrée dans `CHANGELOG.md` quand il est fini.
- **Un seul agent sur la VM à la fois** ; les autres travaillent en copie isolée, sur le cœur et
  les tests natifs.
- **Après un `killall` de DOOM 3, redémarrer l'invité.** Ne jamais supprimer `.run/tiger.lock`
  (le flock meurt avec QEMU ; « Tiger tourne déjà » = un QEMU vit encore). Après `install.sh` en
  root : `sudo chown -R tiger ~/pomppc-build`.

---

## Où lire la suite

| Sujet | Fichier |
|---|---|
| Ce qui est fini (lots, versions du protocole, mesures) | `CHANGELOG.md` |
| Matrice de jeux (A3) : lancer, preuves, références, pièges | `docs/matrice-jeux.md` |
| Banc d'endurance, cause du gel OHCI | `docs/endurance.md` |
| Architecture, rétro-ingénierie, offsets, boucle de dev | `docs/architecture.md`, `docs/gpu-3d-tiger.md`, `docs/re/README.md` |
| Protocole (v7 → v23) | `docs/protocole-v*.md` |
| Processeur émulé : relevés, patches, A/B | `docs/tcg-g4.md`, `docs/flottant-rapide.md` |
| Backend GL et attente de l'invité | `docs/backend-gl-attente.md` |
| Son | `docs/audio-stabilite.md` |
| Traducteur de second niveau (plan mis de côté) | `docs/plan-traducteur-rapide.md` |
| Bilan raisonné du 23/09 (jeu par jeu) | `docs/bilan-2026-09-23-jeux-tiger.md` |
| Étude « court-circuiter GLEngine ? » | `docs/re/etude-court-circuit-glengine.md` |
| Bug hunts (22/09, 29/09 passes 1-4) | `docs/bug-hunt-2026-09-*.md` |
| Feuille de route de fond (OpenGL 1.5, QE, Core Image) | `docs/roadmap-opengl15.md` |
| Références externes (kext Tiger, IOGraphics, virtio-gpu) | `docs/references-ingenierie.md` |
