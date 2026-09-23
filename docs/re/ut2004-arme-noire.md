# UT2004 : l'arme en main est noire et brillante (enquête en cours, 23/09/2026)

Défaut rapporté par l'utilisateur : « les armes du joueur n'ont pas de texture (mais c'était
déjà le cas avant) ». Le mitrailleur en main est rendu noir, luisant, sans sa peau. Les
ennemis, les ramassages au sol et le décor sont texturés.

Méthode : vidage autonome des soumissions (`POMPPC_GL_DUMP_TRIGGER`, images 3865–3905,
133 soumissions, dossier `UT2004-083001` sur le Tiger de l'utilisateur) et rejeu natif
(`tests/qgpu_replay.c`, backend gl, 0 erreur). Le rejeu reproduit le défaut à l'identique
(image `ut2004-arme-rejeu.png`, hors dépôt avec le vidage, `TEST/`) : le problème est donc dans les données envoyées à l'hôte
ou dans l'hôte, pas entre rendu et écran.

## 1. Quel dessin est l'arme

Nouvel interrupteur du rejoueur : `QGPU_REPLAY_SKIPDRAW=<image>:<i>-<j>` saute les dessins
d'indice i..j de l'image (comptés sur tous les DRAW_RAW/DRAW_RAW_BUF de l'image, la
soumission est exécutée par tronçons autour d'eux). Balayage des 71 dessins de l'image 3880,
un par un, différence d'image avec la référence :

| dessins | contenu | effet du saut |
|---|---|---|
| 28, 29, 30 | 1566 + 1620 + 384 sommets, fmt 0x1ce (POS4, normale, couleur, tex0..2), éclairés, mélange ONE/ZERO | **l'arme disparaît** |
| 8, 9 | 1227 sommets, 4 unités (tex 49/47/47/48) | deux robots au loin (pas l'arme) |
| 54 | 1218 sommets, fmt 0x4e, tex 79 seule, MODULATE(TEX, PRIM) | ramassage au sol, en bas à gauche, **correctement texturé** |
| 38–70 | décor (55–61, 2 textures), icônes | début de l'image suivante : sans effet sur l'image 3880 |

Les dessins 8/9 sont ce que j'avais d'abord pris pour l'arme : fausse piste, réglée par le
balayage.

## 2. L'état de l'arme, tel que le plugin l'a miroité

Trois unités actives, mélange ONE/ZERO (passe opaque unique), éclairage allumé, matériau de
couleur coupé :

| unité | texture | combineur RGB | combineur A |
|---|---|---|---|
| 0 | **120 = carte de cube** 128×128, six faces quasi noires avec un point de lumière (moy. RGB 9/8/8) | REPLACE(TEX) | REPLACE(TEX) |
| 1 | **79 = peau diffuse du mitrailleur** 256×256, alpha = masque spéculaire (4..68) | REPLACE(PREV) | REPLACE(TEX.a) |
| 2 | **0 = texture 1×1 blanche** (la « NoTexture » d'UT2004, première créée, id 0 du plugin) | MODULATE(PRIM, PREV) ×2 | REPLACE(PREV) |
| 3 | coupée | | |

La texture 79 se reconstitue depuis le vidage (TEX_IMAGE3, RGBA 8 bits, offsets absolus dans la
fenêtre : `off - base - arena_off` dans la région arène du fichier).

Résultat de cette chaîne, sur l'hôte comme sur n'importe quel OpenGL : cube × éclairage × 2,
alpha = masque. **La couleur de la peau 79 n'entre nulle part** — d'où le noir brillant.

Expériences de rejeu sans effet : unités 1–3 coupées, constante blanche, sans atténuation,
ambiante à 1. Aucun refus (« fallback ») dans note.txt sur toute la session : tous les dessins
sont partis à l'hôte, il n'y a pas de seconde passe perdue. Aucun facteur DST_ALPHA dans le
vidage.

## 3. Ce que ça implique

Le jeu n'a pas pu vouloir cet état : sur un vrai Mac le même état donnerait la même arme
noire, et elle y est texturée. Donc le miroir du plugin diffère de ce que GLEngine tient,
sur l'un de ces points :

1. **Sources du combineur de l'unité 1 ou 2** (TU_SRC0_RGB… à +0x1c/+0x22 du bloc d'unité,
   pas 0x7c) : une fonction ou une source que le plugin lit à côté quand une carte de cube
   est en jeu, ou un GL_TEXTUREn de croisement mal résolu.
2. **Affectation texture ↔ unité** quand une unité porte une carte de cube (`unit_slot`,
   emplacements 0 cube / 3 2D de CTX_TEXUNITS) : si 79 et 120 étaient échangées, la chaîne
   donnerait 79 × éclairage × 2 — une arme texturée sans reflet, très proche du résultat
   attendu. À tester en premier.
3. Une 4ᵉ unité que le plugin ne voit pas (masque TU_ENABLE de l'unité 3).

À faire (sur le M4, avec le jeu) : sonde plugin qui, au premier dessin avec carte de cube sur
l'unité 0 et trois unités actives, vide dans note.txt les quatre blocs d'unité bruts (0x7c
octets chacun) et la table CTX_TEXUNITS, et compare les coordonnées de texture des unités 0
et 1 dans le vidage (2D en [0,1] ou vecteurs de réflexion 3D) — c'est ce qui tranche entre
les hypothèses 1 et 2. Le vidage (22 Mo) est archivé hors dépôt sur le PC x86, `TEST/ut2004-arme-dump4.tgz` ; sur le M4,
en refaire un : lanceur « UT2004 trace », puis `touch /tmp/pomppc-dump-on` par ssh quand l'arme est
à l'écran.

## Outils ajoutés

- Plugin : `POMPPC_GL_DUMP_TRIGGER=<fichier>` — vidage autonome à partir de l'apparition du
  fichier (invalidate_mirrors → tout l'état et toutes les textures réémis), pendant
  `POMPPC_GL_DUMP_FRAMES` images.
- Rejoueur : prologue (contexte, surface présentée, textures inconnues), images par
  SURF_READBACK, `QGPU_REPLAY_LIST=<image>` (dessins, combineurs, lumières, matériaux),
  `QGPU_REPLAY_SETKEY`, `QGPU_REPLAY_NOATT`, `QGPU_REPLAY_AMB`, `QGPU_REPLAY_SKIPDRAW`.
