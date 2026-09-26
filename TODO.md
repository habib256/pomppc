# TODO — tableau de bord de POMPPC

Un seul fichier pour savoir **ce qu'on fait maintenant et ce qui reste**, rangé **par domaine**.
Chaque domaine a trois rubriques : *En cours*, *Ensuite* (dans l'ordre), *Plus tard*. Règle
d'écriture : une ligne par chantier, un état daté, et **la preuve qui le fermera** (test natif,
scène `gltest`, mesure, ou mot de l'utilisateur). **Ce qui est fini part dans `CHANGELOG.md`**,
les raisonnements longs dans `docs/`. Un chantier nouveau va dans son domaine ; un domaine
nouveau (autre hôte, autre invité) prend une section à lui.

Anciens tableaux de bord : `docs/archive/todo-gpu-3d-2026-09-24.md` ; le TODO d'avant la
réorganisation par domaine est dans l'historique git (commit précédant celui du 25/09/2026).

---

## 0. Maintenant — état au 26/09/2026

| Chantier | Domaine | État | Preuve qui le fermera |
|---|---|---|---|
| Flottant scalaire (`fmuls`, `fmadds`, drapeaux FPSCR) | TCG (§4) | **fait, allumé par défaut le 26/09** (`tcg/0007`, `x-fp-inline`) : DOOM 3 74,4 → 65,1 ms/image (−12,5 %), `docs/tcg-g4.md` §15.9 ; binaire de référence reconstruit | **fermé** : matrice DOOM 3 + Prey verte sur le nouveau binaire (26/09, `20260926-1452`, `20260926-1333`) |
| Lots 4 et 5 du verdict unique | Plugin (§2) | à faire | R5 puis `STATECHECK=1` à zéro ; `sample` DOOM 3 comparé à `sample-nat.txt` |
| Matrice de jeux automatisée (A3) | Outils (§7) | **harnais fait** (26/09) : `tools/matrice/matrice.py`, 9 vertes sur 11 cellules automatisées, ~50 min le tour | suites : Colin McRae, Zenerchi plein écran, Warcraft III fenêtre ; déclencheur du vidage lu par image (§2) pour un tour en une passe |

**Ordre de fond** (« C : le contrat d'abord », 24/09/2026) : figer le contrat (protocole
unique, §3), en faire le harnais (A3, §7), puis optimiser et élargir dessous (A2, A4, TCG).

| Installé | État |
|---|---|
| Protocole | **v19** (`qgpu_abi.h` kext, `qgpu_proto.h` device + plugin) ; 913 tests natifs |
| QEMU de référence | `~/src/qemu/build/qemu-system-ppc64`, reconstruit le 26/09 avec `patches/tcg/0001-0004`, `0006` et `0007` (**allumés par défaut** : `SRTLB=0`, `LFSINLINE=0`, `VFPFAST=0`, `VPERMFAST=0`, `JITNEAR=0`, `FPINLINE=0` les éteignent) ; binaire précédent en `*.avant-fpinline` |
| Invité quotidien (`tiger.qcow2`) | kext v19 ; plugin **`20260924-liste`** ; lanceurs `~/doom3*.command`, `~/prey*.command` (dont `-fs` plein écran, `-env` lisant `~/lot3.env`), `~/rtcw.command`, `~/cmr.command` ; journaux `~/d3-dump/`, `~/prey-dump/` |
| Profils de référence | `.run/d3/sample-nat.txt`, `.run/prey/sample.txt` ; TCG : `bench/tcg/` (non versionné) |
| VM | redémarrages libres autorisés par l'utilisateur ; **un seul agent dessus à la fois** |

**Avant et après un lot qui touche le plugin, le device ou le cœur** : `tools/matrice/matrice.py`
(ou `-j … -m …`), tableau dans `bench/matrice/dernier/tableau.md` (`docs/matrice-jeux.md`).

**Premier geste à la reprise** : `pgrep -fl qemu-system`, `tools/guest/tssh.sh uptime`. Mesurer un
jeu **au premier plan** (sinon repli `Swap60` à chaque image) ; DOOM 3 se mesure en parties
réelles (`timedemo` refusé par la démo) ; avec `x-jit-near` (défaut) trois parties par mode
suffisent, en écartant une partie dont la cinématique est lente (§4, troisième facteur).

---

## 1. Objectif et matrice de jeux

**Que Mac OS X Tiger sous QEMU tienne le bureau et les jeux OpenGL sur le GPU de l'hôte**, avec
une matrice de jeux verte comme preuve. Un jeu est « vert » quand il a ses trois preuves,
**en fenêtre ET en plein écran** (demande de l'utilisateur, 24/09) :

1. **image juste** — rejeu natif du vidage identique à la VM, et scène `gltest` de chaque notion
   nouvelle comparée au rendu d'Apple ;
2. **zéro repli par image** (`fb=0` dans `frames.csv`) ;
3. **mesure** — ms/image à une scène fixe, avec un plancher par jeu.

Les trois preuves sont produites par `tools/matrice/matrice.py` (`docs/matrice-jeux.md`) :
rejeu natif du vidage identique à la capture de la VM (0,00 % d'écart partout au 26/09) et
rejeu de la référence validée à l'œil, replis de `frames.csv` (en fenêtre, 2 par 90 images
admis : rafraîchissement voulu de la fenêtre), ms/image sans le déclencheur de vidage.
Dernier tour : `bench/matrice/20260926-0923/tableau.md`.

| Jeu | Famille | Fenêtre | Plein écran | Vitesse à scène fixe (26/09) | Manque |
|---|---|---|---|---|---|
| Marble Blast Gold | pipeline fixe | **rouge** : 2 replis par image (fenêtre 1024×768 sous la barre de menus) | **vert** | 12,4 ms/image (`gems.mis`, plein écran) | fenêtre (§6) |
| Zenerchi | pipeline fixe (AGL) | **vert** | non automatisé | 5,2 ms/image (menu) | plein écran (§7) |
| DOOM 3 Demo | ARB2, VBO, 7 unités, DXT | **vert** | **vert** | 78,1 / 73,8 ms/image (Mars City, joueur immobile) | vitesse (§2, §4) ; changement de mode (§6) |
| Prey Demo | ARB2, VBO, DXT5 | **vert** | **vert** | 69,6 / 69,8 ms/image (« Fuite ») | vitesse |
| UT2004 Demo | tableaux, VBO, S3TC | **vert** | **vert** | 29,5 / 29,0 ms/image (intro d'AS-Convoy) | arme en main noire, hors de la scène (§6) |
| Warcraft III | tableaux | non automatisé | **vert** | 19,3 ms/image (menu) | fenêtre (§7) |
| Colin McRae | ARB via IndirectX | non automatisé | non automatisé | — | géométrie éclatée en course (§6), portage (§7) |
| RTCW | idTech3, pipeline fixe | non automatisé | non automatisé | — | absent du disque quotidien ; quitte après 8 s (§6) |
| Bureau (Quartz Extreme) | WindowServer | — | — | — | §5 |

---

## 2. Plugin GL de l'invité (`guest/gldriver/`)

Le plugin lit l'état de GLEngine, décide du verdict (hôte ou repli), empaquette et envoie.
Verdict unique : lots 0 à 3 faits (CHANGELOG, `docs/re/etude-court-circuit-glengine.md`,
`docs/re/bloc-changements-r4.md`) ; DOOM 3 100 → ~86 ms/image par le plugin seul.

**En cours / ensuite**

- [ ] **Mémoire par texture et par époque** pour `geom_texture_ok` (`texture_uploadable`,
      `intern_tex`/`find_tex`, `tex_params_ok`) et `texture_ok`, qui refont le même travail :
      c'est ce qui reste des dispatches recalculés (ceux qui lient des textures). Épreuve :
      `VERDICTCHECK=1` à zéro écart, `sample` du dispatch en baisse. Piste voisine :
      `geom_format` appelle `texturing_on` par unité (N², 22 éch.).
- [ ] **Lot 4 — `compute_state` sauté**, seulement si le relevé R5 prouve que tout ce qu'il lit
      pose un bit du bloc (matrices en particulier). Sinon abandonner. Épreuve :
      `POMPPC_GL_STATECHECK=1` à zéro.
- [ ] **Lot 5 — bilan** : nouveau `sample` de DOOM 3 à la scène de `sample-nat.txt` ; l'option A
      (crocheter la table de dispatch) est classée si GLEngine reste sous 5 %.
- [ ] **Déclencheur du vidage lu à chaque dessin** (vu par la matrice, 26/09) : tant que
      `POMPPC_GL_DUMP_TRIGGER` n'existe pas, `draw_probe` et `cube_probe` font un `access()`
      par dessin texturé (~40 µs dans l'invité) : DOOM 3 139 ms/image au lieu de 77, Marble
      Blast 36 au lieu de 12. Le lire une fois par image (dans `dump_submit` ou à l'échange).
      Épreuve : `tools/matrice/matrice.py --une-passe` donne la même vitesse que la mesure
      sans déclencheur ; alors la matrice repasse à un lancement par cellule.
- [ ] **`frames.csv` vidé toutes les 5 s** : la matrice ne peut pas s'en servir comme
      horloge (elle lit les en-têtes du vidage). Un `fflush` à l'image quand
      `POMPPC_GL_DUMP_TRIGGER` est posé suffirait. Petit, avec le précédent.
- [ ] **`VERDICTCHECK=1` sur Warcraft III, UT2004, Colin McRae** (dessins sans dispatch
      possibles, jamais exercés par DOOM 3 et Prey). Épreuve : zéro écart.
- [ ] **Replis à retirer** une fois la mesure en jeu faite par l'utilisateur :
      `POMPPC_GL_VERDICT=0`, `POMPPC_GL_WHITELIST=0`.

**Plus tard**

- [ ] **A2 — Découper le plugin en modules à frontières écrites** (`pomppc_accel.c`, 12 000
      lignes) : lecteur d'état (une seule table d'offsets `gctx+…`, vérifiée au chargement par
      une empreinte de GLEngine), textures, géométrie, programmes, transport, diagnostic. Après
      A3. Épreuve : mêmes scènes `gltest` à l'octet, mêmes `frames.csv`.
- [ ] **A5 (plugin) — Configuration lue une fois** : une structure remplie au chargement (plus
      de `getenv` dans le chemin chaud), documentée, purge des drapeaux `POMPPC_GL_*` dont le
      repli est mort.
- [ ] **Gardes de faute ramenées à la cause** : chaque `faute de lecture` et niveau envoyé noir
      devient un compteur observé à zéro sur la matrice. Après A3.
- [ ] **Transmission paresseuse** : mesure honnête (hangar de DOOM 3, jeu à replis) et décision
      du défaut (allumée depuis le 24/09 sur le ressenti). Quand la matrice existe.

---

## 3. Device QEMU et protocole qgpu (`patches/qgpu/`)

**Ensuite**

- [ ] **Un seul `docs/protocole.md` pour v19** (capacités, clés, formats, tailles) à la place
      des notes `docs/protocole-v7…v19.md`. Figé, c'est le contrat que A3 éprouve.
- [ ] **Doorbell asynchrone côté invité** (bug hunt D2) : asynchrone + barrière maintenant que
      K5/K6 sont faits. Mesure : BQL tenu par image sous `GPU_TRACE=1`. (Le rendu est déjà sur
      son fil `qgpu-render`, 7-9 % d'un cœur en jeu : `docs/smp-coeurs.md` §3, levier L4.)
- [ ] **`QGPU_REG_ERRORS` par client** : aujourd'hui global, un autre processus fait passer le
      plugin en synchrone sans faute de sa part.
- [ ] **`gltest tex14` « λ=2 sans biais »** : défaut du GL de l'hôte macOS (le biais d'unité
      du dessin précédent reste appliqué ; un `glFlush` le corrige mais coûte). Décision :
      ne réappliquer que sur changement, ou passer le biais d'unité dans l'échantillonneur.

**Plus tard**

- [ ] **A4 — Déplacer le travail vers l'hôte** : bloc d'état partagé en mémoire invité que le
      device lit et diffère lui-même, textures lues par DMA sur plages sales, empaquetage
      minimal (position en trois mots, texcoords à taille déclarée, tampons hôte réutilisés).
      Épreuve : `send_state` et `compute_state` sortent du profil.
- [ ] **Backend GL** : G7 `glTexSubImage*` par rectangle sale, G8 `tex_copy` par
      `glCopyTexSubImage2D` et PBO en rotation pour `SURF_PRESENT`, G9 cache d'état dans
      `gl_target`.

---

## 4. Processeur émulé — TCG PowerPC (`patches/tcg/`, `docs/tcg-g4.md`)

Relevés et mesures : `docs/tcg-g4.md`. Fait : `x-sr-tlb` (TLB gardé par jeu de segments,
`tlbie` global en SMP), **allumé par défaut le 25/09** (DOOM 3 médiane 89,0 → 80,1 ms/image) ;
`lfs`/`stfs` en ligne, flottant AltiVec à 4 voies, `vperm` par table (`tcg/0002-0004`),
**allumés par défaut le 25/09 au soir** (DOOM 3 à placement égal : −7,5 %, §14.7) ; tampon du
JIT gardé près du texte (`tcg/0006`, `x-jit-near`), **allumé par défaut le 25/09 à 23 h** :
supprime le régime lent (DOOM 3 ~93 → ~80 sans les patches flottants, §14) ;
`lmw`/`stmw` en ligne essayé et classé (`patches/tcg/essais/`).

**En cours**

**Ensuite**

- [ ] **Retours prédits** : chaque `blr` passe par `helper_lookup_tb_ptr` (18-21 % du temps
      vCPU sur Marble Blast). Épreuve : A/B, zéro divergence.
- [ ] **Verrou global à chaque `mtmsr`/`rfi`** (`ppc_maybe_interrupt`, `cpu_interrupt_exittb` :
      9-10 % + attente) : chemin sans verrou quand l'état d'interruption ne change pas.
- [ ] **Troisième facteur de lenteur** (`docs/tcg-g4.md` §14.7) : une partie DOOM 3 sur huit
      lente de bout en bout (95,3 contre 73,9 ms/image) avec le tampon du JIT bien placé ;
      cinématique aussi lente (témoin). Cause inconnue (cœurs P/E, autre processus,
      thermique ?). Épreuve : cause trouvée ou fréquence < 1 sur 20.
- [ ] **Flottant scalaire** (DOOM 3 : ~19 % du temps vCPU dans `helper_FMULS/FMADDS/FADDS/
      FSUBS`, `fcmpu`, `do_float_check_status` 4,5 %, `compute_fprf` 2,5 %) : **fait le
      26/09, `patches/tcg/0007` (`x-fp-inline`), allumé par défaut le 26/09 (`FPINLINE=0` l'éteint)**, docs/tcg-g4.md
      §15 : `fadds`…`fnmsubs` en un appel pur + FPRF en ligne, `fcmpu` tout en ligne, quand
      le FPSCR est amorcé sans trappe et les opérandes des simples normaux. Prouvé (hôte
      814 M vecteurs, 10 mutations détectées ; invité 30 M instructions, empreinte identique ;
      Marble Blast vérifié 3,04 milliards de passages, 0 divergence). Banc −21 à −46 % ;
      Marble Blast +5 à +8 % (bruité). **DOOM 3 joué** (§15.9, trois parties par mode,
      placement « même fenêtre » partout) : **74,4 → 65,1 ms/image (−12,5 %)** ; partie
      vérifiée 4,56 milliards de passages, 0 divergence ; aucun gel au chargement (0/7).
      Défaut tranché par l'utilisateur le 26/09 : allumé. Ops aarch64 natives dans le
      code généré : bornées, non faites (§15.7).
- [ ] **Cœurs invités** (`docs/smp-coeurs.md`) : **tranché le 26/09 — SMP=2 reste le défaut** :
      DOOM 3 deux cœurs 74,3 contre un cœur 80,8 ms/image (−8 %, à-coups des autres fils de
      Tiger sur un seul vCPU) ; Marble Blast à égalité de 1 à 4 cœurs. 3-4 cœurs possibles
      (essai `patches/smp-mac99/essais/qemu-mac99-4cpus.patch`, 1-2 jours pour un mode
      propre) mais sans gain pour les jeux, monofils. Reste : panique AppleUSBOHCI au boot
      ~1/10 en SMP=2.
- [ ] **3-4 vCPU** : Tiger démarre sur 3 et 4 (`hw.ncpu 4`) avec
      `patches/smp-mac99/essais/qemu-mac99-4cpus.patch` (GPIO 15/16 de KeyLargo) ; 0 gain sur
      les jeux, ~linéaire sur du travail parallèle dans l'invité jusqu'aux 4 cœurs P de l'hôte.
      Pas de chantier sans demande (1-2 jours pour en faire un mode ; au-delà de 4 : AppleMPIC).
- [ ] **`tlbie` en SMP stock** : défaut de QEMU 9.2 (n'atteint pas l'autre vCPU), corrigé par
      `x-sr-tlb` ; à signaler en amont.

**Plus tard**

- [ ] **Traducteur de second niveau** (`docs/plan-traducteur-rapide.md`, **mis de côté par
      l'utilisateur, 25/09**) : régions chaudes recompilées par LLVM depuis les ops TCG.
      6-10 mois de travail effectif. Première étape si repris : phase 0 (2-4 jours), go si
      ≥ 70 % du temps vCPU tient dans ≤ 1 000 régions et le plafond dépasse ×1,3 sur l'image.

---

## 5. Système Tiger et cycle de vie (kext, WindowServer)

**Ensuite**

- [ ] **A6 — `docs/architecture.md` avec les invariants** : qui possède quoi, quel côté peut
      refuser, ce qu'un client mort laisse derrière lui, cycle de vie d'un contexte.
- [ ] **`kCGLBadDisplay` après un `killall` de DOOM 3** : tout lancement suivant échoue jusqu'au
      redémarrage de l'invité ; Prey et `gltest` démarrent. Cause non trouvée. Avec A6.
- [ ] **Créneaux perdus, kext sans constante compilée** (robustesse de session). Avec A6.
- [ ] **Gel de l'invité au chargement de DOOM 3** (matrice, 26/09, 1 lancement sur ~12) :
      plus de ssh, vCPU 0 bouclant en `0x268b4` (EE coupé), vCPU 1 en `0xaf6b4`, pas de
      `panic.log` ; ensuite deux `system_reset` bloqués au démarrage (« using 1966 buffer
      headers ») : seul un QEMU relancé repart (`tools/matrice/hote.py`, `relance_qemu`).
      À symboliser (`mach_kernel`) au prochain cas ; relevé des registres dans
      `docs/matrice-jeux.md` §5.

**Plus tard**

- [ ] **Quartz Extreme et Core Image** sur `tiger-dev.raw` : surfaces hôte pour le
      WindowServer, **plus de quatre clients** (`docs/roadmap-opengl15.md`).

---

## 6. Jeux — défauts par jeu

- [ ] **DOOM 3 et Prey en plein écran** : joués le 24/09 (images justes, zéro repli). Reste le
      changement de mode par le jeu (`r_mode 3`, `CGDisplaySwitchToMode`, `SURF_PRESENT` sur
      l'écran redimensionné) et la mesure en combat en plein écran.
- [ ] **UT2004, arme noire** (`docs/re/ut2004-arme-noire.md`) : trancher entre sources du
      combineur mal lues et textures 79/120 échangées entre les unités 0 et 1.
- [ ] **Colin McRae** : GLEngine déroule des tableaux déjà libérés par IndirectX ; onze
      reproductions `gltest arbvp0cmr` n'y arrivent pas (`docs/re/programmes-arb.md` §3 ter).
- [ ] **Warcraft III** : texte des menus (chemin tableaux, un sommet sans couleur reste blanc).
      Au 26/09 le menu principal est juste (référence de la matrice validée) : à revoir en
      partie avant de fermer.
- [ ] **Marble Blast en fenêtre** (matrice, 26/09) : la fenêtre fait toujours 1024×768
      (taille du bureau, quelle que soit `$pref::Video::resolution`), la barre de menus la
      recouvre, la présentation directe est coupée : Swap60 et Swap58 repliés à chaque image,
      ~38 ms/image contre 12 en plein écran. Accepter une fenêtre partiellement hors écran
      (présentation du rectangle visible) ou trouver le réglage de taille du jeu.
- [ ] **RTCW** quitte après 8 s (`~/rtcw.command`, LaunchCFMApp) sans rien dessiner ; stdout
      dans `~/rtcw-dump`. **Absent du disque quotidien au 26/09** (`~/Desktop/Wolfenstein`
      manque) : le réinstaller d'abord.

---

## 7. Outils, mesure, frontend

**Ensuite**

- [ ] **A3, suites** (harnais fait le 26/09 : `tools/matrice/`, `docs/matrice-jeux.md`) :
      porter Colin McRae (`tools/guest/cycle.sh` : touches jusqu'en course, arrêt par le stub
      GDB) ; Zenerchi en plein écran et Warcraft III en fenêtre (réglage à trouver, sinon
      clic par System Events) ; ramener un tour à ~30 min (mesure et preuve dans le même
      lancement une fois le déclencheur corrigé, §2). Épreuve : plus de cellule « non
      automatisé » sauf RTCW absent.
- [ ] **A5 (scripts) — reste** : `tssh.sh`, `cycle.sh`, `killgame.py` sont dans
      `tools/guest/` (26/09) ; `.run/cmr/` n'a plus que des données. Reste `d3run.sh` à
      appuyer sur `tools/guest/tssh.sh`.
- [ ] **Preuves du bug hunt non produites** (`docs/bug-hunt-2026-09-22.md` §11) : kext
      `kextunload` + `SUBMIT` → erreur propre ; `QFB=1` avec Marble Blast ; `run-all.sh` par
      les deux chemins.

**Plus tard**

- [ ] **Frontend F1, restes non joués** : Tiger 1024×768 dans le frontend, la vraie touche
      Ctrl+Cmd+F au clavier, écran Retina, plusieurs moniteurs.
- [ ] **Métrologie boot** (`docs/metrologie-boot.md`) : baseline de 23,32 s à refaire avec le
      harnais durci ; A/B du coût du Screamer jamais lancé.

---

## 8. Distribution 1.0 et hôtes

**Plus tard**

- [ ] **1.0 = installation reproductible** : CD ou paquet, `install.sh` qui reconstruit kext et
      plugin, disque quotidien recréable depuis l'ISO, matrice verte à chaque commit.
- [ ] **Hôte PC x86** : construire et éprouver sur le PC (`x-fast-fp` et `x-sr-tlb` jamais
      validés sur x86 ; moins de registres, FMA3 non garanti : `docs/plan-traducteur-rapide.md`
      §1.3).
- [ ] **Un seul disque, deux hôtes** : `tiger.raw` brut partagé par USB entre le Mac et le PC,
      `devloop` et jeux sur le même disque.
- [ ] **Firmware reproductible** : `openbios-smp-screamer.elf` est livré en binaire.

---

## 9. Règles de travail

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
- **Après un `killall` de DOOM 3, redémarrer l'invité.** Avant `./run_tiger.sh` : `rm -f
  .run/tiger.lock`. Après `install.sh` en root : `sudo chown -R tiger ~/pomppc-build`.

---

## 10. Où lire la suite

| Sujet | Fichier |
|---|---|
| Ce qui est fini (lots, versions du protocole, mesures) | `CHANGELOG.md` |
| Matrice de jeux (A3) : lancer, preuves, références, pièges | `docs/matrice-jeux.md` |
| Architecture, rétro-ingénierie, offsets, boucle de dev | `docs/gpu-3d-tiger.md`, `docs/re/README.md` |
| Protocole (v7 → v19, à fusionner, §3) | `docs/protocole-v*.md` |
| Processeur émulé : relevés, patches, A/B | `docs/tcg-g4.md`, `docs/flottant-rapide.md` |
| Traducteur de second niveau (plan mis de côté) | `docs/plan-traducteur-rapide.md` |
| Bilan raisonné du 23/09 (jeu par jeu) | `docs/bilan-2026-09-23-jeux-tiger.md` |
| Étude « court-circuiter GLEngine ? » | `docs/re/etude-court-circuit-glengine.md` |
| Bug hunt du 22/09 (90 findings, verdicts) | `docs/bug-hunt-2026-09-22.md` |
| Feuille de route de fond (OpenGL 1.5, QE, Core Image) | `docs/roadmap-opengl15.md` |
| Références externes (kext Tiger, IOGraphics, virtio-gpu) | `docs/references-ingenierie.md` |
