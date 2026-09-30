# A4, volet état — le bloc d'état lu par le device (30/09/2026)

Chantier **A4** du `TODO.md` (« Plus tard > Vitesse ») : *déplacer le travail vers
l'hôte*. Trois volets en parallèle (état, textures, géométrie) ; celui-ci est le
**bloc d'état** : `compute_state` et `send_state` doivent sortir du profil.
L'intégrateur passera l'ensemble en v23 ; ce volet n'incrémente pas
`QGPU_PROTO_VERSION` et s'annonce par un bit de capacité.

## 1. Profil de départ et plafond

Profil de départ de tout A4 : `bench/a4/depart/LISEZMOI.md` (QEMU de référence,
plugin `20260930-wlunif`, neuf jeux, trois relevés `sample` agrégés par jeu pour
DOOM 3, Prey, UT2004, Nexuiz ARB, Colin McRae). Ce que pèse l'état, en part du
fil principal et en ms/image :

| jeu | ms/image | `compute_state` | `send_state` net¹ | `geom_send_all` |
|---|---|---|---|---|
| DOOM 3 (fenêtre) | 60,8 | 1,8 % · 1,08 | 3,4 % · 2,06 | 2,1 % · 1,30 |
| Prey (fenêtre) | 71,3 | 1,6 % · 1,16 | 2,3 % · 1,61 | 0,9 % · 0,64 |
| UT2004 (fenêtre) | 27,5 | 2,2 % · 0,60 | 3,2 % · 0,88 | 1,9 % · 0,53 |
| Nexuiz ARB (fenêtre) | 110,4 | 2,0 % · 2,17 | 4,0 % · 4,45 | 2,2 % · 2,38 |
| Colin McRae (plein écran) | 66,9 | 2,5 % · 1,66 | 3,0 % · 2,03 | 1,1 % · 0,75 |
| Warcraft III (plein écran) | 17,6 | 1,1 % · 0,20 | 2,7 % · 0,47 | 2,7 % · 0,48 |

1. `send_state` sans le `close_raw` (balayage des NaN du lot `DRAW_RAW`
   précédent, `raw_scan_nan`) que son `reserve` déclenche : ce coût-là est de la
   géométrie. Compté naïvement, `send_state` pèse 14,5 % dans Nexuiz ARB et
   8,8 % dans Warcraft III — c'est `raw_scan_nan`, pas l'état.

**Plafond** : `send_state` net, 2 à 4 % du fil principal selon le jeu (DOOM 3 :
2,1 ms/image sur 60,8, soit 3,4 %), au-dessus du seuil de ~2 % fixé pour
construire. Il se compte par rapport au lot 4 (`POMPPC_GL_STSKIP`, déjà allumé :
`compute_state` sauté dans 44 % des dessins de DOOM 3). `geom_send_all`
(matrices, lumières, texgen) n'est pas dans ce volet.

Ce que coûte `send_state` dans DOOM 3 (1 318 dessins et 3 446 `SET_STATE` par
image) : `compute_state` 1,7 % (les ~56 % de dessins où le lot 4 ne saute pas),
le différentiel et son `reserve` par clé 1,5 %, `state_units` (verdict des
unités, à chaque dessin) 0,2 à 0,6 %.

## 2. Conception retenue

Les deux pistes de la commande, tranchées à la lecture du code :

- **(a) le vecteur dans un bloc partagé, différentiel par le device** : ne retire
  que le différentiel, garde `compute_state` ;
- **(b) le device lit les pages du contexte GLEngine** : retire aussi
  `compute_state`, mais les dessins partent en **asynchrone** — quand le device
  traite un dessin, GLEngine a déjà écrit l'état des dessins suivants. Lire les
  pages en place donnerait l'état d'un autre dessin. Il faudrait un instantané
  par dessin : autant le mettre dans le flux.

Retenu : **(b) par le flux**. À chaque dessin où le lot 4 aurait recalculé,
l'invité ne fait plus que **recopier** (quatre `memcpy` et huit mots) les
fenêtres de l'état de GLEngine qu'il lisait, dans une commande
`STATE_BLOCK` ; le **cœur** en tire les clés comme `compute_state` et ne pose que
celles qui diffèrent de l'état du contexte. Rien n'est lu dans la mémoire de
l'invité hors du flux ; aucune ABI de transport ne change (ni kext ni
redémarrage) ; chaque clé tirée passe par la validation de `SET_STATE`.

| élément | valeur |
|---|---|
| capacité | `QGPU_CAP_STATE_BLOCK` = `0x00008000`, annoncée par le cœur quel que soit le backend ; `QGPU_STATE_BLOCK=0` dans l'environnement de QEMU la retire |
| opcode | `QGPU_OP_STATE_BLOCK` = `0x0080` (plage réservée 0x0080..0x0087) |
| arguments | `[drapeaux, largeur, hauteur, sbits, W0 (1 mot), W1 (65), W2 (7), W3 (23), LOD (8)]` = `QGPU_LEN_STATE_BLOCK` 109 mots ; `+ QGPU_SK_COUNT` mots sous `QGPU_SB_F_CHECK` (271) |
| fenêtres | état de GLEngine à `0x24ac`, `0x2d44..0x2e48`, `0x30bc..0x30d8`, `0x3168..0x31c4`, et le biais de LOD de l'unité u à `0x3200 + u × 0x7c` (octets gros-boutistes tels quels) |
| drapeaux | `F_RAW` chemin brut (modes de polygone), `F_VALID` valeurs conservées = état courant, `F_STENCIL` surface à stencil suivi, `F_CHECK` contrôle, et les plages que l'invité enverrait (`F_V7`, `F_V8`, `F_TEX14`, `F_PROG`, `F_UNITS8`, `F_GLSL`) ; bit inconnu = `BAD_ARG` |
| clés tirées | toutes celles des plages de `send_state`, **sauf les clés chaudes** : quatre clés de chaque unité 0..15, `GL_COMBINE` et ses sources 0..7, activation des programmes. Elles viennent du verdict de l'invité (identifiants de texture hôte, programmes connus de l'hôte) et partent par `SET_STATE`, avant le bloc |
| pose | ordre des plages, clé croissante, seulement si la valeur change, par `set_state_key` (la validation de `SET_STATE`, extraite) ; au premier refus, son statut, les clés suivantes ne sont pas posées |
| `QGPU_MAX_CMD_ARGS_LONG` | 320 : le tableau d'arguments du cœur (la plus longue commande était `SET_POLYGON_STIPPLE`, 32) |

Détails de fidélité : les ciseaux sont calculés en arithmétique 32 bits signée
comme sur le G4 ; `to_u8` (couleurs de mélange et de brouillard) est un
`fmadds` dans le plugin (désassemblage de `_to_u8`) et un `fmaf` dans le cœur ;
les sondes `POMPPC_GL_NOCULL`, `FLIPFACE`, `GLYPHTEST`, qui retouchent le
vecteur, gardent l'ancienne voie.

**Plugin** (`20260930-stblk3`) : `POMPPC_GL_STATEBLK` (défaut **0**) et la
capacité du device ; sur le QEMU de référence, ancienne voie (vérifié :
`stateblk=0` dans la note, `gltest` inchangé). `send_state_blk` :

1. même saut que le lot 4 (rien de sale, même chemin, même surface) ;
2. clés chaudes : `state_units` et `state_progs`, envoyées si elles diffèrent de
   `p->st`. **Reprise des unités** : si le verdict des unités (`TexInfo`) est le
   même, octet pour octet, que celui de la dernière série, à la même époque des
   textures (`vd_epoch`, sans laquelle aucun `qtex` ne bouge) et `p->st` valide,
   les clés des unités sont déjà celles du device : seules celles des
   programmes sont refaites ;
3. sinon rien de sale n'est sauté : `STATE_BLOCK` (copie des fenêtres) ;
4. les clés non chaudes de `p->st` ne sont plus tenues (`st_host`) : le seul
   autre lecteur (taille de point du chemin hérité) le sait.

**Contrôle** : `POMPPC_GL_STATEBLK=1 POMPPC_GL_STATECHECK=1` calcule aussi
`compute_state` et joint son vecteur (`F_CHECK`) : le cœur compare clé à clé ce
qu'il a tiré, compte les écarts (journal de QEMU, « qgpu: bloc d'état : N blocs
contrôlés, M en écart » toutes les 65 536, et chaque écart jusqu'à 24) et pose
le vecteur de l'invité ; le plugin tient alors `p->st` en entier, garde le
contrôle du lot 4, et refait toujours les unités en comptant comme écart un
envoi là où la reprise aurait sauté (lignes `STATEBLK` de la note).

## 3. Épreuves d'équivalence

@@EPREUVES@@

## 4. A/B à scène égale

@@AB@@

## 5. Ce qui reste

@@RESTE@@
