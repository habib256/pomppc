# Protocole qgpu v20 : surface hôte comme texture, relecture de texture (27/09/2026)

Pour Colin McRae Rally 2005 (`docs/re/cmr-rendu-vers-texture.md`) : IndirectX rend dans des
fenêtres cachées puis les échantillonne par `aglSurfaceTexture`, souvent en texture
**rectangle**. Jusqu'à la v19, ces lots et ces échanges se repliaient chez Apple, qui rend les
textures de surface en blanc : la VM affichait l'image d'Apple. Pas de repli : le protocole
apprend les deux.

`QGPU_PROTO_VERSION` 19 → **20**. `qgpu_abi.h` ne change pas : **pas de kext à reconstruire**.
QEMU et le plugin se reconstruisent ensemble ; un plugin v19 (`20260926-var`) marche sur un QEMU
v20 (il n'émet rien de nouveau), un plugin v20 sur un QEMU v19 n'émet pas les nouveaux opcodes
(version et capacités vérifiées).

## Messages

| opcode | valeur | mots | capacité | sens |
|---|---|---|---|---|
| `QGPU_OP_SURF_TEX` | `0x001E` | `[tex, cible d'image, niveau, surf]` (`QGPU_LEN_SURF_TEX` 5) | `QGPU_CAP_SURF_TEX` `0x400` (backend qui sait relire) | le niveau devient une copie de la couleur de la surface (w×h, RGBA), **ligne 0 = haut** ; cible 2D ou RECT, celle de la texture ; niveau 0 pour un rectangle ; mipmaps régénérés si la texture le demande |
| `QGPU_OP_TEX_READBACK` | `0x001F` | `[tex, cible d'image, niveau, off, max]` (`QGPU_LEN_TEX_READBACK` 6) | `QGPU_CAP_TEX_READBACK` `0x800` (toujours) | écrit à `off` : w, h, d, format de base, puis w·h·d texels ARGB big-endian ; outil du vidage autonome, jamais au rendu |

Refus détaillés dans `qgpu_proto.h` (section « v20 »). Les textures rectangle n'ont pas d'opcode
propre : `QGPU_TT_RECTANGLE` existait (`TEX_CREATE3`, `TEX_IMAGE3`, `COPY_TEX`), c'est le plugin
qui l'annonce maintenant (bit 25 de GLEngine).

## Cœur

- `fp_samples[u]` (`QGPU_FPS_1D/2D/3D/CUBE/RECT`) relevé au `PROG_STRING` d'un programme de
  fragments (`texture[u], CIBLE`, blancs admis) ; au dessin, une unité lue par le programme
  mais sans texture reçoit la texture 0 sur ces cibles (backend gl) — sinon l'hôte lisait la
  dernière texture qui y était liée.
- `SURF_TEX` et `COPY_TEX` se font sur le GPU de l'hôte depuis le 27/09 (section suivante) ;
  `QGPU_GPU_COPY=0` rend l'ancien chemin, relecture CPU (`be->readback`).

## Épreuves

`tests/qgpu_core_test.c` `run_v20()` (orientation 16×8, dessin échantillonné, sémantique de copie,
rectangle, refus ; `TEX_READBACK` en-tête, texels et refus ; `fp_samples`) : 938 contrôles, backends
soft et gl. `tests/run-all.sh` : 140 OK, 0 échec. Scènes `gltest rect`, `rectfp`, programme
`guest/gltest/rtt` (`2d`, `rect`). Colin McRae : capture de la VM = rejeu (0,000), 0 repli.

## Plugin (`20260927-rtt`)

`POMPPC_GL_RECT` (bit 25 et cible RECT), `POMPPC_GL_SURFTEX` (textures de surface), `POMPPC_GL_HIDDEN`
(drawables cachés : pas de présentation ni de relecture) — tous à 1 par défaut, `=0` pour
l'ancien comportement. Au vidage déclenché : `TEX_READBACK` des textures n'existant que dans
l'hôte, `SURF_READBACK` + `SURF_UPLOAD` de chaque surface vivante, `surfaces.txt`.

## Copie GPU (27/09, hôte seul)

Rien ne change sur le fil : ni `qgpu_proto.h`, ni le plugin, ni le kext. Jusqu'ici `SURF_TEX` et
`COPY_TEX` relisaient la surface (`glReadPixels`, qui attend la fin de son rendu), rangeaient
les texels dans le niveau du cœur, puis le niveau entier repartait vers le GPU
(`glTexImage2D`) au dessin suivant. Ils se font maintenant d'objet GL à objet GL
(`patches/qgpu/qgpu-gl.c`, `gl_tex_copy`) :

- **`SURF_TEX`** (ligne 0 = haut) : le FBO d'une surface a sa ligne 0 de surface en ligne 0 GL, et
  `gl_tex_level` envoie la ligne 0 de `px` en ligne 0 GL ; `glCopyTexSubImage2D` depuis le FBO
  donne donc, sans retournement, exactement ce que la relecture aurait rangé.
  `glCopyTexImage2D` quand le niveau GL change de taille ou de format (`GlTexture.lv`).
- **`COPY_TEX`** (orientation OpenGL, ligne `sy + h − 1` en `y`) : `glBlitFramebuffer` retourné du
  FBO de la surface vers un FBO intermédiaire RGBA8 (même taille, `NEAREST` : texels
  identiques), puis `glCopyTexSubImage1D/2D/3D` vers le niveau. Toutes les cibles (1D, 2D,
  rectangle, faces de cube, tranches 3D) et tous les formats : la copie réduit RGBA au format
  du niveau comme l'envoi le faisait (L = R, A = A…). Sans blit (hôte sans
  `EXT_framebuffer_blit`) : une ligne par `glCopyTexSubImage2D`.
- **Cœur** (`qgpu-core.c`) : un niveau copié par le GPU porte `QgpuTexLevel.gpu` — la texture
  GL est la vérité, `px` garde taille et format mais son contenu est périmé ; `gl_tex_sync` ne
  l'écrase pas. Le cœur le rapatrie (`be->tex_fetch`, `glGetTexImage`) avant d'y écrire en
  partie (`TEX_SUBIMAGE`, `COPY_TEX` repli) ou de le lire (`TEX_READBACK`, mipmaps) ;
  `qgpu_core_tex_px()` fait de même pour les épreuves. Les texels rapatriés sont ceux du format
  du niveau (un RGB revient avec alpha 255) : ce que la texture échantillonne, donc un nouvel
  envoi du niveau redonne la même texture. Une copie qui doit régénérer des mipmaps
  automatiques (`GENERATE_MIPMAP` sur le niveau de base) reste sur la relecture : le cœur
  calcule les mipmaps depuis `px`, et sa moyenne est la référence. Un échec de `tex_copy` retombe
  sur la relecture.
- **A/B** : `QGPU_GPU_COPY=0` dans l'environnement de QEMU (ou du rejeu) garde l'ancien chemin ;
  le journal de QEMU dit lequel tourne (`qgpu: copies surface → texture : GPU de l'hôte`).
  `c->cstats` compte copies, copies GPU, rapatriements et leur temps ; `qgpu_replay` les
  affiche, avec le temps passé dans le cœur.

### Preuves

- `tests/qgpu_core_test.c` `run_gpu_copy()` (backend gl) : le même flux joué en relecture puis
  en copie GPU — rectangle décalé, luminance, face de cube RGB (et face voisine intacte),
  rectangle, `SURF_TEX` redimensionnée 24×12 → 64×64 → 24×12, rectangle de surface, tranche
  3D, 1D, mipmaps automatiques (restent sur la relecture), sous-image par-dessus une copie GPU
  (rapatriement) puis seconde copie dans le même niveau : texels identiques dans les
  composantes gardées par le format, dessin identique à l'octet, 11 copies par le GPU, 1
  rapatriement. Les épreuves v17 (`COPY_TEX`) et v20 (`SURF_TEX`, `TEX_READBACK`) passent par
  les deux chemins (`QGPU_GPU_COPY=0` ou non) ; `tests/run-all.sh` : 140 OK, 0 échec.
- **Rejeu natif** des 21 vidages de la matrice (`bench/matrice/ref/*` et tour `20260927-0307`) :
  binaire d'avant (63d15f1), nouveau en relecture, nouveau en copie GPU. **Toutes les images
  identiques à l'octet**, sauf la première image de `ref/cmr-pe` (f62507), qui n'est pas
  reproductible avec le binaire d'avant lui-même (3 résultats différents sur 12 rejeux : la
  texture 53 y est liée sans image dans le vidage, Apple l'échantillonne « unloadable ») ; ses
  trois autres images, dont l'image de référence, sont identiques sur 36 rejeux. Aucun
  rapatriement dans aucun jeu.

### Temps hôte (rejeu, médiane de 7, M4)

Temps des copies elles-mêmes et temps total passé dans le cœur (soumissions et relectures de
présentation ; il compte aussi le renvoi du niveau entier au dessin suivant, que la copie GPU
supprime) :

| vidage | copies | copies : relecture → GPU | cœur : relecture → GPU | par image |
|---|---|---|---|---|
| `ref/cmr-pe` (4 images) | 7 `SURF_TEX` 800×600 | 3,6 → 1,7 ms | 58,0 → 55,0 ms | −0,8 ms |
| `0307/cmr-pe` (10) | 20 `SURF_TEX` | 10,4 → 2,7 ms | 102,7 → 91,2 ms | −1,2 ms |
| `ref/d3-pe` (14) | 28 `COPY_TEX` 1024×768 | 44,7 → 9,9 ms | 227,1 → 211,3 ms | −1,1 ms |
| `0307/d3-pe` (20) | 40 `COPY_TEX` | 59,0 → 6,5 ms | 300,0 → 276,6 ms | −1,2 ms |
| `0307/d3-fen` (20) | 60 `COPY_TEX` | 48,8 → 7,1 ms | 268,5 → 254,7 ms | −0,7 ms |
| `ref/prey-fen` (13) | 80 `COPY_TEX` | 55,8 → 12,8 ms | 155,6 → 124,6 ms | −2,4 ms |
| `0307/prey-pe` (20) | 63 `COPY_TEX` | 35,1 → 6,9 ms | 110,6 → 81,5 ms | −1,5 ms |
| `0307/prey-fen` (20) | 60 `COPY_TEX` | 26,6 → 6,5 ms | 78,7 → 60,0 ms | −0,9 ms |

En régime (après la première copie, qui crée le FBO intermédiaire et le niveau GL) : un
`SURF_TEX` 800×600 passe de 0,25-1,0 ms à 0,06-0,13 ms, un `COPY_TEX` 1024×768 de 2,4-3,9 ms à
~0,2 ms, sans compter le renvoi du niveau (1,9 à 3 Mo) évité au dessin suivant. Le gain est pris
sur le fil de rendu de l'hôte (`qgpu-render`), qui tourne en parallèle de l'invité : en VM il ne
se voit sur le ms/image que là où l'invité attend le rendu (relectures, présentations).

Mesure en VM : `tools/matrice/ab-copie-gpu.sh` (matrice `-j cmr,d3,prey` jouée en relecture puis
en copie GPU sur le même binaire, QEMU relancé à chaque passe, VM rendue au QEMU de référence à
la fin).
