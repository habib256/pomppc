# TODO — priorités de POMPPC

État au 29/09/2026. Objectif : Mac OS X Tiger sous QEMU, bureau et jeux OpenGL
sur le GPU de l'hôte, avec une matrice de jeux verte comme preuve.

**Orientation choisie le 29/09 : une VM qui ne plante plus.** Le code a été relu
en quatre passes (`docs/bug-hunt-2026-09-2*.md`) ; ce qui reste casse en usage
réel et ne se trouvera qu'en le reproduisant dans la VM. Les autres orientations
proposées le même jour sont rangées dans « Plus tard » sous leur nom ; **la vitesse
est l'orientation suivante** (choisie le 29/09), puis jeux, maintenabilité et 1.0.

Les chantiers sont rangés par priorité ; leur domaine figure entre crochets.
**Maintenant** contient au plus trois chantiers, **Ensuite** donne l'ordre de prise,
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

- [ ] **[Outils] Banc d'endurance de session** — **livré le 29/09** : `tools/endurance/`
  (`docs/endurance.md`). Démarrages à froid, `shutdown -r` ou `system_reset` en série,
  plusieurs instances, sur des recouvrements qcow2 d'un clone APFS (`disks/tiger-endurance.qcow2`),
  sans fenêtre ni son ; panique lue dans la mémoire de l'invité (`panicstr`, `debug_buf`),
  gel par ssh et échantillons de NIP ; par incident : capture, `info registers -a`, 20 relevés
  de NIP, 0,3 s de `log int,mmu`, liste `kmod`, registres de l'OHCI, `panic.log` après
  reset, PC/LR/piles symbolisés (`mach_kernel` + `.sym` de `kextload -n -s -A`) ; taux par type
  avec intervalle de Wilson. Cycles de jeu (`jeu --jeu mb|d3|…`) éprouvés sur 2 cycles seulement.
  **Reste pour fermer :** une nuit sans intervention (démarrages + cycles de jeu), et une
  vraie campagne de cycles de jeu (DOOM 3 sans redémarrage : `kCGLBadDisplay`).

- [x] **[Système] « Panique » AppleUSBOHCI au démarrage SMP=2** — **cause trouvée le
  29/09** (`docs/endurance.md` §6) : c'est un **gel**, pas une panique, et seulement au
  redémarrage à chaud (0/100 à froid ; 3 sur 185 `shutdown -r` avant correction). Tempête
  d'IRQ 28 sur le CPU 0 dans `AppleUSBOHCI::FilterInterrupt`, alors que l'OHCI ne demande rien :
  `openpic_reset()` de QEMU gardait `pending` et repassait la source en front, et la
  baisse de la ligne par le reset PCI, qui arrive ensuite, était perdue. Trace : un « IRQ 28
  pending au reset » ↔ un gel. Correctif : `patches/openpic/0001` (agent DOOM 3).
  Après correction : 0/150 redémarrages (`apres-openpic-reboot`) ; avec la trace, 0/103 dont 2 déclencheurs traversés (0/253 au total, IC 0–1,5 %).
  **Reste :** la « panique cpu 1 » du 24/09 n'est pas reproduite (hypothèse : verrou tenu par
  le CPU 0 pris dans la même tempête) ; revoir sur la VM quotidienne après le correctif.
  Fermé le 29/09 (critère atteint : cause, correction, ≥ 100 redémarrages sans incident).

- [ ] **[Système] Paniques par délai de verrou (`LockTimeOut` = 250 ms)** — le « gel au
  chargement de DOOM 3 » (26/09, 1 lancement sur ~12) est une **panique** : `0x268b4` =
  `_panic+0x254` (boucle finale, EE coupé), vCPU 1 au repos (`_machine_idle`) ; sans
  kdp, Tiger n'en garde ni écran ni `panic.log`. Texte perdu ; mécanisme probable : un
  arrêt de vCPU côté QEMU (BQL, doorbell synchrone, hôte chargé) compté par `mftb` dans
  un délai de verrou tournant, comme la panique UT2004 du 27/09 (`Lock timeout` dans
  `_fpu_switch`). Arrêts mesurés : 95 ms au plus (`stallmeter`, kills de DOOM 3).
  Non reproduit le 29/09 : 0 gel sur 39 chargements, dont 8 juste après un redémarrage.
  `docs/gel-doom3-baddisplay.md` §2. **Au prochain gel :** `python3 tools/re/kpanic.py
  mach_kernel` AVANT tout reset (texte, appelant, pile). **Fermeture :** texte d'une
  vraie occurrence lu, cause établie, puis correction (relever `LockTimeOut` par le
  kext, ou borner les arrêts de vCPU) et série sans incident.

## Ensuite

Dans l'ordre. Chaque entrée passe par le banc d'endurance quand elle s'y prête.

- [ ] **[Son] Le son saccade dans DOOM 3** — **cause trouvée et corrigée le 29/09**
  (`docs/audio-stabilite.md` §« Saccades de DOOM 3 ») : ni sous-alimentation ni BQL.
  Le compteur de trames du Screamer (registre 5), dont le pilote de Tiger tire sa position
  et sa tête d'effacement, retardait sur le DMA (8192 trames d'anneau, la moitié du tampon
  invité) et ignorait sa remise à 0 : l'effacement tombait sur ce que le HAL venait
  d'écrire. Seuls DOOM 3 et Prey (tampon d'E/S de 4096 trames) écrivent assez loin pour
  être touchés : ~47 ms de zéros toutes les 93 ms. Avant : 84,6 % de trames nulles et
  776 trous en 84 s (DOOM 3, premier jeu après démarrage), Prey 33,7 % ; après : 0 trou
  sur 272 s (DOOM 3 fenêtre), 0 en plein écran, Prey et Marble Blast propres.
  **Reste pour fermer :** reconstruire `~/src/qemu` (`scripts/build_qemu_qfb.sh`, VM
  arrêtée) et la confirmation à l'oreille de l'utilisateur (extraits avant/après dans
  `bench/son-doom3/`). À part : un arrêt de la boucle principale de 186 ms (fin de
  cinématique, doorbell synchrone exclu), 1 fois en 13 parties, 93 ms de silence hôte.

- [ ] **[Système] Panique pendant le diagnostic UT2004 (27/09, 15:50)** : CPU 1,
  `Lock timeout`, PC `0x000AA010`, LR `0x00003820`, pile désalignée. Cause inconnue ;
  ne pas l'attribuer sans preuve à AppleUSBOHCI. Capture `bench/utweapon-priorites/panic.png`,
  détails dans `docs/matrice-jeux.md`. **Fermeture :** cause reproduite et correction
  vérifiée ; des lancements réussis ne suffisent pas.

- [ ] **[TCG] `tcg/0010` dans le binaire de référence** (code réécrit par l'autre vCPU,
  corrigé le 29/09, `docs/tcg-g4.md` §17) : reconstruire `~/src/qemu` par
  `scripts/build_qemu_qfb.sh` (VM quotidienne arrêtée), puis `smctest` E sur la VM
  quotidienne et, pendant une partie de DOOM 3, les compteurs de
  `patches/tcg/essais/0010-smcstat.patch` (démarrage et Marble Blast : 0 course ; le défaut
  n'est donc pas, sans autre preuve, la cause des incidents SMP ci-dessus).
  `x-icbi-sync` reste éteint par défaut (`ICBISYNC=1` l'allume) : sans lui, E garde des
  erreurs isolées d'une génération ; il sera allumé après l'A/B DOOM 3 (orientation vitesse).
  **Fermeture :** binaire de référence avec 0010, E à 0 erreur sur la VM quotidienne
  (`ICBISYNC=1`), compteurs DOOM 3 relevés, décision sur `x-icbi-sync` par A/B.

- [ ] **[Validation] Correctifs des bug hunts jamais éprouvés dans la VM** : `kextunload`
  avec un jeu ouvert (K4, KG4, KT1), `SUBMIT` après déchargement → erreur propre,
  `QFB=1` avec Marble Blast, GPU hôte bloqué puis rechargement du kext et
  `system_reset` (GL3, KT4, K6), `system_reset` en SMP=2 sans relance prématurée du
  CPU 1 (M1), son Tiger lecture/pause/arrêt/relance (S1–S7, Q3–Q7), plafond mémoire
  `QGPU_MEM_MB=512` sous DOOM 3 (R5), `run-all.sh` par les deux chemins
  (`docs/bug-hunt-2026-09-22.md` §11). **Fermeture :** une ligne de preuve par point dans
  `docs/bug-hunt-2026-09-29-passe4.md`.

- [ ] **[Système] Créneaux perdus, kext sans constante compilée** : vérifier que KT2
  (MAGIC au démarrage du kext), KT3 et K7 (bug hunt du 29/09) les ferment ; sinon cause.

- [ ] **[Protocole] `QGPU_REG_ERRORS` par client** : aujourd'hui global, un autre
  processus fait passer le plugin en synchrone et invalide ses miroirs sans faute de sa
  part (le plancher `err_floor` du 29/09 limite l'effet, pas la cause).

- [ ] **[Architecture] Suites A6 — écarts restants de [docs/architecture.md](docs/architecture.md)** :
  L1 (quarantaine et flux coupé, 29/09) et L6 (bandes et bascule ciblée, 29/09) sont
  traités ; reste à relire L2 à L5 contre le code actuel et à fermer ce qui tient encore.

- [ ] **[Outils — validation différée à la demande de l'utilisateur] Captures** :
  le rendez-vous `POMPPC_GL_CAPTURE` remplace `dump_attente`. Premier contrôle DOOM 3/Prey
  fenêtre vert. **Reste :** dix tours consécutifs verts, explicitement reportés le 27/09 ;
  à jouer sur le banc d'endurance quand l'utilisateur le demande.

## Plus tard

Les orientations proposées le 29/09, dans l'ordre de reprise prévu. Conserver les
dépendances et mesurer avant d'optimiser.

### Vitesse — orientation suivante (choisie le 29/09)

Ouverte dès que les chantiers de fiabilité libèrent l'hôte : une mesure A/B exige un
hôte au repos (aucune autre VM, charge relevée avant et après la fenêtre de mesure).
Ordre : planchers recalés, puis TCG (cache de sauts, verrou `mtmsr`/`rfi`), puis GPU
(`glUniform`, doorbell asynchrone), puis A4 et flottant AArch64 natif.

- [x] **[Métrologie] Planchers recalés** (30/09) : tour `bench/matrice/20260929-2344` sur hôte
  au repos, **15 vertes sur 16 automatisées**, images toutes justes ; planchers = mesure × ~1,25
  (DOOM 3 76, Prey 89, UT 36, WC3 22, CMR 88, Nexuiz 137/52, Marble Blast 13). Correction :
  le « DOOM 3 à 33,8 ms/image » du 29/09 après-midi était une fenêtre de mesure prise trop
  tôt (images 2968..3198 au lieu de ~4050) ; DOOM 3 est à 61, comme le 27/09.

- [x] **[Métrologie] « Régression » Zenerchi 4,4 → 7,9 ms/image** (30/09) : pas de régression,
  une fenêtre de mesure mal placée. L'écran de l'éditeur dure un temps fixe et tourne selon le
  lancement à ~3,4 ou ~15 ms/image : le menu arrive vers l'image 1650 ou 490, et la fenêtre fixe
  1500..2500 contenait parfois ses ~4 s de chargement. Bissection hôte au repos (QEMU
  `*.avant-bughunt`, kext et plugin de `8af9efc`, HEAD) : Zenerchi au menu 3,94-4,07 ms/image
  dans toutes les piles, Marble Blast plein écran 9,4-10,2 (9,3-10,3 dès le 26/09) ; le plugin de
  `8af9efc` donne aussi 4,3 ou 7,7 selon le lancement (`docs/matrice-jeux.md` §7). Fenêtre
  repérée sur la scène (`jeux/zen.py`), plancher 7 → 6 : 4,4 / 4,5 / 4,4 / 4,4 ; tour
  `20260930-0128` 16 vertes sur 16.

- [ ] **[Métrologie] Troisième facteur de lenteur** (`docs/tcg-g4.md` §14.7) : une partie DOOM 3
  sur huit lente de bout en bout ; cause inconnue (cœurs P/E, autre processus, thermique ?).
  Le 29/09, une charge hôte de 40–53 (autre projet, Spotlight) a doublé les ms/image :
  la matrice relève désormais la charge avant et après la fenêtre de mesure.
- [x] **[Plugin] Nexuiz / GLSL : `glUniform` neutre** (30/09, `POMPPC_GL_WLUNIF`, défaut 1) :
  le bit `0x04000000` ne fait plus recalculer, samplers suivis par la clé. 0 écart
  (`VERDICTCHECK`, `STATECHECK`), `gltest glslsmp` juste ; **gain dans le bruit** (fenêtre
  39,7 → 39,4, plein écran 39,9 → 39,4 ms/image, médianes de 3) : le bit n'est que dans
  51 k des 175 k dispatches, et presque tous portent aussi une liaison de texture (`+04`) ou
  une matrice/cible de texture (`+08 00010000`) — ce sont elles qui font recalculer
  (`CHANGELOG.md`). Suite éventuelle : la mémoire par texture et par époque du verdict.
- [x] **[TCG] Cache de sauts plus grand** (30/09, `tcg/0011`, `x-jc-bits`, propriété de
  l'accélérateur ; `docs/tcg-g4.md` §18) : 2^14 entrées, 0 divergence sur 23 G blocs
  vérifiés ; réussite 91,4 → 92,6 % ; DOOM 3 61,6 → 60,4 ms/image (médianes, 6 + 6
  entrelacées, p ≈ 0,06). **Allumé par défaut** (`JCBITS=14`). Les conflits plafonnent
  (65 536 entrées : 92,8 %) : la limite est la structure du hachage (2^(N/2) emplacements
  par page), un hachage replié fait moins bien (essai `x-jc-mix`).
- [x] **[TCG] Verrou global à chaque `mtmsr`/`rfi`** (30/09, `tcg/0012`, `x-msr-nobql`,
  §19) : décision sans verrou contre un compteur de séquence, `EXITTB` redondant supprimé ;
  378 M décisions vérifiées sous verrou, 0 divergence, 70 redémarrages SMP=2 sans incident ;
  DOOM 3 61,7 → 61,3 (p ≈ 0,004). **Allumé par défaut** (`MSRNOBQL=1`). Reste : Marble Blast
  non concluant (cellule trop bruitée), à refaire par `tools/tcg/mbab.sh`.
- [x] **[TCG] `x-icbi-sync` : A/B DOOM 3** (30/09, §20) : 61,8 → 61,6, dans le bruit ;
  **allumé par défaut** (`ICBISYNC=1`). Matrice complète sur la configuration retenue
  (`bench/tcg/ab/matrice-vit`) : 16 vertes sur 16, images toutes justes (§21).
  **À faire par l'utilisateur** : reconstruire le binaire de référence
  (`scripts/build_qemu_qfb.sh`, patches 0011 et 0012).
- [x] **[Protocole] Doorbell asynchrone côté invité** (bug hunt D2, levier L4) : **mesuré, rien
  à convertir** (30/09). Lignes `SYNC` de la note : en jeu, les seuls doorbells synchrones
  sont les sondes de définition de programme (DOOM 3 0,0005 ms/image, Prey ≤ 0,014, Nexuiz
  GLSL 0,05, UT2004 2 au chargement) ; BeginPrimitiveBuffer, file pleine, asynchrone coupé :
  0 dans les 13 cellules de la matrice. Très loin du seuil S-M6 (2 ms/image).
- [ ] **[Plugin] Transmission paresseuse** : mesure honnête et décision du défaut.
- [ ] **[Protocole] A4 — Déplacer le travail vers l'hôte** : bloc d'état partagé lu par le
  device, textures par DMA sur plages sales, empaquetage minimal. Épreuve : `send_state` et
  `compute_state` sortent du profil.
- [ ] **[Backend GL]** G7 `glTexSubImage*` par rectangle sale, G8 PBO en rotation pour
  `SURF_PRESENT`, G9 cache d'état dans `gl_target`.
- [ ] **[TCG] Flottant scalaire en instructions AArch64 natives** (`docs/tcg-g4.md` §15.7).

### Jeux — tous les jeux, toutes les résolutions

- [ ] **[Jeux] Colin McRae en 1024×768 et au-delà** (défaut signalé par l'utilisateur le
  29/09 : 3D fausse en course, polygones justes) : l'hôte copie juste à toutes les tailles
  (repro natif) ; le bug hunt 3 a corrigé l'effacement des textures hôte seulement à chaque
  erreur (T1), le 4 le `SURF_TEX` refusé en `NO_MEM`. **Le 29/09, après ces correctifs :
  cellule plein écran 1024×768 verte** (`bench/matrice/20260929-1537/cmr-pe`, voiture en
  course à 78 km/h, rejeu identique à la VM, 63 ms/image) ; journal : cibles cachées
  1024×768, 512×384 et 256×192 à la taille de l'écran, aucune erreur du device, aucun
  écart de taille GLEngine/surface. Reste la confirmation par l'utilisateur en jouant,
  puis tableau des autres modes (4:3 et larges : menus, course,
  proportions, changement de mode, retour au bureau, temps/image). La voiture qui roule
  reste à prouver ; `POMPPC_GL_RECT=0` fait planter le jeu chez Apple : ne pas s'en servir.

- [ ] **[Jeux — session] Marble Blast en fenêtre, bureau 800×600×16** : échoue en fenêtre
  (surface rognée, replis, pas de présentation capturable), le plein écran est vert.
  Identifier le choix du mode vidéo et garantir un bureau assez grand, puis refaire
  Nexuiz fenêtre/plein écran. **Fermeture :** cellules vertes après démarrage frais,
  réglages utilisateur préservés.

- [ ] **[Jeux] DOOM 3 et Prey en plein écran** : changement de mode par le jeu (`r_mode 3`,
  `CGDisplaySwitchToMode`, `SURF_PRESENT` sur l'écran redimensionné) et mesure en combat.

- [ ] **[Jeux] Warcraft III** : texte des menus (chemin tableaux, un sommet sans couleur
  reste blanc) ; menu principal juste au 26/09, à revoir en partie.

- [ ] **[Outils] A3, suites** : Zenerchi en plein écran et Warcraft III en fenêtre
  (réglage à trouver, sinon clic par System Events) ; Colin McRae fenêtre non applicable.

- [ ] **[Outils] Rejeu : `SURF_READBACK` d'une surface jamais liée dans le vidage** → `NO_SURF`
  (DOOM 3 fenêtre, tour `20260926-2156`) : le prologue de `tests/qgpu_replay.c` ne crée la
  surface qu'au `SURF_BIND`/présentation. Épreuve : rejeu sans `NO_SURF`, image inchangée.

- [ ] **[Plugin] Coordonnées de texture en mode immédiat sous programme de sommets** : perdues
  (vu en écrivant `gltest rectfp`). Scène à écrire ; fermeture : comparé au rendu d'Apple.

- [ ] **[Backend GL] `gltest tex14` « λ=2 sans biais »** : défaut du GL de l'hôte macOS (le
  biais d'unité du dessin précédent reste appliqué ; un `glFlush` le corrige mais coûte).
  Décision : ne réappliquer que sur changement, ou passer le biais dans l'échantillonneur.

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
- [ ] **[Hôtes] Hôte PC x86** : `x-fast-fp` et `x-sr-tlb` jamais validés sur x86
  (`docs/plan-traducteur-rapide.md` §1.3).
- [ ] **[Hôtes] Un seul disque, deux hôtes** : `tiger.raw` partagé par USB entre le Mac et le PC.
- [ ] **[TCG] `tlbie` en SMP stock** : défaut de QEMU 9.2, corrigé par `x-sr-tlb` ; à signaler en amont.

### Divers

- [ ] **[Système] Quartz Extreme et Core Image** sur `tiger-dev.raw` : surfaces hôte pour le
  WindowServer, plus de quatre clients (`docs/roadmap-opengl15.md`).
- [ ] **[Frontend] Frontend F1, restes non joués** : la vraie touche Ctrl+Cmd+F, écran Retina,
  plusieurs moniteurs.
- [ ] **[Métrologie] Métrologie boot** (`docs/metrologie-boot.md`) : baseline de 23,32 s à
  refaire avec le harnais durci ; A/B du coût du Screamer jamais lancé.

## Bloqué

Chaque entrée indique la condition de reprise ; une cause encore inconnue reste
un travail de diagnostic dans « Ensuite », pas un blocage.

- [ ] **[Plugin] Replis à retirer** une fois la mesure en jeu faite par l'utilisateur :
      `POMPPC_GL_VERDICT=0`, `POMPPC_GL_WHITELIST=0`, `POMPPC_GL_TEXMEMO=0`,
      `POMPPC_GL_STSKIP=0`.
      **Reprise :** mesure en jeu par l'utilisateur ; ensuite retirer les options
      devenues inutiles et vérifier la matrice.

- [ ] **[Jeux] RTCW** quitte après 8 s (`~/rtcw.command`, LaunchCFMApp) sans rien dessiner ; stdout
      dans `~/rtcw-dump`. **Absent du disque quotidien au 26/09** (`~/Desktop/Wolfenstein`
      manque) : le réinstaller d'abord.
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

État au 29/09/2026 (bug hunt en quatre passes, `main` `2d48131` puis `bf14f79`).

| Élément | État |
|---|---|
| Protocole / ABI | GL **v22**, transport kext **v19** (`QGPU_ST_NO_MEM`, `QGPU_REG_NOMEM`, `QGPU_CAP_GLSL_PATHS` ajoutés sans changer d'ABI) |
| QEMU de référence | `~/src/qemu/build/qemu-system-ppc64`, reconstruit depuis `18ad012` ; précédent en `*.avant-bughunt` |
| Invité quotidien | `tiger.qcow2`, kext et plugin de `18ad012` (gcc-4.0 dans l'invité), SMP=2 |
| TCG | `0001–0004`, `0006–0008` activés par défaut ; `SRTLB=0`, `LFSINLINE=0`, `VFPFAST=0`, `VPERMFAST=0`, `JITNEAR=0`, `FPINLINE=0`, `RETINLINE=0`, `JCIDX=0` les désactivent |
| Tests natifs et scripts | **156 OK, 0 échec, 6 ignorés** ; frontend ctest 3/3 |
| Travaux clos | Voir `CHANGELOG.md` et `docs/bug-hunt-2026-09-29*.md` |
| Profils | Plugin : `bench/plugin/ab2-B*/{d3,prey}-fen/mesure/sample.txt` ; TCG : `bench/tcg/` (hors git) |

## Matrice de jeux

Une cellule verte exige une image juste (rejeu = VM et référence validée), zéro repli
hors rafraîchissements admis (2 par 90 images en fenêtre), et une mesure à scène fixe
sous le seuil du jeu. Une scène `gltest` comparée au rendu d'Apple éprouve chaque notion
nouvelle. Les modes fenêtre et plein écran sont requis quand le jeu les propose.

**Dernier tour complet : `bench/matrice/20260930-wlunif/tableau.md`** (QEMU de référence
avec `0011`/`0012`, plugin `20260930-wlunif`) : 16 vertes sur 16, images toutes justes ;
Marble Blast 9,2 / 9,7 ; Zenerchi 3,9 ; DOOM 3 59,9 / 59,5 ; Prey 71,5 / 71,0 ; UT2004
25,9 / 25,9 ; Warcraft III — / 17,8 ; Colin McRae — / 69,3 ; Nexuiz ARB 109,0 / 109,6 ;
Nexuiz GLSL 39,9 / 40,8.

**Tour de référence : `bench/matrice/20260930-0128/tableau.md`** (b9004cc pour QEMU, kext et
plugin, matrice de ce jour, hôte au repos, charge 1-2) : **16 vertes sur 16 automatisées**,
images toutes justes. ms/image fenêtre / plein écran : Marble Blast 9,9 / 9,7 ; Zenerchi 4,4 / — ;
DOOM 3 61,7 / 61,6 ; Prey 70,9 / 70,3 ; UT2004 27,2 / 27,5 ; Warcraft III — / 17,8 ;
Colin McRae — / 71,5 ; Nexuiz ARB 110,7 / 109,4 ; Nexuiz GLSL 41,7 / 41,1. Précédent :
`20260929-2344` (15/16, Zenerchi rouge à 7,9 par sa fenêtre de mesure). Le tableau
ci-dessous est celui du 27/09, gardé pour comparaison.

Tour complet précédent : `bench/matrice/20260927-1137/tableau.md`, **13 vertes sur 15**.
DOOM 3 fenêtre est ensuite verte au contrôle isolé `20260927-1216` ; ce contrôle
ne transforme pas le tour complet en « 14/15 ». Sur le QEMU de référence reconstruit,
DOOM 3 et Nexuiz GLSL plein écran sont verts au tour `20260927-1223`.

| Jeu / chemin | Fenêtre | Plein écran | ms/image fenêtre / plein écran (tour 1137) | Travail restant |
|---|---|---|---|---|
| Marble Blast Gold | rouge | vert | 30,9 / 9,3 | fenêtre |
| Zenerchi | vert | non automatisé | 4,4 / — | automatiser plein écran |
| DOOM 3 | rouge au tour, vert au contrôle isolé | vert | 60,7 / 60,4 | captures, changements de mode, combat |
| Prey | vert | vert | 69,3 / 69,5 | captures intermittentes, changements de mode, combat |
| UT2004 | vert | vert | 26,4 / 26,0 | arme noire hors scène mesurée |
| Warcraft III | non automatisé | vert | — / 17,4 | fenêtre, texte en partie |
| Colin McRae | non applicable | vert | — / 69,4 | voiture en mouvement |
| Nexuiz ARB (`+r_glsl 0`) | vert | vert | 107,8 / 108,1 | référence de comparaison |
| Nexuiz GLSL (`+r_glsl 1`) | vert | vert | 40,5 / 41,2 | coût du verdict après `glUniform` |
| RTCW | non automatisé | non automatisé | — / — | réinstallation puis diagnostic |

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
| Architecture, rétro-ingénierie, offsets, boucle de dev | `docs/gpu-3d-tiger.md`, `docs/re/README.md` |
| Protocole (v7 → v21, à unifier) | `docs/protocole-v*.md` |
| Processeur émulé : relevés, patches, A/B | `docs/tcg-g4.md`, `docs/flottant-rapide.md` |
| Traducteur de second niveau (plan mis de côté) | `docs/plan-traducteur-rapide.md` |
| Bilan raisonné du 23/09 (jeu par jeu) | `docs/bilan-2026-09-23-jeux-tiger.md` |
| Étude « court-circuiter GLEngine ? » | `docs/re/etude-court-circuit-glengine.md` |
| Bug hunt du 22/09 (90 findings, verdicts) | `docs/bug-hunt-2026-09-22.md` |
| Feuille de route de fond (OpenGL 1.5, QE, Core Image) | `docs/roadmap-opengl15.md` |
| Références externes (kext Tiger, IOGraphics, virtio-gpu) | `docs/references-ingenierie.md` |
