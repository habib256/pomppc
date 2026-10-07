# Changelog

Ce que chaque version publiée de POMPPC a changé, de la plus récente à la plus ancienne
(publications : https://github.com/habib256/pomppc/releases). Le **protocole qgpu**
(`QGPU_PROTO_VERSION`, v22 en 0.3) a son propre numéro : chaque changement sémantique exige de
reconstruire QEMU et le plugin ; le kext ne change que si l'ABI de transport change. Les mesures
sont celles prises le jour même, sur l'hôte indiqué. Le détail de chaque lot est dans le message
de commit et dans `docs/`.

## Non publié

- **`glMaterial` entre `glBegin` et `glEnd` : le « T&L perdu » de GLEngine honoré, 07/10**
  (`docs/re/opengl-1.4.md` §3.3) : `_glMaterial*_Exec` appelle `_gleForceToSoftwareTCL`, qui
  pose `0x10000000` dans le mot `+0x0c` du bloc de changements puis appelle le dispatch ; le
  plugin rendait toujours « T&L gardé », GLEngine relançait notre `BeginPrimitiveBuffer` avec
  son curseur dans son propre tampon (primitive perdue) et l'objet matériau de `gctx` ne bougeait
  plus (primitives suivantes à l'ancien matériau, même après un `glMaterial` hors `glBegin`).
  Le dispatch qui porte ce bit rend désormais le pilote sans T&L, au pipeline fixe seulement
  (pas sous programme : `_gleBuildInterpolateFunc` → `exit`) : la primitive est éclairée par
  GLEngine et rastérisée par le chemin hérité ; le dispatch suivant rend le T&L.
  `POMPPC_GL_TCLLOST=0` pour l'A/B. Nouvelle scène `gltest matsonde` (matériau avant le premier
  sommet, au milieu d'une liste, primitive suivante, hors `glBegin`). Job `gpu` **56 OK, 0 échec**
  (`matbegin` et `matsonde` ajoutées, `pixstore` comprise) ; `gtgeo.sh` : `matbegin` identique
  à l'octet au rendu d'Apple dans les quatre modes, toutes les autres scènes au même md5
  qu'avant ; `POMPPC_GL_RDIRTY=0` : `tcprobe` passe aussi. `tests/run-all.sh` 239 OK, 0 échec,
  7 ignorés (aucun code hôte touché).
- **Binaires, paquet invité et matrice après la nuit du 06-07/10, PC, 07/10 au matin**
  (`docs/binaire-rapide-x86.md` §6, `docs/matrice-jeux.md` §6 quater) :
  `disks/prebuilt` régénéré par le job `prebuilt` sur une copie de la VM de dev (plugin de la
  nuit : erreurs par client, `vpimm`, garde `unpack_trusted` ; `gltest` à 256×256 et `vpimm` ;
  kext POMPPCGPU recompilé, octets identiques ; ancien en `bench/devloop/prebuilt-20261004`) ;
  job `gpu` avant reconstruction 52 OK, 1 échec (`vpimm`, 7 témoins : device du 06/10, attendu).
  Paquet `disks/pomppc-guest-20261007.iso` (`scripts/make_guest_iso.sh`) installé dans la VM
  quotidienne par son `install.sh` (ancien plugin en `~/pomppc-sauvegarde-20261007`) : kext
  `caps 0xefffe`, plugin chargé (`plugin 20261004-d3x qgpu v22` : la chaîne de révision n'a pas
  été montée cette nuit), `gltest vpimm` 26/26 dans la session. QEMU reconstruit
  (`QEMU_FAST=1`, `build/` et `build-fast/`, anciens en `*.avant-0707`) : série
  `3d7534b0f2217d54`, le lanceur reprend le binaire rapide ; `tests/run-all.sh` 239 OK,
  1 échec (binaire rapide écarté), 5 ignorés → **240 OK, 0 échec, 5 ignorés** (deux tests du
  frontend désormais construits). Tour `bench/matrice/20261007-0709` avec vidage : Marble Blast
  15,5 / 15,2, Zenerchi 7,4, UT2004 48,8 / 48,5 vertes, images justes ; DOOM 3 123,4 / 121,5
  rouge faute de référence validée (`d3-fen`, et `d3-pe` créée par ce tour dans
  `references-linux.csv`, `validee=non`), images rejeu = VM (0,03 et 0,04). Vitesse peu probante
  (un QEMU d'un autre agent sur l'hôte) ; `POMPPC_GL_VPIMM=0` ne change pas DOOM 3 (118,6
  contre 119,4 sans vidage).
- **État de `glPixelStorei` : la base était fausse, pas les offsets ; garde remplacée par une
  vérification, 07/10** (`docs/re/pixelstore.md`, `docs/backend-gl-unites-fixes.md`, « Cause :
  la mauvaise base ») : le plugin lisait `CTX_UNPACK_*` (`+0x31cc..0x31e5`) sur le contexte du
  GLDriver (un bloc de 0x800 octets) au lieu du contexte de GLEngine (`gctx = GS − 0x360`) :
  lecture ~10 Kio hors du bloc, d'où les 0 de 10.4.11, le 4 du 22/09 et le plantage de
  `try_draw_pixels` dans la session bureau. Relevé par diff de vidages (scène `gltest
  pixsonde`) : `GC_PACK_*` en `gctx+0x31b0..0x31c9`, `GC_UNPACK_*` en `gctx+0x31cc..0x31e5`,
  GLEngine identique en 10.4.6 et 10.4.11. `pixstore_read()` remplace `unpack_trusted()`
  (cohérence seulement, compteur `POMPPC_GL_STATS`, 0 relevé) ; restrictions aux lignes
  serrées levées pour `DrawPixels`, `Bitmap` et stencil, `ReadPixels` suit `GL_PACK_*` ;
  couleur raster de `glBitmap` lue sur `gctx`. Nouvelle scène `pixstore` (64 témoins, dans le
  job `gpu`) : verte, écart 0 à Apple, fausse avec le plugin d'avant ; job `gpu` 54/54,
  8 × `mixte`/`v15` par le relais sans plantage ; job `tools/guest/jobs/pixgui`.
- **Correctifs des bug hunts éprouvés dans la VM quotidienne, PC, 07/10**
  (`docs/bug-hunt-2026-09-29-passe4.md`, dernière section) : `kextunload` avec Marble Blast
  et un client tenu ouvert (K4, KG4, KT1 : dormeur réveillé en 0,9 s, aucune panique, le
  jeu repasse en rendu d'Apple), appels après déchargement refusés proprement
  (`kIOReturnBadArgument` par IOKit sous 10.4.11), rechargement et jeu relancé en
  asynchrone sans `LIMIT` (KT2), aucune tranche perdue après rafales et `kill -9`, `QFB=1`
  avec Marble Blast (614 présentations sur VGA), `system_reset` SMP=2 (CPU 1 parqué jusqu'à
  l'entrée du CPU 0 dans le noyau), son lecture/pause/arrêt/relance (silence numérique
  exact en pause, avant et après `system_reset`), `run-all.sh` rapide et `--slow`.
  **Échoué** : DOOM 3 sous `QGPU_MEM_MB=128` (201 `NO_MEM` propres, puis `exit()` de
  GLEngine dans le repli logiciel) ; à 512 Mio aucun refus n'arrive. **Non éprouvable** :
  GPU hôte bloqué (pas de propriété de test au device). Nouvel outil invité
  `guest/qgpu-test/unloadpeer`.
- **`tcg/0010` (code réécrit par l'autre vCPU) : preuve close, PC, 07/10**
  (`docs/tcg-g4.md` §17.4, §17.5) : `smctest` E × 100 sur la VM quotidienne (10.4.11,
  session bureau, SMP=2, binaire de référence) : 100 fois l'empreinte `aa4d72ebdba6b6eb`,
  0 erreur, 189 s ; A-F une fois, empreintes du §17.4. Compteurs de
  `patches/tcg/essais/0010-smcstat.patch` (porté sur l'arbre du 02/10, relevé périodique
  horodaté) pendant une partie de DOOM 3 (cellule `d3-fen`, 19 min, ~15 min dans le
  niveau) : **0 course** au démarrage, au chargement et en jeu (34 protections de page en
  jeu, 4,2 M `icbi` tous par le chemin court). Les 5 « courses 2 » relevées après la partie
  sont des bits `VGA` effacés par l'affichage, pas du code (compteur trop large, noté).
  Aucun changement de code livré.
- **`glDrawPixels` : plus de lecture guidée par des mots d'état non prouvés, 07/10**
  (`docs/backend-gl-unites-fixes.md`, « Scènes gltest sur le PC ») : sur la VM 10.4.11 du PC,
  dans la session de l'utilisateur, `try_draw_pixels` a planté trois fois (`EXC_BAD_ACCESS`
  dans `memcpy`, scènes `mixte` et `v15`) : les mots `CTX_UNPACK_*` (relevés sur 10.4.6, déjà
  pris en défaut le 22/09) valent 0 à chaque appel relevé — `ALIGNMENT` compris, dont le défaut
  GL est 4 — et ont valu autre chose ces trois fois-là, d'où une adresse source à
  `pixels + 0x5d410000`. Le plugin n'accepte plus le chemin hôte de `DrawPixels` (couleur,
  profondeur, stencil) et de `Bitmap` que si ces mots ont leur valeur par défaut ; sinon rendu
  logiciel, exact. `gltest` : les neuf scènes dont les témoins sortent de 64×64 se jouent en
  256×256 sans taille donnée, et la scène `stencil` demande toujours son tampon de stencil
  (neuf faux rouges d'une passe de nuit). Job `gpu` 52/53 (seul `vpimm`, qui attend un QEMU
  reconstruit) ; même verdict en session bureau, par l'agent racine et par le relais, sans
  plantage du plugin. Plugin à réinstaller ; le `gltest` précompilé (`disks/prebuilt`) est à
  régénérer.
- **Mode immédiat sous programme de sommets : coordonnées de texture et couleurs, 07/10**
  (`docs/protocole-v16-programmes.md` §7) : `glTexCoord`/`glMultiTexCoord`/`glColor` entre
  `glBegin` et `glEnd` sous un programme ARB se perdaient (quad entier au texel (0,0), à la
  couleur courante de `glBegin`) : `geom_format` ne portait, sous programme, un attribut
  conventionnel que si son **tableau** était actif — jamais en mode immédiat — et
  `vertex.texcoord[u]` d'une unité sans texture n'était porté (plugin) ni lié (hôte) que
  sous GLSL. Le texte du programme décide désormais seul quand il dit ce qu'il lit
  (`POMPPC_GL_VPIMM=0` : ancien filtre) ; la crainte des pointeurs résolus périmés (23/09)
  est réfutée sur le chemin des tableaux. Nouvelle scène **`gltest vpimm`** (26 témoins,
  dans le job `gpu`) : 24 faux avant, 26/26 et identique au rendu d'Apple après ; job `gpu`
  53 OK, 0 échec ; `tests/run-all.sh` inchangé (237 OK, 1 échec binaire rapide, 7 ignorés).
  **QEMU à reconstruire** (`patches/qgpu/qgpu-gl.c`) et plugin à réinstaller.
- **UT2004 : armes noires dans la démo de test, PC NVIDIA, 07/10**
  (`docs/re/ut2004-arme-noire.md`, `docs/protocole.md` « Combineurs ATI ») : le backend GL
  n'annonçait `QGPU_CAP_COMBINE3` qu'avec `GL_ATI_texture_env_combine3`, absent chez
  NVIDIA ; UT2004 prenait alors son repli `REPLACE(PREVIOUS)` et les présentoirs d'armes de
  la démo (images ~166-195) sortaient noirs. Le backend l'annonce aussi avec
  `GL_NV_texture_env_combine4` et y traduit les fonctions ATI et les sources ZERO/ONE
  (exact pour MODULATE_ADD et MODULATE_SIGNED_ADD, MODULATE_SUBTRACT approché). Démo
  entière revidée sur la VM quotidienne avec un QEMU reconstruit : armes texturées, rejeu
  `gl` = `soft`, cellule `ut-fen` juste, `gltest` sans régression. Rejoueur :
  `QGPU_REPLAY_STATE`, `QGPU_REPLAY_TEXOUT`, `QGPU_REPLAY_UNUSED`. Reste le M4, qui a
  l'extension ATI et où l'utilisateur avait vu le défaut le 01/10.

- **`tests/run-all.sh` sur le PC : deux échecs hors binaire rapide levés, 07/10**
  (`docs/backend-gl-unites-fixes.md` « Test natif », `docs/audio-stabilite.md`) :
  `qgpu_core_test` v17 (a)/(b) exigeait les unités 5 et 6 **au pipeline fixe** du backend GL,
  que `QGPU_CAP_FIXED4` (NVIDIA, 02/10) ne promet plus et que le plugin n'envoie pas (rendu
  d'Apple) ; sous `FIXED4`, ils sont refaits sous programme de fragments (mêmes pixels, obtenus
  sur la RTX 4060 Ti), le backend logiciel garde le pipeline fixe. `screamer_audio_test.py` ne
  compilait plus depuis le portage à QEMU 11.1.2 (02/10 : `audio_be_write`,
  `audio_be_set_volume_out_lr`, membre `audio_be`) ; ses bouchons suivent le device et
  vérifient que PCM et volume vont au backend du device. PC : 235 OK, 3 échecs, 7 ignorés →
  **237 OK, 1 échec (binaire rapide écarté), 7 ignorés**.
- **Architecture, suites A6 : L2 à L5 relues contre le code, 07/10**
  (`docs/architecture.md` §5-§8) : **L3** (attentes non bornées du reset et de l'arrêt du
  thread) et **L4** (arrêt du kext avec dormeurs) sont **fermées dans le code** par les
  correctifs du 29/09 (GL3, KT5 ; KG4, KT1) — seul reste non borné le drainage du retrait du
  device, inatteignable (`hotpluggable = false`) ; leurs épreuves en VM restent dans l'entrée
  « Validation ». **L2** (créneau rendu malgré un nettoyage échoué) et **L5** (verdict global)
  tiennent : correction du kext et ABI par tranche décrites, avec leurs épreuves. Nouveau test
  natif `run_a6` (`tests/qgpu_core_test.c`) : ce que le cœur fait d'un nettoyage manqué
  (`QGPU_ST_LIMIT` fatal), retardé (sans effet, FIFO) ou rejoué après réattribution.
- **Rejeu : surface lue sans être liée dans le vidage, 07/10** (`docs/matrice-jeux.md` §5) :
  `tests/qgpu_replay.c` ne créait une surface d'avant le vidage qu'au `SURF_BIND` ou d'après
  la présentation ; un `SURF_READBACK`/`UPLOAD` (ou `DEPTH_*`, `STENCIL_*`, `SURF_TEX`) d'une
  surface jamais liée donnait `NO_SURF` (DOOM 3 fenêtre, tour `20260926-2156` du M4, vidage
  sans `surfaces.txt`). Le prologue la crée comme le device l'avait (relue noire si jamais
  dessinée) et relie le contexte à la surface de ses transferts quand sa liaison était
  devinée. DOOM 3 fenêtre du PC sans `surfaces.txt` : 2 `NO_SURF` → 0, 20 images identiques
  au rejeu avec `surfaces.txt` ; rejeux des quatre cellules du PC (avec `surfaces.txt`)
  inchangés à l'octet.
- **`QGPU_REG_ERRORS` par client, 07/10** (`docs/protocole-v19-transport.md`, « Erreurs par
  client ») : le compteur d'erreurs du device était global, et la sonde attendue ou le flux fautif
  d'un **autre** processus faisait repasser le plugin en synchrone 120 images, miroirs invalidés.
  Le device tient maintenant un compteur par tranche de BAR0 (`QGPU_CAP_CLIENT_ERRORS`,
  `QGPU_REG_CLIENT_ERRORS(i)` en 0xC8 + 4·i), attribué par l'offset du flux ; le plugin garde le
  global comme déclencheur et ne relit le sien (`QGPU_UC_READ_REG`) que quand il bouge. Ni version
  du protocole ni `qgpu_abi.h` : **QEMU et plugin, kext inchangé**. Épreuve à deux clients dans
  l'invité (`tools/guest/jobs/regerr`, client fautif `guest/qgpu-test/errpeer` toutes les 10 ms
  pendant `gltest game`) : plugin d'avant **1 repli, fin en synchrone** ; d'après **0 repli, 71
  mouvements étrangers ignorés**. `gltest` 52 OK / 0 échec, `qgpu_smoke.py` 61/61 (dont l'attente
  périmée de 4 contextes par client, 32 depuis le 26/09, corrigée).
- **Binaire rapide du PC mesuré hôte au repos, matrice finale, planchers du PC, 06-07/10**
  (`docs/vitesse-doom3-x86.md` §13.5-13.7) : A/B entrelacé DOOM 3 `d3-fen`, 3 parties par bras,
  défauts du 06/10 : binaire de référence **122,9** ms/image (121,8..124,6), **binaire rapide
  111,5 (108,9..112,9), −9,3 %**. Matrice avec vidage, anciens défauts du 04/10 sur le binaire
  de référence contre binaire rapide et nouveaux défauts : **Marble Blast 20,9 → 14,9 (−29 %),
  Zenerchi 7,6 → 5,9 (−22 %), UT2004 58,1 → 48,7 (−16 %), DOOM 3 136,3 → 111,9 (−18 %)**,
  images justes (rejeu = VM et référence 0,00/0,00 % ; DOOM 3 0,03/0,00 % contre la VM dans
  les deux bras, comme le 04/10). Planchers `plancher_ms_linux` : **DOOM 3 140** (il prenait
  les 76 du M4), **Marble Blast 26 → 19**, **Zenerchi 10 → 8** ; UT2004 garde 104 (une partie).
  L'autre session qui avait pollué le premier essai travaillait encore : une barrière avant
  chaque partie (`MATAB_AVANT` de `matab.sh`, `tools/tcg/chargehote.py attendre`) et un relevé
  de la charge étrangère toutes les 5 s (`chargehote.py releve`, `chargeparties.py`) ont tenu
  ses rafales de 10 à 13 cœurs hors des 8 parties, toutes valides. La référence `d3-fen` du
  PC attend la validation à l'œil de l'utilisateur.
- **Le cas « sans traduction » de `x-ret-verify` expliqué, 06/10** (`docs/tcg-g4.md` §35) :
  ce n'est pas le noyau mais **BootX sous OpenBIOS**. Quand un PTEG est plein, le gestionnaire
  de fautes d'OpenBIOS (`hash_page`, `tlbie` en `0xfff08a5c`) évince une PTE et ne fait
  `tlbie` que de la page qui entre ; `x-tlb-precise` retire bien la classe du TLB mais garde
  l'entrée du cache de sauts de la page évincée, que l'architecture permet de garder. Prouvé
  par une instrumentation (`patches/tcg/essais/0031-retv-diag.patch`, non appliquée) et par
  les deux bras : 768 Mo, 4 cas à chaque démarrage SMP=2 (1 en SMP=1) avec `x-tlb-precise`,
  **0 sans**. Sans gravité : les traductions d'OpenBIOS ne changent jamais, le bloc exécuté
  est celui que l'ISI ferait retrouver. Correction du §32.5 : un 7400 ne garderait pas cette
  traduction (même classe), il prendrait l'ISI. Aucun patch de la série modifié.
  `tools/tcg/retvboot.sh` démarre la VM de dev en `-snapshot` pour ces essais.
- **Bilan de la phase 2 sur le PC, 06/10** (`docs/vitesse-doom3-x86.md` §13) : une partie
  DOOM 3 sous **tous** les vérificateurs (TLB précis, `lmw`/`stmw` et `dcbz` en ligne, flottant
  et AltiVec natifs avec leurs comparaisons, sorties) : 0 divergence sur 2,9 G accès
  retraduits, 2,2 G `lmw`/`stmw`, 837 M `dcbz`, 810 M opérations flottantes et 2,9 G
  vectorielles ; `x-ret-verify` relève 2 cas « sans traduction » sur 12,8 G sorties, au même
  pc du noyau (`0x5605d88`), au démarrage et au redémarrage de l'invité seulement (à
  comprendre). A/B entrelacés, 3 parties par bras, binaire de référence : le côté mémoire
  (`TLBPRECISE LMWINLINE DCBZINLINE`) fait **DOOM 3 144,3 → 132,0 ms/image (−8,5 %)**, **Marble
  Blast 20,9 → 16,5 (−21 %)**, **UT2004 58,9 → 54,7 (−7 %)** (aucune rafale dans la fenêtre) ;
  par-dessus, `JITREL32 FPNATIVECMP VFPNATIVECMP LMWVEC JCWORD` ensemble **−2,6 %** sur DOOM 3.
  **Tous allumés par défaut sur Linux x86-64** (`=0` pour couper ; macOS arm64 inchangé). Le
  profil PGO du binaire rapide est entraîné sur cette configuration (mb, zen, ut, d3 ;
  1 356 `.gcda`) et `build-fast/` reconstruit (complet, même série) : `run_tiger.sh` le prend.
  Départ du 04/10 (référence) : DOOM 3 138, Marble Blast 21, Zenerchi 7,6, UT2004 56
  ms/image ; DOOM 3 sur le binaire rapide : 124,1 sur une partie. L'A/B du binaire rapide et la
  matrice finale avec vidage restent à faire hôte au repos (une autre session occupait le PC ;
  commandes dans TODO.md). `tools/tcg/utrafales.py` lit les enregistrements courts d'UT2004.
- **Le côté mémoire du traducteur sur le PC, 06/10** (`docs/tcg-g4.md` §32) : trois
  propriétés éteintes par défaut. `x-tlb-precise` (`tcg/0031`, `TLBPRECISE=1`) : `tlbie`, les
  changements de registre de segment (avec `x-sr-tlb`) et les écritures de BAT ne retirent plus
  du TLB que les entrées concernées (classe de `tlbie` élargie, pages journalisées par segment,
  balayage), et le TLB grandit enfin (**−90 % de remplissages**). `x-lmw-inline`
  (`tcg/0032`, `LMWINLINE=1`) : `lmw`/`stmw` dans une page en accès mot ; `x-dcbz-inline`
  (`tcg/0035`, `DCBZINLINE=1`) : `dcbz` en quatre rangements. Les trois ensemble, A/B ABBA dans
  la VM de dev : **Marble Blast 19,5 → 35 img/s (+80 %)**, softmmu 24 → 7,5 % du temps vCPU,
  allers-retours par tubes −74 %, `dcbz` −65 %, `lmw`/`stmw` −9 à −47 %. Vérificateurs
  (`x-tlb-precise-verify=N`, `x-lmw-inline-verify`, `x-dcbz-inline-verify`) à zéro divergence
  sur des centaines de millions d'opérations, empreintes invité identiques (`tlbtest`,
  `dcbztest`, `lmwtest`, `smctest`), 10 mutants sur 11 détectés ; compteurs `x-mem-stats`. Défaut
  corrigé en route : le vérificateur de `x-sr-tlb` lisait la mauvaise table depuis QEMU 11.
  A/B en jeu (DOOM 3, Marble Blast, UT2004) à faire sur la VM quotidienne.
- **Flottant AltiVec restant sur le PC, 06/10** (`docs/tcg-g4.md` §34,
  `patches/tcg/0034-ppc-vfp-native-cmp.patch`) : `vcmpeqfp`, `vcmpgefp`, `vcmpgtfp`,
  `vcmpbfp` (et formes Rc, CR6 calculé en ops TCG), `vcfsx`/`vcfux` et `vctsxs`/`vctuxs`
  (saturation et VSCR[SAT]) par l'op native de `x-vfp-native` (émetteur x86-64, sans
  effet sur arm64). Propriété `x-vfp-native-cmp`, **éteinte par défaut**
  (`VFPNATIVECMP=1`), vérificateur `x-vfp-native-cmp-verify` (`VFPNCMPVERIFY=1`). Exact
  au bit près : 422,7 M vecteurs contre les helpers et le softfloat de QEMU, émetteur
  réel sous `qemu-ppc` linux-user (10 mutants sur 10), `vfptest c` identique dans Tiger
  (SMP=1 et 2), Marble Blast sous vérificateur sans divergence. Banc invité :
  comparaisons −40 à −71 %, conversions −94 à −96 %. Le vérificateur montre que Tiger
  tourne avec VSCR[NJ] = 1 et que ~42 % des `vmaddfp`/`vsubfp` de Marble Blast
  retombent au logiciel pour des dénormaux mis à zéro par NJ : prochaine cible.
  Gain attendu en jeu ~0,5 % sur DOOM 3, ~1 % sur UT2004, A/B à jouer.

- **La série 0025-0028 sur le PC x86-64, 06/10** (`docs/tcg-g4.md` §30) : binaire de référence
  du PC reconstruit avec `tcg/fixes/` et 0025-0028, plus **`tcg/0029`**, le pendant x86-64 de la
  copie `lmw`/`stmw` de 0025 (gcc 13 laisse la boucle d'origine scalaire, un `bswap` par mot) :
  quatre mots à la fois en SSE2, inlinés, sous la même propriété `x-lmw-vector` ; microbanc de
  la copie −40 % sur 19 registres. Preuves hôte : 1 024 064 cas en 32 et 64 bits, 12 mutants sur
  12 ; `vfpproof` helpers et modèle natif x86 648 M vecteurs chacun sans divergence (le zéro
  signé de `vnmsubfp` corrigé sur le M4 n'existe pas sur le chemin FMA3 x86, qui nie après
  l'arrondi par un `xor`) ; `jcwordproof` 573 440 cas. VM de dev SMP=1 et SMP=2, options de
  production et vérificateurs : empreintes VFP/VMX/LMW identiques à celles du M4, 0 divergence.
  `jitcheck.py` et `jit-m4-proof.sh` portables sous Linux. `LMWVEC`/`JCWORD` restent éteints par
  défaut sur le PC en attendant l'A/B en jeu.
- **Binaire rapide du PC, 06/10** (`docs/binaire-rapide-x86.md`) : la compilation
  `-O3 -march=native`, sans durcissements, LTO et PGO (DOOM 3 −23 %, autres jeux −18-19 % le
  04/10) quitte l'arbre d'essai : `QEMU_FAST=1 ./scripts/build_qemu_qfb.sh` construit
  `~/src/qemu/build-fast/` à côté de la référence `build/` (même série, capacités sondées,
  relevé `pomppc-build.txt`), et `run_tiger.sh` le préfère sur Linux x86-64 quand il est
  complet, construit sur ce processeur et sur la série du dépôt (`QEMU_FAST=0` : référence ;
  `QEMU_BIN` prime ; la bannière dit lequel). Profil entraîné sur une charge mixte (Marble Blast,
  Zenerchi, UT2004, DOOM 3) par `tools/tcg/pgo-train.sh`, constructions sous plafond mémoire ;
  les bras de `matab.sh` et le paquet publié restent sur la référence générique. Mesures sur la
  VM quotidienne : à venir.

- **`x-jit-rel32` sur le PC, 06/10** (`docs/tcg-g4.md` §31) : ses « deux régimes »
  sur UT2004 (57 ou 64-67 ms/image) ne viennent pas de QEMU mais de la mesure. La fenêtre
  `ut-fen` (images 13..73, ~4 s) attrape ou non la rafale de 1 à 3 s de processeur invité
  que coûte l'`osascript` de `premier_plan()` de la matrice, selon la phase de ses relevés ;
  QEMU plus rapide, la phase glisse et la rafale y tombe (8 parties sur 9 avec `JITREL32`).
  À contenu égal (pas de simulation fixe, image rapportée à la même image des 21 parties),
  `x-jit-rel32` est **−2,9 %** sur UT2004. Placement prouvé stable (20/20 lancements :
  aligné 2 Mio, `MADV_HUGEPAGE`, 64 Mio sous le texte), appels réellement directs
  (`-d out_asm`). Outils : `tools/tcg/utrafales.py` (lecture d'une campagne `ut-fen` sans
  les rafales), `tools/tcg/jitcheck.py --rel32` (placement et THP sous Linux). `JITREL32`
  reste à 0 jusqu'à l'A/B de phase 2.
- **Matrice : plus d'osascript dans la fenêtre de mesure, ssh multiplexé** (06/10,
  `docs/matrice-jeux.md` §5) : les jeux à fenêtre fixe la déclarent (`images_fenetre`) ;
  `premier_plan()` part pendant le chargement, jamais à moins de 6 s de la fenêtre ni dedans
  (replis notés, remise au premier plan après) ; System Events lancé avant le jeu.
  `tools/guest/tssh.sh` multiplexe ses connexions (`ControlMaster`, `.run/tssh-<port>`,
  `TSSH_MUX=0` pour revenir) : 0,04 s par commande au lieu de 0,9-2,2 s.
- **Flottant scalaire restant sur le PC, 06/10** (`docs/tcg-g4.md` §33,
  `patches/tcg/0033-ppc-fp-native-cmp.patch`) : `frsp`, `fctiw`, `fctiwz`,
  `fcmpo`, `fdivs` et `fdiv` passent par l'op native de `x-fp-native` (émetteur
  x86-64 ; sans effet sur arm64 en attendant le pendant NEON), `fsel` en ops TCG.
  Propriété `x-fp-native-cmp`, **éteinte par défaut** (`FPNATIVECMP=1`), vérificateur
  `x-fp-native-cmp-verify` (`FPNCMPVERIFY=1`). Exact au bit près hors FI/FX déjà
  admis par `x-fast-fp` : 461,7 M vecteurs contre le softfloat de QEMU sans
  divergence, émetteur réel éprouvé sous `qemu-ppc` linux-user (9 mutants sur 9
  détectés), `fptest c` identique dans Tiger (SMP=1 et 2), Marble Blast sous le
  vérificateur : 70 M opérations, 0 divergence. Banc invité : `frsp` −57 %, `fctiwz` −72 %.
  L'inventaire montre que le reste du « softfloat » de DOOM 3 et d'UT2004 est surtout
  de l'AltiVec (`vcmp*fp`, `vcfsx`). Gain attendu en jeu ~1 % sur DOOM 3, A/B à jouer.

- **JIT M4, 05/10** (`docs/jit-m4-2026-10-05.md`) : modèle natif NEON enfin
  sélectionné par `VFPPROOF_NATIVE=1`, 648 M vecteurs sans divergence, huit mutants
  détectés ; correction du zéro signé `vnmsubfp` et du helper lent aarch64 (0028).
  Vérification en VM SMP=1 et SMP=2, instructions et invalidations sans divergence.
  Flottant AltiVec natif : **1 336 → 362 ms (−73 %) sur microbanc**.
  Variantes `LMWVEC`/`JCWORD` exactes mais sans gain mesuré sur microbanc. À la demande de
  l’utilisateur, `VFPNATIVE`, `LMWVEC` et `JCWORD` sont allumés dans `run_tiger.sh`
  sur macOS arm64 ; chaque variable mise à `0` coupe son option. Rebuild QEMU
  avec 0025–0028 effectué sur le M4 le 05/10 à 21:12 (ppc et ppc64,
  capacités complètes, backend GL compris). A/B DOOM 3 du 06/10, six parties
  par bras entrelacées : **44,9 → 42,2 ms/image (−6,0 %)** pour les trois options
  ensemble. Matrice arrêtée à la demande de l’utilisateur : **7/15 cellules
  terminées, 6 vertes** ; les sept images sont justes, Zenerchi seul dépasse
  le seuil de vitesse (6,3 ms/image pour 6). Bilan partiel conservé dans
  `docs/mesures/m4-20261006/`, vérification de Zenerchi à reprendre.
  Profil des blocs et du code ARM (`hotblocks`, `jitblocks`) ; correction du
  placement RX sous `split-wx` (0027), 20/20 lancements dans la fenêtre du texte.

- **DOOM 3 sur le PC : 150 → 115 ms/image (−23 %), allumé par défaut le 04/10** (nuit du 03
  au 04/10, `docs/vitesse-doom3-x86.md`). Le surcoût propre à DOOM 3 sur x86 (2,5× le M4 au lieu
  de 2×) vient de sa logique à 60 tics/s, payée ~9 fois par image à 150 ms (§3.1) : tout gain
  brut y est amplifié. Leviers, chacun en A/B entrelacé (3 parties par bras) :
  `QEMU_OPT=native,nohard,lto,pgo` (variante de `build_qemu_qfb.sh`, arbre séparé) **−12,9 %** ;
  `tcg/0024` `x-jit-rel32` (tampon du JIT à < 2 Gio du texte sous Linux : appels de helpers
  directs) −2,3 % ; `tcg/0021` `x-vmx-inline` + `tcg/0022` `x-vfp-native` (vsldoi, vmrg[hl]w,
  stve*x et le flottant AltiVec dans le code généré) −3,8 % ; plugin `20261003-d3x` (4 leviers)
  −3,3 % ; `tcg/0023` (mftb sans `div`) dans le bruit. Épreuves : vfptest et vmxtest à
  l'empreinte identique dans 4 modes ; une partie de DOOM 3 sous tous les vérificateurs
  (2,35 G opérations vfp, 2,7 G vmx, 2,1 G lectures de base de temps, 0 divergence ; plugin en
  contrôle, 0 écart) ; 5 mutants de l'émetteur réel sur 5 détectés ; gltest 59 scènes
  identiques dans 7 modes. Matrice du PC tout allumé, avec vidage : mb 17,5, zen 6,3, ut 47,9
  ms/image (−18-19 %), images justes. **Allumés par défaut le 04/10** (à la demande de
  l'utilisateur) : `VMXINLINE` partout, `VFPNATIVE` sur Linux x86-64 seulement
  (émetteur aarch64 de 0022 jamais compilé : à prouver sur le M4), les 4 leviers du plugin
  (révision `20261004-d3x`). **Reconstruire QEMU** (`scripts/build_qemu_qfb.sh`, patches
  0021-0024) **et réinstaller le plugin** ; le binaire PGO reste une variante (`QEMU_OPT`).
  **`JITREL32` reste éteint** : sur UT2004 il a deux régimes selon la place du tampon du JIT
  (57 ou 64-67 ms/image contre 58 sans lui, `bench/tcg/ab/x86-ut-tcg`), cause à trouver. `r_useIndexBuffers 1` : +8 %, à éviter.
  Le vrai `-O3` (`-Doptimization=3`, posé par `native` ; un `-O3` en CFLAGS était écrasé par
  meson) : encore −2,4 %. Piège : la liaison LTO+PGO simultanée des deux QEMU a gelé le PC (swap) ;
  `build_qemu_qfb.sh` lie désormais un binaire à la fois, 6 ltrans.
- **Plugin : trois leviers pour DOOM 3 sur le PC, éteints** (03/10, `docs/d3-plugin-x86.md`,
  révision `20261003-d3x`). Diagnostic : DOOM 3 prend déjà `DRAW_NATIVE` pour tous ses dessins
  (`GEOMHOST 0` n'est pas un refus : il n'a pas de tableaux clients) ; restent au G4 le verdict
  des ~46 % de dispatches qui relient des textures, le balayage et la recopie de 456 000
  indices par image, et trois verrous par dispatch. `POMPPC_GL_IDXLAZY` (pas de balayage quand
  les miroirs sont propres, commande identique), `POMPPC_GL_IDXVEC` (balayage AltiVec,
  `pomppc_vec.c` compilé avec `-faltivec`), `POMPPC_GL_UNITVD` (verdict refait par unité,
  contrôlé par `VERDICTCHECK`), `POMPPC_GL_DISPONE` (dispatch en une passe). Ni protocole ni
  kext : **plugin à recompiler et réinstaller seulement**. Prouvé et mesuré la nuit suivante
  (entrée ci-dessus) ; installé dans la VM quotidienne du PC, leviers éteints.
- **Carillon de démarrage sous Linux** (03/10) : le frontend n'avait de lecteur que pour macOS
  (`NSSound`) ; sous Linux un bouchon refusait de jouer. `StartupChimeLinux.cpp` joue le WAV ou
  l'AIFF par PulseAudio (PipeWire compris), volume réglable pendant la lecture. Le son par défaut
  (`disks/chimes/powermac3-1-4.2.8.wav`, extrait du firmware G4, local) reste à copier sur chaque
  hôte ; `POMPPC_CHIME_FILE` en désigne un autre.
- **VM quotidienne du PC en Tiger 10.4.11, DOOM 3 Demo installé** (03/10). Marble Blast 20,7,
  Zenerchi 6,8, UT2004 57,6 ms/image (10.4.6 : 21,0 / 7,0 / 60,0) ; DOOM 3 150,5 ms/image en
  fenêtre. Le combo remet la mise en veille à 10 min : l'invité endormi coupe ssh et fait
  échouer DOOM 3 (`kCGLBadDisplay`) ; à couper par `pmset`. Lanceur `Doom 3 trace.app`.
- **`x-sr-tlb` validé sur l'hôte x86-64** (03/10, `docs/tcg-g4.md` §27) : `x-sr-tlb-verify`
  sans divergence en SMP=1 (6,6 M contrôles) et en SMP=2 sous Marble Blast (2,2 M). Le PC est
  désormais au niveau du M4 sur toutes les accélérations TCG. La matrice sait profiler QEMU
  sous Linux (`--sample-hote` par `perf`, code JIT nommé par `EXTRA_ARGS=-perfmap`).
- **Flottant AltiVec à 4 voies sur hôte x86-64** (03/10, `patches/tcg/0020`, `docs/tcg-g4.md`
  §26). `x-vfp-fast` laissait `vmaddfp`/`vnmsubfp` au logiciel sur x86 ; ils passent maintenant
  par un `vfmadd231ps` (FMA3, sondé à l'exécution) et `vaddfp`/`vsubfp` par AVX, avec les mêmes
  décisions par voie que l'arm64. `vfpproof.sh` (porté sur QEMU 11) : 648 M vecteurs, 0
  divergence, même nombre de passages rapides que le M4 ; 9 mutants sur 9 détectés ; `vfptest`
  invité à l'empreinte identique. Banc invité (i7-10700F) : 4 054 → 1 492 ms (−63 %).
- **Matrice de jeux sur le PC Linux** (nuit du 02 au 03/10, `docs/matrice-jeux.md` §6 quater).
  Portage (rejoueur EGL, ImageMagick, `/proc/loadavg`, `matab.sh` sans chemin du M4), références
  et planchers par hôte (`references-linux.csv`, `plancher_ms_linux`). Cinq cellules (Marble Blast,
  Zenerchi, UT2004 : les seuls jeux de ce PC) : 4 vertes sur 5 au tour de référence, images
  justes ; un « rejeu ≠ VM » d'UT2004 plein écran (1 sur 7), déjà vu sur le M4, montré cette fois
  hors de tout le vidage. A/B en jeu de tcg/0017-0019 : UT2004 −4,3 %, Marble Blast −3,7 %,
  Zenerchi −6,7 % (`docs/tcg-g4.md` §25.6). UT2004 y est limité par l'émulation (vCPU à 96 %).
- **Marge de tablette par disque** : `<disque>.tablet-margin` (lu par `run_tiger.sh` et
  `devloop.py`) ; le `tiger.qcow2` du PC est en 10.4.6 : 0.
- **GPU 3D sur l'hôte NVIDIA, de nouveau** (02/10, `docs/backend-gl-unites-fixes.md`). Depuis
  la v17 (24/09), l'auto-test du backend GL exigeait 8 unités au pipeline fixe ; NVIDIA en annonce
  4 (et ignore en silence les suivantes) : le PC rendait tout en logiciel. Le backend accepte 4
  unités fixes avec 8 coordonnées et 8 unités d'image et annonce `QGPU_CAP_FIXED4` (sans changer
  la version du protocole) ; le plugin annonce alors `GL_MAX_TEXTURE_UNITS` = 4 et garde 8 sous
  programme. `gltest` 51 scènes sur 51 sur la RTX 4060 Ti, couloir 626 img/s (16 en logiciel).
  Le backend logiciel n'hérite plus des capacités d'un backend GL refusé. **Plugin à
  réinstaller** dans les VM. `stage.sh` joint le contrat `qgpu_proto.h` aux jobs invités.
- **Tablette USB juste sous Tiger 10.4.11** (02/10, `patches/usbhid/0001`,
  `docs/tablette-tiger-10.4.11.md`). L'IOHIDEventDriver de 10.4.11 retire 7,5 % de chaque bout
  des axes absolus : le pointeur s'écartait du clic (×1,18 depuis le centre). `usb-tablet` gagne
  `x-abs-margin`, que `run_tiger.sh` et `devloop.py` règlent à 15 (`TABLET_MARGIN=0` pour
  ≤ 10.4.10).
- **`run_tiger.sh` lance bien ImGuiDock et QEMU 11.1.2** (02/10). La version du QEMU est
  vérifiée avant d'ouvrir le frontend (sa sortie s'y perdait) et un autre binaire est refusé,
  sauf `QEMU_BIN=` explicite ou `POMPPC_QEMU_ANY=1` ; `run_frontend.sh` reconstruit un frontend
  plus vieux que ses sources (celui du PC datait du 22/08, compilé contre une ImGui sans docking).
- **Hôte x86-64 au niveau de l'arm64** (02/10, `patches/tcg/0017`-`0019`, `docs/tcg-g4.md`
  §25). `x-fp-native` et `x-fp-native64` ont leur émetteur x86_64 (VEX et FMA3, sondés à
  l'exécution ; `x-fp-flat` sans eux), `x-tb-fast` lit le TSC invariant sous Linux (source
  d'horloge `tsc` du noyau), `vperm` passe par `pshufb` (AVX). `fptest` simple et double
  identiques octet pour octet, 486 M opérations sous `x-fp-verify` sans divergence, 1,2 G
  lectures de la base de temps sans écart positif, `vperm` 50,7 M cas sans divergence. Bancs
  invités (i7-10700F) contre l'x86 d'avant, options par défaut : chaîne simple 287 → 180 ms,
  sommets double 1 227 → 749 ms, `mftb` 31,8 → 17,5 ns, `vperm` 452 → 95 ms ; mutants de
  l'émetteur 5/5 détectés. Le vérificateur ignore `float_flag_input_denormal_used` (QEMU 10+, jamais lu
  par la cible) : faux positif de `fcmpu` sur opérande dénormal, commun aux deux hôtes.
- **Paquet Linux x86-64** : `scripts/package_release_linux.sh` (dépôt, QEMU, frontend, CD
  invité repris de la publication ou gravé par `xorriso`) ; les bibliothèques viennent du
  système, `DEPENDS.txt` liste les paquets apt. `make_guest_iso.sh` grave aussi sous Linux.

## 0.3 (02/10/2026)

Paquet macOS Apple Silicon (`scripts/package_release_macos.sh 0.3`) au commit de la version.

- **QEMU 11.1.2** (01-02/10). Toute la série `patches/` reposée sur QEMU v11.1.2 (elle visait
  9.2.0), un commit par patch, appliquée sans fuzz (`patches/README.md`, « Base : QEMU 11.1.2 »).
  Gel au démarrage de Tiger après « BSD root » corrigé : en 11.x, `tcg_optimize` n'appelle plus
  `finish_folding` pour un `case` resté `!done`, et le `case` de `ppc_fp32` (tcg/0014) gardait
  les listes de copies de son temporaire de sortie, d'où une boucle sans fin de l'optimiseur.
  Clone neuf : 25 capacités sur 25 ; matrice `bench/matrice/20261001-qemu11` 15 vertes sur 15,
  images justes (Warcraft III au second passage, échec de lancement intermittent déjà vu sous
  9.2.0) : DOOM 3 48,0 / 47,9, Prey 53,8 / 50,4, UT2004 25,3 / 25,4, Colin McRae 50,5,
  Nexuiz ARB 74,4 / 74,2, Nexuiz GLSL 35,0 / 34,5 ms/image (fenêtre / plein écran).
  `config.env` porte la version visée (`POMPPC_QEMU_VERSION`) : `build_qemu_qfb.sh` en tire le
  tag et refuse un arbre d'une autre version, `run_tiger.sh` l'affiche et prévient si le
  binaire diffère ; le titre du frontend montre la version du QEMU lancé (accueil QMP).
- **UT2004 ne gèle plus en quittant** (02/10). `clearDrawable` → `gldAttachDrawable` → relecture
  différée (`run_posts`) dans le tampon du drawable déjà rendu : faute, verrou du plugin tenu ;
  le gestionnaire du jeu appelait `exit()`, dont le nettoyage SDL revenait dans le plugin et
  attendait ce verrou. Garde de faute dédiée (`post_jmp`) : les copies sans destination sont
  jetées. Gel reproduit avec l'ancien plugin, sortie propre confirmée par l'utilisateur.
- **Paquet : `x-tb-fast` réellement actif** (02/10). Le script d'empaquetage signait QEMU avec
  l'entitlement Hypervisor.framework ; un processus qui le porte voit `cntfrq_el0` à 24 MHz
  (Apple M4, macOS 26) au lieu d'1 GHz, et la base de temps rapide (tcg/0015) retombait sur
  l'horloge d'origine (« x-tb-fast : cntfrq 24000000 Hz »). C'était le cas du paquet v0.1.0.
  Signature ad hoc sans entitlement : HVF ne sert jamais à un invité PowerPC.
- **gltest et POMPPCFsqrt sous Tiger 10.4.11** (01/10) : `gl15`, `tex13`, `tex14`, `texlod` en
  256×256 par défaut ; la dépendance de `POMPPCFsqrt` porte la version exacte du noyau
  (`uname -r`, écrite par les installateurs), sans quoi le kext ne se chargeait pas sous 10.4.11.
