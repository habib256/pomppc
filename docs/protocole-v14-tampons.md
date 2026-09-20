# Protocole qgpu v14 — tampons hôte

Les maillages statiques ne retraversent plus la fenêtre partagée à chaque
dessin. L'invité emballe une fois au format `DRAW_RAW`, copie le paquet dans
un tampon hôte, et les images suivantes pointent cet identifiant.

## L'ajout

`QGPU_OP_BUF_CREATE` (`0x001B`), longueur 3 : `[id, size]`

`QGPU_OP_BUF_DESTROY` (`0x001C`), longueur 2 : `[id]`

`QGPU_OP_BUF_SUBDATA` (`0x001D`), longueur 5 : `[id, dst_off, src_off, len]`

Copie `len` octets de BAR0 (`src_off`) dans le tampon `id` à `dst_off`.
`len = 0` : sans effet. Un débordement du tampon ou de BAR0 rend `OOB`.
Recréer un id déjà pris rend `LIMIT`. `size = 0` ou `size > 16 Mio` rend
`BAD_ARG`.

`QGPU_OP_DRAW_RAW_BUF` (`0x0059`), longueur 12 :

```
[mode, n, vbuf, voff, pas, format, ibuf, ioff, itype, premier, nverts]
```

Mêmes règles que `DRAW_RAW`, mais `voff` / `ioff` sont relatifs au tampon
hôte `vbuf` / `ibuf`. `QGPU_BUF_SHMEM` (`0xFFFFFFFF`) à la place d'un
identifiant veut dire « offset dans BAR0 », pour mixer sommets hôte et
indices encore dans la fenêtre.

Un flux v13 ignore ces opcodes (`BAD_OPCODE`). Le plugin ne les émet que si
`version >= 14`. `POMPPC_GL_VBO=0` reprend `DRAW_RAW` à chaque image.

Le layout d'un VBO d'application n'est **pas** celui de `DRAW_RAW` : le
plugin emballe au premier dessin après `FlushBuffer`, puis réutilise.

## Bornes

256 tampons, 16 Mio chacun, 64 par client. À la fermeture d'un client, le
kext envoie `BUF_DESTROY` sur sa plage, comme pour les textures.
