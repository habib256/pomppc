# GPU 3D sous Tiger PPC — le plugin OpenGL qui rend sur le GPU de l'hôte

Document d'implémentation — POMPPC, septembre 2026.
Cible : Mac OS X 10.4.6 PPC invité sur QEMU `mac99` ; hôte Apple Silicon (macOS 26) ou Linux.
Prolonge `docs/gpu-tiger-4060ti.md` (affichage 2D, `qfb-pci`), qui reste valable pour l'écran.

---

## 0. Verdict

**La demande** — « un kext qui soit un wrapper vers Vulkan, OpenGL ou DirectX pour exploiter la
puissance des cartes graphiques modernes » — est **réalisée**, mais pas là où le mot « kext » le
suggère : un kext Tiger tourne dans un noyau PowerPC de 2005 et ne peut appeler aucune API
graphique de l'hôte. Le pont est en trois couches, et une seule est un kext :

| Couche | Où | Rôle | État |
|---|---|---|---|
| **A. Device `qgpu-pci`** + cœur + backends | hôte, `patches/qgpu/` | exécute des flux de commandes sur le GPU de l'hôte (backend OpenGL : CGL sur macOS, EGL sur Linux ; backend logiciel de référence) | **fait, testé** |
| **B. Kext `POMPPCGPU`** | invité, `kext/POMPPCGPU/` | transport : tranches de fenêtre partagée par processus, doorbell, IRQ, nettoyage | **fait, chargé et testé dans Tiger** |
| **C. Plugin OpenGL `GLDriver-POMPPC`** | invité, `guest/gldriver/` | chargé par `OpenGL.framework` comme un pilote : traduit la rastérisation en commandes qgpu | **fait, testé dans Tiger, hors écran et en fenêtre** |

« Vulkan » et « DirectX » se branchent au même endroit que le backend OpenGL (couche A, par
ANGLE, Zink ou un backend natif) : l'invité n'en sait rien.

**Mesures dans Tiger** (même programme, même image, rendu Apple `Generic` contre POMPPC) :

| Scène | Taille | Apple logiciel | POMPPC | Gain |
|---|---|---|---|---|
| couloir multitexture + brouillard (`game`) | 640×480 | 3,0 img/s | 574 img/s | ×190 |
| plan en perspective, trilinéaire (`texpersp`) | 512×384 | 14,2 img/s | 837 img/s | ×59 |
| 40 grands triangles, profondeur + mélange (`fill`) | 512×512 | 4,6 img/s | 490 img/s | ×107 |
| 400 petits triangles lissés (`spin`) | 512×512 | 155 img/s | 377 img/s | ×2,4 |
| fenêtre GLUT double tampon (`glwin`) | 320×240 | 81 img/s | 101 img/s | ×1,2 |

Plus la scène remplit de pixels, plus le gain est grand ; sur de petites scènes, le coût de la
relecture de l'image vers l'invité domine. Écart d'image avec le rendu d'Apple : moyenne
inférieure à 2 niveaux sur 255, concentrée sur les pixels d'arête (règle de remplissage du GPU
hôte) ; aucun écart sur les scènes sans arête diagonale.

---

## 1. Architecture

```
 INVITÉ Tiger 10.4 PPC (big-endian)                          HÔTE (QEMU, little-endian)
 ┌───────────────────────────────────────────────────┐       ┌─────────────────────────────┐
 │ application OpenGL (CGL, AGL, NSOpenGL, GLUT)     │       │                             │
 │   └ OpenGL.framework → GLEngine                   │       │ qgpu-pci.c    transport      │
 │        (transformations, éclairage, découpage,    │       │   │ doorbell                  │
 │         élimination des faces)                    │       │   ▼                           │
 │        └ C. GLDriver-POMPPC.bundle ─┐              │ BAR0  │ qgpu-core.c   validation,    │
 │             ├ procédures accélérées │ flux qgpu   │◄─────►│   │ objets, état GL          │
 │             └ GLDriver d'Apple      │ + sommets   │fenêtre│   ▼                          │
 │               (module privé, pour   │ + texels    │       │ qgpu-gl.c → OpenGL → GPU     │
 │                tout le reste)       ▼             │ BAR1  │ qgpu-soft.c (référence)      │
 │        B. POMPPCGPU.kext (IOUserClient, IRQ)      │◄─────►│                             │
 └───────────────────────────────────────────────────┘ regs  └─────────────────────────────┘
```

Principes fixés une fois pour toutes :

- **Un seul contrat, un seul fichier** : `qgpu_proto.h` (macros uniquement), copié à l'identique
  côté hôte et côté invité ; `tests/run-all.sh` refuse toute divergence.
- **Zéro échange d'octets dans l'invité** : registres `DEVICE_BIG_ENDIAN`, flux en mots
  big-endian, flottants IEEE big-endian, pixels ARGB big-endian (le format du WindowServer).
- **Toute la validation est côté hôte**, dans `qgpu-core.c`, avant le backend : bornes, identifiants,
  énumérations GL, NaN. Un flux invalide s'arrête à la commande fautive, `STATUS_PC` la désigne.
- **Le plugin est un mandataire du rendu d'Apple.** Ce qu'il n'accélère pas est rendu par le code
  d'Apple, donc exact. L'accélération n'est jamais une condition de correction.

---

## 2. Livrables

| Fichier | Rôle |
|---|---|
| `patches/qgpu/qgpu_proto.h` | **le contrat** v6 : registres, opcodes, clés d'état, tranches des clients |
| `patches/qgpu/qgpu-core.[ch]` | analyse et validation du flux, contextes, surfaces, textures ; indépendant de QEMU |
| `patches/qgpu/qgpu-soft.c` | backend logiciel de référence : pipeline OpenGL 1.x par fragment |
| `patches/qgpu/qgpu-gl.c` | **backend OpenGL** : CGL (macOS) / EGL (Linux), FBO + profondeur par surface |
| `patches/qgpu/qgpu-pci.c` | device QEMU ; `backend=auto\|soft\|gl`, `shmem_mb`, `trace=on` |
| `kext/POMPPCGPU/` | le kext : 4 clients, une tranche de 16 Mio et une plage d'identifiants chacun |
| `guest/gldriver/` | **le plugin OpenGL** (`pomppc_gld.c` points d'entrée, `pomppc_accel.c` accélération, `pomppc_qgpu.c` client du kext, `gld_tramp.s` trampolines générés), `install.sh` |
| `guest/gltest/` | `gltest` (hors écran, 12 scènes avec pixels témoins) et `glwin` (fenêtre GLUT) |
| `guest/qgpu-test/` | test du transport seul |
| `tests/qgpu_core_test.c` | test natif hôte : 53 vérifications par backend, soft et GL |
| `tests/qgpu_smoke.py` | bout en bout sans invité, depuis Open Firmware |
| `tools/re/ppcanno.py` | désassembleur PowerPC annoté (références PIC résolues) pour la rétro-ingénierie |
| `tools/gld/gen_tramp.py` | générateur des trampolines du plugin |
| `tools/guest/` | boucle de développement dans l'invité (`devloop.py`, agent, relais de session) |

---

## 3. Le protocole qgpu (v6)

Flux de mots big-endian ; en-tête `opcode << 16 | longueur en mots`. Détail : `qgpu_proto.h`.

| Groupe | Opcodes |
|---|---|
| objets | `CTX_CREATE/DESTROY/BIND`, `SURF_CREATE` (couleur + profondeur), `SURF_DESTROY/BIND` |
| transferts | `SURF_READBACK/UPLOAD` (ARGB), `DEPTH_READBACK/UPLOAD` (flottants dans [0,1]) |
| dessin | `CLEAR`, `DRAW_TRIANGLES` (8 mots/sommet), `DRAW_TRIANGLES_TEX` (12), `DRAW_TRIANGLES_TEX2` (16), `DRAW_LINES`, `DRAW_POINTS` |
| textures | `TEX_CREATE/DESTROY`, `TEX_IMAGE` (niveau, format de base), `TEX_PARAM` (filtres, répétition) |
| état | `SET_STATE` : profondeur (test, fonction, écriture), masque de couleur, mélange (facteurs, équations), test alpha, ciseaux, deux unités de texture (liaison, mode et couleur d'environnement), brouillard (actif, couleur), largeur de ligne, taille de point, décalage de polygone |

Sommet : `x y z f r g b a [s t r q]×n`, coordonnées fenêtre en pixels (origine en haut à gauche),
`z` dans [0,1], `f` = facteur de brouillard déjà calculé par GLEngine, coordonnées de texture déjà
divisées par w (projectives : l'hôte interpole puis divise par q).

**Plusieurs processus.** Le kext découpe BAR0 en 4 tranches de 16 Mio et donne à chaque client
une plage d'identifiants (4 contextes, 16 surfaces, 128 textures). Un 5e client est refusé et son
plugin rend en logiciel ; à la fermeture d'un client, le kext détruit ses objets.

---

## 4. Le plugin : ce que la rétro-ingénierie a établi (Tiger 10.4.6)

Rien de ce qui suit n'est documenté par Apple. Tout a été lu dans l'image Tiger avec
`tools/re/ppcanno.py`, puis **vérifié sur pièces** avec le plugin en mode traceur
(`POMPPC_GLTRACE`, vidages binaires comparés d'un réglage GL à l'autre).

### 4.1 Chargement et identité

- `GLEngine` (`_glepLoadRenderers`) charge d'abord les bundles nommés par la propriété IOKit
  `IOGLBundleName` des accélérateurs, puis, dans `Resources/` d'`OpenGL.framework` — **ou dans le
  dossier de la variable `GL_RESOURCES`** —, tout bundle dont le nom commence par `GLDriver`, plus
  `GLRendererFloat`. Disposition plate : `Dossier/Nom.bundle/Nom`.
- `_glepPluginConnect` résout **61 symboles `gld*`** dans une table de la structure plugin
  (`+0x114` à `+0x204`), plus `gldInitializeLibrary(services, ?, masque, cb_flush, cb_surface)`.
- `_glepValidatePlugin` exige `gldGetVersion` = 2.4.11 et un **identifiant d'octet `0xff00`
  unique** : 0x0200 est le GLDriver d'Apple, le plugin prend 0x7700.
- Un pixel format est rattaché au plugin par cet octet de son identifiant de rendu : le plugin
  réécrit `0x02xx → 0x77xx` dans `RendererInfo` et dans les pixel formats, et restaure avant de
  rendre la main au code d'Apple.
- CGL retient **le premier renderer qui convient** : le bundle s'appelle `GLDriver-POMPPC` pour
  trier avant `GLDriver.bundle` (« - » < « . »). Il est aussi le seul à répondre à
  `kCGLPFAAccelerated` (l'attribut 73 est retiré de la copie passée au GLDriver d'Apple, et le
  bit `0x100` posé dans nos formats).
- `RendererInfo` : `+0x04` identifiant, `+0x08` drapeaux (`0x100` accéléré, `0x4` hors écran),
  `+0x10`/`+0x18` modes couleur/profondeur, `+0x30` mémoire vidéo.
- Sans WindowServer (single-user), CGL compte zéro écran et échoue, **sauf** si les attributs
  contiennent `kCGLPFARemotePBuffer` (91), qui interdit la connexion au WindowServer.

### 4.2 Où passe le rendu

`gldInitDispatch(ctx, procs, …)` remplit une **table de procédures de rastérisation** ; GLEngine
transforme, éclaire, découpe, élimine les faces, puis appelle ces procédures avec des sommets de
0x100 octets en coordonnées fenêtre :

| Offset | Procédure | Signature relevée |
|---|---|---|
| `+0x04` | `Clear` | `(ctx, masque GL)` |
| `+0x08` `+0x0c` `+0x10` | `ReadPixels`, `DrawPixels`, `CopyPixels` | |
| `+0x18` `+0x40` | `RenderPoints`, `…Ptr` | `(ctx, sommets \| pointeurs, n, drapeaux)` |
| `+0x1c` `+0x20` `+0x24` `+0x44` | `RenderLines`, `LineStrip`, `LineLoop`, `LinesPtr` | idem ; `LinesPtr` : paires de pointeurs |
| `+0x28` `+0x48` | `RenderPolygon`, `…Ptr` | idem ; couleur plate = premier sommet |
| `+0x2c` `+0x34` `+0x38` `+0x3c` | `Triangles`, `TriangleStrip`, `Quads`, `QuadStrip` | idem ; couleur plate = dernier sommet |
| `+0x30` | `TriangleFan` | `(ctx, pivot, sommets suivants, n, drapeaux)` |
| `+0x58` `+0x5c` `+0x60` | échange / vidage | appelées par `glFinish`, `glFlush`, `CGLFlushDrawable` |
| `+0x74` `+0x78` `+0x7c` | `CopyTexSubImage`, `ModifyTexSubImage`, `GenerateTexMipmaps` | `(ctx, texture, …)` |

Sommet : `+0x00` x, `+0x04` y (fenêtre, origine en bas), `+0x08` z (échelle : flottant en
`ctx+0x14`, 1,0 = 0x3fffffc0), `+0x30` couleur RGBA, `+0x4c` facteur de brouillard,
`+0x80`/`+0x90` s t r q des unités 0 et 1 (déjà divisés par w).

Contexte du GLDriver : `+0x0c` état GL de GLEngine, `+0x10` table des textures liées
(unité·0x14 + cible·4, 2D = 3), `+0x1c/+0x20` taille du drawable, `+0x48` pixels par ligne,
`+0x88` tampon de profondeur (mots de 32 bits, lignes de haut en bas), `+0x94` → adresse du tampon
de dessin (ARGB 32 bits, lignes de haut en bas), `+0xc0`/`+0xcc` bits couleur/profondeur,
`+0x6f4` texturage effectif.

### 4.3 État GL de GLEngine (offsets relevés)

| Offset | Champ | Offset | Champ |
|---|---|---|---|
| `0x2d60` | réf. alpha (f) | `0x2e0a` | brouillard actif |
| `0x2d64` | fonction alpha (u16) | `0x2e14` | indication de brouillard (u16) |
| `0x2d66` | test alpha | `0x2e20` | largeur de ligne (f) |
| `0x2d68..6e` | facteurs de mélange (u16 ×4) | `0x2e2c` / `0x2e2d` | stipple / lissage de ligne |
| `0x2d80..82` | équations de mélange | `0x2e33` | opération logique |
| `0x2d84` | mélange actif | `0x2e40..43` | masque de couleur R G B A |
| `0x2d88` | profondeur d'effacement (double) | `0x2e44` | masque de profondeur |
| `0x2da0` | couleur d'effacement (f ×4) | `0x30bc` / `0x30dc` | taille / lissage de point |
| `0x2dc4` | fonction de profondeur (u16) | `0x3168` / `0x316c` | décalage : facteur / unités |
| `0x2dc8` | test de profondeur | `0x3170` | modes polygone (u16 ×2) |
| `0x2de0` | couleur du brouillard (f ×4) | `0x3178`/`79`/`7b`/`7c`/`7d` | stipple, lissage, décalages point/ligne/plein |
| `0x2e04` | mode du brouillard (u16) | `0x3180` / `0x3190` | ciseaux (i32 ×4) / actif |
| | | `0x3194` | ombrage (0x1d00 plat) |
| | | `0x31c0` | stencil (bit 0) |
| | | `0x31c4 + u·0x7c` | unité u : couleur d'env. (f ×4), `+0x10` cibles actives, `+0x14` mode d'env. |

Texture du GLDriver : `+0x000` → paramètres de GLEngine (`+0x10/+0x12` répétition,
`+0x16/+0x18` filtres), `+0x078 + n·0x74` niveau n (largeur, hauteur, bordure, pixels par ligne,
format, type, pointeur de données), `+0x578` format de base.

### 4.4 Synchronisation invité ↔ hôte

La surface hôte double le tampon de dessin du GLDriver. Chaque tampon (couleur, profondeur) porte
un drapeau `SYNCED` / `HOST_NEWER` / `SW_NEWER` : avant un dessin hôte, ce que le logiciel a
dessiné est téléversé ; avant un échange, un vidage ou un chemin logiciel, ce que l'hôte a dessiné
est relu. Un effacement complet ne téléverse rien ; la profondeur n'est relue que si un chemin
logiciel s'en sert. Les triangles de plusieurs appels consécutifs sont regroupés en une seule
commande de dessin.

Le drapeau `0x80` de `gldUpdateDispatch` (« tampon de dessin changé ») arrive aussi à chaque
`CGLFlushDrawable`, sans changement réel : la synchronisation est donc paresseuse. Après la mise à
jour, si l'adresse du tampon a vraiment changé, l'image hôte est relue dans l'ancien tampon, qui
reste alloué.

**Présentation directe.** `gldSwapBuffers` (procédure `+0x60`) ne fait que sauter vers
`glsSwapBuffers(drawable, ctx+0x60, …)` de `libGLSystem`, qui recopie le tampon dans la mémoire de
la fenêtre puis attend que le WindowServer l'ait affichée. Quand l'application est au premier plan,
menus cachés, avec un drawable de la taille de l'écran en 32 bits, le plugin écrit l'image hôte
droit dans la mémoire vidéo (`CGDisplayBaseAddress`) et remplace l'échange par une procédure vide,
comme le `gldSwapNoop` d'Apple.

**En fenêtre** (Marble Blast y passait un tiers de chaque image), la même écriture vise le
rectangle de la surface à l'écran. La position vient de `CGSGetWindowBounds` et
`CGSGetSurfaceBounds` (SPI de CoreGraphics), l'identifiant de surface de `CGSGetSurfaceList`
(celui du drawable n'est pas toujours le bon). Garde-fous : application au premier plan ; aucune
fenêtre au-dessus ne touche le rectangle (`CGSGetOnScreenWindowList`, en ignorant le voile plein
écran du système et les tuiles 128x128 que le Dock gare en 0,0) ; curseur immobile s'il est dans
la surface — le curseur de la VGA de QEMU est composé en logiciel dans la mémoire vidéo, écrire
par-dessus l'efface jusqu'à son prochain mouvement. Un échange normal toutes les 90 images
rafraîchit la mémoire de la fenêtre. Un `glFinish` juste avant l'échange ne relit plus l'image.
Conditions réévaluées toutes les 10 images. `POMPPC_GL_DIRECT=0` coupe tout, `=f` limite au
plein écran, `=c` présente même avec un curseur en mouvement.

**Le mode asynchrone (protocole v9, tâche 2.2).** Jusqu'à la v8 l'hôte exécutait le flux *dans*
l'écriture MMIO du doorbell : le vCPU restait gelé pendant tout le rendu. La v9 met les soumissions
en file sur un thread de rendu hôte, et le plugin y dépose sans attendre (`POMPPC_GL_ASYNC`, défaut
**activé**). Ce qui change côté invité :

*Deux moitiés de tranche.* Tant que `FENCE` n'a pas dépassé une soumission, l'hôte peut lire ou
écrire à tout moment ce qu'elle désigne — flux de commandes, sommets, indices, texels — et écrire
dans ses zones de relecture. Écrire l'image *n+1* là où l'hôte lit encore la *n* ne planterait pas :
cela ferait **clignoter des triangles**, ce qui est bien pire à diagnostiquer. La tranche du client
(16 Mio) est donc coupée en deux moitiés de 8 Mio, alternées à chaque soumission ; les offsets de
flux, de sommets et d'indices sont relatifs à la moitié courante, **seule l'arène** est adressée en
absolu, parce qu'une copie différée survit au changement de moitié. Deux, et pas trois : à 5,3 Mio,
la troisième moitié ne laisserait plus la place à deux transferts plein écran dans l'arène.

*Où tombe l'attente.* Une soumission sans relecture — dessins, changements d'état, téléversements,
c'est-à-dire l'immense majorité — n'est **jamais** attendue. On n'attend qu'aux endroits où
l'invité a *besoin* du résultat : avant un chemin logiciel (`sync_to_sw_locked`), pour un compte
d'occlusion (`gldGetQueryInfo`), et quand il faut **reprendre** une moitié encore en vol. La
présentation directe, elle, place son attente le plus tard possible : au **début de l'échange
suivant**. L'écran montre alors l'image *n−1* pendant qu'on prépare la *n+1* — **une image de
latence, jamais plus**, et l'hôte a eu toute la construction d'une image pour finir la précédente
(mesuré : 0,1 ms d'attente restante par image). Contrepartie assumée : une application qui *cesse*
de dessiner laisse sa dernière image en vol jusqu'au prochain point de synchronisation, qui la
présente.

*Le `BeginPrimitiveBuffer` ouvert est le point délicat.* Entre `Begin` et `EndPrimitiveBuffer`,
GLEngine écrit les sommets **lui-même**, dans la moitié courante, à une adresse qu'on lui a déjà
donnée : on ne peut ni changer de moitié ni laisser celle-ci en vol. Un vidage qui survient dans cet
intervalle (vu en vrai : un téléversement de texture au milieu d'une primitive) repasse donc en
**synchrone** pour cette soumission-là. C'est rare — `geom_begin` fait la place avant d'ouvrir — et
c'est la seule façon d'être exact.

*Erreurs.* Avec plusieurs soumissions en vol, `QGPU_REG_STATUS` ne décrit que la dernière
**terminée** : il ne dit plus « tout s'est bien passé ». C'est `QGPU_REG_ERRORS` qui fait foi, et le
kext le rend à chaque soumission asynchrone, à la place de `status_pc` (qui n'a pas de sens à la
soumission). **Mais ce compteur est global au device**, et il monte sans que personne n'ait de
bogue : le balayage de fermeture du kext détruit les 148 identifiants de la plage d'un client, dont
la plupart n'existent pas — une erreur chacun. Relevé au premier essai : **8 877 erreurs** sur une
VM qui rendait des images justes depuis des heures. On ne peut donc ni nommer la soumission fautive
après coup (sa moitié a pu être réécrite), ni attribuer le compteur. D'où la règle, bornée et qui se
répare toute seule : le compteur bouge → **synchrone pendant 120 images**. Si l'erreur était la
nôtre elle est déterministe, la prochaine image la reproduit, et `broken_all` a alors *son* statut
et *son* `pc`, exacts, comme avant la v9 ; sinon on reprend l'asynchrone et on se recale. Vu en
vrai : un repli par application GL qui se ferme à côté, suivi d'une reprise.

*File pleine.* `QGPU_ST_QUEUE_FULL` ne met rien en file et n'avance rien : on attend notre plus
ancienne barrière (il n'y en a qu'une, l'autre moitié) et on réessaie ; si la file reste pleine —
un autre client l'occupe — on frappe le doorbell **synchrone**, qui n'est jamais refusé et attend
sa place. Sur Marble Blast, jamais rencontré.

### 4.6 Cas réel : Zenerchi (PlayFirst, 2007)

Jeu 2D Carbon/AGL, plein écran en 800x600, quelques centaines de quads texturés par image. Mesures
dans l'invité à 2 cœurs (`POMPPC_GL_STATS=<fichier>`, outil `sample` de Tiger) :

| Étape | Partie en cours | Cause levée |
|---|---|---|
| départ | ~1 img/s, chargement de plusieurs minutes (« écran blanc ») | textures en BGRA + `UNSIGNED_INT_8_8_8_8` refusées : tout passait par le rastériseur logiciel |
| formats compacts | 16 img/s | 41 % du temps à attendre le WindowServer dans `glsSwapBuffers` |
| présentation directe, recâblage en temps constant | 42 img/s | recherche linéaire parmi des centaines de textures (8 %) |
| table de hachage des textures | 50 img/s | le jeu est désormais limité par le processeur émulé |

Le rendu logiciel d'Apple seul ne démarre pas ce jeu : `aglChoosePixelFormat` échoue
(« invalid pixel format »). Un jeu PlayFirst au second plan se limite volontairement à 1 img/s.

### 4.5 Domaine accéléré

*(À jour du lot 3, 18/09/2026 — protocole v8.)*

Accéléré : effacement couleur/profondeur/**stencil** ; triangles, bandes, éventails, quads, bandes
de quads, polygones ; lignes, bandes et boucles de lignes ; points ; **toute la géométrie**
(matrices, viewport, 8 lumières, 2 matériaux, texgen, 6 plans de découpe, élimination des faces)
par le chemin brut, cf. §4.7 ; profondeur (toutes fonctions), masques, **stencil** (9 clés),
mélange — facteurs et équations d'OpenGL 1.x **plus, depuis la v8, la couleur constante
(`GL_CONSTANT_COLOR` et sœurs) et les équations `GL_MIN` / `GL_MAX`** —, test alpha, ciseaux,
ombrage lisse et plat, brouillard (facteur par sommet, ou calculé par l'hôte sur le chemin brut),
décalage de polygone (plein, **ligne et point**) ; **opérations logiques** (les 16) ; **pointillé
de polygone** (les deux chemins) ; **pointillé de ligne** et **modes de polygone** `GL_POINT` /
`GL_LINE` (chemin brut seulement, cf. §4.8) ; **requêtes d'occlusion** ; **quatre unités de
texture** 2D et 1D (formats de base ALPHA, RGB, RGBA, LUMINANCE, LUMINANCE_ALPHA, INTENSITY ;
données RGBA, RGB, BGRA, BGR, LUMINANCE, LUMINANCE_ALPHA, ALPHA, RED en octets ; RGBA et BGRA en
`UNSIGNED_INT_8_8_8_8` et `_REV`, BGRA en `UNSIGNED_SHORT_1_5_5_5_REV`, RGB en
`UNSIGNED_SHORT_5_6_5`, RGBA en `UNSIGNED_SHORT_4_4_4_4` ; filtres avec mipmaps,
REPEAT/CLAMP/CLAMP_TO_EDGE ; environnements MODULATE, REPLACE, DECAL, BLEND, ADD et **GL_COMBINE**
avec dot3).

Rendu par le code d'Apple (exact, plus lent) : cinq unités de texture ou plus, textures 3D, cube et
rectangle, **textures compressées**, modes de répétition autres que REPEAT/CLAMP/CLAMP_TO_EDGE,
filtres inconnus, lissage (`GL_*_SMOOTH`), **taille de point atténuée par la distance**, mode de
polygone non plein **par le chemin hérité**, pointillé de ligne par le chemin hérité, brouillard en
`GL_NICEST` hors chemin brut, lignes et points texturés ou décalés, opérations de pixels
(`glDrawPixels`, `glBitmap`, `glCopyPixels`, accumulation), programmes ARB de sommets ou de
fragments, tampons autres que 32 bits.

Le bilan `POMPPC_GL_STATS` nomme chaque sortie du domaine et son premier cas : depuis le lot 3,
les motifs `logicop/stipple/lissage` et `polygonmode` ne comptent plus que ce qui reste vraiment
hors domaine, et deux motifs sont nés — `param-texture` (une valeur de filtre ou de répétition que
le cœur refuserait, donc que le plugin n'envoie pas) et `taille-de-point-attenuee`.

### 4.7 Géométrie sur l'hôte — le chemin « brut » (protocole v7, lot 2)

Jusqu'ici, **GLEngine transformait, éclairait, découpait et éliminait les faces sur le PowerPC
émulé**, et le plugin ne recevait que des sommets en coordonnées fenêtre. C'était la limite : sur
Marble Blast, le débit suivait le nombre de triangles. Le chemin brut sort ce travail de l'invité.

#### Principe

`docs/re/verification-tcl.md` et `docs/re/descripteur-de-sommet.md` ont établi le mécanisme :

1. **Le verrou** est le **bit 0 de la valeur rendue par `gldInitDispatch`/`gldUpdateDispatch`**,
   que `_gleUpdateDispatchCodeChange` relit **à chaque changement d'état GL**. Le plugin rend
   `(retour d'Apple) | 1` quand l'état courant est dans son domaine, et le retour d'Apple **tel
   quel** sinon. Le bit 1 (`| 3`) force la reconstruction du chemin ; il n'est demandé que
   lorsque le descripteur change sans que le verrou change. `cfg+0x79 = 1` à la création donne la
   valeur initiale.
2. **Le descripteur de sortie de sommet**, publié en `cfg+0x11c` (durée de vie = celle du
   contexte), dit à GLEngine où écrire chaque attribut : `u8 n ; u8 0 ; u8 pasEnMots ; u8 0 ;`
   puis `n` entrées `(code << 10) | ((composantes − 1) << 8) | décalageEnMots`. Sans lui, GLEngine
   prend le chemin T&L et **jette la géométrie en silence**. Le code 6 (drapeau d'arête) plante
   GLEngine : il n'est jamais demandé.
3. `BeginPrimitiveBuffer` (+0x50) rend le tampon où écrire, `EndPrimitiveBuffer` (+0x54) dit
   combien de sommets y sont. **`BeginPrimitiveBuffer` ne peut pas refuser** : rendre 0 ferait
   écrire GLEngine à l'adresse nulle.

#### Zéro copie

Le descripteur est choisi pour que la disposition de GLEngine **soit** celle de `DRAW_RAW`
(position4, normale3, couleur4, brouillard1, coordonnées de texture 4 par unité, dans l'ordre fixe
du protocole), et `BeginPrimitiveBuffer` rend un pointeur **dans la fenêtre partagée**
(`VTX_OFF + G.vtx`). `EndPrimitiveBuffer` n'a plus qu'à écrire dix mots de commande : il n'y a
aucune recopie de sommet. Les commandes s'accumulent dans le flux existant et partent aux points
de synchronisation existants ; les `DRAW_RAW` consécutifs sont fusionnés quand le mode, l'état et
la contiguïté le permettent (`TRIANGLES`, `QUADS`, `LINES`, `POINTS`).

Le format suit l'état : la normale n'est portée que si l'éclairage est allumé **ou** si un texgen
en a besoin (`SPHERE_MAP`, `NORMAL_MAP`, `REFLECTION_MAP` — l'oublier donnait une case entière
fausse, écart 166/255) ; la couleur seulement si elle atteint la sortie ; la coordonnée de
brouillard seulement si c'est bien elle la source ; les coordonnées de texture des seules unités
texturées. Un format fixe et large (`POMPPC_GL_GEOM=2`) a été mesuré : il est plus lent sur une
scène géométrique (730 contre 881 img/s sur `gltest spin` 16×16) parce qu'il fait écrire à
GLEngine 27 mots par sommet au lieu de 11.

**Combien de sommets offrir.** `*n` est le nombre d'emplacements que le plugin met à disposition.
Avec les 192 du pilote Rage 128, GLEngine coupait les primitives ; en offrant tout ce que la zone
des sommets permet (jusqu'à 8192), une bande de 1000 sommets arrive en **un seul** `DRAW_RAW`.
Quand il doit couper, **GLEngine répète les sommets qu'il faut** : mesuré sur la scène `bigstrip`
(bande de 1000, éventail de 302, polygone de 250, boucle de 400), un plafond de 8192, 512, 192, 64
puis 16 sommets donne 1952, 1954, 1969, 2005 puis 2197 sommets transmis en 4, 5, 13, 33 puis 139
`DRAW_RAW` — et **exactement la même image** à chaque fois.

#### Fusion des dessins : un `DRAW_RAW` par lot d'état (18/09/2026)

Le chemin brut a déplacé le goulot d'étranglement : sur une scène lourde de
Marble Blast, **8 200 sommets partaient en ~1 065 `DRAW_RAW` par image, soit 7,7
sommets par dessin**. GLEngine remet la géométrie par `glBegin`/`glEnd`, et un
jeu de 2001 la décrit en rubans et éventails **courts**. Or chaque `DRAW_RAW`
coûte à l'hôte un `gl_target` complet (liaison du FBO, remise à plat de tout
l'état) plus `gl_draw_raw` et `gl_reset_raw` : **2 µs, mesurés** (voir plus bas).

Bout à bout, seules les primitives **indépendantes** se recollent (`TRIANGLES`,
`QUADS`, `LINES`, `POINTS`). La fusion convertit donc rubans, éventails, quads,
bandes de quads et polygones en **`GL_TRIANGLES` indexés** : les sommets restent
exactement où GLEngine les a écrits — toujours zéro recopie de sommet — et le
plugin n'écrit que des **indices u16**, 2 octets par sommet de triangle contre 44
pour un sommet recopié. Les indices vivent dans les 256 derniers kio de la zone
des sommets (`IDX_OFF`), donc dans la fenêtre partagée, et meurent avec elle.

Une série est prolongée si et seulement si le lot suivant a le **même contexte**,
le **même format de sommet**, et des sommets **contigus** dans la zone partagée.
Il n'y a rien d'autre à vérifier : tout changement d'état passe par `send_cmd` →
`reserve` → `close_raw`, donc il ferme la série de lui-même. Et **on ne fusionne
que des lots consécutifs** : jamais de réordonnancement, sans quoi le mélange et
l'égalité de profondeur changeraient l'image. Une série reste non indexée tant
qu'elle n'a qu'un seul mode recollable : une bande de 1 000 sommets part comme
avant, en un seul `DRAW_RAW` non indexé.

Pièges, tous vus en vrai :

- **Le sommet provoquant de l'ombrage plat n'est pas le même selon le mode**
  (`assemble` de `qgpu-soft.c`) : dernier sommet du triangle pour `TRIANGLES`,
  `TRIANGLE_STRIP` et `TRIANGLE_FAN` ; **4ᵉ sommet du quadrilatère** pour
  `QUADS`, pour ses **deux** triangles ; sommet `i+3` pour `QUAD_STRIP` ;
  **premier** sommet du polygone pour `POLYGON`. Comme `GL_TRIANGLES` prend le
  dernier, chaque triangle est ordonné pour que son dernier indice soit le
  provoquant. Une **rotation circulaire** suffit partout (même triangle, même
  orientation, donc même élimination de face) sauf pour `GL_QUADS` : sa diagonale
  de référence (0–2) laisse le premier triangle sans le sommet 3, donc en ombrage
  **plat** on coupe sur la diagonale 1–3 — exact, puisque la couleur du
  quadrilatère y est uniforme. En ombrage lisse on garde 0–2, celle du backend de
  référence.
- **L'alternance d'orientation d'un ruban** (un triangle sur deux retourné) doit
  être reproduite par les indices, sinon `GL_CULL_FACE` élimine un triangle sur
  deux.
- **`ioff` doit être un multiple de 4** (`in_shmem` du cœur), mais tous les
  indices d'une série sont lus d'affilée depuis ce seul offset : aligner *au
  milieu* d'une série glisse deux octets de bourrage dans le tableau et les
  triangles pointent sur les sommets du lot voisin (symptôme : un polygone sur
  trois faux). L'alignement se fait donc **à l'ouverture de la série, et nulle
  part ailleurs**.
- La zone des sommets du **chemin de rastérisation** (`prim`) devait elle aussi
  s'arrêter à `VTX_LIMIT` : les deux chemins cohabitent dans une même image.

**Résultat.** Marble Blast, scènes lourdes : **1 065 → 65-85 `DRAW_RAW` par
image** (7 sommets par dessin → 90-125), et le temps de soumission tombe de 3,5 à
1,6 ms par image. `POMPPC_GL_MERGE=0` coupe la fusion.

#### État envoyé, et seulement ce qui change

Matrices modèle-vue et projection (et de texture des unités actives), viewport, plage de
profondeur, les 8 lumières, les deux matériaux, l'ambiante du modèle, le texgen des unités
texturées, les 6 plans de découpe, les valeurs courantes des attributs absents du format, et les
clés d'état v7 (éclairage, normalisation, ombrage, faces, color material, deux faces, spéculaire
séparée, brouillard). Chaque commande est comparée à ce qui a déjà été posé sur le device : sur
Marble Blast, 30 à 55 commandes d'état par image pour 400 à 1000 `DRAW_RAW`.

Pièges vus en vrai :

- **Positions de lumière, directions de spot, plans œil et plans de découpe sont déjà en
  coordonnées ŒIL** dans l'état GLEngine, et le protocole les attend ainsi : on recopie, on ne
  retransforme rien.
- Le **seuil de spot** est rangé en **cosinus** (`< 0` = pas un spot) alors que `SET_LIGHT` veut
  l'**angle en degrés** (0..90, ou exactement 180) : conversion par `acos`.
- Comparer les 64 octets d'une matrice coûte moins que d'exploiter le masque « matrice modifiée »
  (`GS+0x4d48`), qui ne dit rien des matrices de texture d'une unité qui vient de s'allumer.
- Les clés v7 n'influencent **pas** les opcodes de dessin v1–v6 : `gl_apply_state` remet
  l'ombrage, l'éclairage, l'élimination des faces et le brouillard à plat avant chaque dessin
  ancien, et `qgpu-soft.c` ne lit `QGPU_SK_FOG_MODE` que dans `soft_draw_raw`. Les deux chemins
  cohabitent donc dans la même image sans se dérégler.

#### Domaine, et repli

Le domaine est **celui de la rastérisation** (`accel_ok`, `texture_ok`) **plus** ce que la v7 sait
faire : éclairage complet, color material, deux faces, normalize/rescale, élimination des faces,
ombrage plat, texgen des cinq modes, six plans de découpe, brouillard `LINEAR`/`EXP`/`EXP2` calculé
par l'hôte (y compris en `GL_NICEST`, que le chemin de rastérisation refusait), matrices de
texture. En sortent : mode de polygone non plein (le code 6 du descripteur plante), pointillés,
lissage, opérations logiques, atténuation de la taille des points, programmes ARB
(`gctx+0x4e1c ≠ 0x1c00`), plus de 4 unités, contexte sans surface accélérable.

**Un seul prédicat, appelé à deux endroits.** `geom_ok()` décide au dispatch (où l'on peut encore
refuser) *et* à `BeginPrimitiveBuffer` (où l'on ne peut plus). S'il est vrai au premier et faux au
second, c'est que l'état a bougé sans passer par `gldUpdateDispatch` : le cas est compté
(« brut:etat-tardif ») et le contexte quitte le domaine pour de bon. **Ce compteur doit rester à
zéro** — c'est la garantie d'exactitude, et il l'est sur toutes les scènes et sur Marble Blast.
Deux garde-fous complètent le prédicat à `BeginPrimitiveBuffer` : le pas réellement retenu par
GLEngine (`gctx+0x4880`) et le descripteur qu'il a relu (`gctx+0x48d0`) doivent être les nôtres —
sans quoi c'est son sommet **interne**, déjà transformé, qui arriverait.

Tout ce qui peut échouer (téléversement de texture, place dans le flux et dans la zone des
sommets) est fait **à `BeginPrimitiveBuffer`**, avant que GLEngine écrive quoi que ce soit.

#### Mesures (18/09/2026)

`gltest spin`, 400 triangles par image, 60 images :

| Taille | Apple | plugin, `GEOM=0` | plugin, `GEOM=1` | gain |
|---|---|---|---|---|
| 16×16 | 520 img/s | 406 | **900** | ×2,22 |
| 256×256 | 242 | 420 | **822** | ×1,96 |
| 640×480 | 122 | 323 | **481** | ×1,49 |

`gltest game` (couloir multitexturé + brouillard, 640×480) : 684 → **854 img/s** (×1,25) ;
le rendu d'Apple seul y fait 2,75 img/s.

**Marble Blast Gold**, en fenêtre 800×600, démo qui se joue toute seule, fenêtres de 5 s alignées
sur le lancement (même séquence des deux côtés : les quatre premières fenêtres, les menus, donnent
42,1 / 85,5 / 147,3 / 101,1 img/s contre 41,6 / 83,0 / 145,7 / 97,3) :

| Fenêtre de jeu | `GEOM=0` | `GEOM=1` | gain |
|---|---|---|---|
| la plus lourde | 24,3 img/s | **47,6** | ×1,96 |
| lourdes (5 fenêtres) | 25 à 30 | **43 à 49** | ×1,6 à ×1,9 |
| moyenne des 16 fenêtres de jeu | 42,4 | **62,6** | **+48 %** |

#### Mesures de la fusion (18/09/2026)

**Marble Blast Gold**, fenêtre 800×600, 2 cœurs, même démo jouée toute seule,
**même binaire** : `POMPPC_GL_MERGE=0` contre `=1`, fenêtres de 5 s appariées par
leur nombre de sommets (à ±2 % près : ce sont les mêmes images).

| Sommets/img | `DRAW_RAW`/img | sommets/dessin | submit ms/img | img/s |
|---|---|---|---|---|
| 8 100 | 969 → **85** | 8 → **94** | 3,39 → **1,66** | 40,1 → **43,9** |
| 7 900 | 1 031 → **80** | 7 → **101** | 3,46 → **1,62** | 37,5 → **39,9** |
| 7 800 | 1 030 → **65** | 7 → **122** | 3,47 → **1,61** | 38,1 → **43,0** |
| 7 450 | 971 → **80** | 7 → **93** | 3,36 → **1,63** | 38,4 → **47,2** |
| 5 050 | 602 → **60** | 8 → **82** | 2,63 → **1,58** | 37,4 → **42,4** |
| 3 800 | 480 → **30** | 7 → **125** | 2,25 → **1,35** | 64,1 → **74,9** |
| 3 100 | 442 → **36** | 6 → **87** | 2,15 → **1,34** | 63,2 → **71,2** |
| **moyenne des 16 fenêtres de jeu** | **−92 %** | | **−41 %** | **52,6 → 58,2 (+10,7 %)** |

`gltest` n'y gagne rien de mesurable, et c'est attendu : `spin` dessine ses 400
triangles en `GL_TRIANGLES`, donc en **un** `DRAW_RAW` déjà sans la fusion (845
contre 838 img/s en 16×16, 422 contre 413 en 640×480 — l'écart entre deux
passes de la même mesure atteint 25 %). `game`, qui a des bandes, passe de 719 à
785 img/s en moyenne de deux passes, mais dans le même bruit. **La fusion ne
sert que là où les primitives sont courtes et nombreuses**, c'est-à-dire dans les
jeux réels.

**Le coût hôte d'un `DRAW_RAW`, chiffré.** Le temps de soumission par image est
affine en nombre de dessins : les fenêtres de menu (11 à 15 dessins) coûtent
1,27 ms, et chaque dessin ajoute **2,0 µs** (3,39 − 1,27 sur 969 dessins ;
3,47 − 1,27 sur 1 030 ; 2,15 − 1,27 sur 442 — les trois donnent 1,9 à 2,0 µs).
C'est `gl_target` + `gl_draw_raw` + `gl_reset_raw`, soit ~150 appels GL. **Ne pas
refaire la liaison du FBO et la remise à plat quand deux dessins consécutifs
partagent contexte, surface et état** ferait donc gagner, *avant* la fusion,
2,1 ms sur une image de 26 ms (8 %) ; **après** la fusion il ne reste que 30 à
85 dessins par image, soit **0,06 à 0,17 ms — moins de 1 %**. Le gain hôte est
donc déjà pris par la fusion : il n'y a plus lieu de toucher `qgpu-gl.c` pour
cela.

**Ce que le profileur a appris.** Un `sample` de 10 s sur Marble Blast a d'abord montré `put_f`
en tête des feuilles de la pile principale (7,4 % du temps, devant `geom_begin` à 4,4 %) :
fabriquer les arguments des commandes d'état pour les comparer ensuite coûtait plus cher que tout
le reste du suivi d'état — parce que `BeginPrimitiveBuffer` est appelé **mille fois par image**,
pas une fois par lot de rastérisation. Les comparaisons se font maintenant sur les **octets
sources** du bloc GLEngine (un `memcmp` par matrice, par lumière, par unité de texgen), et les
arguments ne sont fabriqués que quand ils partent : les scènes lourdes sont passées de 34 à
47 img/s. Restent en tête `__memcpy` (13 %, ce que GLEngine écrit dans notre tampon) et
`mach_msg_trap` (11 %, l'attente du device et du WindowServer).

Après la fusion, un `sample` de 12 s sur les mêmes scènes montre que le coût par
appel du plugin est retombé de **17 % à 8 %** du temps (sur 1 140 échantillons :
`geom_begin` 18 contre 35, `bcmp`+`memcmp` 15 contre 33, `changed` 10 contre 17,
`texture_ok` sorti des trente premières feuilles). Deux économies par appel y
contribuent, et elles comptent parce que `BeginPrimitiveBuffer` est appelé mille
fois par image : `geom_send_texgen` lit les **quatre octets d'activation** d'une
unité avant de comparer ses 148 octets, et `geom_send_clip` lit le **masque** des
plans avant de comparer les cent octets du bloc — dans les deux cas, « rien
d'allumé et rien à défaire » est le cas courant. Restent en tête `__memcpy`
(15,8 %, les sommets que GLEngine écrit dans notre tampon) et `mach_msg_trap`
(13,9 %, l'attente du device et du WindowServer) : **c'est là qu'est le prochain
gain**, et il demande soit un doorbell asynchrone (tâche 2.2), soit des objets
tampon pour que les maillages statiques ne retraversent plus la fenêtre partagée
(tâche 2.1).

#### Écarts d'image avec le rendu d'Apple

Toutes les scènes (22) sont comparées image entière au rendu d'Apple. **Hors arêtes** (pixel dont
le voisinage 3×3 est uniforme dans l'image de référence), l'écart maximal est de **0 à 2/255**
partout, sauf :

- `game` : 3/255 (interpolation des couleurs en perspective) ;
- `lit` : 19/255 sur 104 composantes, au **bord du cône du spot**. Cause établie : OpenGL ne
  définit pas la découpe d'un quadrilatère en triangles, et quand la valeur aux sommets varie
  brutalement, deux rendus qui coupent la diagonale autrement donnent des images différentes. Avec
  la même scène en `GL_TRIANGLES` (`GLTEST_LIT_TRIS=1`), l'écart retombe à **3/255**.

Sur les arêtes, les écarts vont jusqu'à 255/255 sur quelques dizaines de pixels : silhouettes d'un
pixel de large (règles de remplissage et de tracé de ligne différentes entre le rasteriseur d'Apple
et celui de l'hôte). C'était déjà le cas du chemin de rastérisation.

### 4.8 La fin du pipeline fixe (protocole v8, lot 3)

Cinq fonctions du pipeline fixe manquaient encore. Elles sont branchées depuis le 18/09/2026 ; les
offsets de l'état de GLEngine ont été relevés par la sonde `v8probe` (`docs/re/etat-v8.md`).

| Fonction | Chemin | Preuve (`gltest`, 128×128, image entière vs rendu d'Apple) |
|---|---|---|
| Mélange à couleur constante, `GL_MIN` / `GL_MAX` | les **deux** | `blendc` : témoins exacts, **0/255** |
| Opérations logiques (les 16) | les **deux** | `logicop` : les six opérations vérifiées au bit près, **0/255** |
| Pointillé de polygone | les **deux** | `stipple` : **0/255** |
| Pointillé de ligne | brut | `stipple` : **0/255** |
| Modes de polygone `GL_POINT` / `GL_LINE` | **brut seulement** | `polymode` : **0/255** |
| Requêtes d'occlusion | brut et hérité | `occl` : 4 096 / 2 048 / 0 échantillons exacts |

Trois points méritent d'être retenus.

**Les clés v8 valent pour les deux chemins**, contrairement à celles de la v7 : elles agissent au
fragment (mélange, opération logique, pointillé de polygone) ou à l'assemblage des triangles, donc
après l'endroit où le chemin brut et le chemin hérité se rejoignent. `send_state` a désormais deux
plages de clés : les clés de géométrie ne partent qu'avec le chemin brut, les clés de fragment dès
que le device est un v8.

**Les modes de polygone sont réservés au chemin brut.** Sur le chemin hérité, GLEngine a déjà
décomposé le quadrilatère en triangles : le contour est perdu avant d'arriver à l'hôte, et en mode
`GL_LINE` les diagonales seraient tracées. Le plugin refuse donc, et le rendu d'Apple reprend la
main — image exacte. Même sur le chemin brut, il a fallu **couper la fusion** des dessins quand le
mode n'est pas `GL_FILL` : elle recolle les lots en `GL_TRIANGLES` indexés, ce qui détruit
exactement l'information que `glPolygonMode` consomme (vu en vrai : 3 % de l'image fausse).

**Le pointillé de polygone demandait un décalage d'une ligne.** Le protocole indexe le motif par
`(hauteur − ys) mod 32`, la spécification par la coordonnée fenêtre `hauteur − 1 − ys` : le plugin
envoie `motif[(j − 1) mod 32]` au mot `j`. Sans cela, exactement 384 composantes différaient du
rendu d'Apple, avec un écart de 255/255 (`docs/re/etat-v8.md` §1).

### 4.9 Ce que le renderer annonce (tâche 4.1)

`GL_VERSION` sort tel quel de `gldGetString`, et `GL_EXTENSIONS` est fabriqué par GLEngine à partir
de 25 noms fixes et d'un tableau de 79 bits que le pilote pose dans le bloc de configuration. Le
plugin y ajoute **ce que la chaîne tient, et rien d'autre**, établi fonction par fonction dans
**`docs/re/version-extensions.md`** :

* `GL_VERSION = "1.1 POMPPC-1.0"`. Ce n'est pas 1.5, ni même 1.2 : **OpenGL 1.2 exige les textures
  3D**, que GLEngine refuse (`GL_MAX_3D_TEXTURE_SIZE = 0`, `glTexImage3D` → `GL_INVALID_VALUE`) et
  que le protocole qgpu ne porte pas davantage. Manquent aussi, mesurés : cartes de cube,
  compression de texture, multiéchantillonnage (1.3) ; couleur secondaire, `GL_MIRRORED_REPEAT`,
  textures de profondeur, paramètres de point (1.4).
* Trois extensions ajoutées : `GL_ARB_occlusion_query` (tenue par nous, et seulement sur un device
  v8), `GL_ARB_vertex_buffer_object` et `GL_EXT_blend_func_separate` (tenues, et vérifiées au
  rendu). 42 extensions au lieu de 39.
* **Les limites d'Apple sont laissées telles quelles** — 8 unités de texture, 4096 de côté. Le
  chemin accéléré n'en tient que 4 et 2048, mais au-delà le repli sur le rendu d'Apple est exact :
  c'est vérifié, pas supposé. Abaisser ces limites aurait retiré une capacité que la chaîne tient.

Le relevé de la table bit → extension de `docs/re/capacites-glengine.md` §3.2 s'est révélé
**décalé d'un cran à partir du bit 24** ; il est corrigé, par l'expérience, dans
`docs/re/version-extensions.md` §4. La première version du code d'annonce, écrite d'après
l'ancienne table, annonçait deux extensions non tenues : c'est la scène `caps`, qui imprime la
liste telle qu'une application la lit, qui l'a montré.

---

## 5. Vérification

| Niveau | Outil | Résultat (17/09/2026) |
|---|---|---|
| cœur + backends, natif hôte | `tests/qgpu_core_test.c` | 53 vérifications × 2 backends, pixels identiques |
| device, sans invité | `tests/qgpu_smoke.py` | 11/11, backend GL |
| harnais complet | `tests/run-all.sh --slow` | 34 OK |
| transport dans Tiger | `guest/qgpu-test` | OK, y compris pendant 4 applications GL |
| plugin hors écran | `gltest` × 28 scènes | pixels témoins OK avec `POMPPC_GL_GEOM=0` **et** `=1` ; image entière comparée au rendu d'Apple, écart max **hors arêtes** de 0 à 3/255 (voir §4.7) |
| pipeline fixe v8 | `gltest blendc logicop polymode stipple occl` | mélange constant, min/max, opérations logiques, modes de polygone, pointillés, requêtes d'occlusion — **0/255 sur l'image entière**, comptes d'occlusion exacts (§4.8) |
| textures v10 (hôte seul, 19/09/2026) | `tests/qgpu_core_test.c`, `run_v10` | 3D, cube (orientation vérifiée contre le pilote), rectangle, 1D, miroir, bordure, profondeur et comparaison, LOD, mipmaps générés, 18 formats, S3TC, sous-images, 18 refus — **0 échec**, backend logiciel et RTX 4060 Ti (EGL, NVIDIA). Côté invité : rien encore (`docs/protocole-v10-textures.md` §7) |
| hôte Linux + NVIDIA (19/09/2026) | `qgpu_core_test`, `qgpu_smoke.py`, `run-all.sh` | verts après deux corrections (contexte EGL partagé entre threads, profondeur selon la présence d'un stencil) ; `qgpu_smoke.py` publie désormais `caps = 0x1e` |
| version et extensions | `gltest caps entry v15` | ce qui est annoncé est tenu, fonction par fonction (`docs/re/version-extensions.md`) |
| géométrie sur l'hôte | `gltest lit texgen clip fogz bigstrip dlist mixte` | éclairage, texgen, découpe, brouillard, longues primitives, listes d'affichage, alternance domaine / hors domaine |
| application réelle | Zenerchi (§4.6) | menus et partie corrects, 42 img/s en partie, présentation directe |
| plugin en fenêtre, dans le bureau | `glwin` (GLUT) | OK ; image témoin visible dans la fenêtre à l'écran |
| multi-processus | 5 × `gltest game` + `qgpu_test` | 4 accélérés, le 5e en logiciel, tous corrects |
| doorbell asynchrone (v9) | `guest/qgpu-test`, section v9 | soumission asynchrone, barrière, relecture visible **seulement après** elle, rafale de 64 dont 3 refusées file pleine, erreur comptée dans `ERRORS`, puis un `SUBMIT` **synchrone** qui marche toujours — **les deux formes d'appel cohabitent** |
| plugin asynchrone (v9) | `gltest` × 29 scènes × {`ASYNC=0`, `ASYNC=1`}, + sans fusion | tout « OK (0 échec) », et **0/255 sur l'image entière** entre synchrone, asynchrone et asynchrone-sans-fusion ; `glwin` OK |
| stress asynchrone | 4 × `gltest game` simultanés + `qgpu_test` | tous « OK (0 échec) », images des clients 1 et 4 identiques au pixel près |

### 5.1 La boucle de développement dans l'invité

`tools/guest/devloop.py` garde une VM Tiger allumée et lui envoie des « jobs » (un dossier avec
`job.sh`) par deux fichiers préalloués du disque, lus et écrits en brut par l'hôte et l'invité :
une itération prend quelques secondes, sans redémarrage ni conversion d'image.

```sh
export DEVDISK=disks/tiger-dev.raw
python3 tools/guest/devloop.py prepare        # VM arrêtée : agent, boîtes aux lettres, relais
python3 tools/guest/devloop.py start --gui    # bureau ; sans --gui : single-user
python3 tools/guest/devloop.py run mon_job/   # exécute mon_job/job.sh, rapatrie out/
python3 tools/guest/devloop.py click 400 150 800x600   # clic souris (résolution courante)
```

`SMP=2` et `SND=1` (ou `SND=none`, Screamer muet) au démarrage reproduisent les conditions de
`run_tiger.sh` : deux cœurs MTTCG, son, RAM plafonnée à 768 Mo.

En mode bureau, un processus racine ne peut pas ouvrir de fenêtre (ni même de contexte CGL :
erreur 10006) : `gui_run 'commande'` (`tools/guest/guilib.sh`) fait exécuter la commande dans la
session de l'utilisateur par l'élément d'ouverture `POMPPCGuiRunner`.

Pièges rencontrés, tous corrigés et commentés dans le code :

| Symptôme | Cause |
|---|---|
| `dd: /dev/rdisk0: Resource busy` | Tiger interdit l'écriture brute sur un disque monté : la boîte de sortie s'écrit à travers son fichier |
| boîte aux lettres introuvable ou périmée | les blocs d'une préparation précédente restent dans l'image : chaque préparation porte un nonce |
| `cannot execute binary file` pour un script | le bash 2.05 de Tiger juge « binaire » une première ligne non ASCII |
| StartupItem ignoré (« not owned by UID 0 ») | fichiers posés depuis l'hôte : l'agent single-user corrige les propriétaires |
| `ld: unknown flag: -nostdlib`, `can't locate file for: -lcc_kext`, « doesn't contain kernel extension code », « relocation overflow » | chaîne kext PPC : voir `kext/POMPPCGPU/README.md` |
| `non-relocatable subtraction expression` | l'as d'Apple refuse une référence PIC vers un symbole non défini dans le fichier : les trampolines reçoivent leur cible du crochet C |
| un kext qui panique fige la VM, et `-prom-env boot-args=-x` n'est pas disponible par `devloop` | on **compile puis `kextload`** le kext depuis son dossier de sources, `/System/Library/Extensions` gardant l'ancien : un panic au chargement se répare par `devloop stop/start`. L'installation ne vient qu'après un `qgpu_test` vert |

---

## 6. Installation

Dans l'invité (Xcode Tools installés), depuis le CD de `scripts/make_kext_iso.sh` :

```sh
cp -R /Volumes/POMPPCSRC /tmp/src
sudo sh /tmp/src/guest/gldriver/install.sh        # --remove pour désinstaller
```

Puis lancer Tiger avec le device : `GPU=1 ./run_tiger.sh`. Sans le device, le kext ne se charge
pas et le plugin se comporte exactement comme le rendu logiciel d'Apple.

---

## 7. Pistes

Le plan à court terme est dans **`docs/todo-gpu-3d.md`**, la feuille de route (OpenGL 1.5,
Quartz Extreme, Core Image) dans **`docs/roadmap-opengl15.md`**. En résumé :

1. **Monter d'un étage** : se brancher au niveau du tableau de sommets et du programme de
   pipeline pour envoyer les sommets non transformés — transformation et éclairage sur le GPU
   hôte. Plus gros gain restant, plus gros travail.
2. **Stencil**, puis modes de polygone, pointillés et lissage : ce qui manque à OpenGL 1.3.
3. **Zero-copy** : laisser le device écrire dans la VRAM plutôt que recopier l'image relue.
4. **Exécution asynchrone** du doorbell (FENCE et IRQ DONE déjà en place).
5. **Accélérateur IOKit** (`IOGLBundleName`) à la place de l'astuce du nom de bundle.
6. **Autres backends hôte** : Vulkan (natif ou Zink), Metal (ANGLE).
