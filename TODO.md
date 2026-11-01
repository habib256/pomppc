# TODO — priorités de POMPPC

> **Deux hôtes (PC x86-64 et Mac M4) en alternance** : l'état de chacun et ce qui y reste à faire
> sont dans [docs/etat-des-hotes.md](docs/etat-des-hotes.md), à lire en premier.

État au 01/11/2026. Objectif : Mac OS X Tiger sous QEMU, bureau et jeux OpenGL
sur le GPU de l'hôte, avec une matrice de jeux verte comme preuve.

**Où on en est.** La fiabilité a été tenue (endurance PC 683 démarrages sans incident le 28/10,
M4 342 démarrages et 40 cycles de jeu sans incident le 29/10 ; Quartz Extreme allumé sur les
deux hôtes). **L'orientation est de nouveau la vitesse, sur les deux hôtes**, car la version x86
reste trop lente pour être publiée (règle du 20/10). Vitesse au 01/11, ms/image, hôte au repos :

| jeu | PC x86-64 (binaire rapide, plugin `packbits`) | M4 (30/10, QE) | rapport |
|---|---:|---:|---:|
| DOOM 3, fenêtre | 78-80 | 29 | ×2,7 |
| UT2004, fenêtre | 40 | 19,6 | ×2,0 |
| Marble Blast, fenêtre | 8,3 | 6,1 | ×1,4 |
| Zenerchi, fenêtre | 4,6 | 2,8 | ×1,6 |
| Colin McRae, plein écran, **en course** | 82 | à remesurer (`6054367`) | — |

Cible du PC : DOOM 3 à 50 ms/image (`docs/vitesse-doom3-x86.md`, départ 111,5 le 02/10).

**Ce que les profils ont établi** (`docs/vitesse-doom3-x86.md` §18-§25, `docs/tcg-g4.md` §44-§47) :
- Sur le PC, le goulot du code généré est le **chargement des instructions** (frontend bound
  40 %, 100 octets de x86 par instruction PowerPC) et la latence mémoire, pas le nombre
  d'instructions : retirer des tests hors du chemin critique ne rapporte plus rien (plafond
  mesuré 3,5 %). Les leviers restants du JIT x86 sont petits et génériques : recherche de
  blocs (mb 10 %, zen 14 %, cmr 7 % du vCPU), base de temps (ut 14 %), PGO.
- **Le plugin GL de l'invité est le levier commun aux deux hôtes** : 28 % du vCPU de cmr,
  22 % de d3, 16 % de mb, 12 % de zen, 11 % d'ut sur le PC. Chaque pourcent gagné là vaut sur
  x86 et sur arm64 sans portage, et c'est le seul levier qui agit sur cmr (PACKBITS −3,5 %
  quand les leviers TCG le laissent neutre ou le ralentissent).
- Le fil de rendu de l'hôte (`qgpu-render`) occupe 55 % d'un cœur sur mb, 41 % sur zen, 25 %
  sur d3 (≈ 20 ms par image). Sur le PC, l'invité ne l'attend pas (0,1-3 ms/image) ; sur le M4,
  où d3 est à 29 ms, il peut devenir le chemin critique : à mesurer.

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

- [ ] **[Plugin — les deux hôtes] Alléger le plugin de l'invité, suite de PACKBITS**
  (`docs/plugin-allegement.md` §8, profil `bench/tcg/ab/xprof-0111-cmr-pb`). Le coût est
  maintenant étalé : `geom_draw_client` 3,7 % du vCPU de cmr, `geom_draw_native` 1,5,
  `send_state` 1,3, `prog_sync` 1,1, `geom_dispatch_locked` 1,1, **`___stub_getrealaddr` 1,0**
  (stub PIC de libgcc : appels indirects à travers les stubs, à ramener en appels directs ou
  en ligne), `texture_unit_ok` 0,9, `geom_format` 0,8, `us_fill`/`us_get` 1,3,
  `prog_params_arena`/`prog_state` 1,4 ; dans libSystem, le bloc `0x90131200` fait 1,8 % sur
  cmr et 0,7 % sur d3/mb (à identifier : `memcpy`, verrou ?). Méthode du §2 : un levier par
  variable `POMPPC_GL_*`, mode `=2` de contrôle, `plugmd5` 0 écart, A/B des cinq jeux sur le
  PC ; le M4 reçoit le bundle (à reconstruire dans sa VM de dev, puis A/B PACKBITS là-bas,
  gain attendu 1-4 %). **Fermeture :** plugin sous 20 % du vCPU sur cmr et d3, aucun jeu en
  perte, matrice verte.

- [ ] **[TCG — PC] Les leviers génériques restants du JIT x86** (`docs/tcg-g4.md` §44-§47) :
  1. **PGO entraîné avec cmr** : `pgo-train.sh --plein-ecran cmr` (01/11), réentraînement
     fait, A/B neuf contre ancien (`bench/tcg/ab/pgo-0111-*`) : neutre en fenêtre, cmr non
     prouvé (+6 % sur 2 parties, l'une polluée par une charge de l'hôte, `neuf-1` quittée
     `exit 138`). **Ancien binaire remis**, nouveau gardé dans `~/src/qemu/build-fast-pgo0111cmr/` :
     recontrôle cmr 3 parties au repos strict, et entraînement de cmr sur un binaire qui
     atteint la course (l'instrumenté n'y arrive pas en 901 s).
  2. **Recherche de blocs** (mb 10 %, zen 14 %, cmr 7 %) : `tcg/0060` x8 (effacement exact, 256
     cases) gagne mb −8,7 % et ut −1,9 % mais **cmr +4,2 %** en course, seule perte propre à
     cmr qui reste inexpliquée. À instruire : compter ce que coûte le parcours exact sous les
     ~2 000 écritures de BAT par seconde de cmr (32 pages chacune), puis essayer **x8 +
     `tcg/0063`** (plus de vidage du cache de sauts sur un DBAT) ; l'un sans l'autre a été
     mesuré, pas les deux.
  3. **Base de temps** : `helper_load_tbu/tbl` 14 % du vCPU sur ut, 3 % sur zen. D'abord
     identifier la boucle chaude du noyau `0x33f3c-0x33fec` (11,7 % du vCPU d'ut, près de
     `_idle_thread` : `mach_absolute_time`, `timer_event`) : si c'est une attente active du
     noyau, le levier est d'endormir le fil hôte (ut occupe 1,9 cœur) plutôt que d'accélérer
     `mftb` ; sinon `mftb` en ligne (sans helper), gain modeste (−2,4 % mesuré pour helper
     lent → rapide).
  4. **`tcg/0063` `x-dbat-lite`** : témoin cmr sous `TLBPVERIFY=16 RETVERIFY=1` **sans** 0063,
     pour savoir si la panique de délai de verrou du 01/11 vient des vérificateurs ; puis
     rejouer l'A/B en fenêtre à 3 parties (2 parties : d3 −3 %, mb −2,2 %, ut +3,7 %).
  **Fermeture de chaque levier :** preuve, A/B des cinq jeux, allumé ou archivé avec le chiffre.

- [ ] **[TCG — M4] Mesurer le M4 avant de l'optimiser, puis rattraper le PC.** Le M4 n'a pas
  de profil croisé depuis la parité du 25/10. Dans l'ordre :
  1. **Rattrapage** : reconstruire `~/src/qemu` (`qgpu-core.c` du 31/10), reconstruire le plugin
     dans la VM de dev (PACKBITS, bundle de HEAD) et l'installer, rejouer cmr-pe avec la fenêtre
     en course (`6054367`, le M4 mesurait peut-être déjà la course : à vérifier sur ses
     `frames.csv`), référence cmr à reprendre (faux reflet).
  2. **Profil croisé des 5 jeux** (`sample` du processus QEMU, fils vCPU et `qgpu-render`,
     comme `tools/re/perfpart.py` sur le PC) : part du JIT, recherche de blocs, `mftb`,
     helpers, et **part du fil de rendu hôte** ; sur d3, si `qgpu-render` approche les 29 ms
     de l'image, le levier `nat_conv_attr` (27 % du fil de rendu : attributs reconvertis à
     chaque dessin natif, à convertir une fois au `BUF_SUBDATA`) passe en tête.
  3. **`tcg/0058` délais courts** : l'attente à la microseconde fait zen −18 % mais ut +6,6 % ;
     variante `select` pour les seuls délais sous la milliseconde, profil d'UT (verrou).
  4. Parité de la zone froide (`tcg/0056`, saut long aarch64) en dernier : gain PC de 1-2 %
     sur un goulot L1i propre au PC.
  **Fermeture :** doc de profil M4, PACKBITS mesuré, A/B de chaque levier, `etat-des-hotes.md`.

## Ensuite

Dans l'ordre. Chaque entrée passe par le banc d'endurance quand elle s'y prête.

- [ ] **[Système — QE] Bande d'ancien jeu sous l'ombre du Terminal (PC, 01/11)** : DOOM 3 en
  fenêtre au chargement (fenêtre noire), Terminal posé dessus, une bande d'une ancienne image
  du jeu sous le bord droit de l'ombre. Variante « ancien jeu » du bug corrigé le 30/10 sur le
  M4 (`9b5431a`, `tex_bind_id`). Pistes : texture d'export périmée sous QE direct, chemin
  `FIXED4` du PC, `qgpu-core.c` du 31/10. **En diagnostic (agent) ; fermeture :** cause
  prouvée, reproduction avant/après, matrice en fenêtre verte, journal `docs/quartz-extreme.md`.

- [ ] **[Jeux] Performances selon la résolution** — gros ralentissements signalés par
  l'utilisateur dans **Colin McRae en 1920 × 1080** (27/10 ; sur le M4 la cellule y est à +11 %
  contre 1024×768). Étendre les essais aux autres résolutions, jeux et modes ; relever les temps
  d'image et les replis sur une même scène. Les cellules vertes aux résolutions de la matrice ne
  valent pas pour toutes. Lié au point précédent (relecture et présentation par bandes).

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

- [ ] **[Validation] Restes des épreuves en VM des bug hunts** : reconstruire `build/` et
  `build-fast/` (PC) puis le M4 avec le device de l'épreuve « GPU hôte bloqué »
  (`docs/epreuve-gpu-bloque.md`), installer le kext A6 dans les VM (paquet invité ; ABI
  inchangée). Le reste est tenu (`docs/bug-hunt-2026-09-29-passe4.md`, dernière section).

- [ ] **[OS 9] Mesurer les gains CPU et élargir les essais** — profils expérimentaux livrés
  le 27/10 (`docs/os9-optimisations-cpu.md`), défaut `g4` conservé. Reste : A/B de vitesse sur
  une charge reproductible, autres jeux et application AltiVec.

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
- [ ] **[Jeux] UT2004 : armes noires dans la démo de test — reste le M4** (PC corrigé le
  07/10, `docs/re/ut2004-arme-noire.md`). Revider la démo sur le M4 (`@1`, 450 images), rejouer
  les images ~166-195. Fermeture : image juste dans la démo sur le M4.
- [ ] **[Outils] A3, suites** : Zenerchi en plein écran et Warcraft III en fenêtre.
- [ ] **[Outils] Rejeu : `SURF_READBACK` d'une surface jamais liée** — corrigé le 07/10 sur le
  PC ; à confirmer sur le tour `20260926-2156` du M4.
- [ ] **[Plugin] Mode immédiat sous programme de sommets** : revu sur DOOM 3 (PC, 07/10) ;
  reste Colin McRae au M4 (`POMPPC_GL_VPIMM=0` pour l'A/B).
- [ ] **[Backend GL] `gltest tex14` « λ=2 sans biais »** : défaut du GL de l'hôte macOS.
  Décision : ne réappliquer que sur changement, ou passer le biais dans l'échantillonneur.

## À confirmer par l'utilisateur

Le travail est fait ; il ne manque que l'essai ou la décision de l'utilisateur.

- [ ] **[Matrice — PC] Valider à l'œil les références `d3-fen` et `cmr-pe` du PC**
  (`tools/matrice/matrice.py --valider d3-fen`, `--valider cmr-pe` ; cmr-pe reprise le 31/10
  après le correctif des reflets, l'ancienne dans
  `bench/matrice/ref-linux/cmr-pe.faux-reflet-2810/`) ; regarder les captures de
  `bench/tcg/ab/cmr-res-3110/`.
- [ ] **[Plugin] Replis à retirer** : `POMPPC_GL_VERDICT=0`, `POMPPC_GL_WHITELIST=0`,
  `POMPPC_GL_TEXMEMO=0`, `POMPPC_GL_STSKIP=0`. **Reprise :** mesure en jeu par l'utilisateur ;
  ensuite retirer les options devenues inutiles et vérifier la matrice.
- [ ] **[Outils] Captures, dix tours consécutifs verts** : à jouer sur le banc d'endurance quand
  l'utilisateur le demande.
- [ ] **[TCG — PC] Gouverneur `performance`** (`docs/vitesse-doom3-x86.md` §5.4) : un A/B à
  faire, il demande la racine.

## Plus tard

### Vitesse

- [ ] **[Plugin] Suites A4** : GLEngine déroule les tableaux clients en Begin/End (Nexuiz ARB
  14-16 %, Warcraft III 4,5 %) — RE du canal `gldCreateVertexArray`
  (`docs/protocole-v23-geometrie.md`) ; clés chaudes par une table « objet GLEngine → qtex »
  côté device ; `geom_send_all`.
- [ ] **[Protocole] Nexuiz : les ~2 ms restantes d'attente des requêtes d'occlusion** :
  soumettre par tranches pendant les dessins, ou résultats écrits par l'hôte.
- [ ] **[Plugin] Transmission paresseuse** : mesure honnête et décision du défaut.
- [ ] **[Plugin] Verdict GLSL : mémoire par texture et par époque** (suite de `WLUNIF`).
- [ ] **[Plugin] Indices en VBO** (`docs/vitesse-doom3-x86.md` §10.4) : `r_useIndexBuffers 1`
  retombe sur l'empaquetage ; comprendre le refus de `raw_ensure` pour le tampon d'éléments.
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
- [ ] **[TCG] `tlbie` en SMP stock** : défaut de QEMU 9.2, corrigé par `x-sr-tlb` ; à signaler
  en amont. Aussi : `helper_store_[id]batu` vide l'ancien BAT avec le masque de la nouvelle
  valeur (`tcg-g4.md` §45, `tcg/0062` le relève).

### Divers

- [ ] **[Système] Quartz Extreme** : allumé sur les deux hôtes (`docs/quartz-extreme.md`).
  Restes : bug de l'ombre (Ensuite), `install.sh QE=1` et personnalités, Core Image.
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
| Régions chaudes recompilées par LLVM | +17,6 % sur DOOM 3 | idem §9 ; 6-10 mois de travail, mis de côté le 25/09 |
| Mémoire directe (« fastmem ») | net ≤ 5 % pour un chantier entier | `vitesse-doom3-x86.md` §21 |
| Vérification TLB compacte | extrapolé de la zone froide : 0-4 % | §21 |
| « Opérande flottant connu partout » | plafond 3,5 %, même sans aucun test | §18.1 |
| TLB logiciel en pages énormes | les deux bras exécutaient le même code | §18.3 |
| `tcg/0053` AltiVec flottant connu | neutre en jeu | `tcg-g4.md` §41, `archive/0053-vfp-known` |
| `tcg/0057` QoS des vCPU (M4) | neutre au repos | §42 |
| `tcg/0059` `vperm` en PSHUFB | exact, neutre | §43 |
| `tcg/0060` x8 tel quel | cmr +4,2 % en course | §46 (voir « Maintenant » pour x8 + 0063) |
| `tcg/0061` `x-bat-skip` | neutre partout en course, rafale non expliquée | §46 |
| `tcg/0063` `x-dbat-lite` tel quel | cmr neutre, ut +3,7 % | §47 (témoin à jouer) |
| Appel MMIO synchrone « plugin vers l'hôte » | natif ×0,6-1,5 sur le travail réel | `pomppc-plugin-hote` (branche non fusionnée) |
| 3-4 vCPU | 0 gain sur les jeux | Bloqué |

## Bloqué

- [ ] **[Jeux] RTCW** quitte après 8 s sans rien dessiner ; absent du disque quotidien.
      **Reprise :** jeu réinstallé sur le disque quotidien, puis diagnostic du lancement.
- [ ] **[TCG] 3-4 vCPU** : Tiger démarre sur 3 et 4 avec
      `patches/smp-mac99/essais/qemu-mac99-4cpus.patch` ; 0 gain sur les jeux.
      **Reprise :** demande explicite d'un mode 3–4 vCPU.
- [ ] **[TCG] Traducteur de second niveau** (`docs/plan-traducteur-rapide.md`, mis de côté par
      l'utilisateur le 25/09). **Reprise :** décision explicite de rouvrir ce chantier.

## Référence validée

L'état exact de chaque hôte (binaires, défauts allumés, plugin installé, VM) est tenu dans
`docs/etat-des-hotes.md`, section 2 (PC) et 3 (M4). Repères au 01/11 :

| Élément | État |
|---|---|
| Protocole / ABI | GL **v22**, transport kext **v19** ; capacités ajoutées sans changer de version (`TEX_PRESENT`, `TEX_READBACK_ROWS`, …) |
| QEMU | 11.1.2 + série `patches/` ; PC : `~/src/qemu/build-fast` (PGO, -O3, -march=native, réentraîné le 01/11 avec cmr, A/B en cours) et `build/` (référence) ; M4 : `~/src/qemu/build` (à reconstruire avec `qgpu-core.c` du 31/10) |
| Défauts TCG | PC : série jusqu'à `0056` (zone froide) ; M4 : série du PC jusqu'à `0052` + `0054` ; `0057`-`0063` éteints, non nommés |
| Invité | Tiger 10.4.11, kext du 30/10 (curseur QE), plugin `20261101-packbits` sur le PC (`disks/prebuilt`), `20261023-uvwon` + QE (`f01d2d7`) sur le M4 |
| Matrice | PC : `references-linux.csv`, 5 cellules automatisées (mb, zen, ut, d3 en fenêtre, cmr plein écran **en course** depuis `6054367`) ; M4 : `references.csv` (jamais touché par le PC), 17/17 vertes le 28/10 |
| Tests natifs | PC 240 OK / 0 échec (07/10), M4 287 OK / 0 échec (27/10) |

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
| DOOM 3 | vert | vert | 56,1 / 56,2 | changements de mode, combat ; PC : référence à valider |
| Prey | vert | vert | 50,5 / 49,5 | changements de mode, combat |
| UT2004 | vert | vert | 25,0 / 25,4 | armes noires (M4) |
| Warcraft III | non automatisé | vert | — / 15,4 | fenêtre, texte des menus |
| Colin McRae | non applicable | vert | — / 48,1 | mesure **en course** sur les deux hôtes ; 1920×1080 |
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
| Vitesse sur le PC x86-64 (profils, leviers, chiffres) | `docs/vitesse-doom3-x86.md` |
| Parité et vitesse sur le M4 | `docs/parite-m4-2510.md`, `docs/etat-des-hotes.md` §3 |
| Allègement du plugin de l'invité | `docs/plugin-allegement.md`, `docs/d3-plugin-x86.md` |
| Processeur émulé : relevés, patches, A/B | `docs/tcg-g4.md`, `docs/flottant-rapide.md` |
| Matrice de jeux (A3) : lancer, preuves, références, pièges | `docs/matrice-jeux.md` |
| Banc d'endurance | `docs/endurance.md` |
| Quartz Extreme | `docs/quartz-extreme.md` |
| Architecture, rétro-ingénierie, offsets, boucle de dev | `docs/architecture.md`, `docs/gpu-3d-tiger.md`, `docs/re/README.md` |
| Protocole (v7 → v23) | `docs/protocole-v*.md` |
| Backend GL et attente de l'invité | `docs/backend-gl-attente.md` |
| Son | `docs/audio-stabilite.md` |
| Traducteur de second niveau (plan mis de côté) | `docs/plan-traducteur-rapide.md` |
| Bug hunts (22/09, 29/09 passes 1-4) | `docs/bug-hunt-2026-09-*.md` |
| Feuille de route de fond (OpenGL 1.5, QE, Core Image) | `docs/roadmap-opengl15.md` |
| Références externes (kext Tiger, IOGraphics, virtio-gpu) | `docs/references-ingenierie.md` |
