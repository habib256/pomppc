# TODO — priorités de POMPPC

État au 27/09/2026. Objectif : Mac OS X Tiger sous QEMU, bureau et jeux OpenGL
sur le GPU de l'hôte, avec une matrice de jeux verte comme preuve.

Les chantiers sont rangés par priorité ; leur domaine figure entre crochets.
**Maintenant** contient au plus trois chantiers, **Ensuite** donne l'ordre de prise,
**Plus tard** garde les pistes différées, **Bloqué** indique ce qui manque pour reprendre.
Une entrée décrit le travail restant et la preuve qui la fermera. Pour les défauts de
rendu : scène reproduisant le défaut, image corrigée et matrice verte. Pour les
optimisations : preuve d'équivalence et mesure A/B à scène égale. Pour les défauts de
session : reproduction avant correction et contrôle après correction. Les travaux clos
sont dans [CHANGELOG.md](CHANGELOG.md), les études détaillées dans `docs/`.

L'ancien tableau par domaine est conservé dans
[docs/archive/todo-par-domaines-2026-09-27.md](docs/archive/todo-par-domaines-2026-09-27.md) :
les anciens renvois « TODO §0…§10 » se lisent dans cette archive.

## Maintenant

Contrat unifié (désormais v22, ABI v19) et rendez-vous de capture implémentés.
Le réglage des deux exports vidéo Torque a donné un premier contrôle vert de
Marble Blast, mais le contrôle ultérieur en fenêtre ci-dessous reste ouvert.
Détails et preuves dans [CHANGELOG.md](CHANGELOG.md).

- [ ] **[Jeux — session] Marble Blast en fenêtre, bureau 800×600×16** : le tour
  `bench/matrice/priorites-imgui-v22` échoue en fenêtre (surface rognée, replis,
  pas de présentation capturable), tandis que le plein écran est vert.
  Identifier le choix du mode vidéo et garantir un bureau assez grand ; un
  `CGDisplaySwitchToMode` temporaire ne suffit pas à établir une correction
  persistante. Refaire ensuite Nexuiz fenêtre/plein écran sur le bureau restauré.
  **Fermeture :** cellules vertes après démarrage frais, réglages utilisateur préservés.

- [ ] **[Outils — validation différée à la demande de l'utilisateur] Captures** :
  le rendez-vous `POMPPC_GL_CAPTURE` remplace `dump_attente`, sans élargir les tolérances.
  Premier contrôle DOOM 3/Prey fenêtre vert, image exacte consignée.
  **Reste :** dix tours consécutifs verts, explicitement reportés le 27/09.
  Ce contrôle d'endurance n'est pas remplacé par les tests ciblés
  (`docs/matrice-jeux.md`).

## Ensuite

Dans l'ordre ci-dessous. Les défauts de rendu et de session précèdent les optimisations.

- [ ] **[Outils] Rejeu : `SURF_READBACK` d'une surface jamais liée dans le vidage** → `NO_SURF` (une
      soumission en erreur, DOOM 3 fenêtre, tour `20260926-2156`, image pourtant juste) : le
      prologue de `tests/qgpu_replay.c` ne crée la surface qu'au `SURF_BIND`/présentation.
      Épreuve : rejeu du vidage concerné sans erreur `NO_SURF`, image inchangée.

- [ ] **[Outils] A3, suites** (harnais fait le 26/09 : `tools/matrice/`, `docs/matrice-jeux.md`) :
      Zenerchi en plein écran et Warcraft III en fenêtre (réglage à trouver, sinon
      clic par System Events). Épreuve : Zenerchi plein écran et Warcraft III fenêtre
      automatisés et verts ;
      Colin McRae fenêtre reste non applicable (pas de mode fenêtre).

- [ ] **[Plugin] Coordonnées de texture en mode immédiat sous programme de sommets** : perdues (vu le
      27/09 en écrivant `gltest rectfp` ; la scène passe par des tableaux). Scène à écrire ; fermeture : coordonnées correctes comparées au rendu d'Apple.

- [ ] **[Backend GL] `gltest tex14` « λ=2 sans biais »** : défaut du GL de l'hôte macOS (le biais d'unité
      du dessin précédent reste appliqué ; un `glFlush` le corrige mais coûte). Décision :
      ne réappliquer que sur changement, ou passer le biais d'unité dans l'échantillonneur.

- [ ] **[Architecture] Suites A6 — invariants documentés dans [docs/architecture.md](docs/architecture.md)** :
      fermer les écarts L1/L2 (mémoire en vol et créneau réutilisés sans fin confirmée),
      L3/L4 (arrêt bloqué ou dormeurs restants), L5 (erreurs globales) et L6
      (`SYNCED` sur transfert couleur impossible). Constats statiques, pas des causes
      établies des incidents de session. Épreuves et règles de fermeture dans le document.

- [ ] **[Système] `kCGLBadDisplay` après un `killall` de DOOM 3** : tout lancement suivant échoue jusqu'au
      redémarrage de l'invité ; Prey et `gltest` démarrent. Cause non trouvée. Avec A6.

- [ ] **[Système] Créneaux perdus, kext sans constante compilée** (robustesse de session). Avec A6.

- [ ] **[Système] Gel de l'invité au chargement de DOOM 3** (matrice, 26/09, 1 lancement sur ~12) :
      plus de ssh, vCPU 0 bouclant en `0x268b4` (EE coupé), vCPU 1 en `0xaf6b4`, pas de
      `panic.log` ; ensuite deux `system_reset` bloqués au démarrage (« using 1966 buffer
      headers ») : seul un QEMU relancé repart (`tools/matrice/hote.py`, `relance_qemu`).
      À symboliser (`mach_kernel`) au prochain cas ; relevé des registres dans
      `docs/matrice-jeux.md` §5.

- [ ] **[Système] Panique AppleUSBOHCI au démarrage SMP=2** : environ un démarrage
  sur dix (`docs/smp-coeurs.md`). Le choix SMP=2 est déjà tranché.
  **Fermeture :** cause identifiée, correction et série de démarrages sans panique.

- [ ] **[Système] Panique pendant le diagnostic UT2004 (27/09, 15:50)** : CPU 1,
  `Lock timeout`, PC `0x000AA010`, LR `0x00003820`, pile désalignée. Cause inconnue ;
  ne pas la confondre avec le défaut visuel corrigé par combine3, ni l'attribuer
  sans preuve à AppleUSBOHCI. Capture `bench/utweapon-priorites/panic.png`,
  détails dans `docs/matrice-jeux.md`. **Fermeture :** cause reproduite et correction
  vérifiée ; les lancements réussis suivants ne suffisent pas à la déclarer réparée.

- [ ] **[TCG] Code réécrit par l'autre vCPU non vu** (défaut de QEMU 9.2 en MTTCG, présent sans aucun
      patch, docs/tcg-g4.md §16.7) : `tools/guest/jobs/smctest` essai E échoue en SMP=2. Cause
      non trouvée (deux courses de `cputlb.c` refermées sans effet). Épreuve : E à 0 erreur.

- [ ] **[Protocole] `QGPU_REG_ERRORS` par client** : aujourd'hui global, un autre processus fait passer le
      plugin en synchrone sans faute de sa part.

- [ ] **[Validation] Preuves du bug hunt non produites** (`docs/bug-hunt-2026-09-22.md` §11) : kext
      `kextunload` + `SUBMIT` → erreur propre ; `QFB=1` avec Marble Blast ; `run-all.sh` par
      les deux chemins.

- [ ] **[Jeux] DOOM 3 et Prey en plein écran** : joués le 24/09 (images justes, zéro repli). Reste le
      changement de mode par le jeu (`r_mode 3`, `CGDisplaySwitchToMode`, `SURF_PRESENT` sur
      l'écran redimensionné) et la mesure en combat en plein écran.

- [ ] **[Jeux] Colin McRae, suites** (rendu vers texture fait le 27/09,
      `docs/re/cmr-rendu-vers-texture.md`) : en course la voiture roule-t-elle juste (seul le
      départ, voiture arrêtée, est prouvé) ; `POMPPC_GL_RECT=0` fait planter le jeu chez Apple
      (échantillonneur nul dans `glrPolyRGB000`) : ne pas s'en servir pour comparer.

- [ ] **[Jeux] Colin McRae — autres résolutions** (demandé le 27/09) : tester les
      modes proposés par le jeu au-delà de la résolution déjà validée, dont les
      formats 4:3 et larges disponibles. Vérifier menus, course, proportions,
      rendu vers texture, changement de mode et retour au bureau ; consigner
      résolution, profondeur, capture et temps/image. **Fermeture :** tableau
      des modes testés et anomalies éventuelles, sans supposer un mode fenêtre.

- [ ] **[Jeux] Warcraft III** : texte des menus (chemin tableaux, un sommet sans couleur reste blanc).
      Au 26/09 le menu principal est juste (référence de la matrice validée) : à revoir en
      partie avant de fermer.

- [ ] **[Plugin] Nexuiz / GLSL, vitesse** : chaque `glUniform` pose le bit `0x04000000` du bloc
      de changements et fait recalculer le verdict (154 298 dispatches sur 173 572
      dans la démo). Étudier ce coût sans perdre la détection des changements de samplers.

- [ ] **[Métrologie] Troisième facteur de lenteur** (`docs/tcg-g4.md` §14.7) : une partie DOOM 3 sur huit
      lente de bout en bout (95,3 contre 73,9 ms/image) avec le tampon du JIT bien placé ;
      cinématique aussi lente (témoin). Cause inconnue (cœurs P/E, autre processus,
      thermique ?). Épreuve : cause trouvée ou fréquence < 1 sur 20.

- [ ] **[TCG] Cache de sauts plus grand** (16 384 entrées) : sur DOOM 3 les ratés restants sont aux
      deux tiers des conflits (`docs/tcg-g4.md` §16.6). Épreuve : taux de réussite de `x-ret-verify`, A/B DOOM 3.

- [ ] **[TCG] Verrou global à chaque `mtmsr`/`rfi`** (`ppc_maybe_interrupt`, `cpu_interrupt_exittb` :
      9-10 % + attente ; au 26/09 : Marble Blast 5 % + 5 % d'attente, DOOM 3 3,5 + 3,4 %) : chemin
      sans verrou quand l'état d'interruption ne change pas.

- [ ] **[Protocole] Doorbell asynchrone côté invité** (bug hunt D2) : asynchrone + barrière maintenant que
      K5/K6 sont faits. Mesure : BQL tenu par image sous `GPU_TRACE=1`. (Le rendu est déjà sur
      son fil `qgpu-render`, 7-9 % d'un cœur en jeu : `docs/smp-coeurs.md` §3, levier L4.)

- [ ] **[Plugin] Transmission paresseuse** : mesure honnête (hangar de DOOM 3, jeu à replis) et décision
      du défaut (allumée depuis le 24/09 sur le ressenti). À mesurer avec la matrice existante.

## Plus tard

Après les priorités ci-dessus ; conserver les dépendances et mesurer avant d'optimiser.

- [ ] **[Plugin] A2 — Découper le plugin en modules à frontières écrites** (`pomppc_accel.c`) : lecteur d'état (une seule table d'offsets `gctx+…`, vérifiée au chargement par
      une empreinte de GLEngine), textures, géométrie, programmes, transport, diagnostic. Après fiabilisation de la matrice. Épreuve : mêmes scènes `gltest` à l'octet, mêmes `frames.csv`.
      Plan de découpage dans `docs/architecture.md` §10 ; premier essai retiré
      à la demande d’arrêt, validation des scènes avant/après inachevée.

- [ ] **[Plugin] A5 (plugin) — Configuration lue une fois** : une structure remplie au chargement (plus
      de `getenv` dans le chemin chaud), documentée, purge des drapeaux `POMPPC_GL_*` dont le
      repli est mort.

- [ ] **[Plugin] Gardes de faute ramenées à la cause** : chaque `faute de lecture` et niveau envoyé noir
      devient un compteur observé à zéro sur la matrice. Après fiabilisation de la matrice.

- [ ] **[Protocole] A4 — Déplacer le travail vers l'hôte** : bloc d'état partagé en mémoire invité que le
      device lit et diffère lui-même, textures lues par DMA sur plages sales, empaquetage
      minimal (position en trois mots, texcoords à taille déclarée, tampons hôte réutilisés).
      Épreuve : `send_state` et `compute_state` sortent du profil.

- [ ] **[Backend GL] Backend GL** : G7 `glTexSubImage*` par rectangle sale, G8 PBO en rotation pour
      `SURF_PRESENT` (copies surface → texture déjà en service, voir `CHANGELOG.md`), G9 cache d'état dans `gl_target`.

- [ ] **[Outils] A5 (scripts) — reste** : `tssh.sh`, `cycle.sh`, `killgame.py` sont dans
      `tools/guest/` (26/09) ; `.run/cmr/` n'a plus que des données. Reste `d3run.sh` à
      appuyer sur `tools/guest/tssh.sh`.

- [ ] **[TCG] `tlbie` en SMP stock** : défaut de QEMU 9.2 (n'atteint pas l'autre vCPU), corrigé par
      `x-sr-tlb` ; à signaler en amont.

- [ ] **[Système] Quartz Extreme et Core Image** sur `tiger-dev.raw` : surfaces hôte pour le
      WindowServer, **plus de quatre clients** (`docs/roadmap-opengl15.md`).

- [ ] **[Frontend] Frontend F1, restes non joués** : la vraie touche
      Ctrl+Cmd+F au clavier, écran Retina, plusieurs moniteurs.

- [ ] **[Métrologie] Métrologie boot** (`docs/metrologie-boot.md`) : baseline de 23,32 s à refaire avec le
      harnais durci ; A/B du coût du Screamer jamais lancé.

- [ ] **[Distribution] 1.0 = installation reproductible** : CD ou paquet, `install.sh` qui reconstruit kext et
      plugin, disque quotidien recréable depuis l'ISO, matrice verte à chaque commit.

- [ ] **[Hôtes] Hôte PC x86** : construire et éprouver sur le PC (`x-fast-fp` et `x-sr-tlb` jamais
      validés sur x86 ; moins de registres, FMA3 non garanti : `docs/plan-traducteur-rapide.md`
      §1.3).

- [ ] **[Hôtes] Un seul disque, deux hôtes** : `tiger.raw` brut partagé par USB entre le Mac et le PC,
      `devloop` et jeux sur le même disque.

- [ ] **[Distribution] Firmware reproductible** : `openbios-smp-screamer.elf` est livré en binaire.

- [ ] **[TCG] Flottant scalaire en instructions AArch64 natives** : piste bornée mais
  non réalisée (`docs/tcg-g4.md` §15.7). Le chemin `x-fp-inline` actuel est déjà
  validé et activé. Fermeture : preuve d'équivalence et gain A/B en jeu.

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

État consigné dans les rapports du 27/09 ; aucune nouvelle mesure VM lors de cette réorganisation.

| Élément | État |
|---|---|
| Protocole / ABI | GL **v21**, transport kext **v19** inchangé |
| QEMU de référence | `~/src/qemu/build/qemu-system-ppc64`, reconstruit en v21 ; précédent en `*.avant-glsl` ; contrôle `20260927-1223` |
| Invité quotidien | `tiger.qcow2`, kext v19, plugin **`20260927-glsl`**, SMP=2 |
| TCG | `0001–0004`, `0006–0008` activés par défaut ; `SRTLB=0`, `LFSINLINE=0`, `VFPFAST=0`, `VPERMFAST=0`, `JITNEAR=0`, `FPINLINE=0`, `RETINLINE=0`, `JCIDX=0` les désactivent |
| Tests natifs et scripts | **145 OK, 0 échec, 4 ignorés** : shellcheck absent et trois tests `--slow` non lancés |
| Travaux clos | Flottant scalaire, sorties indirectes, verdict lots 0–5, Colin McRae VAR/RTT, copies GPU mesurées en VM, Nexuiz GLSL : `CHANGELOG.md` |
| Profils | Plugin : `bench/plugin/ab2-B*/{d3,prey}-fen/mesure/sample.txt` ; TCG : `bench/tcg/` (hors git) |

## Matrice de jeux

Une cellule verte exige une image juste (rejeu = VM et référence validée), zéro repli
hors rafraîchissements admis (2 par 90 images en fenêtre), et une mesure à scène fixe
sous le seuil du jeu. Une scène `gltest` comparée au rendu d'Apple éprouve chaque notion
nouvelle. Les modes fenêtre et plein écran sont requis quand le jeu les propose.

Dernier tour complet : `bench/matrice/20260927-1137/tableau.md`, **13 vertes sur 15**.
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
