# TODO — priorités de POMPPC

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

- [ ] **[Outils] Endurance sur la configuration par défaut du 01/10** — banc livré le 29/09
  (`tools/endurance/`, `docs/endurance.md` : démarrages à froid, `shutdown -r`/`system_reset`
  en série, plusieurs instances sur recouvrements qcow2, panique lue dans `panicstr`/`debug_buf`,
  gel par ssh, captures et piles symbolisées, taux avec intervalle de Wilson). Il n'a jamais
  tourné sur le binaire de référence reconstruit le 01/10 (A4, 0011-0016, `QGPU_GL_FLUSH`).
  **Reste pour fermer :** une nuit sans intervention (démarrages + cycles de jeu), et une
  vraie campagne de cycles de jeu (`jeu --jeu mb|d3|…`, éprouvés sur 2 cycles seulement ;
  DOOM 3 sans redémarrage : `kCGLBadDisplay` après un kill).

- [ ] **[Système] Plantages en jeu sans cause** — trois défauts, peut-être un seul mécanisme ;
  le banc ci-dessus doit les faire sortir.
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

- [ ] **[Validation] Correctifs des bug hunts jamais éprouvés dans la VM** : `kextunload`
  avec un jeu ouvert (K4, KG4, KT1), `SUBMIT` après déchargement → erreur propre,
  `QFB=1` avec Marble Blast, GPU hôte bloqué puis rechargement du kext et
  `system_reset` (GL3, KT4, K6), `system_reset` en SMP=2 sans relance prématurée du
  CPU 1 (M1), son Tiger lecture/pause/arrêt/relance (S1–S7, Q3–Q7), plafond mémoire
  `QGPU_MEM_MB=512` sous DOOM 3 (R5), `run-all.sh` par les deux chemins
  (`docs/bug-hunt-2026-09-22.md` §11). Créneaux perdus du kext sans constante compilée :
  vérifier que KT2 (MAGIC au démarrage), KT3 et K7 (29/09) les ferment, sinon cause.
  **Fermeture :** une ligne de preuve par point dans `docs/bug-hunt-2026-09-29-passe4.md`.

## Ensuite

Dans l'ordre. Chaque entrée passe par le banc d'endurance quand elle s'y prête.

- [ ] **[TCG] `tcg/0010` : finir la preuve** (`docs/tcg-g4.md` §17, §19.3). Fait : 0010 dans le
  binaire de référence, `x-icbi-sync` allumé, `smctest` A-F puis E × 100 à 0 erreur sur une
  copie du disque de dev. **Reste :** `smctest` E sur la VM quotidienne, et les compteurs de
  `patches/tcg/essais/0010-smcstat.patch` pendant une partie de DOOM 3 (démarrage et Marble
  Blast : 0 course).

- [ ] **[Protocole] `QGPU_REG_ERRORS` par client** : aujourd'hui global, un autre
  processus fait passer le plugin en synchrone et invalide ses miroirs sans faute de sa
  part (le plancher `err_floor` du 29/09 limite l'effet, pas la cause).

- [ ] **[Architecture] Suites A6 — écarts restants de [docs/architecture.md](docs/architecture.md)** :
  L1 (quarantaine et flux coupé) et L6 (bandes et bascule ciblée) traités le 29/09 ; relire
  L2 à L5 contre le code actuel et fermer ce qui tient encore.

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
- [ ] **[Jeux] UT2004 : armes noires dans la démo de test** (plus en jeu, constaté par
  l'utilisateur le 01/10). Scène à isoler ; fermeture : image juste dans la démo.
- [ ] **[Outils] A3, suites** : Zenerchi en plein écran et Warcraft III en fenêtre (réglage à
  trouver, sinon clic par System Events) ; Colin McRae fenêtre non applicable.
- [ ] **[Outils] Rejeu : `SURF_READBACK` d'une surface jamais liée dans le vidage** → `NO_SURF`
  (DOOM 3 fenêtre, tour `20260926-2156`) : le prologue de `tests/qgpu_replay.c` ne crée la
  surface qu'au `SURF_BIND`/présentation. Épreuve : rejeu sans `NO_SURF`, image inchangée.
- [ ] **[Plugin] Coordonnées de texture en mode immédiat sous programme de sommets** : perdues
  (vu en écrivant `gltest rectfp`). Scène à écrire ; fermeture : comparé au rendu d'Apple.
- [ ] **[Backend GL] `gltest tex14` « λ=2 sans biais »** : défaut du GL de l'hôte macOS (le
  biais d'unité du dessin précédent reste appliqué ; un `glFlush` le corrige mais coûte).
  Décision : ne réappliquer que sur changement, ou passer le biais dans l'échantillonneur.

## À confirmer par l'utilisateur

Le travail est fait ; il ne manque que l'essai ou la décision de l'utilisateur.

- [ ] **[Plugin] Replis à retirer** : `POMPPC_GL_VERDICT=0`, `POMPPC_GL_WHITELIST=0`,
  `POMPPC_GL_TEXMEMO=0`, `POMPPC_GL_STSKIP=0`. **Reprise :** mesure en jeu par l'utilisateur ;
  ensuite retirer les options devenues inutiles et vérifier la matrice.
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

- [ ] **[Système] Quartz Extreme et Core Image** sur `tiger-dev.raw` : surfaces hôte pour le
  WindowServer, plus de quatre clients (`docs/roadmap-opengl15.md`).
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
| QEMU de référence | `~/src/qemu/build/qemu-system-ppc64`, reconstruit le 01/10 au soir (device A4, TCG 0010-0016, Screamer corrigé) ; précédents en `*.avant-tbfp`, `*.avant-a4` |
| Invité quotidien | `tiger.qcow2`, kext v19, plugin `20261001-tout` (gcc-4.0 dans l'invité), SMP=2 ; `POMPPCFsqrt.kext` chargé au démarrage par `/Library/StartupItems/POMPPCFsqrt` (`kHasFsqrt` : la libm prend `fsqrt` ; `sudo sh install.sh --retirer` l'enlève) |
| TCG | `0001–0004`, `0006–0008`, `0010–0012`, `0014–0016` activés par défaut (`JCBITS=14`) ; `SRTLB=0`, `LFSINLINE=0`, `VFPFAST=0`, `VPERMFAST=0`, `JITNEAR=0`, `FPINLINE=0`, `RETINLINE=0`, `JCIDX=0`, `ICBISYNC=0`, `MSRNOBQL=0`, `FPNATIVE=0`, `FPNATIVE64=0`, `TBFAST=0` les désactivent ; `0013` (`FPFLAT=1`) éteint |
| Plugin et device | `POMPPC_GL_STATEBLK`, `POMPPC_GL_RAWSANE`, `POMPPC_GL_NATSHM`, `POMPPC_GL_WLUNIF`, `POMPPC_GL_QFLUSH`, `QGPU_GL_FLUSH` allumés (`=0` éteint chacun) |
| Tests natifs et scripts | **190 OK, 0 échec, 4 ignorés** ; frontend ctest 3/3 |
| Travaux clos | Voir `CHANGELOG.md` et `docs/bug-hunt-2026-09-29*.md` |
| Profils | Plugin : `bench/plugin/ab2-B*/{d3,prey}-fen/mesure/sample.txt`, `bench/a4/depart/LISEZMOI.md` ; TCG : `bench/tcg/` (hors git) |

## Matrice de jeux

Une cellule verte exige une image juste (rejeu = VM et référence validée), zéro repli
hors rafraîchissements admis (2 par 90 images en fenêtre), et une mesure à scène fixe
sous le seuil du jeu. Une scène `gltest` comparée au rendu d'Apple éprouve chaque notion
nouvelle. Les modes fenêtre et plein écran sont requis quand le jeu les propose.

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
