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
- `SURF_TEX` passe par `be->readback` (copie CPU) ; une copie GPU est un gain possible plus tard.

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
