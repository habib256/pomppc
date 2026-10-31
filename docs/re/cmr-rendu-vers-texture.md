# Colin McRae : le rendu vers texture d'IndirectX tenu par l'hôte (27/09/2026)

Suite de `cmr-var.md` §6. La géométrie était juste depuis le 26/09, mais **la VM affichait l'image
d'Apple** (noire et blanche, sans textures) et le rejeu « juste » ne l'était pas : de grands aplats
blancs couvraient la route, la paroi et le fond (`.run/cmr/r14/rj-0005-f45417.png`, remarque de
l'utilisateur). Ce document dit comment IndirectX rend vers une texture, ce que l'hôte fait
maintenant (protocole **v20**, plugin **`20260927-rtt`**) et d'où venaient les aplats blancs.

Légende : [E] établi par une expérience, [L] lu dans le code désassemblé, [H] hypothèse.

## 1. Le mécanisme d'IndirectX

**Le choix de la méthode** [L] : `mRenderTargetMethod` vient de la clé `RenderTargetMethod` des
réglages ; à défaut elle vaut **1** si le rendu annonce `GL_EXT_texture_rectangle` ou si la carte
est une ATI, **2** sinon ; 3 = aucune cible.

- **Méthode 1** (`TextureFromRenderTarget`) : `aglSwapBuffers(cible)` puis **un seul**
  `aglSurfaceTexture(ctx courant, cible de la texture (texture+0x2c), GL_RGBA8, ctx de la cible)`.
  La texture est ensuite échantillonnée telle quelle.
- **Méthode 2** : `glCopyTexImage2D` dans une texture tampon puis un quad retourné
  (`vert_array` : t = 1 en bas). Le retournement dit en creux l'orientation d'une texture de
  surface : **ligne 0 = haut** de l'image rendue.

Nous tenons la méthode 1, celle que le jeu prend avec l'extension rectangle annoncée (la 2 aurait
été plus simple, mais le chemin par défaut est celui-là ; aucun réglage à changer).

**Les cibles** [E, `tools/guest/gltrap` `POMPPC_GLTRAP_AGL`, `.run/cmr/t1/gltrap.txt`] : des
fenêtres Carbon **cachées** (`CreateNewWindow` classe 6, jamais de `ShowWindow`), chacune avec son
contexte AGL partagé avec le principal :

| cible | rôle | par image |
|---|---|---|
| 800×600 | masque d'ombres et de reflets de la course | seule sa profondeur est effacée ; échangée deux fois |
| 400×300, 200×150 | passes de halo (glow) / flou | `glCopyTexSubImage2D` vers les textures **RECT** 82/83, puis quad de réduction en pipeline fixe, 2D **et** RECT allumées (masque 0xc) |
| 256×256 | décalques au chargement | — |

Le contexte principal est plein écran (`aglSetFullScreen`, 800×600, changement de mode). En course,
le plugin voit **trois échanges par image du jeu** (plein écran + deux de la cible 800×600).

**Le chemin d'`aglSurfaceTexture`** [L] : AGL appelle `CGLGetSurface(ctx)` → (cid, wid, sid), puis
`CGLSetParameter(cur, 999, {sid, target, ifmt, w, h, type})` ; CGL le passe à `gliSetInteger`
(999) de GLEngine, qui fait `gleCheckTexImage2DArgs` et pose le drapeau u16 `0x400` dans les bits
`0x3C00` de l'objet texture. Côté plugin : `gp = DT_PARAMS` (gleTex+0x24),
`GLD_U8(gp,0) & 0x3c == 0x04` ⇒ texture de surface, `GLD_U32(gp,8)` = sid ; les niveaux n'ont
**pas de données**. Ce sid est celui du drawable à **+0x88** (`GD_TSID`), pas `GD_SID` (+0x90) [E].

**Apple ne sait pas** [E, `gltest rect`, `rtt`] : sans le bit 25 des extensions GLEngine refuse la
cible RECT (0x501) ; avec, le rendu logiciel d'Apple rend les textures de surface **blanches**. Et
`POMPPC_GL_RECT=0` fait planter Colin McRae chez Apple (échantillonneur nul dans
`glrPolyRGB000`) : ce n'est pas un mode d'essai valable pour ce jeu.

## 2. Ce que fait l'hôte (v20, `docs/protocole-v20-surface-texture.md`)

1. **Textures rectangle** : bit 25 annoncé (`GL_EXT/ARB_texture_rectangle`),
   `GL_MAX_RECTANGLE_TEXTURE_SIZE` (`cfg+0xc0`) = `QGPU_MAX_TEX_DIM` ; `TEX_CREATE3`/`TEX_IMAGE3`
   RECT, niveau 0 seul, paramètres assainis (filtres mip → LINEAR/NEAREST, REPEAT → CLAMP_TO_EDGE),
   `COPY_TEX` vers RECT (la cible vient de la texture quand GLEngine passe 0).
2. **Textures de surface** : `QGPU_OP_SURF_TEX [tex, cible, niveau, surf]` recopie **dans l'hôte**
   la surface (FBO) de la cible dans le niveau de la texture, sans retournement. Le plugin
   l'émet au premier usage puis à chaque nouvel échange de la source (`surf_gen` / `swap_gen`) ;
   aucun aller-retour par l'invité. Si la surface source est plus récente côté Apple
   (`SW_NEWER`), il se tait.
3. **Drawables cachés** : fenêtre absente de `CGSGetOnScreenWindowList` (revérifiée toutes les
   `DIRECT_RECHECK` images). Leurs échanges ne présentent rien (`direct_noop` au lieu de
   `present_direct`), `sync_to_sw` ne les relit pas, et un rattachement à la même taille garde
   l'état hôte. Fini les relectures à chaque rattachement et les replis sur ces échanges.
4. **Unités échantillonnées sans texture** : un programme de fragments qui lit `texture[u]` sur une
   unité sans texture lisait l'ancienne texture liée côté hôte ; le cœur relève les cibles lues
   (`fp_samples`, à `PROG_STRING`) et l'hôte y lie la texture 0.

`POMPPC_GL_RECT=0`, `POMPPC_GL_SURFTEX=0`, `POMPPC_GL_HIDDEN=0` rendent l'ancien comportement
(sonde `POMPPC_GL_UNITPROBE=1` : unités coupées faute de niveau, une note par texture).

## 3. D'où venaient les aplats blancs (rejeu du 26/09)

La remarque de l'utilisateur était juste : `r14/rj-0005` n'était **pas** juste. Deux hypothèses
tranchées en sautant les lots un à un dans le rejeu (`QGPU_REPLAY_SKIPDRAW`, script de zone
modifiée) :

- **Pas le brouillard** [E] : sa couleur est (0,55 ; 0,33 ; 0,29), pas du blanc, et son facteur
  (formule du brouillard D3D dans le programme) est juste ; les lots qui font le blanc sont
  ceux qui suivent.
- **Les cibles de halo** [E] : la texture RECT de copie était jugée incomplète par `tex_cp` (aucun
  niveau côté invité : la texture n'existe que dans l'hôte), l'unité coupée, le quad de réduction
  envoyé sans coordonnées de texture : les cibles 400×300 et 200×150 sortaient **blanches**, puis
  s'ajoutaient à l'image. Correctif : une texture `host_only` créée dans l'hôte est complète
  (niveau de base, filtre sans mip ou rectangle).
- **Le menu blanc** [E] : un quad de fondu de couleur `0x00000000`, que le plugin remplaçait par du
  blanc (substitution « couleur morte » de Warcraft III). Sous `GL_APPLE_vertex_array_range` la
  couleur est vraie : la substitution est sautée pour ces lots (`var_draw`, dans la clé du
  paqueteur).

## 4. Vidage autonome

La cible 800×600 n'efface que sa profondeur : sa couleur garde le contenu des images d'avant, que
le rejeu n'avait pas (voiture différente de la capture). Au déclenchement, le vidage relit
maintenant l'état hôte qui n'existe pas dans l'invité : `QGPU_OP_TEX_READBACK` pour les textures
`host_only` et de surface (réécrites en tête du vidage par `TEX_IMAGE3`), `SURF_READBACK` +
`SURF_UPLOAD` pour chaque surface vivante, et `surfaces.txt` (« ctx Q surf S W H stencil ») que
`qgpu_replay` lit pour recréer contextes et surfaces. La matrice range `surfaces.txt` avec la
référence.

## 5. Épreuves (27/09)

| Épreuve | Avant (plugin `20260926-var`) | Après (`20260927-rtt`, QEMU v20) |
|---|---|---|
| Capture de la VM en course | noir et blanc : `.run/cmr/rtt/avant-course.png` | la spéciale : `.run/cmr/rtt/apres-course.png` |
| Menu | `.run/cmr/rtt/avant-menu.png` | `.run/cmr/rtt/apres-menu.png` |
| Capture VM contre rejeu (`.run/cmr/t11-course`, image 46188) | — | **0,000** d'écart |
| Replis par image en course | 0 en course, ~2 800 en démonstration (halo) | **0** |
| ms par image du jeu au départ (voiture arrêtée) | 70,4 | 70,2 (≈ 14 img/s) |
| `VERDICTCHECK=1 STATECHECK=1` | 0 écart | **0 écart** jusqu'à l'image 39 500 |
| Matrice, cellule `cmr-pe` (`tools/matrice/jeux/cmr.py`) | non automatisée | **verte** : tours `20260927-0228` (référence validée) et `20260927-0307` (72,6 ms/image, 0 repli, 0,00 %) |
| `gltest rect`, `rectfp` | 0x501 | justes |
| `gltest/rtt 2d`, `rtt rect` (AGL, fenêtre cachée → texture) | Apple : blanc | justes (orientation, tampon avant, mise à jour après échange) |

Toutes les autres scènes `gltest` : mêmes codes de retour et mêmes empreintes d'image qu'avec
l'ancien plugin (échecs connus inchangés : `clip` 1, `matbegin` 2, `tex14` 1 ; `bigstrip`,
`fusion`, `lit`, `texgen` plantent aussi chez Apple ; `v15` plante sous le plugin, ancien compris).

## 6. Ce qui reste

- Les copies `SURF_TEX` et `COPY_TEX` passent par une relecture CPU côté hôte (`be->readback`) ;
  une copie GPU (FBO → texture) serait plus rapide. 70 ms/image restent dominés par le TCG.
- Coordonnées de texture en mode immédiat sous programme de sommets : perdues (limite ancienne,
  vue en écrivant `rectfp` ; la scène passe par des tableaux).
- Mode fenêtre : le jeu n'en a pas (dialogue d'options : résolution, couleurs, FSAA).

## 7. Les reflets trop blancs de la carrosserie (31/10/2026)

Remarque de l'utilisateur (PC) : « les reflets sortent trop blancs » sur la voiture. **Cause : le
cœur qgpu de l'hôte, pas le TCG ni le plugin.** Corrigé dans `patches/qgpu/qgpu-core.c`
(`unit_texture_bound`) ; `build/` et `build-fast/` du PC reconstruits.

**Ce que dessine la carrosserie** [E, rejeu natif du vidage de la cellule `cmr-pe`, programmes
sortis par `QGPU_REPLAY_PROGS`] : `Car_Ext.arp`, en deux variantes engendrées par IndirectX
selon la texture de l'unité 1 — programme `texture[1], 2D` (carte d'environnement 64×64 DXT1,
texture du jeu 1030) ou `texture[1], RECT` (cible de rendu ; `tex_factor1` = `program.local[1]`
du programme de sommets vaut alors **256 256**, les coordonnées en texels d'une cible 256×256).
Le terme `MAD r0.rgb, t1, r1.a, r0` ajoute la carte à la peinture. En éteignant ce seul terme
dans le texte du programme (vidage recopié et modifié à longueur égale), la carrosserie redevient
bleue ; seul, il est presque blanc avec des bandes noires.

**Ce que le jeu lie** [E, `gltrap` avec `POMPPC_GLTRAP_BINDS=1` (nouveau : unité active par
`glActiveTextureARB`, que le jeu appelle et que `gltrap` ne suivait pas), cellule `cmr-trace2-3110`
du 31/10] : sous la variante RECT (programme 18 du jeu), unité 1 = texture rectangle **105**, que
rien ne remplit (aucun `aglSurfaceTexture` : les trois de la partie visent les cibles 800×600,
400×300 et 200×150 ; aucune copie). La sonde du plugin (`POMPPC_GL_UNITPROBE=1`) le confirme
depuis GLEngine : `UNITE coupée u1 masque 4 fp 1 … niv0 0x0 données 00000000 (GLEngine 0x0
00000000)`. Texture incomplète : sur un vrai Mac, elle se lit en noir, **pas de reflet ajouté**.
Le jeu se juge lent (`ValLowMachine`/`ValLowConfig` remis à 1 à chaque lancement même effacés) et
ne rend sans doute pas cette carte ; c'est le comportement d'un vrai Tiger sur un vrai PowerPC,
gardé tel quel (consigne de l'utilisateur).

**Le défaut** : sous programme de fragments, le plugin allume une unité d'après la cible que le
TEXTE échantillonne (`unit_mask`) et l'**éteint** quand la texture n'a pas d'image, en gardant
son `QGPU_SK_U_BIND` d'avant. `unit_texture_bound` du cœur prenait ce `BIND` sans regarder
`QGPU_SK_U_ENABLE` (« Direct3D n'allume rien », vrai pour `glEnable`, pas pour l'allumage du
plugin) : la carrosserie lisait la texture du dessin précédent — le **masque d'ombres et de
lumière de la piste** (1078, cible 800×600, presque blanc, lu sur son coin 256×256) après la
piste, ou la carte 2D d'une autre voiture. D'où le blanc. Relevé sur l'hôte (sonde de rejeu
temporaire) : 1 280 dessins `Car_Ext`/`Car_Lights` RECT ainsi servis dans un vidage de 30 images.

**Épreuves** : rejeu du vidage corrigé : seule la voiture change (7 % des pixels de l'image,
tous sur la carrosserie) ; les références validées du PC rejouées avec le cœur corrigé : d3-fen,
d3-pe, mb-fen, mb-pe, ut-fen, ut-pe, zen-fen **0,00 %** ; cmr-pe 5,00 / 7,53 % (le faux reflet).
Ancienne référence gardée dans `bench/matrice/ref-linux/cmr-pe.faux-reflet-2810/`, nouvelle
référence prise le 31/10 (à valider à l'œil). Le M4 : même `qgpu-core.c`, à reconstruire.
