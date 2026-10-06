# Changelog

Ce que chaque version publiée de POMPPC a changé, de la plus récente à la plus ancienne
(publications : https://github.com/habib256/pomppc/releases). Le **protocole qgpu**
(`QGPU_PROTO_VERSION`, v22 en 0.3) a son propre numéro : chaque changement sémantique exige de
reconstruire QEMU et le plugin ; le kext ne change que si l'ABI de transport change. Les mesures
sont celles prises le jour même, sur l'hôte indiqué. Le détail de chaque lot est dans le message
de commit et dans `docs/`.

## Non publié

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
