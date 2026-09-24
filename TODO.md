# TODO — tableau de bord de POMPPC

Un seul fichier pour savoir **où on en est, ce qu'on fait maintenant, et dans quel ordre vient
le reste**. Il est tenu à jour à chaque lot ; l'historique des lots va dans `CHANGELOG.md`, les
raisonnements longs dans `docs/`. Règle d'écriture : une ligne par chantier, un état daté, et
**la preuve qui le fermera** (test natif, scène `gltest`, mesure, ou mot de l'utilisateur).

Anciens tableaux de bord (états datés du 17/09 au 24/09/2026, lots du bug hunt, recherches
amorcées) : `docs/archive/todo-gpu-3d-2026-09-24.md`.

---

## 0. Reprise — état au 24/09/2026, 18 h

| Quoi | État |
|---|---|
| Dépôt | `main`, chantier A1 commité (v19) |
| Protocole | **v19** (transport séparé : `qgpu_abi.h` pour le kext, `qgpu_proto.h` pour device + plugin) ; copies à jour dans `~/src/qemu/hw/display/` ; 913 tests natifs |
| QEMU hôte | reconstruit 24/09 17h40 (`~/src/qemu/build/qemu-system-ppc64`, celui que `run_tiger.sh` lance) |
| Invité quotidien (`tiger.qcow2`) | kext **v19** installé (`ioreg` : `QGPUClients = 4`, `QGPUVersion = 19`) ; plugin v19 (= `pomppc_accel.c` de c18c5f5 + transport v19) ; `~/pomppc-build/patches/qgpu/` contient les deux en-têtes ; lanceurs `~/doom3*.command`, `~/prey*.command`, `~/rtcw.command`, `~/cmr.command` ; journaux `~/d3-dump/`, `~/prey-dump/` |
| Vérifié en jeu | DOOM 3 (plugin v19, 24/09 soir, l'utilisateur) : image parfaite, 22 img/s et plus dans les scènes, 6-12 en combat, « ça devient jouable ». Prey (idem) : **image parfaite, 10-22 img/s** ; dialogue de plantage d'Apple au démarrage, puis tout va bien (§5) |
| Profils de référence | `.run/d3/sample-nat.txt` (DOOM 3 sous DRAW_NATIVE), `.run/prey/sample.txt` |
| VM | redémarrages libres autorisés par l'utilisateur |

**Premier geste à la reprise** : `pgrep -fl qemu-system` puis `.run/cmr/tssh.sh uptime` ; DOOM 3 et
Prey ont été rejoués avec le plugin v19 (24/09 soir, l'utilisateur : parfaits) ; enchaîner sur le
lot 0 de la section 2, le dialogue d'Apple de Prey (§5) ou la bissection des scènes `gltest`
cassées (§5).

---

## 1. Objectif et critère

**Que Mac OS X Tiger sous QEMU tienne le bureau et les jeux OpenGL sur le GPU de l'hôte**, avec
une matrice de jeux verte comme preuve. Un jeu est « vert » quand il a ses trois preuves :

1. **image juste** — rejeu natif du vidage identique à la VM, et scène `gltest` de chaque notion
   nouvelle comparée au rendu d'Apple ;
2. **zéro repli par image** (`fb=0` dans `frames.csv`) ;
3. **mesure** — ms/image à une scène fixe, avec un plancher par jeu.

| Jeu | Famille | Image | Replis | Vitesse (24/09) | Manque |
|---|---|---|---|---|---|
| Marble Blast Gold | pipeline fixe | juste | 0 | ~88 img/s | rien (témoin de non-régression) |
| Zenerchi | pipeline fixe (AGL) | juste | 0 | ~50 img/s | rien (témoin) |
| DOOM 3 Demo | ARB2, VBO, 7 unités, DXT | **parfaite** | 0 | 22 img/s et plus en scène, 6-12 en combat (v19) | vitesse (§2) |
| Prey Demo | ARB2, VBO, DXT5 | **parfaite** | 0 | 10-22 img/s | vitesse (§2) ; dialogue d'Apple au démarrage (§5) |
| UT2004 Demo | tableaux, VBO, S3TC | arme en main noire | 0 | ~20 img/s | `docs/re/ut2004-arme-noire.md` |
| Warcraft III | tableaux | texte des menus | ? | jouable | chemin tableaux (texte) |
| Colin McRae | ARB via IndirectX | géométrie éclatée en course | 0 | — | tableaux libérés avant `Begin` (`docs/re/programmes-arb.md` §3 ter) |
| RTCW | idTech3, pipeline fixe | — | — | — | quitte après 8 s par script, cause inconnue |
| Bureau (Quartz Extreme) | WindowServer | — | — | — | plus de 4 clients, surfaces hôte (§4, A6) |

---

## 2. En cours — verdict unique dans le plugin (décidé le 24/09/2026)

Étude : `docs/re/etude-court-circuit-glengine.md`. GLEngine ne pèse que 1,6 % du fil principal
sous `glDrawElements` ; le plugin calcule **deux fois** le même verdict (`geom_ok` +
`texture_ok` + `geom_format` au `gldUpdateDispatch`, puis au dessin : 207 des 818 échantillons),
plus des parasites. Gain estimé : 20-25 %. Chaque lot = un changement, une épreuve, un repli par
variable d'environnement tant que la mesure en jeu n'est pas faite.

- [ ] **Lot 0 — compter** : dispatches et dessins par image, dessins sans dispatch, histogramme
      des motifs du bloc de changements `gctx+0x310` (5 premiers mots). Épreuve : notes de DOOM 3
      (`demo_mars_city1`) et de Prey.
- [ ] **Lot 1 — parasites** : `pthread_self` une fois par entrée ; `getenv` de `target_probe` et
      `draw_probe` en statique ; `tex_complete` mémorisé par image. Épreuve : `gltest texup
      texcache texdelmid cube tex3d arbvp arbfp varrayvbo` inchangés ; `sample` : `__pthread_self`
      et `getenv` < 3 échantillons.
- [ ] **Lot 2 — verdict unique** : le dispatch range `ok`, `TexInfo`, `fmt`, `gs` et une clé
      (image, époque des textures, VAO, `VA_EN`, programmes, `G.state`) dans le `PCtx` ; le dessin
      reprend si la clé est identique. `POMPPC_GL_VERDICTCHECK=1` recalcule et note les écarts.
      Épreuve : zéro écart sur DOOM 3 et Prey ; `geom_ok`+`texture_ok`+`geom_format` sous
      `geom_draw_client` < 10 échantillons.
- [ ] **Lot 3 — liste blanche du bloc de changements** (après relevé R4 : quel bit pose chaque
      appel GL) : verdict gardé si seuls des bits neutres sont posés (env de programmes
      `0x00800000|0x02000000` en `+0x0c`). Épreuve : `VERDICTCHECK` étendu au dispatch, zéro
      écart ; `pomppc_geom_dispatch` divisé par deux.
- [ ] **Lot 4 — `compute_state` sauté**, seulement si R5 prouve que tout ce qu'il lit pose un bit
      du bloc (matrices en particulier). Sinon abandonner. Épreuve : `POMPPC_GL_STATECHECK=1`.
- [ ] **Lot 5 — bilan** : nouveau `sample` de DOOM 3 à la scène de `sample-nat.txt` ; l'option A
      (crocheter la table de dispatch) est classée si GLEngine reste sous 5 %.

---

## 3. Ordre de travail retenu (« C : le contrat d'abord », 24/09/2026)

Figer le contrat, en faire le harnais, puis optimiser et élargir dessous. Les étapes, avec le
chantier d'architecture (§4) qu'elles ouvrent :

| # | Étape | État | Chantier |
|---|---|---|---|
| 1 | Vitres de DOOM 3 par vidage rejoué ; v18 DRAW_NATIVE | **fait** 24/09 | — |
| 2 | Vitesse du plugin : verdict unique (§2) | en cours | A2 |
| 3 | Un seul `docs/protocole.md` pour v19 (capacités, clés, formats, tailles) à la place des notes v7…v19 | à faire | — (A1 fait) |
| 4 | Matrice de jeux automatisée (§1) : lancement, vidage à image fixe, rejeu, comparaison d'image, plancher | à faire | A3 |
| 5 | Empaquetage minimal sous contrat figé ; bloc d'état partagé lu par l'hôte ; textures par DMA sur plages sales | après 4 | A4 |
| 6 | Robustesse de session : `kCGLBadDisplay` après `killall`, créneaux perdus, kext sans constante compilée | après 4 | A1, A6 |
| 7 | Gardes de faute ramenées à la cause : chaque `faute de lecture` et niveau envoyé noir est un compteur observé à zéro sur la matrice | après 4 | A2 |
| 8 | Colin McRae, Warcraft III, UT2004 (arme noire) : restes du chemin tableaux | après 4 | — |
| 9 | Mesure honnête de la transmission paresseuse (hangar de DOOM 3, jeu à replis), décision du défaut (allumée depuis le 24/09 sur le ressenti) | quand la matrice existe | — |
| 10 | Quartz Extreme et Core Image sur `tiger-dev.raw` : surfaces hôte pour le WindowServer, plus de quatre clients | après 6 | A6 |
| 11 | 1.0 = installation reproductible : CD ou paquet, `install.sh` qui reconstruit kext et plugin, disque quotidien recréable depuis l'ISO, matrice verte à chaque commit | fin | A5 |

---

## 4. Chantiers d'architecture (revue du 24/09/2026)

Ce que la revue d'architecture a relevé, et ce qu'on en fait. Chacun a un livrable et une épreuve.

- [x] **A1 — Séparer l'ABI de transport de la sémantique GL.** **Fait le 24/09/2026, v19**
      (`docs/protocole-v19-transport.md`). `qgpu_abi.h` (transport) est le seul en-tête du kext ;
      `qgpu_proto.h` (sémantique) n'est plus copié sous `kext/`. Le device publie ses tranches
      (`QGPU_REG_CLIENTS`, table `QGPU_REG_LAYOUT`) et détruit lui-même les objets d'un client
      (`QGPU_REG_CLIENT_RESET`, en file) ; le kext lit tout dans les registres et laisse le
      plugin lire les registres (`QGPU_UC_READ_REG`). Épreuves : harnais §5 bis (kext sans aucun
      symbole sémantique, par construction : il n'inclut pas le fichier), `run_v19` natif (913
      OK), `qgpu_smoke.py` (CLIENT_RESET de bout en bout), `qgpu_test` 44/44 dans Tiger, `gltest`
      inchangé. Reste : la « clé fictive » n'a pas été jouée en jeu — la première clé réelle de
      la suite (lot 2 ou 3 de §2) le fera, sans toucher au kext.
- [ ] **A2 — Découper le plugin en modules à frontières écrites.** `pomppc_accel.c` (12 000
      lignes) mélange lecture d'état GLEngine, empaquetage, textures, programmes, gardes, vidage
      et drapeaux. Modules : lecteur d'état (une seule table d'offsets `gctx+…`, vérifiée au
      chargement par une empreinte du binaire GLEngine et une sonde), textures, géométrie,
      programmes, transport, diagnostic. Le verdict unique (§2) est la première frontière :
      *le dispatch produit, le dessin consomme*. Épreuve : mêmes scènes `gltest` à l'octet,
      mêmes `frames.csv`.
- [ ] **A3 — Matrice de jeux automatisée** (§1, étape 4). C'est le harnais qui autorise A2 et A4
      sans peur. Livrable : un script par jeu (lancement, `POMPPC_GL_DUMP_TRIGGER` à une image
      fixe, rejeu natif, comparaison avec tolérance, ms/image), et un tableau vert/rouge produit à
      chaque commit.
- [ ] **A4 — Continuer à déplacer le travail vers l'hôte.** Après DRAW_NATIVE : bloc d'état
      partagé en mémoire invité que le device lit et diffère lui-même (le PowerPC ne compare plus
      les clés une à une), textures lues par DMA sur plages sales. Épreuve : `send_state` et
      `compute_state` disparaissent du profil.
- [ ] **A5 — Configuration lue une fois, scripts versionnés.** Une structure de configuration
      remplie au chargement (plus de `getenv` dans le chemin chaud), documentée, avec purge des
      drapeaux `POMPPC_GL_*` dont le repli est mort. `.run/cmr/tssh.sh`, `cycle.sh`, `killgame.py`
      passent dans `tools/guest/` (la clé ssh reste hors dépôt).
- [ ] **A6 — `docs/architecture.md` avec les invariants** : qui possède quoi, quel côté peut
      refuser, ce qu'un client mort laisse derrière lui, cycle de vie d'un contexte. Les plantages
      non résolus (`kCGLBadDisplay`, créneaux perdus) sont des questions de cycle de vie et
      apparaîtront en l'écrivant.

---

## 5. Points ouverts (bugs, dettes, mesures à faire)

- [ ] **Scènes `gltest` cassées, antérieures à A1** (vu le 24/09 en jouant l'épreuve d'A1, plugin
      courant) : `tex3d` 4 échecs (**cassé par c18c5f5**, « paramètres mémorisés une fois par
      image » : avec le `pomppc_accel.c` d'avant ce commit sur le transport v19, 9/9) ;
      `tex13`, `tex14`, `gl15`, `texlod` : échecs puis **SIGSEGV** (rc 139), avant et après
      c18c5f5 (7, 7, 12, 1 ok avant ; 7, 4, 8, 1 après) — à bissecter sur les commits du 24/09
      (`git show <rev>:guest/gldriver/pomppc_accel.c` + transport v19, `cycle.sh NORUN=1`, une
      scène par lancement : `gltest <scène>`, les arguments suivants sont la taille). Les autres
      scènes (`tri texup texcache texdelmid cube arbvp arbfp varray varrayvbo caps blendc logicop
      polymode stipple occl sepspec spin game`) passent.

- [ ] **`kCGLBadDisplay` après un `killall` de DOOM 3** : tout lancement suivant échoue jusqu'au
      redémarrage de l'invité ; Prey et `gltest` démarrent. Cause non trouvée (état de
      l'accélérateur ou du WindowServer). Étape 6.
- [ ] **RTCW** quitte après 8 s (`~/rtcw.command`, LaunchCFMApp) sans rien dessiner ; stdout dans
      `~/rtcw-dump`. À lancer à la main d'abord.
- [ ] **Prey : dialogue de plantage d'Apple au démarrage, puis le jeu tourne** (revu le 24/09 soir
      avec le plugin v19 : image parfaite, 10-22 img/s). Cause lue dans `Prey.crash.log` (17h49) :
      `EXC_BAD_ACCESS` à `tex_lv0_sig` (`pomppc_accel.c:3176`, lecture de `d[n-1]` au-delà du
      niveau, adresse en bord de page) sous `glCopyTexSubImage2D` ← `idImage::CopyFramebuffer` ←
      `RB_STD_DrawView` ← `idCommonLocal::InitGame` : la garde `sig_jmp` est armée, mais le
      crochet n'est réarmé qu'à `PROC_Swap60` (`crash_hook_check`, ligne 984) et ce premier
      `EndFrame` de `InitGame` précède la première image — Prey a déjà remplacé nos gestionnaires,
      la faute va chez lui. **État : code écrit, à compiler et jouer dans l'invité.**
      (1) `crash_hook_fresh()` relit les gestionnaires à chaque armement de garde (`sig_jmp`,
      `pack_jmp`, `PROC_GUARD_ARM`) tant que `G.n_frames == 0`, coût nul ensuite ; le réarmement
      par image (`stats_frame`) valait déjà sans `POMPPC_GL_STATS`. (2) Aucun champ de taille en
      octets connu dans la structure de niveau : `tex_lv0_sig` ne lit plus les texels sous
      `upload_blank` ni d'une texture `host_only`, et `try_copy_tex` pose `host_only` **avant**
      l'empreinte (cohérence I1 entre COPY_TEX et dessin) ; borne resserrée à
      `(ROWPIX·(h−1)+w)·octets`, comme la copie d'`upload_texture`. Épreuve : `~/prey.command` →
      plus de dialogue d'Apple, `~/Library/Logs/CrashReporter/Prey.crash.log` ne grossit pas,
      `gltest tri cube arbvp texcache` inchangés ; DOOM 3 : vitres et effets de chaleur justes.
      Profil (`.run/prey/sample.txt`) à comparer à DOOM 3 après le lot 5.
- [ ] **UT2004, arme noire** (`docs/re/ut2004-arme-noire.md`) : trancher entre sources du
      combineur mal lues et textures 79/120 échangées entre les unités 0 et 1.
- [ ] **Colin McRae** : GLEngine déroule des tableaux déjà libérés par IndirectX ; onze
      reproductions `gltest arbvp0cmr` n'y arrivent pas.
- [ ] **Warcraft III** : texte des menus (chemin tableaux, un sommet sans couleur reste blanc).
- [ ] **`QGPU_REG_ERRORS` global au device** : un autre processus fait passer le plugin en
      synchrone sans faute de sa part. À distinguer par client.
- [ ] **Doorbell synchrone côté invité** (bug hunt D2) : à supprimer (asynchrone + barrière)
      maintenant que K5/K6 sont faits. Mesure S-M6 : BQL tenu par image sous `GPU_TRACE=1`.
- [ ] **Preuves du bug hunt non produites** (`docs/bug-hunt-2026-09-22.md` §11) : kext
      `kextunload` + `SUBMIT` → erreur propre ; `QFB=1` avec Marble Blast ; `run-all.sh` par les
      deux chemins.
- [ ] **Backend GL** : G7 `glTexSubImage*` par rectangle sale, G8 `tex_copy` par
      `glCopyTexSubImage2D` et PBO en rotation pour `SURF_PRESENT`, G9 cache d'état dans
      `gl_target`.
- [ ] **SMP** : A/B 1 vs 2 cœurs sur Marble Blast (seuil +15 %) ; panique AppleUSBOHCI au boot
      environ une fois sur dix (aléa MTTCG, `moncmd system_reset`).
- [ ] **Un seul disque, deux hôtes** (archive, lot 10) : `tiger.raw` brut partagé par USB entre
      le Mac et le PC, `devloop` et jeux sur le même disque.
- [ ] **Métrologie boot** (`docs/metrologie-boot.md`) : la baseline de 23,32 s est à refaire
      avec le harnais durci ; A/B du coût du Screamer jamais lancé ; le firmware
      `openbios-smp-screamer.elf` n'est pas reproductible (livré en binaire).

---

## 6. Règles de travail

- **Pas de repli : étendre le protocole.** Sous programme ARB, tout repli vers le rendu d'Apple
  finit dans `gleBuildInterpolateFunc` → `exit(1)` (`docs/re/glengine-exit-interpolateur.md`).
- **`qgpu_proto.h` ⇒ QEMU + plugin** (`cycle.sh NORUN=1`, sans redémarrer) ; **`qgpu_abi.h` ⇒
  QEMU + kext (`install.sh` dans l'invité, redémarrage) + plugin**, et c'est rare. Le plugin
  refuse un kext d'avant la v19 et un QEMU d'un autre `qgpu_proto.h` ; le kext refuse un device
  sans `QGPU_CAP_CLIENTS`. (A1, 24/09/2026.)
- **Mesurer avant d'optimiser** : `sample <pid> 10` dans Tiger, `frames.csv` à scène égale ; le
  self % localise, il ne valide pas (`docs/metrologie-boot.md`).
- **Un lot = un changement + une épreuve + un commit**, et une ligne ici.
- **Un seul agent sur la VM à la fois** ; les autres travaillent en copie isolée, sur le cœur et
  les tests natifs.
- **Après un `killall` de DOOM 3, redémarrer l'invité.** Avant `./run_tiger.sh` : `rm -f
  .run/tiger.lock`. Après `install.sh` en root : `sudo chown -R tiger ~/pomppc-build`.

---

## 7. Où lire la suite

| Sujet | Fichier |
|---|---|
| Historique des lots, versions du protocole | `CHANGELOG.md` |
| Architecture, rétro-ingénierie, offsets, boucle de dev | `docs/gpu-3d-tiger.md`, `docs/re/README.md` |
| Protocole (v7 → v19, un fichier par version, à fusionner à l'étape 3) | `docs/protocole-v*.md` |
| Bilan raisonné du 23/09 (jeu par jeu) | `docs/bilan-2026-09-23-jeux-tiger.md` |
| Étude « court-circuiter GLEngine ? » | `docs/re/etude-court-circuit-glengine.md` |
| Bug hunt du 22/09 (90 findings, verdicts) | `docs/bug-hunt-2026-09-22.md` |
| Feuille de route de fond (OpenGL 1.5, QE, Core Image) | `docs/roadmap-opengl15.md` |
| Références externes (kext Tiger, IOGraphics, virtio-gpu) | `docs/references-ingenierie.md` |
