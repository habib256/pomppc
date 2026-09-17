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
| `patches/qgpu/qgpu_proto.h` | **le contrat** v4 : registres, opcodes, clés d'état, tranches des clients |
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

## 3. Le protocole qgpu (v4)

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
comme le `gldSwapNoop` d'Apple. Les conditions sont réévaluées toutes les 30 images ; une fenêtre
ordinaire garde le chemin normal. `POMPPC_GL_DIRECT=0` coupe ce mode.

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

Accéléré : effacement couleur/profondeur ; triangles, bandes, éventails, quads, bandes de quads,
polygones ; lignes, bandes et boucles de lignes ; points ; profondeur (toutes fonctions), masques,
mélange (facteurs et équations d'OpenGL 1.x), test alpha, ciseaux, ombrage lisse et plat,
brouillard (facteur par sommet), décalage de polygone plein ; **textures 2D et 1D sur deux unités**
(formats de base ALPHA, RGB, RGBA, LUMINANCE, LUMINANCE_ALPHA, INTENSITY ; données RGBA, RGB, BGRA,
BGR, LUMINANCE, LUMINANCE_ALPHA, ALPHA, RED en octets ; RGBA et BGRA en `UNSIGNED_INT_8_8_8_8`
et `_REV`, BGRA en `UNSIGNED_SHORT_1_5_5_5_REV`, RGB en `UNSIGNED_SHORT_5_6_5`, RGBA en
`UNSIGNED_SHORT_4_4_4_4` ; filtres
avec mipmaps, REPEAT/CLAMP/CLAMP_TO_EDGE ; environnements MODULATE, REPLACE, DECAL, BLEND, ADD).

Rendu par le code d'Apple (exact, plus lent) : trois unités de texture ou plus, GL_COMBINE,
textures 3D, cube et rectangle, stencil, opérations logiques, stipple, lissage, mode polygone non
plein, brouillard en `GL_NICEST`, lignes et points texturés ou décalés, opérations de pixels
(`glDrawPixels`, `glBitmap`, `glCopyPixels`, accumulation), tampons autres que 32 bits.

---

## 5. Vérification

| Niveau | Outil | Résultat (17/09/2026) |
|---|---|---|
| cœur + backends, natif hôte | `tests/qgpu_core_test.c` | 53 vérifications × 2 backends, pixels identiques |
| device, sans invité | `tests/qgpu_smoke.py` | 11/11, backend GL |
| harnais complet | `tests/run-all.sh --slow` | 34 OK |
| transport dans Tiger | `guest/qgpu-test` | OK, y compris pendant 4 applications GL |
| plugin hors écran | `gltest` × 13 scènes | pixels témoins OK ; images comparées au rendu d'Apple (§0) ; `texpack` : formats compacts identiques à Apple |
| application réelle | Zenerchi (§4.6) | menus et partie corrects, 42 img/s en partie, présentation directe |
| plugin en fenêtre, dans le bureau | `glwin` (GLUT) | OK ; image témoin visible dans la fenêtre à l'écran |
| multi-processus | 5 × `gltest game` + `qgpu_test` | 4 accélérés, le 5e en logiciel, tous corrects |

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
