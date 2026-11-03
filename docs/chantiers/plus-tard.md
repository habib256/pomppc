# Plus tard

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Pistes différées, rangées par thème.

### Vitesse

- [ ] **[TCG] `tcg/0064` x-tb-head : A/B en jeu** (fusionné le 01/11, éteint par défaut,
  `TBHEAD=3 ./run_tiger.sh` ; `docs/anatomie-ppc32.md`). Mesuré sur banc seulement (−5,0 / −3,1 %
  d'instructions, −2 % de cycles). Binaires à reconstruire d'abord (empreinte de série changée).
  **Fermeture :** A/B des cinq jeux au repos sur les deux hôtes, allumé ou archivé avec le chiffre.
- [ ] **[Plugin] Suites A4** : GLEngine déroule les tableaux clients en Begin/End (Nexuiz ARB
  14-16 %, Warcraft III 4,5 %) — RE du canal `gldCreateVertexArray`
  (`docs/protocole-v23-geometrie.md`) ; clés chaudes par une table « objet GLEngine → qtex »
  côté device ; `geom_send_all`.
- [ ] **[Protocole] Nexuiz : les ~2 ms restantes d'attente des requêtes d'occlusion** :
  soumettre par tranches pendant les dessins, ou résultats écrits par l'hôte.
- [ ] **[Plugin] Transmission paresseuse** : mesure honnête et décision du défaut.
- [ ] **[Plugin] Verdict GLSL : mémoire par texture et par époque** (suite de `WLUNIF`).
- [ ] **[Plugin] Indices en VBO** (`docs/vitesse-doom3-x86.md` §10, point 4, et §5.6) :
  `r_useIndexBuffers 1` retombe sur l'empaquetage ; comprendre le refus de `raw_ensure` pour le
  tampon d'éléments.
- [ ] **[TCG] Flottant natif, angles non vérifiés** : exception FP levée depuis le chemin
  natif, formes Rc=1, cible 32 bits en VM (`docs/tcg-g4.md` §22).
- [ ] **[TCG] `vperm` en ligne sur arm64** (TBL/TBX, pendant de `tcg/0059`) : gain plafonné
  vers 0,3 % ; seulement si un profil M4 montre `helper_VPERM_FAST` haut.
- [ ] **[TCG] `tcg/0052`, rangements** (piste par compteur de génération, `tcg-g4.md` §41.7).
- [ ] **[qgpu] Cache disque des shaders traduits** sur l'hôte (démarrage des jeux, pas la
  vitesse en jeu).
- [ ] **[Leopard, plus tard] Correctif de la fenêtre PCI d'OpenBIOS** (BAR au-delà de
  `0x90000000`), utile à la VRAM de qgpu.

### Maintenabilité du plugin

- [ ] **[Plugin] A2 — Découper `pomppc_accel.c` en modules à frontières écrites** : lecteur
  d'état, textures, géométrie, programmes, transport, diagnostic (`docs/architecture.md` §10).
  Épreuve : mêmes scènes `gltest` à l'octet, mêmes `frames.csv`.
- [ ] **[Plugin] A5 (plugin) — Configuration lue une fois** (plus de `getenv` dans le chemin
  chaud) et purge des drapeaux `POMPPC_GL_*` dont le repli est mort.
- [ ] **[Plugin] Gardes de faute ramenées à la cause** : chaque `faute de lecture` et niveau
  envoyé noir devient un compteur observé à zéro sur la matrice.

### Distribution 1.0 et hôtes

- [ ] **[Distribution] 1.0 = installation reproductible** : CD ou paquet, `install.sh` qui
  reconstruit kext et plugin, disque quotidien recréable depuis l'ISO, matrice verte à chaque
  commit. **Pas de publication x86 avant la vitesse** (règle du 20/10).
- [ ] **[Distribution] Firmware reproductible** : `openbios-smp-screamer.elf` est livré en binaire.
- [ ] **[Hôtes] Un seul disque, deux hôtes** : `tiger.raw` partagé par USB entre le Mac et le PC.
- [ ] **[TCG] `tlbie` en SMP stock** : défaut de QEMU 9.2, **toujours présent dans 11.1.2**
  (vérifié le 01/11 : `ppc_tlb_invalidate_one`, cas `POWERPC_MMU_32B`, ne pose que
  `TLB_NEED_LOCAL_FLUSH`), corrigé chez nous par `x-sr-tlb` ; à signaler en amont. Aussi : `helper_store_[id]batu` vide l'ancien BAT avec le masque de la nouvelle
  valeur (`tcg-g4.md` §45, `tcg/0062` le relève).

### Divers

- [ ] **[Système] Quartz Extreme** : allumé sur les deux hôtes (`docs/quartz-extreme.md`).
  Restes : `install.sh QE=1` et personnalités, Core Image (avis borné : chantier plugin).
- [ ] **[Frontend] Ergonomie, hors lots** : empêcher la veille pendant l'installation (point 1 de
  l'étude), barre de menus native.
- [ ] **[Frontend] Frontend F1, restes non joués** : la vraie touche Ctrl+Cmd+F, écran Retina,
  plusieurs moniteurs.
- [ ] **[Son] Arrêt de la boucle principale de 186 ms** en fin de cinématique de DOOM 3
  (`docs/audio-stabilite.md`).
- [ ] **[Métrologie] Métrologie boot** (`docs/metrologie-boot.md`) : baseline de 23,32 s à
  refaire ; A/B du coût du Screamer jamais lancé.
- [ ] **[Système] « Panique cpu 1 » du 24/09** : jamais reproduite depuis le correctif OpenPIC.
