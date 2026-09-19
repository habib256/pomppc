/*
 * qgpu_proto.h — contrat hôte ↔ invité du GPU paravirtuel « qgpu » (POMPPC).
 *
 * UNE SEULE SOURCE DE VÉRITÉ, copiée à l'identique à deux endroits :
 *   patches/qgpu/qgpu_proto.h     (hôte : device QEMU + backends + tests)
 *   kext/POMPPCGPU/qgpu_proto.h   (invité : kext + programmes userland Tiger)
 * tests/run-all.sh vérifie que les deux copies sont identiques au bit près.
 *
 * Ce fichier ne contient QUE des macros : il doit se compiler tel quel dans
 * le noyau Tiger (gcc 4.0, C++, -nostdinc), dans QEMU (C11) et dans un
 * programme Tiger userland (gcc 4.0, C89). Aucun include, aucun typedef.
 *
 * ── Architecture ────────────────────────────────────────────────────────────
 *
 *   Le device est un COPROCESSEUR DE COMMANDES, pas un écran : il n'a pas de
 *   scanout. L'invité écrit un flux de commandes dans la fenêtre partagée
 *   (BAR0, de la RAM côté QEMU), déclare (offset, longueur) dans les registres
 *   (BAR1), frappe le doorbell ; l'hôte exécute le flux sur son backend de
 *   rendu (logiciel de référence, ou OpenGL — cf. QGPU_CAP_*), met à jour le
 *   compteur FENCE et le STATUS, et lève l'interruption DONE si elle est
 *   démasquée. Les surfaces vivent côté hôte ; l'invité les lit avec
 *   SURF_READBACK (hôte → BAR0) et les remplit avec SURF_UPLOAD (BAR0 → hôte).
 *
 *   Depuis la v9 le doorbell a DEUX modes : synchrone (valeur 1, celui de
 *   v1–v8, inchangé au bit près) et asynchrone (valeur 3, la soumission est
 *   mise en file et la main est rendue tout de suite). Voir la section « v9 »
 *   en fin de fichier : c'est là qu'est écrit le contrat mémoire.
 *
 * ── Endianness ──────────────────────────────────────────────────────────────
 *
 *   L'invité est un PowerPC big-endian. Les registres (BAR1) sont déclarés
 *   DEVICE_BIG_ENDIAN côté QEMU, et le flux de commandes est défini comme une
 *   suite de mots de 32 bits BIG-ENDIAN : l'invité ne fait AUCUN échange
 *   d'octets, jamais. Les flottants sont IEEE 754 simple précision, big-endian.
 *   Les pixels 32 bpp sont des mots xRGB big-endian (octets : x, R, G, B), soit
 *   exactement le format du framebuffer QFB et du WindowServer Tiger.
 */

#ifndef QGPU_PROTO_H
#define QGPU_PROTO_H

/* ── Identité PCI ────────────────────────────────────────────────────────── */
#define QGPU_PCI_VENDOR_ID      0x1234      /* vendor QEMU */
#define QGPU_PCI_DEVICE_ID      0x0fb2      /* local, à côté de qfb-pci (0x0fb1) */
/* IOPCIPrimaryMatch attend 0xDDDDVVVV : device en poids fort. */
#define QGPU_IOPCI_PRIMARY_MATCH 0x0fb21234

#define QGPU_MAGIC              0x71677031  /* 'qgp1' */
#define QGPU_PROTO_VERSION      10  /* v2 : profondeur, état GL ; v3 : textures ;
                                       v4 : brouillard, 2e unité, lignes, points ;
                                       v5 : 4 unités, GL_COMBINE ;
                                       v6 : stencil ;
                                       v7 : géométrie brute (matrices, éclairage,
                                            texgen, découpe, DRAW_RAW) ;
                                       v8 : fin du pipeline fixe (mélange à couleur
                                            constante, MIN/MAX, opérations logiques,
                                            modes de polygone, pointillés,
                                            requêtes d'occlusion) ;
                                       v9 : doorbell asynchrone (file de
                                            soumissions, thread de rendu hôte) ;
                                       v10 : ce qui manquait à OpenGL 1.2–1.4 :
                                            textures 1D, 3D, cube, rectangle,
                                            profondeur et comparaison, S3TC,
                                            formats de l'application convertis
                                            par l'hôte, sous-images, LOD ;
                                            couleur secondaire, paramètres de
                                            point */

/* ── BAR0 : fenêtre partagée (RAM) ───────────────────────────────────────── */
#define QGPU_SHMEM_DEFAULT_MB   64
#define QGPU_SHMEM_MIN_MB       16
#define QGPU_SHMEM_MAX_MB       256

/* ── BAR1 : registres (4 Kio, accès 32 bits, big-endian) ─────────────────── */
#define QGPU_CTRL_BAR_SIZE      4096
/* v9 : la fenêtre de registres passe de 0x40 à 0x50 octets. Au-delà, la
 * lecture rend 0xFFFFFFFF et l'écriture est ignorée, comme avant. */
#define QGPU_CTRL_TOPADDR       0x50

#define QGPU_REG_MAGIC          0x00  /* r  : QGPU_MAGIC ; w : reset complet */
#define QGPU_REG_VERSION        0x04  /* r  : QGPU_PROTO_VERSION */
#define QGPU_REG_CAPS           0x08  /* r  : QGPU_CAP_* du backend actif */
#define QGPU_REG_SHMEM_SIZE     0x0C  /* r  : taille de BAR0 en octets */
#define QGPU_REG_SUBMIT_OFF     0x10  /* rw : offset du flux dans BAR0 (mult. de 4) */
#define QGPU_REG_SUBMIT_LEN     0x14  /* rw : longueur du flux en octets (mult. de 4) */
#define QGPU_REG_DOORBELL       0x18  /* w  : QGPU_DOORBELL_* ; r : soumissions en
                                              attente ou en cours (0 = tout est fini) */
#define QGPU_REG_FENCE          0x1C  /* r  : nombre de soumissions TERMINÉES */
#define QGPU_REG_STATUS         0x20  /* r  : QGPU_ST_* de la dernière TERMINÉE */
#define QGPU_REG_STATUS_PC      0x24  /* r  : index (en mots) de la commande fautive */
#define QGPU_REG_IRQ_MASK       0x28  /* rw : QGPU_IRQ_* démasquées */
#define QGPU_REG_IRQ            0x2C  /* r  : en attente ; w : acquitte les bits écrits */
#define QGPU_REG_DEBUG          0x30  /* w  : un octet vers stderr de QEMU (trace invité) */
#define QGPU_REG_BACKEND_NAME   0x34  /* r  : 4 premiers caractères du backend ('soft'/'gl  ') */
/* v9 — file de soumissions. Détail et contrat mémoire : section « v9 ». */
#define QGPU_REG_QUEUE_FREE     0x38  /* r  : places libres dans la file
                                              (QGPU_QUEUE_DEPTH − DOORBELL) */
#define QGPU_REG_FENCE_SUBMITTED 0x3C /* r  : nombre de soumissions ACCEPTÉES ; la
                                              barrière de celle qu'on vient de
                                              soumettre est sa valeur juste après
                                              l'écriture du doorbell */
#define QGPU_REG_SUBMIT_ST      0x40  /* r  : QGPU_ST_OK ou QGPU_ST_QUEUE_FULL —
                                              suite donnée à la DERNIÈRE écriture
                                              du doorbell (acceptation, pas rendu) */
#define QGPU_REG_ERRORS         0x44  /* r  : nombre de soumissions TERMINÉES avec un
                                              statut ≠ QGPU_ST_OK depuis le reset */
#define QGPU_REG_QUEUE_DEPTH    0x48  /* r  : profondeur de la file de CE device */

/* v9 : valeurs écrites dans QGPU_REG_DOORBELL. */
#define QGPU_DOORBELL_GO        0x00000001  /* v1 : exécuter, synchrone */
#define QGPU_DOORBELL_ASYNC     0x00000002  /* v9 : … mais en file (écrire GO|ASYNC = 3) */

/* v9 : profondeur de la file du device de référence. Le device publie la
 * sienne dans QGPU_REG_QUEUE_DEPTH — un invité prudent lit le registre
 * plutôt que cette macro. */
#define QGPU_QUEUE_DEPTH        16

#define QGPU_CAP_SOFT           0x00000001  /* backend logiciel de référence */
#define QGPU_CAP_GL             0x00000002  /* backend OpenGL (rendu sur le GPU hôte) */
/* v8 : le backend actif sait compter les échantillons (QUERY_*). Le backend de
 * référence le sait toujours ; le backend OpenGL ne l'annonce que s'il a résolu
 * glGenQueries/glBeginQuery/glEndQuery/glGetQueryObjectuiv (GL 1.5 ou
 * ARB_occlusion_query). Sans ce bit, les opcodes QUERY_* répondent
 * QGPU_ST_BACKEND : l'invité se replie, il ne plante pas. */
#define QGPU_CAP_OCCLUSION      0x00000004
/* v9 : le device sait mettre les soumissions en file et les exécuter sur un
 * thread de rendu. Sans ce bit, QGPU_DOORBELL_ASYNC est traité comme
 * QGPU_DOORBELL_GO (exécution synchrone) : un invité v9 reste correct sur un
 * device qui n'a pas le thread, il est seulement aussi lent qu'en v8. */
#define QGPU_CAP_ASYNC          0x00000008
/* v10 : le backend actif tient ce que la v10 demande au GPU : cibles et
 * paramètres de texture (1D, 3D, cube, rectangle, profondeur et comparaison,
 * MIRRORED_REPEAT, CLAMP_TO_BORDER, LOD) et paramètres de point. Le backend de
 * référence le tient toujours ; le backend OpenGL ne l'annonce que si l'hôte
 * est en OpenGL 1.4 au moins et qu'il a résolu glTexImage3D et
 * glPointParameterf(v). Sans ce bit, TEX_CREATE3 d'une autre cible que 2D, toute
 * image de profondeur, les paramètres de texture v10 et des paramètres de
 * point autres qu'initiaux répondent QGPU_ST_BACKEND. Les conversions de
 * format, la décompression S3TC, TEX_SUBIMAGE et la génération des mipmaps
 * sont faites par le CŒUR : elles ne dépendent pas de ce bit. */
#define QGPU_CAP_GL14           0x00000010

#define QGPU_IRQ_DONE           0x00000001

/* ── Statuts ─────────────────────────────────────────────────────────────── */
#define QGPU_ST_OK              0
#define QGPU_ST_BAD_SUBMIT      1   /* offset/longueur hors de BAR0 ou non alignés */
#define QGPU_ST_BAD_HEADER      2   /* longueur de commande nulle ou dépassant le flux */
#define QGPU_ST_BAD_OPCODE      3
#define QGPU_ST_BAD_ARG         4   /* argument hors bornes (id, dimensions, format…) */
#define QGPU_ST_OOB             5   /* accès BAR0 hors de la fenêtre partagée */
#define QGPU_ST_NO_CTX          6   /* aucun contexte lié */
#define QGPU_ST_NO_SURF         7   /* surface inexistante ou aucune surface liée */
#define QGPU_ST_LIMIT           8   /* trop d'objets (QGPU_MAX_*) */
#define QGPU_ST_BACKEND         9   /* erreur du backend hôte */
/* v9 : file pleine. Ce statut ne décrit PAS un flux : il dit qu'une écriture
 * de QGPU_DOORBELL_ASYNC a été REFUSÉE. Rien n'a été mis en file, rien ne sera
 * exécuté, ni FENCE ni QGPU_REG_FENCE_SUBMITTED n'avancent, et QGPU_REG_ERRORS
 * ne bouge pas non plus (aucune soumission n'a fini en erreur). Il n'apparaît
 * que dans QGPU_REG_SUBMIT_ST, jamais dans QGPU_REG_STATUS. Le doorbell
 * SYNCHRONE ne le rend jamais : il attend une place. */
#define QGPU_ST_QUEUE_FULL      10

/* ── Limites ─────────────────────────────────────────────────────────────── */
#define QGPU_MAX_CTX            16
#define QGPU_MAX_SURF           64
#define QGPU_MAX_SURF_DIM       4096
#define QGPU_MAX_CMD_WORDS      (1 << 20)   /* 4 Mio par soumission */
#define QGPU_MAX_VERTS          (1 << 16)
#define QGPU_MAX_TEX            512         /* v3 */
#define QGPU_MAX_TEX_DIM        2048
#define QGPU_MAX_TEX_LEVELS     12
#define QGPU_MAX_TEX_3D_DIM     256         /* v10 : largeur, hauteur et profondeur */
#define QGPU_MAX_LOD_BIAS       16          /* v10 : |biais de texture + biais d'unité| */
#define QGPU_MAX_UNITS          4           /* v5 : unités de texture */
#define QGPU_MAX_LIGHTS         8           /* v7 : GL_LIGHT0..GL_LIGHT7 */
#define QGPU_MAX_CLIP_PLANES    6           /* v7 : GL_CLIP_PLANE0..5 */
/* v8 : requêtes d'occlusion. Le découpage est celui des textures : l'espace
   d'identifiants est GLOBAL au device et le kext en donne une tranche à chaque
   client (query_base = index × QGPU_CLIENT_QUERY_IDS), pour qu'aucun client ne
   puisse lire ni écraser la requête d'un autre. 16 requêtes en vol par client
   est très au-delà de ce que GLEngine demande (une à la fois par contexte). */
#define QGPU_MAX_QUERIES        64
/* v7 : les commandes de géométrie sont longues (SET_LIGHT en fait 26), et la v8
   ajoute SET_POLYGON_STIPPLE, qui en fait 32. Le cœur recopie les arguments
   dans un tableau de cette taille : la borne est NOMMÉE ici pour que l'hôte et
   l'invité ne puissent pas en avoir deux idées. */
#define QGPU_MAX_CMD_ARGS       32

/* ── Flux de commandes ───────────────────────────────────────────────────────
 *
 *   mot 0 (en-tête) : opcode << 16 | longueur en MOTS, en-tête compris.
 *   mots 1..n-1     : arguments, dans l'ordre documenté ci-dessous.
 *   Une longueur incompatible avec l'opcode = QGPU_ST_BAD_ARG.
 */
#define QGPU_CMD_HDR(op, len)   (((unsigned long)(op) << 16) | ((unsigned long)(len) & 0xFFFF))
#define QGPU_CMD_OP(hdr)        (((unsigned long)(hdr) >> 16) & 0xFFFF)
#define QGPU_CMD_LEN(hdr)       ((unsigned long)(hdr) & 0xFFFF)

#define QGPU_OP_NOP             0x0000  /* [] */

#define QGPU_OP_CTX_CREATE      0x0001  /* [ctx]  ctx dans 0..QGPU_MAX_CTX-1 */
#define QGPU_OP_CTX_DESTROY     0x0002  /* [ctx] */
#define QGPU_OP_CTX_BIND        0x0003  /* [ctx]  contexte courant de la soumission */

#define QGPU_OP_SURF_CREATE     0x0010  /* [surf, width, height, format] */
#define QGPU_OP_SURF_DESTROY    0x0011  /* [surf] */
#define QGPU_OP_SURF_BIND       0x0012  /* [surf]  cible de rendu du contexte courant */
#define QGPU_OP_SURF_READBACK   0x0013  /* [surf, off, stride, x, y, w, h]  hôte → BAR0 */
#define QGPU_OP_SURF_UPLOAD     0x0014  /* [surf, off, stride, x, y, w, h]  BAR0 → hôte */
#define QGPU_OP_DEPTH_READBACK  0x0015  /* v2, idem, valeurs de profondeur f32 BE dans [0,1] */
#define QGPU_OP_DEPTH_UPLOAD    0x0016  /* v2, idem, BAR0 → hôte */
#define QGPU_OP_STENCIL_READBACK 0x0017 /* v6, idem, valeurs de stencil, cf. ci-dessous */
#define QGPU_OP_STENCIL_UPLOAD  0x0018  /* v6, idem, BAR0 → hôte */

#define QGPU_OP_CLEAR           0x0020  /* [mask, color 0xxxRRGGBB, depth f32] */
#define QGPU_OP_VIEWPORT        0x0021  /* [x, y, w, h]  (réservé : accepté, sans effet) */
#define QGPU_OP_SET_STATE       0x0022  /* v2, [clé QGPU_SK_*, valeur] du contexte courant */
#define QGPU_OP_DRAW_TRIANGLES  0x0030  /* [nverts, off]  sommets dans BAR0, cf. ci-dessous */
#define QGPU_OP_DRAW_TRIANGLES_TEX 0x0031 /* v3, [nverts, off]  sommets texturés, cf. ci-dessous */
#define QGPU_OP_DRAW_TRIANGLES_TEX2 0x0032 /* v4, [nverts, off]  deux unités de texture */
#define QGPU_OP_DRAW_LINES      0x0033  /* v4, [nverts, off]  segments (paires), sommets de 8 mots */
#define QGPU_OP_DRAW_POINTS     0x0034  /* v4, [nverts, off]  points, sommets de 8 mots */
#define QGPU_OP_DRAW_TRIANGLES_TEXN 0x0035 /* v5, [nverts, off, nunits]  1 à 4 unités, cf. ci-dessous */

#define QGPU_OP_TEX_CREATE      0x0040  /* v3, [tex] */
#define QGPU_OP_TEX_DESTROY     0x0041  /* v3, [tex] */
#define QGPU_OP_TEX_IMAGE       0x0042  /* v3, [tex, niveau, w, h, format de base, off] texels 0xAARRGGBB BE, w×h */
#define QGPU_OP_TEX_PARAM       0x0043  /* v3, [tex, clé QGPU_TP_*, valeur] */
/* v10 : textures générales. Détail du contrat plus bas, section « v10 ». */
#define QGPU_OP_TEX_CREATE3     0x0044  /* [tex, cible] */
#define QGPU_OP_TEX_IMAGE3      0x0045  /* [tex, cible d'image, niveau, w, h, d,
                                           format de base, format, type, off,
                                           octets par ligne, octets par tranche] */
#define QGPU_OP_TEX_SUBIMAGE    0x0046  /* [tex, cible d'image, niveau, x, y, z,
                                           w, h, d, format, type, off,
                                           octets par ligne, octets par tranche] */

/* v7 : géométrie brute. Détail du contrat plus bas, section « v7 ». */
#define QGPU_OP_SET_MATRIX      0x0050  /* [quelle, 16 flottants, ordre colonne] */
#define QGPU_OP_DEPTH_RANGE     0x0051  /* [proche, lointain] flottants dans [0,1] */
#define QGPU_OP_SET_LIGHT       0x0052  /* [i, actif, amb4, diff4, spec4, pos4, spot3,
                                           exposant, coupure, att3] */
#define QGPU_OP_SET_MATERIAL    0x0053  /* [face, amb4, diff4, spec4, emis4, brillance] */
#define QGPU_OP_SET_LIGHT_MODEL 0x0054  /* [ambiante4 du modèle d'éclairage] */
#define QGPU_OP_SET_TEXGEN      0x0055  /* [unité, coord, actif, mode, plan objet4, plan œil4] */
#define QGPU_OP_SET_CLIP_PLANE  0x0056  /* [i, actif, équation4 en coordonnées œil] */
#define QGPU_OP_SET_CURRENT     0x0057  /* [quoi QGPU_CUR_*, x, y, z, w] */
#define QGPU_OP_DRAW_RAW        0x0058  /* [mode, n, voff, pas, format, ioff, itype,
                                           premier, nverts] */

/* v8 : fin du pipeline fixe. Détail du contrat plus bas, section « v8 ». */
#define QGPU_OP_SET_POLYGON_STIPPLE 0x0060 /* [32 mots de 32 bits, cf. ci-dessous] */
#define QGPU_OP_QUERY_BEGIN     0x0061  /* [id] */
#define QGPU_OP_QUERY_END       0x0062  /* [id] */
#define QGPU_OP_QUERY_RESULT    0x0063  /* [id, off] : 2 mots BE écrits à `off` */

/* Longueurs (en mots, en-tête compris) attendues par opcode. */
#define QGPU_LEN_NOP            1
#define QGPU_LEN_CTX            2
#define QGPU_LEN_SURF_CREATE    5
#define QGPU_LEN_SURF           2
#define QGPU_LEN_SURF_XFER      8
#define QGPU_LEN_CLEAR          4
#define QGPU_LEN_VIEWPORT       5
#define QGPU_LEN_DRAW           3
#define QGPU_LEN_DRAW_N         4
#define QGPU_LEN_SET_STATE      3
#define QGPU_LEN_TEX            2
#define QGPU_LEN_TEX_IMAGE      7
#define QGPU_LEN_TEX_PARAM      4
#define QGPU_LEN_TEX_CREATE3    3           /* v10 */
#define QGPU_LEN_TEX_IMAGE3     13
#define QGPU_LEN_TEX_SUBIMAGE   15
#define QGPU_LEN_SET_MATRIX     18          /* v7 */
#define QGPU_LEN_DEPTH_RANGE    3
#define QGPU_LEN_SET_LIGHT      27
#define QGPU_LEN_SET_MATERIAL   19
#define QGPU_LEN_SET_LIGHT_MODEL 5
#define QGPU_LEN_SET_TEXGEN     13
#define QGPU_LEN_SET_CLIP_PLANE 7
#define QGPU_LEN_SET_CURRENT    6
#define QGPU_LEN_DRAW_RAW       10
#define QGPU_LEN_SET_POLYGON_STIPPLE 33     /* v8 : la plus longue commande */
#define QGPU_LEN_QUERY          2
#define QGPU_LEN_QUERY_RESULT   3

/* Formats de surface. Le mot de pixel échangé est 0xAARRGGBB big-endian :
 * l'octet « x » du framebuffer Tiger EST l'alpha (v2 ; v1 l'ignorait). */
#define QGPU_FMT_XRGB8888       1   /* 32 bpp ARGB big-endian */
#define QGPU_FMT_MASK           0xFF
#define QGPU_FMT_FLAG_DEPTH     0x100  /* v2 : la surface a un tampon de profondeur */
/* v6 : la surface a un tampon de stencil de 8 bits. IL EXIGE LA PROFONDEUR
 * (QGPU_FMT_FLAG_STENCIL seul = QGPU_ST_BAD_ARG) : le stencil sans profondeur
 * n'a aucun usage dans GLEngine, et l'imposer laisse l'hôte OpenGL n'allouer
 * qu'UN tampon combiné GL_DEPTH24_STENCIL8 — le seul format de stencil garanti
 * partout (EXT_packed_depth_stencil, GL 3.0), et le seul qui rende un FBO
 * complet sur tous les pilotes testés. */
#define QGPU_FMT_FLAG_STENCIL   0x200

/* CLEAR : masque. Comme glClear, l'effacement respecte les ciseaux, le
 * masque de couleur et le masque de profondeur du contexte courant. */
#define QGPU_CLEAR_COLOR        0x1
#define QGPU_CLEAR_DEPTH        0x2   /* sans effet si la surface n'a pas de profondeur */
/* v6 : efface le stencil à QGPU_SK_STENCIL_CLEAR, en respectant le masque
 * d'écriture QGPU_SK_STENCIL_WRITE_MASK et les ciseaux, comme glClear. La
 * valeur d'effacement est une CLÉ D'ÉTAT et non un mot de la commande : la
 * longueur de CLEAR reste QGPU_LEN_CLEAR (4 mots), donc un hôte v6 accepte
 * tels quels les flux v1–v5. */
#define QGPU_CLEAR_STENCIL      0x4

/* SET_STATE : clés. Les valeurs d'énumération sont celles d'OpenGL
 * (GL_LESS, GL_SRC_ALPHA, GL_FUNC_ADD…), l'invité les recopie telles quelles.
 * État initial = état initial d'OpenGL (tests désactivés, LESS, écriture de
 * profondeur active, masque de couleur 0xF, ONE/ZERO, FUNC_ADD, ALWAYS, 0). */
#define QGPU_SK_DEPTH_TEST      1   /* booléen */
#define QGPU_SK_DEPTH_FUNC      2   /* GL_NEVER..GL_ALWAYS (0x0200..0x0207) */
#define QGPU_SK_DEPTH_WRITE     3   /* booléen */
#define QGPU_SK_COLOR_MASK      4   /* bit0 R, bit1 G, bit2 B, bit3 A */
#define QGPU_SK_BLEND           5   /* booléen */
#define QGPU_SK_BLEND_SRC_RGB   6
#define QGPU_SK_BLEND_DST_RGB   7
#define QGPU_SK_BLEND_SRC_A     8
#define QGPU_SK_BLEND_DST_A     9
#define QGPU_SK_BLEND_EQ_RGB    10  /* GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT */
#define QGPU_SK_BLEND_EQ_A      11
#define QGPU_SK_ALPHA_TEST      12  /* booléen */
#define QGPU_SK_ALPHA_FUNC      13  /* GL_NEVER..GL_ALWAYS */
#define QGPU_SK_ALPHA_REF       14  /* flottant (bits IEEE) */
#define QGPU_SK_SCISSOR         15  /* booléen */
#define QGPU_SK_SCISSOR_X       16  /* rectangle en coordonnées de surface, origine en haut à gauche */
#define QGPU_SK_SCISSOR_Y       17
#define QGPU_SK_SCISSOR_W       18
#define QGPU_SK_SCISSOR_H       19
#define QGPU_SK_TEXTURE         20  /* v3, booléen : texturage de l'unité unique */
#define QGPU_SK_TEX_BIND        21  /* v3, identifiant de texture */
#define QGPU_SK_TEX_ENV_MODE    22  /* v3, GL_MODULATE, GL_DECAL, GL_BLEND, GL_REPLACE, GL_ADD ;
                                       v5 : GL_COMBINE (cf. QGPU_SK_COMBINE*) */
#define QGPU_SK_TEX_ENV_COLOR   23  /* v3, 0xAARRGGBB */
#define QGPU_SK_FOG             24  /* v4, booléen : le mot w du sommet est le facteur f */
#define QGPU_SK_FOG_COLOR       25  /* v4, 0xAARRGGBB */
#define QGPU_SK_LINE_WIDTH      26  /* v4, flottant (bits IEEE), > 0 */
#define QGPU_SK_POINT_SIZE      27  /* v4, flottant (bits IEEE), > 0 */
#define QGPU_SK_TEXTURE1        28  /* v4, texturage de la 2e unité */
#define QGPU_SK_TEX1_BIND       29
#define QGPU_SK_TEX1_ENV_MODE   30
#define QGPU_SK_TEX1_ENV_COLOR  31
#define QGPU_SK_POLY_OFFSET     32  /* v4, booléen : décalage des triangles pleins */
#define QGPU_SK_POLY_FACTOR     33  /* v4, flottant (bits IEEE) */
#define QGPU_SK_POLY_UNITS      34  /* v4, flottant (bits IEEE) */
#define QGPU_SK_TEXTURE2        35  /* v5, unités 2 et 3 : mêmes quatre clés */
#define QGPU_SK_TEX2_BIND       36
#define QGPU_SK_TEX2_ENV_MODE   37
#define QGPU_SK_TEX2_ENV_COLOR  38
#define QGPU_SK_TEXTURE3        39
#define QGPU_SK_TEX3_BIND       40
#define QGPU_SK_TEX3_ENV_MODE   41
#define QGPU_SK_TEX3_ENV_COLOR  42
#define QGPU_SK_COMBINE0        43  /* v5, unités 0..3 : fonctions et échelles, QGPU_COMBINE() */
#define QGPU_SK_COMBINE_SRC0    47  /* v5, unités 0..3 : sources et opérandes, QGPU_COMBINE_SRC_*() */
/* v6 : stencil. Sans tampon de stencil sur la surface, le test est inopérant
 * (le fragment passe) et rien n'est écrit, comme en OpenGL. REF, les deux
 * masques et la valeur d'effacement sont bornés à 0..255 (stencil de 8 bits) ;
 * la comparaison est (REF & VALUE_MASK) fonc (stencil & VALUE_MASK). */
#define QGPU_SK_STENCIL_TEST    51  /* booléen ; initial 0 */
#define QGPU_SK_STENCIL_FUNC    52  /* GL_NEVER..GL_ALWAYS ; initial GL_ALWAYS */
#define QGPU_SK_STENCIL_REF     53  /* 0..255 ; initial 0 */
#define QGPU_SK_STENCIL_VALUE_MASK 54  /* 0..255 ; initial 0xFF */
#define QGPU_SK_STENCIL_WRITE_MASK 55  /* 0..255 ; initial 0xFF */
#define QGPU_SK_STENCIL_OP_FAIL 56  /* QGPU_SOP_* ; initial GL_KEEP */
#define QGPU_SK_STENCIL_OP_ZFAIL 57 /* idem ; test de stencil réussi, profondeur échouée */
#define QGPU_SK_STENCIL_OP_ZPASS 58 /* idem ; les deux réussis, OU pas de test de
                                       profondeur (le fragment « passe » la profondeur) */
#define QGPU_SK_STENCIL_CLEAR   59  /* 0..255 ; valeur lue par QGPU_CLEAR_STENCIL */
/* v7 : étage géométrique. Ces clés ne servent QU'À DRAW_RAW — les opcodes de
 * dessin antérieurs reçoivent des sommets déjà transformés, éclairés, aplatis
 * et découpés, et continuent de les prendre tels quels quelles que soient ces
 * valeurs. Une seule exception assumée, dite là où elle est : le brouillard. */
#define QGPU_SK_LIGHTING        60  /* booléen ; initial 0 */
#define QGPU_SK_NORMALIZE       61  /* booléen (GL_NORMALIZE) */
#define QGPU_SK_RESCALE_NORMAL  62  /* booléen (GL_RESCALE_NORMAL) ; ignoré si NORMALIZE */
#define QGPU_SK_SHADE_MODEL     63  /* GL_FLAT 0x1D00 / GL_SMOOTH 0x1D01 ; initial SMOOTH */
#define QGPU_SK_CULL_FACE       64  /* booléen */
#define QGPU_SK_CULL_MODE       65  /* GL_FRONT 0x0404, GL_BACK 0x0405, GL_FRONT_AND_BACK 0x0408 */
#define QGPU_SK_FRONT_FACE      66  /* GL_CW 0x0900 / GL_CCW 0x0901 ; initial CCW */
#define QGPU_SK_COLOR_MATERIAL  67  /* booléen */
#define QGPU_SK_COLOR_MAT_FACE  68  /* GL_FRONT, GL_BACK, GL_FRONT_AND_BACK ; initial les deux */
#define QGPU_SK_COLOR_MAT_MODE  69  /* GL_EMISSION 0x1600, GL_AMBIENT 0x1200, GL_DIFFUSE 0x1201,
                                       GL_SPECULAR 0x1202, GL_AMBIENT_AND_DIFFUSE 0x1602 (initial) */
#define QGPU_SK_LOCAL_VIEWER    70  /* booléen : observateur local */
#define QGPU_SK_TWO_SIDE        71  /* booléen : éclairage deux faces */
#define QGPU_SK_COLOR_CONTROL   72  /* GL_SINGLE_COLOR 0x81F9 (initial) /
                                       GL_SEPARATE_SPECULAR_COLOR 0x81FA */
/* Brouillard calculé par l'hôte (v7). QGPU_SK_FOG_MODE vaut initialement
 * QGPU_FOG_VERTEX : le facteur vient du sommet, comme en v4 — c'est ce qui
 * garde les flux v4–v6 valides. Avec GL_LINEAR / GL_EXP / GL_EXP2, DRAW_RAW
 * calcule f depuis la coordonnée de brouillard du sommet si le format en a
 * une, sinon depuis |z œil| (GL_FRAGMENT_DEPTH). Les opcodes antérieurs
 * ignorent ce mode et gardent le facteur par sommet. */
#define QGPU_SK_FOG_MODE        73  /* QGPU_FOG_VERTEX, GL_LINEAR, GL_EXP, GL_EXP2 */
#define QGPU_SK_FOG_DENSITY     74  /* flottant (bits IEEE), >= 0 ; initial 1.0 */
#define QGPU_SK_FOG_START       75  /* flottant ; initial 0.0 */
#define QGPU_SK_FOG_END         76  /* flottant ; initial 1.0 */
/* ── v8 : ce qui manquait au pipeline fixe ──────────────────────────────────
 *
 * Contrairement aux clés v7, celles-ci valent pour TOUS les chemins de dessin :
 * elles agissent au fragment (mélange, opération logique, pointillé de polygone)
 * ou sur l'assemblage des triangles (mode de polygone, pointillé de ligne), donc
 * après l'endroit où les deux chemins se rejoignent. Un flux v1–v7 qui ne les
 * pose jamais garde exactement son rendu : leurs valeurs initiales sont celles
 * d'OpenGL, et elles sont toutes neutres. */
#define QGPU_SK_BLEND_COLOR     77  /* 0xAARRGGBB ; couleur constante de mélange
                                       (GL_CONSTANT_COLOR & co.) ; initial 0 */
/* Opération logique. Quand elle est active, elle REMPLACE le mélange (le
 * mélange est ignoré, comme le dit la spécification d'OpenGL) ; elle travaille
 * sur les quatre canaux de 8 bits du pixel et son résultat passe par le masque
 * de couleur. glClear n'en dépend pas. */
#define QGPU_SK_LOGIC_OP        78  /* booléen (GL_COLOR_LOGIC_OP) ; initial 0 */
#define QGPU_SK_LOGIC_OP_MODE   79  /* GL_CLEAR 0x1500 … GL_SET 0x150F ; initial GL_COPY */
/* Modes de polygone. Ils s'appliquent aux triangles de DRAW_RAW comme à ceux
 * des DRAW_TRIANGLES* antérieurs (pour ceux-là, la face avant est établie sur
 * le sens trigonométrique À L'ÉCRAN, comme en v7 pour le chemin brut). */
#define QGPU_SK_POLYGON_MODE_FRONT 80  /* GL_POINT 0x1B00, GL_LINE 0x1B01,
                                          GL_FILL 0x1B02 (initial) */
#define QGPU_SK_POLYGON_MODE_BACK  81  /* idem */
#define QGPU_SK_POLY_OFFSET_LINE   82  /* booléen ; décalage en mode LINE */
#define QGPU_SK_POLY_OFFSET_POINT  83  /* booléen ; décalage en mode POINT.
                                          Facteur et unités sont ceux de la v4
                                          (QGPU_SK_POLY_FACTOR / _UNITS). */
/* Pointillé de ligne. Le compteur repart de 0 à chaque primitive et à chaque
 * SEGMENT de GL_LINES (et de l'ancien DRAW_LINES) ; il court le long d'un
 * GL_LINE_STRIP et d'un GL_LINE_LOOP. Bit employé : (compteur / facteur) & 15. */
#define QGPU_SK_LINE_STIPPLE       84  /* booléen ; initial 0 */
#define QGPU_SK_LINE_STIPPLE_FACTOR 85 /* 1..256 ; initial 1 */
#define QGPU_SK_LINE_STIPPLE_PATTERN 86 /* 16 bits ; initial 0xFFFF */
#define QGPU_SK_POLYGON_STIPPLE    87  /* booléen ; motif posé par
                                          QGPU_OP_SET_POLYGON_STIPPLE */
/* v10 : biais de LOD de l'UNITÉ u (GL_TEXTURE_FILTER_CONTROL /
 * GL_TEXTURE_LOD_BIAS de glTexEnv, OpenGL 1.4), flottant (bits IEEE), initial
 * 0. Il s'ajoute au biais de la texture (QGPU_TP_LOD_BIAS), cf. section v10. */
#define QGPU_SK_TEX_LOD_BIAS0      88  /* unités 0..3 : + u */
/* v10 : couleur secondaire et paramètres de point (OpenGL 1.4). Comme les clés
 * v7, elles ne servent QU'À DRAW_RAW : les sommets des anciens opcodes n'ont pas
 * de couleur secondaire, et leur taille de point est déjà calculée. */
#define QGPU_SK_COLOR_SUM          92  /* QGPU_CSUM_* ; initial QGPU_CSUM_FORMAT */
#define QGPU_SK_POINT_SIZE_MIN     93  /* flottant dans [0, 64] ; initial 0 */
#define QGPU_SK_POINT_SIZE_MAX     94  /* flottant dans ]0, 64] ; initial 64 */
#define QGPU_SK_POINT_FADE         95  /* flottant ≥ 0 ; initial 1. Sans effet :
                                          le fondu n'existe qu'en
                                          multiéchantillonnage (GL 1.4 §3.3) */
#define QGPU_SK_POINT_ATT_CONST    96  /* flottants ≥ 0 : a, b, c de
                                          GL_POINT_DISTANCE_ATTENUATION ; */
#define QGPU_SK_POINT_ATT_LINEAR   97  /* initial 1, 0, 0 */
#define QGPU_SK_POINT_ATT_QUAD     98
#define QGPU_SK_COUNT           99

/* Valeurs d'énumération d'OpenGL utilisées par les clés v7, nommées pour que
 * l'invité n'ait pas à les recopier à la main. */
#define QGPU_FOG_VERTEX         0       /* v7 : « le sommet fournit le facteur » (v4) */
#define QGPU_FOG_EXP            0x0800  /* GL_EXP */
#define QGPU_FOG_EXP2           0x0801  /* GL_EXP2 */
#define QGPU_FOG_LINEAR         0x2601  /* GL_LINEAR */

/* Opérations de stencil (valeurs OpenGL, recopiées telles quelles). */
#define QGPU_SOP_ZERO           0x0000
#define QGPU_SOP_INVERT         0x150A
#define QGPU_SOP_KEEP           0x1E00
#define QGPU_SOP_REPLACE        0x1E01  /* met REF (non masqué par VALUE_MASK) */
#define QGPU_SOP_INCR           0x1E02  /* sature à 255 */
#define QGPU_SOP_DECR           0x1E03  /* sature à 0 */
#define QGPU_SOP_INCR_WRAP      0x8507  /* v6, GL 1.4 / EXT_stencil_wrap */
#define QGPU_SOP_DECR_WRAP      0x8508

/* Groupe de quatre clés de l'unité u (0..3) : +0 texturage, +1 texture,
 * +2 mode d'environnement, +3 couleur d'environnement. */
#define QGPU_SK_UNIT(u)         ((u) == 0 ? QGPU_SK_TEXTURE : (u) == 1 ? QGPU_SK_TEXTURE1 : \
                                 QGPU_SK_TEXTURE2 + 4 * ((u) - 2))
#define QGPU_SK_U_ENABLE        0
#define QGPU_SK_U_BIND          1
#define QGPU_SK_U_ENV_MODE      2
#define QGPU_SK_U_ENV_COLOR     3

/* GL_COMBINE (v5, ARB_texture_env_combine + dot3), actif quand le mode
 * d'environnement de l'unité vaut GL_COMBINE (0x8570).
 *   QGPU_SK_COMBINE<u>     = fonction RGB | fonction alpha << 4
 *                            | log2(RGB_SCALE) << 8 | log2(ALPHA_SCALE) << 10
 *   QGPU_SK_COMBINE_SRC<u> = pour i = 0..2 : (source | opérande << 3) << 5i pour RGB,
 *                            puis (source | opérande << 3) << (15 + 4i) pour alpha.
 * Valeur initiale d'OpenGL : MODULATE/MODULATE, échelles 1 ;
 * sources TEXTURE, PREVIOUS, CONSTANT ; opérandes RGB SRC_COLOR, SRC_COLOR,
 * SRC_ALPHA ; opérandes alpha SRC_ALPHA. */
#define QGPU_CB_REPLACE         0
#define QGPU_CB_MODULATE        1
#define QGPU_CB_ADD             2
#define QGPU_CB_ADD_SIGNED      3
#define QGPU_CB_INTERPOLATE     4
#define QGPU_CB_SUBTRACT        5
#define QGPU_CB_DOT3_RGB        6   /* RGB seulement */
#define QGPU_CB_DOT3_RGBA       7   /* RGB seulement ; l'alpha prend aussi le produit */
#define QGPU_CS_TEXTURE         0
#define QGPU_CS_CONSTANT        1
#define QGPU_CS_PRIMARY         2
#define QGPU_CS_PREVIOUS        3
#define QGPU_CO_COLOR           0   /* opérandes RGB */
#define QGPU_CO_ONE_MINUS_COLOR 1
#define QGPU_CO_ALPHA           2
#define QGPU_CO_ONE_MINUS_ALPHA 3
#define QGPU_CA_ALPHA           0   /* opérandes alpha */
#define QGPU_CA_ONE_MINUS_ALPHA 1
#define QGPU_COMBINE(rgb, a, rgb_shift, a_shift) \
    ((unsigned long)(rgb) | ((unsigned long)(a) << 4) | \
     ((unsigned long)(rgb_shift) << 8) | ((unsigned long)(a_shift) << 10))
#define QGPU_COMBINE_SRC_RGB(i, src, op) \
    (((unsigned long)(src) | ((unsigned long)(op) << 3)) << (5 * (i)))
#define QGPU_COMBINE_SRC_A(i, src, op) \
    (((unsigned long)(src) | ((unsigned long)(op) << 3)) << (15 + 4 * (i)))
#define QGPU_COMBINE_DEFAULT    QGPU_COMBINE(QGPU_CB_MODULATE, QGPU_CB_MODULATE, 0, 0)
#define QGPU_COMBINE_SRC_DEFAULT \
    (QGPU_COMBINE_SRC_RGB(0, QGPU_CS_TEXTURE, QGPU_CO_COLOR) | \
     QGPU_COMBINE_SRC_RGB(1, QGPU_CS_PREVIOUS, QGPU_CO_COLOR) | \
     QGPU_COMBINE_SRC_RGB(2, QGPU_CS_CONSTANT, QGPU_CO_ALPHA) | \
     QGPU_COMBINE_SRC_A(0, QGPU_CS_TEXTURE, QGPU_CA_ALPHA) | \
     QGPU_COMBINE_SRC_A(1, QGPU_CS_PREVIOUS, QGPU_CA_ALPHA) | \
     QGPU_COMBINE_SRC_A(2, QGPU_CS_CONSTANT, QGPU_CA_ALPHA))

/* Textures (v3) : une seule unité, cible 2D. TEX_IMAGE reçoit toujours des
 * texels ARGB ; le format de base (GL_ALPHA, GL_RGB, GL_RGBA, GL_LUMINANCE,
 * GL_LUMINANCE_ALPHA, GL_INTENSITY) dit quels canaux la texture garde, comme
 * le paramètre internalformat d'OpenGL : L et I viennent du canal rouge. Une
 * texture incomplète (filtre de réduction avec mipmaps et chaîne de niveaux
 * incomplète) désactive le texturage, comme en OpenGL. (v10 : autres cibles,
 * autres formats et paramètres ci-dessous, section « v10 ».) */
#define QGPU_TP_MIN_FILTER      1   /* GL_NEAREST, GL_LINEAR, GL_*_MIPMAP_* */
#define QGPU_TP_MAG_FILTER      2   /* GL_NEAREST, GL_LINEAR */
#define QGPU_TP_WRAP_S          3   /* GL_REPEAT, GL_CLAMP, GL_CLAMP_TO_EDGE ;
                                       v10 : GL_MIRRORED_REPEAT, GL_CLAMP_TO_BORDER */
#define QGPU_TP_WRAP_T          4
/* v10 */
#define QGPU_TP_WRAP_R          5   /* comme WRAP_S ; ne sert qu'aux textures 3D */
#define QGPU_TP_BORDER_COLOR    6   /* 0xAARRGGBB ; initial 0 */
#define QGPU_TP_MIN_LOD         7   /* flottant (bits IEEE) ; initial −1000 */
#define QGPU_TP_MAX_LOD         8   /* flottant ; initial 1000 ; MIN_LOD ≤ MAX_LOD
                                       n'est PAS exigé (OpenGL ne l'exige pas) */
#define QGPU_TP_BASE_LEVEL      9   /* 0..QGPU_MAX_TEX_LEVELS−1 ; initial 0 */
#define QGPU_TP_MAX_LEVEL       10  /* 0..1000 ; initial 1000 */
#define QGPU_TP_LOD_BIAS        11  /* flottant, |biais| ≤ QGPU_MAX_LOD_BIAS ; initial 0 */
#define QGPU_TP_COMPARE_MODE    12  /* GL_NONE 0 (initial) / GL_COMPARE_R_TO_TEXTURE 0x884E */
#define QGPU_TP_COMPARE_FUNC    13  /* GL_NEVER..GL_ALWAYS ; initial GL_LEQUAL */
#define QGPU_TP_DEPTH_MODE      14  /* GL_LUMINANCE (initial), GL_INTENSITY, GL_ALPHA */
#define QGPU_TP_GENERATE_MIPMAP 15  /* booléen ; initial 0 */

/* DRAW_TRIANGLES : nverts multiple de 3 ; à `off` dans BAR0, nverts sommets de
 * QGPU_VERTEX_WORDS mots chacun, tous des flottants big-endian :
 *   x, y  : coordonnées en PIXELS de la surface, origine en haut à gauche,
 *           y vers le bas (convention QuickDraw/Quartz, et glOrtho(0,w,h,0)) ;
 *   z     : profondeur fenêtre dans [0,1] (v2, utilisée si la surface en a) ;
 *   f     : facteur de brouillard dans [0,1] (v4, 1 = pas de brouillard), utilisé
 *           seulement si QGPU_SK_FOG : couleur = f·C + (1−f)·couleur du brouillard.
 *           (Ce mot était « w, réservé, 1.0 » : les flux v1–v3 restent valides.)
 *   r, g, b, a : couleur 0.0..1.0, interpolée (Gouraud) ; a sert au mélange
 *           et au test alpha (v2).
 */
#define QGPU_VERTEX_WORDS       8
#define QGPU_VERTEX_BYTES       (QGPU_VERTEX_WORDS * 4)

/* DRAW_TRIANGLES_TEX (v3) : les 8 mots ci-dessus, puis s, t, r, q. Ces
 * coordonnées sont interpolées linéairement en espace écran, puis s/q et t/q
 * adressent la texture : c'est la forme que produit GLEngine (déjà divisée
 * par w), et le texturage reste correct en perspective. r était ignoré
 * jusqu'à la v9 ; depuis la v10, r/q sert aux textures 3D (troisième
 * coordonnée), aux cartes de cube ((s, t, r) est la direction) et à la
 * comparaison des textures de profondeur (valeur de référence). */
#define QGPU_VERTEX_TEX_WORDS   12
#define QGPU_VERTEX_TEX_BYTES   (QGPU_VERTEX_TEX_WORDS * 4)

/* DRAW_TRIANGLES_TEX2 (v4) : les 12 mots ci-dessus (unité 0), puis s, t, r, q
 * de l'unité 1. L'unité 1 s'applique au résultat de l'unité 0, comme
 * GL_TEXTURE1 en OpenGL 1.3. */
#define QGPU_VERTEX_TEX2_WORDS  16
#define QGPU_VERTEX_TEX2_BYTES  (QGPU_VERTEX_TEX2_WORDS * 4)

/* DRAW_TRIANGLES_TEXN (v5) : les 8 mots de base, puis s, t, r, q de chaque
 * unité 0..nunits-1 (1 ≤ nunits ≤ QGPU_MAX_UNITS). Une unité dont le
 * texturage est coupé garde ses quatre mots, ignorés. Les unités
 * s'appliquent dans l'ordre, comme GL_TEXTURE0..3. */
#define QGPU_VERTEX_TEXN_WORDS(n) (8 + 4 * (n))
#define QGPU_VERTEX_MAX_WORDS   QGPU_VERTEX_TEXN_WORDS(QGPU_MAX_UNITS)

/* DRAW_LINES / DRAW_POINTS (v4) : sommets de QGPU_VERTEX_WORDS mots, sans
 * texture. Lignes : largeur QGPU_SK_LINE_WIDTH, chaque paire est un segment.
 * Points : carrés de QGPU_SK_POINT_SIZE pixels centrés sur le sommet. */

/* STENCIL_READBACK / STENCIL_UPLOAD (v6) : mêmes sept arguments et même
 * longueur QGPU_LEN_SURF_XFER que DEPTH_*. Sur le fil, UN MOT DE 32 BITS
 * BIG-ENDIAN PAR PIXEL, de valeur 0..255 (les 24 bits de poids fort sont nuls
 * en relecture, ignorés en envoi). Un octet par pixel aurait été plus compact,
 * mais ce choix garde le pas (stride) et les offsets multiples de 4 — donc les
 * MÊMES contraintes que tous les autres transferts, et aucun cas particulier
 * ni côté invité PowerPC ni dans la validation du cœur. Les deux commandes
 * exigent QGPU_FMT_FLAG_STENCIL sur la surface (sinon QGPU_ST_BAD_ARG). */

/* ── v7 : la géométrie sur l'hôte ────────────────────────────────────────────
 *
 *   Jusqu'à la v6, l'invité envoyait des sommets DÉJÀ transformés, éclairés,
 *   découpés et aplatis par GLEngine sur le PowerPC émulé. C'est ce travail-là
 *   qui limite les jeux. La v7 ajoute de quoi confier TOUT le pipeline fixe
 *   d'OpenGL 1.x à l'hôte : matrices, viewport, éclairage, matériaux,
 *   génération de coordonnées, plans de découpe, et un dessin indexé de
 *   sommets BRUTS (coordonnées objet, normales, couleurs, coordonnées de
 *   texture non divisées).
 *
 *   Règle de compatibilité, comme à chaque version : rien n'est retiré, aucune
 *   longueur de commande existante ne change. Les opcodes de dessin v1–v6
 *   ignorent tout l'état ci-dessous et gardent exactement leur sémantique.
 *
 * REPÈRES ET SENS DE L'IMAGE (le point à ne pas se tromper)
 *
 *   Le chemin existant travaille en PIXELS DE SURFACE : origine EN HAUT à
 *   gauche, y vers le BAS. Le chemin brut travaille en coordonnées OpenGL :
 *   l'invité recopie ses matrices et son viewport tels que GL les lui donne,
 *   donc en repère GL (origine du viewport EN BAS à gauche, y vers le HAUT).
 *   L'hôte fait la couture : une coordonnée fenêtre GL yw devient la ligne de
 *   surface (hauteur de la surface − yw). Autrement dit, LE VIEWPORT EST
 *   RAPPORTÉ AU BAS DE LA SURFACE, et les deux chemins produisent une image
 *   dans le même sens — l'invité relit la surface de la même façon dans les
 *   deux cas.
 *
 *   Conséquence pratique : pour dessiner en pixels par le chemin brut, il
 *   suffit de poser modèle-vue = identité et projection = glOrtho(0, w, h, 0,
 *   0, −1), c'est-à-dire exactement la projection que le plugin utilise déjà.
 *   Un sommet (x, y, z) y donne le pixel (x, y) et la profondeur fenêtre z.
 *
 *   Le sens des faces suit OpenGL : il est établi sur les coordonnées fenêtre
 *   GL (y vers le haut), AVANT le retournement, pour que GL_CCW veuille dire
 *   la même chose côté invité et côté hôte.
 *
 * VIEWPORT ET PROFONDEUR
 *
 *   QGPU_OP_VIEWPORT [x, y, w, h] — jusqu'ici « réservé, sans effet » — porte
 *   maintenant le viewport GL du chemin brut : x et y sont comptés depuis le
 *   coin BAS-GAUCHE de la surface, comme glViewport. Sans VIEWPORT, le chemin
 *   brut prend toute la surface. Le chemin existant, lui, continue d'ignorer
 *   ce rectangle : il est déjà en pixels de surface.
 *   QGPU_OP_DEPTH_RANGE [proche, lointain] : comme glDepthRange, deux
 *   flottants bornés à [0,1], initialement 0 et 1.
 *
 * MATRICES
 *
 *   QGPU_OP_SET_MATRIX [quelle, m0..m15] : 16 flottants big-endian dans
 *   L'ORDRE COLONNE d'OpenGL (m0..m3 = première colonne), c'est-à-dire la
 *   disposition que glLoadMatrixf attend et que GLEngine range en mémoire :
 *   l'invité recopie, il ne transpose pas.
 */
#define QGPU_MTX_MODELVIEW      0
#define QGPU_MTX_PROJECTION     1
#define QGPU_MTX_TEXTURE0       2   /* unités 0..3 : QGPU_MTX_TEXTURE0 + u */
#define QGPU_MTX_COUNT          (QGPU_MTX_TEXTURE0 + QGPU_MAX_UNITS)

/* LUMIÈRES — QGPU_OP_SET_LIGHT
 *
 *   [i (0..7), actif, ambiante4, diffuse4, spéculaire4, position4,
 *    direction de spot3, exposant, angle de coupure, atténuation constante,
 *    linéaire, quadratique]
 *
 *   La position ET la direction de spot sont EN COORDONNÉES ŒIL : OpenGL les
 *   transforme par la modèle-vue au moment de glLight, et c'est la valeur
 *   transformée que GLEngine garde. L'invité envoie donc ce qu'il lit dans
 *   l'état GL, sans rien retransformer. position[3] = 0 : lumière
 *   directionnelle (pas d'atténuation, pas de spot). Angle de coupure : 0..90,
 *   ou exactement 180 (pas de spot). Exposant : 0..128.
 *
 * MATÉRIAU — QGPU_OP_SET_MATERIAL
 *
 *   [face, ambiante4, diffuse4, spéculaire4, émission4, brillance]
 *   face : GL_FRONT, GL_BACK ou GL_FRONT_AND_BACK. Brillance : 0..128.
 *   L'ambiante du MODÈLE d'éclairage est à part (QGPU_OP_SET_LIGHT_MODEL,
 *   [ambiante4]) ; observateur local, deux faces et spéculaire séparée sont
 *   des clés d'état (QGPU_SK_LOCAL_VIEWER, _TWO_SIDE, _COLOR_CONTROL).
 *
 * TEXGEN — QGPU_OP_SET_TEXGEN
 *
 *   [unité (0..3), coordonnée (0=S, 1=T, 2=R, 3=Q), actif, mode,
 *    plan objet4, plan œil4]
 *   Le plan œil est DÉJÀ en coordonnées œil, pour la même raison que la
 *   position des lumières. Le plan objet, lui, est pris tel quel.
 */
#define QGPU_TG_S               0
#define QGPU_TG_T               1
#define QGPU_TG_R               2
#define QGPU_TG_Q               3
#define QGPU_TG_OBJECT_LINEAR   0x2401
#define QGPU_TG_EYE_LINEAR      0x2400
#define QGPU_TG_SPHERE_MAP      0x2402
#define QGPU_TG_NORMAL_MAP      0x8511
#define QGPU_TG_REFLECTION_MAP  0x8512

/* PLANS DE DÉCOUPE — QGPU_OP_SET_CLIP_PLANE [i (0..5), actif, équation4]
 *   L'équation est en coordonnées œil (même raison). Un point est gardé si
 *   (a,b,c,d)·(x,y,z,w) >= 0, comme en OpenGL.
 *
 * VALEURS COURANTES — QGPU_OP_SET_CURRENT [quoi, x, y, z, w]
 *   L'équivalent de glColor / glNormal / glTexCoord hors tableau : un attribut
 *   ABSENT du format de sommet prend cette valeur pour tous les sommets du
 *   dessin. Les composantes inutilisées par `quoi` sont ignorées (mais doivent
 *   être des flottants valides : la commande a toujours la même longueur). */
#define QGPU_CUR_NORMAL         0   /* x, y, z ; initial (0, 0, 1) */
#define QGPU_CUR_COLOR          1   /* r, g, b, a ; initial (1, 1, 1, 1) */
#define QGPU_CUR_SEC_COLOR      2   /* r, g, b ; initial (0, 0, 0) */
#define QGPU_CUR_FOG            3   /* x ; initial 1.0 (pas de brouillard) */
#define QGPU_CUR_TEXCOORD0      4   /* s, t, r, q ; unités 0..3 : +u ; initial (0,0,0,1) */
#define QGPU_CUR_COUNT          (QGPU_CUR_TEXCOORD0 + QGPU_MAX_UNITS)

/* DESSIN BRUT — QGPU_OP_DRAW_RAW
 *
 *   [mode, n, voff, pas, format, ioff, itype, premier, nverts]
 *
 *   mode     : un des dix modes d'OpenGL (QGPU_PRIM_MODE_* ci-dessous, valeurs
 *              d'OpenGL recopiées telles quelles).
 *   n        : nombre de sommets dessinés (itype = AUCUN) ou d'indices lus.
 *   voff     : offset des sommets dans BAR0 (multiple de 4).
 *   pas      : pas d'un sommet EN MOTS ; 0 = serré (= QGPU_VF_WORDS(format)).
 *              Un pas inférieur au format est refusé.
 *   format   : masque QGPU_VF_* ; dit quels attributs sont présents.
 *   ioff     : offset des indices dans BAR0 (ignoré si itype = AUCUN).
 *   itype    : QGPU_IDX_NONE, QGPU_IDX_U16 ou QGPU_IDX_U32, big-endian.
 *   premier  : premier sommet dessiné (comme glDrawArrays) ; doit valoir 0
 *              quand des indices sont donnés.
 *   nverts   : NOMBRE DE SOMMETS PRÉSENTS dans le tableau à partir de voff.
 *              C'est contre lui que les indices sont validés : un indice >=
 *              nverts vaut QGPU_ST_BAD_ARG, et l'hôte ne lit jamais hors de
 *              BAR0. Il est explicite plutôt que déduit parce qu'un tableau
 *              indexé ne dit pas sa propre taille.
 *
 *   FORMAT DE SOMMET. Des flottants big-endian, dans un ordre FIXE, chaque
 *   attribut n'occupant de la place que s'il est présent :
 *      position (2, 3 ou 4 composantes — champ de 2 bits, toujours présente),
 *      normale (3), couleur (4), couleur secondaire (3), brouillard (1),
 *      puis coordonnées de texture des unités 0, 1, 2, 3 (4 chacune).
 *   Un attribut absent prend la valeur courante (QGPU_OP_SET_CURRENT).
 *   Une position à 2 composantes complète z = 0 et w = 1 ; à 3, w = 1.
 *   Comme pour les autres dessins, un NaN ou un infini dans les sommets vaut
 *   QGPU_ST_BAD_ARG. */
#define QGPU_PRIM_MODE_POINTS         0x0000
#define QGPU_PRIM_MODE_LINES          0x0001
#define QGPU_PRIM_MODE_LINE_LOOP      0x0002
#define QGPU_PRIM_MODE_LINE_STRIP     0x0003
#define QGPU_PRIM_MODE_TRIANGLES      0x0004
#define QGPU_PRIM_MODE_TRIANGLE_STRIP 0x0005
#define QGPU_PRIM_MODE_TRIANGLE_FAN   0x0006
#define QGPU_PRIM_MODE_QUADS          0x0007
#define QGPU_PRIM_MODE_QUAD_STRIP     0x0008
#define QGPU_PRIM_MODE_POLYGON        0x0009

#define QGPU_IDX_NONE           0
#define QGPU_IDX_U16            1
#define QGPU_IDX_U32            2

/* Masque de format. Les deux bits de poids faible portent le nombre de
   composantes de position moins deux (0 → 2, 1 → 3, 2 → 4) ; 3 est invalide. */
#define QGPU_VF_POS(n)          ((unsigned long)((n) - 2))
#define QGPU_VF_POS_MASK        0x0003
#define QGPU_VF_POS_COUNT(m)    ((int)((m) & QGPU_VF_POS_MASK) + 2)
#define QGPU_VF_NORMAL          0x0004
#define QGPU_VF_COLOR           0x0008
#define QGPU_VF_SEC_COLOR       0x0010
#define QGPU_VF_FOG             0x0020
#define QGPU_VF_TEX0            0x0040      /* unités 0..3 : QGPU_VF_TEX(u) */
#define QGPU_VF_TEX(u)          (QGPU_VF_TEX0 << (u))
#define QGPU_VF_ALL             0x03FF      /* tout bit hors de là = QGPU_ST_BAD_ARG */

/* Taille d'un sommet, en mots, dans l'ordre fixe ci-dessus. */
#define QGPU_VF_WORDS(m) \
    (QGPU_VF_POS_COUNT(m) + \
     (((m) & QGPU_VF_NORMAL)    ? 3 : 0) + \
     (((m) & QGPU_VF_COLOR)     ? 4 : 0) + \
     (((m) & QGPU_VF_SEC_COLOR) ? 3 : 0) + \
     (((m) & QGPU_VF_FOG)       ? 1 : 0) + \
     (((m) & QGPU_VF_TEX(0))    ? 4 : 0) + \
     (((m) & QGPU_VF_TEX(1))    ? 4 : 0) + \
     (((m) & QGPU_VF_TEX(2))    ? 4 : 0) + \
     (((m) & QGPU_VF_TEX(3))    ? 4 : 0))
/* Sommet le plus gros : 4 + 3 + 4 + 3 + 1 + 4×4. Écrit en clair, parce que
   QGPU_VF_WORDS(QGPU_VF_ALL) lirait un champ de position invalide (3). */
#define QGPU_VF_MAX_WORDS       31

/* ── v8 : ce qui manquait au pipeline fixe ───────────────────────────────────
 *
 *   Rien n'est retiré, aucune longueur de commande existante ne change : la v8
 *   n'ajoute que des clés d'état et quatre opcodes. Les valeurs initiales sont
 *   celles d'OpenGL, et toutes neutres : un flux v1–v7 rend exactement pareil.
 *
 * MÉLANGE À COULEUR CONSTANTE (GL 1.4 / ARB_imaging)
 *
 *   Les quatre clés de facteurs existantes (QGPU_SK_BLEND_SRC_RGB et sœurs)
 *   acceptent en plus GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
 *   GL_CONSTANT_ALPHA et GL_ONE_MINUS_CONSTANT_ALPHA ; la constante est
 *   QGPU_SK_BLEND_COLOR, un 0xAARRGGBB comme les autres couleurs du fil.
 *
 * ÉQUATIONS MINIMUM ET MAXIMUM (GL 1.4 / EXT_blend_minmax)
 *
 *   Les deux clés d'équation acceptent GL_MIN et GL_MAX. Rappel de la
 *   spécification : avec MIN et MAX, LES FACTEURS SONT IGNORÉS — la sortie est
 *   min(source, destination) ou max(source, destination), canal par canal.
 */
#define QGPU_BF_CONSTANT_COLOR      0x8001
#define QGPU_BF_ONE_MINUS_CONSTANT_COLOR 0x8002
#define QGPU_BF_CONSTANT_ALPHA      0x8003
#define QGPU_BF_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define QGPU_BEQ_ADD                0x8006  /* GL_FUNC_ADD (v2) */
#define QGPU_BEQ_MIN                0x8007  /* v8 */
#define QGPU_BEQ_MAX                0x8008  /* v8 */
#define QGPU_BEQ_SUBTRACT           0x800A
#define QGPU_BEQ_REVERSE_SUBTRACT   0x800B

/* OPÉRATIONS LOGIQUES (les 16 d'OpenGL, valeurs recopiées telles quelles).
 *   s = fragment, d = pixel de la surface, sur 8 bits par canal. */
#define QGPU_LO_CLEAR           0x1500  /* 0 */
#define QGPU_LO_AND             0x1501  /* s & d */
#define QGPU_LO_AND_REVERSE     0x1502  /* s & ~d */
#define QGPU_LO_COPY            0x1503  /* s (initial) */
#define QGPU_LO_AND_INVERTED    0x1504  /* ~s & d */
#define QGPU_LO_NOOP            0x1505  /* d */
#define QGPU_LO_XOR             0x1506  /* s ^ d */
#define QGPU_LO_OR              0x1507  /* s | d */
#define QGPU_LO_NOR             0x1508  /* ~(s | d) */
#define QGPU_LO_EQUIV           0x1509  /* ~(s ^ d) */
#define QGPU_LO_INVERT          0x150A  /* ~d */
#define QGPU_LO_OR_REVERSE      0x150B  /* s | ~d */
#define QGPU_LO_COPY_INVERTED   0x150C  /* ~s */
#define QGPU_LO_OR_INVERTED     0x150D  /* ~s | d */
#define QGPU_LO_NAND            0x150E  /* ~(s & d) */
#define QGPU_LO_SET             0x150F  /* tous les bits à 1 */

/* MODES DE POLYGONE (valeurs d'OpenGL). */
#define QGPU_POLY_POINT         0x1B00
#define QGPU_POLY_LINE          0x1B01
#define QGPU_POLY_FILL          0x1B02  /* initial */

/* POINTILLÉ DE POLYGONE — QGPU_OP_SET_POLYGON_STIPPLE [m0..m31]
 *
 *   32 mots de 32 bits big-endian, un par ligne du motif 32×32. Deux
 *   conventions, à ne pas confondre :
 *
 *   - EN X, le bit de POIDS FORT du mot (bit 31) est la colonne x = 0 du motif,
 *     comme le premier octet d'un octet de glPolygonStipple (MSB à gauche).
 *     Le motif se répète tous les 32 pixels : colonne = x mod 32.
 *
 *   - EN Y, le mot 0 est la ligne yw = 0 de la COORDONNÉE FENÊTRE OpenGL,
 *     c'est-à-dire le BAS de l'image — la convention de glPolygonStipple. Or le
 *     protocole range les surfaces avec la ligne 0 EN HAUT (QuickDraw). L'hôte
 *     fait donc la même couture qu'en v7 pour la géométrie : la ligne de
 *     surface ys emploie le mot (hauteur_de_la_surface − ys) mod 32. Les deux
 *     chemins de dessin, brut et hérité, suivent cette même règle : un motif
 *     donné produit la même image, quel que soit l'opcode employé.
 *
 *   Le pointillé de polygone ne s'applique qu'aux polygones REMPLIS : en mode
 *   GL_LINE ou GL_POINT, c'est le pointillé de ligne (ou rien) qui vaut, comme
 *   en OpenGL.
 *
 * REQUÊTES D'OCCLUSION (OpenGL 1.5 / ARB_occlusion_query)
 *
 *   QGPU_OP_QUERY_BEGIN [id] ouvre la requête `id` (0..QGPU_MAX_QUERIES-1) sur
 *   le contexte courant et remet son compte à zéro ; QGPU_OP_QUERY_END [id] la
 *   ferme. UNE SEULE requête active à la fois par contexte : ouvrir une requête
 *   alors qu'une autre court, ou fermer une requête qui ne court pas, vaut
 *   QGPU_ST_BAD_ARG — c'est la règle d'OpenGL, dite ici plutôt que laissée au
 *   backend.
 *
 *   QGPU_OP_QUERY_RESULT [id, off] écrit à `off` dans BAR0 DEUX mots de 32 bits
 *   big-endian : « disponible » (0 ou 1) puis le nombre d'échantillons passés,
 *   saturé à 2^32−1. Le device est synchrone : le résultat est toujours
 *   disponible après un QUERY_END, et il peut être relu autant de fois qu'on
 *   veut. `off` doit être multiple de 4 et laisser 8 octets dans la fenêtre,
 *   sinon QGPU_ST_OOB. Une requête jamais ouverte vaut QGPU_ST_BAD_ARG.
 *
 *   Est compté tout fragment qui passe TOUS les tests — ciseaux, alpha,
 *   stencil, profondeur —, que le masque de couleur soit ouvert ou fermé (c'est
 *   précisément l'usage : dessiner une boîte englobante sans rien peindre).
 */

/* ── v9 : le doorbell asynchrone ─────────────────────────────────────────────
 *
 *   Jusqu'à la v8, l'hôte exécutait le flux DANS l'écriture MMIO du doorbell :
 *   le vCPU restait gelé pendant tout le rendu. Le profil de Marble Blast le
 *   disait sans détour — 14 % du temps invité passé à attendre le device. La
 *   v9 ajoute une FILE DE SOUMISSIONS et un THREAD DE RENDU côté hôte : le
 *   vCPU dépose, l'hôte dessine pendant que l'invité continue.
 *
 *   RIEN N'EST RETIRÉ. Un invité v1–v8 qui écrit 1 dans QGPU_REG_DOORBELL puis
 *   lit QGPU_REG_STATUS / QGPU_REG_FENCE juste après voit exactement ce qu'il
 *   voyait : l'écriture ne rend la main qu'une fois la soumission terminée.
 *   Le mode synchrone reste le DÉFAUT ; l'asynchrone se demande par soumission.
 *
 * LES DEUX MODES
 *
 *   QGPU_DOORBELL_GO (1)                 : exécution SYNCHRONE. Au retour du
 *     `stw`, FENCE, STATUS, STATUS_PC et toutes les relectures de la
 *     soumission sont à jour. Si des soumissions asynchrones sont encore en
 *     file, celle-ci passe APRÈS elles (une seule file, l'ordre est l'ordre de
 *     soumission), donc l'écriture attend aussi leur fin. Si la file est
 *     pleine, elle ATTEND une place — un doorbell synchrone n'est jamais
 *     refusé, c'est ce qui garde les invités v1–v8 exacts.
 *
 *   QGPU_DOORBELL_GO|QGPU_DOORBELL_ASYNC (3) : la soumission (SUBMIT_OFF,
 *     SUBMIT_LEN, lus au moment du doorbell) est mise en FILE et l'écriture
 *     rend la main tout de suite. Si la file est pleine, RIEN n'est mis en
 *     file : QGPU_REG_SUBMIT_ST vaut QGPU_ST_QUEUE_FULL et l'invité réessaie
 *     (ou se replie sur le mode synchrone). Toute autre valeur portant le bit 0
 *     sans le bit 1 est un doorbell synchrone ; une valeur sans le bit 0 ne
 *     fait rien, comme en v1.
 *
 * LES COMPTEURS
 *
 *   QGPU_REG_FENCE_SUBMITTED  soumissions ACCEPTÉES (mises en file).
 *   QGPU_REG_FENCE            soumissions TERMINÉES. Toujours ≤ SUBMITTED.
 *   QGPU_REG_DOORBELL (lu)    SUBMITTED − FENCE : en attente ou en cours.
 *                             0 = l'hôte n'a plus rien à faire.
 *   QGPU_REG_QUEUE_FREE       QGPU_REG_QUEUE_DEPTH − QGPU_REG_DOORBELL.
 *   QGPU_REG_ERRORS           soumissions terminées avec un statut ≠ OK.
 *   QGPU_REG_STATUS/_PC       statut de la DERNIÈRE TERMINÉE (inchangé).
 *
 *   ORDRE DE LECTURE. L'hôte publie FENCE EN DERNIER, après le statut et après
 *   les relectures : FENCE >= n GARANTIT que tout ce qu'a produit la soumission
 *   n° n est visible. L'invité lit donc FENCE D'ABORD, STATUS/STATUS_PC
 *   ensuite — dans l'autre sens il pourrait attribuer à la soumission n° n un
 *   statut plus ancien qu'elle. (Avec plusieurs soumissions en vol, le statut
 *   ainsi lu peut au contraire venir d'une soumission PLUS RÉCENTE : c'est
 *   inévitable, et c'est pourquoi QGPU_REG_ERRORS existe, cf. plus bas.)
 *
 *   ATTENDRE UNE SOUMISSION : noter f = QGPU_REG_FENCE_SUBMITTED juste après
 *   avoir frappé le doorbell (c'est le numéro de barrière de CETTE
 *   soumission), puis attendre QGPU_REG_FENCE ≥ f. Les compteurs sont des
 *   entiers de 32 bits qui bouclent : comparer par (SInt32)(fence − f) >= 0.
 *
 *   ERREURS. Chaque soumission est indépendante, comme en v8 : une soumission
 *   qui échoue n'empêche pas les suivantes de la file de s'exécuter. Elle
 *   avance FENCE comme les autres, pose son statut dans QGPU_REG_STATUS et
 *   incrémente QGPU_REG_ERRORS. Avec plusieurs soumissions en vol, STATUS ne
 *   suffit donc plus à conclure « tout s'est bien passé » : c'est QGPU_REG_ERRORS
 *   qui fait foi — le lire avant la rafale et après la barrière dit s'il y a eu
 *   une erreur, et STATUS/STATUS_PC disent laquelle pour la dernière.
 *
 *   INTERRUPTION. QGPU_IRQ_DONE est levée à CHAQUE soumission terminée, comme
 *   en v8, et se démasque de la même façon (QGPU_REG_IRQ_MASK). Elle est de
 *   niveau et se coalesce : une seule interruption peut couvrir plusieurs
 *   soumissions terminées. Le gestionnaire acquitte puis relit FENCE — il ne
 *   compte pas les interruptions.
 *
 * CONTRAT MÉMOIRE (le point à ne pas se tromper)
 *
 *   Tant que QGPU_REG_FENCE n'a pas dépassé une soumission, elle est « en
 *   vol » et l'hôte peut lire ou écrire à tout moment les zones de BAR0
 *   qu'elle désigne. L'invité doit donc, pour une soumission de barrière n,
 *   ET JUSQU'À CE QUE FENCE ≥ n :
 *
 *   1. NE PAS MODIFIER le flux de commandes [SUBMIT_OFF, SUBMIT_OFF+SUBMIT_LEN)
 *      ni aucune zone qu'il désigne : sommets et indices de DRAW_* / DRAW_RAW,
 *      texels de TEX_IMAGE, pixels de SURF_UPLOAD / DEPTH_UPLOAD /
 *      STENCIL_UPLOAD. Concrètement, l'invité qui veut vraiment recouvrir le
 *      rendu et la préparation double (ou triple) ces tampons.
 *   2. NE PAS LIRE les zones de destination : les relectures (SURF_READBACK,
 *      DEPTH_READBACK, STENCIL_READBACK, QUERY_RESULT) n'apparaissent dans
 *      BAR0 qu'à l'avancement de FENCE. Avant, leur contenu est indéterminé —
 *      ancien, partiel ou en cours d'écriture.
 *   3. NE PAS RÉUTILISER SUBMIT_OFF / SUBMIT_LEN comme mémoire : le device les
 *      a lus au doorbell et n'y revient pas. Les réécrire pour la soumission
 *      suivante est donc sans danger, même file pleine.
 *
 *   OBJETS ET ORDRE. Contextes, surfaces, textures et requêtes sont partagés
 *   par toutes les soumissions ; il n'y a qu'UNE file et UN thread de rendu, et
 *   l'exécution suit l'ordre de soumission — sans réordonnancement, y compris
 *   entre clients différents du kext. Une soumission voit donc l'état laissé
 *   par celle qui la précède, exactement comme en v8. Le device reste
 *   mono-contexte courant : chaque soumission commence par un CTX_BIND.
 *
 *   RESET. Écrire QGPU_REG_MAGIC vide la file (les soumissions en attente sont
 *   JETÉES, elles n'avanceront jamais FENCE) et attend la fin de celle qui est
 *   en cours, puis remet tous les compteurs à zéro. Une soumission jetée ne
 *   touche plus BAR0 après le retour de l'écriture : c'est ce qui rend le reset
 *   sûr avant de rendre la mémoire d'un client.
 *
 *   HORS PÉRIMÈTRE. La migration et le retrait à chaud du device ne sont pas
 *   tenus en v9 (ils ne l'étaient pas davantage avant : les objets hôte ne
 *   migrent pas). Le device draine sa file avant de sauver son état, et
 *   l'invité repart d'un device vide au chargement.
 */

/* ── v10 : les textures d'OpenGL 1.2 à 1.5 ───────────────────────────────────
 *
 *   Jusqu'à la v9, une texture était 2D, ses texels arrivaient en ARGB déjà
 *   convertis par l'invité, et seuls trois modes de répétition existaient. Ce
 *   qui manquait bloquait l'annonce d'OpenGL 1.2 (textures 3D), 1.3 (cartes de
 *   cube, compression, CLAMP_TO_BORDER) et 1.4 (textures de profondeur et
 *   comparaison, MIRRORED_REPEAT, biais de LOD, mipmaps automatiques).
 *
 *   RIEN N'EST RETIRÉ, aucune longueur de commande existante ne change.
 *   TEX_CREATE crée une texture 2D, TEX_IMAGE envoie des texels ARGB, comme
 *   avant ; les valeurs initiales des nouveaux paramètres sont celles
 *   d'OpenGL et elles sont neutres.
 *
 * CIBLES — QGPU_OP_TEX_CREATE3 [tex, cible]
 *
 *   La cible d'une texture est fixée À SA CRÉATION, comme par le premier
 *   glBindTexture : c'est elle qui dit quels paramètres ont un sens et quelles
 *   images la texture accepte. TEX_CREATE [tex] vaut TEX_CREATE3 [tex, 2D].
 *   Valeurs d'OpenGL, recopiées telles quelles : */
#define QGPU_TT_1D              0x0DE0  /* images de hauteur 1 ; t est ignoré */
#define QGPU_TT_2D              0x0DE1
#define QGPU_TT_3D              0x806F
#define QGPU_TT_CUBE_MAP        0x8513  /* images : une par face, cf. ci-dessous */
#define QGPU_TT_RECTANGLE       0x84F5  /* coordonnées en texels, sans mipmap */
#define QGPU_TT_CUBE_FACE(f)    (0x8515 + (f))  /* f : 0 +X, 1 −X, 2 +Y, 3 −Y, 4 +Z, 5 −Z */
/*
 *   Valeurs initiales propres à la cible, comme en OpenGL : une texture
 *   RECTANGLE part en GL_LINEAR / GL_CLAMP_TO_EDGE, toutes les autres en
 *   GL_NEAREST_MIPMAP_LINEAR / GL_REPEAT. Une texture RECTANGLE refuse
 *   (QGPU_ST_BAD_ARG) les filtres avec mipmaps, REPEAT, MIRRORED_REPEAT, un
 *   niveau de base autre que 0, et MIN_LOD / MAX_LOD (sans objet sans mipmaps,
 *   et refusés par des pilotes hôtes).
 *
 * IMAGES — QGPU_OP_TEX_IMAGE3
 *
 *   [tex, cible d'image, niveau, w, h, d, format de base, format, type, off,
 *    octets par ligne, octets par tranche]
 *
 *   Définit (ou redéfinit) un niveau, comme glTexImage1D/2D/3D.
 *   cible d'image : la cible de la texture, sauf pour une carte de cube où
 *                   c'est la FACE (QGPU_TT_CUBE_FACE(f)). Toute autre valeur
 *                   vaut QGPU_ST_BAD_ARG.
 *   w, h, d       : 1D : h = d = 1 ; 2D, cube, rectangle : d = 1 ; face de
 *                   cube : w = h. Bornes : QGPU_MAX_TEX_DIM (1D, 2D, cube,
 *                   rectangle), QGPU_MAX_TEX_3D_DIM (3D). Tailles quelconques,
 *                   pas seulement des puissances de 2.
 *   format de base: GL_ALPHA, GL_RGB, GL_RGBA, GL_LUMINANCE,
 *                   GL_LUMINANCE_ALPHA, GL_INTENSITY comme en v3, et
 *                   GL_DEPTH_COMPONENT (0x1902 : 1D, 2D et rectangle
 *                   seulement, et seulement avec un format de profondeur).
 *   format, type  : les DONNÉES, telles que l'application les a données à
 *                   glTexImage — l'hôte fait la conversion que l'invité faisait
 *                   jusqu'ici sur le PowerPC émulé. Couples acceptés :
 *
 *     GL_UNSIGNED_BYTE (0x1401) avec GL_RGBA, GL_RGB, GL_BGRA, GL_BGR,
 *         GL_LUMINANCE, GL_LUMINANCE_ALPHA, GL_ALPHA, GL_RED ;
 *     GL_UNSIGNED_INT_8_8_8_8 (0x8035) et _REV (0x8367) avec GL_RGBA, GL_BGRA ;
 *     GL_UNSIGNED_SHORT_5_6_5 (0x8363) et _REV (0x8364) avec GL_RGB ;
 *     GL_UNSIGNED_SHORT_4_4_4_4 (0x8033) avec GL_RGBA, _REV (0x8365) avec GL_BGRA ;
 *     GL_UNSIGNED_SHORT_5_5_5_1 (0x8034) avec GL_RGBA, _1_5_5_5_REV (0x8366)
 *         avec GL_BGRA ;
 *     GL_DEPTH_COMPONENT (0x1902) avec GL_FLOAT (0x1406, borné à [0,1]),
 *         GL_UNSIGNED_INT (0x1405), GL_UNSIGNED_SHORT (0x1403) ;
 *     les formats COMPRESSÉS S3TC, avec type = 0 : QGPU_TF_DXT1_RGB,
 *         QGPU_TF_DXT1_RGBA, QGPU_TF_DXT3, QGPU_TF_DXT5 (2D et faces de cube
 *         seulement ; le format de base doit être GL_RGB pour le premier,
 *         GL_RGBA pour les trois autres).
 *
 *   Les types compactés (USHORT, UINT, FLOAT) sont des mots BIG-ENDIAN, comme
 *   tout le fil ; les octets de GL_UNSIGNED_BYTE sont dans l'ordre de la
 *   mémoire. GL_BGRA + GL_UNSIGNED_INT_8_8_8_8_REV est exactement le mot ARGB
 *   de TEX_IMAGE : c'est ainsi que TEX_IMAGE se réécrit en v10.
 *
 *   off           : offset des données dans BAR0, multiple de 4 ; ou
 *                   QGPU_TEX_NO_DATA : le niveau est défini, à zéro (comme
 *                   glTexImage avec un pointeur nul — une sous-image le
 *                   remplira ensuite).
 *   octets par ligne / par tranche : pas des données, pour les données
 *                   alignées (GL_UNPACK_ALIGNMENT, _ROW_LENGTH, _IMAGE_HEIGHT
 *                   déjà appliqués par l'invité) ; 0 = serré. Un pas plus petit
 *                   qu'une ligne (ou qu'une tranche) vaut QGPU_ST_BAD_ARG.
 *                   Ignorés pour un format compressé : les blocs de 4×4
 *                   sont serrés, ceil(w/4)·ceil(h/4) blocs de 8 (DXT1) ou 16
 *                   octets.
 *
 *   Un niveau garde son format de base : la texture est complète quand les
 *   niveaux requis ont tous le format de base du niveau de base (règle
 *   d'OpenGL). Pour une carte de cube, les six faces doivent en plus être
 *   carrées, de même taille et de même format à chaque niveau.
 *
 * SOUS-IMAGES — QGPU_OP_TEX_SUBIMAGE
 *
 *   [tex, cible d'image, niveau, x, y, z, w, h, d, format, type, off,
 *    octets par ligne, octets par tranche]
 *
 *   Remplace la boîte [x, x+w) × [y, y+h) × [z, z+d) d'un niveau DÉJÀ défini,
 *   comme glTexSubImage. La boîte doit tenir dans le niveau. Le format de
 *   profondeur n'est accepté que sur un niveau de profondeur, et inversement.
 *   Format compressé : x et y multiples de 4, w et h multiples de 4 ou
 *   atteignant le bord du niveau.
 *
 * PARAMÈTRES (QGPU_OP_TEX_PARAM, clés QGPU_TP_* ci-dessus)
 *
 *   Répétition : GL_MIRRORED_REPEAT (0x8370) et GL_CLAMP_TO_BORDER (0x812D)
 *   s'ajoutent. GL_CLAMP et GL_CLAMP_TO_BORDER prennent la COULEUR DE BORDURE
 *   (QGPU_TP_BORDER_COLOR) — c'était le noir transparent jusqu'ici, qui est
 *   aussi la valeur initiale : rien ne change pour un flux v9.
 *
 *   LOD (OpenGL 1.2 et 1.4). Avec λb = log2 ρ :
 *     λ' = λb + borné(biais de texture + biais d'unité, ±QGPU_MAX_LOD_BIAS)
 *     λ  = λ' borné à [MIN_LOD, MAX_LOD]
 *   Les niveaux employés vont du niveau de base b à q = min(b + p, MAX_LEVEL),
 *   p = log2 de la plus grande dimension du niveau b ; ce sont eux que la
 *   complétude exige quand le filtre de réduction emploie les mipmaps.
 *
 *   Profondeur (OpenGL 1.4, ARB_depth_texture et ARB_shadow). Une texture de
 *   format de base GL_DEPTH_COMPONENT rend sa valeur D, ou, en mode
 *   GL_COMPARE_R_TO_TEXTURE, le résultat 0 ou 1 de « r fonc D » (r = r/q,
 *   borné à [0,1]) ; avec un filtre linéaire, chacun des texels est comparé
 *   PUIS pondéré. Le résultat devient une luminance, une intensité ou un alpha
 *   selon QGPU_TP_DEPTH_MODE, et l'environnement de texture le voit comme une
 *   texture de ce format de base. La bordure d'une texture de profondeur vaut
 *   la composante rouge de QGPU_TP_BORDER_COLOR.
 *
 *   Mipmaps automatiques (OpenGL 1.4). Avec QGPU_TP_GENERATE_MIPMAP, toute
 *   image ou sous-image du NIVEAU DE BASE recalcule les niveaux b+1..q (bornés
 *   à QGPU_MAX_TEX_LEVELS−1), par moyenne des blocs de 2×2 (2×2×2 en 3D) du
 *   niveau précédent, sur l'HÔTE — donc identiques pour tous les backends.
 */
#define QGPU_TEX_NO_DATA        0xFFFFFFFFUL
#define QGPU_TF_DXT1_RGB        0x83F0  /* GL_COMPRESSED_RGB_S3TC_DXT1_EXT */
#define QGPU_TF_DXT1_RGBA       0x83F1  /* GL_COMPRESSED_RGBA_S3TC_DXT1_EXT */
#define QGPU_TF_DXT3            0x83F2  /* GL_COMPRESSED_RGBA_S3TC_DXT3_EXT */
#define QGPU_TF_DXT5            0x83F3  /* GL_COMPRESSED_RGBA_S3TC_DXT5_EXT */
#define QGPU_TW_MIRRORED_REPEAT 0x8370
#define QGPU_TW_CLAMP_TO_BORDER 0x812D
#define QGPU_TC_NONE            0x0000
#define QGPU_TC_COMPARE_R       0x884E  /* GL_COMPARE_R_TO_TEXTURE */

/* COULEUR SECONDAIRE — QGPU_SK_COLOR_SUM (OpenGL 1.4)
 *
 *   Jusqu'à la v9, DRAW_RAW ajoutait la couleur secondaire dès que le format
 *   de sommet en portait une (QGPU_VF_SEC_COLOR) : l'invité ne pouvait donc pas
 *   l'envoyer sans allumer GL_COLOR_SUM. La clé le dit maintenant :
 *     QGPU_CSUM_OFF    : coupée, comme glDisable(GL_COLOR_SUM) ;
 *     QGPU_CSUM_ON     : allumée — la couleur secondaire du sommet, ou à défaut
 *                        la valeur courante (QGPU_CUR_SEC_COLOR), s'ajoute ;
 *     QGPU_CSUM_FORMAT : comportement v7–v9, la valeur INITIALE : allumée si et
 *                        seulement si le format porte QGPU_VF_SEC_COLOR.
 *   Éclairage allumé : c'est l'éclairage qui fournit la couleur secondaire (la
 *   spéculaire en GL_SEPARATE_SPECULAR_COLOR, zéro sinon), et elle s'ajoute
 *   toujours, quelle que soit la clé — règle d'OpenGL.
 *
 * PARAMÈTRES DE POINT — QGPU_SK_POINT_* (OpenGL 1.4, ARB_point_parameters)
 *
 *   Taille dérivée d'un point de DRAW_RAW, d étant la distance à l'œil en
 *   coordonnées œil (sqrt(x² + y² + z²)) :
 *     taille · sqrt(1 / (a + b·d + c·d²)), bornée à [MIN, MAX]
 *   (un dénominateur nul ou négatif donne MAX). Elle vaut pour GL_POINTS comme
 *   pour les sommets d'un polygone en mode GL_POINT. Les valeurs initiales
 *   rendent la taille de QGPU_SK_POINT_SIZE telle quelle : un flux v9 ne voit
 *   aucune différence. */
#define QGPU_CSUM_OFF           0
#define QGPU_CSUM_ON            1
#define QGPU_CSUM_FORMAT        2

/* ── Interface du kext POMPPCGPU (IOUserClient) ──────────────────────────────
 *
 *   Sélecteurs de IOConnectMethodScalarIScalarO, et types de
 *   IOConnectMapMemory. Partagés entre le kext et les programmes invités.
 *
 *   Plusieurs processus (une application OpenGL = un client) partagent le
 *   device. Le kext découpe BAR0 en QGPU_MAX_CLIENTS tranches égales et donne
 *   à chaque client une tranche et une plage d'identifiants d'objets :
 *     contextes  [ctx_base,  ctx_base  + QGPU_CLIENT_CTX_IDS)
 *     surfaces   [surf_base, surf_base + QGPU_CLIENT_SURF_IDS)
 *   Le mapping (IOConnectMapMemory) ne couvre QUE la tranche du client, et les
 *   (off, len) de SUBMIT sont relatifs à la tranche. En revanche, les offsets
 *   écrits DANS le flux sont absolus dans BAR0 : le client y ajoute slot_base.
 *   À la fermeture d'un client, le kext détruit les objets de sa plage.
 *   Le device reste mono-contexte courant : chaque soumission doit commencer
 *   par un CTX_BIND (un autre client a pu en changer entre-temps).
 */
#define QGPU_MAX_CLIENTS        4
#define QGPU_CLIENT_CTX_IDS     (QGPU_MAX_CTX / QGPU_MAX_CLIENTS)    /* 4 */
#define QGPU_CLIENT_SURF_IDS    (QGPU_MAX_SURF / QGPU_MAX_CLIENTS)   /* 16 */
#define QGPU_CLIENT_TEX_IDS     (QGPU_MAX_TEX / QGPU_MAX_CLIENTS)    /* 128 */
#define QGPU_CLIENT_QUERY_IDS   (QGPU_MAX_QUERIES / QGPU_MAX_CLIENTS) /* 16, v8 */

#define QGPU_UC_GET_INFO        0   /* in : —              out : version, caps, taille de tranche, fence */
#define QGPU_UC_SUBMIT          1   /* in : off, len       out : fence, status, status_pc (off relatif à la tranche) */
#define QGPU_UC_WAIT_FENCE      2   /* in : fence, ms      out : fence courante */
/* v9 : l'ABI du user client ne change pas, sa SÉMANTIQUE peut changer quand le
 * kext posera le doorbell asynchrone (QGPU_CAP_ASYNC) : QGPU_UC_SUBMIT rendra
 * alors la BARRIÈRE DE LA SOUMISSION (QGPU_REG_FENCE_SUBMITTED) au lieu de la
 * fence déjà atteinte, et `status` dira l'acceptation (QGPU_ST_OK ou
 * QGPU_ST_QUEUE_FULL) et non le résultat du rendu — le résultat se lit après
 * QGPU_UC_WAIT_FENCE, qui dormira sur l'interruption DONE au lieu de scruter.
 * Détail et plan de bascule : docs/protocole-v9-asynchrone.md. */
#define QGPU_UC_RESET           3   /* in : —              out : — (détruit les objets du client) */
#define QGPU_UC_GET_SLOT        4   /* in : —              out : index, slot_base, ctx_base, surf_base
                                       (tex_base   = index × QGPU_CLIENT_TEX_IDS,
                                        query_base = index × QGPU_CLIENT_QUERY_IDS) */
#define QGPU_UC_METHOD_COUNT    5

#define QGPU_UC_MEM_SHMEM       0   /* IOConnectMapMemory : la tranche du client */

#endif /* QGPU_PROTO_H */
