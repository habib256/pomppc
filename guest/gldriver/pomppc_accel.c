/*
 * pomppc_accel.c — accélération du plugin OpenGL POMPPC par le GPU de l'hôte.
 *
 * Principe (docs/gpu-3d-tiger.md §5) :
 *
 *   GLEngine transforme, éclaire, découpe et élimine les faces, puis appelle
 *   les procédures de rastérisation installées par gldInitDispatch avec des
 *   sommets en coordonnées fenêtre. Le plugin remplace celles qu'il sait
 *   rendre (effacement, triangles, bandes, éventails, quads, polygones) par des
 *   versions qui traduisent en commandes qgpu, exécutées sur le GPU hôte dans
 *   une surface qui double le tampon de dessin logiciel.
 *
 *   Tout le reste (textures, brouillard, stencil, points, lignes, pixels…)
 *   continue d'être rendu par le GLDriver d'Apple, dans son tampon. Les deux
 *   copies (tampon invité, surface hôte) sont tenues cohérentes par un
 *   drapeau de fraîcheur par tampon (couleur, profondeur) :
 *
 *     SYNCED      les deux copies sont identiques ;
 *     HOST_NEWER  l'hôte a dessiné depuis : relire avant tout usage invité ;
 *     SW_NEWER    l'invité a dessiné (ou contenu inconnu) : téléverser avant
 *                 tout dessin hôte.
 *
 *   Les points de synchronisation vers l'invité sont les procédures d'échange
 *   et de vidage (0x58/0x5c/0x60), gldFlush/gldFinish, et chaque procédure
 *   logicielle qui lit ou écrit le tampon. Un effacement complet rend la copie
 *   précédente inutile : il ne déclenche aucun téléversement.
 *
 * Le module est sûr par défaut : si le device est absent, si un état GL sort
 * du domaine accéléré ou si une soumission échoue, on retombe sur le code
 * d'Apple, qui est exact.
 *
 * Variables d'environnement :
 *   POMPPC_GL_DISABLE=1   ne jamais accélérer (le plugin reste un mandataire)
 *   POMPPC_GL_STATS=1     bilan sur stderr à la fin du processus
 *   POMPPC_GL_DIRECT=0    pas de présentation directe (voir present_direct)
 *   POMPPC_GL_STATS=fichier  bilan ajouté au fichier toutes les 5 s (images/s,
 *                         relectures, replis, temps passé à soumettre et à copier)
 *   POMPPC_GL_GEOM=0/1/2  chemin brut : 0 coupé, 1 activé (défaut), 2 activé
 *                         avec un format de sommet fixe et large (mesure)
 *   POMPPC_GL_ASYNC=0/1   doorbell asynchrone (défaut : activé si le device et
 *                         le kext le tiennent ; voir « soumission » plus bas)
 *   POMPPC_GLTRACE=dir    trace (voir pomppc_gld.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <math.h>

#include "qgpu_proto.h"
#include "pomppc_gld.h"
#include "pomppc_qgpu.h"

/* ───────────────────── dispositions relevées (Tiger 10.4.6) ─────────────────────
 * Contexte du GLDriver (argument r3 de toutes les procédures) : */
#define CTX_GLSTATE      0x0c     /* état GL de GLEngine */
#define CTX_DEPTH_SCALE  0x14     /* float : valeur de profondeur 1.0 */
#define CTX_WIDTH        0x1c     /* drawable */
#define CTX_HEIGHT       0x20
#define CTX_ROWPIX       0x48     /* pixels par ligne des tampons (pas) */
#define CTX_DEPTHBUF     0x88     /* tampon de profondeur (u32 par pixel) ou 0 */
#define CTX_DRAWSLOT     0x94     /* → mot contenant l'adresse du tampon de dessin */
#define CTX_COLOR_BITS   0xc0     /* 32 : xRGB */
#define CTX_DEPTH_BITS   0xcc     /* 32 : mots de 32 bits */
#define CTX_STENCIL_BITS 0xd0     /* 0 ou 8 : le stencil occupe alors les 8 bits BAS de
                                     chaque mot du tampon de profondeur (24 bits hauts) */
#define CTX_TEXUNITS     0x10     /* table des textures liées : unité·0x14 + cible·4 */
#define CTX_TEXTURING    0x6f4    /* octet : texturage effectif (textures complètes) */
/* Objet texture du GLDriver (créé par gldCreateTexture) : */
#define DT_PARAMS        0x000    /* → paramètres de GLEngine */
#define DT_LEVEL0        0x078    /* niveaux : 0x74 octets chacun, 11 niveaux */
#define DT_LEVEL_SIZE    0x74
#define DT_LEVELS        11
#define DT_BASE_FORMAT   0x578    /* GL_RGB, GL_RGBA, GL_LUMINANCE… */
#define LV_W 0x00                 /* s16 */
#define LV_H 0x02
#define LV_BORDER 0x04
#define LV_ROWPIX 0x06            /* s16 : pixels par ligne des données (alignement) */
#define LV_FORMAT 0x08            /* u16 */
#define LV_TYPE 0x0a              /* u16 */
#define LV_DATA 0x10              /* données dans le format de l'application */
/* Paramètres de GLEngine (DT_PARAMS) : */
#define TP_WRAP_S 0x10
#define TP_WRAP_T 0x12
#define TP_MIN    0x16
#define TP_MAG    0x18
/* État GL de GLEngine : */
#define GS_ALPHA_REF     0x2d60   /* float */
#define GS_ALPHA_FUNC    0x2d64   /* u16 */
#define GS_ALPHA_TEST    0x2d66
#define GS_BLEND_SRC_RGB 0x2d68   /* u16 ×4 */
#define GS_BLEND_DST_RGB 0x2d6a
#define GS_BLEND_SRC_A   0x2d6c
#define GS_BLEND_DST_A   0x2d6e
#define GS_BLEND_EQ_RGB  0x2d80   /* u16 ×2 */
#define GS_BLEND_EQ_A    0x2d82
#define GS_BLEND         0x2d84
#define GS_CLEAR_DEPTH   0x2d88   /* double */
#define GS_CLEAR_COLOR   0x2da0   /* float ×4 */
#define GS_DEPTH_FUNC    0x2dc4   /* u16 */
#define GS_DEPTH_TEST    0x2dc8
#define GS_FOG_COLOR     0x2de0   /* float ×4 */
#define GS_FOG_MODE      0x2e04   /* u16 */
#define GS_FOG           0x2e0a
#define GS_FOG_HINT      0x2e14   /* u16 : GL_NICEST = brouillard par fragment chez Apple */
#define GS_LINE_WIDTH    0x2e20   /* float */
#define GS_LINE_STIPPLE  0x2e2c
#define GS_LINE_SMOOTH   0x2e2d
#define GS_POINT_SIZE    0x30bc   /* float */
#define GS_POINT_SMOOTH  0x30dc
#define GS_POLY_FACTOR   0x3168   /* float */
#define GS_POLY_UNITS    0x316c   /* float */
#define GS_POLY_OFS_PT   0x317b
#define GS_POLY_OFS_LINE 0x317c
#define GS_LOGIC_OP      0x2e33
/* ─── v8 : relevé le 18/09/2026 par la sonde « v8probe » de guest/gltest
 * (un réglage GL par glClear, diff des vidages par tools/re/diffstate.py ;
 * docs/re/etat-v8.md). Tous ces offsets ont été confirmés par un second
 * réglage de valeur différente, et recoupés par glGet dans la même scène. ─── */
#define GS_BLEND_COLOR   0x2d70   /* float ×4 : R, G, B, A (glBlendColor) */
#define GS_LOGIC_OP_MODE 0x2e30   /* u16 : GL_CLEAR 0x1500 … GL_SET 0x150F */
#define GS_LINE_STIP_FACT 0x2e26  /* u16 : facteur 1..256 */
#define GS_LINE_STIP_PAT 0x2e28   /* u16 : motif */
#define GS_POLY_STIP_MASK 0x30e8  /* 128 octets, DANS L'ORDRE de glPolygonStipple :
                                     octet 0 = première ligne du masque = bas de
                                     l'image, MSB à gauche. Le PowerPC étant
                                     gros-boutiste, une lecture de 32 bits donne
                                     déjà le mot que SET_POLYGON_STIPPLE attend :
                                     ni retournement, ni échange d'octets. */
#define GS_COLOR_MASK    0x2e40   /* octets R G B A */
#define GS_DEPTH_MASK    0x2e44
#define GS_POLY_MODE     0x3170   /* u16 avant, u16 arrière */
#define GS_POLY_STIPPLE  0x3178
#define GS_POLY_SMOOTH   0x3179
#define GS_POLY_OFFSET   0x317d
#define GS_SCISSOR_RECT  0x3180   /* i32 x, y, w, h (origine en bas à gauche) */
#define GS_SCISSOR       0x3190
#define GS_SHADE_MODEL   0x3194   /* u32 : 0x1d00 plat, 0x1d01 lisse */
#define GS_STENCIL       0x31c0   /* bit 0 : test de stencil (docs/re/stencil.md) */
#define GS_STENCIL_VMASK 0x3198   /* u32 : masque de valeur */
#define GS_STENCIL_REF   0x319c   /* u32 */
#define GS_STENCIL_FUNC  0x31a0   /* u16 (face avant ; arrière à +0x18) */
#define GS_STENCIL_FAIL  0x31a2   /* u16 ×3 : fail, zfail, zpass */
#define GS_STENCIL_ZFAIL 0x31a4
#define GS_STENCIL_ZPASS 0x31a6
#define GS_STENCIL_WMASK 0x2e38   /* u32 : masque d'écriture */
#define GS_STENCIL_CLEAR 0x2db4   /* u32 : valeur d'effacement */
#define GS_TEXUNIT0      0x31c4   /* unité i : +i·0x7c */
#define GS_TEXUNIT_SIZE  0x7c
#define TU_ENV_COLOR     0x00     /* float ×4 */
#define TU_ENABLE        0x10     /* bits : 1 cube, 2 3D, 4 rectangle, 8 2D, 0x10 1D */
#define TU_ENV_MODE      0x14     /* u16 */
/* GL_COMBINE (relevé par la sonde « combprobe » de guest/gltest) : */
#define TU_COMBINE_RGB   0x18     /* u16 */
#define TU_COMBINE_A     0x1a
#define TU_SRC0_RGB      0x1c     /* u16 ×3 */
#define TU_SRC0_A        0x22     /* u16 ×3 */
#define TU_OP0_RGB       0x28     /* u16 ×3 */
#define TU_OP0_A         0x2e     /* u16 ×3 */
#define TU_RGB_SCALE     0x34     /* float */
#define TU_ALPHA_SCALE   0x38     /* float */
#define GL_MAX_TEXUNITS  8
/* Sommet GLEngine (0x100 octets) : */
#define V_X 0x00
#define V_Y 0x04
#define V_Z 0x08
#define V_COLOR 0x30              /* r g b a */
#define V_FOG   0x4c              /* facteur de brouillard f (1 = pas de brouillard) */
#define V_TEX0  0x80              /* s t r q de l'unité 0, déjà divisés par w */
#define V_TEX(u) (V_TEX0 + 0x10 * (u))   /* unités 0 à 7 */

/* ─── État de transformation et d'éclairage de GLEngine (docs/re/etat-tcl.md §10,
 * relevé par sondes le 18/09/2026). Offsets relatifs au bloc d'état GL (GS).
 * Les offsets NÉGATIFS désignent la zone qui précède le bloc (gctx + x). ─── */
#define GS_MAT_PROJ       0x1920   /* float[16], ordre colonne */
#define GS_MAT_MODELVIEW  0x1960
#define GS_MAT_TEXTURE(u) (0x1c60 + (u) * 0x40)
#define GS_VIEWPORT       0x1840   /* i32 ×4 : x, y, largeur, hauteur (origine en bas) */
#define GS_DEPTH_NEAR_F   0x1850   /* float ×3 : proche, lointain, lointain−proche */
#define GS_NORMALIZE      0x24ad   /* u8 */
#define GS_RESCALE_NORMAL 0x24ae   /* u8 */
#define GS_SCENE_AMBIENT  0x24b0   /* float ×4 : GL_LIGHT_MODEL_AMBIENT */
#define GS_LIGHT(i)       (0x24c0 + (i) * 0x80)
#define LT_AMBIENT        0x00     /* float ×4 */
#define LT_DIFFUSE        0x10
#define LT_SPECULAR       0x20
#define LT_POSITION       0x30     /* float ×4, coordonnées ŒIL : ne pas retransformer */
#define LT_SPOT_DIR       0x40     /* float ×3, coordonnées ŒIL */
#define LT_SPOT_COS       0x4c     /* float : COSINUS du seuil ; < 0 = pas un spot */
#define LT_ATT_CONST      0x50
#define LT_ATT_LINEAR     0x54
#define LT_ATT_QUAD       0x58
#define LT_SPOT_EXP       0x5c
#define GS_MATERIAL_FRONT 0x28c0   /* objets de 0x240 octets, DANS le bloc */
#define GS_MATERIAL_BACK  0x2b00
#define MT_AMBIENT        0x00     /* float ×4 */
#define MT_DIFFUSE        0x10
#define MT_SPECULAR       0x20
#define MT_EMISSION       0x30
#define MT_SHININESS      0x40     /* float */
#define GS_LIGHT_MASK     0x2d40   /* u32 : bit i = GL_LIGHT0+i allumée */
#define GS_COLORMAT_FACE  0x2d44   /* u16 */
#define GS_COLORMAT_MODE  0x2d46   /* u16 */
#define GS_COLOR_CONTROL  0x2d48   /* u16 : 0x81f9 SINGLE / 0x81fa SEPARATE_SPECULAR */
#define GS_LIGHTING       0x2d4a   /* u8 */
#define GS_COLOR_MATERIAL 0x2d4b   /* u8 */
#define GS_TWO_SIDE       0x2d4c   /* u8 */
#define GS_LOCAL_VIEWER   0x2d4d   /* u8 */
#define GS_FOG_DENSITY    0x2df0   /* float */
#define GS_FOG_START      0x2df4
#define GS_FOG_END        0x2df8
#define GS_FOG_COORD_SRC  0x2e06   /* u16 : 0x8451 FOG_COORDINATE / 0x8452 FRAGMENT_DEPTH */
#define GS_POINT_ATT      0x30cc   /* float ×3 : constante, linéaire, quadratique */
#define GS_FRONT_FACE     0x3174   /* u16 : GL_CW 0x900 / GL_CCW 0x901 */
#define GS_CULL_MODE      0x3176   /* u16 : 0x404 / 0x405 / 0x408 */
#define GS_CULL_FACE      0x317a   /* u8 */
#define GS_TEXGEN(u)      (0x3988 + (u) * 0x94)
#define TG_COORD(c)       ((c) * 0x24)
#define TG_MODE           0x00     /* u16 : énumération GL telle quelle */
#define TG_EYE_PLANE      0x04     /* float ×4, coordonnées ŒIL */
#define TG_OBJ_PLANE      0x14     /* float ×4, brut */
#define TG_ENABLE         0x90     /* u8 ×4 : S, T, R, Q */
#define GS_CLIP_MASK      0x3e28   /* u32 : bit i = GL_CLIP_PLANE0+i */
#define GS_CLIP_PLANE(i)  (0x3e2c + (i) * 0x10)     /* float ×4, coordonnées ŒIL */
/* Valeurs courantes : elles sont AVANT le bloc (gctx = GS − 0x360). */
#define GS_CUR_TEXCOORD(u) (-0x240 + (u) * 0x10)    /* float ×4 */
#define GS_CUR_COLOR      (-0x0c0)                  /* float ×4 */
#define GS_CUR_NORMAL     (-0x0b0)                  /* float ×3 */
#define GS_CUR_SECCOLOR   (-0x0a0)                  /* float ×3 */
#define GS_CUR_FOGCOORD   (-0x094)                  /* float */

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_FLAT             0x1d00
#define GL_FILL             0x1b02

#define U16(p, o) (*(unsigned short *)((unsigned char *)(p) + (o)))
#define S16(p, o) (*(short *)((unsigned char *)(p) + (o)))
#define I32(p, o) (*(long *)((unsigned char *)(p) + (o)))
#define F64(p, o) (*(double *)((unsigned char *)(p) + (o)))

/* ───────────────────────────── disposition de la tranche ─────────────────────────────
 *
 * La tranche du client (16 Mio sur une fenêtre de 64) est coupée en DEUX
 * MOITIÉS identiques, alternées à chaque soumission. C'est ce que le contrat
 * mémoire de la v9 exige : tant que FENCE n'a pas dépassé une soumission,
 * l'hôte peut lire à tout moment son flux de commandes, ses sommets, ses
 * indices et les texels qu'elle désigne, et écrire dans ses zones de
 * relecture. Écrire l'image n+1 dans la même moitié pendant que l'hôte dessine
 * la n, ce serait des triangles qui clignotent — pas un plantage, ce qui est
 * bien pire à diagnostiquer.
 *
 * DEUX, et pas trois : à 5,3 Mio la troisième moitié ne laisserait plus la
 * place à DEUX transferts plein écran dans l'arène (1024×768×4 = 3 Mio chacun,
 * couleur + profondeur), et chaque manque de place coûte un vidage de plus. Et
 * deux suffisent à ce qu'on cherche : une soumission en vol pendant qu'on
 * prépare la suivante.
 *
 * Les offsets ci-dessous sont RELATIFS à la moitié courante. G.hb en donne le
 * début dans la tranche, G.win/G.base/G.cmd les adresses correspondantes ;
 * SEULE l'arène est adressée en absolu (G.q.win + off), parce que ses copies
 * différées survivent au changement de moitié. */
#define HALVES      2
#define CMD_WORDS   (0x40000 / 4)       /* flux : 256 Kio */
#define VTX_OFF     0x40000             /* sommets : jusqu'à 1,5 Mio */
#define VTX_END     0x200000
/* Les indices de la FUSION (v7 indexée) vivent en HAUT de la zone des sommets :
   ils naissent et meurent avec elle (même vidage, même remise à zéro), et le
   cœur les veut dans la fenêtre partagée comme les sommets. 256 Kio = 131 072
   indices u16, soit largement plus que les ~24 000 d'une image de Marble Blast. */
#define IDX_SIZE    0x40000
#define IDX_OFF     (VTX_END - IDX_SIZE)
#define VTX_LIMIT   IDX_OFF             /* les sommets s'arrêtent là */
#define ARENA_OFF   0x200000            /* transferts : le reste de la moitié */
#define MAX_POST    16
/* Place de flux que arena_alloc garantit en plus de l'arène : de quoi écrire la
   commande qui désignera l'arène SANS que reserve() vide le flux entre les
   deux. Sinon la commande partirait dans une autre soumission — et, depuis la
   v9, dans une autre MOITIÉ — que la mémoire qu'elle désigne. */
#define ARENA_CMD_ROOM 64

enum { SYNCED = 0, HOST_NEWER = 1, SW_NEWER = 2 };

/* Dernière clé d'état remplie par le plugin, plus un (clés v7 de géométrie). */
#define PLUGIN_SK_END_GEOM (QGPU_SK_FOG_END + 1)
/* Idem pour les clés v8 (mélange constant, opération logique, modes de polygone,
 * pointillés) : elles sont CONTIGUËS et commencent juste après les clés v7. */
#define PLUGIN_SK_END_V8   (QGPU_SK_POLYGON_STIPPLE + 1)

/* Genres de séries de sommets : opcode et taille des sommets. */
enum { RK_TRI = 0, RK_TRI_TEX = 1, RK_TRI_TEX2 = 2, RK_LINES = 3, RK_POINTS = 4,
       RK_TRI_TEX3 = 5, RK_TRI_TEX4 = 6, RK_COUNT = 7 };
/* unités de texture portées par le sommet, par genre */
static const int rk_units[RK_COUNT] = { 0, 1, 2, 0, 0, 3, 4 };
static const unsigned long rk_words[RK_COUNT] = {
    QGPU_VERTEX_WORDS, QGPU_VERTEX_TEX_WORDS, QGPU_VERTEX_TEX2_WORDS,
    QGPU_VERTEX_WORDS, QGPU_VERTEX_WORDS,
    QGPU_VERTEX_TEXN_WORDS(3), QGPU_VERTEX_TEXN_WORDS(4),
};

typedef struct PCtx {
    struct PCtx   *next;
    void          *ctx;                 /* contexte du GLDriver d'Apple */
    void         **procs;               /* table de procédures de GLEngine */
    void          *real[PROC_COUNT];    /* procédures d'Apple pour ce contexte */
    void          *mine[PROC_COUNT];    /* ce que le plugin a installé (0 : rien) */
    long           qctx;                /* identifiant qgpu, -1 si aucun */
    long           surf;                /* surface qgpu, -1 si aucune */
    unsigned long  sw, sh;              /* taille de la surface */
    int            color, depth;        /* fraîcheur */
    int            broken;              /* plus jamais d'accélération */
    unsigned long  st[QGPU_SK_COUNT];   /* état envoyé au device */
    int            st_valid;
    unsigned char *draw_seen;           /* tampon de dessin connu */
    unsigned long  direct_at;           /* n° de la dernière image présentée directement */
    int            stencil;             /* la surface hôte a un stencil ; sa fraîcheur est
                                           celle de la profondeur (même mot côté invité) */
    int            sten_used;           /* le contexte s'est VRAIMENT servi du stencil */
    /* ── chemin brut (v7) : la géométrie part non transformée sur l'hôte ── */
    unsigned char *cfg;                 /* bloc de configuration de gldCreateContext */
    unsigned long  desc[8];             /* descripteur de sortie de sommet (cfg+0x11c),
                                           durée de vie = celle du contexte */
    unsigned long  geom_fmt;            /* masque QGPU_VF_* publié */
    unsigned long  geom_words;          /* pas d'un sommet, en mots */
    int            desc_dirty;          /* republié : demander le bit 1 du dispatch */
    int            geom_on;             /* dernier verdict du domaine */
    int            geom_lost;           /* une primitive a été perdue : plus jamais de brut */
    /* État géométrique déjà posé sur le device. On garde une copie des OCTETS
       SOURCES du bloc GLEngine, pas des arguments envoyés : comparer la source
       coûte un memcmp, fabriquer les arguments pour les comparer coûtait 7 % du
       temps de Marble Blast (relevé `sample`, put_f en tête des feuilles). */
    int            g_sent;
    unsigned long  c_mtx[QGPU_MTX_COUNT][16];        /* 64 o par matrice */
    unsigned long  c_vp[4];                          /* viewport i32 ×4 */
    unsigned long  c_dr[2];                          /* proche, lointain */
    unsigned long  c_lmask;
    unsigned long  c_light[QGPU_MAX_LIGHTS][24];     /* 0x60 o utiles par lumière */
    unsigned long  c_mat[2][17];                     /* 0x44 o par face */
    unsigned long  c_lm[4];
    unsigned long  c_tg[QGPU_MAX_UNITS][37];         /* 0x94 o : modes, plans, actifs */
    unsigned char  c_tg_on[QGPU_MAX_UNITS];          /* du texgen est-il posé sur le device ? */
    unsigned long  c_clip[25];                       /* masque + 6 plans, contigus */
    unsigned long  c_cur[4][4];                      /* couleur, normale, secondaire, brouillard */
    /* ── v8 ── */
    unsigned long  c_pstip[32];         /* motif de pointillé posé sur le device */
    int            c_pstip_valid;
    long           q_open;              /* requête d'occlusion ouverte, -1 si aucune */
    unsigned long  q_extra;             /* fragments dessinés par le LOGICIEL pendant
                                           la requête : comptés en trop, jamais en
                                           moins (voir q_end) */
} PCtx;

typedef struct PTex {                   /* texture du GLDriver suivie par le plugin */
    struct PTex   *next;
    struct PTex   *hnext;               /* chaînage de la table de hachage */
    void          *drvtex;
    long           qtex;                /* identifiant hôte, -1 si aucun */
    int            dirty;               /* niveaux à (re)téléverser */
    unsigned long  prm[4];              /* paramètres envoyés : min, mag, wrap s, wrap t */
    int            prm_valid;
} PTex;

typedef struct TexUnit {
    PTex          *t;                   /* 0 : unité inactive */
    unsigned long  env_mode, env_color;
    unsigned long  combine, combine_src; /* GL_COMBINE empaqueté (v5) */
} TexUnit;

typedef struct TexInfo {                /* textures à appliquer pour le dessin en cours */
    TexUnit        u[QGPU_MAX_UNITS];
} TexInfo;

typedef struct Post {                   /* copie à faire APRÈS la barrière */
    int            depth;               /* 0 couleur, 1 profondeur, 2 stencil */
    int            packed;              /* profondeur 24 bits + stencil 8 bits dans le mot */
    unsigned long  off;                 /* dans l'arène, ABSOLU dans la tranche */
    unsigned char *dst;
    unsigned long  w, h, rowbytes;
    float          scale;
} Post;

/* Une moitié de la tranche, et la soumission qui l'occupe (v9). */
typedef struct Half {
    unsigned long  fence;               /* barrière de la soumission en vol */
    int            busy;                /* elle n'est pas encore terminée */
    Post           post[MAX_POST];      /* relectures à recopier après la barrière */
    int            npost;
} Half;

static struct {
    pthread_mutex_t mu;
    int             state;              /* 0 inconnu, 1 actif, -1 désactivé */
    QgpuClient      q;
    /* ── moitiés de la tranche (v9) ── */
    Half            h[HALVES];
    int             cur;                /* moitié en cours d'écriture */
    int             nhalf;              /* 2 en asynchrone, 1 sinon */
    unsigned long   half;               /* taille d'une moitié */
    unsigned long   hb;                 /* début de la moitié courante dans la tranche */
    unsigned char  *win;                /* = q.win + hb */
    unsigned long   base;               /* = q.base + hb (offsets écrits DANS le flux) */
    int             async;              /* doorbell asynchrone actif MAINTENANT */
    int             async_avail;        /* … et disponible tout court */
    unsigned long   async_retry_at;     /* image où le reprendre (voir check_errors) */
    const char     *async_why;          /* pourquoi il ne l'est pas */
    unsigned long   errors;             /* QGPU_REG_ERRORS vu à la dernière soumission */
    int             err_valid;
    unsigned long  *cmd;                /* = win */
    unsigned long   ncmd;
    unsigned long   vtx;                /* octets utilisés depuis VTX_OFF */
    unsigned long   arena;              /* octets utilisés depuis ARENA_OFF */
    PCtx           *bound;              /* contexte lié dans le flux en cours */
    PCtx           *run_ctx;            /* série de triangles ouverte */
    unsigned long   run_start, run_count;
    int             run_kind;           /* genre de la série : RK_* */
    /* série DRAW_RAW ouverte (chemin brut) : la commande n'est écrite qu'à sa
       fermeture, pour fusionner les EndPrimitiveBuffer contigus */
    PCtx           *raw_ctx;
    unsigned long   raw_start, raw_count, raw_fmt, raw_words;
    unsigned long   raw_mode;
    unsigned long   raw_vend;           /* octet APRÈS le dernier sommet de la série */
    unsigned long   raw_idx;            /* offset des indices dans la fenêtre, 0 = aucun */
    unsigned long   raw_nidx;           /* indices déjà écrits pour cette série */
    unsigned long   raw_lots;           /* lots recollés dans cette série */
    unsigned long   idx;                /* octets d'indices pris depuis IDX_OFF */
    /* tampon rendu par BeginPrimitiveBuffer, en attente de EndPrimitiveBuffer */
    PCtx           *pend;
    unsigned long   pend_half;          /* moitié où GLEngine écrit en ce moment */
    unsigned long   pend_off, pend_words, pend_fmt, pend_slots;
    int             pend_drop;
    int             pend_flat;          /* ombrage plat : change l'ordre des indices */
    int             pend_wire;          /* mode de polygone ≠ GL_FILL : pas de fusion
                                           en triangles, elle perdrait le contour */
    unsigned long   ctx_used, surf_used;
    unsigned long   tex_used[(QGPU_CLIENT_TEX_IDS + 31) / 32];
    PCtx           *list;
    PTex           *textures;
    /* statistiques */
    unsigned long   n_tris, n_clears, n_submits, n_uploads, n_readbacks, n_fallback;
    unsigned long   n_textris, n_texuploads, n_lines, n_points;
    unsigned long   n_frames, n_direct, n_tex_incomplete;
    unsigned long   n_rawverts, n_rawdraws, n_geomcmds, n_geomdrop, n_rawmerged;
    int             v7;                 /* device v7 ET chemin brut autorisé */
    int             v8;                 /* device v8 : pipeline fixe complet */
    unsigned long   query_base;         /* premier identifiant de requête du client */
    double          t_submit, t_copy, t_upload;   /* secondes cumulées */
    /* ── bilan du mode asynchrone ── */
    unsigned long   n_waits;            /* barrières réellement attendues */
    unsigned long   n_qfull;            /* soumissions refusées (file pleine) */
    unsigned long   n_syncfall;         /* retours en synchrone sur ERRORS */
    unsigned long   n_qsamples, n_qsum; /* profondeur de file échantillonnée */
    double          t_wait;             /* secondes passées à attendre une barrière */
} G = { PTHREAD_MUTEX_INITIALIZER };

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

/* Motifs de refus de l'accélération : compteurs et détail du premier cas,
 * rapportés par le bilan périodique. */
enum {
    NO_BUFFER, NO_RASTER, NO_FOG, NO_POLYMODE, NO_DEPTH, NO_BLEND, NO_ALPHA,
    NO_SURFACE, NO_TEX_UNITS, NO_TEX_TARGET, NO_TEX_ENV, NO_TEX_UNKNOWN,
    NO_TEX_BASE, NO_TEX_SIZE, NO_TEX_FORMAT, NO_TEX_ID, NO_TEX_COMBINE, NO_STENCIL,
    /* sorties du domaine propres au chemin brut (v7) */
    NO_G_RASTER, NO_G_POINT, NO_G_PROGRAM, NO_G_STRIDE, NO_G_LATE,
    /* v8 : ce qui reste hors domaine une fois les fonctions v8 branchées */
    NO_Q_FALLBACK, NO_TEX_PARAM, NO_COUNT
};
static const char *const no_name[NO_COUNT] = {
    "tampon", "logicop/stipple/lissage", "brouillard", "polygonmode", "profondeur",
    "melange", "alphatest", "surface", "unites>2", "cible-texture", "texenv",
    "texture-inconnue", "format-base", "taille-texture", "format-texels", "id-texture",
    "combine", "stencil",
    "brut:lissage", "taille-de-point-attenuee", "brut:programme",
    "brut:pas-de-sommet", "brut:etat-tardif",
    "requete:repli-logiciel", "param-texture",
};
static unsigned long no_count[NO_COUNT];
static char no_detail[NO_COUNT][64];
static unsigned long fb_count[PROC_COUNT];      /* replis par procédure */

static int no(int why, unsigned long a, unsigned long b)
{
    if (!no_count[why]++)
        snprintf(no_detail[why], sizeof(no_detail[why]), "%lx/%lx", a, b);
    return 0;
}

static const char *direct_why(void);
static int geom_switch(void);
static void async_rearm(void);
static unsigned long fbits(float f);
/* Requêtes d'occlusion : coupées si l'hôte ne les tient pas (le refus arrive
   par le statut d'une soumission, donc dans broken_all, bien avant la section
   qui les réalise). */
static int qry_off;

/* Bilan périodique (POMPPC_GL_STATS=<fichier>), appelé à chaque échange. */
static void stats_frame(void)
{
    static const char *path;
    static int init;
    static double t0;
    static unsigned long f0, tr0, rb0, up0, fb0, sub0, tu0, di0;
    static unsigned long rv0, rd0, gc0, rm0, w0, qf0;
    static double ts0, tc0, tu_0, tw0;
    double t;

    if (!init) {
        const char *e = getenv("POMPPC_GL_STATS");
        init = 1;
        path = (e && e[0] == '/') ? e : 0;
        t0 = now_s();
    }
    G.n_frames++;
    async_rearm();
    if (!path)
        return;
    /* Profondeur de la file du device, une fois par image et SEULEMENT quand
       le bilan périodique est demandé : c'est un appel au kext de plus. */
    if (G.async) {
        unsigned long inflight = 0, fr = 0, depth = 0;
        if (qgpu_queue(&G.q, &inflight, &fr, &depth) == 0) {
            G.n_qsum += inflight;
            G.n_qsamples++;
        }
    }
    t = now_s();
    if (t - t0 >= 5.0) {
        FILE *f = fopen(path, "a");
        if (f) {
            double dt = t - t0;
            fprintf(f, "%.1f img/s | tri %lu/img | relect %lu | televers %lu (tex %lu) | "
                    "replis %lu | soumissions %lu | submit %.2f ms/img | copie %.2f ms/img | "
                    "prep televers %.2f ms/img | directes %lu\n",
                    (G.n_frames - f0) / dt,
                    (G.n_tris - tr0) / (G.n_frames - f0 ? G.n_frames - f0 : 1),
                    G.n_readbacks - rb0, G.n_uploads - up0, G.n_texuploads - tu0,
                    G.n_fallback - fb0, G.n_submits - sub0,
                    (G.t_submit - ts0) * 1000 / (G.n_frames - f0),
                    (G.t_copy - tc0) * 1000 / (G.n_frames - f0),
                    (G.t_upload - tu_0) * 1000 / (G.n_frames - f0),
                    G.n_direct - di0);
            {   /* v9 : ce que coûte (ou ne coûte plus) l'attente de l'hôte. */
                unsigned long fr = G.n_frames - f0 ? G.n_frames - f0 : 1;
                fprintf(f, "    soumission %s : %lu barrière(s) attendue(s)/img, "
                        "%.2f ms d'attente/img, file %.2f en vol, %lu QUEUE_FULL, "
                        "%lu repli(s) synchrone(s)\n",
                        G.async ? "asynchrone" : "synchrone",
                        (G.n_waits - w0) / fr, (G.t_wait - tw0) * 1000 / fr,
                        G.n_qsamples ? (double)G.n_qsum / G.n_qsamples : 0.0,
                        G.n_qfull - qf0, G.n_syncfall);
                G.n_qsum = 0; G.n_qsamples = 0;
            }
            {
                unsigned long fr = G.n_frames - f0 ? G.n_frames - f0 : 1;
                if (G.n_rawverts != rv0 || G.n_rawdraws != rd0) {
                    unsigned long nd = G.n_rawdraws - rd0;
                    fprintf(f, "    brut : %lu sommets/img, %lu DRAW_RAW/img, "
                            "%lu commandes d'état/img, %lu fusionnés/img, "
                            "%lu sommets/dessin%s\n",
                            (G.n_rawverts - rv0) / fr, nd / fr,
                            (G.n_geomcmds - gc0) / fr, (G.n_rawmerged - rm0) / fr,
                            nd ? (G.n_rawverts - rv0) / nd : 0,
                            G.n_geomdrop ? " ⚠ PRIMITIVES PERDUES" : "");
                }
            }
            {
                int k;
                for (k = 0; k < NO_COUNT; k++)
                    if (no_count[k])
                        fprintf(f, "    refus %s : %lu (premier : %s)\n",
                                no_name[k], no_count[k], no_detail[k]);
                if (direct_why()[0])
                    fprintf(f, "    présentation directe coupée : %s\n", direct_why());
                for (k = 0; k < PROC_COUNT; k++)
                    if (fb_count[k])
                        fprintf(f, "    repli %s : %lu\n", pomppc_proc_name(k), fb_count[k]);
                memset(no_count, 0, sizeof(no_count));
                memset(fb_count, 0, sizeof(fb_count));
            }
            fclose(f);
        }
        t0 = t; f0 = G.n_frames; tr0 = G.n_tris; rb0 = G.n_readbacks; up0 = G.n_uploads;
        fb0 = G.n_fallback; sub0 = G.n_submits; tu0 = G.n_texuploads; di0 = G.n_direct;
        ts0 = G.t_submit; tc0 = G.t_copy; tu_0 = G.t_upload;
        rv0 = G.n_rawverts; rd0 = G.n_rawdraws; gc0 = G.n_geomcmds;
        rm0 = G.n_rawmerged; w0 = G.n_waits; qf0 = G.n_qfull; tw0 = G.t_wait;
    }
}

/* ────────────────────────────── utilitaires ────────────────────────────── */

static float clamp01(float v)
{
    return !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v;
}

static unsigned long to_u8(float v)
{
    v = clamp01(v);
    return (unsigned long)(v * 255.0f + 0.5f);
}

static unsigned char *gls(PCtx *p)
{
    return (unsigned char *)GLD_U32(p->ctx, CTX_GLSTATE);
}

static unsigned char *sw_color(PCtx *p)
{
    unsigned long slot = GLD_U32(p->ctx, CTX_DRAWSLOT);
    return slot ? (unsigned char *)GLD_U32(slot, 0) : 0;
}

static unsigned char *sw_depth(PCtx *p)
{
    return (unsigned char *)GLD_U32(p->ctx, CTX_DEPTHBUF);
}

static unsigned long sw_rowbytes(PCtx *p)
{
    return GLD_U32(p->ctx, CTX_ROWPIX) * 4;
}

static PCtx *find_ctx(void *ctx)
{
    PCtx *p;
    for (p = G.list; p; p = p->next)
        if (p->ctx == ctx)
            return p;
    return 0;
}

static void on_exit_stats(void)
{
    if (getenv("POMPPC_GL_STATS")) {
        int k;
        for (k = 0; k < NO_COUNT; k++)
            if (no_count[k])
                fprintf(stderr, "POMPPC GL : refus %s : %lu (premier : %s)\n",
                        no_name[k], no_count[k], no_detail[k]);
    }
    if (getenv("POMPPC_GL_STATS"))
        fprintf(stderr, "POMPPC GL : %lu triangles (%lu texturés), %lu segments, %lu points "
                "et %lu effacements sur l'hôte, %lu soumissions, %lu téléversements, "
                "%lu niveaux de texture, %lu relectures, %lu appels logiciels synchronisés ; "
                "brut : %lu sommets en %lu DRAW_RAW, %lu primitives perdues ; "
                "soumission %s : %lu barrières attendues (%.0f ms), %lu QUEUE_FULL, "
                "%lu repli(s) synchrone(s)\n",
                G.n_tris, G.n_textris, G.n_lines, G.n_points, G.n_clears, G.n_submits,
                G.n_uploads, G.n_texuploads, G.n_readbacks, G.n_fallback,
                G.n_rawverts, G.n_rawdraws, G.n_geomdrop,
                G.async ? "asynchrone" : "synchrone", G.n_waits, G.t_wait * 1000,
                G.n_qfull, G.n_syncfall);
}

int pomppc_accel_enabled(void)
{
    return G.state > 0;
}

/* POMPPC_GL_ASYNC : doorbell asynchrone. Défaut ACTIVÉ dès que les quatre
 * preuves passent — device v9, QGPU_CAP_ASYNC annoncé, kext qui sait poser le
 * drapeau (sondé par un « peek » qu'un kext plus ancien refuse proprement), et
 * tranche assez grande pour deux moitiés utilisables. Une seule manque et on
 * reste en synchrone, exactement comme avant : le mode asynchrone n'est pas un
 * repli, c'est un supplément. */
static void async_switch(void)
{
    const char *e = getenv("POMPPC_GL_ASYNC");
    unsigned long half = (G.q.size / HALVES) & ~0xFFFUL;

    G.async = 0;
    if (e && e[0] == '0') {
        G.async_why = "POMPPC_GL_ASYNC=0";
        return;
    }
    if (G.q.version < 9) {
        G.async_why = "device antérieur à la v9";
        return;
    }
    if (!(G.q.caps & QGPU_CAP_ASYNC)) {
        G.async_why = "le device n'annonce pas QGPU_CAP_ASYNC";
        return;
    }
    /* Une moitié doit porter le flux, les sommets, les indices et une arène
       digne de ce nom (deux transferts plein écran). Sinon, une seule moitié. */
    if (half < ARENA_OFF + 0x100000) {
        G.async_why = "tranche trop petite pour deux moitiés";
        return;
    }
    if (!qgpu_async_ok(&G.q)) {
        G.async_why = "le kext installé ne connaît pas le doorbell asynchrone";
        return;
    }
    G.half  = half;
    G.nhalf = HALVES;
    G.async = 1;
    G.async_avail = 1;
}

void pomppc_backend_init(void)
{
    const char *why = 0;
    pthread_mutex_lock(&G.mu);
    if (G.state == 0) {
        if (getenv("POMPPC_GL_DISABLE")) {
            G.state = -1;
            why = "POMPPC_GL_DISABLE";
        } else if (qgpu_open(&G.q, &why) == 0) {
            G.state = 1;
            /* Une seule moitié par défaut : le mode synchrone garde alors
               exactement la disposition et le comportement d'avant la v9. */
            G.nhalf = 1;
            G.half  = G.q.size;
            G.cur   = 0;
            G.hb    = 0;
            G.win   = G.q.win;
            G.base  = G.q.base;
            G.cmd   = (unsigned long *)G.win;
            async_switch();
            /* Le chemin brut demande un device v7 : sur un device plus ancien,
               les clés de géométrie n'existent pas et les envoyer ferait
               refuser toutes les soumissions (vu en vrai au passage en v6). */
            G.v7 = G.q.version >= 7 && geom_switch() > 0;
            /* La v8 (mélange constant, opération logique, modes de polygone,
               pointillés, requêtes) vaut pour les DEUX chemins de dessin : elle
               ne dépend donc pas de POMPPC_GL_GEOM, seulement du device. */
            G.v8 = G.q.version >= 8;
            G.query_base = G.q.index * QGPU_CLIENT_QUERY_IDS;
            atexit(on_exit_stats);
        } else {
            G.state = -1;
        }
        if (G.state > 0)
            pomppc_log("POMPPC: qgpu actif (tranche %lu à 0x%lx, %lu Mio, v%lu, caps 0x%lx,"
                       " chemin brut %s, pipeline fixe v8 %s, soumission %s%s%s)\n",
                       G.q.index, G.q.base, G.q.size >> 20, G.q.version, G.q.caps,
                       G.v7 ? "actif" : "coupé", G.v8 ? "actif" : "coupé",
                       G.async ? "asynchrone (2 moitiés)" : "synchrone",
                       G.async ? "" : " : ", G.async ? "" : G.async_why);
        else
            pomppc_log("POMPPC: accélération désactivée : %s\n", why);
    }
    pthread_mutex_unlock(&G.mu);
}

/* ───────────────────────────── flux de commandes ───────────────────────────── */

static void flush(void);

static void close_run(void)
{
    if (G.run_ctx && G.run_count) {
        unsigned long *c = G.cmd + G.ncmd;
        static const unsigned long ops[RK_COUNT] = {
            QGPU_OP_DRAW_TRIANGLES, QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_OP_DRAW_TRIANGLES_TEX2,
            QGPU_OP_DRAW_LINES, QGPU_OP_DRAW_POINTS,
            QGPU_OP_DRAW_TRIANGLES_TEXN, QGPU_OP_DRAW_TRIANGLES_TEXN,
        };
        int n = rk_units[G.run_kind];
        c[0] = QGPU_CMD_HDR(ops[G.run_kind], n >= 3 ? QGPU_LEN_DRAW_N : QGPU_LEN_DRAW);
        c[1] = G.run_count;
        c[2] = G.base + VTX_OFF + G.run_start;
        if (n >= 3) {
            c[3] = n;                   /* DRAW_TRIANGLES_TEXN : nombre d'unités */
            G.ncmd += QGPU_LEN_DRAW_N;
        } else {
            G.ncmd += QGPU_LEN_DRAW;
        }
    }
    G.run_ctx = 0;
    G.run_count = 0;
}

/* Ferme la série DRAW_RAW en cours (chemin brut) : la commande n'est écrite
 * qu'ici, pour pouvoir fusionner les EndPrimitiveBuffer contigus. La place est
 * garantie par la marge de reserve(). */
static void close_raw(void)
{
    if (G.raw_ctx && G.raw_count) {
        unsigned long *c;
        /* Un vidage a pu survenir entre l'ouverture de la série et ici :
           geom_begin vide le flux APRÈS avoir envoyé l'état, quand la place des
           sommets manque. Le flux est alors neuf et plus aucun contexte n'y est
           lié — sans ce CTX_BIND, le DRAW_RAW serait la première commande d'une
           soumission et l'hôte répondrait QGPU_ST_NO_CTX (le device est
           mono-contexte courant : chaque soumission commence par un CTX_BIND). */
        if (G.bound != G.raw_ctx) {
            c = G.cmd + G.ncmd;
            c[0] = QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX);
            c[1] = G.raw_ctx->qctx;
            G.ncmd += QGPU_LEN_CTX;
            G.bound = G.raw_ctx;
        }
        c = G.cmd + G.ncmd;
        c[0] = QGPU_CMD_HDR(QGPU_OP_DRAW_RAW, QGPU_LEN_DRAW_RAW);
        c[1] = G.raw_mode;
        c[2] = G.raw_count;             /* sommets, ou INDICES si la série est indexée */
        c[3] = G.base + VTX_OFF + G.raw_start;
        c[4] = 0;                       /* pas serré : le format donne le pas */
        c[5] = G.raw_fmt;
        c[6] = G.raw_idx ? G.base + G.raw_idx : 0;
        c[7] = G.raw_idx ? QGPU_IDX_U16 : QGPU_IDX_NONE;
        c[8] = 0;                       /* premier : toujours 0, et le cœur l'exige
                                           quand des indices sont donnés */
        /* nverts : le cœur valide les indices contre lui, et relit tout le
           tableau. C'est l'étendue des sommets de la série, pas le nombre
           d'indices — les deux coïncident quand la série n'est pas indexée. */
        c[9] = (G.raw_vend - G.raw_start) / (G.raw_words * 4);
        G.ncmd += QGPU_LEN_DRAW_RAW;
        G.n_rawdraws++;
        if (G.raw_lots > 1)
            G.n_rawmerged += G.raw_lots - 1;
    }
    G.raw_ctx = 0;
    G.raw_count = 0;
    G.raw_idx = 0;
    G.raw_nidx = 0;
    G.raw_lots = 0;
}

/* Réserve `words` mots de flux pour le contexte p (CTX_BIND inclus au besoin).
   La marge couvre ce que close_run et close_raw peuvent encore écrire, leurs
   CTX_BIND compris (4 mots : deux liaisons possibles). */
static unsigned long *reserve(PCtx *p, unsigned long words)
{
    unsigned long *c;
    close_run();
    close_raw();
    if (G.ncmd + words + 4 + QGPU_LEN_DRAW_N + QGPU_LEN_DRAW_RAW > CMD_WORDS)
        flush();
    if (G.bound != p) {
        c = G.cmd + G.ncmd;
        c[0] = QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX);
        c[1] = p->qctx;
        G.ncmd += QGPU_LEN_CTX;
        G.bound = p;
    }
    c = G.cmd + G.ncmd;
    G.ncmd += words;
    return c;
}

/* Réserve n octets d'arène (alignés sur 4) ET la place de flux de la commande
 * qui les désignera ; peut vider le flux. 0 = impossible.
 *
 * La place de flux est réservée ICI, et c'est essentiel : tous les appelants
 * font « arena_alloc puis reserve », et si reserve vidait le flux entre les
 * deux, la commande partirait dans une soumission — depuis la v9, dans une
 * MOITIÉ — différente de la mémoire qu'elle désigne. L'hôte lirait alors une
 * arène qu'on est déjà en train de réécrire. */
static long arena_alloc(unsigned long n, unsigned long *off)
{
    n = (n + 3) & ~3UL;
    if (ARENA_OFF + G.arena + n > G.half || G.h[G.cur].npost >= MAX_POST ||
        G.ncmd + ARENA_CMD_ROOM > CMD_WORDS) {
        flush();
        if (ARENA_OFF + n > G.half)
            return 0;
    }
    /* ABSOLU dans la tranche : une copie différée peut survivre au changement
       de moitié, et se relit alors par G.q.win + off. */
    *off = G.hb + ARENA_OFF + G.arena;
    G.arena += n;
    return 1;
}

static void broken_all(const char *why, long st, unsigned long pc)
{
    PCtx *p;
    /* Un DRAW_RAW refusé vient presque toujours d'un sommet que l'application
       a laissé indéfini (NaN, infini, coordonnée démesurée) : GLEngine les
       découpait avant de nous les donner, plus maintenant. Couper le chemin
       brut suffit — l'accélération de la rastérisation, elle, reste bonne. */
    if (pc < CMD_WORDS && QGPU_CMD_OP(G.cmd[pc]) == QGPU_OP_DRAW_RAW) {
        pomppc_log("POMPPC: DRAW_RAW refusé (statut %ld, commande %lu) : "
                   "chemin brut coupé\n", st, pc);
        fprintf(stderr, "POMPPC GL : géométrie brute refusée par l'hôte "
                "(statut %ld), repli sur la rastérisation\n", st);
        for (p = G.list; p; p = p->next)
            p->geom_lost = 1;
        G.v7 = 0;
        return;
    }
    /* Un opcode de requête refusé veut dire que l'hôte ne tient pas
       QGPU_CAP_OCCLUSION (le bit n'est pas encore publié dans QGPU_REG_CAPS,
       cf. docs/protocole-v8 §5 : on le reconnaît au statut). On coupe les
       requêtes, et rien d'autre : le dessin, lui, marche. */
    if (pc < CMD_WORDS && QGPU_CMD_OP(G.cmd[pc]) >= QGPU_OP_QUERY_BEGIN &&
        QGPU_CMD_OP(G.cmd[pc]) <= QGPU_OP_QUERY_RESULT) {
        pomppc_log("POMPPC: opcode de requête refusé (statut %ld) : "
                   "requêtes d'occlusion coupées\n", st);
        qry_off = 1;
        for (p = G.list; p; p = p->next)
            p->q_open = -1;
        return;
    }
    {   /* Dire QUELLE commande, avec ses arguments : sans cela un refus coûte
           un aller-retour dans l'invité pour deviner. */
        unsigned long k, n = 0;
        char buf[160];
        int at = 0;
        if (pc < CMD_WORDS) {
            n = QGPU_CMD_LEN(G.cmd[pc]);
            if (n == 0 || n > 16 || pc + n > CMD_WORDS)
                n = 1;
        }
        for (k = 0; k < n && at < (int)sizeof(buf) - 10; k++)
            at += snprintf(buf + at, sizeof(buf) - at, " %lx", G.cmd[pc + k]);
        buf[at] = 0;
        pomppc_log("POMPPC: commande fautive (op %lx) :%s\n",
                   pc < CMD_WORDS ? (unsigned long)QGPU_CMD_OP(G.cmd[pc]) : 0UL, buf);
    }
    pomppc_log("POMPPC: soumission refusée (%s, statut %ld, commande %lu) : "
               "accélération coupée\n", why, st, pc);
    fprintf(stderr, "POMPPC GL : soumission refusée (statut %ld, commande %lu), "
            "retour au rendu logiciel\n", st, pc);
    for (p = G.list; p; p = p->next)
        p->broken = 1;
}

/* ─────────────────────────── soumission (v9) ───────────────────────────────
 *
 * MODE SYNCHRONE (v1–v8, et repli). `qgpu_submit` ne rend la main qu'une fois
 * la soumission TERMINÉE : les relectures sont là, on les recopie tout de
 * suite, la moitié est libre. Une image = une attente du GPU hôte par
 * soumission.
 *
 * MODE ASYNCHRONE. `qgpu_submit_async` dépose et rend la main. La moitié qui
 * porte le flux, les sommets, les indices et l'arène reste EN VOL jusqu'à ce
 * que FENCE dépasse sa barrière : on n'y touche plus, et on écrit la suite
 * dans l'autre. Il n'y a donc jamais plus d'UNE de nos soumissions en vol, ce
 * qui est exactement ce qu'on cherche — l'hôte dessine l'image n pendant que
 * l'invité prépare la n+1.
 *
 * QUAND ON ATTEND, ET SEULEMENT ALORS :
 *   — au moment de REPRENDRE une moitié (wait_half depuis switch_half) ;
 *   — quand l'invité a BESOIN d'une relecture (sync_to_sw_locked avant un
 *     chemin logiciel, q_info pour un compte d'occlusion) ;
 *   — à la présentation directe, pour l'image PRÉCÉDENTE (present_direct).
 * Une soumission sans relecture — dessins, changements d'état, téléversements,
 * c'est-à-dire l'immense majorité — n'est jamais attendue.
 *
 * ERREURS. Avec plusieurs soumissions en vol, QGPU_REG_STATUS ne décrit que la
 * dernière TERMINÉE : il ne dit plus « tout s'est bien passé ». C'est
 * QGPU_REG_ERRORS qui fait foi, et le kext le rend à chaque soumission
 * asynchrone (à la place de status_pc, qui n'a pas de sens à la soumission).
 * Voir check_errors : on ne peut pas nommer la fautive après coup, on repasse
 * en synchrone pour que la prochaine se nomme elle-même.
 */
#define WAIT_MS   5000                  /* délai maximal d'une barrière */

/* Recopie ce qu'une soumission terminée a déposé dans l'arène. */
static void run_posts(Half *h)
{
    double t;
    int i;

    if (!h->npost)
        return;
    t = now_s();
    for (i = 0; i < h->npost; i++) {
        Post *po = &h->post[i];
        /* arène = offset ABSOLU dans la tranche : la moitié courante a pu
           changer depuis que la relecture a été demandée. */
        unsigned long *src = (unsigned long *)(G.q.win + po->off);
        unsigned long y, x;
        for (y = 0; y < po->h; y++) {
            unsigned long *s = src + y * po->w;
            unsigned char *d = po->dst + y * po->rowbytes;
            if (!po->depth) {
                memcpy(d, s, po->w * 4);
            } else {
                unsigned long *dd = (unsigned long *)d;
                if (po->depth == 2) {           /* stencil : 8 bits bas du mot */
                    for (x = 0; x < po->w; x++)
                        dd[x] = (dd[x] & 0xFFFFFF00UL) | (s[x] & 0xFF);
                } else if (po->packed) {        /* profondeur : 24 bits hauts */
                    for (x = 0; x < po->w; x++) {
                        float f = *(float *)(s + x);
                        unsigned long z = (unsigned long)(clamp01(f) * po->scale + 0.5f);
                        dd[x] = (z & 0xFFFFFF00UL) | (dd[x] & 0xFF);
                    }
                } else {
                    for (x = 0; x < po->w; x++) {
                        float f = *(float *)(s + x);
                        dd[x] = (unsigned long)(clamp01(f) * po->scale + 0.5f);
                    }
                }
            }
        }
    }
    h->npost = 0;
    G.t_copy += now_s() - t;
}

/* Attend la barrière de la moitié i (si elle est en vol), puis fait ses copies
   différées. Après quoi la moitié est libre : on peut la réécrire. */
static void wait_half(int i)
{
    Half *h = &G.h[i];

    if (!h->busy) {
        run_posts(h);                   /* mode synchrone : rien à attendre */
        return;
    }
    {
        double t = now_s();
        int ok = qgpu_wait(&G.q, h->fence, WAIT_MS) == 0;
        G.t_wait += now_s() - t;
        G.n_waits++;
        if (!ok) {
            /* L'hôte ne répond plus. On ne peut pas réécrire une moitié encore
               en vol : on coupe l'accélération pour tout le processus plutôt
               que de dessiner sur ce que l'hôte est en train de lire. */
            PCtx *p;
            pomppc_log("POMPPC: barrière %lu jamais atteinte (%d ms) : "
                       "accélération coupée\n", h->fence, WAIT_MS);
            fprintf(stderr, "POMPPC GL : l'hôte n'a pas terminé une soumission "
                    "en %d ms, retour au rendu logiciel\n", WAIT_MS);
            G.async = 0;
            G.async_avail = 0;
            for (p = G.list; p; p = p->next)
                p->broken = 1;
            h->npost = 0;
            h->busy = 0;
            return;
        }
    }
    h->busy = 0;
    run_posts(h);
}

/* ── détection d'erreur en asynchrone ────────────────────────────────────────
 *
 * QGPU_REG_ERRORS a-t-il bougé ? Le kext le rend à chaque soumission
 * asynchrone, à la place de status_pc — qui n'a pas de sens à la soumission.
 * C'est le seul verdict utilisable : avec plusieurs soumissions en vol,
 * QGPU_REG_STATUS ne décrit que la DERNIÈRE TERMINÉE, qui n'est pas forcément
 * la nôtre.
 *
 * MAIS ERRORS EST GLOBAL AU DEVICE, et il bouge sans que personne n'ait de
 * bogue. Vu en vrai au premier essai : 8 877 erreurs comptées sur une VM qui
 * rend des images justes depuis des heures. C'est le balayage de fermeture du
 * kext (destroyClientObjects détruit les 148 identifiants de la plage d'un
 * client, dont la plupart n'existent pas — une erreur chacun, attendue et sans
 * conséquence). Chaque application GL qui se ferme en ajoute donc ~148.
 *
 * On ne peut ni nommer la soumission fautive après coup (sa moitié a pu être
 * réécrite), ni attribuer le compteur à quelqu'un. D'où la règle, bornée et
 * qui se répare toute seule : ERRORS bouge → on repasse en SYNCHRONE pendant
 * ASYNC_RETRY images. Si l'erreur était la nôtre, elle est déterministe (un
 * sommet indéfini, un opcode que l'hôte ne tient pas, un paramètre hors
 * domaine) : la prochaine image la reproduit, et broken_all a alors SON statut
 * et SON pc, exacts, comme avant la v9. Si elle ne revient pas — c'était le
 * ménage d'un autre client — on reprend l'asynchrone et on se recale.
 */
#define ASYNC_RETRY   120               /* images de synchrone avant de réessayer */

static void check_errors(unsigned long errors)
{
    unsigned long e2 = 0, status = 0, pc = 0;

    if (!G.err_valid) {
        G.errors = errors;
        G.err_valid = 1;
        return;
    }
    if (errors == G.errors)
        return;
    pomppc_log("POMPPC: QGPU_REG_ERRORS %lu → %lu", G.errors, errors);
    G.errors = errors;
    qgpu_peek(&G.q, &e2, &status, &pc);
    pomppc_log(" (dernier statut %lu, commande %lu) : synchrone pendant %d images "
               "pour retrouver la fautive\n", status, pc, ASYNC_RETRY);
    G.async = 0;
    G.n_syncfall++;
    G.async_retry_at = G.n_frames + ASYNC_RETRY;
}

/* Reprend l'asynchrone si la fenêtre de synchrone n'a rien trouvé. */
static void async_rearm(void)
{
    if (G.async || !G.async_avail || !G.async_retry_at)
        return;
    if (G.n_frames < G.async_retry_at)
        return;
    G.async_retry_at = 0;
    G.err_valid = 0;                    /* on se recale sur ERRORS */
    G.async = 1;
    pomppc_log("POMPPC: aucune erreur de notre fait en %d images : "
               "doorbell asynchrone repris\n", ASYNC_RETRY);
}

/* Soumet la moitié courante. Pose sa barrière (asynchrone) ou fait ses copies
   tout de suite (synchrone). */
static void submit_cur(void)
{
    Half *h = &G.h[G.cur];
    double t = now_s();
    unsigned long pc = 0, fence = 0, errors = 0;
    long st;
    /* Pendant un BeginPrimitiveBuffer, GLEngine écrit dans CETTE moitié à une
       adresse qu'on lui a déjà donnée : on ne peut pas en changer, donc pas la
       laisser en vol non plus. Synchrone, comme avant la v9. C'est rare —
       geom_begin fait la place avant d'ouvrir — et c'est la seule façon
       d'être exact (vu en vrai : un téléversement de texture au milieu d'une
       primitive vide le flux). */
    int async = G.async && !G.pend;

    if (async) {
        st = qgpu_submit_async(&G.q, G.hb, G.ncmd * 4, &fence, &errors);
        if (st == QGPU_ST_QUEUE_FULL) {
            /* Rien n'a été mis en file, et SUBMIT_OFF/SUBMIT_LEN se réécrivent
               sans danger : on attend notre plus ancienne barrière (il n'y en a
               qu'une : l'autre moitié) et on réessaie. */
            G.n_qfull++;
            wait_half(G.cur ^ 1);
            st = qgpu_submit_async(&G.q, G.hb, G.ncmd * 4, &fence, &errors);
        }
        if (st != QGPU_ST_OK) {
            /* File toujours pleine (un autre client l'occupe) ou appel refusé :
               le doorbell SYNCHRONE, lui, n'est jamais refusé — il attend sa
               place. C'est le repli le plus simple et le plus sûr. */
            if (st == QGPU_ST_QUEUE_FULL)
                G.n_qfull++;
            async = 0;
        }
    }
    if (async) {
        G.t_submit += now_s() - t;
        G.n_submits++;
        h->fence = fence;
        h->busy = 1;
        check_errors(errors);
        return;
    }
    st = qgpu_submit(&G.q, G.hb, G.ncmd * 4, &pc);
    G.t_submit += now_s() - t;
    G.n_submits++;
    h->busy = 0;
    if (st != QGPU_ST_OK) {
        broken_all("flush", st, pc);
        h->npost = 0;
        return;
    }
    run_posts(h);
}

/* Passe à l'autre moitié, en attendant qu'elle soit libre. */
static void switch_half(void)
{
    if (G.nhalf < 2)
        return;
    G.cur ^= 1;
    G.hb   = (unsigned long)G.cur * G.half;
    G.win  = G.q.win + G.hb;
    G.base = G.q.base + G.hb;
    G.cmd  = (unsigned long *)G.win;
    /* La moitié qu'on reprend peut encore être en vol : contrat mémoire,
       point 1. C'est ici, et seulement ici, que le pipeline se referme. */
    wait_half(G.cur);
}

static void flush(void)
{
    close_run();
    close_raw();
    if (G.ncmd)
        submit_cur();
    G.ncmd = 0;
    G.arena = 0;
    G.bound = 0;
    /* Un BeginPrimitiveBuffer ouvert occupe déjà la zone des sommets : GLEngine
       y écrit pendant ce temps, on ne peut ni la rendre ni changer de moitié
       (submit_cur a soumis en synchrone pour cette raison). */
    if (G.pend)
        return;
    G.vtx = 0;
    G.idx = 0;
    switch_half();
}

/* Attend et recopie tout ce qui reste en vol (fin de contexte, changement de
   tampon) : après quoi l'invité voit tout ce que l'hôte a dessiné. */
static void drain_all(void)
{
    int i;
    for (i = 0; i < HALVES; i++)
        wait_half(i);
}

/* ───────────────────────────── objets qgpu ───────────────────────────── */

static long alloc_id(unsigned long *used, unsigned long base, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++) {
        if (!(*used & (1UL << i))) {
            *used |= 1UL << i;
            return base + i;
        }
    }
    return -1;
}

static long alloc_tex_id(void)
{
    unsigned long i;
    for (i = 0; i < QGPU_CLIENT_TEX_IDS; i++) {
        if (!(G.tex_used[i / 32] & (1UL << (i % 32)))) {
            G.tex_used[i / 32] |= 1UL << (i % 32);
            return G.q.tex_base + i;
        }
    }
    return -1;
}

/* Textures par adresse d'objet du GLDriver. Une recherche par unité de
 * texture et par dessin : une liste suffisait aux tests, pas à un jeu qui
 * garde des centaines de textures (vu en vrai : 8 % du temps de Zenerchi). */
#define TEX_HASH 512
static PTex *tex_hash[TEX_HASH];

static unsigned long tex_bucket(void *drvtex)
{
    unsigned long a = (unsigned long)drvtex;
    return ((a >> 4) ^ (a >> 13)) & (TEX_HASH - 1);
}

static PTex *find_tex(void *drvtex)
{
    PTex *t;
    for (t = tex_hash[tex_bucket(drvtex)]; t; t = t->hnext)
        if (t->drvtex == drvtex)
            return t;
    return 0;
}

void pomppc_texture_created(void *drvtex)
{
    PTex *t;
    if (G.state <= 0 || !drvtex)
        return;
    t = calloc(1, sizeof(*t));
    if (!t)
        return;
    t->drvtex = drvtex;
    t->qtex = -1;
    t->dirty = 1;
    pthread_mutex_lock(&G.mu);
    t->next = G.textures;
    G.textures = t;
    t->hnext = tex_hash[tex_bucket(drvtex)];
    tex_hash[tex_bucket(drvtex)] = t;
    pthread_mutex_unlock(&G.mu);
}

/* La série et le flux en cours peuvent référencer la texture : on vide d'abord. */
void pomppc_texture_deleted(void *drvtex)
{
    PTex **pp, *t;
    unsigned long *c;
    if (G.state <= 0)
        return;
    pthread_mutex_lock(&G.mu);
    for (pp = &G.textures; *pp; pp = &(*pp)->next) {
        if ((*pp)->drvtex != drvtex)
            continue;
        t = *pp;
        *pp = t->next;
        {
            PTex **hp = &tex_hash[tex_bucket(drvtex)];
            while (*hp && *hp != t)
                hp = &(*hp)->hnext;
            if (*hp)
                *hp = t->hnext;
        }
        if (t->qtex >= 0) {
            unsigned long i = t->qtex - G.q.tex_base;
            close_run();
            if (G.ncmd + QGPU_LEN_TEX > CMD_WORDS)
                flush();
            c = G.cmd + G.ncmd;
            c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_DESTROY, QGPU_LEN_TEX);
            c[1] = t->qtex;
            G.ncmd += QGPU_LEN_TEX;
            flush();
            G.tex_used[i / 32] &= ~(1UL << (i % 32));
        }
        free(t);
        break;
    }
    pthread_mutex_unlock(&G.mu);
}

void pomppc_texture_changed(void *drvtex, int levels)
{
    PTex *t;
    if (G.state <= 0)
        return;
    pthread_mutex_lock(&G.mu);
    t = find_tex(drvtex);
    if (t && levels)
        t->dirty = 1;
    pthread_mutex_unlock(&G.mu);
}

/* Couples (format, type) que convert_level sait traduire. Le chemin brut doit
 * le savoir SANS convertir : le domaine est décidé à chaque changement d'état,
 * bien avant le dessin. Toute entrée ajoutée ici doit avoir son cas ci-dessous. */
static int level_convertible(unsigned int fmt, unsigned int type)
{
    switch ((fmt << 16) | type) {
    case (0x1908 << 16) | 0x1401: case (0x1907 << 16) | 0x1401:
    case (0x80E1 << 16) | 0x1401: case (0x80E0 << 16) | 0x1401:
    case (0x80E1 << 16) | 0x8367: case (0x1908 << 16) | 0x8035:
    case (0x80E1 << 16) | 0x8035: case (0x1908 << 16) | 0x8367:
    case (0x80E1 << 16) | 0x8366: case (0x1907 << 16) | 0x8363:
    case (0x1908 << 16) | 0x8033: case (0x1909 << 16) | 0x1401:
    case (0x190A << 16) | 0x1401: case (0x1906 << 16) | 0x1401:
    case (0x1903 << 16) | 0x1401:
        return 1;
    default:
        return 0;
    }
}

/* Convertit un niveau (format de l'application) en mots ARGB ; 0 si non géré. */
static int convert_level(const unsigned char *lv, unsigned long *out)
{
    unsigned long w = S16(lv, LV_W), h = S16(lv, LV_H), n = w * h, i;
    const unsigned char *d = (const unsigned char *)GLD_U32(lv, LV_DATA);
    unsigned int fmt = U16(lv, LV_FORMAT), type = U16(lv, LV_TYPE);

    if (!d || S16(lv, LV_ROWPIX) != (short)w)
        return 0;
    switch ((fmt << 16) | type) {
    case (0x1908 << 16) | 0x1401:                        /* RGBA, octets */
        for (i = 0; i < n; i++, d += 4)
            out[i] = ((unsigned long)d[3] << 24) | (d[0] << 16) | (d[1] << 8) | d[2];
        return 1;
    case (0x1907 << 16) | 0x1401:                        /* RGB */
        for (i = 0; i < n; i++, d += 3)
            out[i] = 0xFF000000UL | (d[0] << 16) | (d[1] << 8) | d[2];
        return 1;
    case (0x80E1 << 16) | 0x1401:                        /* BGRA */
        for (i = 0; i < n; i++, d += 4)
            out[i] = ((unsigned long)d[3] << 24) | (d[2] << 16) | (d[1] << 8) | d[0];
        return 1;
    case (0x80E0 << 16) | 0x1401:                        /* BGR */
        for (i = 0; i < n; i++, d += 3)
            out[i] = 0xFF000000UL | (d[2] << 16) | (d[1] << 8) | d[0];
        return 1;
    case (0x80E1 << 16) | 0x8367:                        /* BGRA, UINT_8_8_8_8_REV : ARGB BE */
        memcpy(out, d, n * 4);
        return 1;
    case (0x1908 << 16) | 0x8035: {                      /* RGBA, UINT_8_8_8_8 */
        const unsigned long *wd = (const unsigned long *)d;
        for (i = 0; i < n; i++)
            out[i] = (wd[i] >> 8) | ((wd[i] & 0xFF) << 24);
        return 1;
    }
    case (0x80E1 << 16) | 0x8035: {                      /* BGRA, UINT_8_8_8_8 (Zenerchi) */
        const unsigned long *wd = (const unsigned long *)d;
        for (i = 0; i < n; i++) {
            unsigned long v = wd[i];                     /* B G R A → A R G B */
            out[i] = (v >> 24) | ((v >> 8) & 0xFF00) | ((v & 0xFF00) << 8) | (v << 24);
        }
        return 1;
    }
    case (0x1908 << 16) | 0x8367: {                      /* RGBA, UINT_8_8_8_8_REV */
        const unsigned long *wd = (const unsigned long *)d;
        for (i = 0; i < n; i++) {
            unsigned long v = wd[i];                     /* A B G R → A R G B */
            out[i] = (v & 0xFF00FF00UL) | ((v & 0xFF) << 16) | ((v >> 16) & 0xFF);
        }
        return 1;
    }
    case (0x80E1 << 16) | 0x8366: {                      /* BGRA, USHORT_1_5_5_5_REV (ARGB1555) */
        const unsigned short *wd = (const unsigned short *)d;
        for (i = 0; i < n; i++) {
            unsigned long v = wd[i];
            unsigned long r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
            out[i] = ((v & 0x8000) ? 0xFF000000UL : 0) | (((r << 3) | (r >> 2)) << 16) |
                     (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
        }
        return 1;
    }
    case (0x1907 << 16) | 0x8363: {                      /* RGB, USHORT_5_6_5 */
        const unsigned short *wd = (const unsigned short *)d;
        for (i = 0; i < n; i++) {
            unsigned long v = wd[i];
            unsigned long r = v >> 11, g = (v >> 5) & 63, b = v & 31;
            out[i] = 0xFF000000UL | (((r << 3) | (r >> 2)) << 16) |
                     (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
        }
        return 1;
    }
    case (0x1908 << 16) | 0x8033: {                      /* RGBA, USHORT_4_4_4_4 */
        const unsigned short *wd = (const unsigned short *)d;
        for (i = 0; i < n; i++) {
            unsigned long v = wd[i];
            out[i] = (((v & 15) * 17) << 24) | (((v >> 12) * 17) << 16) |
                     ((((v >> 8) & 15) * 17) << 8) | (((v >> 4) & 15) * 17);
        }
        return 1;
    }
    case (0x1909 << 16) | 0x1401:                        /* LUMINANCE */
        for (i = 0; i < n; i++)
            out[i] = 0xFF000000UL | (d[i] * 0x010101UL);
        return 1;
    case (0x190A << 16) | 0x1401:                        /* LUMINANCE_ALPHA */
        for (i = 0; i < n; i++, d += 2)
            out[i] = ((unsigned long)d[1] << 24) | (d[0] * 0x010101UL);
        return 1;
    case (0x1906 << 16) | 0x1401:                        /* ALPHA */
        for (i = 0; i < n; i++)
            out[i] = (unsigned long)d[i] << 24;
        return 1;
    case (0x1903 << 16) | 0x1401:                        /* RED */
        for (i = 0; i < n; i++)
            out[i] = 0xFF000000UL | ((unsigned long)d[i] << 16);
        return 1;
    default:
        return 0;
    }
}

static int base_format_ok(unsigned long f)
{
    return (f >= 0x1906 && f <= 0x190A) || f == 0x8049;
}

/* Les quatre paramètres que le plugin transmet tels quels (QGPU_TP_*). Le cœur
 * n'accepte que GL_REPEAT, GL_CLAMP et GL_CLAMP_TO_EDGE pour la répétition, et
 * les six filtres d'OpenGL : tout le reste ferait REFUSER LA SOUMISSION, donc
 * couper l'accélération pour tout le processus.
 *
 * VU EN VRAI (scène « v15 ») : un seul glTexParameteri(GL_TEXTURE_WRAP_S,
 * GL_MIRRORED_REPEAT) — une extension que ni GLEngine ni nous ne tenons, mais
 * qu'une application a le droit de DEMANDER — suffisait à faire rejeter le
 * flux et à faire retomber tout le processus sur le rendu logiciel. Un refus
 * propre (repli sur Apple pour cette texture) coûte infiniment moins. */
static int tex_filter_ok(unsigned long f)
{
    return f == 0x2600 || f == 0x2601 || (f >= 0x2700 && f <= 0x2703);
}

static int tex_wrap_ok(unsigned long w)
{
    return w == 0x2900 || w == 0x2901 || w == 0x812F;
}

static int tex_params_ok(const unsigned char *gp)
{
    return tex_filter_ok(U16(gp, TP_MIN)) && tex_filter_ok(U16(gp, TP_MAG)) &&
           tex_wrap_ok(U16(gp, TP_WRAP_S)) && tex_wrap_ok(U16(gp, TP_WRAP_T));
}

static int tex_id_available(void)
{
    unsigned long i;
    for (i = 0; i < QGPU_CLIENT_TEX_IDS; i++)
        if (!(G.tex_used[i / 32] & (1UL << (i % 32))))
            return 1;
    return 0;
}

/* upload_texture réussira-t-il ? Prédicat PUR (aucune commande, aucune
 * conversion) : le chemin brut doit trancher au changement d'état, où il ne
 * peut plus se dédire. Une texture déjà téléversée et propre est connue bonne. */
static int texture_uploadable(PTex *t)
{
    unsigned char *dt = t->drvtex;
    unsigned long base = GLD_U32(dt, DT_BASE_FORMAT);
    int l;

    if (!GLD_U32(dt, DT_PARAMS) || !base_format_ok(base))
        return no(NO_TEX_BASE, base, 0);
    if (!tex_params_ok((const unsigned char *)GLD_U32(dt, DT_PARAMS)))
        return no(NO_TEX_PARAM, U16((unsigned char *)GLD_U32(dt, DT_PARAMS), TP_WRAP_S),
                  U16((unsigned char *)GLD_U32(dt, DT_PARAMS), TP_MIN));
    if (t->qtex >= 0 && !t->dirty)
        return 1;
    if (t->qtex < 0 && !tex_id_available())
        return no(NO_TEX_ID, 0, 0);
    for (l = 0; l < DT_LEVELS; l++) {
        unsigned char *lv = dt + DT_LEVEL0 + l * DT_LEVEL_SIZE;
        unsigned long w = S16(lv, LV_W), h = S16(lv, LV_H);
        if (w == 0 || h == 0)
            continue;
        if (S16(lv, LV_BORDER) || w > QGPU_MAX_TEX_DIM || h > QGPU_MAX_TEX_DIM)
            return no(NO_TEX_SIZE, w, h);
        if (!GLD_U32(lv, LV_DATA) || S16(lv, LV_ROWPIX) != (short)w ||
            !level_convertible(U16(lv, LV_FORMAT), U16(lv, LV_TYPE)))
            return no(NO_TEX_FORMAT, (U16(lv, LV_FORMAT) << 16) | U16(lv, LV_TYPE),
                      ((unsigned long)S16(lv, LV_ROWPIX) << 16) | w);
    }
    return 1;
}

/* Téléverse les niveaux et les paramètres de t si besoin ; 0 = logiciel. */
static int upload_texture(PCtx *p, PTex *t)
{
    unsigned char *dt = t->drvtex;
    unsigned char *gp = (unsigned char *)GLD_U32(dt, DT_PARAMS);
    unsigned long base = GLD_U32(dt, DT_BASE_FORMAT), prm[4], off, *c;
    int l, k;

    if (gp && base_format_ok(base) && !tex_params_ok(gp))
        return no(NO_TEX_PARAM, U16(gp, TP_WRAP_S), U16(gp, TP_MIN));
    if (!gp || !base_format_ok(base)) {
        const unsigned char *lv0 = dt + DT_LEVEL0;
        unsigned long lmask = 0;
        int li;
        for (li = 0; li < DT_LEVELS; li++) {
            const unsigned char *lv = dt + DT_LEVEL0 + li * DT_LEVEL_SIZE;
            if (S16(lv, LV_W) && GLD_U32(lv, LV_DATA))
                lmask |= 1UL << li;
        }
        return no(NO_TEX_BASE, base | (lmask << 16),
                  ((unsigned long)(unsigned short)S16(lv0, LV_W) << 16) |
                  (unsigned short)S16(lv0, LV_H));
    }
    if (t->qtex < 0) {
        t->qtex = alloc_tex_id();
        if (t->qtex < 0)
            return no(NO_TEX_ID, 0, 0);
        c = reserve(p, QGPU_LEN_TEX);
        c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX);
        c[1] = t->qtex;
        t->dirty = 1;
        t->prm_valid = 0;
    }
    if (t->dirty) {
        for (l = 0; l < DT_LEVELS; l++) {
            unsigned char *lv = dt + DT_LEVEL0 + l * DT_LEVEL_SIZE;
            unsigned long w = S16(lv, LV_W), h = S16(lv, LV_H);
            if (w == 0 || h == 0)
                continue;
            if (S16(lv, LV_BORDER) || w > QGPU_MAX_TEX_DIM || h > QGPU_MAX_TEX_DIM ||
                !arena_alloc(w * h * 4, &off))
                return no(NO_TEX_SIZE, w, h);
            if (!convert_level(lv, (unsigned long *)(G.q.win + off)))
                return no(NO_TEX_FORMAT, (U16(lv, LV_FORMAT) << 16) | U16(lv, LV_TYPE),
                          ((unsigned long)S16(lv, LV_ROWPIX) << 16) | w);
            c = reserve(p, QGPU_LEN_TEX_IMAGE);
            c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE);
            c[1] = t->qtex; c[2] = l; c[3] = w; c[4] = h; c[5] = base;
            c[6] = G.q.base + off;
            G.n_texuploads++;
        }
        t->dirty = 0;
    }
    prm[0] = U16(gp, TP_MIN);
    prm[1] = U16(gp, TP_MAG);
    prm[2] = U16(gp, TP_WRAP_S);
    prm[3] = U16(gp, TP_WRAP_T);
    for (k = 0; k < 4; k++) {
        if (t->prm_valid && t->prm[k] == prm[k])
            continue;
        c = reserve(p, QGPU_LEN_TEX_PARAM);
        c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_PARAM, QGPU_LEN_TEX_PARAM);
        c[1] = t->qtex; c[2] = QGPU_TP_MIN_FILTER + k; c[3] = prm[k];
        t->prm[k] = prm[k];
    }
    t->prm_valid = 1;
    return 1;
}

/* ─────────────────────── GL_COMBINE → état qgpu (v5) ───────────────────────
 * GLEngine range les paramètres de combinaison dans le bloc de l'unité
 * (offsets TU_*, relevés par la scène « combprobe » de guest/gltest).
 * Les valeurs hors du domaine du device (fonction inconnue, source croisée
 * d'une autre unité, échelle autre que 1, 2 ou 4) font retomber le dessin sur
 * le rendu d'Apple, comme tout le reste. */
static int combine_fn_code(unsigned long e, int rgb)
{
    switch (e) {
    case 0x1E01: return QGPU_CB_REPLACE;
    case 0x2100: return QGPU_CB_MODULATE;
    case 0x0104: return QGPU_CB_ADD;
    case 0x8574: return QGPU_CB_ADD_SIGNED;
    case 0x8575: return QGPU_CB_INTERPOLATE;
    case 0x84E7: return QGPU_CB_SUBTRACT;
    case 0x86AE: case 0x8740: return rgb ? QGPU_CB_DOT3_RGB : -1;
    case 0x86AF: case 0x8741: return rgb ? QGPU_CB_DOT3_RGBA : -1;
    default:     return -1;
    }
}

static int combine_src_code(unsigned long e, int unit)
{
    switch (e) {
    case 0x1702: return QGPU_CS_TEXTURE;          /* GL_TEXTURE */
    case 0x8576: return QGPU_CS_CONSTANT;
    case 0x8577: return QGPU_CS_PRIMARY;
    case 0x8578: return QGPU_CS_PREVIOUS;
    default:
        /* GL_TEXTUREn (crossbar) : accepté seulement pour sa propre unité */
        if (e >= 0x84C0 && e < 0x84C0 + GL_MAX_TEXUNITS)
            return (int)(e - 0x84C0) == unit ? QGPU_CS_TEXTURE : -1;
        return -1;
    }
}

static int combine_scale_code(float f)
{
    if (f > 0.5f && f < 1.5f) return 0;
    if (f > 1.5f && f < 2.5f) return 1;
    if (f > 3.5f && f < 4.5f) return 2;
    return -1;
}

/* Remplit tu->combine et tu->combine_src ; 0 si l'unité sort du domaine. */
static int combine_ok(const unsigned char *us, int unit, TexUnit *tu)
{
    int frgb = combine_fn_code(U16(us, TU_COMBINE_RGB), 1);
    int fa = combine_fn_code(U16(us, TU_COMBINE_A), 0);
    int rs = combine_scale_code(GLD_F32(us, TU_RGB_SCALE));
    int as = combine_scale_code(GLD_F32(us, TU_ALPHA_SCALE));
    unsigned long src = 0;
    int i;

    if (frgb < 0 || fa < 0 || rs < 0 || as < 0)
        return no(NO_TEX_COMBINE, (U16(us, TU_COMBINE_RGB) << 16) | U16(us, TU_COMBINE_A), unit);
    for (i = 0; i < 3; i++) {
        int sr = combine_src_code(U16(us, TU_SRC0_RGB + 2 * i), unit);
        int sa = combine_src_code(U16(us, TU_SRC0_A + 2 * i), unit);
        unsigned long orgb = U16(us, TU_OP0_RGB + 2 * i);
        unsigned long oa = U16(us, TU_OP0_A + 2 * i);
        if (sr < 0 || sa < 0 || orgb < 0x300 || orgb > 0x303 || (oa != 0x302 && oa != 0x303))
            return no(NO_TEX_COMBINE, (orgb << 16) | oa, unit);
        src |= QGPU_COMBINE_SRC_RGB(i, sr, orgb - 0x300);
        src |= QGPU_COMBINE_SRC_A(i, sa, oa == 0x303 ? QGPU_CA_ONE_MINUS_ALPHA : QGPU_CA_ALPHA);
    }
    tu->combine = QGPU_COMBINE(frgb, fa, rs, as);
    tu->combine_src = src;
    return 1;
}

/* Objet texture du GLDriver lié à l'unité u pour la cible active, ou 0.
 * `*mask` reçoit les cibles activées (bits TU_ENABLE). */
static void *unit_drvtex(PCtx *p, int u, unsigned long *mask)
{
    unsigned char *us = gls(p) + GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE;
    unsigned long m = GLD_U32(us, TU_ENABLE) & 0x1f;
    unsigned long units = GLD_U32(p->ctx, CTX_TEXUNITS);
    *mask = m;
    if (!m || (m & 0x7) || !units)
        return 0;
    return (void *)GLD_U32(units, u * 0x14 + ((m & 8) ? 3 * 4 : 4 * 4));
}

/* Unité u : texture et environnement ; 0 = hors domaine (logiciel). */
static int texture_unit_ok(PCtx *p, int u, TexUnit *tu)
{
    unsigned char *g = gls(p);
    unsigned char *us = g + GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE;
    unsigned long mask = GLD_U32(us, TU_ENABLE) & 0x1f;
    unsigned long units = GLD_U32(p->ctx, CTX_TEXUNITS);
    unsigned long env = U16(us, TU_ENV_MODE);
    const float *ec;
    void *dt;

    tu->t = 0;
    if (!mask)
        return 1;                                   /* unité coupée */
    if ((mask & 0x7) || !units)                      /* cube, 3D, rectangle */
        return no(NO_TEX_TARGET, mask, units);
    if (env != 0x2100 && env != 0x2101 && env != 0x0BE2 && env != 0x1E01 &&
        env != 0x0104 && env != 0x8570)
        return no(NO_TEX_ENV, env, u);
    tu->combine = QGPU_COMBINE_DEFAULT;
    tu->combine_src = QGPU_COMBINE_SRC_DEFAULT;
    if (env == 0x8570 && !combine_ok(us, u, tu))
        return 0;
    dt = (void *)GLD_U32(units, u * 0x14 + ((mask & 8) ? 3 * 4 : 4 * 4));
    tu->t = dt ? find_tex(dt) : 0;
    if (!tu->t)
        return no(NO_TEX_UNKNOWN, (unsigned long)dt, mask);
    /* Texture sans image (jamais définie) : OpenGL la dit incomplète et coupe
       le texturage de CETTE unité, sans toucher aux autres. Marble Blast
       laisse ainsi des unités actives sans texture. */
    if (!S16((unsigned char *)tu->t->drvtex + DT_LEVEL0, LV_W) ||
        !GLD_U32((unsigned char *)tu->t->drvtex + DT_LEVEL0, LV_DATA)) {
        tu->t = 0;
        G.n_tex_incomplete++;
        return 1;
    }
    if (!upload_texture(p, tu->t))
        return 0;
    ec = (const float *)(us + TU_ENV_COLOR);
    tu->env_mode = env;
    tu->env_color = (to_u8(ec[3]) << 24) | (to_u8(ec[0]) << 16) | (to_u8(ec[1]) << 8) | to_u8(ec[2]);
    return 1;
}

/* Le texturage en cours relève-t-il du domaine accéléré ? Remplit ti. */
static int texture_ok(PCtx *p, TexInfo *ti)
{
    unsigned char *g = gls(p);
    int i;

    for (i = 0; i < QGPU_MAX_UNITS; i++)
        ti->u[i].t = 0;
    if (!GLD_U8(p->ctx, CTX_TEXTURING))
        return 1;                                   /* pas de texture : dessin simple */
    for (i = QGPU_MAX_UNITS; i < GL_MAX_TEXUNITS; i++)
        if (GLD_U32(g, GS_TEXUNIT0 + i * GS_TEXUNIT_SIZE + TU_ENABLE) & 0x1f)
            return no(NO_TEX_UNITS, i, 0);          /* 5 unités ou plus : logiciel */
    for (i = 0; i < QGPU_MAX_UNITS; i++)
        if (!texture_unit_ok(p, i, &ti->u[i]))
            return 0;
    return 1;
}

static void destroy_surface(PCtx *p)
{
    unsigned long *c;
    if (p->surf < 0)
        return;
    c = reserve(p, QGPU_LEN_SURF);
    c[0] = QGPU_CMD_HDR(QGPU_OP_SURF_DESTROY, QGPU_LEN_SURF);
    c[1] = p->surf;
    G.surf_used &= ~(1UL << (p->surf - G.q.surf_base));
    p->surf = -1;
}

/* Surface hôte à la taille du drawable ; 0 si impossible. */
static int ensure_surface(PCtx *p)
{
    unsigned long w = GLD_U32(p->ctx, CTX_WIDTH), h = GLD_U32(p->ctx, CTX_HEIGHT);
    unsigned long *c;
    int want_stencil;

    if (p->qctx < 0 || w == 0 || h == 0 || w > QGPU_MAX_SURF_DIM || h > QGPU_MAX_SURF_DIM)
        return 0;
    /* stencil de 8 bits logé dans une profondeur de 32 bits (cas du GLDriver d'Apple) */
    /* (le tampon invité lui-même n'est alloué par Apple qu'à son premier usage :
       ne pas l'exiger ici — la synchronisation saute un tampon absent) */
    want_stencil = GLD_U32(p->ctx, CTX_STENCIL_BITS) == 8 &&
                   GLD_U32(p->ctx, CTX_DEPTH_BITS) == 32;
    if (p->surf >= 0 && p->sw == w && p->sh == h && p->stencil == want_stencil)
        return 1;
    destroy_surface(p);
    p->surf = alloc_id(&G.surf_used, G.q.surf_base, QGPU_CLIENT_SURF_IDS);
    if (p->surf < 0)
        return 0;
    c = reserve(p, QGPU_LEN_SURF_CREATE + QGPU_LEN_SURF);
    c[0] = QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE);
    c[1] = p->surf;
    c[2] = w;
    c[3] = h;
    c[4] = QGPU_FMT_XRGB8888 | QGPU_FMT_FLAG_DEPTH | (want_stencil ? QGPU_FMT_FLAG_STENCIL : 0);
    p->stencil = want_stencil;
    c[5] = QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF);
    c[6] = p->surf;
    p->sw = w;
    p->sh = h;
    p->color = SW_NEWER;
    p->depth = SW_NEWER;
    p->draw_seen = sw_color(p);
    return 1;
}

/* ───────────────────────────── synchronisation ───────────────────────────── */

static void sync_to_host(PCtx *p, int color, int depth)
{
    unsigned long off, y, x, *c;
    unsigned long w = p->sw, h = p->sh;

    double t0 = now_s();
    if (color && p->color == SW_NEWER) {
        unsigned char *src = sw_color(p);
        if (src && arena_alloc(w * h * 4, &off)) {
            for (y = 0; y < h; y++)
                memcpy(G.q.win + off + y * w * 4, src + y * sw_rowbytes(p), w * 4);
            c = reserve(p, QGPU_LEN_SURF_XFER);
            c[0] = QGPU_CMD_HDR(QGPU_OP_SURF_UPLOAD, QGPU_LEN_SURF_XFER);
            c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
            c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
            G.n_uploads++;
        }
        p->color = SYNCED;
    }
    if (depth && p->depth == SW_NEWER) {
        unsigned char *src = sw_depth(p);
        float inv = 1.0f / GLD_F32(p->ctx, CTX_DEPTH_SCALE);
        if (src && arena_alloc(w * h * 4, &off)) {
            for (y = 0; y < h; y++) {
                unsigned long *s = (unsigned long *)(src + y * sw_rowbytes(p));
                float *d = (float *)(G.q.win + off + y * w * 4);
                if (p->stencil) {
                    for (x = 0; x < w; x++)
                        d[x] = clamp01((s[x] & 0xFFFFFF00UL) * inv);
                } else {
                    for (x = 0; x < w; x++)
                        d[x] = clamp01(s[x] * inv);
                }
            }
            c = reserve(p, QGPU_LEN_SURF_XFER);
            c[0] = QGPU_CMD_HDR(QGPU_OP_DEPTH_UPLOAD, QGPU_LEN_SURF_XFER);
            c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
            c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
            G.n_uploads++;
        }
        /* Le stencil de l'invité vit dans les 8 bits bas des mêmes mots. On ne
           le téléverse QUE si le contexte s'en sert vraiment : sur une surface
           hôte combinée, QGPU_OP_STENCIL_UPLOAD abîme la PROFONDEUR déjà posée
           (vu en vrai, scène « mixte » avec un format de pixel à stencil : tout
           ce qui suivait le premier repli logiciel disparaissait ; le seul fait
           de sauter ce téléversement rend l'image exacte). Beaucoup
           d'applications — GLUT, Marble Blast — demandent un stencil sans
           jamais s'en servir : elles ne paient plus ni le bogue ni le transfert. */
        if (src && p->stencil && p->sten_used && arena_alloc(w * h * 4, &off)) {
            for (y = 0; y < h; y++) {
                unsigned long *s = (unsigned long *)(src + y * sw_rowbytes(p));
                unsigned long *d = (unsigned long *)(G.q.win + off + y * w * 4);
                for (x = 0; x < w; x++)
                    d[x] = s[x] & 0xFF;
            }
            c = reserve(p, QGPU_LEN_SURF_XFER);
            c[0] = QGPU_CMD_HDR(QGPU_OP_STENCIL_UPLOAD, QGPU_LEN_SURF_XFER);
            c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
            c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
            G.n_uploads++;
        }
        p->depth = SYNCED;
    }
    G.t_upload += now_s() - t0;
}

static void queue_readback_to(PCtx *p, int depth, unsigned char *dst, unsigned long rowbytes)
{
    unsigned long off, *c;
    unsigned long w = p->sw, h = p->sh;
    if (!arena_alloc(w * h * 4, &off))
        return;
    c = reserve(p, QGPU_LEN_SURF_XFER);
    c[0] = QGPU_CMD_HDR(depth == 2 ? QGPU_OP_STENCIL_READBACK :
                        depth ? QGPU_OP_DEPTH_READBACK : QGPU_OP_SURF_READBACK,
                        QGPU_LEN_SURF_XFER);
    c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
    c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
    {   /* La copie appartient à la MOITIÉ qui porte la soumission : elle ne se
           fera qu'une fois sa barrière atteinte (contrat mémoire, point 2 —
           avant, le contenu de l'arène est indéterminé). */
        Half *hf = &G.h[G.cur];
        Post *po = &hf->post[hf->npost];
        po->depth = depth;
        po->packed = p->stencil;
        po->off = off;
        po->dst = dst;
        po->w = w;
        po->h = h;
        po->rowbytes = rowbytes;
        po->scale = GLD_F32(p->ctx, CTX_DEPTH_SCALE);
        hf->npost++;
    }
    G.n_readbacks++;
}

static void queue_readback(PCtx *p, int depth, unsigned char *dst)
{
    queue_readback_to(p, depth, dst, sw_rowbytes(p));
}

/* Recopie dans le tampon invité ce que l'hôte a dessiné (verrou tenu).
 * La profondeur n'est relue que si `want_depth` : un échange ou un vidage n'en
 * a pas besoin, et la relire à chaque image coûtait autant que la couleur. */
static void sync_to_sw_locked(PCtx *p, int want_depth)
{
    int any = 0;
    if (p->surf < 0)
        return;
    if (p->color == HOST_NEWER) {
        unsigned char *dst = p->draw_seen;
        if (dst) {
            queue_readback(p, 0, dst);
            any = 1;
        }
        p->color = SYNCED;
    }
    if (want_depth && p->depth == HOST_NEWER) {
        unsigned char *dst = sw_depth(p);
        if (dst && GLD_U32(p->ctx, CTX_DEPTH_BITS) == 32) {
            queue_readback(p, 1, dst);
            if (p->stencil && p->sten_used)
                queue_readback(p, 2, dst);
            any = 1;
            p->depth = SYNCED;
        }
        /* Pas (encore) de tampon de profondeur invité : le rendu d'Apple ne
           l'alloue qu'à son premier usage, et plus tard encore quand il porte
           un stencil. On garde alors HOST_NEWER : se dire « synchronisé » ici,
           c'était perdre la profondeur de l'hôte — le repli logiciel allouait
           ensuite un tampon au contenu indéfini, marqué SW_NEWER, téléversé
           par-dessus, et tout ce que l'hôte dessinait après était éliminé par
           le test de profondeur. Vu en vrai : scène « mixte » avec un format de
           pixel à stencil, toute la géométrie postérieure au premier repli
           disparaissait. */
    }
    if (any) {
        /* C'est LE point où l'invité a besoin du résultat : le chemin logiciel
           qui suit va lire ces pixels. On soumet, puis on attend la barrière de
           cette soumission-là et on fait ses copies. Ailleurs, on ne les attend
           jamais — c'est tout l'intérêt du mode asynchrone. */
        int i = G.cur;
        flush();
        wait_half(i);
    } else if (G.ncmd) {
        flush();
    }
}

static void sync_to_sw(void *ctx, int want_depth)
{
    PCtx *p;
    if (G.state <= 0)
        return;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p)
        sync_to_sw_locked(p, want_depth);
    pthread_mutex_unlock(&G.mu);
}

/* gldFlush / gldFinish : l'application attend l'image, pas la profondeur. */
void pomppc_sync_to_sw(void *ctx)
{
    sync_to_sw(ctx, 0);
}

/* Le tampon de dessin a-t-il changé d'adresse (glDrawBuffer, réallocation) ? */
static void check_draw_buffer(PCtx *p)
{
    unsigned char *cur = sw_color(p);
    if (cur != p->draw_seen) {
        /* l'ancien tampon a été servi par sync_to_sw avant le changement */
        p->draw_seen = cur;
        p->color = SW_NEWER;
    }
}

/* ───────────────────────────── état GL ───────────────────────────── */

static int blend_factor_ok(unsigned long f)
{
    /* v8 : les quatre facteurs à couleur constante (GL_CONSTANT_COLOR 0x8001 …
       GL_ONE_MINUS_CONSTANT_ALPHA 0x8004) passent dans les clés que le plugin
       remplissait déjà ; seule la couleur est une clé neuve. */
    return f <= 1 || (f >= 0x300 && f <= 0x308) ||
           (G.v8 && f >= QGPU_BF_CONSTANT_COLOR && f <= QGPU_BF_ONE_MINUS_CONSTANT_ALPHA);
}

static int blend_eq_ok(unsigned long e)
{
    /* v8 : GL_MIN et GL_MAX. Rappel de la spécification, tenu par l'hôte :
       avec eux les FACTEURS SONT IGNORÉS (docs/protocole-v8 §1). */
    return e == QGPU_BEQ_ADD || e == QGPU_BEQ_SUBTRACT || e == QGPU_BEQ_REVERSE_SUBTRACT ||
           (G.v8 && (e == QGPU_BEQ_MIN || e == QGPU_BEQ_MAX));
}

static int logic_op_ok(unsigned long m)
{
    return m >= QGPU_LO_CLEAR && m <= QGPU_LO_SET;
}

static int poly_mode_ok(unsigned long m)
{
    return m == QGPU_POLY_POINT || m == QGPU_POLY_LINE || m == QGPU_POLY_FILL;
}

static int stencil_op_ok(unsigned long op)
{
    return op == 0 || op == 0x150A || (op >= 0x1E00 && op <= 0x1E03) ||
           op == 0x8507 || op == 0x8508;
}

/* Test de stencil actif ET tampon présent (sinon le test n'a aucun effet).
 * Note au passage que le contexte se sert du stencil : tant qu'il ne s'en sert
 * pas, on n'échange pas le stencil avec l'hôte (voir sync_to_host). */
static int stencil_active(PCtx *p)
{
    int on = (GLD_U32(gls(p), GS_STENCIL) & 1) && GLD_U32(p->ctx, CTX_STENCIL_BITS) != 0;
    if (on)
        p->sten_used = 1;
    return on;
}

/* L'état courant relève-t-il du domaine rendu par l'hôte ?
 * `raw` : on juge pour le chemin BRUT (v7), qui calcule le brouillard sur
 * l'hôte — GL_NICEST n'est donc plus une raison de refuser. */
static int accel_ok_for(PCtx *p, int raw)
{
    unsigned char *g;

    if (G.state <= 0 || p->broken || p->qctx < 0)
        return 0;
    g = gls(p);
    if (!g || !sw_color(p) || GLD_U32(p->ctx, CTX_COLOR_BITS) != 32 ||
        GLD_U32(p->ctx, CTX_ROWPIX) < GLD_U32(p->ctx, CTX_WIDTH))
        return no(NO_BUFFER, GLD_U32(p->ctx, CTX_COLOR_BITS), GLD_U32(p->ctx, CTX_ROWPIX));
    /* v8 : l'opération logique et le pointillé de polygone sont des étages de
       FRAGMENT — ils agissent après l'endroit où les deux chemins de dessin se
       rejoignent, donc ils valent pour le chemin brut comme pour l'ancien. Le
       lissage de polygone, lui, reste hors périmètre (protocole v8 §6). */
    if (GLD_U8(g, GS_POLY_SMOOTH) ||
        (!G.v8 && (GLD_U8(g, GS_LOGIC_OP) || GLD_U8(g, GS_POLY_STIPPLE))))
        return no(NO_RASTER, (GLD_U8(g, GS_LOGIC_OP) << 8) | GLD_U8(g, GS_POLY_STIPPLE),
                  GLD_U8(g, GS_POLY_SMOOTH));
    if (G.v8 && GLD_U8(g, GS_LOGIC_OP) && !logic_op_ok(U16(g, GS_LOGIC_OP_MODE)))
        return no(NO_RASTER, U16(g, GS_LOGIC_OP_MODE), 0);
    /* stencil : sans tampon, le test passe toujours (OpenGL) ; avec, il faut le
       format que l'on sait synchroniser (8 bits dans la profondeur de 32) */
    if (stencil_active(p)) {
        if (GLD_U32(p->ctx, CTX_STENCIL_BITS) != 8 || GLD_U32(p->ctx, CTX_DEPTH_BITS) != 32 ||
            U16(g, GS_STENCIL_FUNC) < 0x200 || U16(g, GS_STENCIL_FUNC) > 0x207 ||
            !stencil_op_ok(U16(g, GS_STENCIL_FAIL)) || !stencil_op_ok(U16(g, GS_STENCIL_ZFAIL)) ||
            !stencil_op_ok(U16(g, GS_STENCIL_ZPASS)))
            return no(NO_STENCIL, U16(g, GS_STENCIL_FUNC), GLD_U32(p->ctx, CTX_STENCIL_BITS));
    }
    /* brouillard : GLEngine fournit le facteur par sommet, sauf en GL_NICEST
       où le GLDriver le calcule par fragment */
    if (!raw && GLD_U8(g, GS_FOG) && U16(g, GS_FOG_HINT) == 0x1102)
        return no(NO_FOG, U16(g, GS_FOG_MODE), 0);
    /* Modes de polygone : réservés au chemin BRUT. Sur le chemin de
       rastérisation, GLEngine nous a déjà donné des TRIANGLES décomposés — le
       contour du quadrilatère ou du polygone d'origine est perdu avant l'hôte,
       et en mode GL_LINE les diagonales de la décomposition seraient tracées
       (docs/protocole-v8-pipeline-fixe.md §3, « écart assumé »). Mesuré dans
       l'invité : la scène « polymode » par le chemin hérité montre bien la
       diagonale du quadrilatère. On préfère donc le repli exact d'Apple. */
    if (U16(g, GS_POLY_MODE) != GL_FILL || U16(g, GS_POLY_MODE + 2) != GL_FILL) {
        if (!raw || !G.v8 || !poly_mode_ok(U16(g, GS_POLY_MODE)) ||
            !poly_mode_ok(U16(g, GS_POLY_MODE + 2)))
            return no(NO_POLYMODE, U16(g, GS_POLY_MODE), U16(g, GS_POLY_MODE + 2));
    }
    if (GLD_U8(g, GS_DEPTH_TEST) &&
        (GLD_U32(p->ctx, CTX_DEPTH_BITS) != 32 || U16(g, GS_DEPTH_FUNC) < 0x200 ||
         U16(g, GS_DEPTH_FUNC) > 0x207))
        return no(NO_DEPTH, GLD_U32(p->ctx, CTX_DEPTH_BITS), U16(g, GS_DEPTH_FUNC));
    if (GLD_U8(g, GS_BLEND) &&
        (!blend_factor_ok(U16(g, GS_BLEND_SRC_RGB)) || !blend_factor_ok(U16(g, GS_BLEND_DST_RGB)) ||
         !blend_factor_ok(U16(g, GS_BLEND_SRC_A)) || !blend_factor_ok(U16(g, GS_BLEND_DST_A)) ||
         !blend_eq_ok(U16(g, GS_BLEND_EQ_RGB)) || !blend_eq_ok(U16(g, GS_BLEND_EQ_A))))
        return no(NO_BLEND, (U16(g, GS_BLEND_SRC_RGB) << 16) | U16(g, GS_BLEND_DST_RGB),
                  (U16(g, GS_BLEND_EQ_RGB) << 16) | U16(g, GS_BLEND_EQ_A));
    if (GLD_U8(g, GS_ALPHA_TEST) && (U16(g, GS_ALPHA_FUNC) < 0x200 || U16(g, GS_ALPHA_FUNC) > 0x207))
        return no(NO_ALPHA, U16(g, GS_ALPHA_FUNC), 0);
    return 1;
}

static int accel_ok(PCtx *p)
{
    return accel_ok_for(p, 0);
}

static void compute_geom_state(PCtx *p, unsigned long *v);

/* ─── v8 : la fin du pipeline fixe (docs/protocole-v8-pipeline-fixe.md) ───
 *
 * Ces clés agissent au FRAGMENT (mélange, opération logique, pointillé de
 * polygone) ou à l'assemblage des triangles (mode de polygone, pointillé de
 * ligne) : elles valent pour les deux chemins de dessin, contrairement aux
 * clés de géométrie de la v7. Seuls les modes de polygone dépendent du chemin,
 * parce que l'ancien reçoit des triangles déjà décomposés (cf. accel_ok_for).
 */
static void compute_v8_state(PCtx *p, unsigned long *v, int raw)
{
    unsigned char *g = gls(p);
    const float *bc = (const float *)(g + GS_BLEND_COLOR);
    unsigned long fr = U16(g, GS_POLY_MODE), bk = U16(g, GS_POLY_MODE + 2);
    unsigned long fact = U16(g, GS_LINE_STIP_FACT);

    /* Couleur constante : toujours envoyée, qu'un facteur s'en serve ou non.
       C'est une valeur d'état comme la couleur de brouillard, et l'envoyer
       inconditionnellement évite un cas de plus dans le cache. */
    v[QGPU_SK_BLEND_COLOR] = (to_u8(bc[3]) << 24) | (to_u8(bc[0]) << 16) |
                             (to_u8(bc[1]) << 8) | to_u8(bc[2]);
    v[QGPU_SK_LOGIC_OP] = GLD_U8(g, GS_LOGIC_OP) != 0;
    v[QGPU_SK_LOGIC_OP_MODE] = logic_op_ok(U16(g, GS_LOGIC_OP_MODE))
                               ? U16(g, GS_LOGIC_OP_MODE) : QGPU_LO_COPY;
    /* Hors chemin brut, on laisse GL_FILL : le domaine a déjà refusé le dessin
       si l'application demandait autre chose (accel_ok_for), et poser la clé
       ferait rendre en fil de fer les triangles que l'ancien chemin envoie. */
    v[QGPU_SK_POLYGON_MODE_FRONT] = (raw && poly_mode_ok(fr)) ? fr : QGPU_POLY_FILL;
    v[QGPU_SK_POLYGON_MODE_BACK] = (raw && poly_mode_ok(bk)) ? bk : QGPU_POLY_FILL;
    v[QGPU_SK_POLY_OFFSET_LINE] = GLD_U8(g, GS_POLY_OFS_LINE) != 0;
    v[QGPU_SK_POLY_OFFSET_POINT] = GLD_U8(g, GS_POLY_OFS_PT) != 0;
    v[QGPU_SK_LINE_STIPPLE] = GLD_U8(g, GS_LINE_STIPPLE) != 0;
    /* Le cœur borne le facteur à 1..256 ; GLEngine le range sur 16 bits et
       glLineStipple l'a déjà écrêté, mais une clé fautive ferait refuser TOUTE
       la soumission : on borne ici aussi. */
    if (fact < 1) fact = 1;
    if (fact > 256) fact = 256;
    v[QGPU_SK_LINE_STIPPLE_FACTOR] = fact;
    v[QGPU_SK_LINE_STIPPLE_PATTERN] = U16(g, GS_LINE_STIP_PAT);
    v[QGPU_SK_POLYGON_STIPPLE] = GLD_U8(g, GS_POLY_STIPPLE) != 0;
}

/* `raw` : l'état est calculé pour un dessin du chemin BRUT (DRAW_RAW). Seuls
 * les modes de polygone en dépendent — cf. accel_ok_for. */
static void compute_state(PCtx *p, const TexInfo *ti, unsigned long *v, int raw)
{
    unsigned char *g = gls(p);
    long sx = I32(g, GS_SCISSOR_RECT), sy = I32(g, GS_SCISSOR_RECT + 4);
    long sw = I32(g, GS_SCISSOR_RECT + 8), sh = I32(g, GS_SCISSOR_RECT + 12);
    long top;
    int scissor = GLD_U8(g, GS_SCISSOR) != 0;

    memset(v, 0, QGPU_SK_COUNT * sizeof(*v));
    v[QGPU_SK_DEPTH_TEST] = GLD_U8(g, GS_DEPTH_TEST) != 0;
    v[QGPU_SK_DEPTH_FUNC] = v[QGPU_SK_DEPTH_TEST] ? U16(g, GS_DEPTH_FUNC) : 0x0201;
    v[QGPU_SK_DEPTH_WRITE] = GLD_U8(g, GS_DEPTH_MASK) != 0;
    v[QGPU_SK_COLOR_MASK] = (GLD_U8(g, GS_COLOR_MASK) ? 1 : 0) | (GLD_U8(g, GS_COLOR_MASK + 1) ? 2 : 0) |
                            (GLD_U8(g, GS_COLOR_MASK + 2) ? 4 : 0) | (GLD_U8(g, GS_COLOR_MASK + 3) ? 8 : 0);
    v[QGPU_SK_BLEND] = GLD_U8(g, GS_BLEND) != 0;
    if (v[QGPU_SK_BLEND]) {
        v[QGPU_SK_BLEND_SRC_RGB] = U16(g, GS_BLEND_SRC_RGB);
        v[QGPU_SK_BLEND_DST_RGB] = U16(g, GS_BLEND_DST_RGB);
        v[QGPU_SK_BLEND_SRC_A] = U16(g, GS_BLEND_SRC_A);
        v[QGPU_SK_BLEND_DST_A] = U16(g, GS_BLEND_DST_A);
        v[QGPU_SK_BLEND_EQ_RGB] = U16(g, GS_BLEND_EQ_RGB);
        v[QGPU_SK_BLEND_EQ_A] = U16(g, GS_BLEND_EQ_A);
    } else {                            /* valeurs neutres : moins d'envois */
        v[QGPU_SK_BLEND_SRC_RGB] = v[QGPU_SK_BLEND_SRC_A] = 1;
        v[QGPU_SK_BLEND_EQ_RGB] = v[QGPU_SK_BLEND_EQ_A] = 0x8006;
    }
    v[QGPU_SK_ALPHA_TEST] = GLD_U8(g, GS_ALPHA_TEST) != 0;
    v[QGPU_SK_ALPHA_FUNC] = v[QGPU_SK_ALPHA_TEST] ? U16(g, GS_ALPHA_FUNC) : 0x0207;
    v[QGPU_SK_ALPHA_REF] = v[QGPU_SK_ALPHA_TEST] ? GLD_U32(g, GS_ALPHA_REF) : 0;
    if (scissor) {
        /* GL : origine en bas à gauche ; qgpu : en haut à gauche. Borné à la surface. */
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sw < 0) sw = 0;
        if (sh < 0) sh = 0;
        if (sx > (long)p->sw) sx = p->sw;
        if (sy > (long)p->sh) sy = p->sh;
        if (sw > (long)p->sw - sx) sw = p->sw - sx;
        if (sh > (long)p->sh - sy) sh = p->sh - sy;
        top = (long)p->sh - (sy + sh);
        v[QGPU_SK_SCISSOR] = 1;
        v[QGPU_SK_SCISSOR_X] = sx;
        v[QGPU_SK_SCISSOR_Y] = top;
        v[QGPU_SK_SCISSOR_W] = sw;
        v[QGPU_SK_SCISSOR_H] = sh;
    }
    /* textures : liaison et environnement conservés quand l'unité est coupée */
    {
        int u, kb;
        for (u = 0; u < QGPU_MAX_UNITS; u++) {
            const TexUnit *tu = ti ? &ti->u[u] : 0;
            kb = QGPU_SK_UNIT(u);
            v[kb + QGPU_SK_U_ENABLE] = tu && tu->t;
            v[kb + QGPU_SK_U_BIND] = p->st_valid ? p->st[kb + QGPU_SK_U_BIND] : 0;
            v[kb + QGPU_SK_U_ENV_MODE] = p->st_valid ? p->st[kb + QGPU_SK_U_ENV_MODE] : 0x2100;
            v[kb + QGPU_SK_U_ENV_COLOR] = p->st_valid ? p->st[kb + QGPU_SK_U_ENV_COLOR] : 0;
            v[QGPU_SK_COMBINE0 + u] = p->st_valid ? p->st[QGPU_SK_COMBINE0 + u]
                                                  : QGPU_COMBINE_DEFAULT;
            v[QGPU_SK_COMBINE_SRC0 + u] = p->st_valid ? p->st[QGPU_SK_COMBINE_SRC0 + u]
                                                      : QGPU_COMBINE_SRC_DEFAULT;
            if (tu && tu->t) {
                v[kb + QGPU_SK_U_BIND] = tu->t->qtex;
                v[kb + QGPU_SK_U_ENV_MODE] = tu->env_mode;
                v[kb + QGPU_SK_U_ENV_COLOR] = tu->env_color;
                v[QGPU_SK_COMBINE0 + u] = tu->combine;
                v[QGPU_SK_COMBINE_SRC0 + u] = tu->combine_src;
            }
        }
    }
    /* stencil (v6) : le device borne tout à 8 bits ; test coupé = valeurs neutres,
       mais masque d'écriture et valeur d'effacement réels (l'effacement s'en sert) */
    v[QGPU_SK_STENCIL_TEST] = p->stencil && stencil_active(p);
    v[QGPU_SK_STENCIL_FUNC] = 0x0207;
    v[QGPU_SK_STENCIL_VALUE_MASK] = 0xFF;
    v[QGPU_SK_STENCIL_OP_FAIL] = v[QGPU_SK_STENCIL_OP_ZFAIL] = v[QGPU_SK_STENCIL_OP_ZPASS] = 0x1E00;
    v[QGPU_SK_STENCIL_WRITE_MASK] = GLD_U32(g, GS_STENCIL_WMASK) & 0xFF;
    v[QGPU_SK_STENCIL_CLEAR] = GLD_U32(g, GS_STENCIL_CLEAR) & 0xFF;
    if (v[QGPU_SK_STENCIL_TEST]) {
        v[QGPU_SK_STENCIL_FUNC] = U16(g, GS_STENCIL_FUNC);
        v[QGPU_SK_STENCIL_REF] = GLD_U32(g, GS_STENCIL_REF) & 0xFF;
        v[QGPU_SK_STENCIL_VALUE_MASK] = GLD_U32(g, GS_STENCIL_VMASK) & 0xFF;
        v[QGPU_SK_STENCIL_OP_FAIL] = U16(g, GS_STENCIL_FAIL);
        v[QGPU_SK_STENCIL_OP_ZFAIL] = U16(g, GS_STENCIL_ZFAIL);
        v[QGPU_SK_STENCIL_OP_ZPASS] = U16(g, GS_STENCIL_ZPASS);
    }
    v[QGPU_SK_FOG] = GLD_U8(g, GS_FOG) != 0;
    if (v[QGPU_SK_FOG]) {
        const float *fc = (const float *)(g + GS_FOG_COLOR);
        v[QGPU_SK_FOG_COLOR] = (to_u8(fc[3]) << 24) | (to_u8(fc[0]) << 16) |
                               (to_u8(fc[1]) << 8) | to_u8(fc[2]);
    } else {
        v[QGPU_SK_FOG_COLOR] = p->st_valid ? p->st[QGPU_SK_FOG_COLOR] : 0;
    }
    {
        float lw = GLD_F32(g, GS_LINE_WIDTH), ps = GLD_F32(g, GS_POINT_SIZE);
        /* bornes du device : ]0, 64] ; GL arrondit la largeur des lignes non lissées */
        if (!(lw >= 1.0f)) lw = 1.0f;
        if (lw > 64.0f) lw = 64.0f;
        if (!(ps >= 1.0f)) ps = 1.0f;
        if (ps > 64.0f) ps = 64.0f;
        lw = (float)(int)(lw + 0.5f);
        ps = (float)(int)(ps + 0.5f);
        v[QGPU_SK_LINE_WIDTH] = *(unsigned long *)&lw;
        v[QGPU_SK_POINT_SIZE] = *(unsigned long *)&ps;
    }
    v[QGPU_SK_POLY_OFFSET] = GLD_U8(g, GS_POLY_OFFSET) != 0;
    if (v[QGPU_SK_POLY_OFFSET]) {
        v[QGPU_SK_POLY_FACTOR] = GLD_U32(g, GS_POLY_FACTOR);
        v[QGPU_SK_POLY_UNITS] = GLD_U32(g, GS_POLY_UNITS);
    } else {
        v[QGPU_SK_POLY_FACTOR] = p->st_valid ? p->st[QGPU_SK_POLY_FACTOR] : 0;
        v[QGPU_SK_POLY_UNITS] = p->st_valid ? p->st[QGPU_SK_POLY_UNITS] : 0;
    }
    if (G.v7)
        compute_geom_state(p, v);
    if (G.v8)
        compute_v8_state(p, v, raw);
}

/* Le motif de pointillé de polygone : 32 mots pris dans le bloc de GLEngine,
 * décalés d'UNE LIGNE. En x il n'y a rien à faire — le bit de poids fort est la
 * colonne 0 des deux côtés, et le PowerPC est gros-boutiste comme le fil.
 *
 * En y, le protocole dit que la ligne de surface `ys` emploie le mot
 * `(hauteur − ys) mod 32` (docs/protocole-v8-pipeline-fixe.md §4). Or la
 * coordonnée fenêtre OpenGL de cette ligne est `hauteur − 1 − ys`, et c'est
 * elle que la spécification indexe. Les deux formules diffèrent d'une ligne :
 * le plugin envoie donc `motif[(j − 1) mod 32]` au mot `j`.
 *
 * VU EN VRAI (scène « stipple », motif de 16 lignes allumées sur 32) : sans ce
 * décalage, exactement 384 pixels — les 6 lignes de changement de bande du
 * quadrilatère de 64 de large — diffèrent du rendu d'Apple, et l'écart y est de
 * 255/255. Avec, l'image est identique. Le motif en COLONNES, lui, était déjà
 * juste : c'est bien un décalage vertical d'une ligne, pas une erreur de sens. */
static void send_polygon_stipple(PCtx *p)
{
    const unsigned long *src = (const unsigned long *)(gls(p) + GS_POLY_STIP_MASK);
    unsigned long *c;
    int j;
    if (p->c_pstip_valid && !memcmp(p->c_pstip, src, sizeof(p->c_pstip)))
        return;
    memcpy(p->c_pstip, src, sizeof(p->c_pstip));
    p->c_pstip_valid = 1;
    c = reserve(p, QGPU_LEN_SET_POLYGON_STIPPLE);
    c[0] = QGPU_CMD_HDR(QGPU_OP_SET_POLYGON_STIPPLE, QGPU_LEN_SET_POLYGON_STIPPLE);
    for (j = 0; j < 32; j++)
        c[1 + j] = p->c_pstip[(j + 31) & 31];
}

static void send_state(PCtx *p, const TexInfo *ti, int raw)
{
    unsigned long v[QGPU_SK_COUNT], *c;
    int k, r, nr = 0;
    struct { int lo, hi; } rg[2];
    compute_state(p, ti, v, raw);
    /* Sans le chemin brut, seulement les clés de rastérisation (v1–v6) : les
       clés de géométrie de la v7 gardent leur valeur initiale sur le device,
       et les envoyer à zéro serait invalide (vu en vrai au passage en v6, où
       toutes les soumissions ont été refusées). Avec, compute_geom_state les
       remplit toutes — et les anciens opcodes de dessin les ignorent (vérifié
       dans patches/qgpu : gl_apply_state remet l'ombrage, l'éclairage,
       l'élimination des faces et le brouillard à plat avant chaque dessin
       v1–v6, et qgpu-soft.c ne lit QGPU_SK_FOG_MODE que dans soft_draw_raw). */
    /* Bornes EXPLICITES : jamais QGPU_SK_COUNT, qui grandit à chaque version du
       protocole — les clés que compute_state ne remplit pas partiraient à zéro,
       valeur souvent invalide, et tout le flux serait refusé (vu en vrai à
       chaque changement de version : v6, puis v7, puis v8). D'où deux plages :
       les clés de géométrie (v7) ne partent qu'avec le chemin brut, les clés de
       fragment (v8) partent dès que le device est un v8 — elles servent aux
       deux chemins. Quand les deux sont là, les plages se touchent. */
    rg[nr].lo = 1; rg[nr].hi = G.v7 ? PLUGIN_SK_END_GEOM : QGPU_SK_LIGHTING; nr++;
    if (G.v8) { rg[nr].lo = QGPU_SK_BLEND_COLOR; rg[nr].hi = PLUGIN_SK_END_V8; nr++; }
    for (r = 0; r < nr; r++)
        for (k = rg[r].lo; k < rg[r].hi; k++) {
            if (p->st_valid && p->st[k] == v[k])
                continue;
            c = reserve(p, QGPU_LEN_SET_STATE);
            c[0] = QGPU_CMD_HDR(QGPU_OP_SET_STATE, QGPU_LEN_SET_STATE);
            c[1] = k;
            c[2] = v[k];
            p->st[k] = v[k];
        }
    p->st_valid = 1;
    /* Le motif ne part que s'il sert : 33 mots de flux, c'est la commande la
       plus longue du protocole. */
    if (G.v8 && v[QGPU_SK_POLYGON_STIPPLE])
        send_polygon_stipple(p);
}

/* Le dessin qui suit écrira-t-il la profondeur ? (GL : seulement si le test est actif) */
static int writes_depth(PCtx *p)
{
    unsigned char *g = gls(p);
    /* le stencil partage le mot (et donc la fraîcheur) de la profondeur */
    if (stencil_active(p) && (GLD_U32(g, GS_STENCIL_WMASK) & 0xFF))
        return 1;
    return GLD_U8(g, GS_DEPTH_TEST) && GLD_U8(g, GS_DEPTH_MASK);
}


/* ═══════════════ requêtes d'occlusion (v8, OpenGL 1.5) ═══════════════
 *
 * Relevé par lecture de GLEngine (docs/re/etat-v8.md §3) : le moteur tient
 * l'objet de requête lui-même (table de hachage, compte de références) et ne
 * demande au pilote que cinq choses, une par renderer du contexte :
 *
 *   gldCreateQuery(drvctx, &poignée)                entrée gld n° 45 (+0x1c8)
 *   gldDestroyQuery(drvctx, poignée)                entrée gld n° 46 (+0x1cc)
 *   gldGetQueryInfo(drvctx, poignée, nom, &valeur)  entrée gld n° 47 (+0x1d0)
 *   procédure +0x68 (drvctx, poignée)               glBeginQuery
 *   procédure +0x6c (drvctx, poignée)               glEndQuery
 *
 * Les trois entrées gld du GLDriver d'Apple sont des bouchons (`li r3,0; blr`)
 * et il n'installe rien en +0x68 / +0x6c. Mesuré dans l'invité (scène
 * « qprobe ») : sous le rendu d'Apple seul, glBeginQuery / glEndQuery /
 * glGetQueryObjectuiv ne rendent AUCUNE erreur GL et n'écrivent rien — une
 * application lit le contenu initial de sa variable. C'est donc la seule
 * fonction de ce lot que le repli logiciel ne tient pas du tout : le plugin la
 * tient entièrement, ou elle n'est pas annoncée.
 *
 * La poignée rendue au moteur est `identifiant + 1` : GLEngine range 0 dans
 * l'objet quand il n'y a pas de requête, et le premier client a justement
 * l'identifiant 0 (query_base = index de tranche × QGPU_CLIENT_QUERY_IDS).
 *
 * EXACTITUDE. Le compte de l'hôte ne porte que les fragments qu'il a
 * rastérisés. Si un dessin retombe sur le rendu d'Apple pendant qu'une requête
 * court, ses fragments manqueraient. On ajoute alors l'aire de la surface au
 * compte : sur-estimer est la SEULE direction sans danger, puisqu'une requête
 * d'occlusion se lit « si le compte est nul, je peux sauter l'objet » — un
 * objet déclaré visible est dessiné, donc l'image reste exacte, seule la
 * vitesse souffre. Le bilan compte le cas (« requete:repli-logiciel ») ; il
 * doit rester à zéro sur les scènes et sur les jeux.
 */
#define GL_QUERY_RESULT_AVAILABLE 0x8867

static unsigned long qry_used;                        /* identifiants pris */
static unsigned long qry_extra[QGPU_CLIENT_QUERY_IDS];/* replis pendant la requête */

static unsigned long sat_add(unsigned long a, unsigned long b)
{
    return (a > 0xFFFFFFFFUL - b) ? 0xFFFFFFFFUL : a + b;
}

/* Les deux procédures de rastérisation. GLEngine ignore leur valeur de retour. */
static long q_begin(void *ctx, unsigned long h)
{
    PCtx *p;
    unsigned long *c, id;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && G.v8 && !qry_off && h && !p->broken && p->qctx >= 0 &&
        h - 1 < QGPU_CLIENT_QUERY_IDS) {
        id = G.query_base + (h - 1);
        /* Le cœur refuse une seconde ouverture ; GLEngine l'interdit déjà, mais
           un contexte détruit puis recréé pourrait laisser la nôtre ouverte. */
        if (p->q_open >= 0) {
            c = reserve(p, QGPU_LEN_QUERY);
            c[0] = QGPU_CMD_HDR(QGPU_OP_QUERY_END, QGPU_LEN_QUERY);
            c[1] = (unsigned long)p->q_open;
        }
        c = reserve(p, QGPU_LEN_QUERY);
        c[0] = QGPU_CMD_HDR(QGPU_OP_QUERY_BEGIN, QGPU_LEN_QUERY);
        c[1] = id;
        qry_extra[h - 1] = 0;
        p->q_open = (long)id;
        p->q_extra = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return 0;
}

static long q_end(void *ctx, unsigned long h)
{
    PCtx *p;
    unsigned long *c;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && p->q_open >= 0 && h && h - 1 < QGPU_CLIENT_QUERY_IDS &&
        (unsigned long)p->q_open == G.query_base + (h - 1) && !p->broken) {
        c = reserve(p, QGPU_LEN_QUERY);
        c[0] = QGPU_CMD_HDR(QGPU_OP_QUERY_END, QGPU_LEN_QUERY);
        c[1] = (unsigned long)p->q_open;
        qry_extra[h - 1] = p->q_extra;
        p->q_open = -1;
        p->q_extra = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return 0;
}

/* gldCreateQuery : GLEngine veut UNE poignée par renderer, écrite à *out. */
static long q_create(void *ctx, unsigned long *out)
{
    unsigned long i;
    if (!out)
        return 0;
    *out = 0;
    pthread_mutex_lock(&G.mu);
    for (i = 0; i < QGPU_CLIENT_QUERY_IDS; i++)
        if (!(qry_used & (1UL << i))) {
            qry_used |= 1UL << i;
            qry_extra[i] = 0;
            *out = i + 1;
            break;
        }
    pthread_mutex_unlock(&G.mu);
    (void)ctx;
    return 0;
}

static long q_destroy(void *ctx, unsigned long h)
{
    PCtx *p;
    pthread_mutex_lock(&G.mu);
    if (h && h - 1 < QGPU_CLIENT_QUERY_IDS) {
        p = find_ctx(ctx);
        /* Détruire une requête encore ouverte : la refermer d'abord, sinon son
           identifiant resterait bloqué côté hôte. */
        if (p && p->q_open >= 0 && (unsigned long)p->q_open == G.query_base + (h - 1) &&
            !p->broken && p->qctx >= 0) {
            unsigned long *c = reserve(p, QGPU_LEN_QUERY);
            c[0] = QGPU_CMD_HDR(QGPU_OP_QUERY_END, QGPU_LEN_QUERY);
            c[1] = (unsigned long)p->q_open;
            p->q_open = -1;
        }
        qry_used &= ~(1UL << (h - 1));
        qry_extra[h - 1] = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return 0;
}

/* gldGetQueryInfo(drvctx, poignée, nom, &valeur). L'application demande le
 * compte : c'est un des rares endroits où l'invité a vraiment besoin d'une
 * relecture, donc un des rares où l'on attend la barrière (v9). On lit le mot
 * « disponible » plutôt que de le supposer, comme le protocole le demande. */
static long q_info(void *ctx, unsigned long h, unsigned long pname, unsigned long *out)
{
    PCtx *p;
    unsigned long off, *c, id, avail = 1, n = 0;

    if (!out)
        return 0;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && G.v8 && !qry_off && h && h - 1 < QGPU_CLIENT_QUERY_IDS &&
        !p->broken && p->qctx >= 0 && arena_alloc(8, &off)) {
        id = G.query_base + (h - 1);
        c = reserve(p, QGPU_LEN_QUERY_RESULT);
        c[0] = QGPU_CMD_HDR(QGPU_OP_QUERY_RESULT, QGPU_LEN_QUERY_RESULT);
        c[1] = id;
        c[2] = G.q.base + off;
        {
            int i = G.cur;
            flush();
            wait_half(i);
        }
        if (!qry_off && !p->broken) {
            const unsigned long *w = (const unsigned long *)(G.q.win + off);
            avail = w[0] ? 1 : 0;
            n = sat_add(w[1], qry_extra[h - 1]);
        }
    }
    *out = (pname == GL_QUERY_RESULT_AVAILABLE) ? avail : n;
    pthread_mutex_unlock(&G.mu);
    return 0;
}

/* Entrées gld que le plugin réalise lui-même, au lieu de les transmettre au
 * rendu d'Apple. Le crochet pomppc_pre rend l'adresse à appeler : il suffit d'y
 * rendre la nôtre, le trampoline saute dedans avec les arguments d'origine.
 * (C'est aussi ce qui évite de toucher à gld_tramp.s, qui est engendré.) */
void *pomppc_gld_override(int id)
{
    if (G.state <= 0 || !G.v8 || qry_off)
        return 0;
    switch (id) {
    case GLD_CreateQuery:  return (void *)q_create;
    case GLD_DestroyQuery: return (void *)q_destroy;
    case GLD_GetQueryInfo: return (void *)q_info;
    default:               return 0;
    }
}

/* ═════════════ chemin brut : la géométrie sur le GPU de l'hôte (v7) ═════════════
 *
 * Quand l'état courant est DANS LE DOMAINE, gldInitDispatch/gldUpdateDispatch
 * rendent le bit 0 (docs/re/verification-tcl.md §3) : GLEngine cesse de
 * transformer, d'éclairer, de découper et d'éliminer les faces, et remet les
 * attributs BRUTS (coordonnées d'objet) dans le tampon que BeginPrimitiveBuffer
 * lui donne, à la disposition du descripteur publié en cfg+0x11c
 * (docs/re/descripteur-de-sommet.md). Hors domaine, on rend le retour d'Apple
 * tel quel et GLEngine reprend tout le travail : le repli est exact.
 *
 * Deux principes tiennent tout le reste :
 *
 *   — ZÉRO COPIE. Le descripteur est choisi pour que la disposition de GLEngine
 *     SOIT celle de DRAW_RAW (position4, normale3, couleur4, brouillard1,
 *     coordonnées de texture 4 par unité, dans l'ordre fixe du protocole), et
 *     BeginPrimitiveBuffer rend un pointeur DANS la fenêtre partagée, à
 *     VTX_OFF + G.vtx. EndPrimitiveBuffer n'a plus qu'à écrire dix mots.
 *
 *   — UN SEUL PRÉDICAT. geom_ok() décide du domaine, et il est appelé aux DEUX
 *     endroits : au dispatch (où l'on peut encore refuser) et à
 *     BeginPrimitiveBuffer (où l'on ne peut plus : rendre 0 ferait écrire
 *     GLEngine à l'adresse nulle). S'il est vrai au dispatch et faux à Begin,
 *     c'est que l'état a bougé sans passer par gldUpdateDispatch : on compte le
 *     cas (« brut:etat-tardif ») et on quitte le domaine pour de bon. Ce
 *     compteur DOIT rester à zéro ; c'est la garantie d'exactitude.
 *
 * Tout ce qui peut échouer (téléversement de texture, place dans le flux, dans
 * la zone des sommets) est fait à Begin, AVANT que GLEngine écrive quoi que ce
 * soit — jamais à End.
 */

#define GS_LOW           0x360      /* gctx = GS − 0x360 */
#define GC_VTX_DESC      0x48d0     /* descripteur retenu par GLEngine */
#define GC_VTX_STRIDE    0x4880     /* u16 : pas d'un sommet, en octets */
#define GEOM_MAX_MERGE   16384      /* sommets d'une série fusionnée */
#define GEOM_MAX_SLOTS   8192       /* sommets offerts à un BeginPrimitiveBuffer */
#define GEOM_MIN_SLOTS   512        /* en dessous, on vide le flux avant Begin */

/* Tampon de secours : rendu à GLEngine quand on doit refuser une primitive.
 * BeginPrimitiveBuffer ne peut PAS rendre 0 (écriture à l'adresse nulle). */
static unsigned char geom_scratch[64 * 1024];

static unsigned long fbits(float f)
{
    union { float f; unsigned long u; } v;
    v.f = f;
    return v.u;
}

/* Recopie n flottants dans des mots de commande, en bornant ce que le cœur
 * refuse (NaN, infini, |v| > 1e9) : un seul mot fautif ferait rejeter toute la
 * soumission, donc on préfère une valeur nulle à un flux perdu. */
static void put_f(unsigned long *dst, const float *src, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        float v = src[i];
        if (!(v > -1e9f && v < 1e9f))
            v = (v > 0.0f) ? 1e9f : (v < 0.0f) ? -1e9f : 0.0f;
        dst[i] = fbits(v);
    }
}

static unsigned char *gctx_of(PCtx *p)
{
    unsigned char *g = gls(p);
    return g ? g - GS_LOW : 0;
}

/* POMPPC_GL_GEOM : 0 coupé, 1 activé (défaut), 2 activé avec un format de
 * sommet fixe et large (pour mesurer le coût de la republication). */
static int geom_switch(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("POMPPC_GL_GEOM");
        v = (e && *e) ? atoi(e) : 1;
        if (v < 0)
            v = 0;
    }
    return v;
}

/* Le bloc source a-t-il bougé depuis le dernier envoi ? C'est le test chaud :
 * il tourne à chaque BeginPrimitiveBuffer, et Marble Blast en fait un millier
 * par image. On compare les octets de GLEngine, sans rien fabriquer. */
static int changed(PCtx *p, const void *src, void *cache, unsigned long n)
{
    if (p->g_sent && !memcmp(cache, src, n))
        return 0;
    memcpy(cache, src, n);
    return 1;
}

/* Émet une commande dont les arguments viennent d'être construits. */
static void send_cmd(PCtx *p, unsigned long op, unsigned long len,
                     const unsigned long *args)
{
    unsigned long *c = reserve(p, len);
    c[0] = QGPU_CMD_HDR(op, len);
    memcpy(c + 1, args, (len - 1) * sizeof(*args));
    G.n_geomcmds++;
}

/* ─────────────────────────── domaine du chemin brut ─────────────────────────── */

/* L'unité u portera-t-elle une texture sur l'hôte ? (Même verdict que
 * texture_unit_ok, sans téléversement : il faut pouvoir répondre au dispatch.) */
static int unit_textured(PCtx *p, int u)
{
    unsigned long mask;
    void *dt;
    PTex *t;
    unsigned char *lv;

    if (!GLD_U8(p->ctx, CTX_TEXTURING))
        return 0;
    dt = unit_drvtex(p, u, &mask);
    t = dt ? find_tex(dt) : 0;
    if (!t)
        return 0;
    lv = (unsigned char *)t->drvtex + DT_LEVEL0;
    return S16(lv, LV_W) != 0 && GLD_U32(lv, LV_DATA) != 0;
}

/* Le texturage courant tiendra-t-il sur l'hôte ? Prédicat pur (aucune commande,
 * aucune conversion) : c'est la moitié chère du domaine, appelée à chaque
 * changement d'état, d'où le raccourci « déjà téléversée et propre ». */
static int geom_texture_ok(PCtx *p)
{
    unsigned char *g = gls(p);
    int u, i;

    if (!GLD_U8(p->ctx, CTX_TEXTURING))
        return 1;
    for (i = QGPU_MAX_UNITS; i < GL_MAX_TEXUNITS; i++)
        if (GLD_U32(g, GS_TEXUNIT0 + i * GS_TEXUNIT_SIZE + TU_ENABLE) & 0x1f)
            return no(NO_TEX_UNITS, i, 0);
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        unsigned char *us = g + GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE;
        unsigned long mask, env = U16(us, TU_ENV_MODE);
        TexUnit tu;
        void *dt = unit_drvtex(p, u, &mask);
        PTex *t;
        unsigned char *lv;
        if (!mask)
            continue;
        if (mask & 0x7)
            return no(NO_TEX_TARGET, mask, GLD_U32(p->ctx, CTX_TEXUNITS));
        if (env != 0x2100 && env != 0x2101 && env != 0x0BE2 && env != 0x1E01 &&
            env != 0x0104 && env != 0x8570)
            return no(NO_TEX_ENV, env, u);
        if (env == 0x8570 && !combine_ok(us, u, &tu))
            return 0;
        t = dt ? find_tex(dt) : 0;
        if (!t)
            return no(NO_TEX_UNKNOWN, (unsigned long)dt, mask);
        lv = (unsigned char *)t->drvtex + DT_LEVEL0;
        if (!S16(lv, LV_W) || !GLD_U32(lv, LV_DATA))
            continue;                   /* incomplète : l'unité est coupée, comme en GL */
        if (!texture_uploadable(t))
            return 0;
    }
    return 1;
}

/* L'état courant peut-il partir en géométrie brute ? */
static int geom_ok(PCtx *p)
{
    unsigned char *g;
    const float *att;

    if (!G.v7 || G.state <= 0 || p->broken || p->geom_lost || p->qctx < 0 || !p->cfg)
        return 0;
    if (!accel_ok_for(p, 1))
        return 0;
    g = gls(p);
    /* Rastérisation que le protocole ne porte pas. Au dispatch on ne sait pas
       encore quelle primitive viendra : le lissage et la taille de point
       atténuée sortent du domaine quoi qu'il arrive. Le pointillé de LIGNE, lui,
       est entré dans le domaine avec la v8 — et c'est bien ici qu'il faut le
       laisser passer, puisque l'hôte tient le compteur primitive par primitive
       (remise à zéro par segment pour GL_LINES, continu le long d'un ruban). */
    if ((!G.v8 && GLD_U8(g, GS_LINE_STIPPLE)) || GLD_U8(g, GS_LINE_SMOOTH) ||
        GLD_U8(g, GS_POINT_SMOOTH))
        return no(NO_G_RASTER, (GLD_U8(g, GS_LINE_STIPPLE) << 8) | GLD_U8(g, GS_LINE_SMOOTH),
                  GLD_U8(g, GS_POINT_SMOOTH));
    att = (const float *)(g + GS_POINT_ATT);
    if (att[0] != 1.0f || att[1] != 0.0f || att[2] != 0.0f)
        return no(NO_G_POINT, fbits(att[1]), fbits(att[2]));
    /* Le mot gctx+0x4e1c dit quel étage de sommets GLEngine emploie : c'est la
       condition que _gleBuildVertexFuncNO (0x1d318) teste lui-même avant de
       lire notre descripteur. Autre chose que 0x1c00 (pipeline fixe) — un
       programme ARB de sommets, par exemple — et GLEngine nous enverrait son
       sommet INTERNE, déjà transformé : à fuir. (GS+0x50c0 ne convient pas
       comme sonde : il pointe toujours sur un objet, même sans programme lié.) */
    {
        unsigned char *gc = gctx_of(p);
        if (!gc || GLD_U32(gc, 0x4e1c) != 0x1c00)
            return no(NO_G_PROGRAM, gc ? GLD_U32(gc, 0x4e1c) : 0, 0);
    }
    if (!geom_texture_ok(p))
        return 0;
    return 1;
}

/* ─────────────────── format de sommet et descripteur GLEngine ─────────────────── */

/* La normale sert-elle à autre chose qu'à l'éclairage ? Oui : GL_SPHERE_MAP,
 * GL_NORMAL_MAP et GL_REFLECTION_MAP en partent. Sans ce test, le texgen d'une
 * scène sans éclairage recevait la normale COURANTE, donc constante — et
 * l'image entière de la case SPHERE_MAP était fausse (vu en vrai, scène
 * « texgen » : écart 166/255 sur toute la case). */
static int texgen_needs_normal(PCtx *p, int u)
{
    const unsigned char *tg = gls(p) + GS_TEXGEN(u);
    int c;
    for (c = 0; c < 4; c++) {
        unsigned long m = U16(tg + TG_COORD(c), TG_MODE);
        if (GLD_U8(tg, TG_ENABLE + c) &&
            (m == 0x2402 || m == 0x8511 || m == 0x8512))
            return 1;
    }
    return 0;
}

static unsigned long geom_format(PCtx *p)
{
    unsigned char *g = gls(p);
    unsigned long fmt = QGPU_VF_POS(4);
    int u, lighting = GLD_U8(g, GS_LIGHTING) != 0, normal = lighting;

    /* La couleur ne sert que si elle atteint la sortie : éclairage éteint, ou
       allumé avec GL_COLOR_MATERIAL. Quatre mots de moins par sommet sinon. */
    if (!lighting || GLD_U8(g, GS_COLOR_MATERIAL))
        fmt |= QGPU_VF_COLOR;
    /* La coordonnée de brouillard n'est portée QUE si c'est bien elle la source :
       la présence du bit dit à l'hôte de poser GL_FOG_COORDINATE. */
    if (GLD_U8(g, GS_FOG) && U16(g, GS_FOG_COORD_SRC) == 0x8451)
        fmt |= QGPU_VF_FOG;
    for (u = 0; u < QGPU_MAX_UNITS; u++)
        if (unit_textured(p, u)) {
            fmt |= QGPU_VF_TEX(u);
            if (texgen_needs_normal(p, u))
                normal = 1;
        }
    if (normal)
        fmt |= QGPU_VF_NORMAL;
    if (geom_switch() >= 2) {           /* format fixe et large : mesure */
        fmt |= QGPU_VF_NORMAL | QGPU_VF_COLOR |
               QGPU_VF_TEX(0) | QGPU_VF_TEX(1) | QGPU_VF_TEX(2) | QGPU_VF_TEX(3);
    }
    return fmt;
}

/* Une entrée du descripteur : (code << 10) | ((composantes − 1) << 8) | mot.
 * Les codes sont les indices d'attribut d'entrée de GLEngine
 * (docs/re/descripteur-de-sommet.md §4) ; le code 6 plante, on ne le demande
 * jamais. Les décalages sont choisis pour reproduire EXACTEMENT l'ordre fixe
 * de DRAW_RAW : c'est ce qui rend la recopie inutile. */
#define DESC_ENT(code, off, nc) \
    ((unsigned short)(((code) << 10) | (((nc) - 1) << 8) | ((off) & 0xff)))

/* (Re)construit le descripteur pour l'état courant ; 1 s'il a changé. */
static int geom_publish(PCtx *p)
{
    unsigned long fmt = geom_format(p);
    unsigned short ent[8];
    unsigned long off = 0;
    int n = 0, u;

    /* Republier n'a de sens que si le format change, OU si GLEngine n'a pas
       encore notre descripteur (cfg+0x11c remis à zéro à la création). */
    if (fmt == p->geom_fmt && GLD_U32(p->cfg, 0x11c) == (unsigned long)p->desc)
        return 0;
    ent[n++] = DESC_ENT(0, off, 4); off += 4;                 /* position */
    if (fmt & QGPU_VF_NORMAL) { ent[n++] = DESC_ENT(1, off, 3); off += 3; }
    if (fmt & QGPU_VF_COLOR)  { ent[n++] = DESC_ENT(2, off, 4); off += 4; }
    if (fmt & QGPU_VF_FOG)    { ent[n++] = DESC_ENT(3, off, 1); off += 1; }
    for (u = 0; u < QGPU_MAX_UNITS; u++)
        if (fmt & QGPU_VF_TEX(u)) { ent[n++] = DESC_ENT(8 + u, off, 4); off += 4; }
    memset(p->desc, 0, sizeof(p->desc));
    ((unsigned char *)p->desc)[0] = (unsigned char)n;
    ((unsigned char *)p->desc)[2] = (unsigned char)off;       /* pas, en mots */
    for (u = 0; u < n; u++)
        ((unsigned short *)p->desc)[2 + u] = ent[u];
    p->geom_fmt = fmt;
    p->geom_words = off;
    GLD_U32(p->cfg, 0x11c) = (unsigned long)p->desc;
    return 1;
}

/* ─────────────────── l'état géométrique, et seulement ce qui change ─────────────────── */

/* Un flottant que le cœur acceptera : il refuse NaN, infini et |v| >= 1e9, et
 * une seule clé fautive ferait rejeter toute la soumission. */
static unsigned long safe_f(float v, float def)
{
    return fbits((v > -1e9f && v < 1e9f) ? v : def);
}

static void compute_geom_state(PCtx *p, unsigned long *v)
{
    unsigned char *g = gls(p);
    unsigned long cf = U16(g, GS_COLORMAT_FACE), cm = U16(g, GS_COLORMAT_MODE);
    unsigned long cull = U16(g, GS_CULL_MODE), fog = U16(g, GS_FOG_MODE);
    float dens = GLD_F32(g, GS_FOG_DENSITY);

    v[QGPU_SK_LIGHTING] = GLD_U8(g, GS_LIGHTING) != 0;
    v[QGPU_SK_NORMALIZE] = GLD_U8(g, GS_NORMALIZE) != 0;
    v[QGPU_SK_RESCALE_NORMAL] = GLD_U8(g, GS_RESCALE_NORMAL) != 0;
    v[QGPU_SK_SHADE_MODEL] = GLD_U32(g, GS_SHADE_MODEL) == GL_FLAT ? 0x1D00 : 0x1D01;
    v[QGPU_SK_CULL_FACE] = GLD_U8(g, GS_CULL_FACE) != 0;
    v[QGPU_SK_CULL_MODE] = (cull == 0x0404 || cull == 0x0405 || cull == 0x0408) ? cull : 0x0405;
    v[QGPU_SK_FRONT_FACE] = U16(g, GS_FRONT_FACE) == 0x0900 ? 0x0900 : 0x0901;
    v[QGPU_SK_COLOR_MATERIAL] = GLD_U8(g, GS_COLOR_MATERIAL) != 0;
    v[QGPU_SK_COLOR_MAT_FACE] = (cf == 0x0404 || cf == 0x0405 || cf == 0x0408) ? cf : 0x0408;
    v[QGPU_SK_COLOR_MAT_MODE] =
        (cm == 0x1600 || cm == 0x1200 || cm == 0x1201 || cm == 0x1202 || cm == 0x1602)
        ? cm : 0x1602;
    v[QGPU_SK_LOCAL_VIEWER] = GLD_U8(g, GS_LOCAL_VIEWER) != 0;
    v[QGPU_SK_TWO_SIDE] = GLD_U8(g, GS_TWO_SIDE) != 0;
    v[QGPU_SK_COLOR_CONTROL] = U16(g, GS_COLOR_CONTROL) == 0x81FA ? 0x81FA : 0x81F9;
    /* Brouillard : l'hôte le calcule pour DRAW_RAW, par fragment. Les opcodes de
       dessin v1–v6 ignorent ce mode et gardent le facteur par sommet que
       GLEngine leur donne — les deux chemins peuvent donc cohabiter dans la
       même image sans que l'un dérègle l'autre. */
    v[QGPU_SK_FOG_MODE] = (fog == 0x0800 || fog == 0x0801 || fog == 0x2601)
                          ? fog : QGPU_FOG_VERTEX;
    v[QGPU_SK_FOG_DENSITY] = fbits(dens >= 0.0f && dens < 1e9f ? dens : 1.0f);
    v[QGPU_SK_FOG_START] = safe_f(GLD_F32(g, GS_FOG_START), 0.0f);
    v[QGPU_SK_FOG_END] = safe_f(GLD_F32(g, GS_FOG_END), 1.0f);
}

/* Le seuil de spot est rangé par GLEngine en COSINUS (< 0 = pas un spot) ;
 * le protocole veut l'ANGLE en degrés, 0..90 ou exactement 180. */
static float spot_cutoff_deg(float c)
{
    double a;
    if (!(c >= 0.0f))
        return 180.0f;
    if (c > 1.0f)
        c = 1.0f;
    a = acos((double)c) * (180.0 / 3.14159265358979323846);
    if (a < 0.0)
        a = 0.0;
    if (a > 90.0)
        a = 90.0;
    return (float)a;
}

static void geom_send_matrices(PCtx *p, unsigned long fmt)
{
    unsigned char *g = gls(p);
    unsigned long a[QGPU_LEN_SET_MATRIX - 1];
    int u;

    /* Comparer les 64 octets de la matrice coûte moins que d'exploiter le
       masque « matrice modifiée » (GS+0x4d48), qui ne dit rien des matrices de
       texture d'une unité qui vient de s'allumer. */
    if (changed(p, g + GS_MAT_MODELVIEW, p->c_mtx[QGPU_MTX_MODELVIEW], 64)) {
        a[0] = QGPU_MTX_MODELVIEW;
        put_f(a + 1, (const float *)(g + GS_MAT_MODELVIEW), 16);
        send_cmd(p, QGPU_OP_SET_MATRIX, QGPU_LEN_SET_MATRIX, a);
    }
    if (changed(p, g + GS_MAT_PROJ, p->c_mtx[QGPU_MTX_PROJECTION], 64)) {
        a[0] = QGPU_MTX_PROJECTION;
        put_f(a + 1, (const float *)(g + GS_MAT_PROJ), 16);
        send_cmd(p, QGPU_OP_SET_MATRIX, QGPU_LEN_SET_MATRIX, a);
    }
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        if (!(fmt & QGPU_VF_TEX(u)))
            continue;
        if (!changed(p, g + GS_MAT_TEXTURE(u), p->c_mtx[QGPU_MTX_TEXTURE0 + u], 64))
            continue;
        a[0] = QGPU_MTX_TEXTURE0 + u;
        put_f(a + 1, (const float *)(g + GS_MAT_TEXTURE(u)), 16);
        send_cmd(p, QGPU_OP_SET_MATRIX, QGPU_LEN_SET_MATRIX, a);
    }
}

static void geom_send_lights(PCtx *p)
{
    unsigned char *g = gls(p);
    unsigned long mask = GLD_U32(g, GS_LIGHT_MASK);
    unsigned long a[QGPU_LEN_SET_LIGHT - 1];
    unsigned long m[QGPU_LEN_SET_MATERIAL - 1];
    int i, f, remask = !p->g_sent || mask != p->c_lmask;
    float v;

    p->c_lmask = mask;
    /* [i, actif, ambiante4, diffuse4, spéculaire4, position4, spot3, exposant,
       coupure, atténuations3] — 26 mots, la plus longue commande du protocole. */
    for (i = 0; i < QGPU_MAX_LIGHTS; i++) {
        const unsigned char *l = g + GS_LIGHT(i);
        int on = (mask >> i) & 1;
        int moved = changed(p, l, p->c_light[i], 0x60);
        if (!moved && !remask)
            continue;
        if (!on && !remask)
            continue;                   /* éteinte et déjà éteinte sur le device */
        a[0] = (unsigned long)i;
        a[1] = (unsigned long)on;
        put_f(a + 2, (const float *)(l + LT_AMBIENT), 4);
        put_f(a + 6, (const float *)(l + LT_DIFFUSE), 4);
        put_f(a + 10, (const float *)(l + LT_SPECULAR), 4);
        /* Position et direction de spot sont DÉJÀ en coordonnées œil (GLEngine
           les y met au moment de glLight) et le protocole les attend ainsi :
           on recopie, on ne retransforme pas. */
        put_f(a + 14, (const float *)(l + LT_POSITION), 4);
        put_f(a + 18, (const float *)(l + LT_SPOT_DIR), 3);
        v = GLD_F32(l, LT_SPOT_EXP);
        if (!(v >= 0.0f)) v = 0.0f;
        if (v > 128.0f) v = 128.0f;
        a[21] = fbits(v);
        a[22] = fbits(spot_cutoff_deg(GLD_F32(l, LT_SPOT_COS)));
        v = GLD_F32(l, LT_ATT_CONST);  a[23] = fbits(v >= 0.0f && v < 1e9f ? v : 1.0f);
        v = GLD_F32(l, LT_ATT_LINEAR); a[24] = fbits(v >= 0.0f && v < 1e9f ? v : 0.0f);
        v = GLD_F32(l, LT_ATT_QUAD);   a[25] = fbits(v >= 0.0f && v < 1e9f ? v : 0.0f);
        send_cmd(p, QGPU_OP_SET_LIGHT, QGPU_LEN_SET_LIGHT, a);
    }
    /* GL_COLOR_MATERIAL : GLEngine a DÉJÀ écrasé les composantes suivies avec la
       couleur courante dans l'objet matériau (docs/re/etat-tcl.md §2.1). On lit
       donc l'objet tel quel, et la clé QGPU_SK_COLOR_MATERIAL dit à l'hôte de
       refaire le suivi par sommet. */
    for (f = 0; f < 2; f++) {
        const unsigned char *mt = g + (f ? GS_MATERIAL_BACK : GS_MATERIAL_FRONT);
        if (!changed(p, mt, p->c_mat[f], 0x44))
            continue;
        m[0] = f ? 0x0405UL : 0x0404UL; /* GL_BACK / GL_FRONT */
        put_f(m + 1, (const float *)(mt + MT_AMBIENT), 4);
        put_f(m + 5, (const float *)(mt + MT_DIFFUSE), 4);
        put_f(m + 9, (const float *)(mt + MT_SPECULAR), 4);
        put_f(m + 13, (const float *)(mt + MT_EMISSION), 4);
        v = GLD_F32(mt, MT_SHININESS);
        if (!(v >= 0.0f)) v = 0.0f;
        if (v > 128.0f) v = 128.0f;
        m[17] = fbits(v);
        send_cmd(p, QGPU_OP_SET_MATERIAL, QGPU_LEN_SET_MATERIAL, m);
    }
    if (changed(p, g + GS_SCENE_AMBIENT, p->c_lm, 16)) {
        unsigned long lm[QGPU_LEN_SET_LIGHT_MODEL - 1];
        put_f(lm, (const float *)(g + GS_SCENE_AMBIENT), 4);
        send_cmd(p, QGPU_OP_SET_LIGHT_MODEL, QGPU_LEN_SET_LIGHT_MODEL, lm);
    }
}

static void geom_send_texgen(PCtx *p, unsigned long fmt)
{
    unsigned char *g = gls(p);
    unsigned long a[QGPU_LEN_SET_TEXGEN - 1];
    int u, c;

    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        const unsigned char *tg = g + GS_TEXGEN(u);
        int on;
        if (!(fmt & QGPU_VF_TEX(u)))
            continue;
        /* Le cas de loin le plus courant est « aucune coordonnée engendrée, et
           rien à défaire sur le device » : quatre octets lus au lieu de cent
           quarante-huit comparés, et cela mille fois par image. */
        on = GLD_U8(tg, TG_ENABLE) | GLD_U8(tg, TG_ENABLE + 1) |
             GLD_U8(tg, TG_ENABLE + 2) | GLD_U8(tg, TG_ENABLE + 3);
        if (!on && !p->c_tg_on[u])
            continue;
        /* Le bloc d'une unité (0x94 octets) porte les modes, les deux plans et
           les quatre activations : un seul memcmp couvre les quatre coordonnées. */
        if (!changed(p, tg, p->c_tg[u], 0x94))
            continue;
        p->c_tg_on[u] = on != 0;
        for (c = 0; c < 4; c++) {
            const unsigned char *co = tg + TG_COORD(c);
            unsigned long mode = U16(co, TG_MODE);
            int on = GLD_U8(tg, TG_ENABLE + c) != 0;
            /* Le cœur refuse SPHERE_MAP hors de S,T et NORMAL/REFLECTION_MAP sur
               Q : un état impossible en GL, mais une soumission refusée ferait
               perdre toute l'image. On retombe sur EYE_LINEAR. */
            if (mode != 0x2400 && mode != 0x2401 && mode != 0x2402 &&
                mode != 0x8511 && mode != 0x8512)
                mode = 0x2400;
            if ((mode == 0x2402 && c > 1) || ((mode == 0x8511 || mode == 0x8512) && c > 2))
                mode = 0x2400;
            a[0] = u; a[1] = c; a[2] = on; a[3] = mode;
            put_f(a + 4, (const float *)(co + TG_OBJ_PLANE), 4);
            /* le plan œil est déjà transformé par GLEngine : recopie */
            put_f(a + 8, (const float *)(co + TG_EYE_PLANE), 4);
            send_cmd(p, QGPU_OP_SET_TEXGEN, QGPU_LEN_SET_TEXGEN, a);
        }
    }
}

static void geom_send_clip(PCtx *p)
{
    unsigned char *g = gls(p);
    unsigned long mask;
    unsigned long a[QGPU_LEN_SET_CLIP_PLANE - 1];
    int i;

    /* Rien de découpé, rien à défaire : quatre octets lus, pas cent comparés.
       C'est le cas de presque toutes les images, et on passe ici mille fois. */
    if (p->g_sent && !GLD_U32(g, GS_CLIP_MASK) && !p->c_clip[0])
        return;
    /* masque et six plans sont contigus (0x3e28 puis 0x3e2c) : un seul memcmp */
    if (!changed(p, g + GS_CLIP_MASK, p->c_clip, 4 + QGPU_MAX_CLIP_PLANES * 16))
        return;
    mask = GLD_U32(g, GS_CLIP_MASK);
    for (i = 0; i < QGPU_MAX_CLIP_PLANES; i++) {
        a[0] = i;
        a[1] = (mask >> i) & 1;
        put_f(a + 2, (const float *)(g + GS_CLIP_PLANE(i)), 4);
        send_cmd(p, QGPU_OP_SET_CLIP_PLANE, QGPU_LEN_SET_CLIP_PLANE, a);
    }
}

/* Les attributs que le format de sommet n'emporte pas : l'hôte prend la valeur
 * courante, exactement comme glColor / glNormal hors glBegin. Elles vivent
 * AVANT le bloc d'état (gctx + x), docs/re/etat-tcl.md §2.2. */
static void geom_send_current(PCtx *p, unsigned long fmt)
{
    unsigned char *g = gls(p);
    unsigned long a[QGPU_LEN_SET_CURRENT - 1];

    memset(a, 0, sizeof(a));
    if (!(fmt & QGPU_VF_COLOR) && changed(p, g + GS_CUR_COLOR, p->c_cur[0], 16)) {
        a[0] = QGPU_CUR_COLOR;
        put_f(a + 1, (const float *)(g + GS_CUR_COLOR), 4);
        send_cmd(p, QGPU_OP_SET_CURRENT, QGPU_LEN_SET_CURRENT, a);
    }
    if (!(fmt & QGPU_VF_NORMAL) && changed(p, g + GS_CUR_NORMAL, p->c_cur[1], 12)) {
        a[0] = QGPU_CUR_NORMAL;
        put_f(a + 1, (const float *)(g + GS_CUR_NORMAL), 3);
        a[4] = 0;
        send_cmd(p, QGPU_OP_SET_CURRENT, QGPU_LEN_SET_CURRENT, a);
    }
    /* La couleur secondaire n'est jamais portée par le sommet : la mettre dans
       le format ferait allumer GL_COLOR_SUM sur l'hôte, ce que l'état GL ne dit
       pas. Elle passe donc toujours en valeur courante. */
    if (changed(p, g + GS_CUR_SECCOLOR, p->c_cur[2], 12)) {
        a[0] = QGPU_CUR_SEC_COLOR;
        put_f(a + 1, (const float *)(g + GS_CUR_SECCOLOR), 3);
        a[4] = 0;
        send_cmd(p, QGPU_OP_SET_CURRENT, QGPU_LEN_SET_CURRENT, a);
    }
    if (!(fmt & QGPU_VF_FOG) && changed(p, g + GS_CUR_FOGCOORD, p->c_cur[3], 4)) {
        a[0] = QGPU_CUR_FOG;
        put_f(a + 1, (const float *)(g + GS_CUR_FOGCOORD), 1);
        a[2] = a[3] = a[4] = 0;
        send_cmd(p, QGPU_OP_SET_CURRENT, QGPU_LEN_SET_CURRENT, a);
    }
    /* Les coordonnées de texture, elles, sont TOUJOURS dans le format dès que
       l'unité porte une texture : l'hôte n'a jamais à prendre leur valeur
       courante. (Une unité sans texture est coupée, il ne s'y passe rien.) */
}

static void geom_send_viewport(PCtx *p)
{
    unsigned char *g = gls(p);
    unsigned long a[QGPU_LEN_VIEWPORT - 1];
    unsigned long d[QGPU_LEN_DEPTH_RANGE - 1];

    /* GS+0x1840 est le viewport GL tel quel : origine en BAS à gauche, ce que
       DRAW_RAW attend (la couture avec le repère de surface est faite par
       l'hôte, protocole v7 §1). */
    if (changed(p, g + GS_VIEWPORT, p->c_vp, 16)) {
        a[0] = (unsigned long)I32(g, GS_VIEWPORT);
        a[1] = (unsigned long)I32(g, GS_VIEWPORT + 4);
        a[2] = (unsigned long)I32(g, GS_VIEWPORT + 8);
        a[3] = (unsigned long)I32(g, GS_VIEWPORT + 12);
        if ((long)a[2] < 0) a[2] = 0;
        if ((long)a[3] < 0) a[3] = 0;
        send_cmd(p, QGPU_OP_VIEWPORT, QGPU_LEN_VIEWPORT, a);
    }
    if (changed(p, g + GS_DEPTH_NEAR_F, p->c_dr, 8)) {
        d[0] = fbits(clamp01(GLD_F32(g, GS_DEPTH_NEAR_F)));
        d[1] = fbits(clamp01(GLD_F32(g, GS_DEPTH_NEAR_F + 4)));
        send_cmd(p, QGPU_OP_DEPTH_RANGE, QGPU_LEN_DEPTH_RANGE, d);
    }
}

/* Tout l'état géométrique, en n'envoyant que ce qui a changé. */
static void geom_send_all(PCtx *p, unsigned long fmt)
{
    geom_send_viewport(p);
    geom_send_matrices(p, fmt);
    if (GLD_U8(gls(p), GS_LIGHTING))
        geom_send_lights(p);
    geom_send_texgen(p, fmt);
    geom_send_clip(p);
    geom_send_current(p, fmt);
    p->g_sent = 1;
}

/* ─────────────────────── les deux procédures de GLEngine ─────────────────────── */

/* Combien de sommets peut-on offrir pour un pas de `words` mots ?
 * Plus on en offre, plus GLEngine groupe : la scène « lit » passe de lots de
 * 192 (son ancien plafond, imposé par le tampon du pilote Rage 128) à un lot
 * par glBegin. POMPPC_GL_GEOM_SLOTS force un petit plafond : c'est ce qui a
 * servi à établir comment GLEngine coupe une bande de 1000 sommets. */
static unsigned long geom_slots(unsigned long words)
{
    static long cap = -1;
    unsigned long avail = VTX_LIMIT - (VTX_OFF + G.vtx);
    unsigned long n = avail / (words * 4);
    unsigned long ni;
    if (cap < 0) {
        const char *e = getenv("POMPPC_GL_GEOM_SLOTS");
        cap = (e && *e) ? atol(e) : GEOM_MAX_SLOTS;
        if (cap < 4 || cap > GEOM_MAX_SLOTS)
            cap = GEOM_MAX_SLOTS;
    }
    /* La fusion peut écrire jusqu'à 3 indices par sommet (ruban, éventail,
       polygone) : on n'offre jamais plus de sommets que la zone d'indices ne
       peut en suivre, sinon un lot arriverait sans pouvoir être fusionné —
       et le vidage se déciderait APRÈS que GLEngine a commencé à écrire. */
    ni = (IDX_SIZE - G.idx) / 6;
    if (n > ni)
        n = ni;
    return n > (unsigned long)cap ? (unsigned long)cap : n;
}

/* +0x50 BeginPrimitiveBuffer(ctx, mode, &n) -> tampon où écrire les sommets.
 * Rendre 0 est INTERDIT (GLEngine écrirait à l'adresse nulle) : quand on doit
 * refuser, on rend un tampon de secours et on jette la primitive. */
static void *geom_begin(void *ctx, short mode, unsigned long *n)
{
    PCtx *p;
    TexInfo ti;
    unsigned char *gc;
    unsigned long words, slots;

    pthread_mutex_lock(&G.mu);
    G.pend = 0;
    G.pend_drop = 1;
    p = find_ctx(ctx);
    if (!p || !geom_ok(p) || !ensure_surface(p) || !texture_ok(p, &ti)) {
        if (p) {
            no(NO_G_LATE, (unsigned long)(unsigned short)mode, p->geom_on);
            p->geom_lost = 1;           /* exact à partir du prochain dispatch */
            G.n_geomdrop++;
        }
        goto refuse;
    }
    /* Ce que GLEngine a VRAIMENT retenu : si le pas ou le descripteur ne sont
       pas les nôtres, c'est son sommet interne (déjà transformé) qui arrive —
       le lire comme de la géométrie brute donnerait n'importe quoi. */
    gc = gctx_of(p);
    words = p->geom_words;
    if (!gc || !words || GLD_U16(gc, GC_VTX_STRIDE) != words * 4 ||
        GLD_U32(gc, GC_VTX_DESC) != (unsigned long)p->desc) {
        no(NO_G_STRIDE, gc ? GLD_U16(gc, GC_VTX_STRIDE) : 0, words * 4);
        p->geom_lost = 1;
        G.n_geomdrop++;
        goto refuse;
    }
    check_draw_buffer(p);
    sync_to_host(p, 1, GLD_U8(gls(p), GS_DEPTH_TEST) || stencil_active(p));
    send_state(p, &ti, 1);
    geom_send_all(p, p->geom_fmt);
    /* Plus aucun vidage entre ici et EndPrimitiveBuffer : GLEngine écrit dans
       la fenêtre partagée pendant ce temps. On fait donc la place maintenant. */
    if (G.ncmd + QGPU_LEN_DRAW_RAW + 4 > CMD_WORDS || geom_slots(words) < GEOM_MIN_SLOTS)
        flush();
    slots = geom_slots(words);
    if (slots < 4)
        goto refuse;                    /* ne devrait pas arriver : 4 Mio de sommets */
    G.pend = p;
    G.pend_half = G.hb;                 /* la moitié ne doit plus changer d'ici End */
    G.pend_off = G.vtx;
    G.pend_words = words;
    G.pend_fmt = p->geom_fmt;
    G.pend_slots = slots;
    G.pend_drop = 0;
    /* L'ombrage décide de l'ordre des indices d'un quadrilatère ; il ne peut
       plus changer entre ici et EndPrimitiveBuffer. */
    G.pend_flat = GLD_U32(gls(p), GS_SHADE_MODEL) == GL_FLAT;
    /* Fil de fer ou points : la fusion recollerait les quadrilatères et les
       polygones en TRIANGLES indexés, et l'hôte tracerait alors les diagonales
       de la décomposition — c'est exactement l'information de contour que la v8
       tient pour DRAW_RAW. On garde donc les primitives telles quelles.
       Vu en vrai (scène « polymode ») : sans ceci, la diagonale du
       quadrilatère en fil de fer est tracée, et 3 % de l'image diffère. */
    G.pend_wire = U16(gls(p), GS_POLY_MODE) != GL_FILL ||
                  U16(gls(p), GS_POLY_MODE + 2) != GL_FILL;
    /* la zone est prise tout de suite : un autre fil ne doit pas la réutiliser */
    G.vtx += slots * words * 4;
    if (n)
        *n = slots;
    pthread_mutex_unlock(&G.mu);
    return G.win + VTX_OFF + G.pend_off;

refuse:
    {
        unsigned long st = (p && p->geom_words) ? p->geom_words * 4 : GLD_VERTEX_SIZE;
        unsigned long ns = sizeof(geom_scratch) / st;
        if (ns > 1024)
            ns = 1024;
        if (n)
            *n = ns;
    }
    pthread_mutex_unlock(&G.mu);
    return geom_scratch;
}

/* ───────────────────── fusion des dessins consécutifs ─────────────────────
 *
 * Marble Blast envoie ~8 200 sommets par image en ~1 065 DRAW_RAW : 7,7 sommets
 * par dessin, des rubans et des éventails COURTS. Chaque DRAW_RAW coûte à
 * l'hôte un gl_target complet (liaison du FBO, remise à plat de tout l'état
 * géométrique) plus un appel de dessin. Bout à bout, seules les primitives
 * INDÉPENDANTES se recollent (TRIANGLES, QUADS, LINES, POINTS) ; un ruban ou un
 * éventail, non.
 *
 * La fusion les convertit donc en GL_TRIANGLES INDEXÉS : les sommets restent
 * EXACTEMENT où GLEngine les a écrits (toujours zéro recopie de sommet), et on
 * n'écrit que des indices u16 — 2 octets par sommet de triangle contre 44 pour
 * un sommet recopié. Un seul DRAW_RAW sort alors par lot d'état.
 *
 * Ce qui se fusionne : des lots CONSÉCUTIFS (jamais de réordonnancement : le
 * mélange et l'égalité de profondeur dépendent de l'ordre), du même contexte,
 * du même format de sommet, aux sommets CONTIGUS dans la zone partagée. Tout
 * changement d'état coupe la série de lui-même : il passe par send_cmd →
 * reserve → close_raw.
 *
 * LE SOMMET PROVOQUANT. En ombrage plat, la couleur du triangle vient d'un
 * sommet précis, et ce n'est pas le même selon le mode (qgpu-soft.c, assemble) :
 *   TRIANGLES, STRIP, FAN : le DERNIER sommet du triangle ;
 *   QUADS                 : le 4ᵉ sommet du quadrilatère, pour ses DEUX triangles ;
 *   QUAD_STRIP            : le sommet i+3 du quadrilatère (i, i+1, i+3, i+2) ;
 *   POLYGON               : le PREMIER sommet du polygone, pour tous ses triangles.
 * Comme GL_TRIANGLES prend le dernier, on ordonne chaque triangle pour que son
 * dernier indice SOIT le sommet provoquant. Une rotation circulaire suffit
 * partout (même triangle, même orientation, donc même élimination de face)
 * SAUF pour GL_QUADS, dont la diagonale de référence (0–2) laisse le premier
 * triangle sans le sommet 3 : en ombrage PLAT on coupe alors sur la diagonale
 * 1–3, ce qui est exact puisque la couleur du quadrilatère y est uniforme. En
 * ombrage lisse on garde la diagonale 0–2, celle du backend de référence.
 *
 * L'ALTERNANCE DES RUBANS. Un triangle sur deux d'un GL_TRIANGLE_STRIP est
 * retourné pour garder l'orientation ; les indices la reproduisent, sans quoi
 * GL_CULL_FACE éliminerait un triangle sur deux (vu en vrai avant correction).
 */

/* POMPPC_GL_MERGE : 0 coupe la fusion (repli et comparaison), 1 par défaut. */
static int merge_switch(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("POMPPC_GL_MERGE");
        v = (e && *e) ? atoi(e) : 1;
        if (v < 0)
            v = 0;
    }
    return v;
}

/* Un mode dont deux lots consécutifs se recollent bout à bout sans changer le
 * dessin : les primitives indépendantes. Une bande, un éventail, un polygone ou
 * une boucle ne se fusionnent pas ainsi — elles passent par les indices. */
static int mode_mergeable(unsigned long m)
{
    return m == QGPU_PRIM_MODE_POINTS || m == QGPU_PRIM_MODE_LINES ||
           m == QGPU_PRIM_MODE_TRIANGLES || m == QGPU_PRIM_MODE_QUADS;
}

/* Les modes qui donnent des triangles, donc convertibles en TRIANGLES indexés.
 * Les lignes et les points n'en sont pas ; les bandes et boucles de lignes non
 * plus (elles se convertiraient en LINES, mais le pointillé court le long de la
 * bande : on ne touche pas). */
static int mode_tris(unsigned long m)
{
    return m == QGPU_PRIM_MODE_TRIANGLES || m == QGPU_PRIM_MODE_TRIANGLE_STRIP ||
           m == QGPU_PRIM_MODE_TRIANGLE_FAN || m == QGPU_PRIM_MODE_QUADS ||
           m == QGPU_PRIM_MODE_QUAD_STRIP || m == QGPU_PRIM_MODE_POLYGON;
}

/* Combien d'indices la conversion d'un lot de n sommets produira-t-elle ? */
static unsigned long tris_nidx(unsigned long m, unsigned long n)
{
    switch (m) {
    case QGPU_PRIM_MODE_TRIANGLES:      return (n / 3) * 3;
    case QGPU_PRIM_MODE_TRIANGLE_STRIP:
    case QGPU_PRIM_MODE_TRIANGLE_FAN:
    case QGPU_PRIM_MODE_POLYGON:        return n >= 3 ? (n - 2) * 3 : 0;
    case QGPU_PRIM_MODE_QUADS:          return (n / 4) * 6;
    case QGPU_PRIM_MODE_QUAD_STRIP:     return n >= 4 ? (n / 2 - 1) * 6 : 0;
    default:                            return 0;
    }
}

/* Écrit les indices du lot [base, base+n) et rend leur nombre. Les u16 sont
 * écrits tels quels : le PowerPC est gros-boutiste, comme le protocole. */
static unsigned long tris_emit(unsigned short *o, unsigned long m,
                               unsigned long base, unsigned long n, int flat)
{
    unsigned long i, k = 0;
#define IX(v) (o[k++] = (unsigned short)(base + (v)))
    switch (m) {
    case QGPU_PRIM_MODE_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) {
            IX(i); IX(i + 1); IX(i + 2);
        }
        break;
    case QGPU_PRIM_MODE_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) {                /* un triangle sur deux est retourné */
                IX(i + 1); IX(i); IX(i + 2);
            } else {
                IX(i); IX(i + 1); IX(i + 2);
            }
        }
        break;
    case QGPU_PRIM_MODE_TRIANGLE_FAN:
        for (i = 1; i + 1 < n; i++) {
            IX(0); IX(i); IX(i + 1);
        }
        break;
    case QGPU_PRIM_MODE_POLYGON:
        /* provoquant = sommet 0 : on tourne (0, i, i+1) en (i, i+1, 0) */
        for (i = 1; i + 1 < n; i++) {
            IX(i); IX(i + 1); IX(0);
        }
        break;
    case QGPU_PRIM_MODE_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            if (flat) {                 /* diagonale 1–3 : les deux finissent par 3 */
                IX(i); IX(i + 1); IX(i + 3);
                IX(i + 1); IX(i + 2); IX(i + 3);
            } else {                    /* diagonale 0–2, celle de qgpu-soft.c */
                IX(i); IX(i + 1); IX(i + 2);
                IX(i); IX(i + 2); IX(i + 3);
            }
        }
        break;
    case QGPU_PRIM_MODE_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            /* quadrilatère (i, i+1, i+3, i+2), provoquant i+3 : la diagonale de
               référence i–(i+3) le contient déjà, une rotation suffit. */
            IX(i); IX(i + 1); IX(i + 3);
            IX(i + 2); IX(i); IX(i + 3);
        }
        break;
    default:
        break;
    }
#undef IX
    return k;
}

/* Place pour k indices de plus dans la série courante ? */
static unsigned short *idx_room(unsigned long k)
{
    if (G.idx + k * 2 > IDX_SIZE)
        return 0;
    return (unsigned short *)(G.win + IDX_OFF + G.idx);
}

/* Convertit la série NON indexée déjà ouverte en TRIANGLES indexés. 0 si la
 * place manque — l'appelant ferme alors la série et en ouvre une neuve. */
static int raw_promote(void)
{
    unsigned long n = (G.raw_vend - G.raw_start) / (G.raw_words * 4);
    unsigned long need = tris_nidx(G.raw_mode, n);
    unsigned short *o;
    /* L'alignement se fait ICI et NULLE PART AILLEURS. Le cœur refuse un `ioff`
       qui n'est pas multiple de 4 (in_shmem), mais tous les indices d'une série
       sont lus d'affilée depuis ce seul offset : aligner au milieu glisserait
       deux octets de bourrage dans le tableau, et les triangles pointeraient
       sur les sommets du lot voisin (vu en vrai : un polygone sur trois faux). */
    G.idx = (G.idx + 3) & ~3UL;
    o = idx_room(need);
    if (!o)
        return 0;
    G.raw_idx = IDX_OFF + G.idx;
    G.raw_nidx = tris_emit(o, G.raw_mode, 0, n, G.pend_flat);
    G.idx += G.raw_nidx * 2;
    G.raw_mode = QGPU_PRIM_MODE_TRIANGLES;
    G.raw_count = G.raw_nidx;
    return 1;
}

/* Ajoute le lot qui vient d'être écrit (mode m, n sommets en G.pend_off) à la
 * série ouverte, en TRIANGLES indexés. 0 = impossible : l'appelant ferme la
 * série et en ouvre une neuve, ce qui est toujours exact. */
static int merge_batch(unsigned long m, unsigned long n, unsigned long words)
{
    unsigned long base = (G.pend_off - G.raw_start) / (words * 4);
    unsigned long need = tris_nidx(m, n);
    unsigned short *o;

    if (base + n > 65536)               /* les indices sont des u16 */
        return 0;
    if (!G.raw_idx && !raw_promote())
        return 0;
    /* `n` de DRAW_RAW, c'est le nombre d'INDICES quand la série est indexée :
       il est borné par QGPU_MAX_VERTS dans le cœur. */
    if (G.raw_nidx + need > QGPU_MAX_VERTS)
        return 0;
    o = idx_room(need);
    if (!o)
        return 0;
    G.idx += tris_emit(o, m, base, n, G.pend_flat) * 2;
    G.raw_nidx += need;
    G.raw_count = G.raw_nidx;
    G.raw_lots++;
    return 1;
}

/* +0x54 EndPrimitiveBuffer(ctx, drapeau, mode, n) : GLEngine a écrit n sommets
 * dans le tampon rendu par +0x50. Il n'y a plus rien qui puisse échouer ici. */
static void geom_end(void *ctx, long flag, short mode, long n)
{
    PCtx *p;
    unsigned long m = (unsigned long)(unsigned short)mode;
    unsigned long words;
    int same;

    (void)flag;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (!G.pend || G.pend != p || G.pend_drop) {
        G.pend = 0;
        pthread_mutex_unlock(&G.mu);
        return;
    }
    /* La moitié a changé sous les pieds de GLEngine : impossible par
       construction (flush() n'alterne pas tant que G.pend est ouvert, et
       submit_cur soumet alors en synchrone), mais si cela arrivait, les sommets
       écrits ne sont plus ceux que DRAW_RAW désignerait. On jette. */
    if (G.hb != G.pend_half) {
        no(NO_G_LATE, G.pend_half, G.hb);
        G.n_geomdrop++;
        p->geom_lost = 1;
        G.pend = 0;
        pthread_mutex_unlock(&G.mu);
        return;
    }
    if (n <= 0 || (unsigned long)n > G.pend_slots || m > QGPU_PRIM_MODE_POLYGON) {
        if (n > 0) {                    /* GLEngine a écrit hors de ce qu'on offrait */
            no(NO_G_LATE, m, (unsigned long)n);
            G.n_geomdrop++;
            p->geom_lost = 1;
        }
        G.vtx = G.pend_off;             /* la place réservée est rendue */
        G.pend = 0;
        pthread_mutex_unlock(&G.mu);
        return;
    }
    words = G.pend_words;
    G.vtx = G.pend_off + (unsigned long)n * words * 4;
    /* Les trois conditions de TOUTE fusion : même contexte, même format de
       sommet, sommets CONTIGUS dans la zone partagée. Il n'y a rien à vérifier
       de l'état : tout changement d'état passe par send_cmd → reserve →
       close_raw, donc la série est déjà fermée quand on arrive ici. Et on ne
       fusionne que des lots CONSÉCUTIFS : jamais de réordonnancement, sans quoi
       le mélange et l'égalité de profondeur changeraient l'image. */
    same = (G.raw_ctx == p && G.raw_fmt == G.pend_fmt && G.raw_words == words &&
            G.raw_vend == G.pend_off &&
            (G.raw_vend - G.raw_start) / (words * 4) + (unsigned long)n
                <= GEOM_MAX_MERGE);
    if (same && !G.raw_idx && G.raw_mode == m && mode_mergeable(m)) {
        G.raw_count += n;               /* bout à bout, sans un seul indice */
        G.raw_lots++;
    } else if (same && merge_switch() && !G.pend_wire && mode_tris(m) &&
               (G.raw_idx || mode_tris(G.raw_mode)) &&
               merge_batch(m, (unsigned long)n, words)) {
        /* recollé en TRIANGLES indexés : les sommets n'ont pas bougé */
    } else {
        close_raw();
        G.raw_ctx = p;
        G.raw_start = G.pend_off;
        G.raw_count = n;
        G.raw_fmt = G.pend_fmt;
        G.raw_words = words;
        G.raw_mode = m;
        G.raw_lots = 1;
    }
    G.raw_vend = G.vtx;
    G.n_rawverts += n;
    switch (m) {                        /* triangles équivalents, pour le bilan */
    case QGPU_PRIM_MODE_TRIANGLES:      G.n_tris += n / 3; break;
    case QGPU_PRIM_MODE_TRIANGLE_STRIP:
    case QGPU_PRIM_MODE_TRIANGLE_FAN:
    case QGPU_PRIM_MODE_POLYGON:        G.n_tris += n > 2 ? n - 2 : 0; break;
    case QGPU_PRIM_MODE_QUADS:          G.n_tris += (n / 4) * 2; break;
    case QGPU_PRIM_MODE_QUAD_STRIP:     G.n_tris += n > 3 ? (n / 2 - 1) * 2 : 0; break;
    case QGPU_PRIM_MODE_LINES:          G.n_lines += n / 2; break;
    case QGPU_PRIM_MODE_LINE_STRIP:     G.n_lines += n - 1; break;
    case QGPU_PRIM_MODE_LINE_LOOP:      G.n_lines += n; break;
    default:                            G.n_points += n; break;
    }
    p->color = HOST_NEWER;
    if (writes_depth(p))
        p->depth = HOST_NEWER;
    G.pend = 0;
    pthread_mutex_unlock(&G.mu);
}

/* Procédure à installer dans la table de GLEngine, ou 0. */
void *pomppc_geom_proc(int slot)
{
    if (!G.v7)
        return 0;
    switch (slot) {
    case PROC_BeginPrimitiveBuffer: return (void *)geom_begin;
    case PROC_EndPrimitiveBuffer:   return (void *)geom_end;
    default:                        return 0;
    }
}

/* Bits à ajouter au retour de gldInitDispatch / gldUpdateDispatch :
 *   bit 0 : « le pilote fait la transformation et l'éclairage » ;
 *   bit 1 : « refais le chemin » — sans lui, GLEngine compare (retour & 3) à
 *           gctx+0x7580, trouve égal et saute tout le bloc, dont la relecture
 *           de cfg+0x11c. Il n'est donc nécessaire que si le descripteur a
 *           changé SANS que le verrou change. */
long pomppc_geom_dispatch(void *ctx)
{
    PCtx *p;
    long bits = 0;

    if (!G.v7)
        return 0;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && geom_ok(p)) {
        if (geom_publish(p))
            p->desc_dirty = 1;
        bits = p->desc_dirty ? 3 : 1;
        p->desc_dirty = 0;
        p->geom_on = 1;
    } else if (p) {
        p->geom_on = 0;
    }
    pthread_mutex_unlock(&G.mu);
    return bits;
}

/* Appelé par gldCreateContext, une fois le bloc de configuration rempli par le
 * GLDriver d'Apple. cfg+0x79 = 1 donne au verrou sa valeur INITIALE ; comme
 * gctx+0x48d0 est encore nul à ce moment-là, le premier dispatch doit porter le
 * bit 1 pour que GLEngine aille lire le descripteur (d'où desc_dirty). */
void pomppc_geom_context(void *ctx, void *cfg)
{
    PCtx *p;
    if (!G.v7 || !cfg)
        return;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p) {
        p->cfg = (unsigned char *)cfg;
        p->desc_dirty = 1;
        GLD_U8(p->cfg, 0x79) = 1;
        GLD_U32(p->cfg, 0x11c) = 0;     /* publié au premier dispatch dans le domaine */
    }
    pthread_mutex_unlock(&G.mu);
}

/* ───────────────────────────── triangles ───────────────────────────── */

typedef struct Batch {
    PCtx  *p;
    float  h, zinv;
    int    flat;
    int    fog;                         /* le mot 3 porte le facteur de brouillard */
    int    kind;                        /* RK_* */
} Batch;

/* Prépare un lot de triangles ; 0 = passer par le logiciel. Verrou tenu. */
static void begin_common(PCtx *p, Batch *b, const TexInfo *ti)
{
    check_draw_buffer(p);
    sync_to_host(p, 1, GLD_U8(gls(p), GS_DEPTH_TEST) || stencil_active(p));
    send_state(p, ti, 0);
    b->p = p;
    b->h = (float)p->sh;
    b->zinv = 1.0f / GLD_F32(p->ctx, CTX_DEPTH_SCALE);
    b->flat = GLD_U32(gls(p), GS_SHADE_MODEL) == GL_FLAT;
    b->fog = GLD_U8(gls(p), GS_FOG) != 0;
}

/* Lignes et points : ni texture, ni stipple, ni lissage, ni décalage.
 *
 * Le pointillé de LIGNE reste hors de ce chemin même avec la v8 : l'hôte remet
 * son compteur à zéro à chaque segment d'un `DRAW_LINES`, ce qui est la règle
 * d'OpenGL pour `GL_LINES` mais pas pour un ruban — et le plugin découpe ici
 * les rubans en segments indépendants. Le chemin brut, lui, transmet le mode de
 * primitive et l'hôte tient le compteur correctement : c'est là que le
 * pointillé de ligne est accéléré.
 *
 * Les paramètres de point (GL 1.4) ne sont pas portés par le protocole : avec
 * une atténuation par la distance, GLEngine calcule une taille PAR SOMMET que
 * `QGPU_SK_POINT_SIZE`, qui est une seule valeur d'état, ne peut pas rendre.
 * Sans ce test, les points sortaient à la taille de base — le seul cas de ce
 * lot où le plugin rendait une image fausse en silence. */
static int begin_lp(PCtx *p, Batch *b, int lines)
{
    unsigned char *g;
    const float *att;
    if (!accel_ok(p) || GLD_U8(p->ctx, CTX_TEXTURING) || !ensure_surface(p))
        return 0;
    g = gls(p);
    if (lines ? (GLD_U8(g, GS_LINE_STIPPLE) || GLD_U8(g, GS_LINE_SMOOTH) || GLD_U8(g, GS_POLY_OFS_LINE))
              : (GLD_U8(g, GS_POINT_SMOOTH) || GLD_U8(g, GS_POLY_OFS_PT)))
        return 0;
    att = (const float *)(g + GS_POINT_ATT);
    if (!lines && (att[0] != 1.0f || att[1] != 0.0f || att[2] != 0.0f))
        return no(NO_G_POINT, fbits(att[1]), fbits(att[2]));
    begin_common(p, b, 0);
    b->kind = lines ? RK_LINES : RK_POINTS;
    return 1;
}

static int begin_tris(PCtx *p, Batch *b)
{
    TexInfo ti;
    if (!accel_ok(p))
        return 0;
    if (!ensure_surface(p))
        return no(NO_SURFACE, GLD_U32(p->ctx, CTX_WIDTH), GLD_U32(p->ctx, CTX_HEIGHT));
    if (!texture_ok(p, &ti))
        return 0;
    begin_common(p, b, &ti);
    /* le sommet porte les coordonnées de toutes les unités jusqu'à la
       dernière active (une unité coupée laisse passer la couleur) */
    b->kind = RK_TRI;
    if (ti.u[3].t)      b->kind = RK_TRI_TEX4;
    else if (ti.u[2].t) b->kind = RK_TRI_TEX3;
    else if (ti.u[1].t) b->kind = RK_TRI_TEX2;
    else if (ti.u[0].t) b->kind = RK_TRI_TEX;
    return 1;
}

static void put_vertex(const Batch *b, float *o, const unsigned char *v, const unsigned char *col)
{
    const float *c = (const float *)(col + V_COLOR);
    int u, nu = rk_units[b->kind];
    for (u = 0; u < nu; u++)
        memcpy(o + 8 + 4 * u, v + V_TEX(u), 4 * sizeof(float));
    o[0] = GLD_F32(v, V_X);
    o[1] = b->h - GLD_F32(v, V_Y);
    o[2] = clamp01(GLD_F32(v, V_Z) * b->zinv);
    o[3] = b->fog ? clamp01(GLD_F32(v, V_FOG)) : 1.0f;
    o[4] = clamp01(c[0]);
    o[5] = clamp01(c[1]);
    o[6] = clamp01(c[2]);
    o[7] = clamp01(c[3]);
}

/* Ajoute une primitive de n sommets (3, 2 ou 1) à la série du lot ;
   `prov` = sommet porteur de la couleur en ombrage plat. */
static void prim(Batch *b, int n, const unsigned char **v, const unsigned char *prov)
{
    float *o;
    unsigned long vw = rk_words[b->kind];
    int i;
    /* VTX_LIMIT, pas VTX_END : le haut de la zone porte les indices de la
       fusion, et les deux chemins cohabitent dans la même image (scène mixte). */
    if (VTX_OFF + G.vtx + n * vw * 4 > VTX_LIMIT)
        flush();                        /* l'état GL reste sur le device, par contexte */
    if (G.run_ctx != b->p || G.bound != b->p || G.run_kind != b->kind) {
        reserve(b->p, 0);               /* ferme la série précédente, lie le contexte */
        G.run_ctx = b->p;
        G.run_start = G.vtx;
        G.run_count = 0;
        G.run_kind = b->kind;
    }
    o = (float *)(G.win + VTX_OFF + G.vtx);
    for (i = 0; i < n; i++)
        put_vertex(b, o + i * vw, v[i], b->flat ? prov : v[i]);
    G.vtx += n * vw * 4;
    G.run_count += n;
}

static void tri(Batch *b, const unsigned char *v0, const unsigned char *v1,
                const unsigned char *v2, const unsigned char *prov)
{
    const unsigned char *v[3];
    v[0] = v0; v[1] = v1; v[2] = v2;
    prim(b, 3, v, prov);
    G.n_tris++;
    if (b->kind != RK_TRI)
        G.n_textris++;
}

static void seg(Batch *b, const unsigned char *v0, const unsigned char *v1,
                const unsigned char *prov)
{
    const unsigned char *v[2];
    v[0] = v0; v[1] = v1;
    prim(b, 2, v, prov);
    G.n_lines++;
}

static void pt(Batch *b, const unsigned char *v0)
{
    prim(b, 1, &v0, v0);
    G.n_points++;
}

static void end_tris(Batch *b)
{
    b->p->color = HOST_NEWER;
    if (writes_depth(b->p))
        b->p->depth = HOST_NEWER;
}

/* Repli : synchronise l'invité et marque ce que le logiciel va écrire.
 * `touches_depth` : la procédure lit ou écrit la profondeur. */
static void *fallback(PCtx *p, int slot, int writes_color, int touches_depth)
{
    if (G.state > 0 && p->surf >= 0) {
        sync_to_sw_locked(p, touches_depth);
        check_draw_buffer(p);
        if (writes_color)
            p->color = SW_NEWER;
        /* Seulement si la profondeur de l'hôte a pu être relue : sinon elle
           reste la référence (voir sync_to_sw_locked). */
        if (touches_depth && writes_depth(p) && p->depth == SYNCED)
            p->depth = SW_NEWER;
        /* Requête d'occlusion ouverte : l'hôte ne verra pas ces fragments. On
           majore (voir la section « requêtes d'occlusion ») plutôt que de
           rendre un compte trop petit, qui ferait sauter un objet visible. */
        if (p->q_open >= 0 && (writes_color || touches_depth)) {
            p->q_extra = sat_add(p->q_extra, p->sw * p->sh);
            no(NO_Q_FALLBACK, (unsigned long)slot, p->q_extra);
        }
        G.n_fallback++;
        fb_count[slot]++;
    }
    return p->real[slot];
}

typedef long (*proc4)(void *, void *, long, long);
typedef long (*proc5)(void *, void *, void *, long, long);
typedef long (*proc8)(void *, long, long, long, long, long, long, long);

#define VTX(base, i) ((const unsigned char *)(base) + (i) * GLD_VERTEX_SIZE)

static long a_triangles(void *ctx, void *verts, long n, long flags)
{
    PCtx *p;
    Batch b;
    long i;
    proc4 real;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && begin_tris(p, &b)) {
        for (i = 0; i + 2 < n; i += 3)
            tri(&b, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 2), VTX(verts, i + 2));
        end_tris(&b);
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    real = p ? (proc4)fallback(p, PROC_RenderTriangles, 1, 1) : 0;
    pthread_mutex_unlock(&G.mu);
    return real ? real(ctx, verts, n, flags) : 0;
}

static long a_strip(void *ctx, void *verts, long n, long flags)
{
    PCtx *p;
    Batch b;
    long i;
    proc4 real;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && begin_tris(p, &b)) {
        for (i = 0; i + 2 < n; i++)
            tri(&b, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 2), VTX(verts, i + 2));
        end_tris(&b);
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    real = p ? (proc4)fallback(p, PROC_RenderTriangleStrip, 1, 1) : 0;
    pthread_mutex_unlock(&G.mu);
    return real ? real(ctx, verts, n, flags) : 0;
}

/* Éventail : (ctx, pivot, sommets suivants, n, drapeaux) */
static long a_fan(void *ctx, void *hub, void *verts, long n, long flags)
{
    PCtx *p;
    Batch b;
    long i;
    proc5 real;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && begin_tris(p, &b)) {
        for (i = 0; i + 1 < n; i++)
            tri(&b, (const unsigned char *)hub, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 1));
        end_tris(&b);
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    real = p ? (proc5)fallback(p, PROC_RenderTriangleFan, 1, 1) : 0;
    pthread_mutex_unlock(&G.mu);
    return real ? real(ctx, hub, verts, n, flags) : 0;
}

static long a_quads(void *ctx, void *verts, long n, long flags)
{
    PCtx *p;
    Batch b;
    long i;
    proc4 real;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && begin_tris(p, &b)) {
        for (i = 0; i + 3 < n; i += 4) {
            tri(&b, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 2), VTX(verts, i + 3));
            tri(&b, VTX(verts, i), VTX(verts, i + 2), VTX(verts, i + 3), VTX(verts, i + 3));
        }
        end_tris(&b);
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    real = p ? (proc4)fallback(p, PROC_RenderQuads, 1, 1) : 0;
    pthread_mutex_unlock(&G.mu);
    return real ? real(ctx, verts, n, flags) : 0;
}

static long a_quadstrip(void *ctx, void *verts, long n, long flags)
{
    PCtx *p;
    Batch b;
    long i;
    proc4 real;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && begin_tris(p, &b)) {
        for (i = 0; i + 3 < n; i += 2) {
            tri(&b, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 3), VTX(verts, i + 3));
            tri(&b, VTX(verts, i), VTX(verts, i + 3), VTX(verts, i + 2), VTX(verts, i + 3));
        }
        end_tris(&b);
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    real = p ? (proc4)fallback(p, PROC_RenderQuadStrip, 1, 1) : 0;
    pthread_mutex_unlock(&G.mu);
    return real ? real(ctx, verts, n, flags) : 0;
}

/* Polygone convexe : éventail depuis le premier sommet, qui porte la couleur. */
static long a_polygon(void *ctx, void *verts, long n, long flags)
{
    PCtx *p;
    Batch b;
    long i;
    proc4 real;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && begin_tris(p, &b)) {
        for (i = 1; i + 1 < n; i++)
            tri(&b, VTX(verts, 0), VTX(verts, i), VTX(verts, i + 1), VTX(verts, 0));
        end_tris(&b);
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    real = p ? (proc4)fallback(p, PROC_RenderPolygon, 1, 1) : 0;
    pthread_mutex_unlock(&G.mu);
    return real ? real(ctx, verts, n, flags) : 0;
}

static long a_polygon_ptr(void *ctx, void *vptrs, long n, long flags)
{
    PCtx *p;
    Batch b;
    long i;
    proc4 real;
    const unsigned char **pp = (const unsigned char **)vptrs;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && begin_tris(p, &b)) {
        for (i = 1; i + 1 < n; i++)
            tri(&b, pp[0], pp[i], pp[i + 1], pp[0]);
        end_tris(&b);
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    real = p ? (proc4)fallback(p, PROC_RenderPolygonPtr, 1, 1) : 0;
    pthread_mutex_unlock(&G.mu);
    return real ? real(ctx, vptrs, n, flags) : 0;
}

/* ───────────────────────────── lignes et points ───────────────────────────── */

typedef long (*lp_fn)(void *, void *, long, long);

/* Enveloppe commune : `body` émet les primitives si le lot a pu s'ouvrir. */
#define LP_PROC(name, slot, lines, body)                                        \
static long name(void *ctx, void *verts, long n, long flags)                     \
{                                                                               \
    PCtx *p;                                                                    \
    Batch b;                                                                    \
    long i;                                                                     \
    lp_fn real;                                                                 \
    pthread_mutex_lock(&G.mu);                                                  \
    p = find_ctx(ctx);                                                          \
    if (p && begin_lp(p, &b, lines)) {                                          \
        body                                                                    \
        p->color = HOST_NEWER;                                                  \
        if (writes_depth(p))                                                    \
            p->depth = HOST_NEWER;                                              \
        pthread_mutex_unlock(&G.mu);                                            \
        return 0;                                                               \
    }                                                                           \
    real = p ? (lp_fn)fallback(p, slot, 1, 1) : 0;                              \
    pthread_mutex_unlock(&G.mu);                                                \
    return real ? real(ctx, verts, n, flags) : 0;                               \
}

#define PP(i) (((const unsigned char **)verts)[i])

/* segments indépendants : la couleur plate est celle du second sommet */
LP_PROC(a_lines, PROC_RenderLines, 1,
    for (i = 0; i + 1 < n; i += 2)
        seg(&b, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 1));)
LP_PROC(a_linestrip, PROC_RenderLineStrip, 1,
    for (i = 0; i + 1 < n; i++)
        seg(&b, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 1));)
/* boucle : le segment de fermeture prend la couleur du premier sommet */
LP_PROC(a_lineloop, PROC_RenderLineLoop, 1,
    for (i = 0; i + 1 < n; i++)
        seg(&b, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 1));
    if (n > 1)
        seg(&b, VTX(verts, n - 1), VTX(verts, 0), VTX(verts, 0));)
/* (ctx, pointeurs, n, mode) : paires de pointeurs */
LP_PROC(a_lines_ptr, PROC_RenderLinesPtr, 1,
    for (i = 0; i + 1 < n; i += 2)
        seg(&b, PP(i), PP(i + 1), PP(i + 1));)
LP_PROC(a_points, PROC_RenderPoints, 0,
    for (i = 0; i < n; i++)
        pt(&b, VTX(verts, i));)
LP_PROC(a_points_ptr, PROC_RenderPointsPtr, 0,
    for (i = 0; i < n; i++)
        pt(&b, PP(i));)

/* ───────────────────────────── effacement ───────────────────────────── */

static long a_clear(void *ctx, long mask, long c, long d, long e, long f, long g8, long h)
{
    PCtx *p;
    unsigned char *g;
    unsigned long v[QGPU_SK_COUNT], *cmd;
    long host = mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    long rest;
    proc8 real = 0;

    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (!p) {
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    real = (proc8)p->real[PROC_Clear];
    /* le stencil s'efface sur l'hôte quand le contexte en a un que l'on sait suivre */
    if ((mask & GL_STENCIL_BUFFER_BIT) && GLD_U32(p->ctx, CTX_STENCIL_BITS) == 8 &&
        GLD_U32(p->ctx, CTX_DEPTH_BITS) == 32) {
        host |= GL_STENCIL_BUFFER_BIT;
        p->sten_used = 1;
    }
    rest = mask & ~host;
    if (host && accel_ok(p) && ensure_surface(p)) {
        const float *cc;
        int full, color_full, depth_full, ds_written;
        g = gls(p);
        check_draw_buffer(p);
        compute_state(p, 0, v, 0);
        full = !v[QGPU_SK_SCISSOR] ||
               (v[QGPU_SK_SCISSOR_X] == 0 && v[QGPU_SK_SCISSOR_Y] == 0 &&
                v[QGPU_SK_SCISSOR_W] == p->sw && v[QGPU_SK_SCISSOR_H] == p->sh);
        color_full = full && v[QGPU_SK_COLOR_MASK] == 0xF;
        depth_full = full && v[QGPU_SK_DEPTH_WRITE];
        /* Un effacement complet rend l'ancienne copie inutile : pas de téléversement. */
        if ((host & GL_COLOR_BUFFER_BIT) && color_full && p->color == SW_NEWER)
            p->color = SYNCED;
        /* profondeur et stencil partagent leur fraîcheur : l'ancienne copie n'est
           inutile que si TOUT le mot est réécrit */
        if (p->stencil)
            depth_full = depth_full && (host & GL_STENCIL_BUFFER_BIT) &&
                         v[QGPU_SK_STENCIL_WRITE_MASK] == 0xFF;
        if ((host & GL_DEPTH_BUFFER_BIT) && depth_full && p->depth == SW_NEWER)
            p->depth = SYNCED;
        sync_to_host(p, (host & GL_COLOR_BUFFER_BIT) != 0,
                     (host & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0);
        send_state(p, 0, 0);
        cc = (const float *)(g + GS_CLEAR_COLOR);
        cmd = reserve(p, QGPU_LEN_CLEAR);
        cmd[0] = QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR);
        cmd[1] = ((host & GL_COLOR_BUFFER_BIT) ? QGPU_CLEAR_COLOR : 0) |
                 ((host & GL_DEPTH_BUFFER_BIT) ? QGPU_CLEAR_DEPTH : 0) |
                 (((host & GL_STENCIL_BUFFER_BIT) && p->stencil) ? QGPU_CLEAR_STENCIL : 0);
        cmd[2] = (to_u8(cc[3]) << 24) | (to_u8(cc[0]) << 16) | (to_u8(cc[1]) << 8) | to_u8(cc[2]);
        *(float *)&cmd[3] = clamp01((float)F64(g, GS_CLEAR_DEPTH));
        if (host & GL_COLOR_BUFFER_BIT)
            p->color = HOST_NEWER;
        ds_written = ((host & GL_DEPTH_BUFFER_BIT) && v[QGPU_SK_DEPTH_WRITE]) ||
                     ((host & GL_STENCIL_BUFFER_BIT) && p->stencil &&
                      v[QGPU_SK_STENCIL_WRITE_MASK]);
        if (ds_written)
            p->depth = HOST_NEWER;
        G.n_clears++;
        pthread_mutex_unlock(&G.mu);
        /* accumulation (et stencil d'un format non suivi) : par le logiciel */
        return rest ? real(ctx, rest, c, d, e, f, g8, h) : 0;
    }
    fallback(p, PROC_Clear, (mask & GL_COLOR_BUFFER_BIT) != 0,
             (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) != 0);
    if ((((mask & GL_DEPTH_BUFFER_BIT) && GLD_U8(gls(p), GS_DEPTH_MASK)) ||
         (mask & GL_STENCIL_BUFFER_BIT)) && p->surf >= 0)
        p->depth = SW_NEWER;
    pthread_mutex_unlock(&G.mu);
    return real(ctx, mask, c, d, e, f, g8, h);
}

/* ─────────────────────────── présentation directe ───────────────────────────
 *
 * Une application plein écran (menus cachés, fenêtre de la taille de l'écran,
 * au premier plan) n'a pas besoin du WindowServer : l'image relue sur l'hôte
 * est écrite droit dans la mémoire vidéo (CGDisplayBaseAddress), et l'échange
 * d'Apple (gldSwapBuffers → glsSwapBuffers : copie dans la mémoire de la
 * fenêtre puis attente du WindowServer) est remplacé par une procédure vide,
 * comme le gldSwapNoop d'Apple pour un contexte à simple tampon.
 *
 * Vu en vrai (Zenerchi, 800x600) : l'échange attendait le WindowServer 41 %
 * du temps, et le WindowServer passait un tiers d'un cœur à recopier.
 *
 * En FENÊTRE (Marble Blast : un tiers de chaque image dans cette attente), la
 * même écriture directe vise le rectangle de la surface à l'écran, tant que
 * rien ne la recouvre et que le curseur n'y est pas ; la mémoire de la fenêtre
 * est rafraîchie par un échange normal toutes les DIRECT_REFRESH images.
 *
 * Les symboles sont cherchés dans le processus (dlsym) : sans HIToolbox ou
 * CoreGraphics chargés, pas de présentation directe. POMPPC_GL_DIRECT=0 la
 * coupe, =f la limite au plein écran. Les conditions sont réévaluées toutes
 * les DIRECT_RECHECK images. */
typedef struct { unsigned long hi, lo; } Psn;
typedef struct { float x, y, w, h; } CgRect;     /* CGRect de Tiger (32 bits) */
typedef struct { float x, y; } CgPoint;
/* Objet « drawable » de libGLSystem (ctx+4), relevé dans les vidages de trace : */
#define GD_CID   0x80             /* connexion CGS */
#define GD_WID   0x84             /* fenêtre */
#define GD_SID   0x90             /* surface de la fenêtre */
#define DIRECT_RECHECK   10       /* images entre deux réévaluations */
#define DIRECT_REFRESH   90       /* en fenêtre : un échange normal de temps en temps */
static struct {
    int            init, enabled, ok, windowed, over_cursor;
    char           why[96];             /* pourquoi la voie directe est coupée */
    unsigned long  checked_at;
    unsigned char *base;
    unsigned long  rowbytes, w, h;
    long           x, y;                /* position de la surface à l'écran */
    float          cur_x, cur_y;        /* curseur à la dernière vérification */
    unsigned long (*main_display)(void);
    void         *(*base_address)(unsigned long);
    unsigned long (*bytes_per_row)(unsigned long);
    unsigned long (*bits_per_pixel)(unsigned long);
    unsigned long (*pixels_wide)(unsigned long);
    unsigned long (*pixels_high)(unsigned long);
    unsigned char (*menubar_visible)(void);
    short         (*front_process)(Psn *);
    short         (*current_process)(Psn *);
    short         (*same_process)(const Psn *, const Psn *, unsigned char *);
    /* en fenêtre (SPI CoreGraphics de Tiger ; absentes : plein écran seulement) */
    long          (*window_bounds)(long cid, long wid, CgRect *);
    long          (*surface_bounds)(long cid, long wid, long sid, CgRect *);
    long          (*onscreen_list)(long cid, long owner, long max, long *list, long *count);
    long          (*cursor_location)(long cid, CgPoint *);
    long          (*cursor_visible)(void);
    long          (*surface_list)(long cid, long wid, long max, long *list, long *count);
    long          (*window_alpha)(long cid, long wid, float *alpha);
} D;

static void direct_noop(void)
{
}

static const char *direct_why(void)
{
    return D.why;
}

static void direct_init(void)
{
    const char *e = getenv("POMPPC_GL_DIRECT");
    D.init = 1;
    D.enabled = !(e && e[0] == '0');
    D.windowed = !(e && e[0] == 'f');   /* POMPPC_GL_DIRECT=f : plein écran seulement */
    D.over_cursor = e && e[0] == 'c';   /* =c : même avec un curseur en mouvement */
    D.main_display    = dlsym(RTLD_DEFAULT, "CGMainDisplayID");
    D.base_address    = dlsym(RTLD_DEFAULT, "CGDisplayBaseAddress");
    D.bytes_per_row   = dlsym(RTLD_DEFAULT, "CGDisplayBytesPerRow");
    D.bits_per_pixel  = dlsym(RTLD_DEFAULT, "CGDisplayBitsPerPixel");
    D.pixels_wide     = dlsym(RTLD_DEFAULT, "CGDisplayPixelsWide");
    D.pixels_high     = dlsym(RTLD_DEFAULT, "CGDisplayPixelsHigh");
    D.menubar_visible = dlsym(RTLD_DEFAULT, "IsMenuBarVisible");
    D.front_process   = dlsym(RTLD_DEFAULT, "GetFrontProcess");
    D.current_process = dlsym(RTLD_DEFAULT, "GetCurrentProcess");
    D.same_process    = dlsym(RTLD_DEFAULT, "SameProcess");
    D.window_bounds   = dlsym(RTLD_DEFAULT, "CGSGetWindowBounds");
    D.surface_bounds  = dlsym(RTLD_DEFAULT, "CGSGetSurfaceBounds");
    D.onscreen_list   = dlsym(RTLD_DEFAULT, "CGSGetOnScreenWindowList");
    D.cursor_location = dlsym(RTLD_DEFAULT, "CGSGetCurrentCursorLocation");
    D.cursor_visible  = dlsym(RTLD_DEFAULT, "CGCursorIsVisible");
    D.surface_list    = dlsym(RTLD_DEFAULT, "CGSGetSurfaceList");
    D.window_alpha    = dlsym(RTLD_DEFAULT, "CGSGetWindowAlpha");
    if (!D.main_display || !D.base_address || !D.bytes_per_row || !D.bits_per_pixel ||
        !D.pixels_wide || !D.pixels_high || !D.menubar_visible || !D.front_process ||
        !D.current_process || !D.same_process) {
        if (D.enabled)
            pomppc_log("POMPPC: présentation directe indisponible (symboles absents)\n");
        D.enabled = 0;
    }
    if (!D.window_bounds || !D.surface_bounds || !D.onscreen_list || !D.cursor_location ||
        !D.cursor_visible)
        D.windowed = 0;
}

static float window_alpha(long cid, long wid)
{
    float a = 1.0f;
    if (D.window_alpha && D.window_alpha(cid, wid, &a) != 0)
        a = 1.0f;
    return a;
}

/* Fenêtre à l'écran mais entièrement transparente (vu en vrai : 128x128 en 0,0). */
static int window_invisible(long cid, long wid)
{
    return window_alpha(cid, wid) < 0.004f;
}

static int rects_touch(long ax, long ay, long aw, long ah, const CgRect *r)
{
    return (float)ax < r->x + r->w && r->x < (float)(ax + aw) &&
           (float)ay < r->y + r->h && r->y < (float)(ay + ah);
}

/* En fenêtre : la surface GL est-elle entièrement visible à l'écran, sans
 * rien au-dessus d'elle (autre fenêtre, Dock, barre des menus) ni curseur
 * dessiné dedans ? (Le curseur de la VGA de QEMU est composé en logiciel par
 * le WindowServer dans la mémoire vidéo : écrire par-dessus l'effacerait.)
 * Remplit D.x / D.y. */
static int direct_window_ok(PCtx *p)
{
    unsigned char *gd = (unsigned char *)GLD_U32(p->ctx, 4);
    long cid, wid, sid, list[96], n = 0, i;
    CgRect wr, sr, o;
    CgPoint cur;

#define WHY(...) (snprintf(D.why, sizeof(D.why), __VA_ARGS__), 0)
    if (!D.windowed || !gd)
        return WHY("fenêtre : SPI absentes ou pas de drawable");
    cid = GLD_U32(gd, GD_CID); wid = GLD_U32(gd, GD_WID); sid = GLD_U32(gd, GD_SID);
    {
        long e1 = D.window_bounds(cid, wid, &wr), e2 = D.surface_bounds(cid, wid, sid, &sr);
        if (!e1 && e2 && D.surface_list) {
            /* l'identifiant du drawable n'est pas (toujours) celui de la surface :
               on cherche, parmi les surfaces de la fenêtre, celle qui a notre taille */
            long sl[16], sn = 0, k;
            if (D.surface_list(cid, wid, 16, sl, &sn) == 0 && sn > 0 && sn <= 16)
                for (k = 0; k < sn && e2; k++)
                    if (D.surface_bounds(cid, wid, sl[k], &sr) == 0 &&
                        (unsigned long)sr.w == p->sw && (unsigned long)sr.h == p->sh)
                        e2 = 0;
            if (e2)
                return WHY("surface introuvable : %ld surfaces, 1re %lx (wid %lx sid %lx)",
                           sn, sn > 0 ? sl[0] : 0, wid, sid);
        }
        if (e1 || e2)
            return WHY("bornes refusées : fenêtre %ld, surface %ld (cid %lx wid %lx sid %lx)",
                       e1, e2, cid, wid, sid);
    }
    if ((unsigned long)sr.w != p->sw || (unsigned long)sr.h != p->sh)
        return WHY("surface %gx%g en %g,%g ≠ drawable %lux%lu", sr.w, sr.h, sr.x, sr.y,
                   p->sw, p->sh);
    D.x = (long)(wr.x + sr.x);
    D.y = (long)(wr.y + sr.y);
    if (D.x < 0 || D.y < 0 || D.x + p->sw > D.w || D.y + p->sh > D.h)
        return WHY("dépasse de l'écran (%ld,%ld)", D.x, D.y);
    /* fenêtres à l'écran, de l'avant vers l'arrière : rien au-dessus ne doit toucher */
    if (D.onscreen_list(cid, 0, 96, list, &n) != 0 || n <= 0 || n > 96)
        return WHY("liste des fenêtres refusée (n %ld)", n);
    for (i = 0; i < n && list[i] != wid; i++)
        if (D.window_bounds(cid, list[i], &o) == 0 &&
            /* Une fenêtre qui couvre tout l'écran devant une application au
               premier plan est un voile transparent du système (vu en vrai :
               rang 0, 1024x768) : une vraie fenêtre plein écran d'une autre
               application nous aurait ôté le premier plan. */
            !(o.x <= 0 && o.y <= 0 && o.w >= (float)D.w && o.h >= (float)D.h) &&
            /* Le Dock gare ses tuiles de travail (128x128 au plus, vides) dans
               le coin 0,0 de l'écran, au niveau du Dock (vu en vrai : 13 fenêtres). */
            !(o.x == 0 && o.y == 0 && o.w <= 128 && o.h <= 128) &&
            rects_touch(D.x, D.y, p->sw, p->sh, &o) && !window_invisible(cid, list[i]))
            return WHY("recouverte par la fenêtre %lx (%g,%g %gx%g), rang %ld/%ld, alpha %g",
                       list[i], o.x, o.y, o.w, o.h, i, n, window_alpha(cid, list[i]));
    if (i == n)
        return WHY("fenêtre %lx absente de la liste (%ld fenêtres)", wid, n);
    /* Curseur visible dans la surface : écrire par-dessus l'efface jusqu'à son
       prochain mouvement. Tant qu'il BOUGE (menus, interface), chemin normal ;
       immobile d'une vérification à l'autre (jeu qui le ramène au centre à
       chaque image, comme Marble Blast, ou souris au repos), voie directe. */
    if (!D.over_cursor && D.cursor_visible() && D.cursor_location(cid, &cur) == 0) {
        int still = cur.x == D.cur_x && cur.y == D.cur_y;
        D.cur_x = cur.x; D.cur_y = cur.y;
        o.x = cur.x - 32; o.y = cur.y - 32; o.w = 64; o.h = 64;
        if (!still && rects_touch(D.x, D.y, p->sw, p->sh, &o))
            return WHY("curseur en mouvement dans la surface (%g,%g)", cur.x, cur.y);
    }
    D.why[0] = 0;
    return 1;
#undef WHY
}

/* Mémoire vidéo où présenter p, ou 0 (échange normal). Verrou tenu. */
static unsigned char *direct_target(PCtx *p, unsigned long *rowbytes)
{
    if (!D.init)
        direct_init();
    if (!D.enabled)
        return 0;
    if (!D.checked_at || G.n_frames - D.checked_at >= DIRECT_RECHECK) {
        unsigned long did = D.main_display();
        Psn front, me;
        unsigned char same = 0;
        int ok, full;
        D.w = D.pixels_wide(did);
        D.h = D.pixels_high(did);
        D.base = D.base_address(did);
        D.rowbytes = D.bytes_per_row(did);
        ok = D.base && D.bits_per_pixel(did) == 32 && D.rowbytes >= D.w * 4 &&
             D.front_process(&front) == 0 && D.current_process(&me) == 0 &&
             D.same_process(&front, &me, &same) == 0 && same;
        if (!ok)
            snprintf(D.why, sizeof(D.why), "pas au premier plan, ou écran non 32 bits");
        full = ok && p->sw == D.w && p->sh == D.h && !D.menubar_visible();
        if (full) {
            D.x = D.y = 0;
            ok = 1;
        } else {
            ok = ok && direct_window_ok(p);
            if (ok)
                ok = 2;                 /* en fenêtre */
        }
        if (ok != D.ok)
            pomppc_log("POMPPC: présentation directe %s (%lux%lu en %ld,%ld ; écran %lux%lu)\n",
                       ok == 1 ? "plein écran" : ok ? "en fenêtre" : "coupée",
                       p->sw, p->sh, D.x, D.y, D.w, D.h);
        D.ok = ok;
        D.checked_at = G.n_frames ? G.n_frames : 1;
    }
    if (!D.ok)
        return 0;
    /* En fenêtre, la mémoire de la fenêtre côté WindowServer n'est plus à jour :
       un échange normal de temps en temps la rafraîchit (déplacement, Exposé,
       capture d'écran). */
    if (D.ok == 2 && G.n_frames % DIRECT_REFRESH == 0)
        return 0;
    *rowbytes = D.rowbytes;
    return D.base + D.y * D.rowbytes + D.x * 4;
}

/* Échange (procédure 0x60) : présente directement si possible. Verrou tenu.
 * Seulement si l'hôte a l'image la plus récente (sinon, chemin normal).
 *
 * v9 — UNE IMAGE DE DÉCALAGE, ET C'EST VOULU. En asynchrone, la relecture de
 * l'image n part avec sa soumission mais n'est recopiée en mémoire vidéo
 * qu'une fois sa barrière atteinte. Attendre ici serait attendre l'hôte à
 * chaque échange, c'est-à-dire ne rien gagner. On place donc l'attente le plus
 * tard possible : AU DÉBUT DE L'ÉCHANGE SUIVANT. L'écran montre alors l'image
 * n−1 pendant qu'on prépare la n+1 — une image de latence, jamais plus, et
 * l'hôte a eu toute la construction d'une image pour finir la précédente : la
 * barrière est en général déjà atteinte quand on arrive ici.
 *
 * Contrepartie assumée : une application qui CESSE de dessiner laisse sa
 * dernière image en vol jusqu'au prochain point de synchronisation (un
 * glReadPixels, un chemin logiciel, la fermeture du contexte), qui la
 * présente. */
static int present_direct(PCtx *p)
{
    unsigned char *vram;
    unsigned long rowbytes;
    if (G.state <= 0 || p->broken || p->surf < 0 || p->color == SW_NEWER)
        return 0;
    /* L'image PRÉCÉDENTE part maintenant en mémoire vidéo. */
    wait_half(G.cur ^ 1);
    vram = direct_target(p, &rowbytes);
    if (!vram)
        return 0;
    queue_readback_to(p, 0, vram, rowbytes);
    flush();
    G.n_direct++;
    p->direct_at = G.n_frames;
    /* le tampon arrière est indéfini après un échange : la copie hôte reste
       la référence (p->color inchangé) */
    return 1;
}

/* ───────────────────────────── table de procédures ───────────────────────────── */

#define PROC_TRAMP(k) extern void pomppc_proc_##k(void);
PROC_TRAMP(0) PROC_TRAMP(1) PROC_TRAMP(2) PROC_TRAMP(3) PROC_TRAMP(4) PROC_TRAMP(5)
PROC_TRAMP(6) PROC_TRAMP(7) PROC_TRAMP(8) PROC_TRAMP(9) PROC_TRAMP(10) PROC_TRAMP(11)
PROC_TRAMP(12) PROC_TRAMP(13) PROC_TRAMP(14) PROC_TRAMP(15) PROC_TRAMP(16) PROC_TRAMP(17)
PROC_TRAMP(18) PROC_TRAMP(19) PROC_TRAMP(20) PROC_TRAMP(21) PROC_TRAMP(22) PROC_TRAMP(23)
PROC_TRAMP(24) PROC_TRAMP(25) PROC_TRAMP(26) PROC_TRAMP(27) PROC_TRAMP(28) PROC_TRAMP(29)
PROC_TRAMP(30) PROC_TRAMP(31) PROC_TRAMP(32) PROC_TRAMP(33) PROC_TRAMP(34) PROC_TRAMP(35)

static void *const proc_tramps[PROC_COUNT] = {
    pomppc_proc_0, pomppc_proc_1, pomppc_proc_2, pomppc_proc_3, pomppc_proc_4,
    pomppc_proc_5, pomppc_proc_6, pomppc_proc_7, pomppc_proc_8, pomppc_proc_9,
    pomppc_proc_10, pomppc_proc_11, pomppc_proc_12, pomppc_proc_13, pomppc_proc_14,
    pomppc_proc_15, pomppc_proc_16, pomppc_proc_17, pomppc_proc_18, pomppc_proc_19,
    pomppc_proc_20, pomppc_proc_21, pomppc_proc_22, pomppc_proc_23, pomppc_proc_24,
    pomppc_proc_25, pomppc_proc_26, pomppc_proc_27, pomppc_proc_28, pomppc_proc_29,
    pomppc_proc_30, pomppc_proc_31, pomppc_proc_32, pomppc_proc_33, pomppc_proc_34,
    pomppc_proc_35,
};

/* Ce que fait chaque procédure logicielle aux tampons (pour le repli). */
enum { K_NONE = 0, K_READ = 1, K_WRITE = 2, K_DEPTH = 4, K_ACCEL = 8 };

static int proc_kind(int slot)
{
    switch (slot) {
    case PROC_Clear:
    case PROC_RenderTriangles: case PROC_RenderTriangleStrip: case PROC_RenderTriangleFan:
    case PROC_RenderQuads: case PROC_RenderQuadStrip: case PROC_RenderPolygon:
    case PROC_RenderPolygonPtr:
    case PROC_RenderPoints: case PROC_RenderLines: case PROC_RenderLineStrip:
    case PROC_RenderLineLoop: case PROC_RenderPointsPtr: case PROC_RenderLinesPtr:
        return K_ACCEL;
    case PROC_CopyTexSubImage:
    case PROC_Swap58: case PROC_Swap5c: case PROC_Swap60:
        return K_READ;
    case PROC_ReadPixels:                     /* peut lire GL_DEPTH_COMPONENT */
        return K_READ | K_DEPTH;
    case PROC_Accum:
        return K_READ | K_WRITE;
    case PROC_DrawPixels: case PROC_CopyPixels: case PROC_RenderBitmap:
    case PROC_RenderVertexArray:
    case PROC_Proc84: case PROC_Proc88: case PROC_Proc8c:
        return K_READ | K_WRITE | K_DEPTH;
    case PROC_Proc68: case PROC_Proc6c:
        /* glBeginQuery / glEndQuery (relevé v8) : ils ne lisent ni n'écrivent
           aucun tampon, et le GLDriver d'Apple ne les installe même pas. */
        return K_NONE;
    case PROC_ModifyTexSubImage: case PROC_GenerateTexMipmaps:
        return K_NONE | 0x100;            /* suivi de texture seulement (voir plus bas) */
    default:  /* Noop, Begin/EndPrimitiveBuffer, RenderVertexBuffer (vides chez Apple),
                 BufferSubData : n'accèdent pas au tampon de dessin */
        return K_NONE;
    }
}

static void *accel_proc(int slot)
{
    switch (slot) {
    case PROC_Clear:               return a_clear;
    case PROC_RenderTriangles:     return a_triangles;
    case PROC_RenderTriangleStrip: return a_strip;
    case PROC_RenderTriangleFan:   return a_fan;
    case PROC_RenderQuads:         return a_quads;
    case PROC_RenderQuadStrip:     return a_quadstrip;
    case PROC_RenderPolygon:       return a_polygon;
    case PROC_RenderPolygonPtr:    return a_polygon_ptr;
    case PROC_RenderLines:         return a_lines;
    case PROC_RenderLineStrip:     return a_linestrip;
    case PROC_RenderLineLoop:      return a_lineloop;
    case PROC_RenderLinesPtr:      return a_lines_ptr;
    case PROC_RenderPoints:        return a_points;
    case PROC_RenderPointsPtr:     return a_points_ptr;
    case PROC_Proc68:              return G.v8 ? (void *)q_begin : 0;
    case PROC_Proc6c:              return G.v8 ? (void *)q_end : 0;
    default:                       return 0;
    }
}

/* Emplacements que le plugin remplit même si le GLDriver d'Apple n'y a RIEN
 * posé : ce sont ceux où l'on n'a pas besoin de lui pour être exact. Les deux
 * procédures de requête sont dans ce cas (sa table les laisse nulles). */
static int proc_standalone(int slot)
{
    return slot == PROC_Proc68 || slot == PROC_Proc6c;
}

/* Appelé par les trampolines de procédures : synchronise puis rend la cible. */
void *pomppc_proc_pre(int slot, unsigned long *a)
{
    PCtx *p;
    void *target;
    int kind = proc_kind(slot);

    pthread_mutex_lock(&G.mu);
    p = find_ctx((void *)a[0]);
    if (!p) {
        pthread_mutex_unlock(&G.mu);
        pomppc_log("POMPPC: procédure %s pour un contexte inconnu %08lx\n",
                   pomppc_proc_name(slot), a[0]);
        abort();                        /* ne jamais sauter à une adresse nulle */
    }
    target = p->real[slot];
    /* Une image = un échange (0x60). Les vidages 0x58/0x5c (glFlush, glFinish)
       n'en sont pas : Marble Blast en fait un par image, et les compter doublait
       le débit affiché (vu en vrai). */
    if (slot == PROC_Swap60)
        stats_frame();
    if (slot == PROC_Swap60 && present_direct(p)) {
        pthread_mutex_unlock(&G.mu);
        return direct_noop;
    }
    /* glFlush / glFinish d'un contexte qui présente directement : l'application
       attend la fin du dessin, pas une copie dans le tampon invité (l'image
       part en mémoire vidéo à l'échange). Marble Blast fait un glFinish par
       image : sans ceci, chaque image était relue deux fois (vu en vrai). */
    if ((slot == PROC_Swap58 || slot == PROC_Swap5c) && p->direct_at &&
        G.n_frames - p->direct_at <= 1 && p->color == HOST_NEWER) {
        if (G.ncmd)
            flush();
        pthread_mutex_unlock(&G.mu);
        return direct_noop;
    }
    kind &= 0xff;
    if (kind != K_NONE)
        fallback(p, slot, (kind & K_WRITE) != 0, (kind & K_DEPTH) != 0);
    /* (ctx, texture, …) : le contenu d'une texture change */
    if (slot == PROC_ModifyTexSubImage || slot == PROC_CopyTexSubImage ||
        slot == PROC_GenerateTexMipmaps) {
        PTex *t = find_tex((void *)a[1]);
        if (t) {
            t->dirty = 1;
        } else {
            for (t = G.textures; t; t = t->next)
                t->dirty = 1;
        }
    }
    pthread_mutex_unlock(&G.mu);
    return target;
}

/* Ce que le plugin installe dans chaque case (constant pour un processus). */
static void *install_for(int k)
{
    static void *tab[PROC_COUNT];
    static int init;
    if (!init) {
        int i;
        for (i = 0; i < PROC_COUNT; i++) {
            tab[i] = 0;
            /* Chemin brut : Begin/EndPrimitiveBuffer sont prioritaires, et
               doivent être réinstallées à chaque gldUpdateDispatch comme toutes
               les autres (le GLDriver d'Apple réécrit la table entière). */
            if (pomppc_geom_proc(i))
                tab[i] = pomppc_geom_proc(i);
            else if (G.state > 0 && accel_proc(i))
                tab[i] = accel_proc(i);
            else if (pomppc_tracing() || (G.state > 0 && proc_kind(i) != K_NONE))
                tab[i] = proc_tramps[i];
        }
        init = G.state != 0;            /* état du device connu : table définitive */
    }
    return tab[k];
}

/* gldUpdateDispatch appelle ces deux fonctions autour de chaque mise à jour
 * de la table, c'est-à-dire à chaque changement d'état GL : elles doivent
 * rester en temps constant par case (vu en vrai : 15 % du temps de Zenerchi
 * avec une recherche linéaire par case). */
void pomppc_hook_procs(void *ctx, void **procs)
{
    PCtx *p;
    int k;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p) {
        p->procs = procs;
        for (k = 0; k < PROC_COUNT; k++) {
            void *mine = install_for(k);
            if (!mine || procs[k] == mine)
                continue;
            if (!procs[k] && !proc_standalone(k))
                continue;               /* rien à quoi se replier : on s'abstient */
            p->real[k] = procs[k];
            p->mine[k] = mine;
            procs[k] = mine;
        }
    }
    pthread_mutex_unlock(&G.mu);
}

void pomppc_unhook_procs(void *ctx, void **procs)
{
    PCtx *p;
    int k;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p)
        for (k = 0; k < PROC_COUNT; k++)
            if (p->mine[k] && procs[k] == p->mine[k])
                procs[k] = p->real[k];
    pthread_mutex_unlock(&G.mu);
}

/* ───────────────────────────── cycle de vie ───────────────────────────── */

void pomppc_context_created(void *ctx)
{
    PCtx *p = calloc(1, sizeof(*p));
    unsigned long *c;
    if (!p)
        return;
    p->ctx = ctx;
    p->qctx = -1;
    p->surf = -1;
    p->q_open = -1;
    p->color = p->depth = SW_NEWER;
    pthread_mutex_lock(&G.mu);
    if (G.state > 0) {
        p->qctx = alloc_id(&G.ctx_used, G.q.ctx_base, QGPU_CLIENT_CTX_IDS);
        if (p->qctx >= 0) {
            close_run();
            if (G.ncmd + QGPU_LEN_CTX > CMD_WORDS)
                flush();
            c = G.cmd + G.ncmd;
            c[0] = QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX);
            c[1] = p->qctx;
            G.ncmd += QGPU_LEN_CTX;
        } else {
            pomppc_log("POMPPC: plus d'identifiant de contexte qgpu : contexte logiciel\n");
        }
    }
    p->next = G.list;
    G.list = p;
    pthread_mutex_unlock(&G.mu);
    if (pomppc_tracing())
        pomppc_dump("ctx-created", ctx, 0x704);
}

void pomppc_context_destroyed(void *ctx)
{
    PCtx **pp, *p;
    unsigned long *c;
    pthread_mutex_lock(&G.mu);
    for (pp = &G.list; *pp; pp = &(*pp)->next) {
        if ((*pp)->ctx != ctx)
            continue;
        p = *pp;
        *pp = p->next;
        if (p->cfg)
            GLD_U32(p->cfg, 0x11c) = 0;     /* le descripteur meurt avec le contexte */
        if (G.pend == p)
            G.pend = 0;
        if (G.raw_ctx == p) {
            G.raw_ctx = 0;
            G.raw_count = 0;
        }
        if (G.state > 0 && p->qctx >= 0) {
            destroy_surface(p);
            /* Le cœur referme la requête laissée ouverte par un contexte
               détruit, mais l'identifiant, lui, est à nous : on le rend. */
            p->q_open = -1;
            c = reserve(p, QGPU_LEN_CTX);
            c[0] = QGPU_CMD_HDR(QGPU_OP_CTX_DESTROY, QGPU_LEN_CTX);
            c[1] = p->qctx;
            flush();
            /* Rien ne doit rester en vol : les copies différées visent des
               tampons (et une mémoire vidéo) que ce contexte laisse derrière
               lui, et la dernière image présentée directement est là-dedans. */
            drain_all();
            G.ctx_used &= ~(1UL << (p->qctx - G.q.ctx_base));
        }
        if (G.run_ctx == p)
            G.run_ctx = 0;
        free(p);
        break;
    }
    pthread_mutex_unlock(&G.mu);
}

/* Avant que le drawable change : ce que l'hôte a dessiné va dans l'ancien tampon. */
void pomppc_before_buffers_change(void *ctx)
{
    sync_to_sw(ctx, 1);
}

/* Après une mise à jour de la table marquée « tampon de dessin changé »
 * (glDrawBuffer, échange avant/arrière, et aussi chaque CGLFlushDrawable
 * sans changement réel). Synchronisation paresseuse : si l'adresse n'a pas
 * bougé, rien à faire ; sinon ce que l'hôte a dessiné va dans l'ancien tampon,
 * toujours alloué. La profondeur, elle, n'est pas concernée.
 * (Vu en vrai : relire la couleur avant chaque mise à jour coûtait deux
 * relectures de 800x600 par image dans Zenerchi.) */
void pomppc_after_draw_buffer_change(void *ctx)
{
    PCtx *p;
    unsigned char *cur;
    if (G.state <= 0)
        return;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && p->surf >= 0) {
        cur = sw_color(p);
        if (cur != p->draw_seen) {
            int any = p->color == HOST_NEWER && p->draw_seen;
            if (any)
                queue_readback(p, 0, p->draw_seen);
            if (any || G.ncmd) {
                /* L'ancien tampon repart au rendu d'Apple : il doit contenir ce
                   que l'hôte a dessiné AVANT qu'on rende la main (v9). */
                int i = G.cur;
                flush();
                if (any)
                    wait_half(i);
            }
            p->draw_seen = cur;
            p->color = SW_NEWER;
        }
    }
    pthread_mutex_unlock(&G.mu);
}

void pomppc_drawable_attached(void *ctx, long kind, long result)
{
    PCtx *p;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && p->surf >= 0) {
        check_draw_buffer(p);
        p->color = SW_NEWER;
        p->depth = SW_NEWER;
        if (GLD_U32(ctx, CTX_WIDTH) != p->sw || GLD_U32(ctx, CTX_HEIGHT) != p->sh)
            destroy_surface(p);         /* recréée à la bonne taille au prochain dessin */
    }
    pthread_mutex_unlock(&G.mu);
    if (pomppc_tracing()) {
        pomppc_dump("ctx-attached", ctx, 0x704);
        pomppc_dump("gls-drawable", (void *)GLD_U32(ctx, 4), 0x100);
    }
    (void)kind; (void)result;
}

/* ─────────────── identité du renderer (RendererInfo, pixel formats) ───────────────
 *
 * GLEngine rattache un pixel format à son plugin par l'octet 0xff00 de
 * l'identifiant de rendu, qui doit valoir l'identifiant rendu par gldGetVersion
 * (vu en vrai : sans réécriture, CGLCreateContext rend kCGLBadPixelFormat).
 * Le GLDriver d'Apple, lui, attend ses propres identifiants 0x02xx : on
 * réécrit à la sortie, on restaure avant de lui rendre la main.
 *
 *   RendererInfo : +0x04 identifiant, +0x08 drapeaux (0x100 = accéléré)
 *   pixel format : +0x00 suivant, +0x04 identifiant, +0x08 drapeaux (même codage)
 *
 * La demande kCGLPFAAccelerated (73) atteint gldChoosePixelFormat : le
 * GLDriver d'Apple, logiciel, n'y rend alors aucun format (vu en vrai). Elle
 * est retirée de la copie qu'il reçoit, et nos formats portent le drapeau.
 */
#define RI_ID          0x04
#define RI_FLAGS       0x08
#define RI_ACCELERATED 0x100
#define PF_NEXT        0x00
#define PF_ID          0x04
#define PF_FLAGS       0x08
#define CGL_RENDERER_ID_ATTR 70
#define CGL_ACCELERATED_ATTR 73
/* attributs CGL suivis d'une valeur (CGLTypes.h de 10.4) */
static int attr_has_value(long a)
{
    switch (a) {
    case 5: case 8: case 11: case 12: case 13: case 14: case 55: case 56:
    case 70: case 84: case 96:
        return 1;
    default:
        return 0;
    }
}

static unsigned long to_ours(unsigned long id)
{
    return ((id & 0xff00) == APPLE_GENERIC_ID) ? ((id & ~0xff00UL) | POMPPC_PLUGIN_ID) : id;
}

static unsigned long to_apple(unsigned long id)
{
    return ((id & 0xff00) == POMPPC_PLUGIN_ID) ? ((id & ~0xff00UL) | APPLE_GENERIC_ID) : id;
}

void pomppc_patch_renderer_info(unsigned char *info)
{
    GLD_U32(info, RI_ID) = to_ours(GLD_U32(info, RI_ID));
    if (pomppc_accel_enabled())
        GLD_U32(info, RI_FLAGS) |= RI_ACCELERATED;
}

/* Copie traduite pour le GLDriver d'Apple ; 0 si la liste est trop longue. */
int pomppc_translate_attribs(const long *a, long *out, int max)
{
    int i, n = 0;
    for (i = 0; a[i]; i++) {
        if (n + 3 > max)
            return 0;
        if (a[i] == CGL_ACCELERATED_ATTR)
            continue;
        if (a[i] == CGL_RENDERER_ID_ATTR) {
            out[n++] = a[i++];
            out[n++] = to_apple(a[i]);
            continue;
        }
        out[n++] = a[i];
        if (attr_has_value(a[i]))
            out[n++] = a[++i];
    }
    out[n] = 0;
    return 1;
}

void pomppc_patch_pixel_format(void *pf)
{
    if (pf) {
        GLD_U32(pf, PF_ID) = to_ours(GLD_U32(pf, PF_ID));
        if (pomppc_accel_enabled())
            GLD_U32(pf, PF_FLAGS) |= RI_ACCELERATED;
    }
}

void pomppc_unpatch_pixel_format(void *pf)
{
    if (pf) {
        GLD_U32(pf, PF_ID) = to_apple(GLD_U32(pf, PF_ID));
        GLD_U32(pf, PF_FLAGS) &= ~RI_ACCELERATED;
    }
}

void pomppc_patch_pixel_list(void *pf)
{
    int n = 0;
    for (; pf && n < 256; pf = (void *)GLD_U32(pf, PF_NEXT), n++)
        pomppc_patch_pixel_format(pf);
}

void pomppc_unpatch_pixel_list(void *pf)
{
    int n = 0;
    for (; pf && n < 256; pf = (void *)GLD_U32(pf, PF_NEXT), n++)
        pomppc_unpatch_pixel_format(pf);
}

/* ─── Sonde : forcer une limite du bloc de configuration (relevé seulement) ───
 * POMPPC_GL_TRY3D=n déclare une taille maximale de texture 3D, que le GLDriver
 * d'Apple laisse à 0. Cela sert à répondre par l'expérience à « le repli
 * logiciel sait-il faire les textures 3D d'OpenGL 1.2 ? » — question dont
 * dépend la version que l'on peut annoncer. Jamais actif par défaut. */
static void caps_probe(unsigned char *cfg)
{
    const char *e;
    /* POMPPC_GL_ALLEXT=1 : allume les 79 bits d'extensions. C'est l'expérience
       V3 de docs/re/capacites-glengine.md §9 — GLEngine émet les noms DANS
       L'ORDRE DES BITS, donc la liste lue donne la table bit → nom sans la
       moindre déduction. Sonde seulement, jamais livrée active. */
    e = getenv("POMPPC_GL_ALLEXT");
    if (e && e[0] == '1') {
        GLD_U32(cfg, 0x124) = 0xFFFFFFFFUL;
        GLD_U32(cfg, 0x128) = 0xFFFFFFFFUL;
        GLD_U32(cfg, 0x12c) = 0x00007FFFUL;   /* bits 64..78 */
    }
    e = getenv("POMPPC_GL_TRY3D");
    if (e && *e) {
        unsigned long n = (unsigned long)atoi(e);
        if (n < 1 || n > 4096)
            n = 256;
        GLD_U16(cfg, 0xbe) = (unsigned short)n;     /* GL_MAX_3D_TEXTURE_SIZE */
        GLD_U16(cfg, 0xc0) = (unsigned short)n;
    }
}

/* ─── Tâche 4.1 : n'annoncer QUE ce que la chaîne tient ───────────────────────
 *
 * `GL_EXTENSIONS` n'est pas une chaîne du pilote : GLEngine la fabrique à
 * partir de 25 noms fixes (les extensions qu'il réalise lui-même) et d'un
 * TABLEAU DE 79 BITS que le pilote pose en cfg+0x124..0x12c
 * (docs/re/capacites-glengine.md §3). Les pilotes font un `|=` : on ajoute des
 * bits, on n'en retire jamais.
 *
 * Ce que l'on ajoute, et pourquoi (relevé complet et preuves dans
 * docs/re/version-extensions.md, scène « v15 ») :
 *
 *   bit 17 GL_ARB_occlusion_query        tenu par NOUS (v8) : GLEngine appelle
 *                                        gldCreateQuery / +0x68 / +0x6c /
 *                                        gldGetQueryInfo, que le rendu d'Apple
 *                                        laisse en bouchon. Annoncé seulement
 *                                        si le device est un v8.
 *   bit 20 GL_ARB_vertex_buffer_object   tenu par GLEngine lui-même (mesuré :
 *                                        glGenBuffers/glBufferData/glDrawArrays
 *                                        dessinent juste, avec et sans nous).
 *   bit 39 GL_EXT_blend_func_separate    mesuré tenu, et accéléré : les quatre
 *                                        facteurs partent depuis la v2.
 *
 * Les NUMÉROS DE BIT viennent de l'expérience, pas de la table lue : les 79
 * bits allumés d'un coup (POMPPC_GL_ALLEXT=1) donnent la liste dans l'ordre des
 * bits, et elle corrige la table de docs/re/capacites-glengine.md §3.2 à partir
 * du bit 24 (table corrigée : docs/re/version-extensions.md §4). La première
 * version de ce code annonçait ainsi GL_EXT_texture_rectangle et
 * GL_EXT_secondary_color — deux extensions que la chaîne NE TIENT PAS — en
 * croyant poser GL_EXT_texture_env_add et GL_EXT_blend_func_separate.
 *
 * Ce que l'on N'AJOUTE PAS, bien que le nom soit tentant : les textures 3D et
 * les cartes de cube (GLEngine rend GL_INVALID_VALUE), la compression S3TC
 * (aucune erreur, mais l'image est fausse), la couleur secondaire
 * (GL_COLOR_SUM n'ajoute rien), GL_MIRRORED_REPEAT (traité comme GL_REPEAT),
 * les textures de profondeur et le multiéchantillonnage. Tout cela est mesuré,
 * pas supposé.
 *
 * Les LIMITES d'Apple sont laissées telles quelles : 8 unités de texture et
 * 4096 de côté. Le chemin accéléré n'en tient que 4 et 2048 — au-delà, le
 * plugin refuse proprement et le rendu d'Apple reprend la main, ce qui est
 * exact (vérifié : « multitexture 8 unites » et « texture 4096 » de la scène
 * « v15 » rendent la bonne image avec le plugin). Annoncer moins serait mentir
 * dans l'autre sens.
 */
static void caps_extensions(unsigned char *cfg)
{
    unsigned long w0 = 0, w1 = 0;
    if (G.v8)
        w0 |= 1UL << 17;                /* GL_ARB_occlusion_query */
    w0 |= 1UL << 20;                    /* GL_ARB_vertex_buffer_object */
    w1 |= 1UL << (39 - 32);             /* GL_EXT_blend_func_separate */
    GLD_U32(cfg, 0x124) |= w0;
    GLD_U32(cfg, 0x128) |= w1;
}

void pomppc_patch_caps(void *cfg)
{
    if (G.state <= 0 || !cfg)
        return;
    caps_probe((unsigned char *)cfg);
    {   /* POMPPC_GL_ANNOUNCE=0 : laisser l'annonce d'Apple intacte (comparaison) */
        const char *e = getenv("POMPPC_GL_ANNOUNCE");
        if (e && e[0] == '0')
            return;
    }
    caps_extensions((unsigned char *)cfg);
}

const char *pomppc_override_string(long name, const char *apple)
{
    static char renderer[64];
    if (G.state <= 0)
        return apple;
    switch (name) {
    case 0x1f00:
        return "POMPPC";
    case 0x1f01:
        if (!renderer[0])
            snprintf(renderer, sizeof(renderer), "POMPPC qgpu (%s host GPU)",
                     (G.q.caps & QGPU_CAP_GL) ? "OpenGL" : "software");
        return renderer;
    case 0x1f02:
        /* GL_VERSION sort TEL QUEL de gldGetString : GLEngine ne le recoupe ni
           avec les bits d'extensions ni avec les limites (relevé
           docs/re/capacites-glengine.md §2). Autrement dit, c'est un simple
           strcpy — et donc une promesse que rien ne vérifie. On y met la plus
           haute version dont TOUTES les fonctions sont tenues, et elle est 1.1 :
           OpenGL 1.2 exige les textures 3D, que GLEngine refuse
           (GL_MAX_3D_TEXTURE_SIZE = 0, glTexImage3D → GL_INVALID_VALUE) et que
           le protocole qgpu ne porte pas non plus. Le détail, fonction par
           fonction, est dans docs/re/version-extensions.md ; ce qui manque pour
           1.2, 1.3, 1.4 et 1.5 y est nommé. Le suffixe, lui, dit qui rend. */
        if (getenv("POMPPC_GL_ANNOUNCE") && getenv("POMPPC_GL_ANNOUNCE")[0] == '0')
            return apple;
        return "1.1 POMPPC-1.0";
    default:
        return apple;
    }
}
