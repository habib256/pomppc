# Changelog

Ce que chaque version publiée de POMPPC a changé, de la plus récente à la plus ancienne
(publications : https://github.com/habib256/pomppc/releases). Le **protocole qgpu**
(`QGPU_PROTO_VERSION`, v22 en 0.3) a son propre numéro : chaque changement sémantique exige de
reconstruire QEMU et le plugin ; le kext ne change que si l'ABI de transport change. Les mesures
sont celles prises le jour même, sur l'hôte indiqué. Le détail de chaque lot est dans le message
de commit et dans `docs/`.

## Non publié

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
