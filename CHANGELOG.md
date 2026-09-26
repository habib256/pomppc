# Changelog

Ce que chaque journée de travail a changé, du plus récent au plus ancien. Les versions sont
celles du **protocole qgpu** (`QGPU_PROTO_VERSION`), seul numéro que le projet porte : chaque
changement de version exige de reconstruire QEMU, le kext et le plugin. Les mesures sont celles
prises le jour même, sur l'hôte indiqué. Le détail de chaque lot est dans le message de commit
et dans `docs/`.

## Non publié

- **Verdict unique : mémoire des unités et des textures, lot 4, lot 5** (plugin
  `20260926-memo`, `docs/re/verdict-lots-4-5.md`). `POMPPC_GL_TEXMEMO` (défaut 1) : relevé
  des unités de texture fait une fois par verdict (`texturing_on`, `geom_texture_ok`,
  `texture_ok`, `geom_format` sans N²), `PTex` gardé par unité entre verdicts tant qu'aucune
  texture n'est détruite, `texture_uploadable` gardée bonne par texture tant qu'aucun crochet
  ne l'a touchée (`hook_gen`). Relevé R5 (`gltest r4`, 50 étapes `R5`) : tout ce que lit
  `compute_state` pose un bit du bloc `gctx+0x310` (il ne lit aucune matrice) ;
  `POMPPC_GL_STSKIP` (défaut 1) saute `compute_state` quand aucun dispatch n'a porté un bit
  qu'il lit (`st_mask`), et n'envoie que les clés des unités et des programmes (44 % des
  dessins de DOOM 3 et Prey). Contrôles `VERDICTCHECK=1` (lignes `TEXMEMO`) et
  `POMPPC_GL_STATECHECK=1` (lignes `STATE`) : **0 écart** sur DOOM 3, Prey, Marble Blast,
  Zenerchi, UT2004, Warcraft III ; 27 scènes `gltest` identiques. A/B entrelacé, deux
  tours : **DOOM 3 −3,5 ms/image** (64,6 → 61,2 ; 70,2 → 66,7), Prey inchangé (bruit ±2).
  Lot 5 : `pomppc_geom_dispatch` 16,5 % (`sample-nat.txt`) → 12,1 % (départ) → 10,4 % du fil
  principal, GLEngine 1,1-2,6 % : **option A classée**. Au passage : `frames.csv` vidé à
  chaque image sous `POMPPC_GL_DUMP_TRIGGER` ; `matrice.py --env K=V --sample S` ;
  `charge_hote` compte toute machine `mac99` autre que la nôtre (il comptait un shell et
  manquait la copie renommée de l'agent TCG) ; matrice complète `20260926-1756` : 9 vertes
  sur 10 automatisées (Marble Blast fenêtre, connu), images à 0,00 % ;
  `tools/re/sampleplug.py` (profil `sample` de l'invité résumé par postes du plugin),
  `tools/guest/jobs/gt5.sh`.
- **Déclencheur de vidage lu une fois par image** (plugin, 26/09) : `dump_trigger()` remplace
  les `access()` du vidage, de la sonde cube et de `draw_probe`, faits à chaque soumission ou
  dessin texturé tant que le fichier manquait (DOOM 3 77 → 139 ms/image). DOOM 3 plein écran
  déclencheur armé 64,2 ms/image contre 62,8 sans, image juste : la matrice passe à **un
  lancement par cellule** par défaut (`--deux-passes` pour un plugin plus ancien).
  Tour complet en une passe avec ce plugin (`20260926-1527`, 23 min) : **8 vertes sur 11**
  (Marble Blast plein écran 9,9 ms/image, Zenerchi 4,1, DOOM 3 63,8 / 63,6, Prey plein
  écran 72,7, UT2004 27,8 / 27,6, Warcraft III 17,3) ; rouges : Marble Blast fenêtre
  (connu), Prey fenêtre (capture hors du vidage, vert aux deux reprises, TODO §7).
- **Fin de cinématique de DOOM 3 à seuil relatif** (26/09) : avec `x-fp-inline` le jeu tourne
  à ~63 ms/image et la règle à 65 ms/image fixe ne trouvait plus la scène (matrice
  `20260926-1333` rouge). S = 0,8 × le niveau lu sur les 400 dernières images, tranches
  stables à 15 % près ; en direct (matrice, `meas-tcg.sh`) seulement après l'image 5000.
  `d3win.py` retrouve le T de ses 49 parties rangées à 15 images près ; l'A/B de
  `x-fp-inline` recalculé donne 64,8 au lieu de 65,1. Matrice DOOM 3 + Prey sur le binaire
  avec `x-fp-inline` : **4 cellules vertes** (DOOM 3 63,1 / 62,8 ms/image, Prey 72,5 / 72,1
  bruités), images justes à 0,00 %.
- **Flottant scalaire en ligne allumé par défaut** (26/09, mot de l'utilisateur) :
  `run_tiger.sh` passe `x-fp-inline=on` sauf `FPINLINE=0` ; QEMU de référence reconstruit
  avec `tcg/0007` (précédent en `*.avant-fpinline`). DOOM 3 74,4 → 65,1 ms/image.
- **A3 — matrice de jeux automatisée** (26/09, `tools/matrice/`, `docs/matrice-jeux.md`) :
  `tools/matrice/matrice.py` joue chaque jeu en fenêtre et en plein écran sur la VM
  quotidienne, deux lancements par cellule (mesure sans déclencheur, puis preuve : vidage
  déclenché, VM arrêtée pendant le vidage pour la capture, rejeu natif comparé à la capture
  et rejeu de la référence rangée hors dépôt, empreinte et validation à l'œil dans
  `tools/matrice/references.csv`), tableau vert/rouge Markdown + CSV. Premier tour
  (`bench/matrice/20260926-0923`, ~50 min) : **9 cellules vertes sur 11 automatisées** —
  Marble Blast plein écran 12,4 ms/image, Zenerchi fenêtre 5,2, DOOM 3 78,1 / 73,8, Prey
  69,6 / 69,8, UT2004 29,5 / 29,0, Warcraft III plein écran 19,3 ; rejeu = VM à 0,00 %
  partout ; rouge : Marble Blast en fenêtre (fenêtre 1024×768 sous la barre de menus, deux
  replis par image) ; non automatisés : Zenerchi plein écran, Warcraft III fenêtre,
  Colin McRae, RTCW (absent du disque). Défauts trouvés : le déclencheur de vidage coûte un
  `access()` par dessin texturé (DOOM 3 77 → 139 ms/image), `frames.csv` n'est écrit que
  toutes les 5 s, gel de l'invité au chargement de DOOM 3 suivi de démarrages bloqués
  après `system_reset` (TODO §2, §5, §6). `tests/qgpu_replay.c` corrigé (surface à la
  taille présentée, zone présentée, `TEX_CREATE` v3 et `TEX_DESTROY` au prologue) ;
  `tssh.sh`, `cycle.sh`, `killgame.py` versionnés dans `tools/guest/` (A5, clé hors dépôt) ;
  `tests/matrice_test.py` dans `run-all.sh`.
- **Flottant scalaire simple sans ses helpers** (`patches/tcg/0007`, `x-fp-inline`, **éteint
  par défaut**, `FPINLINE=1 ./run_tiger.sh` ; `docs/tcg-g4.md` §15, 26/09) : `fadds fsubs
  fmuls fmadds fmsubs fnmadds fnmsubs` passent de deux helpers sans drapeau à un appel pur
  (l'op float32 sur le FPU hôte) + FPRF/FI en ligne, `fcmpu` entièrement en ligne, quand le
  FPSCR est amorcé sans trappe, RN au plus proche, opérandes float32 nuls ou normaux ; les
  helpers d'origine sinon. Résultats et FPSCR identiques au bit près : preuve hôte contre les
  vrais objets de l'arbre (`tools/tcg/fpproof.sh`, 814 M vecteurs, 0 divergence ; 10
  mutations sur 10 détectées), invitée (`tools/guest/jobs/fptest`, 30 M instructions dans 11
  états du FPSCR, empreinte identique), mode preuve `x-fp-verify` (Marble Blast : 3,04
  milliards de passages vérifiés, 0 divergence, 99 % par le chemin court). Banc invité : chaîne
  −21 %, transformation de sommets −46 %, `fcmpu` −41 % ; Marble Blast +5 à +8 % (VM
  quotidienne chargée pendant la mesure). **DOOM 3** (§15.9, trois parties par mode,
  placement forcé) : **74,4 → 65,1 ms/image (−12,5 %)**, 13,4 → 15,4 img/s ; partie vérifiée :
  4,56 milliards de passages, 0 divergence. Défaut à trancher par l'utilisateur.
- **DOOM 3, un cœur contre deux** (26/09) : SMP=2 74,3 ms/image, SMP=1 80,8 (trois parties
  chacun) : SMP=2 reste le défaut, jeux compris (`docs/smp-coeurs.md` §4.2).
- **Combien de cœurs ? — pourquoi deux, et pourquoi plus ne rapporte rien aux jeux**
  (`docs/smp-coeurs.md`, 26/09/2026). La limite de 2 vient de notre patch QEMU (une seule
  ligne de reset câblée) : ni xnu-792.6.70 (`MAX_CPUS` 256) ni `AppleMacRISC2PE` ne bloquent ;
  AppleMPIC plafonne à 4 (un canal IPI par CPU). Essai `patches/smp-mac99/essais/qemu-mac99-4cpus.patch`
  (GPIO 15/16 de KeyLargo, offsets 0x67/0x68, firmware inchangé) : **Tiger démarre sur 3 et
  4 processeurs** (`hw.ncpu 4`). Marble Blast à placement du JIT forcé, 1/2/3/4 vCPU, trois
  démarrages entrelacés par mode : **73,4 / 72,3 / 73,0 / 72,1 img/s**, à égalité ; la somme
  des fils vCPU reste ≈ 1 cœur hôte (0,93 → 1,03), le processus entier 1,15-1,30 cœur. Dans l'invité :
  48 `gcc` en 10/7/6 s à 1/2/4 vCPU, mais un fil seul 12-17 % plus lent dès 2 vCPU (MTTCG). Leviers hôte
  classés (TCG, déport vers l'hôte, rendu `qgpu` déjà sur son fil, traduction : 0,01 % du
  vCPU). Outils `tools/tcg/smpab.sh`, `threadbusy.py`, `psmsum.py`.
- **Les deux régimes de vitesse expliqués et supprimés** (`docs/tcg-g4.md` §14) : macOS pose
  le tampon du JIT hors de la fenêtre de 4 Gio du texte de QEMU un lancement sur deux, et le
  M4 prédit plus lentement les appels de helpers qui changent de fenêtre. Patch
  `tcg/0006` (`x-jit-near`), **allumé par défaut** (`JITNEAR=0` l'éteint), QEMU de référence
  reconstruit. DOOM 3 à placement forcé : référence 79,8-80,9 ms/image, avec `tcg/0002-0004`
  73,9 (−7,5 %) ; loin : 92,4 et 79,3. Ancien pire cas → nouveau défaut : 92,4 → 73,9
  (−20 %). Une partie sur huit reste lente de bout en bout pour une autre cause (§14.7).
- **Les deux régimes de vitesse : cause trouvée** (25/09 au soir, `docs/tcg-g4.md` §14). macOS
  pose le tampon du JIT (1 Gio) hors de la fenêtre de 4 Gio du texte de QEMU un lancement sur
  deux environ (`0x300000000`) ; l'Apple M4 prédit alors plus lentement chaque appel de helper
  (+0,4 à 1,5 ns dès 8 sites, `tools/tcg/farcall.c`). Marble Blast : le placement prédit le
  régime sur 14 démarrages sur 14, le redémarrage de l'invité dans le même processus ne le
  change jamais, forcer le placement force le régime (73,3 contre 69,3 img/s, 4 sur 4 de
  chaque côté, écart intra-mode 1-2 % ; SMP=1 : 73,7 contre 68,1). Correctif `patches/tcg/0006` : propriétés de
  l'accélérateur `x-jit-near` / `x-jit-addr`, éteintes (`JITNEAR=1 ./run_tiger.sh`), et une
  ligne `tcg: tampon JIT … même/AUTRE fenêtre` au lancement. Outils `tools/tcg/regab.sh`,
  `regidx.py`, `regreport.py`, `jitwhere.sh`, job `regime`. Parties DOOM 3 de confirmation à
  jouer (§14.6).

- **`tcg/0002-0004` allumés par défaut** (25/09 au soir, demande de l'utilisateur) : `lfs`/`stfs`
  sans helper, flottant AltiVec à 4 voies, `vperm` par table ; QEMU de référence reconstruit.
  DOOM 3, six paires entrelacées : médiane 80,8 → 79,7, moyenne 84,7 → 78,0 ms/image, aucune
  régression (`docs/tcg-g4.md` §13).
- **TCG : `lfs`/`stfs` et AltiVec sans le coût des helpers** (25/09/2026, `docs/tcg-g4.md`
  §8-12), trois patches, trois propriétés de CPU éteintes par défaut (`LFSINLINE=1`,
  `VFPFAST=1`, `VPERMFAST=1 ./run_tiger.sh`), appliqués par `build_qemu_qfb.sh`.
  `tcg/0002` (`x-lfs-inline`) : DOUBLE/SINGLE de `lfs`/`stfs` en 13 ops TCG entières sans
  branchement (5,1 % du temps vCPU de DOOM 3 dans les deux helpers) ; preuve exhaustive hôte
  `tools/tcg/lfsproof.sh` (2³² float32, 2³⁵ float64, 0 divergence) et invitée
  `tools/guest/jobs/lfstest` (2³² + 2³² cas, sortie identique à l'octet) ; banc −45 %.
  `tcg/0003` (`x-vfp-fast`) : `vaddfp/vsubfp/vmaddfp/vnmsubfp` à 4 voies d'un coup sur le
  FPU hôte quand le hardfloat les aurait toutes prises (`vmaddfp` 5,1 % du temps vCPU) ;
  preuve contre le vrai softfloat `tools/tcg/vfpproof.sh` (648 M vecteurs, résultats et
  drapeaux, 0 divergence) ; banc −17 %. `tcg/0004` (`x-vperm-fast`) : `vperm` par un `tbl`
  NEON ; `tools/tcg/vpermproof.sh` (50,7 M cas, 0) ; banc −33 %. Test invité
  `tools/guest/jobs/vfptest` identique à l'octet. Essai `tcg/essais/0005` (helpers flottants
  AltiVec en `NO_RWG`) : sans gain, non appliqué. Marble Blast (10 démarrages) : gain de 0002
  **non mesurable**, chaque démarrage tombe dans un régime lent (~66 img/s) ou rapide
  (~73) quel que soit le mode. A/B DOOM 3 à faire (binaire `~/src/qemu-tcg19/build/qsr64`).
- **`x-sr-tlb` allumé par défaut** (25/09/2026, demande de l'utilisateur) : `run_tiger.sh`
  `SRTLB` vaut 1 par défaut (`SRTLB=0` pour l'éteindre) ; QEMU de référence reconstruit avec
  `patches/tcg/0001`. Preuve DOOM 3 (`docs/tcg-g4.md` §6 bis) : SMP=2, six parties par mode
  entrelacées, `demo_mars_city1` T+50..T+280, médiane **89,0 → 80,1 ms/image**. Profil
  `ppcmix` de DOOM 3 : AltiVec 5,3 % des instructions (2,8 % en helper), `lfs`/`stfs` 8,0 %
  en helper. `timedemo` impossible avec la démo (mode restreint).
- **Plan du traducteur de second niveau** (`docs/plan-traducteur-rapide.md`) : régions
  chaudes recompilées par LLVM depuis les ops TCG ; phases 0 à 6 évaluées ; mis de côté.
- **`TODO.md` réorganisé par domaine** ; les éléments finis ne vivent plus qu'ici.
- **TCG / G4 émulé** (`docs/tcg-g4.md`, 25/09/2026, Marble Blast sur copie du disque de dev) :
  relevé statique des helpers de `target/ppc` (AltiVec flottant, `vperm`, `vsldoi`, `vmrg*`,
  `lmw`/`stmw`, `sraw`, `lfs`/`stfs` en helper ; rien de changé en amont jusqu'à 11.1.1) ;
  greffon TCG `tools/tcg/ppcmix` (instructions exécutées par opcode : AltiVec 3,1 % dont
  0,1 % en helper, 213 000 `mtsrin`/s) ; `info jit` et `sample` hôte (`tools/tcg/`). **Patch
  `patches/tcg/0001-ppc-sr-tlb.patch`**, propriété de CPU `x-sr-tlb` (éteinte par défaut,
  `SRTLB=1 ./run_tiger.sh`) : un changement de registre de segment ne vide plus tout le TLB
  (~23 000 vidages complets/s → ~1 600), chaque `mmu_idx` traduit est vidé s'il sert sous un
  autre jeu de segments ; `tlbie` devient global en SMP (défaut latent de QEMU, masqué en
  stock par ces vidages) ; mode preuve `x-sr-tlb-verify=N` (SMP=1 : 936 658 825 entrées
  retraduites, 0 divergence). A/B entrelacé, même binaire : **SMP=2 +9,8 % d'img/s** (109
  paires). A/B cœurs : SMP=2 → SMP=1 −6,7 % (sous le seuil de +15 %). Essai
  `patches/tcg/essais/0002` (`lmw`/`stmw` en ligne, test invité `lmwtest` identique à
  l'octet) : −1,3 %, non appliqué. `run_tiger.sh` : `SRTLB=1`, `CPU_OPTS=…` ;
  `devloop.py` : `QEMU_EXTRA=…`. DOOM 3 en attente de la VM quotidienne
  (`tools/tcg/d3run.sh`).
- En cours : verdict unique dans le plugin (`TODO.md` §2), lots 0 à 5.
- Lot 3 du verdict unique, liste blanche du bloc de changements (plugin `20260924-liste`,
  défaut `POMPPC_GL_WHITELIST=1`) : relevé R4 — quel bit du bloc `gctx+0x310` pose chaque
  appel GL (`docs/re/bloc-changements-r4.md`, scène `gltest r4`, sonde
  `POMPPC_GL_BLOCKDUMP=a[:n]`, bits comptés sous `POMPPC_GL_COUNT=1`) ; `pomppc_geom_dispatch`
  reprend le verdict gardé quand le bloc ne porte que des bits neutres (`wl_mask`), bits de
  rastérisation admis si `geom_raster_ok` tient, tableaux salis : `va_gen_sizes` seul refait.
  `POMPPC_GL_VERDICTCHECK=1` contrôle aussi le dispatch (`VERDICT écart dispatch`). Épreuves :
  0 écart, DOOM 3 `demo_mars_city1` 54 % des dispatches court-circuités (1 396 099 sur
  2 571 457), Prey 49 % (666 569 sur 1 369 003) ; `gltest` identique à l'octet ; même binaire,
  `WHITELIST=0` contre défaut : `pomppc_geom_dispatch` 98 / 105 → 80 / 77 échantillons,
  DOOM 3 T+50..T+280 89,8 / 86,5 → 86,8 / 85,2 ms/image. Objectif « divisé par deux » non
  atteint : les dispatches qui restent recalculés sont ceux qui lient des textures, les plus
  chers.
- Lot 2 du verdict unique (plugin `20260924-verdict` ; même binaire, `POMPPC_GL_VERDICT=0`
  contre défaut : DOOM 3 Mars City, début sans bouger, 100,0 → 87,8 ms/image ; Prey, sauvegarde
  « Fuite à toute vitesse », 100 s après le chargement : 99,5 → 89,4 ms/image ;
  `POMPPC_GL_VERDICTCHECK=1` : 0 écart sur 2 421 790 dessins de DOOM 3 et 6 530 653 de Prey ;
  `gltest` identique à l'octet) : `pomppc_geom_dispatch` range le verdict (`geom_ok`,
  `texture_ok`, `geom_format`, `va_gen_sizes`) et une clé (`vd_key_of` : image, époque des
  textures `vd_epoch`, état du plugin, VAO et masques, drawable, étage de sommets, programmes)
  dans le `PCtx` ; `geom_draw_client_unsafe` le reprend si la clé est la même. `geom_format`
  n'est plus calculé qu'une fois par dispatch (`geom_publish` le reçoit). Lignes `VERDICT` dans
  la note (repris / recalculés / écarts). `POMPPC_GL_VERDICT=0` : recalcul à chaque dessin.
- Lot 1 du verdict unique, parasites (plugin `20260924-parasites` ; DOOM 3 Mars City, scène
  fixe, images 3000-3300 : 99,5 → 95,5 ms/image ; `sample` : `__pthread_self` 4 → 1,
  `getenv` 7 → 0, `tex_complete` 26 → 1 ; 18 scènes `gltest` identiques à l'octet) :
  `self_thr()` garde le fil lu par `pthread_self()` avec l'adresse de pile où il l'a été (même
  fil tant que la pile est à moins de 32 Kio) pour les gardes P3 ; `getenv` de `target_probe`,
  `draw_probe` et `cube_probe` lus une fois ; complétude des textures (`tex_complete`,
  `tex_base_ok`) mémorisée par texture et par image (`tex_cp`), effacée par les crochets.
- **Frontend F1** (`frontend/`, ImGui `v1.92.9b-docking`) : l'écran de l'invité est ancré au
  centre d'un DockSpace (Ludothèque, Journal, Bilan autour, `.run/imgui.ini`), toujours au
  ratio de l'invité avec bandes noires ; **Vue ▸ Plein écran** (Ctrl+Cmd+F, F11, Échap pour
  sortir) sur le moniteur à sa taille native ; souris mise à l'échelle sur le rectangle dessiné.
  Corrigés en chemin : contexte GL 3.0 refusé par macOS (la fenêtre ne s'ouvrait pas sur l'hôte
  Mac), `Mouse.IsAbsolute` lu une seule fois (OS 9 restait en souris relative) et souris HID
  qui reprend la main sur la tablette (`mouse_set`, **Machine ▸ Souris absolue**). Joué sur
  OS 9 : plein écran 1440×1080 entre deux bandes de 240 px sur 1920×1080, double-clics justes
  en plein écran et après retour en fenêtre.
- **Prey, dialogue d'Apple au démarrage** (plugin seul ; vérifié : Prey démarre sans dialogue,
  `Prey.crash.log` inchangé) : les gestionnaires SIGBUS/SIGSEGV sont relus à chaque armement de garde tant
  qu'aucune image n'est présentée (`crash_hook_fresh`) ; `tex_lv0_sig` ne lit plus les texels
  d'un niveau envoyé noir ni d'une texture « hôte seulement » (`host_only` posé avant
  l'empreinte dans `try_copy_tex`), et sa borne s'arrête au dernier texel de la dernière ligne.
- Lot 0 du verdict unique (plugin `20260924-count`, joué sur DOOM 3 Mars City et Prey :
  0 verdict différent sur 80 312 dessins, 0 dispatch « env seulement » — `TODO.md` §2) : `POMPPC_GL_COUNT=1` note toutes
  les 500 images des lignes `COUNT` — dispatches et dessins par image, dessins sans dispatch,
  dispatches sans dessin, verdict du dispatch comparé à celui du dessin (identique / différent),
  motifs des 5 premiers mots du bloc `gctx+0x310` et part « env seulement ».
  `pomppc_geom_dispatch` reçoit le bloc de changements (2ᵉ argument, 0 depuis `gldInitDispatch`).
- Plugin : la mémoire « paramètres synchronisés une fois par image » de c18c5f5 est invalidée
  par une empreinte du bloc de paramètres de GLEngine (`tex_prm_sig`, 15 mots, sans lire de
  texel) : un `glTexParameter` entre deux dessins d’une image ne repartait pas (`gltest tex3d`
  4 échecs, `tex14` 10, `gl15` 8, `texlod` 2). Les SIGSEGV de `tex13 tex14 gl15 texlod` étaient
  ceux de `gltest` lui-même (`px` hors du tampon) : ces scènes se jouent en `256 256`.

## 2026-09-24 (soir) — v19 : transport séparé, chantier A1

Hôte Apple Silicon.

- **Protocole v19** (`docs/protocole-v19-transport.md`) : le contrat est coupé en deux.
  `qgpu_abi.h` (transport : registres, doorbell, barrières, tranches, user client, drapeaux
  `POMPPC_SUB_*`) est le **seul** en-tête du kext ; `qgpu_proto.h` (sémantique) l'inclut et n'est
  plus copié sous `kext/`. Le device publie ses tranches (`QGPU_REG_CLIENTS`, table
  `QGPU_REG_LAYOUT` par classe d'objets) et détruit lui-même les objets d'une tranche
  (`QGPU_REG_CLIENT_RESET`, mis en file comme une soumission, requêtes comprises) ;
  `QGPU_CAP_CLIENTS` ; `QGPU_CTRL_TOPADDR` 0x50 → 0x100.
- **Kext** : lit `CLIENTS` dans les registres, exige `QGPU_CAP_CLIENTS`, ne connaît plus aucun
  opcode ni aucune plage (plus de page de service, plus de `QGPU_PROTO_MIN`) ; nouveau sélecteur
  `QGPU_UC_READ_REG` ; `POMPPC_SUB_LAYOUT` retiré. **Modifier `qgpu_proto.h` ne demande plus de
  reconstruire le kext** : la règle la plus coûteuse du projet tombe.
- **Plugin** : à l'ouverture, vérifie le kext (`READ_REG`), le device (`CAP_CLIENTS`) et la
  disposition (table = `QGPU_CLIENT_*_IDS`), dans cet ordre ; bases lues dans la table.
- **Épreuves** : harnais §5 bis (copies identiques, kext sans symbole sémantique hors
  commentaires) ; `run_v19` natif (913 OK soft + GL) ; `qgpu_smoke.py` (CLIENT_RESET depuis Open
  Firmware, OK) ; dans Tiger, kext v19 chargé (« 4 clients x 16384 KiB »), `qgpu_test` 44/44,
  `gltest tri cube arbvp varrayvbo caps texcache texup texdelmid varray blendc logicop polymode
  stipple occl sepspec spin game` OK.
- **Trouvé en passant** (pas A1, `TODO.md` §5) : `gltest tex3d` cassé par c18c5f5 ; `tex13`,
  `tex14`, `gl15`, `texlod` échouent puis plantent (SIGSEGV) avant comme après c18c5f5.
- Scripts : `install.sh`, `make_kext_iso.sh`, `build_qemu_qfb.sh` et les Makefiles invités
  transportent `qgpu_proto.h` + `qgpu_abi.h` depuis `patches/qgpu/`.

## 2026-09-24 — v18 `DRAW_NATIVE` ; DOOM 3 parfait ; étude GLEngine

Hôte Apple Silicon. 23 commits.

- **Protocole v18** : `QGPU_OP_DRAW_NATIVE` — l'hôte lit les VBO tels quels (descripteurs
  d'attributs code/tampon/offset/pas/type/taille, conversion dans le cœur, `raw_finish` commun) ;
  plugin : miroir brut des VBO dans des réserves hôte de 16 Mio, plages sales de
  `gldFlushBuffer`. DOOM 3 : 22 → 17,5 ms/image en cinématique. `docs/protocole-v18-natif.md`.
- **Génériques à taille déclarée** (`QGPU_CAP_GEN_SIZES`, clé 129) : la cinématique de DOOM 3
  passe de 43-55 à 22 ms/image. `docs/protocole-v17-generiques-tailles.md`.
- **Vitres de DOOM 3** : `COPY_TEX` produit une texture orientée comme en OpenGL (ligne 0 = bas),
  `fragment.position` retourné dans les programmes de fragments, empreinte de niveau cohérente ;
  l'utilisateur : « plus aucun bug graphique ».
- **Chemin tableaux** : empaqueteur planifié (`VaPlan`), volumes d'ombre `w=0` gardés sous
  programme, triangles fous retirés au lieu du lot entier, cache de pointeurs résolus de GLEngine
  reconnu périmé (`va_src` lit le VBO). DOOM 3 : 200 → 43 ms/image le matin.
- **Robustesse** : crochet SIGBUS/SIGSEGV (`<note>.crash`), gardes de faute `sigsetjmp` armées
  verrou tenu et par fil, réarmées à chaque image (Prey remplace les gestionnaires) ; niveaux de
  texture illisibles envoyés noirs, jamais refusés ; `POMPPC_SUB_LAYOUT` : le plugin refuse un
  kext d'un autre en-tête.
- **Textures** : verdict et paramètres mémorisés une fois par image et par texture.
- **Transmission paresseuse à Apple** des dispatches (`docs/re/dispatch-paresseux.md`), allumée
  par défaut (`POMPPC_GL_LAZYAPPLE=0` pour couper).
- **Relecture adversariale** du plugin appliquée (`docs/re/relecture-2026-09-24.md`).
- **Étude** « court-circuiter GLEngine ? » (`docs/re/etude-court-circuit-glengine.md`) : non ;
  le plugin calcule le verdict deux fois. Plan en 6 lots retenu.
- Outils : `tools/re/dumpdec.py`, `sym.py`, `frames.py`. Réorganisation de `README.md`,
  `TODO.md`, ce fichier.

## 2026-09-23 — v16 programmes ARB, v17 huit unités ; DOOM 3 et Prey tournent

- **v16** : programmes ARB de sommets et de fragments exécutés sur l'hôte (`PROG_*`, clés
  99/100, génériques `QGPU_VF_GEN`), `GL_ARB_fragment_program` annoncé. Cause de la géométrie
  éclatée de Colin McRae identifiée (attributs génériques émulés par GLEngine).
  `docs/protocole-v16-programmes.md`, `docs/re/programmes-arb.md`.
- **v17** : 8 unités de texture (clés 101-128), 16 génériques, `QGPU_MAX_TEX` 4096. DOOM 3
  Demo tourne (le repli de GLEngine mourait dans `gleBuildInterpolateFunc`) ; Prey tourne,
  image juste après l'annonce de `GL_EXT_texture_compression_s3tc`.
- **Vidage autonome et rejeu natif** (`POMPPC_GL_DUMP_TRIGGER`, `tests/qgpu_replay.c`) : le rejeu
  reproduit la VM à l'identique ; bisection par réécriture du vidage.
- UT2004 : fin des échardes (tampons de primitives entières), fin du repli total en
  rastérisation (débordement `VTX_LIMIT`) ; enquête « arme en main noire » consignée.
- Bilan raisonné : `docs/bilan-2026-09-23-jeux-tiger.md`.

## 2026-09-22 — bug hunt statique, dix lots de correctifs

Hôte Linux x86-64. Deux vagues de relecture (12 relecteurs, ~90 findings,
`docs/bug-hunt-2026-09-22.md`), puis les correctifs, un commit par lot :

- lot 0 lanceurs et build (toute capacité annoncée est sondée sur le binaire) ; lot 1 chaîne de
  verdict des outils invité (un échec rend un code non nul de bout en bout) ; lot 2 kext (plus de
  panic au déchargement, tranches jamais perdues, destructions asynchrones) ; lot 3 plugin
  (corruptions mémoire, verrouillage, `glDrawPixels` retrouvé) ; lots 4 et 6 device (attente
  bornée, scanout VGA d'abord, migration bloquée) ; lot 5 cœur (`BAD_ARG` de dessin non fatal,
  16 bits croisés, `GL_ALPHA`) ; lot 7 SMP mac99 (kick du second cœur sur son propre thread,
  PIR distinct) ; lot 8 backends (règle top-left, division par w) ; lot 9 documentation.
- Le cœur 1 vit (`hw.activecpu 2`). Plugin : 4 unités annoncées, mouchard `exit()`.
- RE : `exit(1)` de GLEngine dans `_gleBuildInterpolateFunc`, cause du « retour au bureau ».

## 2026-09-21 — v15 tampons 16 bits ; présentation hôte réelle

- **v15** : drawables RGB1555 et profondeur UNORM16 convertis par l'hôte ; A/B `gltest game` en
  16 bits : 48 → 1177 img/s. `docs/protocole-v15-xfer16.md`.
- La v13 était inerte dans la configuration réelle (pas de `qfb-pci` en quotidienne) :
  `SURF_PRESENT` se rabat sur la VRAM du VGA ; ~10 % en jeu. Critère « aucun texel fabriqué
  par l'invité » tenu en fenêtre.
- Sondes de géométrie et de textures pour le débogage.

## 2026-09-20 — v13, v14 ; tableaux de sommets ; UT2004 premier passage

- **v13** `SURF_PRESENT` + `COPY_TEX` ; **v14** tampons hôte (`BUF_*`, `DRAW_RAW_BUF`) ;
  pixels 2.6 (`DrawPixels`/`CopyPixels`). `docs/protocole-v13-present.md`, `-v14-tampons.md`.
- Chemin tableaux « à la GeForce3 » (`RenderVertexArray`) ; plein écran 16 bits (Warcraft III,
  Colin McRae).
- UT2004 Demo : image juste en fenêtre, trop lente ; banc `flyby` et cache de textures.
- Le critère du TODO devient « UT2004 Mac PPC à fond », 1.5 n'est plus le but.

## 2026-09-19 — OpenGL 1.2 à 1.5 annoncés et tenus ; flottant rapide ; Internet

- **v10** textures 1.2-1.5 côté hôte (3D, cube, bordure, compression, rectangle), couleur
  secondaire, points ; **v11** spéculaire séparée ; **v12** crossbar. `GL_VERSION` 1.5 et 42
  extensions annoncés parce que tenus. `docs/protocole-v10…v12`, `docs/re/version-extensions.md`.
- **Flottant rapide** (`x-fast-fp`, `patches/fastfp/`, `FASTFP=1` par défaut) : résultats
  identiques au bit près, ×2 à ×3 sur les noyaux flottants, Marble Blast +20 %, Zenerchi −12 s.
  `docs/flottant-rapide.md`.
- **Accélérateur IOKit** publié : le plugin est chargé comme le pilote d'une carte
  (`docs/re/accelerateur-iokit.md`).
- VM de dev sous Linux (RTX 4060 Ti, EGL) ; CD du pilote installable sans Xcode Tools.
- Relais web : modes sans JS / rendu par Chrome / auto, images converties, adaptateurs.

## 2026-09-18 — v6 à v9 : stencil, géométrie sur l'hôte, pipeline fixe, asynchrone

- **v6** stencil de bout en bout, présentation directe en fenêtre.
- **v7** géométrie brute sur l'hôte : matrices, éclairage, texgen, découpe, brouillard, `DRAW_RAW`
  indexé. Marble Blast 24-42 → 48-63 img/s. Fusion des dessins consécutifs en triangles indexés
  (969 → 85 dessins par image, +11 à +23 %). `docs/protocole-v7-geometrie.md`.
- **v8** ce qui manquait au pipeline fixe : mélange constant, min/max, opérations logiques,
  modes de polygone, pointillés, requêtes d'occlusion. `docs/protocole-v8-pipeline-fixe.md`.
- **v9** doorbell asynchrone : thread de rendu hôte, l'invité prépare l'image suivante pendant
  que l'hôte dessine (soumission 1,6 → 0,06 ms). `docs/protocole-v9-asynchrone.md`.
- État T&L de GLEngine relevé offset par offset (`docs/re/etat-tcl.md`), descripteur de sortie de
  sommet vérifié. Premier passage sur l'hôte Linux + NVIDIA.

## 2026-09-17 — GPU 3D paravirtuel v5 ; Internet sous Tiger

- **`qgpu-pci`** (device QEMU, cœur, backends logiciel et OpenGL), **kext `POMPPCGPU`**
  (transport, 4 clients), **plugin `GLDriver-POMPPC`** (chargé par GLEngine, mandataire du rendu
  d'Apple) : couloir multitexture 3 → 574 img/s. **v5** : 4 unités, `GL_COMBINE`, présentation
  directe ; deux jeux jouables (Marble Blast, Zenerchi). `docs/gpu-3d-tiger.md`.
- Réseau par défaut et relais HTTPS→HTTP (`scripts/web-proxy.py`).
- Feuille de route OpenGL 1.5 (`docs/roadmap-opengl15.md`) ; TODO réorganisé.

## 2026-08-22 — Screamer vendu, harnais, métrologie

- Device audio Screamer dans le dépôt (`patches/screamer/`), build reproductible avec
  vérification de capacités ; lanceurs qui sondent le binaire au lieu de supposer.
- Harnais `tests/run-all.sh` (syntaxe, shellcheck, doc ↔ binaire, registres QFB).
- Métrologie durcie : pré-vol charge, `-pidfile`, `trap`, `ab-measure.sh`.
- QFB : un scanout débordant ne fait plus tomber l'hôte ; frontend corrigé.

## 2026-08-18 → 19 — QFB ; cohérence doc/code

- **Écran paravirtuel QFB** : device `qfb-pci` + kext `POMPPCQFB` (portage PCI/PowerPC du qfb1
  de Solra Bizna).
- Deux passes de nettoyage de cohérence entre la doc, les sources, les patches et les pilotes
  tiers ; le build du dépôt ne reproduit pas la moitié firmware du SMP (signalé).

## 2026-07-20 — création

- Base QEMU/TCG `mac99`, scripts de baseline (disque, install, boot, mesure, profil), SMP mac99
  (d'après la série de BALATON Zoltan), frontend ImGui, lanceurs Tiger et OS 9. Baseline de boot :
  23,32 s CPU QEMU (Linux x86-64).
