# UT2004 : texture diffuse perdue, reflets conservés

## Démo de test, PC Linux (NVIDIA) : cause et correctif du 07/10/2026

**Reproduit sur le PC.** La « démo de test » est la caméra d'introduction
d'AS-Convoy que joue la matrice (`tools/matrice/jeux/ut.py`, graine fixe,
image n identique d'un tour à l'autre). Le vidage `ut-fen` de la nuit
(images 235-295) ne montre aucune arme ; un vidage de toute la démo (images
1-450, `POMPPC_GL_DUMP_TRIGGER=@1`, réglages du jeu inchangés, rangé hors dépôt
sous `bench/ut-arme/demo-complete/`) en montre dans le couloir des images
~166-195 : présentoirs d'armes (pied + arme) **noirs**, dans le rejeu `gl`
comme dans la VM. Le même dessin avec `POMPPC_GL_NATIVE=0` donne l'image à
l'identique (le chemin DRAW_NATIVE est hors de cause).

**Dessin isolé.** Image 176 : balayage `SKIPDRAW`, puis
`QGPU_REPLAY_STATE=176:<i>` (état tenu par le cœur au dessin i, ajouté au
rejoueur ce jour) sur les dessins de l'arme : unité 0 = carte de cube 64×64
(`REFLECTION_MAP`) `REPLACE(TEX)`, unité 1 = peau 2D `RGB REPLACE(PREVIOUS)` /
`A REPLACE(TEX.a)`, unité 2 = `MODULATE(PRIMARY, PREVIOUS)×2` — exactement la
signature du 23/09 : le **repli d'UT2004 sans combineur ATI ni NV**
(`0x461d40..0x461d90`, voir plus bas), qui garde le reflet et jette la couleur
de la peau.

**Cause.** `patches/qgpu/qgpu-gl.c` (initialisation du backend) n'annonçait
`QGPU_CAP_COMBINE3` que si l'hôte a `GL_ATI_texture_env_combine3`. Le pilote
NVIDIA (RTX 4060 Ti, 595.91.07) ne l'a pas — il a
`GL_NV_texture_env_combine4`. Sur le PC le device annonçait donc
`caps 0x6dffe` (bit `0x2000` absent), le plugin taisait l'extension ATI, et le
jeu prenait son repli : le correctif v22 du 27/09, validé sur le M4 (Apple GL a
l'extension ATI), n'avait jamais agi sur le PC.

**Correctif (hôte).** Le backend GL annonce `QGPU_CAP_COMBINE3` avec l'extension
ATI **ou** NV_texture_env_combine4 ; sans l'ATI, une unité qui emploie une
fonction ATI ou une source ZERO/ONE est posée en `COMBINE4_NV`
(`gl_combine4`, `gl_c4_channel` : `MODULATE_ADD` = a0·a2 + a1·1, `SIGNED` en
`ADD_SIGNED`, ONE = ZERO sous opérande inverse, fonctions ARB réécrites en
somme de deux produits). `MODULATE_SUBTRACT` n'a pas d'équivalent exact
(approché sans la soustraction, dit une fois sur stderr) ; la démo ne l'emploie
pas (aucun avertissement sur 450 images). Rien ne change sur un hôte qui a
l'extension ATI (le M4). Pas de changement du plugin.

**Preuves (07/10).**
- `qgpu_core_test` : `run_v22` passe sur le backend GL de la RTX 4060 Ti
  (MODULATE_ADD, MODULATE_SIGNED_ADD, échelles 1/2/4, RGB et alpha, ZERO/ONE et
  inverses) ; MODULATE_SUBTRACT noté « approché » et l'unité 7 « hors contrat
  sous FIXED4 » (4 unités fixes chez NVIDIA) ; 0 échec.
- VM quotidienne sur un QEMU reconstruit avec ce backend (`~/src/qemu-ut`,
  copie de la référence, seuls `qgpu-gl.c` et `qgpu-core.h` changés) : le plugin
  voit `caps 0x6fffe` ; même vidage de toute la démo
  (`bench/ut-arme/demo-combine4/`) : présentoirs texturés (métal, reflets),
  rejeu `gl` ≈ rejeu `soft` (image 176 : écart moyen 0,6, 0,24 % des pixels) ;
  173 images sur 450 changent, toutes dans les scènes à armes. Planches
  `bench/ut-arme/avant-apres.png`, `avant-apres2.png` (avant / après / soft).
- Matrice `ut-fen` sur ce QEMU : image **juste** (rejeu = VM 0,00, référence
  0,00) ; vitesse non concluante (hôte chargé, 6 autres QEMU).
- `gltest`, 82 scènes dans l'invité, binaire de référence contre binaire corrigé :
  mêmes 70 réussites et mêmes 12 scènes en échec (préexistantes sur le PC :
  bigstrip, clip, dlist, fusion, lit, matbegin, mixte, stencil, tcprobe,
  texgen, v15, combine3) ; `combine3` passe de 35 échecs (« extension non
  annoncée ») à 21, tous attendus : MODULATE_SUBTRACT approché et unité 7, hors
  des 4 unités fixes de NVIDIA.

**Détecteur.** `QGPU_REPLAY_UNUSED=1` signale, à chaque dessin du pipeline fixe,
une texture liée dont la couleur n'atteint pas le résultat (chaîne des
combineurs remontée, RGB et alpha séparés). Utile au M4 pour chercher la même
signature ; il signale aussi des usages légitimes (masques d'alpha d'une passe
spéculaire), à lire avec `QGPU_REPLAY_STATE`.

**Reste.** Le M4 : l'utilisateur y a vu les armes noires dans la démo le 01/10
alors que le M4 annonce l'extension ATI ; ce correctif n'y change rien. À
refaire là-bas : vidage de toute la démo (`@1`, 450 images), rejeu, images
~166-195, `QGPU_REPLAY_UNUSED` / `QGPU_REPLAY_STATE` sur les présentoirs. Sur
le PC, le binaire de référence `~/src/qemu` doit être reconstruit avec ce
`qgpu-gl.c` pour que la VM quotidienne en profite.

## Cause établie le 27/09/2026

Le miroir du plugin est fidèle aux getters publics OpenGL **et** à l'état compilé
par le jeu. La peau n'est pas perdue au transfert : UT2004 ne la combine pas,
faute de `GL_ATI_texture_env_combine3` ou de `GL_NV_texture_env_combine4` annoncé.
Le passage de quatre à huit unités fixes ne suffit pas. Les profils de test
utilisent désormais `MaxTextureUnits=8` (limite fixe réelle ; GLSL possède
séparément 16 unités d'image).

Le matériau `Combiner UT2004Weapons.WeaponSpecMap2` porte l'opération 6
(`CO_Add_With_Mask`) : Material1 est `TexEnvMap ...WeaponEnvMap2`, Material2
et Mask sont `Texture UT2004Weapons.NewWeaps.AssaultRifleTex0`. La fonction
`FOpenGLRenderInterface::HandleCombinedMaterial` teste ATI (branche `0x461acc`),
puis NV (`0x461bfc`). Sans ces extensions, son repli à `0x461d40..0x461d90`
pose explicitement **REPLACE(PREVIOUS)** : seule la réflexion précédente reste.
Les sondes en mémoire n'ont modifié aucun fichier du jeu.

Correctif général : protocole **v22**, `QGPU_CAP_COMBINE3`, fonctions ATI
MODULATE_ADD / MODULATE_SIGNED_ADD / MODULATE_SUBTRACT en RGB et alpha, sources
ZERO/ONE, opérandes et échelles. Backend logiciel et GL ; annonce du plugin
conditionnée par version/capacité, pas un traitement spécial d'UT2004.
Voir [le contrat](../protocole.md#combineurs-ati-v22).

Preuves : `tests/qgpu_core_test.c::run_v22` passe sur soft et GL ; la scène
`gltest combine3` passe dans Tiger, renderer POMPPC, unités 0 et 7, RGB/alpha,
échelles 1/2/4, ZERO/ONE et inverses, sans erreur GL. Journaux locaux :
`bench/ut-combine3-native.log`, `bench/ut-combine3-guest.log`.

Contrôle réel : DM-Rankin, huit unités, capture
`bench/utweapon-priorites11/current.png` : peau visible et reflets conservés,
confirmés par l'utilisateur. La sonde publique relève le dessin de l'arme
(1719 indices), unité 1 `GL_COMBINE_RGB=0x8744` : le jeu choisit désormais
le combineur ATI. Trace et vidage conservés sous
`bench/utweapon-priorites11/utweapon-probe11/`. Aucun patch du binaire du jeu.

La sonde publique reproductible est `tools/guest/gltrap/gltrap.c`, activée par
`POMPPC_GLTRAP_CUBE=<fichier déclencheur>`. Elle interpose aussi
`NSAddressOfSymbol`, car SDL d'UT2004 résout ces fonctions dynamiquement.
Ne pas employer cette instrumentation pour les mesures de performances.

## Enquête initiale (23/09/2026)

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

## 3. Hypothèses initiales — écartées le 27/09

L'enquête initiale soupçonnait le miroir du plugin sur les points suivants.
Cette déduction était fausse : le jeu choisit bien cet état en fonction des
extensions annoncées (voir la cause établie ci-dessus).

1. **Sources du combineur de l'unité 1 ou 2** (TU_SRC0_RGB… à +0x1c/+0x22 du bloc d'unité,
   pas 0x7c) : une fonction ou une source que le plugin lit à côté quand une carte de cube
   est en jeu, ou un GL_TEXTUREn de croisement mal résolu.
2. **Affectation texture ↔ unité** quand une unité porte une carte de cube (`unit_slot`,
   emplacements 0 cube / 3 2D de CTX_TEXUNITS) : si 79 et 120 étaient échangées, la chaîne
   donnerait 79 × éclairage × 2 — une arme texturée sans reflet, très proche du résultat
   attendu. À tester en premier.
3. Une 4ᵉ unité que le plugin ne voit pas (masque TU_ENABLE de l'unité 3).

Contrôle alors proposé (effectué depuis sur le M4) : sonde plugin qui, au premier dessin avec carte de cube sur
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
- Rejoueur, 07/10 : `QGPU_REPLAY_STATE=<image>:<i>` (état du cœur juste avant le
  dessin i : clés hors valeur initiale, unités avec texture, cible, taille,
  moyenne ARGB et combineurs décodés, lumières, matériau ; couleurs de sommet
  d'un DRAW_NATIVE indexé), `QGPU_REPLAY_TEXOUT=<dossier>` (avec STATE : niveau
  de base des textures liées en PPM/PGM), `QGPU_REPLAY_UNUSED=1` (détecteur de
  texture liée sans effet sur la couleur). Avec `image_min image_max` en
  arguments, un balayage SKIPDRAW s'arrête à l'image visée.
