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
| Nexuiz GLSL (plein écran)² | 40,1 | 1,4 % · 0,57 | 3,3 % · 1,32 | 3,9 % · 1,55 |
| Colin McRae (plein écran) | 66,9 | 2,5 % · 1,66 | 3,0 % · 2,03 | 1,1 % · 0,75 |
| Warcraft III (plein écran) | 17,6 | 1,1 % · 0,20 | 2,7 % · 0,47 | 2,7 % · 0,48 |

1. `send_state` sans le `close_raw` (balayage des NaN du lot `DRAW_RAW`
   précédent, `raw_scan_nan`) que son `reserve` déclenche : ce coût-là est de la
   géométrie. Compté naïvement, `send_state` pèse 14,5 % dans Nexuiz ARB et
   8,8 % dans Warcraft III — c'est `raw_scan_nan`, pas l'état.
2. Bras A (option éteinte) de l'A/B du §4, sur la copie A4 de QEMU : les relevés
   en fenêtre du départ avaient des replis ou recouvraient le vidage.

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
  que le différentiel, garde `compute_state` ; et un bloc à part dans BAR0 serait
  réécrit par le dessin suivant avant que le device ne le lise (il lui faudrait
  un anneau et des barrières) ;
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

Toutes sur la copie `~/src/qemu-a4etat` (même arbre que le QEMU de référence,
`patches/qgpu/*` de la branche copiés dans `hw/display/`), VM quotidienne.

| épreuve | résultat |
|---|---|
| tests natifs `run_stblock` (`tests/qgpu_core_test.c`, soft et GL) : contrat, refus (sans contexte, longueur, drapeau inconnu, `F_CHECK` sans vecteur, taille hors bornes, sans capacité = `BAD_OPCODE`), toutes les offsets lues dans les fenêtres, état neutre, valeurs calculées à la main (profondeur, masque, mélange, ciseaux retournés, stencil, couleur de brouillard `0xFF8040FF`, ligne 2,6 → 3, décalage borné, mode de polygone brut / non brut, biais borné), clés chaudes intactes, bloc identique = 0 clé posée, arrêt au premier refus, contrôle | 22 × 2 OK ; `tests/run-all.sh` 173 OK, 0 échec |
| `gltest`, 42 scènes (`tools/guest/jobs/gta4.sh` : `state clip fogz blendc stencil logicop polymode stipple sepspec offset depth alpharep` … `lit texgen tex14 gl15` et GLSL) | verdicts et md5 de chaque image **identiques** `STATEBLK=0`, `=1` et `=1` + `STATECHECK` + `VERDICTCHECK` ; 0 écart `STATE`, `VERDICT`, unités ; `clip`, `stencil`, `tex14` ont leurs échecs connus, les mêmes dans les trois modes (et sur le QEMU de référence) |
| plugin sur le QEMU de référence (capacité absente) | `stateblk=0`, même sortie `gltest` |
| onze cellules de la matrice sous contrôle (`bench/a4/etat/controle-*` : Marble Blast, Zenerchi, DOOM 3, Prey, UT2004, Nexuiz ARB et GLSL en fenêtre ; DOOM 3, Warcraft III, Colin McRae, Nexuiz GLSL en plein écran) | **8 388 608 blocs contrôlés par le cœur, 0 écart** ; `STATE` (lot 4) 0, `VERDICT` 0 |
| reprise des unités, neuf cellules (`controle2-*`) puis DOOM 3 et UT2004 (`controle3-*`) | 13 893 632 blocs contrôlés depuis le lancement de QEMU, 0 écart ; unités : **240 853 reprises vérifiées par 500 images** dans DOOM 3, 0 écart |
| `to_u8` | `fmadds` dans `_to_u8` du plugin (désassemblage), `fmaf` dans le cœur |

Remarque sur le flux : DOOM 3 envoie ~724 blocs par image (les 56 % de dessins
que le lot 4 ne saute pas), soit ~310 Kio de commandes de plus par image ; les
`SET_STATE` non chauds disparaissent. Le coût côté invité est une copie de
416 octets par bloc.

## 4. A/B à scène égale

Même binaire (copie A4), `POMPPC_GL_STATEBLK=0` (A, ancienne voie) contre `=1`
(B), parties entrelacées ABBA, `tools/matrice/matrice.py --sans-vidage --sample 10`,
VM quotidienne en affichage natif (`POMPPC_FRONTEND=native`, comme le QEMU de
référence du profil de départ), `bench/a4/etat/ab-natif/`, 16 h 58 à 18 h 15 ;
0 autre QEMU à chaque partie, charge de l'hôte 1,5 à 2,7 avant et après chaque
fenêtre de mesure.

| jeu | A (ms/image) | B (ms/image) | médianes | paires B − A |
|---|---|---|---|---|
| DOOM 3 fenêtre, 6 + 6 | 60,5 60,6 60,9 60,5 60,8 60,7 | 59,0 59,0 59,3 59,6 59,4 59,5 | **60,7 → 59,3 (−1,4, −2,3 %)** | −1,5 −1,6 −1,6 −0,9 −1,4 −1,2 : les six négatives |
| UT2004 fenêtre, 3 + 3 | 26,2 25,6 25,7 | 24,9 25,4 25,1 | 25,7 → 25,1 (−0,6) | −1,3 −0,2 −0,6 |
| Nexuiz GLSL plein écran, 3 + 3 | 40,1 39,3 40,1 | 39,8 39,7 39,5 | 40,1 → 39,7 (−0,3) | −0,3 +0,4 −0,6 : dans le bruit |

Profils agrégés (six `sample` par bras pour DOOM 3, trois pour les autres ;
part du fil principal · ms/image) :

| poste | DOOM 3 A | DOOM 3 B | UT2004 A | UT2004 B | Nexuiz GLSL A | Nexuiz GLSL B |
|---|---|---|---|---|---|---|
| `send_state` | 3,8 % · 2,33 | **2,1 % · 1,27** | 3,2 % · 0,82 | **1,3 % · 0,31** | 3,8 % · 1,53 | **2,0 % · 0,81** |
| `compute_state` | 2,2 % · 1,34 | **0,2 % · 0,09**² | 1,6 % · 0,42 | 0,1 % · 0,03 | 1,4 % · 0,57 | 0,1 % · 0,03 |
| `state_units` | 0,6 % · 0,36 | 0,5 % · 0,27 | 0,4 % · 0,09 | 0,2 % · 0,06 | 1,0 % · 0,39 | 0,2 % · 0,09 |
| `geom_draw_client` | 16,0 % · 9,70 | 14,5 % · 8,58 | 12,1 % · 3,10 | 9,3 % · 2,34 | 12,8 % · 5,13 | 11,5 % · 4,58 |
| `geom_send_all` (hors volet) | 1,5 % · 0,94 | 1,7 % · 1,02 | 2,0 % · 0,51 | 1,5 % · 0,38 | 3,9 % · 1,55 | 3,3 % · 1,32 |

2. Le reste de `compute_state` est l'effacement (`glClear` s'en sert pour
   savoir si l'effacement est complet), hors de `send_state`.

**Lecture** : `compute_state` sort du profil ; `send_state` passe de 3,8 à 2,1 %
dans DOOM 3 (1,06 ms/image de moins), de 3,2 à 1,3 % dans UT2004. Le gain
d'image suit à peu près (DOOM 3 −1,4 ms, UT2004 −0,6). Ce qui reste de
`send_state` (≈ 1,3 ms/image dans DOOM 3) : la copie des fenêtres (propre de
`send_state`, `__memcpy`), les clés chaudes des dessins dont le verdict des unités
change (DOOM 3 alterne ses textures d'un dessin à l'autre : ~1 470 `SET_STATE`
chauds par image, 63 % des dessins) et leur `reserve`.

### Matrice complète, option allumée

`bench/a4/etat/matrice-stblk/tableau.md` (copie A4, `--env POMPPC_GL_STATEBLK=1`,
affichage natif, outils de rejeu construits depuis la branche : `MATRICE_BIN`),
0 autre QEMU sur les 29 relevés de charge : **14 vertes sur 16**, images justes
partout où il y a une image (rejeu = VM et référence à 0,00 %). ms/image
fenêtre / plein écran : Marble Blast 10,1 / 10,1 ; Zenerchi 4,1 ; DOOM 3 59,3 / — ;
Prey 70,9 / 69,2 ; UT2004 25,8 / 25,7 ; Warcraft III — / 17,5 ; Colin McRae — /
68,2 ; Nexuiz ARB 108,7 / 106,8 ; Nexuiz GLSL 39,8 / 39,9.

Les deux rouges, rejouées (`bench/a4/etat/rouges/`) :

- **UT2004 plein écran** : « rejeu ≠ VM » (la capture de la VM montre une autre
  image que l'image 393 rejouée ; le rejeu de la référence est juste). Rejouée
  trois fois : **verte 3 sur 3** (option allumée deux fois, éteinte une fois). Aléa
  de capture, du même genre que celui du TODO pour Prey fenêtre.
- **DOOM 3 plein écran** : le jeu meurt au chargement (image ~180, `exit 139`,
  tas du jeu corrompu : `idHeap::MediumAllocateFromPage` ou
  `RB_STD_T_RenderShaderPasses` selon la partie). Trois fois de suite dans la
  même session de la VM (après ~2 h 30 de parties), **dont une option éteinte** ;
  puis vertes 2 sur 2 dans la même session, 4 sur 4 après un redémarrage de la VM
  (option allumée, 58,8-58,9 ms/image), et 4 sur 4 sur le QEMU de référence avec
  le même plugin. Non attribuable au bloc d'état (il se produit sans lui) ; la
  cause n'est pas trouvée, et la copie A4 de QEMU n'est pas exonérée (0 sur 4 sur
  la référence, 3 sur 11 sur la copie, toutes dans une seule session).

## 5. Ce qui reste

- **Allumer par défaut** : l'équivalence est prouvée et le gain mesuré
  (DOOM 3 −1,4 ms/image, UT2004 −0,6) ; reste, avant `STATEBLK_DEFAULT 1`, à
  comprendre le plantage de DOOM 3 plein écran vu dans une session de la copie A4
  (ci-dessus). Décision de l'intégrateur, avec la v23. Il faut le QEMU qui annonce
  `QGPU_CAP_STATE_BLOCK` : sans lui, ancienne voie.
- **Clés chaudes** : l'identifiant de texture hôte et l'activation des
  programmes ne peuvent venir que de l'invité. Pour les sortir aussi, le cœur
  devrait connaître la table des textures de GLEngine (l'objet par unité) et la
  correspondance objet → identifiant hôte : un second bloc, ou un tableau
  « objet GLEngine → qtex » tenu par le device. Gain possible ≈ 0,3 à 0,6 ms/image
  dans DOOM 3.
- **`geom_send_all`** (matrices, lumières, texgen : 1,5 à 3,9 % du fil
  principal) n'est pas dans ce volet ; la même méthode s'y applique (matrices
  brutes dans le flux, comparées par le cœur ; les bits `+08` du bloc de
  changements disent lesquelles ont bougé).
- **`raw_scan_nan`** (13 % de Nexuiz ARB, 6 % de Warcraft III) : géométrie, noté
  pour le volet empaquetage.
- **Non vérifié** : A/B de vitesse sur Prey, Colin McRae, Warcraft III, Marble
  Blast (seulement l'équivalence et la matrice) ; RTCW absent du disque.
- **Piège de mesure** (vu pendant cet A/B) : arrêter la VM lancée par le
  frontend ImGui (`shutdown -h` de l'invité) laisse le processus
  `frontend/build/pomppc` vivant, à 40-60 % d'un cœur et sur le GPU de l'hôte ;
  avec lui, DOOM 3 passe de 61 à 107-112 ms/image (et 21 au lieu de 17,5 en
  cinématique). Toute mesure : `pgrep -fl frontend/build/pomppc` avant, et
  affichage natif (`POMPPC_FRONTEND=native`) pour comparer au QEMU de référence
  lancé en `-display cocoa`.
