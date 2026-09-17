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
#define QGPU_PROTO_VERSION      5   /* v2 : profondeur, état GL ; v3 : textures ;
                                       v4 : brouillard, 2e unité, lignes, points ;
                                       v5 : 4 unités, GL_COMBINE */

/* ── BAR0 : fenêtre partagée (RAM) ───────────────────────────────────────── */
#define QGPU_SHMEM_DEFAULT_MB   64
#define QGPU_SHMEM_MIN_MB       16
#define QGPU_SHMEM_MAX_MB       256

/* ── BAR1 : registres (4 Kio, accès 32 bits, big-endian) ─────────────────── */
#define QGPU_CTRL_BAR_SIZE      4096
#define QGPU_CTRL_TOPADDR       0x40

#define QGPU_REG_MAGIC          0x00  /* r  : QGPU_MAGIC ; w : reset complet */
#define QGPU_REG_VERSION        0x04  /* r  : QGPU_PROTO_VERSION */
#define QGPU_REG_CAPS           0x08  /* r  : QGPU_CAP_* du backend actif */
#define QGPU_REG_SHMEM_SIZE     0x0C  /* r  : taille de BAR0 en octets */
#define QGPU_REG_SUBMIT_OFF     0x10  /* rw : offset du flux dans BAR0 (mult. de 4) */
#define QGPU_REG_SUBMIT_LEN     0x14  /* rw : longueur du flux en octets (mult. de 4) */
#define QGPU_REG_DOORBELL       0x18  /* w  : 1 = exécuter ; r : 1 tant que ça tourne */
#define QGPU_REG_FENCE          0x1C  /* r  : nombre de soumissions terminées */
#define QGPU_REG_STATUS         0x20  /* r  : QGPU_ST_* de la dernière soumission */
#define QGPU_REG_STATUS_PC      0x24  /* r  : index (en mots) de la commande fautive */
#define QGPU_REG_IRQ_MASK       0x28  /* rw : QGPU_IRQ_* démasquées */
#define QGPU_REG_IRQ            0x2C  /* r  : en attente ; w : acquitte les bits écrits */
#define QGPU_REG_DEBUG          0x30  /* w  : un octet vers stderr de QEMU (trace invité) */
#define QGPU_REG_BACKEND_NAME   0x34  /* r  : 4 premiers caractères du backend ('soft'/'gl  ') */

#define QGPU_CAP_SOFT           0x00000001  /* backend logiciel de référence */
#define QGPU_CAP_GL             0x00000002  /* backend OpenGL (rendu sur le GPU hôte) */

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

/* ── Limites ─────────────────────────────────────────────────────────────── */
#define QGPU_MAX_CTX            16
#define QGPU_MAX_SURF           64
#define QGPU_MAX_SURF_DIM       4096
#define QGPU_MAX_CMD_WORDS      (1 << 20)   /* 4 Mio par soumission */
#define QGPU_MAX_VERTS          (1 << 16)
#define QGPU_MAX_TEX            512         /* v3 */
#define QGPU_MAX_TEX_DIM        2048
#define QGPU_MAX_TEX_LEVELS     12
#define QGPU_MAX_UNITS          4           /* v5 : unités de texture */

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

/* Formats de surface. Le mot de pixel échangé est 0xAARRGGBB big-endian :
 * l'octet « x » du framebuffer Tiger EST l'alpha (v2 ; v1 l'ignorait). */
#define QGPU_FMT_XRGB8888       1   /* 32 bpp ARGB big-endian */
#define QGPU_FMT_MASK           0xFF
#define QGPU_FMT_FLAG_DEPTH     0x100  /* v2 : la surface a un tampon de profondeur */

/* CLEAR : masque. Comme glClear, l'effacement respecte les ciseaux, le
 * masque de couleur et le masque de profondeur du contexte courant. */
#define QGPU_CLEAR_COLOR        0x1
#define QGPU_CLEAR_DEPTH        0x2   /* sans effet si la surface n'a pas de profondeur */

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
#define QGPU_SK_COUNT           51

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
 * incomplète) désactive le texturage, comme en OpenGL. */
#define QGPU_TP_MIN_FILTER      1   /* GL_NEAREST, GL_LINEAR, GL_*_MIPMAP_* */
#define QGPU_TP_MAG_FILTER      2   /* GL_NEAREST, GL_LINEAR */
#define QGPU_TP_WRAP_S          3   /* GL_REPEAT, GL_CLAMP, GL_CLAMP_TO_EDGE */
#define QGPU_TP_WRAP_T          4

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
 * par w), et le texturage reste correct en perspective. r est ignoré. */
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

#define QGPU_UC_GET_INFO        0   /* in : —              out : version, caps, taille de tranche, fence */
#define QGPU_UC_SUBMIT          1   /* in : off, len       out : fence, status, status_pc (off relatif à la tranche) */
#define QGPU_UC_WAIT_FENCE      2   /* in : fence, ms      out : fence courante */
#define QGPU_UC_RESET           3   /* in : —              out : — (détruit les objets du client) */
#define QGPU_UC_GET_SLOT        4   /* in : —              out : index, slot_base, ctx_base, surf_base
                                       (tex_base = index × QGPU_CLIENT_TEX_IDS) */
#define QGPU_UC_METHOD_COUNT    5

#define QGPU_UC_MEM_SHMEM       0   /* IOConnectMapMemory : la tranche du client */

#endif /* QGPU_PROTO_H */
