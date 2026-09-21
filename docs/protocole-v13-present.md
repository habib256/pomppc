# Protocole qgpu v13 — présentation dans la VRAM

Le device écrit lui-même l'image dans la VRAM de l'écran. L'invité n'y
recopie plus un pixel : plus de `SURF_READBACK` à l'échange, plus de `memcpy`,
plus de conversion 1555 sur le G4.

## L'ajout

`QGPU_OP_SURF_PRESENT` (`0x0019`), longueur 9 :

```
[surf, off, stride, x, y, w, h, format]
```

Même rectangle que `SURF_READBACK`, mais la destination n'est **pas** la
fenêtre partagée. `off` est un octet dans la VRAM QFB (BAR0). `format` :

| valeur | pixels | usage |
|---|---|---|
| `QGPU_PF_XRGB8888` (0) | 32 bpp, mêmes octets big-endian que `SURF_READBACK` | millions de couleurs |
| `QGPU_PF_RGB1555` (1) | 16 bpp big-endian, « milliers » QFB | conversion faite par l'hôte |

`QGPU_CAP_SCANOUT` (bit 5) dit que la cible existe. Sans le bit, l'opcode rend
`QGPU_ST_BAD_ARG` : l'invité se replie sur l'ancien chemin. Un rectangle qui
déborde de la VRAM rend `QGPU_ST_OOB`.

`QGPU_OP_COPY_TEX` (`0x001A`), longueur 11 :

```
[tex, cible, niveau, x, y, z, sx, sy, w, h]
```

Copie le rectangle de la surface **liée** vers un niveau de texture déjà
défini. Aucun texel ne traverse la fenêtre partagée. Un flux v12 ignore
l'opcode. `POMPPC_GL_PIXEL=0` reprend Apple.

Un flux v12 ignore `SURF_PRESENT` (`BAD_OPCODE`). Un kext / plugin compilé contre la
v13 s'attache encore à un device v12 (`QGPU_PROTO_MIN`).

## Côté QEMU

`qgpu_bind_scanout` cherche une cible une fois la machine née, pose le pointeur
de VRAM sur le cœur, et marque la plage sale après chaque présentation (le
thread de rendu n'a pas le BQL : `memory_region_set_dirty` suffit).

Deux cibles, dans cet ordre : `qfb-pci` s'il est là, **sinon le framebuffer
VGA** de la machine — sur mac99 c'est l'écran réellement utilisé, que Tiger
pilote par `qemu_vga.ndrv`. Sa VRAM s'atteint sans toucher au code amont : une
MemoryRegion nommée est un enfant QOM de son propriétaire, donc « vga.vram »
est l'enfant « vga.vram[0] » du device « VGA ». Le framebuffer y commence à
l'offset 0, ce que l'image confirme (rien n'est décalé).

Ce repli manquait jusqu'au 21/09/2026, et son absence était **silencieuse** :
la quotidienne tourne sans `qfb-pci`, donc `QGPU_CAP_SCANOUT` n'était jamais
posé, l'opcode répondait `BAD_ARG` et l'invité relisait chaque image sans que
rien ne le signale — pendant que `run_v13` restait vert. D'où, désormais, un
avertissement quand aucune cible n'est trouvée, et un sondage de la capacité
dans `scripts/caps.sh` (`qemu_qgpu_has_scanout`), vérifié à chaque build.

## Ce que le plugin en fait

`present_direct`, device v13 + le bit : un `SURF_PRESENT` à l'offset
`(D.y * rowbytes + D.x * bpp)` dans la VRAM, plus aucune copie dans
`run_posts`. `POMPPC_GL_PRESENT=0` reprend l'ancien chemin.

`DrawPixels` / `CopyPixels` COLOR / `Bitmap` : le 2e argument de la
procédure de rastérisation est un **sommet fenêtre** (`V_X`/`V_Y`), pas
la position objet. `_glDrawPixels_Exec` / `_glBitmap_Exec` l'écrivent
depuis `gctx+0x2e0` avant l'appel. `xorig`/`yorig` arrivent en `f1`/`f2`
(sauvés par le trampoline en `a[12]`). `xmove`/`ymove` sont ajoutés à
la position raster **après** le retour, dans GLEngine. Le plugin
téléverse (ou `COPY_TEX`) dans une texture jetable et dessine un quad
`GL_REPLACE` ; pour `Bitmap`, les bits à 0 sont rejetés par un test
alpha `GREATER 0`. `CopyPixels` / `DrawPixels` DEPTH et STENCIL
utilisent `DEPTH_READBACK`+`UPLOAD` (resp. stencil) : pas de nouvel
opcode. Le test de l'application doit être coupé ou `ALWAYS`, masque
d'écriture plein ; sinon Apple.

## Preuve

`run_v13` de `tests/qgpu_core_test.c`, les deux backends : sans scanout →
`BAD_ARG` ; xRGB identique au `CLEAR` ; 1555 d'un rouge plein = `0x7c00` ;
hors VRAM → `OOB` ; `reset` conserve la cible. `COPY_TEX` : rectangle
rouge → texture noire, pixel copié rouge, reste noir, `w=0` sans effet,
débordement et tex inconnue → `BAD_ARG`.
