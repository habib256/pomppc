# Protocole qgpu v15 — transfert 16 bits

Les drawables « milliers de couleurs » (RGB1555) et la profondeur 16 bits
ne sont plus convertis par le G4. L'invité recopie les octets déjà 16 bits
dans BAR0 ; l'hôte élargit vers xRGB8888 / float32, et l'inverse à la
relecture.

## L'ajout

`SURF_READBACK` / `SURF_UPLOAD` et `DEPTH_READBACK` / `DEPTH_UPLOAD`
acceptent encore la longueur 8 (xRGB8888 / float32, inchangé).

Longueur 9 : le 8e argument est le format.

- couleur : `QGPU_PF_XRGB8888` (0) ou `QGPU_PF_RGB1555` (1, u16 big-endian)
- profondeur : `QGPU_DF_FLOAT32` (0) ou `QGPU_DF_UNORM16` (1, u16 BE 0..65535 ↔ [0,1])

Le stride est en octets : ≥ `w * 2` et aligné sur 2 pour le 16 bits,
≥ `w * 4` et aligné sur 4 pour le 32 bits. `STENCIL_*` reste en mots 32.

Un flux v14 refuse LEN 9 (`BAD_ARG`). Le plugin n'émet LEN 9 que si
`version >= 15`. `POMPPC_GL_XFER16=0` reprend l'ancien trou (fenêtre 16 bits
et Z 16 sans aller-retour).

Le framebuffer interne de l'hôte reste 32 bits : seule la copie BAR0 ↔
surface change de format.
