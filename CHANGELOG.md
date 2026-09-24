# Changelog

Ce que chaque journée de travail a changé, du plus récent au plus ancien. Les versions sont
celles du **protocole qgpu** (`QGPU_PROTO_VERSION`), seul numéro que le projet porte : chaque
changement de version exige de reconstruire QEMU, le kext et le plugin. Les mesures sont celles
prises le jour même, sur l'hôte indiqué. Le détail de chaque lot est dans le message de commit
et dans `docs/`.

## Non publié

- En cours : verdict unique dans le plugin (`TODO.md` §2), lots 0 à 5.
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
