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

- [ ] **[Outils] Banc d'endurance de session** : sans lui, chaque incident ci-dessous
  reste un souvenir. Un outil qui enchaîne seul N démarrages (SMP=2) et N cycles
  lancement/arrêt de jeu, sur un clone APFS du disque quotidien (`cp -c`, pour ne
  jamais toucher `tiger.qcow2` ni occuper la VM quotidienne), détecte panique et gel
  (ssh muet, image figée), et range pour chaque incident : capture d'écran, registres
  de tous les vCPU (`info registers -a` au moniteur), `panic.log` si l'invité l'a écrit,
  PC et LR symbolisés contre `mach_kernel` et les kexts chargés.
  **Fermeture :** une nuit de banc sans intervention, un rapport par incident, taux
  d'incident par type avec son intervalle.

- [ ] **[Système] Panique AppleUSBOHCI au démarrage SMP=2** : environ un démarrage
  sur dix (`docs/smp-coeurs.md`). Premier client du banc : c'est l'incident le plus
  fréquent et le plus facile à provoquer. Le choix SMP=2 est déjà tranché ; le CPU 1
  démarre au relâchement du GPIO 4 depuis le 29/09 (bug hunt, M1) — mesurer le taux
  avant de supposer quoi que ce soit.
  **Fermeture :** cause identifiée, correction, puis série de démarrages (≥ 100) sans panique.

- [ ] **[Système] Gel de l'invité au chargement de DOOM 3 et `kCGLBadDisplay` après
  un `killall`** : gel (matrice, 26/09, 1 lancement sur ~12) = plus de ssh, vCPU 0
  bouclant en `0x268b4` (EE coupé), vCPU 1 en `0xaf6b4`, pas de `panic.log`, puis deux
  `system_reset` bloqués au démarrage (« using 1966 buffer headers ») ; relevé dans
  `docs/matrice-jeux.md` §5. `kCGLBadDisplay` : après un `killall` de DOOM 3, tout
  lancement GL suivant échoue jusqu'au redémarrage de l'invité ; Prey et `gltest`
  démarrent. Causes non trouvées.
  **Fermeture :** chacun reproduit sur le banc, cause établie, correction, série sans incident.

## Ensuite

Dans l'ordre. Chaque entrée passe par le banc d'endurance quand elle s'y prête.

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
  **Fermeture :** binaire de référence avec 0010, E à 0 erreur sur la VM quotidienne,
  compteurs DOOM 3 relevés.

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

- [ ] **[Métrologie] Recaler les planchers de la matrice** (préalable à tout A/B) : tour complet
  sur hôte au repos, planchers réécrits d'après les ms/image mesurés.

Contexte : Sur hôte au repos, le 29/09, Marble Blast
fenêtre tourne à 8,9 ms/image et DOOM 3 fenêtre à 33,8 (contre 60,7 au tour du 27/09).

- [ ] **[Métrologie] Troisième facteur de lenteur** (`docs/tcg-g4.md` §14.7) : une partie DOOM 3
  sur huit lente de bout en bout ; cause inconnue (cœurs P/E, autre processus, thermique ?).
  Le 29/09, une charge hôte de 40–53 (autre projet, Spotlight) a doublé les ms/image :
  la matrice relève désormais la charge avant et après la fenêtre de mesure.
- [ ] **[Plugin] Nexuiz / GLSL** : chaque `glUniform` pose le bit `0x04000000` et fait recalculer
  le verdict (154 298 dispatches sur 173 572). Sans perdre la détection des samplers.
- [ ] **[TCG] Cache de sauts plus grand** (16 384 entrées, `docs/tcg-g4.md` §16.6).
- [ ] **[TCG] Verrou global à chaque `mtmsr`/`rfi`** (`ppc_maybe_interrupt`,
  `cpu_interrupt_exittb`) : chemin sans verrou quand l'état d'interruption ne change pas.
- [ ] **[Protocole] Doorbell asynchrone côté invité** (bug hunt D2) : mesure du BQL tenu par
  image sous `GPU_TRACE=1` (`docs/smp-coeurs.md` §3, levier L4).
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

Tour complet du 29/09 (`bench/matrice/20260929-1334`, commit `4fafc58`) : **images toutes
justes**, vitesses inexploitables (charge hôte 40–53). Contrôle sur hôte au repos
(`20260929-15xx`) : Marble Blast fenêtre 8,9 ms/image, DOOM 3 fenêtre 33,8, verts.

Dernier tour complet de référence : `bench/matrice/20260927-1137/tableau.md`, **13 vertes sur 15**.
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
