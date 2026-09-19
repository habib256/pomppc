# Cartes de cube : ce que GLEngine donne au pilote (Tiger 10.4.6)

Relevé du 19/09/2026, **par l'expérience dans l'invité** (VM de dev Linux, device qgpu v11,
RTX 4060 Ti) : scène `cubeprobe` de `guest/gltest` (six faces de couleurs distinctes, trois
niveaux), sonde de cible du plugin (`POMPPC_GL_T3DDUMP=1` avec `POMPPC_GLTRACE`), et
`POMPPC_GL_TRYCUBE=256`, qui pose `cfg+0xc2` le temps de la sonde. Job :
`tools/guest/jobs/cubeprobe`. Même méthode que `docs/re/textures-3d.md`, dont ce document est la
suite.

## 1. L'annonce : `cfg+0xc2`

`GL_MAX_CUBE_MAP_TEXTURE_SIZE` est le **u16 en `cfg+0xc2`** de la configuration du renderer.
Apple le laisse à 0 : `glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X, …)` rend alors
`GL_INVALID_VALUE` (0x501, vu sous le rendu d'Apple seul). Posé, GLEngine accepte les six cibles
de face et transmet les niveaux. Le plugin y met `QGPU_MAX_TEX_DIM` (2048) quand le device tient
les cubes (`G.cube` : v10 et `QGPU_CAP_GL14`) ; `cfg+0xbe` (3D) est son voisin, `cfg+0xc0` reste
non identifié et n'est pas touché.

## 2. Où est la carte de cube

* `TU_ENABLE`, bit **0x01** ; la texture liée occupe l'**emplacement 0** de la table des textures
  du contexte du GLDriver (`ctx+0x10`, `unité·0x14 + 0`) — c'était déjà la place prévue par le
  tableau de `docs/re/textures-3d.md` §1.
* Dans l'objet texture de GLEngine (`DT_PARAMS`), **`+0x01` = 0** (cible « cube »).
* `gldCreateTextureLevel(ctx, texture, drapeaux, **face**, **niveau**, 7, 0, 1)` : le 4ᵉ
  argument est la face (0 = +X … 5 = −Z, dans l'ordre des cibles `0x8515 + f`), le 5ᵉ le niveau.
  Relevé sur 6 × 3 appels, dans l'ordre face par face.

## 3. Les niveaux de chaque face

**Chaque face a son propre tableau de niveaux dans l'objet de GLEngine, au pas de 15 entrées** :

```
entrée (face f, niveau l) = DT_PARAMS + 0xa4 + f·0x168 + l·0x18
```

(0x168 = 15 × 0x18.) L'entrée a la forme de `docs/re/textures-3d.md` §2 : largeur, hauteur,
profondeur 1, pas en pixels, format et type de l'application, pointeur de données.

La structure de niveau **du GLDriver** (`dt+0x78 + l·0x74`, `LV_*`) ne décrit que la **face 0** :
le rendu d'Apple, qui ne connaît pas les cubes, ne va pas plus loin. Le plugin lit donc les six
faces dans l'objet de GLEngine (`cube_level`).

## 4. Ce que le rendu d'Apple fait

Rien : sans `cfg+0xc2`, les faces sont refusées ; avec, rien n'est échantillonné (la sonde sort
blanc là où la face +X est verte). Il n'y a **pas de repli** : un cube hors du domaine accéléré
sort faux, comme une texture 3D. C'est pourquoi `cfg+0xc2` n'est posé que si l'hôte les tient.

## 5. Le chemin brut et la coordonnée r

> **Élucidé le 19/09/2026** (`docs/re/opengl-1.4.md` §3) : `cfg+0x7a` à 1 garde GLEngine sur
> nous, et les cubes passent désormais par le chemin brut (`cube` 12/12, aucun refus). Le texte
> ci-dessous est le constat d'origine.

Même constat que pour la 3D (`docs/re/textures-3d.md` §4) : avec une carte de cube active et des
coordonnées **explicites** `glTexCoord3f`, GLEngine n'appelle pas `BeginPrimitiveBuffer` et la
géométrie disparaît du chemin brut ; la génération `GL_NORMAL_MAP` passe, elle. Le plugin sort
donc tout cube du chemin brut (motif `brut:texture-3d-ou-cube`) : par le chemin hérité, GLEngine
transforme et génère s, t, r lui-même, et tout est exact.

## 6. Preuve

Scène `cube` (OpenGL 1.3, protocole v10) : les six faces par une direction chacune, l'orientation
des texels de la face +X (table 3.21 d'OpenGL), `GL_NORMAL_MAP` et `GL_REFLECTION_MAP` — **12 cas
sur 12** par le chemin brut **et** par le chemin hérité (`POMPPC_GL_GEOM=0`) ; 12 échecs sous le
rendu d'Apple seul. Le cas « 1.3 cube map » de `v15` est **TENU**. Le cube compressé (faces
DXT1) passe aussi : scène `tex13`, `docs/re/bordure-et-compression.md`.

## 7. Ce qui en découle dans le plugin

`TP_FACE_SIZE`, `cube_level`, `tex_is_cube`, la branche cube de `tex_complete` (six faces carrées,
de même taille et de même format à chaque niveau requis) et de `upload_texture`
(`TEX_CREATE3 QGPU_TT_CUBE_MAP`, puis `TEX_IMAGE3` par face avec `QGPU_TT_CUBE_FACE(f)`) dans
`guest/gldriver/pomppc_accel.c` ; `GL_ARB_texture_cube_map` (bit 6) annoncée avec `G.cube`.
`POMPPC_GL_CUBE=0` coupe tout.
