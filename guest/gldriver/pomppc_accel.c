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
 *   POMPPC_GL_PRESENT=0   pas de SURF_PRESENT (relecture + copie G4, pour A/B)
 *   POMPPC_GL_STATS=fichier  bilan ajouté au fichier toutes les 5 s (images/s,
 *                         relectures, replis, temps passé à soumettre et à copier)
 *   POMPPC_GL_GEOM=0/1/2  chemin brut : 0 coupé, 1 activé (défaut), 2 activé
 *                         avec un format de sommet fixe et large (mesure)
 *   POMPPC_GL_ARRAY=0/1/2 tableaux de sommets (RenderVertexArray / Buffer) :
 *                         0 coupé (défaut) — GLEngine déroule via Begin/End ;
 *                         1 mixte : GL_VERTEX_ARRAY actif pose cfg+0x78,
 *                         DrawArrays/DrawElements quittent Begin/End ; le
 *                         descripteur cfg+0x11c reste pour glBegin ;
 *                         2 forcé GeForce3 : cfg+0x11c retiré (glBegin jeté
 *                         tant qu'il n'a pas forcé le repli mixte)
 *   POMPPC_GL_ASYNC=0/1   doorbell asynchrone (défaut : activé si le device et
 *                         le kext le tiennent ; voir « soumission » plus bas)
 *   POMPPC_GL_VBO=0       pas de tampons hôte v14 (DRAW_RAW retraverse BAR0) ;
 *                         coupe aussi DRAW_NATIVE, qui en dépend
 *   POMPPC_GL_NATIVE=0    pas de DRAW_NATIVE (v18, QGPU_CAP_NATIVE) : les
 *                         tableaux adossés à des VBO sont de nouveau empaquetés
 *   POMPPC_GL_NATIVE_RANGE=0  ne pas croire la plage (ptr, longueur) de
 *                         gldFlushBuffer : tout le VBO devient sale (seul ce
 *                         que les dessins lisent est recopié, cf. raw_sync)
 *   POMPPC_GL_XFER16=0    pas de transfert 16 bits hôte (v15) : fenêtre 16 bits
 *                         et Z 16 restent sans aller-retour
 *   POMPPC_GL_NOTE=chemin journal d'appoint (gl_note). SANS elle, rien n'est
 *                         écrit : le plugin tourne aussi dans le WindowServer,
 *                         en root. Ouvert une fois, O_EXCL|O_NOFOLLOW.
 *   POMPPC_GLTRACE=dir    trace (voir pomppc_gld.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/ucontext.h>
#include <mach-o/dyld.h>

#include "qgpu_proto.h"
#include "pomppc_gld.h"
#include "pomppc_qgpu.h"

/* v18 — DRAW_NATIVE : dessin depuis des tampons hôte BRUTS (copies telles
   quelles des VBO de l'application), sans empaquetage par l'invité. Contrat
   partagé avec le cœur ; ces définitions provisoires cèdent la place au vrai
   qgpu_proto.h dès qu'il les porte (fusion à faire).
     QGPU_OP_DRAW_NATIVE [mode, count, ibuf, ioff, itype, first, nattr, aoff]
     descripteur (6 mots, nattr à aoff dans BAR0) :
       [code, buf, offset, stride, type GL, taille | QGPU_NATTR_NORMALIZED] */
#ifndef QGPU_OP_DRAW_NATIVE
#define QGPU_OP_DRAW_NATIVE     0x005A
#define QGPU_LEN_DRAW_NATIVE    9
#define QGPU_CAP_NATIVE         0x00000100
#define QGPU_NATTR_WORDS        6
#define QGPU_NATTR_MAX          24
#define QGPU_NATTR_POSITION     0
#define QGPU_NATTR_NORMAL       1
#define QGPU_NATTR_COLOR        2
#define QGPU_NATTR_SEC_COLOR    3
#define QGPU_NATTR_FOG          4
#define QGPU_NATTR_TEX(u)       (8 + (u))
#define QGPU_NATTR_GEN(k)       (16 + (k))
#define QGPU_NATTR_NORMALIZED   0x100
#endif

#define POMPPC_PLUGIN_REV "20260924-native"
static void gl_note(const char *fmt, ...);
static void crash_hook_install(void);
/* 24/09/2026 — garde de lecture des tableaux de l'application. Les copies
   clientes des VBO d'idTech4 (cache de sommets, memory plugin d'Apple) sont
   paginées : la plage [vmin, vmax] d'un glDrawElements peut déborder sur une
   page non mappée (DOOM 3 : SIGSEGV à page+0x34 dans le chemin flottant de
   l'empaqueteur, 2 à 4 min de jeu). Le crochet SIGSEGV/SIGBUS revient ici par
   siglongjmp quand pack_jmp_on est posé, et le lot est refusé (GLEngine le
   dessine lui-même), comme le fait un vrai pilote qui copie de la mémoire
   utilisateur sous garde de faute. */
static sigjmp_buf pack_jmp;
static volatile int pack_jmp_on;
/* Même garde pour les LECTURES DE NIVEAUX de texture (empreinte, copie vers
   l'arène) : DOOM 3 crée _currentRender par glTexImage2D(NULL) et GLEngine
   n'engage pas les pages du niveau — lire ses texels faute (SIGSEGV page
   alignée dans tex_lv0_sig). Tampon SÉPARÉ : tex_lv0_sig est appelée depuis
   la région gardée par pack_jmp. */
static sigjmp_buf sig_jmp;
static volatile int sig_jmp_on;
/* Troisième garde : les procédures de rendu à TABLEAU DE POINTEURS que
   GLEngine appelle après SON T&L (RenderPolygonPtr, LinesPtr, PointsPtr).
   Prey (24/09, chargement du Roadhouse) : SIGSEGV dans a_polygon_ptr, un
   des n pointeurs ne menait nulle part. Sur faute : verrou rendu, primitive
   jetée. Tampon à part. Depuis P2 (relecture du 24/09), armée verrou tenu
   autour des seules lectures de l'application (voir PROC_GUARD_ARM). */
static sigjmp_buf proc_jmp;
static volatile int proc_jmp_on;
static volatile unsigned long proc_fault_n;
/* P3 (relecture du 24/09) : le crochet est global au processus ; chaque garde
   note le fil qui l'arme, et seul ce fil-là y revient par siglongjmp. */
static volatile pthread_t pack_thr, sig_thr, proc_thr;
static volatile unsigned long sig_fault_n;
static int upload_blank;                /* COPY_TEX : niveaux noirs, sans lire l'invité */
static volatile unsigned long pack_fault_addr, pack_fault_n;
/* état du dernier empaquetage de tableaux, pour le crochet de plantage (24/09) */
static volatile unsigned long dbg_vtx_i, dbg_vtx_n, dbg_vmin, dbg_words, dbg_fmt;
static const void *volatile dbg_plan;
static const unsigned char *volatile dbg_vao;
static void flush(void);
static void drain_all(void);
static void buf_raw_invalidate_all(void);

/* ───────────────────── dispositions relevées (Tiger 10.4.6) ─────────────────────
 * Contexte du GLDriver (argument r3 de toutes les procédures) : */
#define CTX_GLSTATE      0x0c     /* état GL de GLEngine */
/* glPixelStorei_Exec : UNPACK_* vivent sur gctx, pas dans gls(). */
#define CTX_UNPACK_ROW_LENGTH  0x31cc
#define CTX_UNPACK_SKIP_ROWS   0x31d4
#define CTX_UNPACK_SKIP_PIXELS 0x31d8
#define CTX_UNPACK_ALIGNMENT   0x31e0
#define CTX_UNPACK_LSB_FIRST   0x31e5
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
/* Paramètres de GLEngine (DT_PARAMS) : c'est l'objet texture de GLEngine
   lui-même (docs/re/textures-3d.md). */
#define TP_TARGET 0x01            /* u8 : cible, par emplacement de CTX_TEXUNITS
                                     (1 3D, 3 2D, 4 1D) */
#define TP_WRAP_S 0x10
#define TP_WRAP_T 0x12
#define TP_WRAP_R 0x14            /* relevé le 19/09/2026 (sonde t3dprobe) */
#define TP_MIN    0x16
#define TP_MAG    0x18
#define TP_BORDER 0x1c            /* 4 × f32 : GL_TEXTURE_BORDER_COLOR (sonde
                                     wrapprobe, docs/re/bordure-et-compression.md) */
/* Niveaux et LOD (OpenGL 1.2), relevés le 19/09/2026 (sonde t3dprobe, T3D_LOD) */
#define TP_MIN_LOD    0x30        /* f32, initial −1000 */
#define TP_MAX_LOD    0x34        /* f32, initial 1000 */
#define TP_LOD_BIAS   0x38        /* f32 : relayé avec G.tex14, qui annonce
                                     GL_MAX_TEXTURE_LOD_BIAS (cfg+0xb0) ; sinon le
                                     maximum reste 0 et le biais est sans effet */
#define TP_BASE_LEVEL 0x3c        /* u16, initial 0 */
#define TP_MAX_LEVEL  0x3e        /* u16, initial 1000 */
/* Profondeur et ombre (OpenGL 1.4), sonde v14probe (docs/re/opengl-1.4.md) */
#define TP_COMPARE_FUNC 0x40      /* u16, initial GL_LEQUAL */
#define TP_COMPARE_MODE 0x42      /* u16, initial GL_NONE ; 0x884E COMPARE_R_TO_TEXTURE */
#define TP_DEPTH_MODE   0x48      /* u16, initial GL_LUMINANCE */
/* Tableau des niveaux de l'objet texture de GLEngine (pas 0x18). La structure
   de niveau du GLDriver (LV_*) est remplie par le rendu d'Apple, qui ne connaît
   pas la 3D : la PROFONDEUR et la hauteur de tranche ne sont qu'ici. */
#define TP_LEVEL0        0xa4
#define TP_LEVEL_SIZE    0x18
#define PL_W     0x00             /* u16 : largeur */
#define PL_H     0x02             /* u16 : hauteur */
#define PL_D     0x04             /* u16 : profondeur */
#define PL_ROWPIX 0x08            /* u16 : pixels par ligne des données */
#define PL_IMGH  0x0a             /* u16 : lignes par tranche des données */
#define PL_FORMAT 0x0c            /* u16 */
#define PL_TYPE  0x0e             /* u16 */
#define PL_DATA  0x10             /* u32 */
/* Cartes de cube : la face f a SON tableau de niveaux, au pas de 15 entrées
   (relevé le 19/09/2026, sonde cubeprobe) ; le GLDriver, lui, ne voit que la
   face 0 dans ses propres niveaux (LV_*). */
#define TP_FACE_SIZE     0x168
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
#define TU_LOD_BIAS      0x3c     /* float : GL_TEXTURE_LOD_BIAS de l'unité (glTexEnv
                                     GL_TEXTURE_FILTER_CONTROL), sonde v14probe */
#define GL_MAX_TEXUNITS  8
/* Sommet GLEngine (0x100 octets) : */
#define V_X 0x00
#define V_Y 0x04
#define V_Z 0x08
#define V_EYE   0x20              /* x y z w en coordonnées ŒIL (sonde ptprobe) */
#define V_COLOR 0x30              /* r g b a */
#define V_FOG   0x4c              /* facteur de brouillard f (1 = pas de brouillard) */
#define V_SEC   0x50              /* r g b : spéculaire SÉPARÉE (GL_SEPARATE_SPECULAR_COLOR),
                                     relevé le 19/09/2026 (docs/re/textures-3d.md §5) */
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
#define GS_COLOR_SUM      0x2e0b   /* u8 : glEnable(GL_COLOR_SUM) (sonde v14probe ;
                                      GS+0x4c8a en porte une copie) */
#define GS_LIGHTING       0x2d4a   /* u8 */
#define GS_COLOR_MATERIAL 0x2d4b   /* u8 */
#define GS_TWO_SIDE       0x2d4c   /* u8 */
#define GS_LOCAL_VIEWER   0x2d4d   /* u8 */
#define GS_FOG_DENSITY    0x2df0   /* float */
#define GS_FOG_START      0x2df4
#define GS_FOG_END        0x2df8
#define GS_FOG_COORD_SRC  0x2e06   /* u16 : 0x8451 FOG_COORDINATE / 0x8452 FRAGMENT_DEPTH */
#define GS_POINT_ATT      0x30cc   /* float ×3 : constante, linéaire, quadratique */
#define GS_POINT_SIZE_MIN 0x30c0   /* float, initial 0   (sonde v14probe) */
#define GS_POINT_SIZE_MAX 0x30c4   /* float, initial 1 (!), cf. point_max */
#define GS_POINT_FADE     0x30c8   /* float, initial 1 */
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
/* Objet « tableau de sommets » courant (V), docs/re/tableaux-de-sommets.md §2. */
#define GS_VAO            0x4700
#define VA_SLOT(V, a)     ((unsigned char *)(V) + 0x30 + (a) * 0x18)
#define VA_EN_HI          0x330   /* mot haut du masque 64 bits, bit 16+a */
#define VA_EN_LO          0x334
#define VA_VBO(V, a)      (0x360 + 4 * (a))         /* objet tampon de l'attribut */
#define GC_VA_PTRS        0x48f8  /* gctx : 32 pointeurs résolus */

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
       RK_TRI_TEX3 = 5, RK_TRI_TEX4 = 6,
       /* v11 : triangles avec couleur secondaire, 0 à 4 unités */
       RK_TRI_SEC0 = 7, RK_COUNT = 12 };
#define RK_IS_SEC(k)  ((k) >= RK_TRI_SEC0)
/* unités de texture portées par le sommet, par genre */
static const int rk_units[RK_COUNT] = { 0, 1, 2, 0, 0, 3, 4, 0, 1, 2, 3, 4 };
static const unsigned long rk_words[RK_COUNT] = {
    QGPU_VERTEX_WORDS, QGPU_VERTEX_TEX_WORDS, QGPU_VERTEX_TEX2_WORDS,
    QGPU_VERTEX_WORDS, QGPU_VERTEX_WORDS,
    QGPU_VERTEX_TEXN_WORDS(3), QGPU_VERTEX_TEXN_WORDS(4),
    QGPU_VERTEX_SEC_WORDS(0), QGPU_VERTEX_SEC_WORDS(1), QGPU_VERTEX_SEC_WORDS(2),
    QGPU_VERTEX_SEC_WORDS(3), QGPU_VERTEX_SEC_WORDS(4),
};

/* ─────────── v16 : programmes ARB de sommets et de fragments ───────────
 * Relevé GLEngine 10.4.6 (docs/re/programmes-arb.md) : un « pipeline program »
 * par objet ARB (gldCreatePipelineProgram, descripteur = ppobj+0x4c8, cible u16
 * à +0) ; le TEXTE n'est jamais poussé vers le pilote, il est dans l'objet
 * (ppobj+0x14, longueur +0x18), les paramètres locaux à *(ppobj+0x4e0) (16 o
 * par paramètre, 256 sommets / 128 fragments) ; gldModifyPipelineProgram dit
 * seulement « texte remplacé » (masque 1) ou « locaux modifiés » (2). Les
 * program.env sont du CONTEXTE : *(gctx+0x4668) (sommets) et *(gctx+0x4670)
 * (fragments). Activation : gctx+0x5434 / +0x5438 (_updateShaderState) ; objet
 * courant de chaque cible : gctx+0x5420 / +0x5424.
 * Nos poignées sont les nôtres (PPROG_HANDLE) ; celle d'Apple est gardée pour
 * lui transmettre les appels, puisque le rendu d'Apple reste le repli. */
#define PPROG_MAX      256
#define PPROG_HANDLE   0x50500000UL
typedef struct PProg {
    void          *ctx;                 /* contexte pilote (0 : entrée libre) */
    unsigned char *obj;                 /* ppobj = descripteur − 0x4c8 */
    unsigned long  apple;               /* poignée rendue par le rendu d'Apple */
    unsigned long  target;              /* 0x8620 / 0x8804 / 0 (objet par défaut) */
    long           id;                  /* identifiant qgpu dans le contexte, −1 */
    const char    *text_sent;           /* texte compilé par l'hôte : pointeur, */
    unsigned long  len_sent;            /*   longueur et somme de contrôle */
    unsigned long  sum_sent;
    int            text_dirty;          /* masque 1 vu depuis text_sent */
    int            local_dirty;         /* masque 2 vu depuis le dernier envoi */
    int            refused;             /* l'hôte a refusé le texte : hors domaine */
    unsigned long  env_n, local_n;      /* 1 + plus grand indice lu par le texte */
    const char    *parsed_text;         /* texte dont env_n / local_n / fp_unit */
    unsigned long  parsed_len;          /*   ont été tirés (prog_parse) */
    unsigned char  fp_unit[QGPU_MAX_UNITS];  /* fragments : masque TU_ENABLE de la
                                                cible échantillonnée par unité */
    int            fp_units_bad;        /* fragments : texture[u >= 4] ou cible inconnue */
    unsigned long  vp_need;             /* sommets : entrées vertex.* lues par le texte
                                           (VPN_*) ; 0 = pas encore relu */
} PProg;
static PProg pprog[PPROG_MAX];

/* Mots du bloc de changements que GLEngine passe à gldUpdateDispatch (3e
   argument) : +0x00 masque principal, +0x04 et +0x10 unités de texture
   (lus par le GLDriver d'Apple et par glrSetFunctions) ; +0x08, +0x0c
   cumulés aussi, par prudence. */
#define LAZY_WORDS 5
#define LAZY_DEFAULT 1          /* sans POMPPC_GL_LAZYAPPLE */

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
    unsigned char *fullscreen_buf;      /* owned software back buffer for CGL fullscreen */
    unsigned long  fullscreen_w, fullscreen_h;
    int            stencil;             /* la surface hôte a un stencil ; sa fraîcheur est
                                           celle de la profondeur (même mot côté invité) */
    int            sten_used;           /* le contexte s'est VRAIMENT servi du stencil */
    /* ── chemin brut (v7) : la géométrie part non transformée sur l'hôte ── */
    unsigned char *cfg;                 /* bloc de configuration de gldCreateContext */
    unsigned long  desc[16];            /* descripteur de sortie de sommet (cfg+0x11c),
                                           en-tête + 24 entrées u16 (v16 : + génériques) ;
                                           durée de vie = celle du contexte */
    unsigned long  geom_fmt;            /* masque QGPU_VF_* publié */
    unsigned long  geom_words;          /* pas d'un sommet, en mots */
    int            desc_dirty;          /* republié : demander le bit 1 du dispatch */
    int            desc_pos_code;       /* v16 : code de la position publié (0 ou 16) */
    int            geom_on;             /* dernier verdict du domaine */
    int            geom_lost;           /* une primitive a été perdue : plus jamais de brut */
    /* ── v16 : programmes ARB ── */
    int            vp_on, fp_on;        /* GLEngine dit le programme actif (prog_state) */
    PProg         *vp_rec, *fp_rec;     /* l'objet courant, s'il est connu */
    PProg         *cur_vp, *cur_fp;     /* ce que l'hôte a lié (PROG_BIND) */
    unsigned char  prog_used[QGPU_MAX_PROG];   /* identifiants qgpu pris */
    unsigned long  c_env[2][QGPU_MAX_PROG_PARAMS][4];  /* miroir des program.env */
    unsigned long  c_gs;                /* QGPU_SK_GEN_SIZES posé sur le device */
    int            c_gs_valid;          /*   … et connu (0 : à renvoyer) */
    unsigned long  c_env_n[2];          /* entrées 0..n-1 du miroir à jour ; au-delà,
                                           jamais envoyées (Prey, 23/09 : un « valide »
                                           global posé par un programme à 5 env laissait
                                           les env 10..16 du suivant hors du vidage) */
    int            array_mix;           /* glBegin vu alors que cfg+0x11c était nul :
                                           le descripteur est remis, plus de canal forcé */
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
    /* v18 : valeurs courantes posées par DRAW_NATIVE pour un attribut du
       format sans tableau (ce que l'empaqueteur aurait recopié : couleur
       morte → blanche, etc.) ; bit QGPU_CUR_* = nat_cur est ce que l'hôte a */
    unsigned long  nat_cur_ok;
    unsigned long  nat_cur[QGPU_CUR_COUNT][4];
    /* ── v8 ── */
    unsigned long  c_pstip[32];         /* motif de pointillé posé sur le device */
    int            c_pstip_valid;
    long           q_open;              /* requête d'occlusion ouverte, -1 si aucune */
    unsigned long  q_extra;             /* fragments dessinés par le LOGICIEL pendant
                                           la requête : comptés en trop, jamais en
                                           moins (voir q_end) */
    /* ── F2 : BeginPrimitiveBuffer ouvert, PAR CONTEXTE ──
       La case était globale et à une seule place : le geom_begin d'un contexte
       B (autre fil) effaçait le `pend` de A, le geom_end de A effaçait celui de
       B, et la zone de sommets réservée n'était jamais rendue. G.npend compte
       ceux qui sont ouverts — c'est lui qui interdit à flush() de changer de
       moitié et à submit_cur() de soumettre en asynchrone. */
    int            pend_open;
    unsigned long  pend_half;           /* moitié où GLEngine écrit en ce moment */
    unsigned long  pend_off, pend_words, pend_fmt, pend_slots;
    int            pend_drop;
    int            pend_flat;           /* ombrage plat : change l'ordre des indices */
    int            pend_wire;           /* mode de polygone ≠ GL_FILL : pas de fusion
                                           en triangles, elle perdrait le contour */
    /* ── F7 : présentation directe, décidée PAR CONTEXTE ──
       `D` est un singleton (l'écran) mais le rectangle et le verdict, eux,
       appartiennent au contexte : partagés, l'image de B partait dans le
       rectangle de A. */
    int            d_ok;                /* 0 non, 1 plein écran, 2 en fenêtre */
    long           d_x, d_y;            /* rectangle de CE contexte à l'écran */
    unsigned long  d_checked_at;        /* image de la dernière réévaluation */
    /* ── transmission paresseuse au GLDriver d'Apple (POMPPC_GL_LAZYAPPLE,
       docs/re/dispatch-paresseux.md) : masques de changement de GLEngine
       cumulés depuis le dernier gldUpdateDispatch transmis ── */
    unsigned long  lazy_m[LAZY_WORDS];
    int            lazy_pending;
    long           lazy_ret;            /* dernier retour d'Apple (4) */
} PCtx;

/* v16 : définies avec la synchronisation des programmes, plus bas ; texture_ok
   et unit_mask (avant) en dépendent. */
static void prog_state(PCtx *p);
static int prog_domain_ok(PCtx *p);
static int prog_sync(PCtx *p);
static unsigned char *gctx_of(PCtx *p);          /* sonde v16 dans close_raw */

typedef struct PTex {                   /* texture du GLDriver suivie par le plugin */
    struct PTex   *next;
    struct PTex   *hnext;               /* chaînage de la table de hachage */
    void          *drvtex;
    long           qtex;                /* identifiant hôte, -1 si aucun */
    int            dirty;               /* niveaux à (re)téléverser */
    int            host_only;           /* P15 : le contenu hôte est le SEUL à jour
                                           (CopyTexSubImage fait sur l'hôte, jamais
                                           redescendu dans l'invité) — l'évincer le
                                           perdrait, et le rechargement depuis
                                           l'invité remettrait l'ancien contenu */
    unsigned long  lv0_sig;             /* empreinte du niveau 0 déjà téléversé */
    unsigned long  prm[14];             /* paramètres envoyés : min, mag, wrap s, wrap t,
                                           wrap r (3D), puis (v10) min et max LOD en
                                           bits IEEE, niveau de base, niveau max,
                                           couleur de bordure 0xAARRGGBB, puis
                                           (G.tex14) biais de LOD, mode, fonction
                                           de comparaison, mode de profondeur */
    int            prm_valid;
    uint64_t       last_use;            /* resident texture LRU, under G.mu */
} PTex;

typedef struct TexUnit {
    PTex          *t;                   /* 0 : unité inactive */
    unsigned long  env_mode, env_color;
    unsigned long  combine, combine_src; /* GL_COMBINE empaqueté (v5) */
} TexUnit;

typedef struct TexInfo {                /* textures à appliquer pour le dessin en cours */
    TexUnit        u[QGPU_MAX_UNITS];
} TexInfo;
static void cube_probe(PCtx *p, const TexInfo *ti, const char *where);
static void draw_probe(PCtx *p, const TexInfo *ti);
static void dump_one(const char *path);

typedef struct Post {                   /* copie à faire APRÈS la barrière */
    int            depth;               /* 0 couleur, 1 profondeur, 2 stencil */
    int            packed;              /* profondeur 24 bits + stencil 8 bits dans le mot */
    unsigned long  off;                 /* dans l'arène, ABSOLU dans la tranche */
    unsigned char *dst;
    unsigned long  w, h, rowbytes;
    unsigned long  pixbytes;            /* 4 (xRGB/float32) ou 2 (1555/UNORM16) */
    float          scale;
} Post;

/* Une moitié de la tranche, et la soumission qui l'occupe (v9). */
typedef struct Half {
    unsigned long  fence;               /* barrière de la soumission en vol */
    int            busy;                /* elle n'est pas encore terminée */
    int            waiting;             /* F9 : un fil attend CETTE barrière, G.mu
                                           relâché — personne d'autre ne doit la
                                           réattendre ni la réécrire */
    Post           post[MAX_POST];      /* relectures à recopier après la barrière */
    int            npost;
} Half;

typedef struct PBuf PBuf;

/* v18 — réserves de miroirs BRUTS (DRAW_NATIVE). Un client n'a que
   QGPU_CLIENT_BUF_IDS (64) identifiants de tampon hôte, et idTech4 crée un VBO
   par bloc de son cache de sommets (des centaines) : chaque VBO reçoit donc
   une TRANCHE d'un grand tampon hôte (16 Mio), allouée au premier besoin
   (first-fit, fusion des voisins à la libération). */
#define RAWPOOL_MAX     8                       /* 128 Mio hôte au plus */
#define RAWPOOL_BYTES   QGPU_MAX_BUF_SIZE
#define RAW_ALIGN       256UL
#define RAW_CHUNK       0x100000UL              /* BUF_SUBDATA d'au plus 1 Mio */
#define BUF_HASH        1024                    /* vbo+0x30 → PBuf */
#define RD_MAX          16                      /* plages sales suivies par VBO */
typedef struct RawExt {
    struct RawExt  *next;
    unsigned long   off, len;
} RawExt;

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
    /* F2 : tampons rendus par BeginPrimitiveBuffer et pas encore refermés. Le
       DÉTAIL est dans la PCtx (pend_*) ; ici on ne garde que le COMPTE, parce
       que c'est la seule chose dont le flux ait besoin : tant qu'il n'est pas
       nul, flush() ne change pas de moitié et submit_cur() soumet en
       synchrone — GLEngine écrit en ce moment dans la moitié courante, à une
       adresse qu'on lui a déjà donnée. */
    int             npend;
    /* F9 : une (ou plusieurs) attente de barrière est en cours, G.mu relâché.
       Tant que ce n'est pas nul, RIEN ne doit toucher au flux, aux sommets, aux
       indices ni à l'arène : stream_ready() endort les écrivains. */
    int             halt;
    int             wait_miss;          /* F8 : dépassements de barrière CONSÉCUTIFS */
    unsigned long   ctx_used, surf_used;
    unsigned long   tex_used[(QGPU_CLIENT_TEX_IDS + 31) / 32];
    PCtx           *list;
    PTex           *textures;
    uint64_t        tex_clock;
    unsigned long   n_texevictions;
    /* statistiques */
    unsigned long   n_tris, n_clears, n_submits, n_uploads, n_readbacks, n_fallback;
    unsigned long   n_textris, n_texuploads, n_lines, n_points;
    unsigned long   n_frames, n_direct, n_tex_incomplete;
    unsigned long   n_rawverts, n_rawdraws, n_geomcmds, n_geomdrop, n_rawmerged;
    /* I9 (relecture du 24/09) : niveaux illisibles envoyés noirs ; lots ou
       primitives jetés sur faute de lecture (gardes pack et proc) */
    unsigned long   n_texblack, n_dropped_fault;
    unsigned long   n_arrayverts, n_arraydraws; /* chemin tableaux, inclus dans brut */
    int             v7;                 /* device v7 ET chemin brut autorisé */
    int             v8;                 /* device v8 : pipeline fixe complet */
    int             v10;                /* device v10 : niveaux au format de
                                           l'application, convertis par l'hôte */
    int             tex3d;              /* textures 3D annoncées et tenues (v10 +
                                           QGPU_CAP_GL14) */
    int             cube;               /* cartes de cube, idem */
    int             tex13;              /* idem : CLAMP_TO_BORDER, MIRRORED_REPEAT,
                                           couleur de bordure, niveaux S3TC */
    int             tex14;              /* idem, OpenGL 1.4 : biais de LOD, textures
                                           de profondeur et ombre, GL_COLOR_SUM */
    int             xbar;               /* sources croisées de GL_COMBINE (v12 +
                                           QGPU_CAP_GL14) */
    int             scanout;            /* v13 : SURF_PRESENT dans la VRAM qfb */
    int             pixops;             /* v13 : COPY_TEX / ReadPixels / DrawPixels hôte */
    int             prog;               /* v16 : programmes ARB tenus par l'hôte
                                           (QGPU_CAP_PROGRAMS) ; POMPPC_GL_PROG=0 les coupe */
    int             gensizes;           /* génériques à taille déclarée sur le chemin
                                           tableaux (QGPU_CAP_GEN_SIZES) ;
                                           POMPPC_GL_GENSIZES=0 revient à 4 flottants */
    long            pixtex;             /* texture 2D jetable pour Draw/CopyPixels */
    unsigned long   pixtex_w, pixtex_h;
    unsigned long   n_present;          /* présentations hôte, sans copie G4 */
    unsigned long   n_copytex;          /* CopyTexSubImage sans relecture G4 */
    unsigned long   n_pixread;          /* ReadPixels d'un rectangle, pas du FB */
    unsigned long   n_pixdraw;          /* DrawPixels / CopyPixels / Bitmap sans repli Apple */
    int             hostbuf;            /* v14 : DRAW_RAW_BUF, maillages hors BAR0 */
    int             v15;                /* v15 : SURF/DEPTH xfer 16 bits par l'hôte */
    int             units;              /* v17 : unités de texture que le device tient
                                           (8 ; 4 avant la v17). Au-delà : rendu d'Apple. */
    unsigned long   buf_used[(QGPU_CLIENT_BUF_IDS + 31) / 32];
    unsigned long   buf_base;
    PBuf           *bufs;
    unsigned long   n_vbohits, n_vbomiss;
    /* ── v18 : DRAW_NATIVE, miroirs bruts des VBO ── */
    int             native;             /* QGPU_CAP_NATIVE ; POMPPC_GL_NATIVE=0 le coupe */
    int             native_range;       /* plage de gldFlushBuffer crue (défaut) */
    PBuf           *buf_hash[BUF_HASH]; /* recherche O(1) de buf_from_vbo */
    long            rp_qid[RAWPOOL_MAX];
    RawExt         *rp_free[RAWPOOL_MAX];   /* trié par offset */
    int             rp_n;
    unsigned long   rp_gen;             /* +1 à chaque libération : réessayer */
    unsigned long   n_native_draws, n_native_verts;
    unsigned long   n_native_bytes, n_native_subdata;   /* recopie brute */
    unsigned long   n_native_fall;      /* VBO présents mais repli sur l'empaquetage */
    unsigned long   query_base;         /* premier identifiant de requête du client */
    double          t_submit, t_copy, t_upload;   /* secondes cumulées */
    /* ── bilan du mode asynchrone ── */
    unsigned long   n_waits;            /* barrières réellement attendues */
    unsigned long   n_qfull;            /* soumissions refusées (file pleine) */
    unsigned long   n_syncfall;         /* retours en synchrone sur ERRORS */
    unsigned long   n_qsamples, n_qsum; /* profondeur de file échantillonnée */
    double          t_wait;             /* secondes passées à attendre une barrière */
    /* ── transmission paresseuse au GLDriver d'Apple ── */
    int             lazy;               /* POMPPC_GL_LAZYAPPLE */
    unsigned long   n_lazy_defer;       /* gldUpdateDispatch gardés pour plus tard */
    unsigned long   n_lazy_eager;       /* transmis tout de suite (tampon de dessin) */
    unsigned long   n_lazy_sync;        /* transmis juste avant une procédure d'Apple */
} G = { PTHREAD_MUTEX_INITIALIZER };

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

/* F10 : les mesures de temps ne servent qu'au bilan. Deux gettimeofday par LOT
 * dans sync_to_host, sans même que le bilan soit demandé, coûtaient deux appels
 * système par changement d'état. Un static, comme toutes les autres sondes. */
static int stats_timing(void)
{
    static int timing = -1;
    if (timing < 0)
        timing = getenv("POMPPC_GL_STATS") != 0;
    return timing;
}

/* ── F9 : réveil des fils tenus à l'écart pendant une attente de barrière ──
 *
 * `qgpu_wait` dort jusqu'à WAIT_MS dans le kext, en THREAD_UNINT. Le faire
 * sous G.mu bloquait TOUS les fils GL du processus, et avec un kext qui ne se
 * réveille pas (K4) c'était le processus entier. wait_half relâche donc G.mu
 * autour de l'attente ; pendant cette fenêtre :
 *
 *   — G.halt tient à l'écart tout ce qui ÉCRIT dans la tranche (flux, sommets,
 *     indices, arène) : la moitié attendue est peut-être la courante
 *     (switch_half l'a déjà désignée) et celle qu'on vient de soumettre est
 *     encore en vol. C'est stream_ready() qui les endort ;
 *   — `waiting` par moitié empêche deux fils d'attendre la même barrière, et
 *     surtout empêche le second de repartir en croyant la moitié libre.
 *
 * Tout le reste — recherche de contexte, comptabilité des textures, traces,
 * destruction d'objets — passe, ce qui est exactement ce qu'on cherche.
 *
 * La variable de condition vit hors de G pour lui garder son initialiseur
 * statique (PTHREAD_MUTEX_INITIALIZER sur le premier membre).
 */
static pthread_cond_t half_cv = PTHREAD_COND_INITIALIZER;

/* À appeler, VERROU TENU, avant toute écriture dans la moitié courante.
   Au retour, plus aucune attente n'est en cours : tout ce que l'appelant a pu
   lire avant doit être RELU (G.cur, G.win, G.cmd, G.ncmd, G.vtx…). */
static void stream_ready(void)
{
    while (G.halt > 0)
        pthread_cond_wait(&half_cv, &G.mu);
}

/* F2 — referme la case `pend` du contexte p (verrou tenu). `reclaim` rend la
 * place de sommets que geom_begin avait réservée, mais SEULEMENT si personne
 * n'a alloué par-dessus : avec deux contextes sur deux fils, rendre la place
 * sans regarder écraserait les sommets du voisin. */
static void pend_close(PCtx *p, int reclaim)
{
    if (!p || !p->pend_open)
        return;
    if (reclaim &&
        G.vtx == p->pend_off + p->pend_slots * p->pend_words * 4)
        G.vtx = p->pend_off;
    p->pend_open = 0;
    p->pend_drop = 1;
    if (G.npend > 0)
        G.npend--;
}

/* Motifs de refus de l'accélération : compteurs et détail du premier cas,
 * rapportés par le bilan périodique. */
enum {
    NO_BUFFER, NO_RASTER, NO_FOG, NO_POLYMODE, NO_DEPTH, NO_BLEND, NO_ALPHA,
    NO_SURFACE, NO_TEX_UNITS, NO_TEX_TARGET, NO_TEX_ENV, NO_TEX_UNKNOWN,
    NO_TEX_BASE, NO_TEX_SIZE, NO_TEX_FORMAT, NO_TEX_ID, NO_TEX_COMBINE, NO_STENCIL,
    /* sorties du domaine propres au chemin brut (v7) */
    NO_G_RASTER, NO_G_POINT, NO_G_PROGRAM, NO_G_STRIDE, NO_G_LATE, NO_G_ARRAY,
    /* v8 : ce qui reste hors domaine une fois les fonctions v8 branchées */
    NO_Q_FALLBACK, NO_TEX_PARAM,
    /* v10 */
    NO_G_TEX3D,
    /* 23/09/2026 : source de tableau nulle (Colin McRae) ; sommet NaN retenu
       avant le repli Apple */
    NO_G_SRC, NO_APPLE_NAN, NO_G_GENERIC,
    /* v16 : programme refusé par l'hôte ; programme de fragments hors bornes */
    NO_G_PROG_HOST, NO_G_PROG_UNITS, NO_COUNT
};
static const char *const no_name[NO_COUNT] = {
    "buffer", "logicop/stipple/smooth", "fog", "polygonmode", "depth",
    "blend", "alphatest", "surface", "units>2", "tex-target", "texenv",
    "unknown-texture", "base-format", "tex-size", "texel-format", "tex-id",
    "combine", "stencil",
    "raw:smooth", "attenuated-point-size", "raw:program",
    "raw:no-vertex", "raw:late-state", "raw:arrays",
    "query:sw-fallback", "tex-param",
    "raw:tex-3d-or-cube",
    "raw:null-array", "apple:nan-vertex", "raw:generic-attribs",
    "prog:host-refused", "prog:units",
};
static unsigned long no_count[NO_COUNT];
static char no_detail[NO_COUNT][64];
static unsigned long fb_count[PROC_COUNT];      /* replis par procédure */

static int no(int why, unsigned long a, unsigned long b)
{
    if (!no_count[why]++) {
        snprintf(no_detail[why], sizeof(no_detail[why]), "%lx/%lx", a, b);
        gl_note("fallback %s %lx/%lx\n", no_name[why], a, b);
    }
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

/* Optional per-swap trace. Monotonic guest clock, buffered writes, no extra
 * device query. These are application swap intervals, not GPU completion
 * times; filter by context when a process has several drawables. */
static FILE *frame_file;
static void trace_frame(void *ctx)
{
    static int init;
    static mach_timebase_info_data_t tb;
    static uint64_t start, flushed;
    static char buffer[65536];
    uint64_t tick;
    if (!init) {
        const char *path = getenv("POMPPC_GL_FRAMES");
        init = 1;
        if (!path || path[0] != '/') return;
        if (mach_timebase_info(&tb) != KERN_SUCCESS || !tb.denom) return;
        frame_file = fopen(path, "w");
        if (!frame_file) return;
        setvbuf(frame_file, buffer, _IOFBF, sizeof(buffer));
        start = flushed = mach_absolute_time();
        fprintf(frame_file, "frame,context,elapsed_ms,raw_vertices,raw_draws,fallbacks,readbacks,submit_ms,wait_ms,copy_ms\n");
    }
    if (!frame_file) return;
    tick = mach_absolute_time();
    fprintf(frame_file, "%lu,%p,%.3f,%lu,%lu,%lu,%lu,%.3f,%.3f,%.3f\n",
            G.n_frames, ctx, (double)(tick - start) * tb.numer / tb.denom / 1e6,
            G.n_rawverts, G.n_rawdraws, G.n_fallback, G.n_readbacks,
            G.t_submit * 1000, G.t_wait * 1000, G.t_copy * 1000);
    if ((double)(tick - flushed) * tb.numer / tb.denom >= 5e9) {
        fflush(frame_file);
        flushed = tick;
    }
}

/* Bilan périodique (POMPPC_GL_STATS=<fichier>), appelé à chaque échange. */
static void stats_frame(void *ctx)
{
    static const char *path;
    static int init;
    static double t0;
    static unsigned long f0, tr0, rb0, pr0, up0, fb0, sub0, tu0, di0;
    static unsigned long rv0, rd0, gc0, rm0, w0, qf0, av0, ad0;
    static unsigned long nd0, nv0, nb0, nf0;    /* v18 : DRAW_NATIVE */
    static double ts0, tc0, tu_0, tw0;
    double t;

    if (!init) {
        const char *e = getenv("POMPPC_GL_STATS");
        init = 1;
        path = (e && e[0] == '/') ? e : 0;
        t0 = now_s();
    }
    G.n_frames++;
    trace_frame(ctx);
    async_rearm();
    if (G.lazy && G.n_frames % 500 == 0)
        gl_note("LAZYAPPLE image %lu : %lu dispatch gardés, %lu transmis au tampon de dessin, "
                "%lu transmis avant une procédure d'Apple\n",
                G.n_frames, G.n_lazy_defer, G.n_lazy_eager, G.n_lazy_sync);
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
            fprintf(f, "%.1f fps | tri %lu/frame | readback %lu | present %lu | upload %lu (tex %lu) | "
                    "fallback %lu | submit %lu | submit %.2f ms/frame | copy %.2f ms/frame | "
                    "upload-prep %.2f ms/frame | direct %lu\n",
                    (G.n_frames - f0) / dt,
                    (G.n_tris - tr0) / (G.n_frames - f0 ? G.n_frames - f0 : 1),
                    G.n_readbacks - rb0, G.n_present - pr0, G.n_uploads - up0, G.n_texuploads - tu0,
                    G.n_fallback - fb0, G.n_submits - sub0,
                    (G.t_submit - ts0) * 1000 / (G.n_frames - f0),
                    (G.t_copy - tc0) * 1000 / (G.n_frames - f0),
                    (G.t_upload - tu_0) * 1000 / (G.n_frames - f0),
                    G.n_direct - di0);
            {   /* v9 : ce que coûte (ou ne coûte plus) l'attente de l'hôte. */
                unsigned long fr = G.n_frames - f0 ? G.n_frames - f0 : 1;
                fprintf(f, "    submit %s: %lu fence wait(s)/frame, "
                        "%.2f ms wait/frame, queue %.2f in flight, %lu QUEUE_FULL, "
                        "%lu sync fallback(s)\n",
                        G.async ? "async" : "sync",
                        (G.n_waits - w0) / fr, (G.t_wait - tw0) * 1000 / fr,
                        G.n_qsamples ? (double)G.n_qsum / G.n_qsamples : 0.0,
                        G.n_qfull - qf0, G.n_syncfall);
                G.n_qsum = 0; G.n_qsamples = 0;
            }
            {
                unsigned long fr = G.n_frames - f0 ? G.n_frames - f0 : 1;
                if (G.n_rawverts != rv0 || G.n_rawdraws != rd0) {
                    unsigned long nd = G.n_rawdraws - rd0;
                    fprintf(f, "    raw: %lu verts/frame, %lu DRAW_RAW/frame, "
                            "%lu state cmds/frame, %lu merged/frame, "
                            "%lu verts/draw, arrays %lu verts/frame "
                            "%lu draws/frame%s\n",
                            (G.n_rawverts - rv0) / fr, nd / fr,
                            (G.n_geomcmds - gc0) / fr, (G.n_rawmerged - rm0) / fr,
                            nd ? (G.n_rawverts - rv0) / nd : 0,
                            (G.n_arrayverts - av0) / fr,
                            (G.n_arraydraws - ad0) / fr,
                            G.n_geomdrop ? " ! DROPPED PRIMS" : "");
                }
                if (G.n_native_draws != nd0 || G.n_native_fall != nf0)
                    fprintf(f, "    native: %lu draws/frame, %lu verts/frame, "
                            "%lu KiB raw VBO copied/frame, %lu VBO draws packed/frame\n",
                            (G.n_native_draws - nd0) / fr, (G.n_native_verts - nv0) / fr,
                            ((G.n_native_bytes - nb0) >> 10) / fr,
                            (G.n_native_fall - nf0) / fr);
            }
            {
                int k;
                for (k = 0; k < NO_COUNT; k++)
                    if (no_count[k])
                        fprintf(f, "    reject %s: %lu (first: %s)\n",
                                no_name[k], no_count[k], no_detail[k]);
                if (direct_why()[0])
                    fprintf(f, "    direct present off: %s\n", direct_why());
                if (G.lazy)
                    fprintf(f, "    lazy apple: %lu dispatch kept, %lu sent at draw buffer, "
                            "%lu sent before an Apple proc (cumulative)\n",
                            G.n_lazy_defer, G.n_lazy_eager, G.n_lazy_sync);
                for (k = 0; k < PROC_COUNT; k++)
                    if (fb_count[k])
                        fprintf(f, "    fallback %s: %lu\n", pomppc_proc_name(k), fb_count[k]);
                memset(no_count, 0, sizeof(no_count));
                memset(fb_count, 0, sizeof(fb_count));
            }
            fclose(f);
        }
        t0 = t; f0 = G.n_frames; tr0 = G.n_tris; rb0 = G.n_readbacks; pr0 = G.n_present; up0 = G.n_uploads;
        fb0 = G.n_fallback; sub0 = G.n_submits; tu0 = G.n_texuploads; di0 = G.n_direct;
        ts0 = G.t_submit; tc0 = G.t_copy; tu_0 = G.t_upload;
        rv0 = G.n_rawverts; rd0 = G.n_rawdraws; gc0 = G.n_geomcmds;
        rm0 = G.n_rawmerged; w0 = G.n_waits; qf0 = G.n_qfull; tw0 = G.t_wait;
        av0 = G.n_arrayverts; ad0 = G.n_arraydraws;
        nd0 = G.n_native_draws; nv0 = G.n_native_verts;
        nb0 = G.n_native_bytes; nf0 = G.n_native_fall;
    }
}

/* ────────────────────────────── utilitaires ────────────────────────────── */

static float clamp01(float v)
{
    return !(v > 0.0f) ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* Le cœur refuse NaN, infini et |v| ≥ 1e9 (toute la soumission saute). */
static float sane_f(float v)
{
    return (v > -1e9f && v < 1e9f) ? v : 0.0f;
}

/* |f| ≥ 1e9, NaN ou Inf, par les bits IEEE — pas de FPU (FASTFP éteint). */
static int u_insane(unsigned long u)
{
    return (u & 0x7fffffffUL) >= 0x4e6e6b28UL;      /* 1e9 = 0x4e6e6b28 */
}

/* glPolygonOffset : bits IEEE de l'application, bornés à ±POLY_OFFSET_MAX
 * (au-delà le décalage n'a plus de sens). NaN → 0. La borne est STRICTEMENT
 * à l'intérieur de celle du cœur (valid_state : |f| ≤ 1e6) : borner à 1e6
 * exactement quand le cœur exigeait « < 1e6 » faisait refuser le SET_STATE,
 * donc perdre toute la soumission ET ses relectures — image noire, vu en VM le
 * 22/09/2026 (scène `offset`). Le plugin est le côté conservateur. */
#define POLY_OFFSET_MAX 999999.0f
static unsigned long safe_offset(unsigned long bits)
{
    float f = *(float *)&bits;
    if (!(f > -POLY_OFFSET_MAX && f < POLY_OFFSET_MAX))
        f = (f > 0.0f) ? POLY_OFFSET_MAX : (f < 0.0f) ? -POLY_OFFSET_MAX : 0.0f;
    return *(unsigned long *)&f;
}

/* UT2004 laisse des NaN dans DRAW_RAW. Les remettre à 0 dessinait un triangle
   jusqu'à l'origine : fillrate, jeu figé, image « plus belle » (T&L) mais
   rampante. On jette la primitive, on pose 0 sur le mot pour que l'hôte
   accepte le reste. Comparaisons entières. 0 = ne pas soumettre. */
/* Borne du marquage : le plus grand nombre de sommets qu'un DRAW_RAW puisse
   porter (le cœur refuse au-delà). Le chemin TABLEAUX va jusque-là, celui de
   Begin/End s'arrête bien avant (GEOM_MAX_MERGE). */
#define RAW_NAN_MAX QGPU_MAX_VERTS
static unsigned char raw_bad[RAW_NAN_MAX];

/* Cœur commun aux deux chemins bruts (P12) : assainit nv sommets de `words`
 * mots à partir de `w`, marque dans raw_bad[] ceux qu'il faut jeter (mot fou
 * remis à 0, ou w ≈ 0) et rend leur nombre. Aucun accès aux globales : le
 * chemin TABLEAUX s'en sert aussi, lui qui ne passe pas par G.raw_*. */
static unsigned long raw_scan_nan(unsigned long *w, unsigned long nv,
                                  unsigned long words, unsigned long fmt,
                                  int keep_w0)
{
    unsigned long i, j, nbad = 0;

    for (i = 0; i < nv; i++) {
        unsigned char b = 0;
        unsigned long *v = w + i * words;
        /* w ≈ 0 : après projection, clip infini → triangles géants (ciel
           Colin McRae). On jette le sommet, comme un NaN, plutôt que de
           forcer w=1 (ça collerait le ciel à la caméra). SAUF sous un
           programme de sommets (keep_w0) : les volumes d'ombre de DOOM 3 et
           Prey (shadow.vp) sont des paires (x,y,z,1)/(x,y,z,0), le w=0 est
           le sommet projeté à l'infini par la matrice de projection infinie —
           l'hôte les découpe en homogène comme une vraie carte. Les jeter
           supprimait toutes les ombres ; les refuser envoyait chaque volume
           chez Apple (5 replis et relectures par image, 24/09/2026). */
        if (!keep_w0 && QGPU_VF_POS_COUNT(fmt) == 4 &&
            (v[3] & 0x7fffffffUL) < 0x358637bdUL)        /* |w| < 1e-6 */
            b = 1;
        for (j = 0; j < words; j++) {
            if (u_insane(v[j])) {
                v[j] = 0;
                b = 1;
            }
        }
        raw_bad[i] = b;
        nbad += b;
    }
    return nbad;
}

static int raw_fix_nan(void)
{
    unsigned long nv, i, words, nbad;
    unsigned long *w;
    unsigned char *bad = raw_bad;

    words = G.raw_words;
    if (!words)
        return 0;
    nv = (G.raw_vend - G.raw_start) / (words * 4);
    if (nv == 0 || nv > RAW_NAN_MAX)
        return 0;
    w = (unsigned long *)(G.win + VTX_OFF + G.raw_start);
    /* S3 (relecture du 24/09) : sous programme, w = 0 est gardé ici aussi
       (volumes d'ombre dessinés par Begin/End ou le déroulage de GLEngine) */
    nbad = raw_scan_nan(w, nv, words, G.raw_fmt,
                        G.raw_ctx && G.prog && G.raw_ctx->vp_on);
    if (!nbad)
        return 1;

    if (G.raw_idx && G.raw_mode == QGPU_PRIM_MODE_TRIANGLES) {
        unsigned short *idx = (unsigned short *)(G.win + G.raw_idx);
        unsigned long n = G.raw_count, o = 0, a, b0, c;
        for (i = 0; i + 2 < n; i += 3) {
            a = idx[i];
            b0 = idx[i + 1];
            c = idx[i + 2];
            if (a < nv && b0 < nv && c < nv && !bad[a] && !bad[b0] && !bad[c]) {
                idx[o] = (unsigned short)a;
                idx[o + 1] = (unsigned short)b0;
                idx[o + 2] = (unsigned short)c;
                o += 3;
            }
        }
        G.raw_count = o;
        G.raw_nidx = o;
        return o > 0;
    }
    if (!G.raw_idx && G.raw_mode == QGPU_PRIM_MODE_TRIANGLES) {
        unsigned long o = 0;
        for (i = 0; i + 2 < nv; i += 3) {
            if (!bad[i] && !bad[i + 1] && !bad[i + 2]) {
                if (o != i)
                    memcpy(w + o * words, w + i * words, words * 12);
                o += 3;
            }
        }
        G.raw_count = o;
        G.raw_vend = G.raw_start + o * words * 4;
        return o > 0;
    }
    /* ruban / éventail / quads : un sommet fou gâche la série */
    return 0;
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
    /* Drawable mémoire plein écran : toujours 4 octets/pixel, même si le
       pixel format (et l'écran QFB) sont en 16 bits. */
    if (p->fullscreen_buf)
        return GLD_U32(p->ctx, CTX_ROWPIX) * 4;
    return GLD_U32(p->ctx, CTX_ROWPIX) *
           (GLD_U32(p->ctx, CTX_COLOR_BITS) <= 16 ? 2 : 4);
}

static int color16(PCtx *p)
{
    return !p->fullscreen_buf && GLD_U32(p->ctx, CTX_COLOR_BITS) <= 16;
}

static int depth16(PCtx *p)
{
    return GLD_U32(p->ctx, CTX_DEPTH_BITS) == 16;
}

static unsigned long color_bpp(PCtx *p)
{
    return color16(p) ? 2 : 4;
}

static unsigned long depth_bpp(PCtx *p)
{
    return depth16(p) ? 2 : 4;
}

static unsigned long depth_rowbytes(PCtx *p)
{
    return GLD_U32(p->ctx, CTX_ROWPIX) * depth_bpp(p);
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
    /* Mineur §8.1 : le bilan sortait DEUX fois quand le plugin était déchargé
       avant la fin du processus — une fois par pomppc_backend_fini, une fois
       par l'atexit. Le premier qui passe gagne. */
    static int done;
    if (done)
        return;
    done = 1;
    if (getenv("POMPPC_GL_STATS")) {
        int k;
        fprintf(stderr, "POMPPC GL: texture cache: %lu evictions\n", G.n_texevictions);
        for (k = 0; k < NO_COUNT; k++)
            if (no_count[k])
                fprintf(stderr, "POMPPC GL: reject %s: %lu (first: %s)\n",
                        no_name[k], no_count[k], no_detail[k]);
    }
    if (getenv("POMPPC_GL_STATS"))
        fprintf(stderr, "POMPPC GL: %lu triangles (%lu textured), %lu lines, %lu points "
                "and %lu clears on host, %lu submits, %lu uploads, "
                "%lu tex levels, %lu readbacks, %lu host presents, "
                "%lu host CopyTex, %lu rect ReadPixels, %lu host Draw/CopyPixels/Bitmap, "
                "%lu synced software calls; "
                "raw: %lu verts in %lu DRAW_RAW (%lu / %lu from arrays), "
                "%lu host VBO hits / %lu packs, %lu dropped prims "
                "(%lu on read fault), %lu tex levels sent black; "
                "submit %s: %lu fences waited (%.0f ms), %lu QUEUE_FULL, "
                "%lu sync fallback(s)\n",
                G.n_tris, G.n_textris, G.n_lines, G.n_points, G.n_clears, G.n_submits,
                G.n_uploads, G.n_texuploads, G.n_readbacks, G.n_present,
                G.n_copytex, G.n_pixread, G.n_pixdraw, G.n_fallback,
                G.n_rawverts, G.n_rawdraws, G.n_arrayverts, G.n_arraydraws,
                G.n_vbohits, G.n_vbomiss, G.n_geomdrop,
                G.n_dropped_fault, G.n_texblack,     /* I9 (relecture du 24/09) */
                G.async ? "async" : "sync", G.n_waits, G.t_wait * 1000,
                G.n_qfull, G.n_syncfall);
    if (getenv("POMPPC_GL_STATS"))      /* v18 */
        fprintf(stderr, "POMPPC GL: %lu native draws (%lu verts, DRAW_NATIVE %s), "
                "%lu KiB of raw VBO copied in %lu BUF_SUBDATA, %d host pool(s), "
                "%lu VBO draws packed anyway\n",
                G.n_native_draws, G.n_native_verts,
                G.native ? "on" : ((G.q.caps & QGPU_CAP_NATIVE) ? "cut" : "not offered"),
                G.n_native_bytes >> 10, G.n_native_subdata, G.rp_n, G.n_native_fall);
}

/* gldTerminateLibrary : GLEngine décharge le plugin (NSUnLinkModule suit).
   La tranche du kext est rendue tout de suite — le kext draine la file du
   client avant de détruire ses objets — au lieu de rester prise jusqu'à la fin
   du processus. Vu sur l'installation de la tâche 4.2 : une copie restée dans
   Resources est rejetée par GLEngine juste après son gldInitializeLibrary. */
void pomppc_backend_fini(void)
{
    pthread_mutex_lock(&G.mu);
    if (frame_file) {
        fclose(frame_file);
        frame_file = NULL;
    }
    if (G.state > 0) {
        /* Mineur §8.1 : rien ne doit rester en vol quand la tranche est
           rendue. Les copies différées visent des tampons de l'invité (et une
           mémoire vidéo) ; qgpu_close démappe la fenêtre sous elles, et le
           kext détruit les objets du client.
           G.state passe à -1 AVANT le vidage : drain_all peut relâcher G.mu
           (F9), et tous les points d'entrée qui testent G.state doivent alors
           déjà se retirer plutôt que d'écrire dans une tranche qu'on est en
           train de rendre. */
        G.state = -1;
        flush();
        drain_all();
        if (getenv("POMPPC_GL_STATS"))
            on_exit_stats();
        qgpu_close(&G.q);
        pomppc_log("POMPPC: plugin déchargé, tranche %lu rendue\n", G.q.index);
    }
    G.state = -1;
    pthread_mutex_unlock(&G.mu);
}

/* P8 — fork() SANS exec (Safari/WebKit lancent leurs aides ainsi). L'enfant
 * hérite de la tranche MAPPÉE (mémoire du device, partagée, pas copiée), du
 * port Mach du kext et de G tout entier : il écrirait dans le MÊME flux que le
 * père, et sa première soumission ferait exécuter deux fois la trame du père.
 * Le handler enfant n'a le droit de rien libérer (malloc et les verrous ne
 * sont pas sûrs après fork, et fermer le user client détruirait les objets du
 * PÈRE) : il OUBLIE. G.mu est relâché ici parce que le handler « prepare » l'a
 * pris dans le fil qui appelle fork(), et que c'est ce fil-là qui continue. */
void pomppc_backend_forget(void)
{
    G.state = -1;
    G.async = 0;
    G.async_avail = 0;
    G.ncmd = 0;
    G.npend = 0;
    G.halt = 0;
    G.list = 0;
    G.textures = 0;
    G.bufs = 0;
    /* v18 : oublier aussi les miroirs bruts (sans rien libérer, cf. plus haut) */
    memset(G.buf_hash, 0, sizeof(G.buf_hash));
    memset(G.rp_free, 0, sizeof(G.rp_free));
    G.rp_n = 0;
    G.native = 0;
    G.cmd = 0;
    G.win = 0;
    G.bound = 0;
    G.run_ctx = 0;
    G.raw_ctx = 0;
    frame_file = NULL;                  /* le FILE* du père : ne pas s'en servir */
    qgpu_forget(&G.q);
    pthread_mutex_unlock(&G.mu);
}

void pomppc_backend_prepare_fork(void)
{
    pthread_mutex_lock(&G.mu);
}

void pomppc_backend_parent_fork(void)
{
    pthread_mutex_unlock(&G.mu);
}

/* Nombre d'unités de texture que l'hôte tient : c'est ce que gldCreateContext
   annonce à GLEngine à la place des 8 du GLDriver d'Apple. Mesuré en VM le
   22/09/2026 (UT2004, DM-Rankin) avec un device à 4 unités : avec 8 annoncées,
   le jeu allume l'unité 4 sur certains matériaux et 1 184 lots partaient en
   rendu logiciel (« units>2 »), 1 088 synchronisations hôte ↔ logiciel — un
   défaut de géométrie visible et du temps perdu. Depuis la v17 (23/09/2026,
   DOOM 3 : interaction.vfp lit texture[0..6]) le device en tient 8 et
   l'annonce d'Apple reste telle quelle ; sur un device plus ancien on
   redescend à 4, comme le GeForce3 de référence. */
int pomppc_backend_units(void)
{
    return G.units ? G.units : 4;
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
        G.async_why = "device older than v9";
        return;
    }
    if (!(G.q.caps & QGPU_CAP_ASYNC)) {
        G.async_why = "device does not advertise QGPU_CAP_ASYNC";
        return;
    }
    /* Une moitié doit porter le flux, les sommets, les indices et une arène
       digne de ce nom (deux transferts plein écran). Sinon, une seule moitié. */
    if (half < ARENA_OFF + 0x100000) {
        G.async_why = "slot too small for two halves";
        return;
    }
    if (!qgpu_async_ok(&G.q)) {
        G.async_why = "installed kext has no async doorbell";
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
            crash_hook_install();       /* 24/09 : journal du plantage avant Quit() */
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
            /* v17 : 8 unités de texture (DOOM 3 lit texture[0..6]). Un device
               plus ancien n'en tient que 4 : les clés, bits de format et
               matrices des unités 4..7 lui seraient refusés. */
            G.units = G.q.version >= 17 ? QGPU_MAX_UNITS : 4;
            /* v10 : l'hôte convertit les texels (TEX_IMAGE3). POMPPC_GL_TEX3=0
               revient à la conversion par l'invité, pour comparer. */
            G.v10 = G.q.version >= 10 &&
                    !(getenv("POMPPC_GL_TEX3") && getenv("POMPPC_GL_TEX3")[0] == '0');
            /* Textures 3D (OpenGL 1.2) : il n'y a PAS de repli, le rendu d'Apple
               ne sait pas les échantillonner. On ne les annonce donc que si
               l'hôte les tient (docs/re/textures-3d.md). POMPPC_GL_TEX3D=0 les
               coupe. */
            G.tex3d = G.v10 && (G.q.caps & QGPU_CAP_GL14) &&
                      !(getenv("POMPPC_GL_TEX3D") && getenv("POMPPC_GL_TEX3D")[0] == '0');
            /* Cartes de cube (OpenGL 1.3) : pas de repli non plus. */
            G.cube = G.v10 && (G.q.caps & QGPU_CAP_GL14) &&
                     !(getenv("POMPPC_GL_CUBE") && getenv("POMPPC_GL_CUBE")[0] == '0');
            /* Répétitions et couleur de bordure (1.3, 1.4), niveaux S3TC
               relayés tels quels : l'hôte v10 les tient. POMPPC_GL_TEX13=0
               revient au refus (repli sur Apple). */
            G.tex13 = G.v10 && (G.q.caps & QGPU_CAP_GL14) &&
                      !(getenv("POMPPC_GL_TEX13") && getenv("POMPPC_GL_TEX13")[0] == '0');
            /* OpenGL 1.4 (docs/re/opengl-1.4.md) : biais de LOD de texture et
               d'unité, textures de profondeur et comparaison, GL_COLOR_SUM.
               POMPPC_GL_TEX14=0 revient au comportement 1.3. */
            G.tex14 = G.v10 && (G.q.caps & QGPU_CAP_GL14) &&
                      !(getenv("POMPPC_GL_TEX14") && getenv("POMPPC_GL_TEX14")[0] == '0');
            /* Crossbar (OpenGL 1.4) : protocole v12. Le rendu d'Apple se trompe
               dès qu'une source croisée entre dans une opération (scène tex14). */
            G.xbar = G.q.version >= 12 && (G.q.caps & QGPU_CAP_GL14) &&
                     !(getenv("POMPPC_GL_XBAR") && getenv("POMPPC_GL_XBAR")[0] == '0');
            G.scanout = G.q.version >= 13 && (G.q.caps & QGPU_CAP_SCANOUT) &&
                        !(getenv("POMPPC_GL_PRESENT") &&
                          getenv("POMPPC_GL_PRESENT")[0] == '0');
            G.pixops = G.q.version >= 13 &&
                       !(getenv("POMPPC_GL_PIXEL") &&
                         getenv("POMPPC_GL_PIXEL")[0] == '0');
            G.hostbuf = G.q.version >= 14 &&
                        !(getenv("POMPPC_GL_VBO") &&
                          getenv("POMPPC_GL_VBO")[0] == '0');
            G.v15 = G.q.version >= 15 &&
                    !(getenv("POMPPC_GL_XFER16") &&
                      getenv("POMPPC_GL_XFER16")[0] == '0');
            /* v16 : programmes ARB — seulement avec le chemin brut (c'est lui
               qui porte les attributs génériques et les clés) et si l'hôte
               compile (QGPU_CAP_PROGRAMS). POMPPC_GL_PROG=0 revient à
               l'émulation par GLEngine (repli sur Apple pour ces dessins). */
            G.prog = G.v7 && G.q.version >= 16 && (G.q.caps & QGPU_CAP_PROGRAMS) &&
                     !(getenv("POMPPC_GL_PROG") && getenv("POMPPC_GL_PROG")[0] == '0');
            /* Transmission paresseuse des gldUpdateDispatch au GLDriver
               d'Apple (docs/re/dispatch-paresseux.md) : POMPPC_GL_LAZYAPPLE=1
               l'allume, =0 l'éteint ; sans la variable, LAZY_DEFAULT. */
            {
                const char *lz = getenv("POMPPC_GL_LAZYAPPLE");
                G.lazy = lz && lz[0] ? lz[0] != '0' : LAZY_DEFAULT;
            }
            /* Génériques à taille déclarée (clé QGPU_SK_GEN_SIZES) : le chemin
               tableaux envoie chaque générique à la taille de son tableau
               (DOOM 3 / Prey : génériques en 11 mots au lieu de 16). Seulement si
               le device l'annonce — un device qui ne le sait pas refuserait
               la clé. POMPPC_GL_GENSIZES=0 revient aux 4 flottants. */
            G.gensizes = G.prog && (G.q.caps & QGPU_CAP_GEN_SIZES) &&
                         !(getenv("POMPPC_GL_GENSIZES") &&
                           getenv("POMPPC_GL_GENSIZES")[0] == '0');
            /* v18 : DRAW_NATIVE — les tableaux adossés à des VBO partent tels
               quels (miroirs bruts sur l'hôte), sans empaquetage. Il lui faut
               les tampons hôte v14 et le chemin brut. POMPPC_GL_NATIVE=0
               revient à l'empaquetage. */
            G.native = G.hostbuf && G.v7 && (G.q.caps & QGPU_CAP_NATIVE) &&
                       !(getenv("POMPPC_GL_NATIVE") &&
                         getenv("POMPPC_GL_NATIVE")[0] == '0');
            G.native_range = !(getenv("POMPPC_GL_NATIVE_RANGE") &&
                               getenv("POMPPC_GL_NATIVE_RANGE")[0] == '0');
            G.pixtex = -1;
            G.pixtex_w = G.pixtex_h = 0;
            G.buf_base = G.q.index * QGPU_CLIENT_BUF_IDS;
            G.query_base = G.q.index * QGPU_CLIENT_QUERY_IDS;
            /* Seulement si le bilan est demandé : un atexit pointe dans NOTRE
               code, et GLEngine peut décharger le plugin (NSUnLinkModule) avant
               la fin du processus — le bilan est alors écrit par
               pomppc_backend_fini, et le processus planterait à sa sortie. */
            if (getenv("POMPPC_GL_STATS"))
                atexit(on_exit_stats);
        } else {
            G.state = -1;
        }
        if (G.state > 0) {
            gl_note("plugin " POMPPC_PLUGIN_REV " qgpu v%lu caps 0x%lx v10=%d lazyapple=%d "
                    "native=%d (plages %d)\n",
                    G.q.version, G.q.caps, G.v10, G.lazy, G.native, G.native_range);
            pomppc_log("POMPPC: qgpu actif (tranche %lu à 0x%lx, %lu Mio, v%lu, caps 0x%lx,"
                       " chemin brut %s, pipeline fixe v8 %s, textures %s, soumission %s%s%s%s%s%s%s%s%s)\n",
                       G.q.index, G.q.base, G.q.size >> 20, G.q.version, G.q.caps,
                       G.v7 ? "actif" : "coupé", G.v8 ? "actif" : "coupé",
                       G.v10 ? "converties par l'hôte" : "converties ici",
                       G.async ? "asynchrone (2 moitiés)" : "synchrone",
                       G.async ? "" : " : ", G.async ? "" : G.async_why,
                       G.scanout ? ", présentation hôte" : "",
                       G.pixops ? ", pixels hôte" : "",
                       G.hostbuf ? ", VBO hôte" : "",
                       G.v15 ? ", host 16-bit xfer" : "",
                       G.gensizes ? ", génériques à taille déclarée" : "",
                       G.native ? ", VBO bruts (DRAW_NATIVE)" : "");
        } else
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
            QGPU_OP_DRAW_TRIANGLES_SEC, QGPU_OP_DRAW_TRIANGLES_SEC, QGPU_OP_DRAW_TRIANGLES_SEC,
            QGPU_OP_DRAW_TRIANGLES_SEC, QGPU_OP_DRAW_TRIANGLES_SEC,
        };
        int n = rk_units[G.run_kind], wide = n >= 3 || RK_IS_SEC(G.run_kind);
        c[0] = QGPU_CMD_HDR(ops[G.run_kind], wide ? QGPU_LEN_DRAW_N : QGPU_LEN_DRAW);
        c[1] = G.run_count;
        c[2] = G.base + VTX_OFF + G.run_start;
        if (wide) {
            c[3] = n;                   /* TEXN, SEC (v11) : nombre d'unités */
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
static void geom_check(const char *where, PCtx *p, unsigned long mode, unsigned long n,
                       unsigned long words, unsigned long off, unsigned long extra);
static void va_probe(PCtx *p, const unsigned char *V, unsigned long fmt,
                     const char *tag, int bad);
static void close_raw(void)
{
    if (G.raw_ctx && G.raw_count && raw_fix_nan()) {
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
        geom_check("Send", G.raw_ctx, G.raw_mode, (G.raw_vend - G.raw_start) / (G.raw_words * 4),
                   G.raw_words, G.raw_start, (G.raw_lots << 16) | (G.raw_fmt & 0xffff));
        {   /* 23/09/2026 : savoir de quel chemin viennent les DRAW_RAW d'un
               vidage (Begin/End ici, tableaux dans emit_draw_client) */
            static unsigned long said, said_big, said_gen;
            unsigned long nv = (G.raw_vend - G.raw_start) / (G.raw_words * 4);
            /* v16 : aussi les premiers lots à génériques 3/4 (Colin McRae en
               course : sommets reçus aux valeurs courantes) */
            int gen34 = (G.raw_fmt & (QGPU_VF_GEN(3) | QGPU_VF_GEN(4))) != 0;
            if (said < 3 || (nv >= 200 && said_big < 6) || (gen34 && said_gen < 4)) {
                const float *v0 = (const float *)(G.win + VTX_OFF + G.raw_start);
                unsigned char *g = gls(G.raw_ctx);
                const unsigned char *V = g ? (const unsigned char *)GLD_U32(g, GS_VAO) : 0;
                said++;
                if (nv >= 200)
                    said_big++;
                if (gen34)
                    said_gen++;
                gl_note("DRAW_RAW Begin/End #%lu image %lu : mode %lu n %lu fmt %lx mots %lu v0 %g %g %g %g v1 %g %g %g %g\n",
                        said, G.n_frames, G.raw_mode, G.raw_count, G.raw_fmt, G.raw_words,
                        v0[0], v0[1], v0[2], v0[3],
                        nv > 1 ? v0[G.raw_words] : 0.0f, nv > 1 ? v0[G.raw_words + 1] : 0.0f,
                        nv > 1 ? v0[G.raw_words + 2] : 0.0f, nv > 1 ? v0[G.raw_words + 3] : 0.0f);
                /* le descripteur de tableaux de GLEngine à cet instant : ce sont
                   ces tableaux qu'il déroule dans notre tampon */
                if (V)
                    va_probe(G.raw_ctx, V, G.raw_fmt, nv >= 200 ? "Begin/End grand lot" : "Begin/End", -1);
                /* v16 : pointeurs résolus de GLEngine (GC_VA_PTRS) pour les
                   emplacements 0..3 et 16..19, et les 8 premiers mots des
                   sommets 0 et 1 tels qu'écrits — à comparer entre gltest
                   (juste) et Colin McRae (valeurs courantes) */
                if (gen34) {
                    unsigned char *gc = gctx_of(G.raw_ctx);
                    const unsigned long *w0 = (const unsigned long *)v0;
                    int q;
                    if (gc) {
                        gl_note("  VA_PTRS :");
                        for (q = 0; q < 4; q++) gl_note(" [%d]=%08lx", q, GLD_U32(gc, GC_VA_PTRS + 4 * q));
                        for (q = 16; q < 20; q++) gl_note(" [%d]=%08lx", q, GLD_U32(gc, GC_VA_PTRS + 4 * q));
                        gl_note(" | gctx+0x4e1c=%08lx stride=%u desc=%08lx\n", GLD_U32(gc, 0x4e1c),
                                GLD_U16(gc, 0x4880), GLD_U32(gc, 0x48d0));   /* GC_VTX_STRIDE, GC_VTX_DESC, définis plus bas */
                    }
                    if (V)
                        gl_note("  V+0x300..0x33c : %08lx %08lx %08lx %08lx | %08lx %08lx %08lx %08lx | %08lx %08lx %08lx %08lx | %08lx %08lx %08lx %08lx\n",
                                GLD_U32(V, 0x300), GLD_U32(V, 0x304), GLD_U32(V, 0x308), GLD_U32(V, 0x30c),
                                GLD_U32(V, 0x310), GLD_U32(V, 0x314), GLD_U32(V, 0x318), GLD_U32(V, 0x31c),
                                GLD_U32(V, 0x320), GLD_U32(V, 0x324), GLD_U32(V, 0x328), GLD_U32(V, 0x32c),
                                GLD_U32(V, 0x330), GLD_U32(V, 0x334), GLD_U32(V, 0x338), GLD_U32(V, 0x33c));
                    if (V) {
                        /* la MÉMOIRE des tableaux génériques 0 et 1 à cet instant (sommet 0) */
                        const unsigned char *g0 = (const unsigned char *)GLD_U32(VA_SLOT(V, 16), 0);
                        const unsigned char *g1 = (const unsigned char *)GLD_U32(VA_SLOT(V, 17), 0);
                        if (g0 && g1)
                            gl_note("  tableau gen0 @%p : %08lx %08lx %08lx  gen1 @%p : %08lx %08lx %08lx (pas %lu)\n",
                                    (const void *)g0, ((const unsigned long *)g0)[0], ((const unsigned long *)g0)[1],
                                    ((const unsigned long *)g0)[2], (const void *)g1, ((const unsigned long *)g1)[0],
                                    ((const unsigned long *)g1)[1], ((const unsigned long *)g1)[2],
                                    GLD_U32(VA_SLOT(V, 16), 4));
                    }
                    gl_note("  mots v0 :");
                    for (q = 0; q < 8 && q < (int)G.raw_words; q++) gl_note(" %08lx", w0[q]);
                    gl_note(" | v1 :");
                    for (q = 0; q < 8 && q < (int)G.raw_words && nv > 1; q++) gl_note(" %08lx", w0[G.raw_words + q]);
                    gl_note("\n");
                }
                else
                    gl_note("  (pas de descripteur VAO)\n");
            }
        }
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
    stream_ready();                     /* F9 : G.cmd/G.ncmd relus après */
    close_run();
    close_raw();
    if (G.ncmd + words + 4 + QGPU_LEN_DRAW_N + QGPU_LEN_DRAW_RAW_BUF > CMD_WORDS)
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
/* L'arène pourrait-elle contenir n octets, même VIDE ? Non = ce n'est pas un
 * manque de place passager mais la limite de la moitié (6 Mio : au-delà de
 * 1280×1024 en 32 bits). La distinction compte pour P16 : sur un refus
 * PASSAGER il faut réessayer — et surtout ne pas se déclarer synchronisé —,
 * mais sur une limite DÉFINITIVE réessayer coûterait un vidage par dessin. */
static int arena_fits(unsigned long n)
{
    return ARENA_OFF + ((n + 3) & ~3UL) <= G.half;
}

static long arena_alloc(unsigned long n, unsigned long *off)
{
    stream_ready();                     /* F9 : G.arena/G.hb relus après */
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

/* P9 — L'HÔTE FAIT `break` SUR LE PREMIER REFUS (H4) : tout ce qui SUIT la
 * commande fautive dans la soumission est perdu — SET_STATE, SET_MATRIX,
 * TEX_IMAGE3, CTX_BIND, tout. Nos miroirs (« ce que l'hôte a déjà ») décrivent
 * alors un état que l'hôte n'a jamais reçu, et l'image reste fausse
 * DURABLEMENT : c'est le `TEX_DESTROY BAD_ARG` de la sortie d'UT2004
 * (docs/re/ut2004-demo.md). On les invalide donc TOUS, dans les trois branches
 * qui gardent l'accélération. Coût : une image entière d'état renvoyé. */
static void invalidate_mirrors(void)
{
    PCtx *p;
    PTex *t;

    for (p = G.list; p; p = p->next) {
        p->st_valid = 0;
        p->g_sent = 0;
        p->c_pstip_valid = 0;
        p->c_lmask = 0;
        memset(p->c_mtx, 0, sizeof(p->c_mtx));
        memset(p->c_vp, 0, sizeof(p->c_vp));
        memset(p->c_dr, 0, sizeof(p->c_dr));
        memset(p->c_light, 0, sizeof(p->c_light));
        memset(p->c_mat, 0, sizeof(p->c_mat));
        memset(p->c_lm, 0, sizeof(p->c_lm));
        memset(p->c_tg, 0, sizeof(p->c_tg));
        memset(p->c_tg_on, 0, sizeof(p->c_tg_on));
        memset(p->c_clip, 0, sizeof(p->c_clip));
        memset(p->c_cur, 0, sizeof(p->c_cur));
        memset(p->c_pstip, 0, sizeof(p->c_pstip));
        /* v16 : liaisons et program.env repartent ; les objets, eux, sont
           supposés créés (comme les textures) */
        p->cur_vp = p->cur_fp = 0;
        p->c_env_n[0] = p->c_env_n[1] = 0;
        p->c_gs_valid = 0;              /* tailles des génériques : à renvoyer */
        p->nat_cur_ok = 0;              /* v18 : valeurs courantes de DRAW_NATIVE */
    }
    /* v18 : un BUF_SUBDATA perdu laisserait un miroir brut faux pour de bon
       (on ne recopie que ce qui change) : tout est à recopier */
    buf_raw_invalidate_all();
    {
        int k;
        for (k = 0; k < PPROG_MAX; k++)
            if (pprog[k].ctx) {
                pprog[k].local_dirty = 1;
                /* Prey (23/09) : le texte compilé avant le vidage doit repartir
                   (PROG_STRING), sinon le rejeu natif ne connaît pas le
                   programme et jette la soumission au premier PROG_BIND. */
                pprog[k].text_sent = 0;
                pprog[k].len_sent = pprog[k].sum_sent = 0;
            }
    }
    for (t = G.textures; t; t = t->next) {
        t->prm_valid = 0;
        t->dirty = 1;                   /* le TEX_IMAGE3 perdu doit repartir */
        t->lv0_sig = 0;
        t->host_only = 0;
    }
    G.bound = 0;                        /* le CTX_BIND perdu doit repartir */
}

static void broken_all(const char *why, long st, unsigned long pc)
{
    PCtx *p;
    gl_note("broken_all %s : statut %ld commande %lu, image %lu\n", why, st, pc, G.n_frames);
    {   /* lot 11 : la commande fautive et la tête du lot, quelle que soit la branche */
        char hb2[260]; int a2 = 0; unsigned long k2, n2 = 0;
        if (pc < CMD_WORDS) {
            n2 = QGPU_CMD_LEN(G.cmd[pc]);
            if (n2 == 0 || n2 > 16 || pc + n2 > CMD_WORDS) n2 = 1;
            for (k2 = 0; k2 < n2 && a2 < (int)sizeof(hb2) - 12; k2++)
                a2 += snprintf(hb2 + a2, sizeof(hb2) - a2, " %lx", G.cmd[pc + k2]);
        }
        a2 += snprintf(hb2 + a2, sizeof(hb2) - a2, " | tete :");
        for (k2 = 0; k2 < 12 && k2 < G.ncmd && a2 < (int)sizeof(hb2) - 12; k2++)
            a2 += snprintf(hb2 + a2, sizeof(hb2) - a2, " %lx", G.cmd[k2]);
        hb2[a2] = 0;
        gl_note("FAUTIVE pc %lu (%lu mots) :%s\n", pc, G.ncmd, hb2);
    }
    /* Q2 — un SURF_PRESENT refusé tombait dans « lot ignoré, accélération
       gardée » (BAD_ARG) ou coupait toute la 3D du processus (autre statut) ;
       dans les deux cas present_direct continuait de rendre 1 et Apple
       n'échangeait rien : ÉCRAN FIGÉ POUR TOUJOURS, statut OK, zéro message
       (c'est le symptôme de QFB=1, §8.3 Q1). On coupe la présentation hôte, et
       elle seule : le chemin normal (relecture + échange d'Apple) reprend à
       l'image suivante, et la 3D continue. */
    if (pc < CMD_WORDS && QGPU_CMD_OP(G.cmd[pc]) == QGPU_OP_SURF_PRESENT) {
        pomppc_log("POMPPC: SURF_PRESENT refusé (statut %ld, commande %lu) : "
                   "présentation hôte coupée\n", st, pc);
        fprintf(stderr, "POMPPC GL: host present rejected (status %ld), "
                "falling back to the normal swap\n", st);
        G.scanout = 0;
        invalidate_mirrors();
        return;
    }
    /* v18 : un DRAW_NATIVE refusé (ou la recopie d'un miroir brut) ne coupe
       QUE DRAW_NATIVE : les tableaux adossés à des VBO repartent par
       l'empaquetage, qui a fait ses preuves. */
    if (pc < CMD_WORDS && G.native &&
        (QGPU_CMD_OP(G.cmd[pc]) == QGPU_OP_DRAW_NATIVE ||
         QGPU_CMD_OP(G.cmd[pc]) == QGPU_OP_BUF_SUBDATA)) {
        pomppc_log("POMPPC: %s refusé (statut %ld, commande %lu) : DRAW_NATIVE coupé\n",
                   QGPU_CMD_OP(G.cmd[pc]) == QGPU_OP_DRAW_NATIVE ? "DRAW_NATIVE" : "BUF_SUBDATA",
                   st, pc);
        fprintf(stderr, "POMPPC GL: host rejected native draw (status %ld), "
                "back to vertex packing\n", st);
        G.native = 0;
        invalidate_mirrors();
        return;
    }
    /* Un DRAW_RAW refusé vient presque toujours d'un sommet que l'application
       a laissé indéfini (NaN, infini, coordonnée démesurée) : GLEngine les
       découpait avant de nous les donner, plus maintenant. Couper le chemin
       brut suffit — l'accélération de la rastérisation, elle, reste bonne. */
    if (pc < CMD_WORDS &&
        (QGPU_CMD_OP(G.cmd[pc]) == QGPU_OP_DRAW_RAW ||
         QGPU_CMD_OP(G.cmd[pc]) == QGPU_OP_DRAW_RAW_BUF)) {
        {   /* lot 11 : vidage de la soumission fautive, une fois */
            static int done;
            const char *fd = getenv("POMPPC_GL_DUMPFAIL");
            if (fd && *fd && !done) {
                char fp[300];
                done = 1;
                snprintf(fp, sizeof(fp), "%s/fail-%06lu.bin", fd, G.n_frames);
                dump_one(fp);
                gl_note("DUMPFAIL %s : op %lx pc %lu statut %ld ncmd %lu\n",
                        fp, (unsigned long)QGPU_CMD_OP(G.cmd[pc]), pc, st, G.ncmd);
            }
        }
        pomppc_log("POMPPC: DRAW_RAW refusé (statut %ld, commande %lu) : "
                   "chemin brut coupé\n", st, pc);
        fprintf(stderr, "POMPPC GL: host rejected raw geometry "
                "(status %ld), falling back to rasterization\n", st);
        for (p = G.list; p; p = p->next)
            p->geom_lost = 1;
        G.v7 = 0;
        invalidate_mirrors();
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
        invalidate_mirrors();
        return;
    }
    {   /* Dire QUELLE commande, avec ses arguments : sans cela un refus coûte
           un aller-retour dans l'invité pour deviner. */
        unsigned long k, n = 0, op = 0;
        char buf[160];
        int at = 0;
        if (pc < CMD_WORDS) {
            op = QGPU_CMD_OP(G.cmd[pc]);
            n = QGPU_CMD_LEN(G.cmd[pc]);
            if (n == 0 || n > 16 || pc + n > CMD_WORDS)
                n = 1;
        }
        for (k = 0; k < n && at < (int)sizeof(buf) - 10; k++)
            at += snprintf(buf + at, sizeof(buf) - at, " %lx", G.cmd[pc + k]);
        buf[at] = 0;
        pomppc_log("POMPPC: commande fautive (op %lx) :%s\n", op, buf);
        {   /* lot 11 : tête du lot fautif, pour voir ce qui précède */
            char hb2[200]; int a2 = 0; unsigned long k2;
            for (k2 = 0; k2 < 20 && k2 < G.ncmd && a2 < (int)sizeof(hb2) - 12; k2++)
                a2 += snprintf(hb2 + a2, sizeof(hb2) - a2, " %lx", G.cmd[k2]);
            hb2[a2] = 0;
            gl_note("REJET statut %ld pc %lu op %lx :%s | tete du lot (%lu mots) :%s\n",
                    st, pc, op, buf, G.ncmd, hb2);
        }
        /* UT2004 : un BAD_ARG (filtre mipmap, sommet, etc.) tuait toute
           l'accélération → menu en quads blancs/jaunes, logiciel PPC.
           On jette CE lot et on continue : le texte peut encore partir. */
        if (st == QGPU_ST_BAD_ARG) {
            static unsigned n_badarg;
            pomppc_log("POMPPC: soumission %s refusée (BAD_ARG, pc %lu, op 0x%lx) : "
                       "lot ignoré, accélération gardée\n", why, pc, op);
            if (n_badarg < 8)
                fprintf(stderr, "POMPPC GL: batch rejected (BAD_ARG, pc %lu, op 0x%lx)%s, "
                        "acceleration kept\n", pc, op, buf);
            else if (n_badarg == 8)
                fprintf(stderr, "POMPPC GL: further BAD_ARG batches omitted\n");
            n_badarg++;
            invalidate_mirrors();
            return;
        }
    }
    pomppc_log("POMPPC: soumission refusée (%s, statut %ld, commande %lu) : "
               "accélération coupée\n", why, st, pc);
    fprintf(stderr, "POMPPC GL: submit rejected (status %ld, command %lu), "
            "falling back to software\n", st, pc);
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
        unsigned char *srcb = G.q.win + po->off;
        unsigned long bpp = po->pixbytes ? po->pixbytes : 4;
        unsigned long y, x;
        for (y = 0; y < po->h; y++) {
            unsigned char *s = srcb + y * po->w * bpp;
            unsigned char *d = po->dst + y * po->rowbytes;
            if (!po->depth || bpp == 2) {
                memcpy(d, s, po->w * bpp);
            } else {
                unsigned long *ss = (unsigned long *)s;
                unsigned long *dd = (unsigned long *)d;
                if (po->depth == 2) {           /* stencil : 8 bits bas du mot */
                    for (x = 0; x < po->w; x++)
                        dd[x] = (dd[x] & 0xFFFFFF00UL) | (ss[x] & 0xFF);
                } else if (po->packed) {        /* profondeur : 24 bits hauts */
                    for (x = 0; x < po->w; x++) {
                        float f = *(float *)(ss + x);
                        unsigned long z = (unsigned long)(clamp01(f) * po->scale + 0.5f);
                        dd[x] = (z & 0xFFFFFF00UL) | (dd[x] & 0xFF);
                    }
                } else {
                    for (x = 0; x < po->w; x++) {
                        float f = *(float *)(ss + x);
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
/* F8 : dépassements CONSÉCUTIFS avant de couper toute l'accélération. Un seul
 * suffisait, et il suffit qu'une AUTRE application 3D se ferme pour le
 * provoquer (212 doorbells synchrones côté kext, BQL tenu côté device : K5 +
 * D2) sans que rien ne soit cassé chez nous. */
#define WAIT_GIVEUP   3

/* `unlock` : le seul cas où l'on ne relâche PAS G.mu est l'attente de
 * submit_cur sur QUEUE_FULL — la moitié courante y est en cours de
 * soumission, son flux ne doit ni grandir ni repartir dans une autre
 * soumission. Voir stream_ready(). */
static void wait_half_ex(int i, int unlock)
{
    Half *h = &G.h[i];
    unsigned long fence;
    int ok;

    /* INVARIANT F9 : dès l'entrée et jusqu'à la sortie, G.halt est levé. Toute
       sortie de G.mu depuis cette fonction — le pthread_cond_wait ci-dessous
       comme l'attente elle-même — se fait donc FLUX GELÉ : stream_ready()
       endort quiconque voudrait écrire dans la tranche, y compris dans la
       moitié qu'on attend (switch_half l'a déjà désignée courante) et dans
       celle qu'on vient de soumettre. Tout le reste passe. */
    G.halt++;
    /* Un autre fil attend peut-être déjà CETTE barrière, verrou relâché. On
       dort sur la condition plutôt que d'attendre en double — et surtout
       plutôt que de repartir en croyant la moitié libre. */
    while (h->waiting)
        pthread_cond_wait(&half_cv, &G.mu);
    if (!h->busy) {
        run_posts(h);                   /* mode synchrone : rien à attendre */
        goto done;
    }
    {
        double t = now_s();
        fence = h->fence;
        if (unlock) {
            h->waiting = 1;
            pthread_mutex_unlock(&G.mu);
            ok = qgpu_wait(&G.q, fence, WAIT_MS) == 0;
            pthread_mutex_lock(&G.mu);
            h->waiting = 0;
            pthread_cond_broadcast(&half_cv);
        } else {
            /* submit_cur sur QUEUE_FULL : la moitié COURANTE est en cours de
               soumission. On garde G.mu, sinon un autre fil pourrait allonger
               son flux — ou le soumettre une seconde fois. */
            ok = qgpu_wait(&G.q, fence, WAIT_MS) == 0;
        }
        /* Rien de ce qu'on avait lu n'a pu bouger : G.halt interdisait d'écrire
           dans cette moitié, de la désigner courante et d'y ajouter une
           relecture ; `waiting` interdisait de l'attendre en double. h->busy et
           h->post sont donc encore ceux de NOTRE soumission. */
        G.t_wait += now_s() - t;
        G.n_waits++;
        if (!ok) {
            /* On ne peut pas réécrire une moitié encore en vol sans risquer une
               image fausse. Mais couper DÉFINITIVEMENT toute l'accélération du
               processus au premier dépassement était pire : on coupe d'abord
               l'asynchrone (le synchrone, lui, attend sa place sans jamais
               dépasser), et il faut WAIT_GIVEUP dépassements consécutifs pour
               déclarer l'hôte mort. */
            pomppc_log("POMPPC: barrière %lu jamais atteinte (%d ms), "
                       "dépassement %d/%d\n", fence, WAIT_MS,
                       G.wait_miss + 1, WAIT_GIVEUP);
            G.async = 0;
            G.async_avail = 0;
            G.async_why = "barrière dépassée";
            if (++G.wait_miss >= WAIT_GIVEUP) {
                PCtx *p;
                fprintf(stderr, "POMPPC GL: host did not finish a submit "
                        "in %d ms (%d times), falling back to software\n",
                        WAIT_MS, G.wait_miss);
                for (p = G.list; p; p = p->next)
                    p->broken = 1;
            } else {
                fprintf(stderr, "POMPPC GL: host submit took more than %d ms, "
                        "asynchronous doorbell disabled\n", WAIT_MS);
            }
            h->npost = 0;
            h->busy = 0;
            goto done;
        }
        G.wait_miss = 0;
    }
    h->busy = 0;
    run_posts(h);
done:
    G.halt--;
    pthread_cond_broadcast(&half_cv);
}

static void wait_half(int i)
{
    wait_half_ex(i, 1);
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
    qgpu_peek(&G.q, &e2, &status, &pc);
    /* Note (POMPPC_GL_NOTE) : une soumission asynchrone terminée en erreur
       est une série de dessins PERDUE (mur qui disparaît une image) — 22/09. */
    gl_note("ERRORS %lu -> %lu : statut %lu commande %lu, image %lu\n",
            G.errors, errors, status, pc, G.n_frames);
    G.errors = errors;
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

/* POMPPC_GL_DUMP=<dossier> — vidage de chaque soumission (traînées d'UT2004,
   22/09/2026) pour la REJOUER en natif sur l'hôte (tests/qgpu_replay.c) :
   en-tête, puis les zones utilisées de la moitié courante (flux de commandes,
   sommets, indices, arène) avec leurs offsets ABSOLUS dans BAR0, tels que le
   flux les désigne. POMPPC_GL_DUMP_FRAMES=n borne le nombre d'images (400). */
struct dump_hdr {
    unsigned long magic;                /* 'PQD1' */
    unsigned long frame;
    unsigned long base;                 /* G.base : offset absolu de la moitié */
    unsigned long ncmd_bytes;
    unsigned long vtx_off, vtx_len;     /* relatifs à la moitié */
    unsigned long idx_off, idx_len;
    unsigned long arena_off, arena_len;
    unsigned long reserved[6];
};
static void dump_submit(void)
{
    static int on = -1;
    static char dir[300];
    static unsigned long seq, maxf;
    struct dump_hdr h;
    char path[400];
    FILE *f;
    if (on < 0) {
        const char *e = getenv("POMPPC_GL_DUMP"), *m = getenv("POMPPC_GL_DUMP_FRAMES");
        on = 0;
        if (e && *e == '/') {
            snprintf(dir, sizeof(dir), "%s", e);
            on = 1;
        }
        maxf = (m && *m) ? (unsigned long)atol(m) : 400;
    }
    if (!on || !G.ncmd)
        return;
    {   /* POMPPC_GL_DUMP_TRIGGER=<fichier> : ne vider qu'à partir du moment où
           ce fichier existe (posé par ssh quand la scène voulue est à
           l'écran), pendant POMPPC_GL_DUMP_FRAMES images. Sans déclencheur :
           depuis le début, jusqu'à POMPPC_GL_DUMP_FRAMES. */
        static const char *trig = (const char *)-1;
        static unsigned long from = ~0UL;
        if (trig == (const char *)-1)
            trig = getenv("POMPPC_GL_DUMP_TRIGGER");
        if (trig && *trig) {
            if (from == ~0UL) {
                if (access(trig, F_OK) != 0)
                    return;
                from = G.n_frames;
                /* Vidage AUTONOME : réémettre tout l'état et retéléverser
                   toutes les textures, pour que le rejeu natif n'ait besoin de
                   rien d'antérieur (même geste que broken_all, P9). La
                   soumission EN COURS a été bâtie avec les anciens miroirs
                   (textures déjà à l'hôte, jamais réémises) : on ne vide qu'à
                   partir de la soumission SUIVANTE. Pas de l'image suivante
                   (lot 11) : les textes des programmes (Prey, 23/09) et les
                   textures réutilisées avant la fin de cette image repartent
                   DANS cette image, et le rejeu ne les aurait jamais. */
                invalidate_mirrors();
                return;
            }
            if (G.n_frames > from + maxf)
                return;
        } else if (G.n_frames > maxf) {
            return;
        }
    }
    snprintf(path, sizeof(path), "%s/%06lu.bin", dir, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(&h, 0, sizeof(h));
    h.magic = 0x50514431UL; h.frame = G.n_frames; h.base = G.base;
    h.ncmd_bytes = G.ncmd * 4;
    h.vtx_off = VTX_OFF; h.vtx_len = G.vtx;
    h.idx_off = IDX_OFF; h.idx_len = G.idx;
    h.arena_off = ARENA_OFF; h.arena_len = G.arena;
    fwrite(&h, sizeof(h), 1, f);
    fwrite(G.win, 1, h.ncmd_bytes, f);
    fwrite(G.win + VTX_OFF, 1, h.vtx_len, f);
    fwrite(G.win + IDX_OFF, 1, h.idx_len, f);
    fwrite(G.win + ARENA_OFF, 1, h.arena_len, f);
    fclose(f);
}

/* Lot 11 : vider LA soumission courante (autonome pour un DRAW_RAW : sommets et
   indices sont dans la fenêtre) quand elle vient d'être refusée, pour la
   rejouer en natif. Même format que dump_submit. */
static void dump_one(const char *path)
{
    struct dump_hdr h;
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    memset(&h, 0, sizeof(h));
    h.magic = 0x50514431UL; h.frame = G.n_frames; h.base = G.base;
    h.ncmd_bytes = G.ncmd * 4;
    h.vtx_off = VTX_OFF; h.vtx_len = G.vtx;
    h.idx_off = IDX_OFF; h.idx_len = G.idx;
    h.arena_off = ARENA_OFF; h.arena_len = G.arena;
    fwrite(&h, sizeof(h), 1, f);
    fwrite(G.win, 1, h.ncmd_bytes, f);
    fwrite(G.win + VTX_OFF, 1, h.vtx_len, f);
    fwrite(G.win + IDX_OFF, 1, h.idx_len, f);
    fwrite(G.win + ARENA_OFF, 1, h.arena_len, f);
    fclose(f);
}
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
    int async = G.async && !G.npend;

    dump_submit();
    if (async) {
        st = qgpu_submit_async(&G.q, G.hb, G.ncmd * 4, &fence, &errors);
        if (st == QGPU_ST_QUEUE_FULL) {
            /* Rien n'a été mis en file, et SUBMIT_OFF/SUBMIT_LEN se réécrivent
               sans danger : on attend notre plus ancienne barrière (il n'y en a
               qu'une : l'autre moitié) et on réessaie.
               G.mu reste TENU pendant cette attente (F9) : le flux de la moitié
               courante est en cours de soumission, personne ne doit y écrire ni
               le soumettre une seconde fois. */
            G.n_qfull++;
            wait_half_ex(G.cur ^ 1, 0);
            st = qgpu_submit_async(&G.q, G.hb, G.ncmd * 4, &fence, &errors);
        }
        if (st == QGPU_ST_QUEUE_FULL) {
            /* File toujours pleine (un autre client l'occupe) : le doorbell
               SYNCHRONE, lui, n'est jamais refusé — il attend sa place. */
            G.n_qfull++;
            async = 0;
        } else if (st != QGPU_ST_OK) {
            /* F12 — on ne resoumet QUE sur QUEUE_FULL. Sur tout autre statut
               (kext qui annonce l'asynchrone sans le tenir, K6), la trame a pu
               être PRISE puis refusée : la rejouer en synchrone l'exécuterait
               DEUX fois. On l'abandonne et on repasse en synchrone pour que la
               prochaine se nomme elle-même, exactement comme sur un mouvement
               d'ERRORS. */
            pomppc_log("POMPPC: doorbell asynchrone refusé (statut %ld) : trame "
                       "abandonnée, synchrone pendant %d images\n", st, ASYNC_RETRY);
            G.async = 0;
            G.n_syncfall++;
            G.async_retry_at = G.n_frames + ASYNC_RETRY;
            G.t_submit += now_s() - t;
            G.n_submits++;
            h->busy = 0;
            h->npost = 0;
            return;
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
    stream_ready();                     /* F9 : tout est relu après */
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
    if (G.npend)
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

static void *unit_drvtex(PCtx *p, int u, unsigned long *mask);

static long alloc_tex_id(PCtx *p)
{
    unsigned long i;
    PTex *t, *victim = NULL;
    PCtx *ctx;
    void *protected[QGPU_MAX_UNITS];
    unsigned long mask, *c;
    long id;
    int u;
    for (i = 0; i < QGPU_CLIENT_TEX_IDS; i++) {
        if (!(G.tex_used[i / 32] & (1UL << (i % 32)))) {
            G.tex_used[i / 32] |= 1UL << (i % 32);
            return G.q.tex_base + i;
        }
    }
    /* Close pending RAW/legacy draws and finish both asynchronous halves
     * before recycling an ID. DESTROY precedes CREATE in the new stream.
     * F9 : ces deux appels PEUVENT relâcher G.mu — la victime est donc
     * choisie APRÈS, sur l'état courant, et marquée morte avant tout ce qui
     * pourrait relâcher à nouveau. Sinon deux fils évinçaient la même. */
    flush();
    drain_all();
    /* Protect every effective unit, including units not uploaded yet. A
     * texture bound by another context may be evicted: that context will
     * reload it when drawn again. Guest storage remains owned by GLEngine. */
    for (u = 0; u < QGPU_MAX_UNITS; u++)
        protected[u] = unit_drvtex(p, u, &mask);
    for (t = G.textures; t; t = t->next) {
        /* P15 : une texture remplie par COPY_TEX n'existe QUE sur l'hôte — le
           niveau de l'invité n'a jamais été mis à jour. L'évincer perdrait son
           contenu, et le rechargement depuis l'invité remettrait l'ANCIEN
           (reflets et ombres périmés). Elle n'est jamais une victime. */
        if (t->qtex < 0 || t->host_only ||
            (victim && t->last_use >= victim->last_use))
            continue;
        for (u = 0; u < QGPU_MAX_UNITS; u++)
            if (protected[u] == t->drvtex)
                break;
        if (u == QGPU_MAX_UNITS)
            victim = t;
    }
    if (!victim)
        return -1;
    id = victim->qtex;
    victim->qtex = -1;                  /* morte AVANT reserve() : plus personne
                                           ne peut la choisir comme victime */
    victim->dirty = 1;
    victim->prm_valid = 0;
    victim->host_only = 0;
    c = reserve(p, QGPU_LEN_TEX);
    c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_DESTROY, QGPU_LEN_TEX);
    c[1] = id;
    for (ctx = G.list; ctx; ctx = ctx->next)
        ctx->st_valid = 0;
    G.n_texevictions++;
    return id;                          /* bitmap bit stays allocated */
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

/* Verrou déjà tenu. WC3 (et d'autres) peuvent TexImage avant que
 * gldCreateTexture ait été vu, ou passer un objet que CreateTextureLevel
 * est le premier à nommer : sans ça, le premier lot de glyphes tombe en
 * NO_TEX_UNKNOWN, geom_lost, et le texte n'apparaît qu'au redraw logiciel. */
static PTex *intern_tex(void *drvtex)
{
    PTex *t;
    if (!drvtex)
        return 0;
    t = find_tex(drvtex);
    if (t)
        return t;
    t = calloc(1, sizeof(*t));
    if (!t)
        return 0;
    t->drvtex = drvtex;
    t->qtex = -1;
    t->dirty = 1;
    t->next = G.textures;
    G.textures = t;
    t->hnext = tex_hash[tex_bucket(drvtex)];
    tex_hash[tex_bucket(drvtex)] = t;
    return t;
}

void pomppc_texture_created(void *drvtex)
{
    if (G.state <= 0 || !drvtex)
        return;
    pthread_mutex_lock(&G.mu);
    intern_tex(drvtex);
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
            /* P10 : close_run() seul fermait la série de triangles mais PAS la
               série DRAW_RAW — c'est flush() qui l'écrivait, donc APRÈS le
               TEX_DESTROY de la texture qu'elle emploie. reserve() ferme les
               deux, garantit la place, et relie le contexte (une soumission
               qui commencerait par TEX_DESTROY aurait QGPU_ST_NO_CTX).
               L'identifiant n'est rendu que si la commande est bien écrite —
               même règle que P14. */
            PCtx *bp = (G.bound && G.bound->qctx >= 0) ? G.bound : G.list;
            while (bp && bp->qctx < 0)
                bp = bp->next;
            if (bp) {
                c = reserve(bp, QGPU_LEN_TEX);
                c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_DESTROY, QGPU_LEN_TEX);
                c[1] = t->qtex;
                flush();
                G.tex_used[i / 32] &= ~(1UL << (i % 32));
            }
        }
        free(t);
        break;
    }
    pthread_mutex_unlock(&G.mu);
}

void pomppc_texture_changed(void *drvtex, int levels)
{
    PTex *t;
    if (G.state <= 0 || !drvtex)
        return;
    pthread_mutex_lock(&G.mu);
    t = intern_tex(drvtex);
    if (t && levels)
        t->dirty = 1;
    pthread_mutex_unlock(&G.mu);
}

/* Couples (format, type) que convert_level sait traduire. Le chemin brut doit
 * le savoir SANS convertir : le domaine est décidé à chaque changement d'état,
 * bien avant le dessin. Toute entrée ajoutée ici doit avoir son cas ci-dessous. */
/* v10 : octets par texel des couples que le cœur convertit lui-même (liste de
   qgpu_proto.h, section v10) ; 0 = couple inconnu de l'hôte. */
static unsigned long host_texel_bytes(unsigned int fmt, unsigned int type)
{
    switch (type) {
    case 0x1401:                                         /* octets */
        switch (fmt) {
        case 0x1908: case 0x80E1: return 4;
        case 0x1907: case 0x80E0: return 3;
        case 0x190A: return 2;
        case 0x1909: case 0x1906: case 0x1903: return 1;
        case 0x1900: case 0x80E5: case 0x8049: return 1; /* COLOR_INDEX, INDEX8, INTENSITY */
        }
        return 0;
    case 0x8035: case 0x8367:
        return (fmt == 0x1908 || fmt == 0x80E1) ? 4 : 0;
    case 0x8363: case 0x8364:
        return fmt == 0x1907 ? 2 : 0;
    case 0x8033: case 0x8034:
        return (fmt == 0x1908 || fmt == 0x80E1) ? 2 : 0;
    case 0x8365: case 0x8366:
        return (fmt == 0x80E1 || fmt == 0x1908) ? 2 : 0;
    case 0x1406: case 0x1405:                            /* profondeur (G.tex14) */
        return (fmt == 0x1902 && G.tex14) ? 4 : 0;
    case 0x1403:
        return (fmt == 0x1902 && G.tex14) ? 2 : 0;
    }
    return 0;
}

/* Profondeur : l'hôte n'accepte le format GL_DEPTH_COMPONENT que sur une texture
 * de format de base GL_DEPTH_COMPONENT, et inversement — sinon il refuserait la
 * soumission entière. GLEngine range bien les deux ensemble (relevé : niveau
 * 0x1902/0x1405, base 0x1902), mais un niveau d'un autre format peut s'ajouter. */
static int depth_pair_ok(unsigned long base, unsigned int fmt)
{
    return (fmt == 0x1902) == (base == 0x1902);
}

/* H1 — GL_ALPHA part désormais TEL QUEL vers le cœur.
 *
 * Le plugin blanchissait lui-même les niveaux GL_ALPHA (« blanc + A », en
 * RGBA) pour contourner un hôte GL qui promeut le BGRA R=G=B=0 et noircit les
 * glyphes. Conséquence : le cœur ne voyait JAMAIS 0x1906, son propre bogue
 * (GL_ALPHA décodé en « blanc + A », donc REPLACE/ADD/BLEND rendus blancs)
 * restait invisible en VM, et le test natif restait rouge. Le correctif est en
 * QUATRE endroits ou rien (§9, ligne H1) : cœur:516 → argb(a,0,0,0),
 * cœur:2248/2262 sans promotion RGBA, backend GL avec GL_ALPHA en format
 * interne, et ICI : plus de pack_alpha_as_rgba, plus de cas 0x1906 dans
 * convert_level, plus d'override de format à l'envoi. Ne retirer qu'une part
 * des quatre rendrait les polices noires.
 *
 * Sans device v10 (TEX_IMAGE hérité, qui ne transporte que du xRGB8888), il
 * n'y a aucun moyen de faire voir 0x1906 au cœur : base_format_ok refuse
 * alors GL_ALPHA et c'est le rendu d'Apple — exact — qui s'en charge.
 */

/* S3TC : GLEngine range les blocs tels quels (format 0x83F0..0x83F3, type 0),
 * qu'ils viennent de glCompressedTexImage2D ou de sa propre compression d'un
 * format générique (GL_COMPRESSED_RGB → DXT1). L'hôte v10 les décode. */
static int dxt_format(unsigned int fmt, unsigned int type)
{
    return type == 0 && fmt >= 0x83F0 && fmt <= 0x83F3;
}

/* Octets des données d'un niveau compressé de w × h : blocs de 4×4 serrés. */
static unsigned long dxt_bytes(unsigned int fmt, unsigned long w, unsigned long h)
{
    return ((w + 3) / 4) * ((h + 3) / 4) * (fmt <= 0x83F1 ? 8 : 16);
}

/* Format de base pour l'hôte. Après glCompressedTexImage2D, le GLDriver range
 * le format COMPRESSÉ comme format de base (0x83F0…) ; après une compression
 * par GLEngine, c'est le vrai format de base (GL_RGB). */
static unsigned long host_base(unsigned long base)
{
    if (base == 0x83F0)
        return 0x1907;
    if (base >= 0x83F1 && base <= 0x83F3)
        return 0x1908;
    return base;
}

/* Un niveau S3TC est-il accepté par l'hôte avec ce format de base ? DXT1 RGB
 * veut GL_RGB, les trois autres GL_RGBA (qgpu_proto.h, section v10) : tout
 * autre couple ferait REFUSER la soumission, pas seulement la texture. */
static int dxt_ok(unsigned long base, unsigned int fmt)
{
    return G.tex13 && host_base(base) == (fmt == 0x83F0 ? 0x1907UL : 0x1908UL);
}

static int level_convertible(unsigned int fmt, unsigned int type)
{
    if (G.v10)
        return host_texel_bytes(fmt, type) != 0;
    switch ((fmt << 16) | type) {
    case (0x1908 << 16) | 0x1401: case (0x1907 << 16) | 0x1401:
    case (0x80E1 << 16) | 0x1401: case (0x80E0 << 16) | 0x1401:
    case (0x80E1 << 16) | 0x8367: case (0x1908 << 16) | 0x8035:
    case (0x80E1 << 16) | 0x8035: case (0x1908 << 16) | 0x8367:
    case (0x80E1 << 16) | 0x8366: case (0x1907 << 16) | 0x8363:
    case (0x1908 << 16) | 0x8033: case (0x1909 << 16) | 0x1401:
    case (0x190A << 16) | 0x1401:       /* H1 : plus de 0x1906 ici — voir plus haut */
    case (0x1903 << 16) | 0x1401: case (0x1900 << 16) | 0x1401:
    case (0x80E5 << 16) | 0x1401: case (0x8049 << 16) | 0x1401:
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
    case (0x8049 << 16) | 0x1401:                        /* INTENSITY (octet) */
    case (0x1900 << 16) | 0x1401:                        /* COLOR_INDEX */
    case (0x80E5 << 16) | 0x1401:                        /* COLOR_INDEX8_EXT */
        for (i = 0; i < n; i++)
            out[i] = 0xFF000000UL | (d[i] * 0x010101UL);
        return 1;
    case (0x190A << 16) | 0x1401:                        /* LUMINANCE_ALPHA */
        for (i = 0; i < n; i++, d += 2)
            out[i] = ((unsigned long)d[1] << 24) | (d[0] * 0x010101UL);
        return 1;
    /* H1 : GL_ALPHA n'a plus de cas ici. Le TEX_IMAGE hérité ne transporte que
       du xRGB8888 : blanchir (ou noircir) le niveau serait décider à la place
       du cœur. base_format_ok refuse GL_ALPHA sans la v10 et le rendu d'Apple
       s'en charge. */
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
    /* COLOR_INDEX / INDEX8 : sans la table de couleurs, les envoyer en
       luminance donnait le monde blanc/bruité de Colin McRae (comme les
       quads blancs d'UT2004). On refuse : GLEngine reprend, palette comprise. */
    if (f == 0x1900 || f == 0x80E5)
        return 0;
    f = host_base(f);
    /* H1 : sans la v10, TEX_IMAGE ne transporte que du xRGB8888 — le cœur ne
       pourrait pas recevoir 0x1906 tel quel, et convertir ici reviendrait à
       inventer une couleur. Le rendu d'Apple, lui, est exact. */
    if (f == 0x1906 && !G.v10)
        return 0;
    return (f >= 0x1906 && f <= 0x190A) || f == 0x8049 || (f == 0x1902 && G.tex14);
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
    return w == 0x2900 || w == 0x2901 || w == 0x812F ||
           (G.tex13 && (w == 0x812D || w == 0x8370));   /* v10 : BORDER, MIRRORED */
}

/* Biais de LOD pour l'hôte, qui refuse |biais| > QGPU_MAX_LOD_BIAS. OpenGL
 * borne la SOMME des biais de texture et d'unité à ±GL_MAX_TEXTURE_LOD_BIAS ;
 * chacun est ici borné séparément, ce qui ne change le résultat que pour des
 * sommes au-delà de ±16 — soit plus que les 12 niveaux d'une texture. */
static float lod_bias_clamp(float b)
{
    if (!(b == b))
        return 0.0f;
    return b > QGPU_MAX_LOD_BIAS ? QGPU_MAX_LOD_BIAS :
           b < -QGPU_MAX_LOD_BIAS ? -QGPU_MAX_LOD_BIAS : b;
}

/* Couleur de bordure de GLEngine en mot 0xAARRGGBB (QGPU_TP_BORDER_COLOR). */
static unsigned long tex_border(const unsigned char *gp)
{
    unsigned long v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        float f = GLD_F32(gp, TP_BORDER + 4 * i);
        unsigned long b = f > 0.0f ? (f >= 1.0f ? 255 : (unsigned long)(f * 255.0f + 0.5f)) : 0;
        v |= b << (i == 3 ? 24 : 16 - 8 * i);
    }
    return v;
}

/* Niveaux et bornes de LOD (OpenGL 1.2). Avant la v10 l'hôte ne les connaît
 * pas : il faut alors qu'ils soient à leurs valeurs initiales, sinon le rendu
 * d'Apple reprend la main — jusqu'ici ils étaient IGNORÉS en silence. En v10 ils
 * partent à l'hôte, dans les bornes que le cœur accepte. */
static int tex_lod_ok(const unsigned char *gp)
{
    float mn = GLD_F32(gp, TP_MIN_LOD), mx = GLD_F32(gp, TP_MAX_LOD);
    unsigned long b = U16(gp, TP_BASE_LEVEL), m = U16(gp, TP_MAX_LEVEL);
    if (!G.v10 || !(G.q.caps & QGPU_CAP_GL14))
        return mn == -1000.0f && mx == 1000.0f && b == 0 && m == 1000;
    return mn == mn && mx == mx && mn >= -1e6f && mn <= 1e6f && mx >= -1e6f && mx <= 1e6f &&
           b < DT_LEVELS && m <= 1000;
}

static int tex_params_ok(const unsigned char *gp)
{
    int t3 = GLD_U8(gp, TP_TARGET) == 1;
    /* Avant la v10, GL_CLAMP prend le noir transparent chez l'hôte : une autre
       couleur de bordure reste au rendu d'Apple. */
    if (!G.tex13 && tex_border(gp) &&
        (U16(gp, TP_WRAP_S) == 0x2900 || U16(gp, TP_WRAP_T) == 0x2900 ||
         (t3 && U16(gp, TP_WRAP_R) == 0x2900)))
        return 0;
    if (G.tex14) {
        unsigned long cm = U16(gp, TP_COMPARE_MODE), cf = U16(gp, TP_COMPARE_FUNC);
        unsigned long dm = U16(gp, TP_DEPTH_MODE);
        float lb = GLD_F32(gp, TP_LOD_BIAS);
        if ((cm != 0 && cm != 0x884E) || cf < 0x0200 || cf > 0x0207 ||
            (dm != 0x1909 && dm != 0x8049 && dm != 0x1906) || lb != lb)
            return 0;
    }
    return tex_filter_ok(U16(gp, TP_MIN)) && tex_filter_ok(U16(gp, TP_MAG)) &&
           tex_wrap_ok(U16(gp, TP_WRAP_S)) && tex_wrap_ok(U16(gp, TP_WRAP_T)) &&
           (!t3 || tex_wrap_ok(U16(gp, TP_WRAP_R))) &&
           tex_lod_ok(gp);
}

/* v10 : carte de cube (cible 0 de l'objet de GLEngine) ? */
static int tex_is_cube(const PTex *t)
{
    const unsigned char *gp = (const unsigned char *)GLD_U32(t->drvtex, DT_PARAMS);
    return gp && GLD_U8(gp, TP_TARGET) == 0;
}

/* Entrée du niveau l de la face f, dans l'objet de GLEngine. */
static const unsigned char *cube_level(const void *drvtex, int f, int l)
{
    const unsigned char *gp = (const unsigned char *)GLD_U32(drvtex, DT_PARAMS);
    return gp + TP_LEVEL0 + f * TP_FACE_SIZE + l * TP_LEVEL_SIZE;
}


/* Complétude d'OpenGL 1.2 : niveau de base, puis, si le filtre de réduction
 * emploie les mipmaps, les niveaux b+1..q avec q = min(b + log2(max(w, h, d)),
 * MAX_LEVEL), chacun de la moitié du précédent. C'est la règle que le cœur de
 * l'hôte applique ; le verdict d'Apple (CTX_TEXTURING) ignore niveau de base
 * et niveau max, et ne connaît pas la 3D (vu en vrai, scène texlod). */
static int tex_complete(const void *drvtex)
{
    const unsigned char *gp = (const unsigned char *)GLD_U32(drvtex, DT_PARAMS);
    const unsigned char *lv;
    unsigned long b, q, w, h, d = 1, m, n, minf;
    int t3;

    if (!gp)
        return 0;
    b = U16(gp, TP_BASE_LEVEL);
    if (b >= DT_LEVELS)
        return 0;
    if (GLD_U8(gp, TP_TARGET) == 0) {
        /* carte de cube : six faces carrées, de même taille, à chaque niveau */
        const unsigned char *e0 = cube_level(drvtex, 0, b);
        int f;
        w = U16(e0, PL_W);
        if (!w || U16(e0, PL_H) != w)
            return 0;
        minf = U16(gp, TP_MIN);
        m = w;
        for (q = b; m > 1; m >>= 1)
            q++;
        if (minf == 0x2600 || minf == 0x2601)
            q = b;
        else if (U16(gp, TP_MAX_LEVEL) < q)
            q = U16(gp, TP_MAX_LEVEL);
        if (q < b || q >= DT_LEVELS)
            return 0;
        for (n = b; n <= q; n++, w = w > 1 ? w / 2 : 1)
            for (f = 0; f < 6; f++) {
                const unsigned char *e = cube_level(drvtex, f, n);
                if (U16(e, PL_W) != w || U16(e, PL_H) != w || !GLD_U32(e, PL_DATA) ||
                    U16(e, PL_FORMAT) != U16(e0, PL_FORMAT))
                    return 0;
            }
        return 1;
    }
    lv = (const unsigned char *)drvtex + DT_LEVEL0 + b * DT_LEVEL_SIZE;
    w = S16(lv, LV_W); h = S16(lv, LV_H);
    if (!w || !h || !GLD_U32(lv, LV_DATA))
        return 0;
    t3 = GLD_U8(gp, TP_TARGET) == 1;
    if (t3) {
        d = U16(gp + TP_LEVEL0 + b * TP_LEVEL_SIZE, PL_D);
        if (!d)
            return 0;
    }
    minf = U16(gp, TP_MIN);
    if (minf == 0x2600 || minf == 0x2601)
        return 1;
    m = w > h ? w : h;
    if (d > m)
        m = d;
    for (q = b; m > 1; m >>= 1)
        q++;
    if (U16(gp, TP_MAX_LEVEL) < q)
        q = U16(gp, TP_MAX_LEVEL);
    if (q < b)
        return 0;
    for (n = b + 1; n <= q; n++) {
        w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; d = d > 1 ? d / 2 : 1;
        if (n >= DT_LEVELS)
            return 0;
        lv = (const unsigned char *)drvtex + DT_LEVEL0 + n * DT_LEVEL_SIZE;
        if ((unsigned long)S16(lv, LV_W) != w || (unsigned long)S16(lv, LV_H) != h ||
            !GLD_U32(lv, LV_DATA) ||
            (t3 && U16(gp + TP_LEVEL0 + n * TP_LEVEL_SIZE, PL_D) != d))
            return 0;
    }
    return 1;
}

/* Niveau de base défini, même si la chaîne de mipmaps est incomplète.
 * Le MIN_FILTER par défaut d'OpenGL est NEAREST_MIPMAP_LINEAR : une texture
 * à un seul niveau (polices et curseur d'UT2004) est alors « incomplète ».
 * Couper le texturage dessinait des quads de la couleur du sommet — blancs.
 * On garde le niveau de base ; upload_texture rabat le filtre vers LINEAR
 * ou NEAREST, que l'hôte accepte sans mipmaps. */
static int tex_base_ok(const void *drvtex)
{
    const unsigned char *gp = (const unsigned char *)GLD_U32(drvtex, DT_PARAMS);
    const unsigned char *lv;
    unsigned long b;

    if (!gp)
        return 0;
    b = U16(gp, TP_BASE_LEVEL);
    if (b >= DT_LEVELS)
        return 0;
    if (GLD_U8(gp, TP_TARGET) == 0) {
        const unsigned char *e0 = cube_level(drvtex, 0, b);
        return U16(e0, PL_W) && U16(e0, PL_H) == U16(e0, PL_W) && GLD_U32(e0, PL_DATA);
    }
    lv = (const unsigned char *)drvtex + DT_LEVEL0 + b * DT_LEVEL_SIZE;
    return S16(lv, LV_W) > 0 && S16(lv, LV_H) > 0 && GLD_U32(lv, LV_DATA);
}

/* v10 : la texture est-elle une texture 3D (cible de l'objet de GLEngine) ? */
static int tex_is_3d(const PTex *t)
{
    const unsigned char *gp = (const unsigned char *)GLD_U32(t->drvtex, DT_PARAMS);
    return gp && GLD_U8(gp, TP_TARGET) == 1;
}

/* Profondeur et hauteur de tranche du niveau l, dans l'objet de GLEngine. */
static void tex_level_depth(const PTex *t, int l, unsigned long *d, unsigned long *imgh)
{
    const unsigned char *gp = (const unsigned char *)GLD_U32(t->drvtex, DT_PARAMS);
    const unsigned char *pl = gp + TP_LEVEL0 + l * TP_LEVEL_SIZE;
    *d = U16(pl, PL_D);
    *imgh = U16(pl, PL_IMGH);
}

/* Empreinte du niveau 0 : pointeur, taille, format, et un échantillon des
 * texels. WC3 remplit l'atlas de polices EN PLACE (même pointeur, mêmes
 * dimensions) : sans les octets, on ne reremplissait jamais et le texte
 * n'apparaissait qu'après un autre dirty (souvent des secondes plus tard). */
static unsigned long tex_lv0_sig(const PTex *t)
{
    const unsigned char *lv = (const unsigned char *)t->drvtex + DT_LEVEL0;
    const unsigned char *d = (const unsigned char *)GLD_U32(lv, LV_DATA);
    unsigned long w = (unsigned short)S16(lv, LV_W);
    unsigned long h = (unsigned short)S16(lv, LV_H);
    unsigned int fmt = U16(lv, LV_FORMAT), type = U16(lv, LV_TYPE);
    unsigned long sig = GLD_U32(lv, LV_DATA) ^ (w << 16) ^ h ^
                        ((unsigned long)fmt << 8) ^ type;
    /* S1 (relecture du 24/09) : sigsetjmp hors d'un &&, et l'empreinte de
       base gardée dans un volatile (un local modifié après sigsetjmp est
       indéterminé au retour de siglongjmp). */
    volatile unsigned long vsig = sig;
    unsigned long n;
    /* (24/09, vitres) : pas de sortie anticipée sous upload_blank — l'empreinte
       doit être la MÊME qu'au dessin suivant, sinon la texture paraît modifiée
       à chaque copie et repart en noir avant chaque COPY_TEX de bord, qui
       n'écrit qu'une colonne : la copie 640×480 était effacée. La garde
       sig_jmp couvre la lecture d'un niveau non engagé. */
    if (d && w && h) {
        if (sigsetjmp(sig_jmp, 0) != 0) {
            sig_jmp_on = 0;             /* niveau illisible (24/09, DOOM 3) */
            return vsig ^ 0x5a5a5a5aUL;
        }
        sig_thr = pthread_self();       /* P3 (relecture du 24/09) */
        sig_jmp_on = 1;
        /* P4 — `n` est un index d'OCTET, pas un compte de TEXELS. Un niveau
           DXT1 fait 0,5 octet par texel : lire d[w·h−1] lisait à DEUX FOIS la
           taille du niveau — 512 Kio au-delà sur 1024², et à CHAQUE dessin
           (texture_uploadable appelle ceci). Bus error sur UT2004 (S3TC).
           Taille réelle : blocs serrés pour S3TC, LV_ROWPIX × h × octets par
           texel sinon. */
        if (dxt_format(fmt, type)) {
            n = dxt_bytes(fmt, w, h);
        } else {
            unsigned long rp = (unsigned short)S16(lv, LV_ROWPIX);
            unsigned long tb = host_texel_bytes(fmt, type);
            if (rp < w)
                rp = w;
            if (!tb)
                tb = 1;                 /* couple inconnu : la borne la plus basse */
            n = rp * h * tb;
        }
        if (n >= 4)
            sig ^= GLD_U32(d, 0);
        if (n >= 12)
            sig ^= GLD_U32(d, 8);
        if (n > 32)
            sig ^= d[n / 2] | ((unsigned long)d[n - 1] << 16);
        sig_jmp_on = 0;
    }
    return sig;
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
    if (t->qtex >= 0 && !t->dirty) {
        /* Sonde POMPPC_GL_TEXALWAYS : l'empreinte à quatre points peut manquer
           un remplissage en place (atlas de polices). Tout retéléverser est
           lent mais dit si c'est bien la détection qui laisse l'hôte périmé. */
        static long always = -1;
        if (always < 0) {
            const char *e = getenv("POMPPC_GL_TEXALWAYS");
            always = (e && *e && *e != '0') ? 1 : 0;
        }
        if (always || tex_lv0_sig(t) != t->lv0_sig)
            t->dirty = 1;
        else
            return 1;
    }
    /* Residency is a cache, not a domain restriction: at most four textures
     * are protected for a draw, out of 128 host slots. */
    if (tex_is_3d(t) && !G.tex3d)
        return no(NO_TEX_TARGET, 2, 0);
    if (tex_is_cube(t)) {
        int f;
        if (!G.cube || S16(dt + DT_LEVEL0, LV_BORDER))
            return no(NO_TEX_TARGET, 1, 0);
        for (f = 0; f < 6; f++)
            for (l = 0; l < DT_LEVELS; l++) {
                const unsigned char *e = cube_level(dt, f, l);
                unsigned long w = U16(e, PL_W), h = U16(e, PL_H);
                if (!w || !h)
                    continue;
                if (w != h || w > QGPU_MAX_TEX_DIM || !GLD_U32(e, PL_DATA) ||
                    U16(e, PL_FORMAT) == 0x1902 ||              /* profondeur : 1D, 2D */
                    (dxt_format(U16(e, PL_FORMAT), U16(e, PL_TYPE))
                         ? !dxt_ok(base, U16(e, PL_FORMAT))
                         : U16(e, PL_ROWPIX) < w ||
                           !host_texel_bytes(U16(e, PL_FORMAT), U16(e, PL_TYPE))))
                    return no(NO_TEX_FORMAT, (U16(e, PL_FORMAT) << 16) | U16(e, PL_TYPE),
                              (f << 16) | w);
            }
        return 1;
    }
    for (l = 0; l < DT_LEVELS; l++) {
        unsigned char *lv = dt + DT_LEVEL0 + l * DT_LEVEL_SIZE;
        unsigned long w = S16(lv, LV_W), h = S16(lv, LV_H);
        if (w == 0 || h == 0)
            continue;
        if (S16(lv, LV_BORDER) || w > QGPU_MAX_TEX_DIM || h > QGPU_MAX_TEX_DIM)
            return no(NO_TEX_SIZE, w, h);
        if (G.v10 && dxt_format(U16(lv, LV_FORMAT), U16(lv, LV_TYPE))) {
            /* S3TC : 2D seulement, format de base assorti */
            if (tex_is_3d(t) || !GLD_U32(lv, LV_DATA) || !dxt_ok(base, U16(lv, LV_FORMAT)))
                return no(NO_TEX_FORMAT, (unsigned long)U16(lv, LV_FORMAT) << 16,
                          (host_base(base) << 16) | w);
            continue;
        }
        if (tex_is_3d(t)) {
            unsigned long d, imgh;
            tex_level_depth(t, l, &d, &imgh);
            if (d == 0 || w > QGPU_MAX_TEX_3D_DIM || h > QGPU_MAX_TEX_3D_DIM ||
                d > QGPU_MAX_TEX_3D_DIM || imgh < h)
                return no(NO_TEX_SIZE, w, (h << 16) | d);
        }
        if ((!GLD_U32(lv, LV_DATA) && !upload_blank) ||
            (!upload_blank &&
             (G.v10 ? S16(lv, LV_ROWPIX) < (short)w : S16(lv, LV_ROWPIX) != (short)w)) ||
            !level_convertible(U16(lv, LV_FORMAT), U16(lv, LV_TYPE)) ||
            !depth_pair_ok(host_base(base), U16(lv, LV_FORMAT)) ||
            (U16(lv, LV_FORMAT) == 0x1902 && tex_is_3d(t)))
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
    unsigned long base = GLD_U32(dt, DT_BASE_FORMAT), prm[14], off, *c;
    static const unsigned long keys[14] = {
        QGPU_TP_MIN_FILTER, QGPU_TP_MAG_FILTER, QGPU_TP_WRAP_S, QGPU_TP_WRAP_T,
        QGPU_TP_WRAP_R, QGPU_TP_MIN_LOD, QGPU_TP_MAX_LOD, QGPU_TP_BASE_LEVEL,
        QGPU_TP_MAX_LEVEL, QGPU_TP_BORDER_COLOR, QGPU_TP_LOD_BIAS, QGPU_TP_COMPARE_MODE,
        QGPU_TP_COMPARE_FUNC, QGPU_TP_DEPTH_MODE };
    int l, k, t3 = tex_is_3d(t);

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
        t->qtex = alloc_tex_id(p);
        if (t->qtex < 0)
            return no(NO_TEX_ID, 0, 0);
        if (t3 || tex_is_cube(t)) {
            c = reserve(p, QGPU_LEN_TEX_CREATE3);
            c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_CREATE3, QGPU_LEN_TEX_CREATE3);
            c[1] = t->qtex; c[2] = t3 ? QGPU_TT_3D : QGPU_TT_CUBE_MAP;
        } else {
            c = reserve(p, QGPU_LEN_TEX);
            c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_CREATE, QGPU_LEN_TEX);
            c[1] = t->qtex;
        }
        t->dirty = 1;
        t->prm_valid = 0;
    }
    t->last_use = ++G.tex_clock;
    if (t->dirty) {
        static unsigned texlog;
        const unsigned char *lv0 = dt + DT_LEVEL0;
        if (texlog < 16) {
            gl_note("tex#%u %dx%d base=%lx fmt=%04x type=%04x\n",
                    texlog, (int)S16(lv0, LV_W), (int)S16(lv0, LV_H),
                    base, U16(lv0, LV_FORMAT), U16(lv0, LV_TYPE));
            texlog++;
        }
    }
    if (t->dirty && tex_is_cube(t)) {
        /* carte de cube : les six faces, niveaux lus dans l'objet de GLEngine */
        int f;
        for (f = 0; f < 6; f++)
            for (l = 0; l < DT_LEVELS; l++) {
                const unsigned char *e = cube_level(dt, f, l);
                unsigned long w = U16(e, PL_W), h = U16(e, PL_H);
                unsigned int fmt = U16(e, PL_FORMAT), type = U16(e, PL_TYPE);
                unsigned long bpp = host_texel_bytes(fmt, type);
                unsigned long row = U16(e, PL_ROWPIX) * bpp, size;
                int dxt = dxt_format(fmt, type);
                if (!w || !h)
                    continue;
                if (dxt) {
                    if (!dxt_ok(base, fmt))
                        return no(NO_TEX_FORMAT, (unsigned long)fmt << 16, w);
                    row = 0;
                    size = dxt_bytes(fmt, w, h);
                } else {
                    size = row * (h - 1) + w * bpp;
                }
                if ((!bpp && !dxt) || !GLD_U32(e, PL_DATA) || !arena_alloc(size, &off))
                    return no(NO_TEX_SIZE, w, h);
                memcpy(G.q.win + off, (const void *)GLD_U32(e, PL_DATA), size);
                c = reserve(p, QGPU_LEN_TEX_IMAGE3);
                c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE3, QGPU_LEN_TEX_IMAGE3);
                c[1] = t->qtex; c[2] = QGPU_TT_CUBE_FACE(f); c[3] = l;
                c[4] = w; c[5] = h; c[6] = 1; c[7] = host_base(base); c[8] = fmt; c[9] = type;
                c[10] = G.q.base + off; c[11] = row; c[12] = 0;
                G.n_texuploads++;
            }
        t->dirty = 0;
        /* P15 : l'invité vient de redonner le contenu — il n'est plus
           « hôte seulement », la texture redevient évinçable. */
        t->host_only = 0;
        t->lv0_sig = tex_lv0_sig(t);
    }
    if (t->dirty) {
        for (l = 0; l < DT_LEVELS; l++) {
            unsigned char *lv = dt + DT_LEVEL0 + l * DT_LEVEL_SIZE;
            unsigned long w = S16(lv, LV_W), h = S16(lv, LV_H);
            if (w == 0 || h == 0)
                continue;
            if (S16(lv, LV_BORDER) || w > QGPU_MAX_TEX_DIM || h > QGPU_MAX_TEX_DIM)
                return no(NO_TEX_SIZE, w, h);
            if (G.v10) {
                /* v10 : les données telles quelles, pas de ligne compris ; le
                   cœur de l'hôte convertit (TEX_IMAGE3, 2D). */
                unsigned int fmt = U16(lv, LV_FORMAT), type = U16(lv, LV_TYPE);
                unsigned long bpp = host_texel_bytes(fmt, type);
                unsigned long row = (unsigned long)S16(lv, LV_ROWPIX) * bpp;
                unsigned long dep = 1, imgh = h, img, size;
                const unsigned char *d = (const unsigned char *)GLD_U32(lv, LV_DATA);
                if (upload_blank && (unsigned long)S16(lv, LV_ROWPIX) < w)
                    row = w * bpp;          /* niveau sans données : notre pas */
                if (t3)                 /* 3D : profondeur dans l'objet de GLEngine */
                    tex_level_depth(t, l, &dep, &imgh);
                img = row * imgh;
                size = img * (dep - 1) + row * (h - 1) + w * bpp;
                if (dxt_format(fmt, type) && !t3 && d && dxt_ok(base, fmt)) {
                    /* S3TC : les blocs serrés, tels quels */
                    row = 0;
                    size = dxt_bytes(fmt, w, h);
                } else if (!bpp || (!d && !upload_blank) ||
                           (!upload_blank && S16(lv, LV_ROWPIX) < (short)w) || !dep || imgh < h ||
                           !depth_pair_ok(host_base(base), fmt) || (fmt == 0x1902 && t3))
                    return no(NO_TEX_FORMAT, (fmt << 16) | type,
                              ((unsigned long)S16(lv, LV_ROWPIX) << 16) | w);
                /* H1 : GL_ALPHA part tel quel (plus de « blanc + A » ici) —
                   c'est le cœur qui décide de son décodage, et le backend GL
                   qui le prend en format interne GL_ALPHA. */
                if ((fmt == 0x1900 || fmt == 0x80E5 || fmt == 0x8049) && type == 0x1401) {
                    fmt = 0x1909;
                }
                if (!arena_alloc(size, &off))
                    return no(NO_TEX_SIZE, w, h);
                if (upload_blank) {
                    memset(G.q.win + off, 0, size);     /* COPY_TEX écrasera */
                } else if (sigsetjmp(sig_jmp, 0) == 0) {
                    sig_thr = pthread_self();   /* P3 (relecture du 24/09) */
                    sig_jmp_on = 1;
                    memcpy(G.q.win + off, d, size);
                    sig_jmp_on = 0;
                } else {
                    /* niveau illisible (pages non engagées : cible de copie
                       d'écran dont la copie hôte a été refusée) : NOIR plutôt
                       qu'un refus — refusé, le dessin partait chez Apple, qui
                       lit la même page (Prey, 24/09 : SIGSEGV dans a_quads →
                       rendu d'Apple). */
                    static unsigned long told;
                    sig_jmp_on = 0;
                    if (t->host_only) {
                        /* cible de copie d'écran : le contenu à jour est sur
                           l'hôte (COPY_TEX) — ne pas l'écraser de noir
                           (vitres de DOOM 3, 24/09) */
                        continue;       /* l'espace d'arène réservé est perdu, c'est rare */
                    }
                    memset(G.q.win + off, 0, size);
                    G.n_texblack++;     /* I9 (relecture du 24/09) : compté au bilan */
                    if (told < 3) {
                        told++;
                        gl_note("TEXTURE niveau %lu illisible (%lux%lu) : envoye noir\n", l, w, h);
                    }
                }
                c = reserve(p, QGPU_LEN_TEX_IMAGE3);
                c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE3, QGPU_LEN_TEX_IMAGE3);
                c[1] = t->qtex; c[2] = t3 ? QGPU_TT_3D : QGPU_TT_2D; c[3] = l;
                c[4] = w; c[5] = h; c[6] = dep; c[7] = host_base(base); c[8] = fmt; c[9] = type;
                c[10] = G.q.base + off; c[11] = row; c[12] = t3 ? img : 0;
                G.n_texuploads++;
                continue;
            }
            if (!arena_alloc(w * h * 4, &off))
                return no(NO_TEX_SIZE, w, h);
            if (!convert_level(lv, (unsigned long *)(G.q.win + off)))
                return no(NO_TEX_FORMAT, (U16(lv, LV_FORMAT) << 16) | U16(lv, LV_TYPE),
                          ((unsigned long)S16(lv, LV_ROWPIX) << 16) | w);
            c = reserve(p, QGPU_LEN_TEX_IMAGE);
            c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE, QGPU_LEN_TEX_IMAGE);
            c[1] = t->qtex; c[2] = l; c[3] = w; c[4] = h;
            /* H1 : plus d'override en 0x1908. GL_ALPHA n'arrive plus jusqu'ici
               (base_format_ok le refuse sans la v10, convert_level ne le sait
               plus convertir) : le format de base part tel quel. */
            c[5] = host_base(base);
            c[6] = G.q.base + off;
            G.n_texuploads++;
        }
        t->dirty = 0;
        /* P15 : l'invité vient de redonner le contenu — il n'est plus
           « hôte seulement », la texture redevient évinçable. */
        t->host_only = 0;
        t->lv0_sig = tex_lv0_sig(t);
    }
    prm[0] = U16(gp, TP_MIN);
    /* Filtre mipmap sans la chaîne : l'hôte refuserait la soumission
       (qgpu_texture_levels = 0) et UT2004 n'aurait plus que des quads blancs. */
    if (!tex_complete(dt) && prm[0] != 0x2600 && prm[0] != 0x2601)
        prm[0] = (U16(gp, TP_MAG) == 0x2600) ? 0x2600 : 0x2601;
    prm[1] = U16(gp, TP_MAG);
    prm[2] = U16(gp, TP_WRAP_S);
    prm[3] = U16(gp, TP_WRAP_T);
    prm[4] = U16(gp, TP_WRAP_R);
    prm[5] = GLD_U32(gp, TP_MIN_LOD);                 /* bits IEEE, tels quels */
    prm[6] = GLD_U32(gp, TP_MAX_LOD);
    prm[7] = U16(gp, TP_BASE_LEVEL);
    prm[8] = U16(gp, TP_MAX_LEVEL);
    prm[9] = tex_border(gp);
    prm[10] = fbits(lod_bias_clamp(GLD_F32(gp, TP_LOD_BIAS)));
    prm[11] = U16(gp, TP_COMPARE_MODE);
    prm[12] = U16(gp, TP_COMPARE_FUNC);
    prm[13] = U16(gp, TP_DEPTH_MODE);
    for (k = 0; k < 14; k++) {
        /* wrap r : 3D seulement ; LOD et niveaux : v10 seulement (avant, ils
           sont forcés aux valeurs initiales par tex_lod_ok) ; bordure : avec
           G.tex13 (sinon, seule la valeur initiale passe, cf. tex_params_ok) ;
           biais et profondeur : avec G.tex14 (sinon le biais est sans effet,
           GL_MAX_TEXTURE_LOD_BIAS valant 0, et la profondeur est refusée) */
        if ((k == 4 && !t3) || (k >= 5 && !G.v10) || (k == 9 && !G.tex13) ||
            (k >= 10 && !G.tex14))
            continue;
        if (t->prm_valid && t->prm[k] == prm[k])
            continue;
        c = reserve(p, QGPU_LEN_TEX_PARAM);
        c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_PARAM, QGPU_LEN_TEX_PARAM);
        c[1] = t->qtex; c[2] = keys[k]; c[3] = prm[k];
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
        /* GL_TEXTUREn (crossbar, OpenGL 1.4) : sa propre unité est GL_TEXTURE ;
           une autre, jusqu'à la 4e, passe au device v12 (QGPU_CS_TEXTURE0 + n) ;
           au-delà, ou avant la v12, le rendu d'Apple reprend la main. */
        if (e >= 0x84C0 && e < 0x84C0 + GL_MAX_TEXUNITS) {
            int n = (int)(e - 0x84C0);
            if (n == unit)
                return QGPU_CS_TEXTURE;
            if (G.xbar && n < 4)                /* champ source de 3 bits : unités 0..3 */
                return QGPU_CS_TEXTURE0 + n;
        }
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

/* Sonde (relevé seulement, POMPPC_GL_T3DDUMP=1 avec POMPPC_GLTRACE) : quand
   une unité a une cible cube, 3D ou rectangle active, vide la table des
   textures liées de l'unité, son bloc d'état, et pour chaque emplacement non
   nul l'objet texture du GLDriver, ses paramètres et le début des données des
   deux premiers niveaux. C'est ce qui établit, sans rien deviner, où GLEngine
   range une texture 3D et sa profondeur (docs/re/textures-3d.md). */
static void target_probe(PCtx *p, int u, unsigned long mask, unsigned long units)
{
    static int shots;
    char tag[32];
    int k, l;
    if (!getenv("POMPPC_GL_T3DDUMP") || shots++ >= 2 || !units)
        return;
    pomppc_log("SONDE cible : unité %d masque %02lx table %08lx\n", u, mask, units);
    for (k = 0; k < 5; k++)
        pomppc_log("  emplacement %d : %08lx\n", k, GLD_U32(units, u * 0x14 + k * 4));
    pomppc_dump("cible-unite", gls(p) + GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE, GS_TEXUNIT_SIZE);
    for (k = 0; k < 5; k++) {
        unsigned char *dt = (unsigned char *)GLD_U32(units, u * 0x14 + k * 4);
        if (!dt)
            continue;
        sprintf(tag, "cible-dt%d", k);
        pomppc_dump(tag, dt, 0x600);
        if (GLD_U32(dt, DT_PARAMS)) {
            sprintf(tag, "cible-prm%d", k);
            pomppc_dump(tag, (void *)GLD_U32(dt, DT_PARAMS), 0x100);
        }
        for (l = 0; l < 2; l++) {
            unsigned char *lv = dt + DT_LEVEL0 + l * DT_LEVEL_SIZE;
            if (!GLD_U32(lv, LV_DATA))
                continue;
            sprintf(tag, "cible-dt%d-niv%d", k, l);
            pomppc_dump(tag, (void *)GLD_U32(lv, LV_DATA), 0x100);
        }
    }
}

/* Objet texture du GLDriver lié à l'unité u pour la cible active, ou 0.
 * `*mask` reçoit les cibles activées (bits TU_ENABLE). */
/* Emplacement de CTX_TEXUNITS de la cible EFFECTIVE d'une unité, selon la
 * priorité d'OpenGL (cube > 3D > rectangle > 2D > 1D) : le bit 1<<k de
 * TU_ENABLE correspond à l'emplacement k (relevé, docs/re/textures-3d.md).
 * -1 : la cible effective n'est pas tenue (cube, rectangle ; 3D sans l'hôte). */
static int unit_slot(unsigned long m)
{
    if (m & 1)
        return G.cube ? 0 : -1;                     /* carte de cube (v10) */
    if (m & 2)
        return G.tex3d ? 1 : -1;                    /* 3D (v10) */
    if (m & 4)
        return -1;                                  /* rectangle */
    return (m & 8) ? 3 : 4;                         /* 2D, 1D */
}

/* v16 : masque de cibles de l'unité u — TU_ENABLE, ou, sous un programme de
 * fragments actif, la cible que le TEXTE échantillonne (texture[u], 2D…) :
 * Direct3D n'allume jamais GL_TEXTURE_2D, et sous programme glEnable ne
 * compte pas. Une unité que le texte ne lit pas est coupée. */
static unsigned long unit_mask(PCtx *p, int u)
{
    if (G.prog && p->fp_on && p->fp_rec && !p->fp_rec->refused)
        return u < QGPU_MAX_UNITS ? p->fp_rec->fp_unit[u] : 0;
    return GLD_U32(gls(p) + GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE, TU_ENABLE) & 0x1f;
}

static void *unit_drvtex(PCtx *p, int u, unsigned long *mask)
{
    unsigned long m = unit_mask(p, u);
    unsigned long units = GLD_U32(p->ctx, CTX_TEXUNITS);
    *mask = m;
    if (m & 0x7)
        target_probe(p, u, m, units);
    if (!m || unit_slot(m) < 0 || !units)
        return 0;
    return (void *)GLD_U32(units, u * 0x14 + unit_slot(m) * 4);
}

/* Texturage effectif. CTX_TEXTURING est calculé par le rendu d'Apple, qui ne
 * connaît pas la 3D : une unité dont la seule texture est 3D l'y laisse à 0
 * (vu en vrai, sonde t3dprobe). On le complète donc nous-mêmes. */
static int texturing_on(PCtx *p)
{
    unsigned long units = GLD_U32(p->ctx, CTX_TEXUNITS);
    int u;
    for (u = 0; u < GL_MAX_TEXUNITS; u++) {
        unsigned long m = unit_mask(p, u);
        void *dt;
        if (!m)
            continue;
        /* cible que nous ne tenons pas : laisser texture_ok / geom_texture_ok
           la refuser (repli sur Apple), jamais la dessiner sans texture */
        if (unit_slot(m) < 0 || !units)
            return 1;
        dt = (void *)GLD_U32(units, u * 0x14 + unit_slot(m) * 4);
        if (dt && (tex_complete(dt) || tex_base_ok(dt)))
            return 1;
    }
    return 0;
}

/* Unité u : texture et environnement ; 0 = hors domaine (logiciel). */
static int texture_unit_ok(PCtx *p, int u, TexUnit *tu)
{
    unsigned char *g = gls(p);
    unsigned char *us = g + GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE;
    unsigned long mask = unit_mask(p, u);
    unsigned long units = GLD_U32(p->ctx, CTX_TEXUNITS);
    unsigned long env = U16(us, TU_ENV_MODE);
    /* v16 : sous un programme de fragments, l'environnement est ignoré par
       l'hôte — on ne le valide pas, il peut porter n'importe quoi */
    int fp = G.prog && p->fp_on && p->fp_rec && !p->fp_rec->refused;
    const float *ec;
    void *dt;

    tu->t = 0;
    if (!mask)
        return 1;                                   /* unité coupée */
    if (unit_slot(mask) < 0 || !units) {             /* cube, rectangle (3D sans v10) */
        target_probe(p, u, mask, units);
        return no(NO_TEX_TARGET, mask, units);
    }
    if (fp)
        env = 0x2100;                               /* ignoré par l'hôte : MODULATE */
    if (env != 0x2100 && env != 0x2101 && env != 0x0BE2 && env != 0x1E01 &&
        env != 0x0104 && env != 0x8570)
        return no(NO_TEX_ENV, env, u);
    tu->combine = QGPU_COMBINE_DEFAULT;
    tu->combine_src = QGPU_COMBINE_SRC_DEFAULT;
    if (env == 0x8570 && !combine_ok(us, u, tu))
        return 0;
    dt = (void *)GLD_U32(units, u * 0x14 + unit_slot(mask) * 4);
    tu->t = dt ? intern_tex(dt) : 0;
    if (!tu->t)
        return no(NO_TEX_UNKNOWN, (unsigned long)dt, mask);
    /* Texture sans image (jamais définie) : OpenGL la dit incomplète et coupe
       le texturage de CETTE unité, sans toucher aux autres. Marble Blast
       laisse ainsi des unités actives sans texture. S'il y a un niveau de
       base, on s'en sert (filtre mipmap rabattu) au lieu de dessiner blanc. */
    if (!tex_complete(tu->t->drvtex)) {
        if (!tex_base_ok(tu->t->drvtex)) {
            tu->t = 0;
            G.n_tex_incomplete++;
            return 1;
        }
        G.n_tex_incomplete++;
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
    int i;

    for (i = 0; i < QGPU_MAX_UNITS; i++)
        ti->u[i].t = 0;
    prog_state(p);                                  /* v16 : unit_mask en dépend */
    if (!texturing_on(p))
        return 1;                                   /* pas de texture : dessin simple */
    for (i = G.units; i < GL_MAX_TEXUNITS; i++)
        if (unit_mask(p, i))
            return no(NO_TEX_UNITS, i, 0);          /* au-delà du device : logiciel */
    for (i = 0; i < QGPU_MAX_UNITS; i++)
        if (!texture_unit_ok(p, i, &ti->u[i]))
            return 0;
    cube_probe(p, ti, "texture_ok");
    draw_probe(p, ti);
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
    int tm = stats_timing();            /* F10 : plus de gettimeofday par lot */
    double t0 = tm ? now_s() : 0.0;

    if (color && p->color == SW_NEWER) {
        unsigned char *src = sw_color(p);
        unsigned long bpp = color_bpp(p);
        unsigned long row = sw_rowbytes(p);
        /* arena_fits : une image trop grande pour la moitié ne passera jamais
           — inutile de réessayer (et de vider le flux) à chaque dessin. */
        int can = src && (bpp == 4 || G.v15) && arena_fits(w * h * bpp);
        int done = !can;                /* rien à téléverser : c'est « fait » */
        if (can && arena_alloc(w * h * bpp, &off)) {
            for (y = 0; y < h; y++)
                memcpy(G.q.win + off + y * w * bpp, src + y * row, w * bpp);
            if (bpp == 2) {
                c = reserve(p, QGPU_LEN_SURF_XFER_PF);
                c[0] = QGPU_CMD_HDR(QGPU_OP_SURF_UPLOAD, QGPU_LEN_SURF_XFER_PF);
                c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 2;
                c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
                c[8] = QGPU_PF_RGB1555;
            } else {
                c = reserve(p, QGPU_LEN_SURF_XFER);
                c[0] = QGPU_CMD_HDR(QGPU_OP_SURF_UPLOAD, QGPU_LEN_SURF_XFER);
                c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
                c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
            }
            G.n_uploads++;
            done = 1;
        }
        /* P16 : SYNCED voulait dire « les deux copies sont identiques ». Le
           poser alors qu'arena_alloc a refusé (w·h·4 > 6 Mio : 1600×1200)
           mentait, et l'hôte dessinait sur un contenu périmé — image figée ou
           aléatoire en haute résolution. On laisse SW_NEWER : le prochain
           dessin réessaiera. */
        if (done)
            p->color = SYNCED;
    }
    if (depth && p->depth == SW_NEWER) {
        unsigned char *src = sw_depth(p);
        unsigned long dbpp = depth_bpp(p);
        unsigned long drow = depth_rowbytes(p);
        int done = 0;
        if (dbpp == 2) {
            if (!G.v15 || !src || !arena_fits(w * h * 2))
                done = 1;               /* jamais téléversable : ne pas boucler */
            else if (arena_alloc(w * h * 2, &off)) {
                for (y = 0; y < h; y++)
                    memcpy(G.q.win + off + y * w * 2, src + y * drow, w * 2);
                c = reserve(p, QGPU_LEN_SURF_XFER_PF);
                c[0] = QGPU_CMD_HDR(QGPU_OP_DEPTH_UPLOAD, QGPU_LEN_SURF_XFER_PF);
                c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 2;
                c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
                c[8] = QGPU_DF_UNORM16;
                G.n_uploads++;
                done = 1;
            }
        } else {
            float inv = 1.0f / GLD_F32(p->ctx, CTX_DEPTH_SCALE);
            if (!src || !arena_fits(w * h * 4))
                done = 1;               /* pas de tampon invité : rien à porter */
            else if (arena_alloc(w * h * 4, &off)) {
                for (y = 0; y < h; y++) {
                    unsigned long *s = (unsigned long *)(src + y * drow);
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
                done = 1;
            }
        }
        /* Le stencil de l'invité vit dans les 8 bits bas des mêmes mots. On ne
           le téléverse QUE si le contexte s'en sert vraiment : sur une surface
           hôte combinée, QGPU_OP_STENCIL_UPLOAD abîme la PROFONDEUR déjà posée
           (vu en vrai, scène « mixte » avec un format de pixel à stencil : tout
           ce qui suivait le premier repli logiciel disparaissait ; le seul fait
           de sauter ce téléversement rend l'image exacte). Beaucoup
           d'applications — GLUT, Marble Blast — demandent un stencil sans
           jamais s'en servir : elles ne paient plus ni le bogue ni le transfert. */
        if (src && p->stencil && p->sten_used && dbpp == 4 && arena_fits(w * h * 4)) {
            if (arena_alloc(w * h * 4, &off)) {
                for (y = 0; y < h; y++) {
                    unsigned long *s = (unsigned long *)(src + y * drow);
                    unsigned long *d = (unsigned long *)(G.q.win + off + y * w * 4);
                    for (x = 0; x < w; x++)
                        d[x] = s[x] & 0xFF;
                }
                c = reserve(p, QGPU_LEN_SURF_XFER);
                c[0] = QGPU_CMD_HDR(QGPU_OP_STENCIL_UPLOAD, QGPU_LEN_SURF_XFER);
                c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
                c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
                G.n_uploads++;
            } else {
                done = 0;               /* P16 : stencil non porté = pas synchronisé */
            }
        }
        /* P16 : voir la couleur — SYNCED ment dès qu'un transfert a été refusé. */
        if (done)
            p->depth = SYNCED;
    }
    if (tm)
        G.t_upload += now_s() - t0;
}

/* P16 : rend 1 si la relecture est bien en file, 0 si elle n'a PAS pu l'être
 * (arène pleine, format que l'hôte ne sait pas rendre). L'appelant ne doit
 * alors surtout pas se déclarer synchronisé — ni se dire qu'il a présenté. */
static int queue_readback_to(PCtx *p, int depth, unsigned char *dst, unsigned long rowbytes)
{
    unsigned long off, *c, bpp, fmt, len, op;
    unsigned long w = p->sw, h = p->sh;
    if (depth == 2) {
        bpp = 4; fmt = 0; len = QGPU_LEN_SURF_XFER; op = QGPU_OP_STENCIL_READBACK;
    } else if (depth) {
        bpp = depth_bpp(p);
        fmt = (bpp == 2) ? QGPU_DF_UNORM16 : QGPU_DF_FLOAT32;
        len = (bpp == 2) ? QGPU_LEN_SURF_XFER_PF : QGPU_LEN_SURF_XFER;
        op = QGPU_OP_DEPTH_READBACK;
        if (bpp == 2 && !G.v15)
            return 0;
    } else {
        bpp = color_bpp(p);
        fmt = (bpp == 2) ? QGPU_PF_RGB1555 : QGPU_PF_XRGB8888;
        len = (bpp == 2) ? QGPU_LEN_SURF_XFER_PF : QGPU_LEN_SURF_XFER;
        op = QGPU_OP_SURF_READBACK;
        if (bpp == 2 && !G.v15)
            return 0;
    }
    if (!arena_alloc(w * h * bpp, &off))
        return 0;
    c = reserve(p, len);
    c[0] = QGPU_CMD_HDR(op, len);
    c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * bpp;
    c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
    if (len == QGPU_LEN_SURF_XFER_PF)
        c[8] = fmt;
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
        po->pixbytes = bpp;
        po->scale = GLD_F32(p->ctx, CTX_DEPTH_SCALE);
        hf->npost++;
    }
    G.n_readbacks++;
    return 1;
}

static void queue_present(PCtx *p, unsigned long dest_off, unsigned long stride,
                          unsigned long fmt)
{
    unsigned long *c;
    unsigned long w = p->sw, h = p->sh;
    c = reserve(p, QGPU_LEN_SURF_PRESENT);
    c[0] = QGPU_CMD_HDR(QGPU_OP_SURF_PRESENT, QGPU_LEN_SURF_PRESENT);
    c[1] = p->surf;
    c[2] = dest_off;
    c[3] = stride;
    c[4] = 0;
    c[5] = 0;
    c[6] = w;
    c[7] = h;
    c[8] = fmt;
    G.n_present++;
}

static int queue_readback(PCtx *p, int depth, unsigned char *dst)
{
    return queue_readback_to(p, depth, dst,
                             depth ? depth_rowbytes(p) : sw_rowbytes(p));
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
        /* Ne pas déverser du xRGB 32 bits dans un tampon invité 16 bits
           tant que l'hôte ne sait pas packer (v15). */
        /* P16 : SYNCED seulement si la relecture est vraiment partie — sauf
           quand l'arène ne pourra JAMAIS la porter (limite connue de la
           moitié), auquel cas réessayer à chaque appel coûterait un vidage
           pour rien. */
        if (dst && (color_bpp(p) == 4 || G.v15)) {
            if (queue_readback(p, 0, dst)) {
                any = 1;
                p->color = SYNCED;
            } else if (!arena_fits(p->sw * p->sh * color_bpp(p))) {
                p->color = SYNCED;
            }
        }
    }
    if (want_depth && p->depth == HOST_NEWER) {
        unsigned char *dst = sw_depth(p);
        if (dst && (depth_bpp(p) == 4 || G.v15)) {
            if (queue_readback(p, 1, dst)) {
                if (p->stencil && p->sten_used)
                    queue_readback(p, 2, dst);
                any = 1;
                p->depth = SYNCED;
            } else if (!arena_fits(p->sw * p->sh * depth_bpp(p))) {
                p->depth = SYNCED;
            }
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
    /* gldFlush / gldFinish : en plein écran l'image part à l'échange, pas
       dans le tampon invité. Relire à chaque vidage (Colin McRae) tuait le
       débit, et le scanout 16 bits n'est pas le drawable 32 bits. */
    if (p && !want_depth && p->fullscreen_buf && p->color == HOST_NEWER) {
        if (G.ncmd)
            flush();
        pthread_mutex_unlock(&G.mu);
        return;
    }
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
    /* 16 bits : Warcraft III et Colin McRae demandent « milliers de couleurs ».
       En plein écran le drawable mémoire reste xRGB 32 bits, convertis à la
       présentation. En fenêtre, v15 téléverse/relit le 1555 tel quel. */
    {
        unsigned long cbits = GLD_U32(p->ctx, CTX_COLOR_BITS);
        if (!g || !sw_color(p) ||
            (cbits != 32 && cbits != 16) ||
            GLD_U32(p->ctx, CTX_ROWPIX) < GLD_U32(p->ctx, CTX_WIDTH))
            return no(NO_BUFFER, cbits, GLD_U32(p->ctx, CTX_ROWPIX));
    }
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
    /* Profondeur 16 : l'hôte a toujours un tampon float 32 bits. v15 échange
       UNORM16 sans conversion G4 ; avant v15 on refuse (repli Apple). */
    {
        unsigned long dbits = GLD_U32(p->ctx, CTX_DEPTH_BITS);
        if (GLD_U8(g, GS_DEPTH_TEST) &&
            ((dbits != 32 && dbits != 16) || U16(g, GS_DEPTH_FUNC) < 0x200 ||
             U16(g, GS_DEPTH_FUNC) > 0x207))
            return no(NO_DEPTH, dbits, U16(g, GS_DEPTH_FUNC));
        if (GLD_U8(g, GS_DEPTH_TEST) && dbits == 16 && !G.v15)
            return no(NO_DEPTH, dbits, 0);
    }
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
/* Borne haute de la taille de point. GLEngine part de POINT_SIZE_MAX = 1 —
 * la valeur de la table d'OpenGL 1.4, alors qu'ARB_point_parameters et tous
 * les pilotes partent de la plus grande taille (le rendu d'Apple, lui, ignore
 * ces paramètres). Le couple initial intact (MIN 0, MAX 1) vaut donc « pas de
 * borne » : 64, le maximum de l'hôte. */
static float point_max(const unsigned char *g)
{
    float mn = GLD_F32(g, GS_POINT_SIZE_MIN), mx = GLD_F32(g, GS_POINT_SIZE_MAX);
    if ((mx == 1.0f && mn == 0.0f) || !(mx <= 64.0f))
        mx = 64.0f;
    return mx > 0.0f ? mx : 64.0f;
}

/* Paramètres de point que l'hôte v10 accepte (flottants finis ≥ 0) ? */
static int point_params_ok(const unsigned char *g)
{
    const float *att = (const float *)(g + GS_POINT_ATT);
    float mn = GLD_F32(g, GS_POINT_SIZE_MIN), fd = GLD_F32(g, GS_POINT_FADE);
    int i;
    for (i = 0; i < 3; i++)
        if (!(att[i] >= 0.0f && att[i] < 1e9f))
            return 0;
    return mn >= 0.0f && mn <= 64.0f && fd >= 0.0f && fd < 1e9f;
}

static int point_att_default(const unsigned char *g)
{
    const float *att = (const float *)(g + GS_POINT_ATT);
    return att[0] == 1.0f && att[1] == 0.0f && att[2] == 0.0f;
}

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
            v[QGPU_SK_COMBINE(u)] = p->st_valid ? p->st[QGPU_SK_COMBINE(u)]
                                                : QGPU_COMBINE_DEFAULT;
            v[QGPU_SK_COMBINE_SRC(u)] = p->st_valid ? p->st[QGPU_SK_COMBINE_SRC(u)]
                                                    : QGPU_COMBINE_SRC_DEFAULT;
            if (tu && tu->t) {
                v[kb + QGPU_SK_U_BIND] = tu->t->qtex;
                v[kb + QGPU_SK_U_ENV_MODE] = tu->env_mode;
                v[kb + QGPU_SK_U_ENV_COLOR] = tu->env_color;
                v[QGPU_SK_COMBINE(u)] = tu->combine;
                v[QGPU_SK_COMBINE_SRC(u)] = tu->combine_src;
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
        /* 1.4 : sans atténuation, la taille est bornée par MIN/MAX — ici, pour
           les deux chemins ; avec, c'est l'hôte qui borne la taille DÉRIVÉE */
        if (G.tex14 && point_att_default(g) && point_params_ok(g)) {
            float mn = GLD_F32(g, GS_POINT_SIZE_MIN), mx = point_max(g);
            if (ps > mx) ps = mx;
            if (ps < mn) ps = mn;
        }
        if (!(ps >= 1.0f)) ps = 1.0f;
        if (ps > 64.0f) ps = 64.0f;
        lw = (float)(int)(lw + 0.5f);
        ps = (float)(int)(ps + 0.5f);
        v[QGPU_SK_LINE_WIDTH] = *(unsigned long *)&lw;
        v[QGPU_SK_POINT_SIZE] = *(unsigned long *)&ps;
    }
    v[QGPU_SK_POLY_OFFSET] = GLD_U8(g, GS_POLY_OFFSET) != 0;
    if (v[QGPU_SK_POLY_OFFSET]) {
        /* Mineur §2 : le facteur et les unités partaient BRUTS. Un NaN ou un
           ±1e6 (scène « offset ») fait refuser toute la soumission par le cœur
           (|v| ≥ 1e9 interdit), et avec H4 tout le reste du flux est perdu. */
        v[QGPU_SK_POLY_FACTOR] = safe_offset(GLD_U32(g, GS_POLY_FACTOR));
        v[QGPU_SK_POLY_UNITS] = safe_offset(GLD_U32(g, GS_POLY_UNITS));
    } else {
        v[QGPU_SK_POLY_FACTOR] = p->st_valid ? p->st[QGPU_SK_POLY_FACTOR] : 0;
        v[QGPU_SK_POLY_UNITS] = p->st_valid ? p->st[QGPU_SK_POLY_UNITS] : 0;
    }
    if (G.v7)
        compute_geom_state(p, v);
    if (G.v8)
        compute_v8_state(p, v, raw);
    if (G.tex14) {
        int u;
        for (u = 0; u < QGPU_MAX_UNITS; u++)
            v[QGPU_SK_TEX_LOD_BIAS(u)] = fbits(lod_bias_clamp(
                GLD_F32(g, GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE + TU_LOD_BIAS)));
        /* Explicite, jamais QGPU_CSUM_FORMAT : la couleur secondaire du sommet
           ne part que si la somme est allumée (geom_format), mais la valeur
           courante doit s'ajouter aussi. Éclairage allumé, l'hôte suit la règle
           d'OpenGL quelle que soit la clé. */
        v[QGPU_SK_COLOR_SUM] = GLD_U8(g, GS_COLOR_SUM) ? QGPU_CSUM_ON : QGPU_CSUM_OFF;
        /* Paramètres de point (1.4), pour DRAW_RAW seulement. Hors domaine,
           geom_ok refuse l'atténuation : on envoie alors les valeurs neutres. */
        if (point_params_ok(g)) {
            const float *att = (const float *)(g + GS_POINT_ATT);
            v[QGPU_SK_POINT_SIZE_MIN] = GLD_U32(g, GS_POINT_SIZE_MIN);
            v[QGPU_SK_POINT_SIZE_MAX] = fbits(point_max(g));
            v[QGPU_SK_POINT_FADE] = GLD_U32(g, GS_POINT_FADE);
            v[QGPU_SK_POINT_ATT_CONST] = fbits(att[0]);
            v[QGPU_SK_POINT_ATT_LINEAR] = fbits(att[1]);
            v[QGPU_SK_POINT_ATT_QUAD] = fbits(att[2]);
        } else {
            v[QGPU_SK_POINT_SIZE_MIN] = fbits(0.0f);
            v[QGPU_SK_POINT_SIZE_MAX] = fbits(64.0f);
            v[QGPU_SK_POINT_FADE] = fbits(1.0f);
            v[QGPU_SK_POINT_ATT_CONST] = fbits(1.0f);
            v[QGPU_SK_POINT_ATT_LINEAR] = v[QGPU_SK_POINT_ATT_QUAD] = fbits(0.0f);
        }
    }
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
    struct { int lo, hi; } rg[5];       /* géométrie, v8, 1.4, programmes, unités 4..7 */
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
    /* v10, OpenGL 1.4 : biais d'unité (les deux chemins), GL_COLOR_SUM et
       paramètres de point (brut) */
    if (G.tex14) { rg[nr].lo = QGPU_SK_TEX_LOD_BIAS0; rg[nr].hi = QGPU_SK_POINT_ATT_QUAD + 1; nr++; }
    /* v16 : activation des programmes ARB (brut seulement ; l'hôte les ignore
       sur les anciens opcodes, et poser 1 sans la capacité serait refusé) */
    if (G.prog) { rg[nr].lo = QGPU_SK_VERTEX_PROGRAM; rg[nr].hi = QGPU_SK_FRAGMENT_PROGRAM + 1; nr++; }
    /* v17 : les unités 4..7 (texturage, GL_COMBINE, biais de LOD), les deux
       chemins ; un device plus ancien refuserait ces clés */
    if (G.units > 4) { rg[nr].lo = QGPU_SK_TEXTURE4; rg[nr].hi = QGPU_SK_TEX_LOD_BIAS4 + 4; nr++; }
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

/* ── Génériques à taille déclarée (QGPU_CAP_GEN_SIZES) ──
 * La clé QGPU_SK_GEN_SIZES est un état du contexte qui s'applique à TOUS les
 * DRAW_RAW / DRAW_RAW_BUF qui suivent. Le chemin tableaux déclare la taille
 * des tableaux (va_gen_sizes) ; Begin/End (descripteur de GLEngine) écrit
 * toujours 4 flottants par générique. Seuls les champs des génériques
 * PRÉSENTS dans le format comptent pour le device : on ne renvoie la clé que
 * si ceux-là diffèrent de ce qu'il a. Sans G.gensizes, la clé n'est jamais
 * posée et reste à 0 sur le device. */
static unsigned long gs_fields(unsigned long fmt)
{
    unsigned long m = 0;
    int k;
    for (k = 0; k < QGPU_VF_GEN_MAX; k++)
        if (fmt & QGPU_VF_GEN(k))
            m |= QGPU_GS_FIELD(k);
    return m;
}

/* 1 si le device ne lirait pas les génériques de `fmt` à la taille `gs`
   (gs ne porte que des champs de gs_fields(fmt)). */
static int gs_stale(PCtx *p, unsigned long fmt, unsigned long gs)
{
    if (!G.gensizes || !(fmt & QGPU_VF_GEN_MASK))
        return 0;
    return !p->c_gs_valid || (p->c_gs & gs_fields(fmt)) != gs;
}

/* Écrit SET_STATE(QGPU_SK_GEN_SIZES, gs) dans les 3 mots `c` (place déjà
   prise, contexte déjà lié). */
static void gs_put(unsigned long *c, PCtx *p, unsigned long gs)
{
    c[0] = QGPU_CMD_HDR(QGPU_OP_SET_STATE, QGPU_LEN_SET_STATE);
    c[1] = QGPU_SK_GEN_SIZES;
    c[2] = gs;
    p->c_gs = gs;
    p->c_gs_valid = 1;
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

static void *geom_alloc_vb(void *ctx, unsigned long id, unsigned long *n);
static void  geom_complete_vb(void *ctx, void *buf, unsigned long used);
static void  geom_free_vb(void *ctx, void *buf);

/* ── 2.1 : objets tampon à la GeForce3 (docs/re/tableaux-de-sommets.md §3) ──
 * Poignée non nulle + FlushBuffer qui acquitte les bits 0-1. BufferSubData
 * reste à 0 : GLEngine fait le memcpy puis FlushBuffer. En v14 le paquet
 * DRAW_RAW packed est copié une fois dans un tampon hôte ; les dessins
 * suivants du même intervalle émettent DRAW_RAW_BUF sans retraverser BAR0. */
struct PBuf {
    PBuf           *next;
    void          **data;               /* vbo+0x30 */
    unsigned long  *flags;              /* drapeaux par rendu, vbo+0x48+4i */
    unsigned long   last_len;
    long            qid;                /* identifiant hôte, -1 tant qu'inutile */
    unsigned long   qsize;
    unsigned long   pack_fmt, pack_vmin, pack_nverts;
    unsigned long   pack_gs;            /* QGPU_SK_GEN_SIZES de l'emballage : le
                                           pas des sommets du tampon en dépend */
    unsigned long   pack_key;           /* P13 : récapitulatif de TOUT ce que
                                           va_pack_vertex a lu (pointeurs, pas,
                                           types, et valeurs COURANTES des
                                           attributs inactifs) */
    int             dirty;
    /* ── v18 : miroir BRUT pour DRAW_NATIVE ──
       Tranche [rp_off, rp_off + rp_cap) de la réserve rp (-1 : aucune) ; elle
       contient la copie octet pour octet de la copie cliente du VBO (vbo+0x30,
       longueur logique vbo+0x38), aux plages sales près. */
    PBuf           *hnext;              /* chaîne de G.buf_hash */
    int             rp;
    unsigned long   rp_off, rp_cap;
    int             rp_failed;          /* plus de place : ne réessayer qu'après */
    unsigned long   rp_fail_gen;        /*   une libération (G.rp_gen) */
    unsigned long   raw_base;           /* vbo+0x30 au dernier ajustement */
    int             rd_n;               /* plages SALES, triées, disjointes, */
    unsigned long   rd_lo[RD_MAX], rd_hi[RD_MAX];   /* non contiguës ; hi borné
                                           à la taille logique à l'usage */
};

/* L'objet tampon de GLEngine (docs/re/tableaux-de-sommets.md §3.2) : */
#define VBO_DATA   0x30                 /* ptr : copie cliente (vm_allocate) */
#define VBO_SIZE   0x38                 /* u32 : taille logique (GL_BUFFER_SIZE) */
#define VA_EBO     0x35c                /* V : GL_ELEMENT_ARRAY_BUFFER lié (A+0x34c) */

static unsigned long buf_hash_of(const void *key)
{
    unsigned long k = (unsigned long)key;
    return ((k >> 4) ^ (k >> 14)) & (BUF_HASH - 1);
}

/* v18 : table de hachage — buf_from_vbo tourne par attribut et par dessin, et
   idTech4 a des centaines de VBO (la liste coûtait un parcours chaque fois). */
static PBuf *buf_from_vbo(unsigned long vbo)
{
    PBuf *b;
    void **key;
    if (!vbo)
        return 0;
    key = (void **)(vbo + VBO_DATA);
    for (b = G.buf_hash[buf_hash_of(key)]; b; b = b->hnext)
        if (b->data == key)
            return b;
    return 0;
}

/* ── v18 : plages sales d'un miroir brut ── */
static void rd_all(PBuf *b)
{
    b->rd_n = 1;
    b->rd_lo[0] = 0;
    b->rd_hi[0] = 0xFFFFFFFFUL;         /* borné à la taille logique à l'usage */
}

static void rd_remove(PBuf *b, int i)
{
    for (; i + 1 < b->rd_n; i++) {
        b->rd_lo[i] = b->rd_lo[i + 1];
        b->rd_hi[i] = b->rd_hi[i + 1];
    }
    b->rd_n--;
}

/* Plus de place : les deux plages voisines les plus proches fusionnent (on
   recopiera l'écart entre elles — de trop, jamais de moins). */
static void rd_merge_closest(PBuf *b)
{
    int j, best = -1;
    unsigned long gap, bg = 0;
    for (j = 0; j + 1 < b->rd_n; j++) {
        gap = b->rd_lo[j + 1] - b->rd_hi[j];
        if (best < 0 || gap < bg) {
            best = j;
            bg = gap;
        }
    }
    if (best >= 0) {
        b->rd_hi[best] = b->rd_hi[best + 1];
        rd_remove(b, best + 1);
    }
}

/* Ajoute [lo, hi) : fusion avec les voisines qui la touchent. */
static void rd_add(PBuf *b, unsigned long lo, unsigned long hi)
{
    int i, j;
    if (lo >= hi)
        return;
    if (b->rd_n == RD_MAX)
        rd_merge_closest(b);
    for (i = 0; i < b->rd_n && b->rd_lo[i] < lo; i++)
        ;
    for (j = b->rd_n; j > i; j--) {
        b->rd_lo[j] = b->rd_lo[j - 1];
        b->rd_hi[j] = b->rd_hi[j - 1];
    }
    b->rd_lo[i] = lo;
    b->rd_hi[i] = hi;
    b->rd_n++;
    /* recoudre : chevauchements et contacts */
    for (j = 0; j + 1 < b->rd_n; ) {
        if (b->rd_lo[j + 1] <= b->rd_hi[j]) {
            if (b->rd_hi[j + 1] > b->rd_hi[j])
                b->rd_hi[j] = b->rd_hi[j + 1];
            rd_remove(b, j + 1);
        } else {
            j++;
        }
    }
}

static void buf_raw_invalidate_all(void)
{
    PBuf *b;
    for (b = G.bufs; b; b = b->next)
        if (b->rp >= 0)
            rd_all(b);
}

/* ── v18 : réserves (tampons hôte de RAWPOOL_BYTES découpés en tranches) ── */
static void rp_release(int pool, unsigned long off, unsigned long len)
{
    RawExt **pp, *e, *n;
    if (pool < 0 || pool >= G.rp_n || !len)
        return;
    for (pp = &G.rp_free[pool]; *pp && (*pp)->off < off; pp = &(*pp)->next)
        ;
    n = (RawExt *)malloc(sizeof(*n));
    if (!n)
        return;                         /* tranche perdue : seulement de la place */
    n->off = off;
    n->len = len;
    n->next = *pp;
    *pp = n;
    /* fusion avec la suivante, puis avec la précédente */
    if (n->next && n->off + n->len == n->next->off) {
        e = n->next;
        n->len += e->len;
        n->next = e->next;
        free(e);
    }
    for (e = G.rp_free[pool]; e && e->next != n; e = e->next)
        ;
    if (e && e->off + e->len == n->off) {
        e->len += n->len;
        e->next = n->next;
        free(n);
    }
    G.rp_gen++;
}

static int rp_take(int pool, unsigned long len, unsigned long *off)
{
    RawExt **pp, *e;
    for (pp = &G.rp_free[pool]; (e = *pp); pp = &e->next) {
        if (e->len < len)
            continue;
        *off = e->off;
        e->off += len;
        e->len -= len;
        if (!e->len) {
            *pp = e->next;
            free(e);
        }
        return 1;
    }
    return 0;
}

/* Une nouvelle réserve : BUF_CREATE dans une soumission SYNCHRONE à part
   (comme prog_ensure) — un BUF_CREATE perdu dans un lot refusé laisserait des
   BUF_SUBDATA vers un identifiant inconnu (BAD_ARG à chaque lot) ; confirmée,
   la réserve existe pour de bon. */
static long submit_probe(void);
static long buf_alloc_id(void);
static int rp_create(PCtx *p)
{
    unsigned long *c;
    RawExt *e;
    long id, st;
    if (G.rp_n >= RAWPOOL_MAX || !p || p->qctx < 0 || p->broken || G.state <= 0)
        return -1;
    e = (RawExt *)malloc(sizeof(*e));
    if (!e)
        return -1;
    id = buf_alloc_id();
    if (id < 0) {
        free(e);
        return -1;
    }
    flush();                            /* tout ce qui précède part d'abord */
    c = reserve(p, QGPU_LEN_BUF_CREATE);
    c[0] = QGPU_CMD_HDR(QGPU_OP_BUF_CREATE, QGPU_LEN_BUF_CREATE);
    c[1] = (unsigned long)id;
    c[2] = RAWPOOL_BYTES;
    st = submit_probe();
    if (st != QGPU_ST_OK) {
        /* l'identifiant n'est pas rendu : on ne sait pas s'il a été pris */
        free(e);
        gl_note("NATIVE : BUF_CREATE de la réserve %d refusé (statut %ld) : "
                "DRAW_NATIVE coupé\n", G.rp_n, st);
        G.native = 0;
        return -1;
    }
    e->off = 0;
    e->len = RAWPOOL_BYTES;
    e->next = 0;
    G.rp_qid[G.rp_n] = id;
    G.rp_free[G.rp_n] = e;
    gl_note("NATIVE : réserve %d = tampon hôte %ld (%lu Mio)\n", G.rp_n, id,
            (unsigned long)(RAWPOOL_BYTES >> 20));
    return G.rp_n++;
}

/* Le miroir de b tient-il `size` octets ? Sinon une tranche neuve (contenu à
   recopier en entier). 0 = pas de place : repli sur l'empaquetage. */
static int raw_ensure(PCtx *p, PBuf *b, unsigned long size, unsigned long base)
{
    unsigned long cap, off;
    int k;
    if (b->rp >= 0 && b->rp_cap >= size) {
        if (b->raw_base != base) {      /* réallouée par glBufferData */
            b->raw_base = base;
            rd_all(b);
        }
        return 1;
    }
    if (b->rp_failed && b->rp_fail_gen == G.rp_gen)
        return 0;
    if (b->rp >= 0) {
        rp_release(b->rp, b->rp_off, b->rp_cap);
        b->rp = -1;
    }
    cap = (size + RAW_ALIGN - 1) & ~(RAW_ALIGN - 1);
    if (cap > RAWPOOL_BYTES)
        return 0;
    for (k = 0; k < G.rp_n; k++)
        if (rp_take(k, cap, &off))
            break;
    if (k == G.rp_n) {
        k = rp_create(p);
        if (k < 0 || !rp_take(k, cap, &off)) {
            b->rp_failed = 1;
            b->rp_fail_gen = G.rp_gen;
            return 0;
        }
    }
    b->rp_failed = 0;
    b->rp = k;
    b->rp_off = off;
    b->rp_cap = cap;
    b->raw_base = base;
    rd_all(b);
    return 1;
}

/* Recopie [lo, hi) de la copie cliente dans le miroir : memcpy vers l'arène
   (LECTURE de la mémoire de l'application : sous la garde pack_jmp de
   l'appelant), puis BUF_SUBDATA. Le memcpy passe AVANT reserve : une faute
   ne laisse aucune commande à moitié écrite. */
static int raw_upload(PCtx *p, PBuf *b, unsigned long base,
                      unsigned long lo, unsigned long hi)
{
    unsigned long n, off, *c;
    while (lo < hi) {
        n = hi - lo;
        if (n > RAW_CHUNK)
            n = RAW_CHUNK;
        if (!arena_alloc(n, &off))
            return 0;
        memcpy(G.q.win + off, (const unsigned char *)base + lo, n);
        c = reserve(p, QGPU_LEN_BUF_SUBDATA);
        c[0] = QGPU_CMD_HDR(QGPU_OP_BUF_SUBDATA, QGPU_LEN_BUF_SUBDATA);
        c[1] = (unsigned long)G.rp_qid[b->rp];
        c[2] = b->rp_off + lo;
        c[3] = G.q.base + off;
        c[4] = n;
        G.n_native_bytes += n;
        G.n_native_subdata++;
        lo += n;
    }
    return 1;
}

/* Rend propre dans le miroir tout ce qui, de [lo, hi), est sale. Le reste
   des plages sales attend un dessin qui le lise (le cache d'idTech4 écrit
   tout son tampon temporaire, puis chaque dessin n'en lit qu'un morceau). */
static int raw_sync(PCtx *p, PBuf *b, unsigned long base, unsigned long size,
                    unsigned long lo, unsigned long hi)
{
    int i = 0;
    unsigned long a, z, ul, uh;
    if (hi > size)
        hi = size;
    while (i < b->rd_n) {
        a = b->rd_lo[i];
        z = b->rd_hi[i] < size ? b->rd_hi[i] : size;
        if (a >= z) {                   /* au-delà de la taille : sans objet */
            rd_remove(b, i);
            continue;
        }
        if (z <= lo || a >= hi) {
            i++;
            continue;
        }
        ul = a > lo ? a : lo;
        uh = z < hi ? z : hi;
        if (ul != a && uh != z && b->rd_n == RD_MAX) {
            /* pas de place pour couper en deux : fusionner ailleurs et
               reprendre (on ne lit jamais plus que ce que le dessin lit —
               la copie cliente peut avoir des pages non mappées) */
            rd_merge_closest(b);
            i = 0;
            continue;
        }
        if (!raw_upload(p, b, base, ul, uh))
            return 0;
        if (ul == a && uh == z) {
            rd_remove(b, i);
        } else if (ul == a) {
            b->rd_lo[i] = uh;
            b->rd_hi[i] = z;
            i++;
        } else if (uh == z) {
            b->rd_hi[i] = ul;
            i++;
        } else {
            int j;
            for (j = b->rd_n; j > i + 1; j--) {
                b->rd_lo[j] = b->rd_lo[j - 1];
                b->rd_hi[j] = b->rd_hi[j - 1];
            }
            b->rd_hi[i] = ul;
            b->rd_lo[i + 1] = uh;
            b->rd_hi[i + 1] = z;
            b->rd_n++;
            i += 2;
        }
    }
    return 1;
}

static long buf_alloc_id(void)
{
    unsigned long i;
    for (i = 0; i < QGPU_CLIENT_BUF_IDS; i++) {
        unsigned long w = i / 32, bit = 1UL << (i % 32);
        if (!(G.buf_used[w] & bit)) {
            G.buf_used[w] |= bit;
            return (long)(G.buf_base + i);
        }
    }
    return -1;
}

static void buf_free_id(long id)
{
    unsigned long i;
    if (id < (long)G.buf_base)
        return;
    i = (unsigned long)id - G.buf_base;
    if (i >= QGPU_CLIENT_BUF_IDS)
        return;
    G.buf_used[i / 32] &= ~(1UL << (i % 32));
}

static void buf_host_destroy(PCtx *p, PBuf *b)
{
    unsigned long *c;
    if (!b || b->qid < 0)
        return;
    /* P14 : l'identifiant ne se rend QUE si le BUF_DESTROY est bien parti.
       Sinon un BUF_CREATE le réutiliserait sur un tampon encore vivant côté
       hôte → QGPU_ST_LIMIT → broken_all coupe l'accélération du processus
       entier. Un identifiant perdu, lui, ne coûte qu'un emplacement. */
    if (p && p->qctx >= 0 && !p->broken && G.state > 0) {
        c = reserve(p, QGPU_LEN_BUF);
        c[0] = QGPU_CMD_HDR(QGPU_OP_BUF_DESTROY, QGPU_LEN_BUF);
        c[1] = (unsigned long)b->qid;
        buf_free_id(b->qid);
    }
    b->qid = -1;
    b->qsize = 0;
    b->pack_fmt = 0;
    b->pack_gs = 0;
    b->pack_nverts = 0;
    b->pack_key = 0;
}

static int buf_host_ensure(PCtx *p, PBuf *b, unsigned long bytes)
{
    unsigned long *c;
    long id;
    if (!p || p->qctx < 0 || p->broken || bytes == 0 || bytes > QGPU_MAX_BUF_SIZE)
        return 0;
    if (b->qid >= 0 && b->qsize >= bytes)
        return 1;
    buf_host_destroy(p, b);
    id = buf_alloc_id();
    if (id < 0)
        return 0;
    c = reserve(p, QGPU_LEN_BUF_CREATE);
    c[0] = QGPU_CMD_HDR(QGPU_OP_BUF_CREATE, QGPU_LEN_BUF_CREATE);
    c[1] = (unsigned long)id;
    c[2] = bytes;
    b->qid = id;
    b->qsize = bytes;
    /* I6 (relecture du 24/09) : tampon neuf, contenu indéfini — aucune clé
       d'emballage ne le décrit (buf_host_destroy le fait déjà quand qid ≥ 0 ;
       ici pour tous les chemins). */
    b->pack_nverts = 0;
    b->pack_key = 0;
    return 1;
}

static long buf_create(void *ctx, unsigned long *handle, void **data,
                       unsigned long *flags)
{
    PBuf *b;
    (void)ctx;
    if (!handle)
        return 0;
    b = calloc(1, sizeof(*b));
    if (!b) {
        *handle = 0;
        return 0;
    }
    b->data = data;
    b->flags = flags;
    b->qid = -1;
    b->dirty = 1;
    b->rp = -1;                         /* v18 : pas encore de miroir brut */
    rd_all(b);
    pthread_mutex_lock(&G.mu);
    b->next = G.bufs;
    G.bufs = b;
    {
        unsigned long h = buf_hash_of(data);
        b->hnext = G.buf_hash[h];
        G.buf_hash[h] = b;
    }
    pthread_mutex_unlock(&G.mu);
    *handle = (unsigned long)b;
    return 0;
}

static long buf_destroy(void *ctx, unsigned long handle)
{
    PBuf *b = (PBuf *)handle, **pp, *pctx_b;
    PCtx *p;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (b) {
        buf_host_destroy(p, b);
        for (pp = &G.bufs; (pctx_b = *pp); pp = &pctx_b->next) {
            if (pctx_b == b) {
                *pp = b->next;
                break;
            }
        }
        for (pp = &G.buf_hash[buf_hash_of(b->data)]; (pctx_b = *pp); pp = &pctx_b->hnext) {
            if (pctx_b == b) {
                *pp = b->hnext;
                break;
            }
        }
        /* v18 : la tranche du miroir brut revient à sa réserve. Aucun
           BUF_* : les dessins déjà émis qui la lisent précèdent, dans le
           flux, toute recopie future dans la même tranche. */
        if (b->rp >= 0)
            rp_release(b->rp, b->rp_off, b->rp_cap);
        free(b);
    }
    pthread_mutex_unlock(&G.mu);
    return 0;
}

/* gldFlushBuffer(ctx, poignée, ptr, longueur) — appelé par GLEngine après
 * glBufferData, glBufferSubData (memcpy du moteur : notre BufferSubData rend 0)
 * et glUnmapBuffer (docs/re/tableaux-de-sommets.md §3.0-3.1). C'est LA
 * notification « la copie cliente a changé » : le moteur pose les bits 0-1 de
 * vbo+0x48+4i après une écriture CPU, le pilote les efface ici.
 *
 * v18 : (ptr, longueur) sert de PLAGE SALE du miroir brut quand elle tombe
 * dans [base, base + taille logique) ; sinon (ou POMPPC_GL_NATIVE_RANGE=0)
 * tout le tampon est sale. Hypothèse à confirmer (sonde ci-dessous, note.txt) :
 * ptr = vbo+0x30 + offset et longueur = taille de glBufferSubData — ce que
 * fait le memcpy du moteur juste avant. Une plage PLUS LARGE que l'écriture
 * reste juste (recopie de trop) ; seule une plage fausse ET incluse dans le
 * tampon tromperait le miroir. */
static long buf_flush(void *ctx, unsigned long handle, void *ptr, unsigned long len)
{
    PBuf *b = (PBuf *)handle;
    (void)ctx;
    pthread_mutex_lock(&G.mu);
    if (b) {
        unsigned long base = b->data ? (unsigned long)*b->data : 0;
        unsigned long size = b->data ? GLD_U32((unsigned char *)b->data - VBO_DATA, VBO_SIZE) : 0;
        unsigned long p0 = (unsigned long)ptr;
        static unsigned long told;
        static int proven;              /* un ptr ≠ base a été vu : ptr porte bien
                                           l'offset (sinon « base, longueur de la
                                           sous-écriture » serait indiscernable) */
        int ranged;
        b->last_len = len;
        b->dirty = 1;
        ranged = G.native_range && base && len && p0 >= base && p0 - base <= size &&
                 len <= size - (p0 - base);
        if (ranged && p0 != base)
            proven = 1;
        if (ranged && p0 == base && len < size && !proven)
            ranged = 0;                 /* ambigu tant que rien n'est prouvé */
        if (ranged)
            rd_add(b, p0 - base, p0 - base + len);
        else
            rd_all(b);                  /* raw_sync ne recopiera que ce que les
                                           dessins lisent */
        if (told < 12) {
            told++;
            gl_note("FlushBuffer #%lu : vbo %08lx base %08lx taille %lu ptr %08lx "
                    "(base%+ld) longueur %lu -> %s\n", told,
                    (unsigned long)b->data - VBO_DATA, base, size, p0,
                    (long)(p0 - base), len, ranged ? "plage" : "tout");
        }
        if (b->flags)
            *b->flags &= ~3UL;          /* GeForce3 : rlwinm bits 0-1 */
    }
    pthread_mutex_unlock(&G.mu);
    return 0;
}

static long buf_reclaim(void *ctx, unsigned long handle)
{
    (void)ctx;
    (void)handle;
    return 0;
}

/* ─────────── sonde : programmes de pipeline (ARB vp/fp) — 23/09/2026 ───────────
 * Colin McRae (IndirectX/ZonicLib) emploie des programmes ARB et des attributs
 * génériques : le protocole doit les apprendre (étape D). On journalise ce que
 * GLEngine dépose (docs/re/tableaux-de-sommets.md §4 : le descripteur reçu est
 * ppobj+0x4c8, cible u16 à +0 ; texte ASCII à ppobj+0x14, longueur +0x18), puis
 * on transmet à Apple tel quel. */
typedef long (*pp_create_fn)(void *, unsigned long *, void *);
typedef long (*pp_modify_fn)(void *, unsigned long, unsigned long);
static void pp_log(const char *what, void *desc, unsigned long mask)
{
    static unsigned long n_text, n_param;
    unsigned char *obj = desc ? (unsigned char *)desc - 0x4c8 : 0;
    unsigned long target = desc ? U16(desc, 0) : 0;
    if (!obj)
        return;
    if (mask & 2)
        n_param++;
    if (mask & 1 || !mask) {
        const char *text = (const char *)GLD_U32(obj, 0x14);
        unsigned long len = GLD_U32(obj, 0x18);
        gl_note("PIPELINE %s cible %04lx type %u masque %lx texte %p len %lu (params vus %lu)\n",
                what, target, U16(desc, 2), mask, (void *)text, len, n_param);
        if (text && len && len < 65536 && n_text < 12) {
            char line[200]; unsigned long i, k = 0;
            n_text++;
            for (i = 0; i < len; i++) {
                char ch = text[i];
                if (ch == '\n' || k >= sizeof(line) - 2) {
                    line[k] = 0; gl_note("  | %s\n", line); k = 0;
                    if (ch != '\n') line[k++] = ch;
                } else if (ch != '\r')
                    line[k++] = ch;
            }
            if (k) { line[k] = 0; gl_note("  | %s\n", line); }
        }
    }
}
typedef long (*pp_destroy_fn)(void *, unsigned long);
typedef long (*pp_info_fn)(void *, unsigned long, unsigned long, void *);

static PProg *pprog_of(unsigned long h)
{
    unsigned long i = h - PPROG_HANDLE - 1;
    return ((h & 0xfff00000UL) == PPROG_HANDLE && i < PPROG_MAX && pprog[i].ctx)
           ? &pprog[i] : 0;
}

static PProg *pprog_find(void *ctx, const unsigned char *obj)
{
    int i;
    for (i = 0; i < PPROG_MAX; i++)
        if (pprog[i].ctx == ctx && pprog[i].obj == obj)
            return &pprog[i];
    return 0;
}

/* L'objet hôte meurt : PROG_DESTROY si le contexte qgpu vit encore, et les
   miroirs du contexte oublient ce programme. Sous G.mu. */
static void pprog_release(PProg *r)
{
    PCtx *p = find_ctx(r->ctx);
    if (p) {
        if (p->cur_vp == r) p->cur_vp = 0;
        if (p->cur_fp == r) p->cur_fp = 0;
        if (p->vp_rec == r) p->vp_rec = 0;
        if (p->fp_rec == r) p->fp_rec = 0;
        if (r->id >= 0 && r->id < QGPU_MAX_PROG) {
            p->prog_used[r->id] = 0;
            if (G.state > 0 && p->qctx >= 0 && !p->broken) {
                unsigned long *c = reserve(p, QGPU_LEN_PROG);
                c[0] = QGPU_CMD_HDR(QGPU_OP_PROG_DESTROY, QGPU_LEN_PROG);
                c[1] = (unsigned long)r->id;
            }
        }
    }
    r->id = -1;
}

static long pp_create(void *ctx, unsigned long *handle, void *desc)
{
    unsigned long apple = 0;
    long r = ((pp_create_fn)pomppc_real[GLD_CreatePipelineProgram])(ctx, &apple, desc);
    int i;
    pp_log("create", desc, 0);
    if (!handle)
        return r;
    *handle = apple;
    if (r != 0)
        return r;
    pthread_mutex_lock(&G.mu);
    for (i = 0; i < PPROG_MAX && pprog[i].ctx; i++)
        ;
    if (i < PPROG_MAX) {
        memset(&pprog[i], 0, sizeof(pprog[i]));
        pprog[i].ctx = ctx;
        pprog[i].obj = desc ? (unsigned char *)desc - 0x4c8 : 0;
        pprog[i].apple = apple;
        pprog[i].target = desc ? U16(desc, 0) : 0;
        pprog[i].id = -1;
        *handle = PPROG_HANDLE + (unsigned long)i + 1;
    } else {
        gl_note("PIPELINE : table pleine, poignée d'Apple rendue telle quelle\n");
    }
    pthread_mutex_unlock(&G.mu);
    gl_note("  -> poignée %lx (Apple %lx) retour %ld\n", *handle, apple, r);
    return r;
}
static long pp_modify(void *ctx, unsigned long handle, unsigned long mask)
{
    static unsigned long seen;
    PProg *r;
    long rc;
    pthread_mutex_lock(&G.mu);
    r = pprog_of(handle);
    if (r) {
        if (mask & 1) r->text_dirty = 1;
        if (mask & 2) r->local_dirty = 1;
        handle = r->apple;
    }
    pthread_mutex_unlock(&G.mu);
    rc = ((pp_modify_fn)pomppc_real[GLD_ModifyPipelineProgram])(ctx, handle, mask);
    if (seen++ < 16 || mask & 1)
        gl_note("PIPELINE modify poignée %lx masque %lx\n", handle, mask);
    return rc;
}
static long pp_destroy(void *ctx, unsigned long handle)
{
    PProg *r;
    pthread_mutex_lock(&G.mu);
    r = pprog_of(handle);
    if (r) {
        handle = r->apple;
        pprog_release(r);
        memset(r, 0, sizeof(*r));
    }
    pthread_mutex_unlock(&G.mu);
    return ((pp_destroy_fn)pomppc_real[GLD_DestroyPipelineProgram])(ctx, handle);
}
static long pp_getinfo(void *ctx, unsigned long handle, unsigned long pname, void *out)
{
    PProg *r;
    pthread_mutex_lock(&G.mu);
    r = pprog_of(handle);
    if (r)
        handle = r->apple;
    pthread_mutex_unlock(&G.mu);
    return ((pp_info_fn)pomppc_real[GLD_GetPipelineProgramInfo])(ctx, handle, pname, out);
}

/* Entrées gld que le plugin réalise lui-même, au lieu de les transmettre au
 * rendu d'Apple. Le crochet pomppc_pre rend l'adresse à appeler : il suffit d'y
 * rendre la nôtre, le trampoline saute dedans avec les arguments d'origine.
 * (C'est aussi ce qui évite de toucher à gld_tramp.s, qui est engendré.) */
void *pomppc_gld_override(int id)
{
    /* F6 — la décision se prenait À CHAQUE APPEL, sur des drapeaux qui
       basculent en vol : broken_all coupe G.v7 (chemin brut refusé) ou pose
       qry_off (requêtes non tenues). Un tampon de sommets NÉ chez nous se
       faisait alors libérer par le code d'Apple, qui recevait notre
       vb_scratch ; un DestroyQuery jamais rendu perdait les 16 identifiants du
       client. La table est donc FIGÉE à la première consultation utile :
       l'objet qui naît chez nous meurt chez nous. */
    static int frozen, use_vb, use_qry;

    if (G.state <= 0)
        return 0;
    if (!frozen) {
        use_vb  = G.v7 != 0;
        use_qry = G.v8 && !qry_off;
        frozen  = 1;
    }
    switch (id) {
    case GLD_CreatePipelineProgram: return (void *)pp_create;
    case GLD_ModifyPipelineProgram: return (void *)pp_modify;
    case GLD_DestroyPipelineProgram: return (void *)pp_destroy;     /* v16 */
    case GLD_GetPipelineProgramInfo: return (void *)pp_getinfo;
    case GLD_CreateBuffer:  return (void *)buf_create;
    case GLD_DestroyBuffer: return (void *)buf_destroy;
    case GLD_FlushBuffer:   return (void *)buf_flush;
    case GLD_ReclaimBuffer: return (void *)buf_reclaim;
    }
    if (use_vb) {
        switch (id) {
        case GLD_AllocVertexBuffer:    return (void *)geom_alloc_vb;
        case GLD_CompleteVertexBuffer: return (void *)geom_complete_vb;
        case GLD_FreeVertexBuffer:     return (void *)geom_free_vb;
        }
    }
    if (!use_qry)
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
 *   — TABLEAUX DE SOMMETS (l'autre canal). Si GL_VERTEX_ARRAY est actif,
 *     on pose cfg+0x78 : _gleDrawArraysOrElements_Exec quitte Begin/End et
 *     appelle RenderVertexArray (+0x70, VAR) ou AllocVertexBuffer /
 *     RenderVertexBuffer (+0x4c). Le descripteur cfg+0x11c RESTE : glBegin
 *     continue d'écrire les attributs bruts. ARRAY=2 retire le descripteur
 *     (canal GeForce3 strict) ; un glBegin dans cet état pose array_mix et
 *     le dispatch suivant republie. Le plugin lit GS_VAO et émet un DRAW_RAW
 *     indexé — les sommets uniques ne sont copiés qu'une fois.
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

/* POMPPC_GL_ARRAY : 0 coupé (défaut), 1 mixte : cfg+0x78 sans retirer
 * cfg+0x11c, 2 forcé GeForce3 (descripteur nul). Le mixte n'est pas le
 * défaut : un DrawElements mal packé ressemble à de la géométrie cassée
 * (Colin McRae, comme le début d'UT2004). */
static int array_switch(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("POMPPC_GL_ARRAY");
        v = (e && *e) ? atoi(e) : 0;
        if (v < 0)
            v = 0;
    }
    return v;
}

/* Bit 16+a du masque 64 bits (haut en VA_EN_HI, bas en VA_EN_LO). */
static int va_enabled(const unsigned char *V, int a)
{
    if (!V || a < 0 || a > 31)
        return 0;
    if (a < 16)
        return (int)((GLD_U32(V, VA_EN_LO) >> (16 + a)) & 1);
    return (int)((GLD_U32(V, VA_EN_HI) >> (a - 16)) & 1);
}

/* Un tableau de positions est-il actif ? Critère pour poser cfg+0x78
 * (DrawArrays/DrawElements quittent Begin/End). ARRAY=2 force ce canal
 * et retire aussi le descripteur. */
static int geom_va_on(PCtx *p)
{
    unsigned char *g, *V;
    int sw = array_switch();
    if (!p)
        return 0;
    g = gls(p);
    V = g ? (unsigned char *)GLD_U32(g, GS_VAO) : 0;
    /* v16 : sous programme de sommets, DrawArrays/DrawElements passent par le
       canal tableaux (nous packons depuis le VAO, génériques compris) : le
       déroulage T&L de GLEngine lit ses pointeurs résolus périmés et rend
       les valeurs courantes (Colin McRae en course, docs/re/programmes-arb.md
       §3 bis). glBegin garde le descripteur (mode mixte). */
    if (G.prog && p->vp_on && V && (va_enabled(V, 0) || va_enabled(V, 16)))
        return 1;
    if (!sw)
        return 0;
    if (sw >= 2)
        return 1;
    return va_enabled(V, 0);
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

/* ─────────── v16 : synchronisation des programmes ARB avec l'hôte ─────────── */

#define GC_PROG_CUR(t)   (0x5420 + 4 * (t))     /* objet courant de la cible t (0 vp, 1 fp) */
#define GC_GLSL_ACTIVE   0x5430                  /* objet shader (GLSL) actif, ou 0 */
#define GC_VP_ENABLE     0x4664                  /* u8 : glEnable(GL_VERTEX_PROGRAM_ARB)
                                                    (_gleGetEnabled 0x22310) */
#define GC_FP_ENABLE     0x466c                  /* u8 : glEnable(GL_FRAGMENT_PROGRAM_ARB)
                                                    (_gleGetEnabled 0x22328) */
#define GC_PROG_ON(gc, t) GLD_U8((gc), (t) ? GC_FP_ENABLE : GC_VP_ENABLE)
#define GC_PROG_ENV(t)   (0x4668 + 8 * (t))     /* ptr : program.env de la cible t */
#define PP_TEXT          0x14                    /* ppobj : texte ASCII (malloc) */
#define PP_LEN           0x18                    /*   longueur */
#define PP_LOCAL         0x4e0                   /*   ptr : program.local, 16 o chacun */
#define PP_PARAMS(t)     ((t) == 0 ? 256UL : 128UL)   /* env et local : 256 vp, 128 fp */

static unsigned long text_sum(const char *s, unsigned long n)
{
    unsigned long h = 5381, i;
    for (i = 0; i < n; i++)
        h = h * 33 + (unsigned char)s[i];
    return h;
}

/* 1 + le plus grand indice littéral de `what[` dans le texte (les plages
   a..b comptent pour b) ; ~0 si un indice n'est pas littéral (registre
   d'adresse : tout est possible) ; 0 si absent. */
static unsigned long text_max_index(const char *s, unsigned long n, const char *what)
{
    unsigned long wl = strlen(what), i, best = 0;
    for (i = 0; i + wl < n; i++) {
        unsigned long j, v = 0, last = 0;
        int any = 0;
        if (memcmp(s + i, what, wl) != 0)
            continue;
        /* ZonicLib écrit « program.env  [0..95] » : des blancs avant le
           crochet (vu en vrai, Colin McRae : env 0 sans ceci) */
        j = i + wl;
        while (j < n && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (j >= n || s[j] != '[')
            continue;                   /* program.environment, etc. */
        j++;
        while (j < n && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (j < n && !(s[j] >= '0' && s[j] <= '9'))
            return ~0UL;
        while (j < n) {
            if (s[j] >= '0' && s[j] <= '9') { v = v * 10 + (unsigned long)(s[j] - '0'); any = 1; }
            else if (s[j] == '.' || s[j] == ' ') { if (any) last = v; v = 0; any = 0; }
            else break;
            j++;
        }
        if (any)
            last = v;
        if (last + 1 > best)
            best = last + 1;
    }
    return best;
}

/* Programme de fragments : quelle cible chaque unité échantillonne-t-elle ?
   « texture[u], 2D » → masque TU_ENABLE de la cible. */
static void text_fp_units(PProg *r, const char *s, unsigned long n)
{
    unsigned long i;
    memset(r->fp_unit, 0, sizeof(r->fp_unit));
    r->fp_units_bad = 0;
    for (i = 0; i + 9 < n; i++) {
        unsigned long j, u = 0;
        unsigned char m = 0;
        if (memcmp(s + i, "texture[", 8) != 0)
            continue;
        j = i + 8;
        if (!(s[j] >= '0' && s[j] <= '9')) { r->fp_units_bad = 1; continue; }
        while (j < n && s[j] >= '0' && s[j] <= '9') { u = u * 10 + (unsigned long)(s[j] - '0'); j++; }
        while (j < n && (s[j] == ']' || s[j] == ' ' || s[j] == ','))
            j++;
        if (j + 2 <= n && !memcmp(s + j, "2D", 2)) m = 8;
        else if (j + 2 <= n && !memcmp(s + j, "1D", 2)) m = 0x10;
        else if (j + 2 <= n && !memcmp(s + j, "3D", 2)) m = 2;
        else if (j + 4 <= n && !memcmp(s + j, "CUBE", 4)) m = 1;
        else if (j + 4 <= n && !memcmp(s + j, "RECT", 4)) m = 4;
        if (u >= (unsigned long)pomppc_backend_units() || !m ||
            (r->fp_unit[u] && r->fp_unit[u] != m))
            r->fp_units_bad = 1;
        else
            r->fp_unit[u] = m;
    }
}

/* Ce que le TEXTE dit (indices lus, unités échantillonnées) : relu dès qu'il
   change — et dès le dispatch, parce que le format de sommet et les unités de
   texture en dépendent AVANT que le lot ne le compile (vu en vrai : la scène
   arbfp perdait sa texture, l'unité n'étant décidée qu'au lot). */
/* 24/09/2026 — quelles entrées vertex.* un programme de sommets lit-il ?
 * DOOM 3 laisse le tableau conventionnel de texcoord 0 ACTIF (menus) avec un
 * pointeur périmé pendant les passes d'interaction, où le programme ne lit
 * que vertex.attrib[8..11], vertex.position et vertex.color : le plugin
 * empaquetait ce tableau fantôme (lecture hors page → SIGSEGV) pour rien.
 * Sous programme, un attribut n'est porté que si le TEXTE le lit. Alias ARB :
 * attrib[0] = position, [2] = normale, [3] = couleur, [4] = secondaire,
 * [5] = brouillard, [8 + u] = texcoord u.
 *
 * I7 (relecture du 24/09) — ce que fait l'hôte (qgpu-gl.c, préparation des
 * tableaux) : GEN(0) seul est aliasé (donné par glVertexPointer) ; normale,
 * couleurs, brouillard et TEX(u) passent par les tableaux conventionnels,
 * GEN(1..15) par glVertexAttribPointerARB(k), sans lien entre les deux. Le
 * masque ne fait donc que NE PAS RETIRER l'autre moitié d'un alias quand son
 * tableau est actif (geom_format ne porte GEN(k) que si le tableau générique
 * est activé, et l'attribut conventionnel que si le sien l'est) :
 *   vertex.normal → NORMAL + GEN(2), vertex.color → COLOR + GEN(3),
 *   vertex.color.secondary → SEC + GEN(4), vertex.fogcoord → FOG + GEN(5),
 *   vertex.texcoord[u] → TEX(u) + GEN(8+u) (données en
 *   glVertexAttribPointerARB(8+u) : le générique reste porté) ;
 *   vertex.attrib[2..5] → générique + conventionnel ;
 *   vertex.attrib[8+u] → GEN(8+u) SEUL : le tableau conventionnel de texcoord
 *   u est justement celui que DOOM 3 laisse périmé.
 * Pas aliasé : un programme hôte qui lit vertex.attrib[3] alors que seul le
 * tableau de couleurs est actif reçoit la valeur courante du générique 3 (il
 * faudrait empaqueter le tableau conventionnel dans GEN(3) ; non fait). Blancs
 * tolérés entre le nom et `[`, et après `[` ; un indice non littéral fait tout
 * garder. */
#define VPN_NORMAL   0x1UL
#define VPN_COLOR    0x2UL
#define VPN_SEC      0x4UL
#define VPN_FOG      0x8UL
#define VPN_TEX(u)   (0x10UL << (u))            /* unités 0..7 : bits 4..11 */
#define VPN_GEN(k)   (0x1000UL << (k))          /* génériques 0..15 : bits 12..27 */
#define VPN_VALID    0x80000000UL
static unsigned long text_vp_inputs(const char *s, unsigned long n)
{
    unsigned long i, need = VPN_VALID;
    for (i = 0; i + 7 < n; i++) {
        unsigned long j, v = 0;
        int any = 0;
        if (memcmp(s + i, "vertex.", 7) != 0)
            continue;
        /* I7 : « tmpvertex.x » (suffixe d'un identificateur) n'est pas une entrée */
        if (i > 0 && ((s[i - 1] >= 'a' && s[i - 1] <= 'z') || (s[i - 1] >= 'A' && s[i - 1] <= 'Z') ||
                      (s[i - 1] >= '0' && s[i - 1] <= '9') || s[i - 1] == '_'))
            continue;
        j = i + 7;
        if (j + 8 <= n && !memcmp(s + j, "position", 8)) continue;
        if (j + 6 <= n && !memcmp(s + j, "normal", 6)) {
            need |= VPN_NORMAL | VPN_GEN(2);    /* I7 : alias gardé */
            continue;
        }
        if (j + 5 <= n && !memcmp(s + j, "color", 5)) {
            /* vertex.color.secondary : la couleur secondaire */
            if (j + 15 <= n && !memcmp(s + j + 5, ".secondary", 10)) need |= VPN_SEC | VPN_GEN(4);
            else need |= VPN_COLOR | VPN_GEN(3);
            continue;
        }
        if (j + 8 <= n && !memcmp(s + j, "fogcoord", 8)) {
            need |= VPN_FOG | VPN_GEN(5);
            continue;
        }
        if (j + 8 <= n && !memcmp(s + j, "texcoord", 8)) {
            j += 8;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < n && s[j] == '[') {
                j++;
                while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
                while (j < n && s[j] >= '0' && s[j] <= '9') { v = v * 10 + (unsigned long)(s[j] - '0'); any = 1; j++; }
                if (!any) return need | 0x0FFFFFFF;     /* indice non littéral : tout */
            }
            /* I7 : données en glVertexAttribPointerARB(8+u) — ne pas masquer le générique */
            if (v < QGPU_MAX_UNITS) need |= VPN_TEX(v) | VPN_GEN(8 + v);
            continue;
        }
        if (j + 6 <= n && !memcmp(s + j, "attrib", 6)) {
            j += 6;
            /* I7 (relecture du 24/09) : « attrib [8] » est du ARB valide */
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j >= n || s[j] != '[')
                return need | 0x0FFFFFFF;       /* forme inconnue : tout */
            j++;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            while (j < n && s[j] >= '0' && s[j] <= '9') { v = v * 10 + (unsigned long)(s[j] - '0'); any = 1; j++; }
            if (!any) return need | 0x0FFFFFFF;
            if (v == 2) need |= VPN_NORMAL;
            else if (v == 3) need |= VPN_COLOR;
            else if (v == 4) need |= VPN_SEC;
            else if (v == 5) need |= VPN_FOG;
            /* attrib[8+u] : le générique porte la donnée, pas le tableau
               conventionnel de texcoord u (c'est lui qui est périmé) */
            if (v < QGPU_VF_GEN_MAX) need |= VPN_GEN(v);
            continue;
        }
    }
    return need;
}

static void prog_parse(PProg *r)
{
    const char *text;
    unsigned long len;
    int t;
    if (!r->obj)
        return;
    if (r->target != QGPU_PT_VERTEX && r->target != QGPU_PT_FRAGMENT) {
        r->target = U16(r->obj, 0x4c8);      /* posée au premier glProgramStringARB */
        if (r->target != QGPU_PT_VERTEX && r->target != QGPU_PT_FRAGMENT)
            return;
    }
    text = (const char *)GLD_U32(r->obj, PP_TEXT);
    len = GLD_U32(r->obj, PP_LEN);
    if (!text || !len || len > QGPU_MAX_PROG_LEN)
        return;
    if (text == r->parsed_text && len == r->parsed_len && !r->text_dirty)
        return;
    t = (r->target == QGPU_PT_FRAGMENT) ? 1 : 0;
    r->env_n = text_max_index(text, len, "program.env");
    r->local_n = text_max_index(text, len, "program.local");
    if (r->env_n > PP_PARAMS(t)) r->env_n = PP_PARAMS(t);
    if (r->local_n > PP_PARAMS(t)) r->local_n = PP_PARAMS(t);
    if (t)
        text_fp_units(r, text, len);
    else
        r->vp_need = text_vp_inputs(text, len);
    r->parsed_text = text;
    r->parsed_len = len;
}

/* Ce que GLEngine dit des programmes : actifs ? lesquels ? Aucune commande. */
static void prog_state(PCtx *p)
{
    unsigned char *gc = gctx_of(p);
    int t;
    p->vp_on = p->fp_on = 0;
    p->vp_rec = p->fp_rec = 0;
    if (!gc || !G.prog)
        return;
    for (t = 0; t < 2; t++) {
        unsigned char *obj = (unsigned char *)GLD_U32(gc, GC_PROG_CUR(t));
        PProg *r;
        if (!GC_PROG_ON(gc, t) || !obj)
            continue;
        r = pprog_find(p->ctx, obj);
        if (r)
            prog_parse(r);
        if (t) { p->fp_on = 1; p->fp_rec = r; } else { p->vp_on = 1; p->vp_rec = r; }
    }
}

/* Le domaine, côté programmes (après prog_state). */
static int prog_domain_ok(PCtx *p)
{
    unsigned char *gc = gctx_of(p);
    int t;
    if (!gc)
        return 1;
    if (GLD_U32(gc, GC_GLSL_ACTIVE))
        return no(NO_G_PROGRAM, 0x5430, GLD_U32(gc, GC_GLSL_ACTIVE));
    for (t = 0; t < 2; t++) {
        int on = t ? p->fp_on : p->vp_on;
        PProg *r = t ? p->fp_rec : p->vp_rec;
        if (!G.prog) {
            /* sans la v16, un programme actif est lisible par gctx+0x4e1c
               (testé avant) ; on ne sait rien de plus ici */
            continue;
        }
        if (!GC_PROG_ON(gc, t))
            continue;
        if (!on || !r)
            return no(NO_G_PROGRAM, GLD_U32(gc, GC_PROG_CUR(t)), (unsigned long)t);
        if (r->refused)
            return no(NO_G_PROG_HOST, (unsigned long)r->id, (unsigned long)t);
        if (t && r->text_sent && r->fp_units_bad)
            return no(NO_G_PROG_UNITS, (unsigned long)r->id, 0);
    }
    return 1;
}

/* Soumission SONDE, synchrone, sans broken_all : le statut est la réponse. */
static long submit_probe(void)
{
    unsigned long pc = 0;
    long st;
    dump_submit();
    st = qgpu_submit(&G.q, G.hb, G.ncmd * 4, &pc);
    G.n_submits++;
    G.h[G.cur].busy = 0;
    G.h[G.cur].npost = 0;
    G.ncmd = 0;
    G.arena = 0;
    G.bound = 0;
    if (st != QGPU_ST_OK)
        gl_note("PROG sonde : statut %ld commande %lu\n", st, pc);
    return st;
}

/* Le texte du programme est-il sur l'hôte, compilé ? Sinon on l'y envoie —
   dans une soumission synchrone à part, pour connaître le verdict du
   compilateur de l'hôte : un texte refusé sort du domaine (Apple l'émule),
   sans rien casser d'autre. */
static int prog_ensure(PCtx *p, PProg *r)
{
    const char *text;
    unsigned long len, off, sum, *c;
    int t;
    long st;

    if (!r->obj)
        return 0;
    /* La cible n'est posée dans l'objet (+0x4c8) qu'au premier
       glProgramStringARB : à la création, GLEngine la laisse à 0. */
    if (r->target != QGPU_PT_VERTEX && r->target != QGPU_PT_FRAGMENT) {
        r->target = U16(r->obj, 0x4c8);
        if (r->target != QGPU_PT_VERTEX && r->target != QGPU_PT_FRAGMENT)
            return 0;
    }
    text = (const char *)GLD_U32(r->obj, PP_TEXT);
    len = GLD_U32(r->obj, PP_LEN);
    t = (r->target == QGPU_PT_FRAGMENT) ? 1 : 0;
    if (!text || !len || len > QGPU_MAX_PROG_LEN)
        return 0;
    if (r->id >= 0 && !r->text_dirty && text == r->text_sent && len == r->len_sent)
        return !r->refused;
    sum = text_sum(text, len);
    if (r->id >= 0 && text == r->text_sent && len == r->len_sent && sum == r->sum_sent) {
        r->text_dirty = 0;
        return !r->refused;
    }
    flush();                            /* tout ce qui précède part d'abord */
    if (r->id < 0) {
        int i;
        for (i = 0; i < QGPU_MAX_PROG && p->prog_used[i]; i++)
            ;
        if (i == QGPU_MAX_PROG)
            return 0;
        p->prog_used[i] = 1;
        r->id = i;
        c = reserve(p, QGPU_LEN_PROG_CREATE);
        c[0] = QGPU_CMD_HDR(QGPU_OP_PROG_CREATE, QGPU_LEN_PROG_CREATE);
        c[1] = (unsigned long)i;
        c[2] = r->target;
    }
    if (!arena_alloc(len, &off))
        return 0;
    memcpy(G.q.win + off, text, len);
    c = reserve(p, QGPU_LEN_PROG_STRING);
    c[0] = QGPU_CMD_HDR(QGPU_OP_PROG_STRING, QGPU_LEN_PROG_STRING);
    c[1] = (unsigned long)r->id;
    c[2] = len;
    c[3] = G.q.base + off;
    prog_parse(r);                      /* déjà fait au dispatch, sauf texte tout neuf */
    st = submit_probe();
    r->text_sent = text;
    r->len_sent = len;
    r->sum_sent = sum;
    r->text_dirty = 0;
    r->local_dirty = 1;                 /* objet hôte neuf : les locaux repartent */
    r->refused = st != QGPU_ST_OK;
    gl_note("PROG %ld cible %04lx : %lu octets, env %lu local %lu%s%s\n", r->id, r->target,
            len, r->env_n, r->local_n, r->refused ? " REFUSÉ par l'hôte" : " compilé",
            (t && r->fp_units_bad) ? " (unités hors bornes)" : "");
    return !r->refused;
}

/* n × 4 flottants d'une table de GLEngine → arène, assainis (put_f) ; 0 si
   l'arène est pleine pour de bon. */
static int prog_params_arena(const unsigned char *src, unsigned long first, unsigned long n,
                             unsigned long *off)
{
    unsigned long i, *dst;
    if (!arena_alloc(n * 16, off))
        return 0;
    dst = (unsigned long *)(G.q.win + *off);
    for (i = 0; i < n; i++)
        put_f(dst + i * 4, (const float *)(src + (first + i) * 16), 4);
    return 1;
}

/* program.env de la cible t : ce qui a changé depuis le miroir, par plages
   (trous de 4 entrées au plus fusionnés). */
static void prog_send_env(PCtx *p, unsigned char *gc, int t, PProg *r)
{
    const unsigned char *tab = (const unsigned char *)GLD_U32(gc, GC_PROG_ENV(t));
    unsigned long n = r->env_n, i = 0;
    if (!tab || !n)
        return;
    if (n > PP_PARAMS(t))
        n = PP_PARAMS(t);
    while (i < n) {
        unsigned long j, off, *c, gap;
        if (i < p->c_env_n[t] && !memcmp(tab + i * 16, p->c_env[t][i], 16)) {
            i++;
            continue;
        }
        /* plage [i, j) : on l'étend tant qu'un changement suit à moins de 5.
           `gap` = dernier indice CHANGÉ : la plage finit juste après lui —
           l'ancien « j -= gap » retombait sur i quand cinq entrées égales
           suivaient, et la boucle ne progressait plus (Colin McRae figé au
           chargement, 23/09/2026, pile CrashReporter dans prog_sync). */
        gap = i;
        j = i + 1;
        while (j < n && j - gap <= 4) {
            if (!(j < p->c_env_n[t] && !memcmp(tab + j * 16, p->c_env[t][j], 16)))
                gap = j;
            j++;
        }
        j = gap + 1;
        if (!prog_params_arena(tab, i, j - i, &off))
            return;
        memcpy(p->c_env[t][i], tab + i * 16, (j - i) * 16);
        c = reserve(p, QGPU_LEN_PROG_PARAMS);
        c[0] = QGPU_CMD_HDR(QGPU_OP_PROG_ENV, QGPU_LEN_PROG_PARAMS);
        c[1] = t ? QGPU_PT_FRAGMENT : QGPU_PT_VERTEX;
        c[2] = i;
        c[3] = j - i;
        c[4] = G.q.base + off;
        i = j;
    }
    if (n > p->c_env_n[t])
        p->c_env_n[t] = n;
}

static void prog_send_local(PCtx *p, PProg *r)
{
    const unsigned char *tab = (const unsigned char *)GLD_U32(r->obj, PP_LOCAL);
    unsigned long n = r->local_n, off, *c;
    r->local_dirty = 0;
    if (!tab || !n)
        return;
    if (!prog_params_arena(tab, 0, n, &off))
        return;
    c = reserve(p, QGPU_LEN_PROG_PARAMS);
    c[0] = QGPU_CMD_HDR(QGPU_OP_PROG_LOCAL, QGPU_LEN_PROG_PARAMS);
    c[1] = (unsigned long)r->id;
    c[2] = 0;
    c[3] = n;
    c[4] = G.q.base + off;
}

/* Avant un lot brut : programmes compilés, liés, paramètres à jour. 0 = ce
   lot doit aller à Apple (texte refusé, table pleine). */
static int prog_sync(PCtx *p)
{
    unsigned char *gc = gctx_of(p);
    int t;
    if (!G.prog || !gc)
        return 1;
    prog_state(p);
    for (t = 0; t < 2; t++) {
        PProg *r = t ? p->fp_rec : p->vp_rec;
        PProg **cur = t ? &p->cur_fp : &p->cur_vp;
        unsigned long *c;
        if (!(t ? p->fp_on : p->vp_on))
            continue;
        if (!r || !prog_ensure(p, r))
            return 0;
        if (t && r->fp_units_bad)
            return no(NO_G_PROG_UNITS, (unsigned long)r->id, 0);
        if (*cur != r) {
            c = reserve(p, QGPU_LEN_PROG_BIND);
            c[0] = QGPU_CMD_HDR(QGPU_OP_PROG_BIND, QGPU_LEN_PROG_BIND);
            c[1] = r->target;
            c[2] = (unsigned long)r->id;
            *cur = r;
        }
        prog_send_env(p, gc, t, r);
        if (r->local_dirty)
            prog_send_local(p, r);
    }
    return 1;
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

    if (!texturing_on(p))
        return 0;
    dt = unit_drvtex(p, u, &mask);
    t = dt ? intern_tex(dt) : 0;
    if (!t)
        return 0;
    (void)lv;
    return tex_complete(t->drvtex) || tex_base_ok(t->drvtex);
}

/* Le texturage courant tiendra-t-il sur l'hôte ? Prédicat pur (aucune commande,
 * aucune conversion) : c'est la moitié chère du domaine, appelée à chaque
 * changement d'état, d'où le raccourci « déjà téléversée et propre ». */
static int geom_texture_ok(PCtx *p)
{
    unsigned char *g = gls(p);
    int u, i;

    if (!texturing_on(p))
        return 1;
    for (i = G.units; i < GL_MAX_TEXUNITS; i++)
        if (unit_mask(p, i))
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
        if (unit_slot(mask) < 0)
            return no(NO_TEX_TARGET, mask, GLD_U32(p->ctx, CTX_TEXUNITS));
        /* Texture 3D ou carte de cube : GLEngine jetait la géométrie brute
           dès qu'une coordonnée r était donnée entre glBegin et glEnd — la
           cible n'y était pour rien (scène tcprobe, docs/re/opengl-1.4.md §3).
           C'est cfg+0x7a qui le règle (pomppc_geom_context) ; sans lui, ces
           cibles restent au chemin hérité, exact. */
        if ((unit_slot(mask) == 0 || unit_slot(mask) == 1) &&
            !(p->cfg && GLD_U8(p->cfg, 0x7a)))
            return no(NO_G_TEX3D, u, mask);
        if (env != 0x2100 && env != 0x2101 && env != 0x0BE2 && env != 0x1E01 &&
            env != 0x0104 && env != 0x8570)
            return no(NO_TEX_ENV, env, u);
        if (env == 0x8570 && !combine_ok(us, u, &tu))
            return 0;
        t = dt ? intern_tex(dt) : 0;
        if (!t)
            return no(NO_TEX_UNKNOWN, (unsigned long)dt, mask);
        (void)lv;
        if (!tex_complete(t->drvtex) && !tex_base_ok(t->drvtex))
            continue;                   /* jamais définie : l'unité est coupée, comme en GL */
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
    /* 1.4 : l'hôte v10 dérive la taille lui-même (QGPU_SK_POINT_*) */
    if ((att[0] != 1.0f || att[1] != 0.0f || att[2] != 0.0f) &&
        !(G.tex14 && point_params_ok(g)))
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
    /* v16 : programmes ARB. Sous G.prog, un programme de sommets ou de
       fragments actif reste dans le domaine si l'hôte l'a compilé (ou peut le
       compiler : prog_sync s'en charge au lot) ; un objet GLSL (gctx+0x5430)
       ou un texte refusé par l'hôte renvoient à Apple. */
    prog_state(p);
    if (!prog_domain_ok(p))
        return 0;
    /* Colin McRae en course (23/09/2026, sonde) : seuls les attributs
       GÉNÉRIQUES 0, 1, 2 (glVertexAttribPointerARB, emplacements 16-18 du
       descripteur, bits du mot haut V+0x330) sont actifs — l'attribut 0 tient
       lieu de position (docs/re/descripteur-de-sommet.md). Notre format ne
       portait que les attributs conventionnels : GLEngine déroulait alors les
       tableaux dans notre tampon avec les VALEURS COURANTES (position 0,0,0,1,
       couleur blanche) — maillages effondrés, « géométrie éclatée ». Depuis la
       v16, le format porte QGPU_VF_GEN(1..7) (geom_format) ; sans elle, on
       laisse GLEngine transformer lui-même (image juste, comme
       POMPPC_GL_GEOM=0). */
    {
        unsigned char *g = gls(p);
        unsigned char *V = g ? (unsigned char *)GLD_U32(g, GS_VAO) : 0;
        if (V && G.prog && (GLD_U32(V, VA_EN_HI) & ~0xffffUL) != 0)
            return no(NO_G_GENERIC, GLD_U32(V, VA_EN_HI), 16);  /* génériques > 15 */
        if (V && !G.prog && GLD_U32(V, VA_EN_HI) != 0) {
            /* Pour de bon sur ce contexte : dessin par dessin, le va-et-vient
               T&L matériel / logiciel faisait sortir le jeu sur une assertion
               (COpenGLFragmentProgram : erreur GL) ; tout logiciel comme
               POMPPC_GL_GEOM=0, il joue. */
            static int probed;
            if (!probed) {
                probed = 1;
                va_probe(p, V, 0, "attributs génériques (refus)", -1);
            }
            p->geom_lost = 1;
            return no(NO_G_GENERIC, GLD_U32(V, VA_EN_HI), GLD_U32(V, VA_EN_LO));
        }
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

    /* Toujours la couleur : les glyphes WC3 sont des quads colorés. Sans ça,
       éclairage allumé et COLOR_MATERIAL éteint → l'hôte n'éclairerait que le
       matériau (souvent alpha 0 : gazon invisible). compute_geom_state pose
       alors COLOR_MATERIAL sur l'hôte pour que cette couleur serve. */
    fmt |= QGPU_VF_COLOR;
    /* La coordonnée de brouillard n'est portée QUE si c'est bien elle la source :
       la présence du bit dit à l'hôte de poser GL_FOG_COORDINATE. */
    if (GLD_U8(g, GS_FOG) && U16(g, GS_FOG_COORD_SRC) == 0x8451)
        fmt |= QGPU_VF_FOG;
    /* Couleur secondaire (1.4) : seulement si la somme est allumée hors
       éclairage — éclairée, c'est l'éclairage de l'hôte qui la fournit. */
    if (G.tex14 && GLD_U8(g, GS_COLOR_SUM) && !lighting)
        fmt |= QGPU_VF_SEC_COLOR;
    for (u = 0; u < QGPU_MAX_UNITS; u++)
        if (unit_textured(p, u)) {
            fmt |= QGPU_VF_TEX(u);
            if (texgen_needs_normal(p, u))
                normal = 1;
        }
    if (normal)
        fmt |= QGPU_VF_NORMAL;
    /* v16 : attributs génériques 1..7 actifs (mot haut du masque du VAO, bit k
       = emplacement 16 + k). Le générique 0 est la position : GLEngine l'y
       écrit lui-même (code 0 inactif → alias 16), inutile de le porter deux
       fois. Sans programme de sommets actif, l'hôte les lit et les ignore —
       on ne les porte alors pas. */
    if (G.prog && p->vp_on) {
        unsigned char *V = g ? (unsigned char *)GLD_U32(g, GS_VAO) : 0;
        unsigned long hi = V ? GLD_U32(V, VA_EN_HI) : 0;
        unsigned long lo = V ? GLD_U32(V, VA_EN_LO) : 0;
        int k;
        static long vpneed = -1;        /* R1 (relecture du 24/09) : lu une fois */
        if (vpneed < 0) {
            const char *e = getenv("POMPPC_GL_VPNEED");
            vpneed = (e && e[0] == '0') ? 0 : 1;
        }
        for (k = 1; k < QGPU_VF_GEN_MAX; k++)
            if (hi & (1UL << k))
                fmt |= QGPU_VF_GEN(k);
        /* Colin McRae en course (sonde VA_PTRS, nuit du 23/09) : les pointeurs
           RÉSOLUS de GLEngine des tableaux conventionnels DÉSACTIVÉS restent
           ceux du dernier usage (le HUD), et le déroulage T&L lit là plutôt
           que de prendre la valeur courante. Sous programme, un attribut
           conventionnel n'est donc demandé que si son tableau est ACTIF (bit
           16 + code du mot bas) ; absent, l'hôte prend la valeur courante,
           ce que le programme attend de vertex.color / vertex.texcoord. */
        if (V) {
            if (!(lo & (1UL << 18))) fmt &= ~(unsigned long)QGPU_VF_COLOR;
            if (!(lo & (1UL << 17))) fmt &= ~(unsigned long)QGPU_VF_NORMAL;
            if (!(lo & (1UL << 20))) fmt &= ~(unsigned long)QGPU_VF_SEC_COLOR;
            if (!(lo & (1UL << 19))) fmt &= ~(unsigned long)QGPU_VF_FOG;
            for (u = 0; u < QGPU_MAX_UNITS; u++)
                if (!(lo & (1UL << (24 + u))))
                    fmt &= ~(unsigned long)QGPU_VF_TEX(u);
        }
        /* 24/09 : et seulement ce que le TEXTE du programme lit (DOOM 3 :
           tableau de texcoord 0 actif mais périmé pendant les interactions) */
        if (p->vp_rec && (p->vp_rec->vp_need & VPN_VALID) && vpneed) {
            unsigned long need = p->vp_rec->vp_need;
            if (!(need & VPN_COLOR))  fmt &= ~(unsigned long)QGPU_VF_COLOR;
            if (!(need & VPN_NORMAL)) fmt &= ~(unsigned long)QGPU_VF_NORMAL;
            if (!(need & VPN_SEC))    fmt &= ~(unsigned long)QGPU_VF_SEC_COLOR;
            if (!(need & VPN_FOG))    fmt &= ~(unsigned long)QGPU_VF_FOG;
            for (u = 0; u < QGPU_MAX_UNITS; u++)
                if (!(need & VPN_TEX(u)))
                    fmt &= ~(unsigned long)QGPU_VF_TEX(u);
            for (k = 1; k < QGPU_VF_GEN_MAX; k++)
                if (!(need & VPN_GEN(k)))
                    fmt &= ~(unsigned long)QGPU_VF_GEN(k);
        }
    }
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
    /* P6 : NEUF entrées sont possibles (position, normale, couleur,
       secondaire, brouillard, tex0–3) et le tableau en tenait huit — deux
       octets de pile écrasés, juste sur `off`, `n` et `u`, qui servent après.
       Atteignable avec COLOR_SUM + coordonnée de brouillard + texgen sans
       éclairage + 4 unités, ou POMPPC_GL_GEOM=2. */
    unsigned short ent[26];             /* v16 : + 15 génériques (24 au plus) */
    unsigned long off = 0;
    int n = 0, u;

    /* v16 : sous programme, la position vient du GÉNÉRIQUE 0 (code 16)
       quand c'est lui qui est actif — le code 0 lirait le pointeur résolu
       périmé du tableau conventionnel désactivé (Colin McRae en course,
       géométrie éclatée ; cf. geom_format). */
    int pos_code = 0;
    if (G.prog && p->vp_on) {
        unsigned char *g = gls(p);
        unsigned char *V = g ? (unsigned char *)GLD_U32(g, GS_VAO) : 0;
        if (V && !(GLD_U32(V, VA_EN_LO) & (1UL << 16)) && (GLD_U32(V, VA_EN_HI) & 1UL))
            pos_code = 16;
    }
    /* Republier n'a de sens que si le format ou le code de position change,
       OU si GLEngine n'a pas encore notre descripteur (cfg+0x11c remis à zéro
       à la création). */
    if (fmt == p->geom_fmt && pos_code == p->desc_pos_code &&
        GLD_U32(p->cfg, 0x11c) == (unsigned long)p->desc)
        return 0;
    ent[n++] = DESC_ENT(pos_code, off, 4); off += 4;          /* position */
    p->desc_pos_code = pos_code;
    if (fmt & QGPU_VF_NORMAL) { ent[n++] = DESC_ENT(1, off, 3); off += 3; }
    if (fmt & QGPU_VF_COLOR)  { ent[n++] = DESC_ENT(2, off, 4); off += 4; }
    if (fmt & QGPU_VF_SEC_COLOR) { ent[n++] = DESC_ENT(4, off, 3); off += 3; }
    if (fmt & QGPU_VF_FOG)    { ent[n++] = DESC_ENT(3, off, 1); off += 1; }
    for (u = 0; u < QGPU_MAX_UNITS; u++)
        if (fmt & QGPU_VF_TEX(u)) { ent[n++] = DESC_ENT(8 + u, off, 4); off += 4; }
    /* v16 : attributs génériques, codes 16 + k, 4 composantes, après les
       coordonnées de texture — l'ordre fixe de DRAW_RAW */
    for (u = 1; u < QGPU_VF_GEN_MAX; u++)
        if (fmt & QGPU_VF_GEN(u)) { ent[n++] = DESC_ENT(16 + u, off, 4); off += 4; }
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
    /* v16 : programmes ARB — la clé ne vaut 1 que si l'hôte a le programme
       (prog_state au dispatch, prog_sync au lot : un texte refusé sort du
       domaine avant d'arriver ici). */
    v[QGPU_SK_VERTEX_PROGRAM] = (G.prog && p->vp_on && p->vp_rec && !p->vp_rec->refused) ? 1 : 0;
    v[QGPU_SK_FRAGMENT_PROGRAM] = (G.prog && p->fp_on && p->fp_rec && !p->fp_rec->refused) ? 1 : 0;
    /* P17 — ces trois sondes étaient relues par getenv à CHAQUE LOT DE DESSIN.
       Le getenv de Darwin 8 balaie `environ` avec strncmp : ≈ 250 000 strncmp
       par image sur le G4 émulé, pour trois sondes éteintes. Toutes les autres
       sondes du fichier sont derrière un static ; celles-ci le sont désormais
       aussi. (C'est aussi la moitié de F3 : plus de getenv par lot, donc plus
       de lecture d'`environ` pendant qu'un autre fil le réalloue.) */
    {
        static int probes = -1;         /* bit 0 NOCULL, bit 1 FLIPFACE */
        static int glyph;               /* 0 aucun, 1 armé, 2 stencil seul */
        if (probes < 0) {
            const char *gt = getenv("POMPPC_GL_GLYPHTEST");
            glyph = gt ? ((*gt == '2') ? 2 : 1) : 0;
            probes = (getenv("POMPPC_GL_NOCULL") ? 1 : 0) |
                     (getenv("POMPPC_GL_FLIPFACE") ? 2 : 0);
        }
        /* Sondes : POMPPC_GL_NOCULL coupe l'élimination, POMPPC_GL_FLIPFACE
           retourne le sens des faces. */
        if (probes & 1)
            v[QGPU_SK_CULL_FACE] = 0;
        if (probes & 2)
            v[QGPU_SK_FRONT_FACE] = v[QGPU_SK_FRONT_FACE] == 0x0900 ? 0x0901 : 0x0900;
    /* POMPPC_GL_GLYPHTEST : rendre inratables les lots faits comme le texte des
       menus (mélange + test alpha + éclairage). S'ils atteignent l'écran, on
       verra des rectangles opaques à la place des lettres. La valeur 2 ne coupe
       que le test de stencil, dont le tampon hôte peut ne pas valoir le nôtre. */
        if (glyph == 2) {
            v[QGPU_SK_STENCIL_TEST] = 0;
            v[QGPU_SK_STENCIL_FUNC] = 0x0207;
            v[QGPU_SK_STENCIL_VALUE_MASK] = 0xFF;
        } else if (glyph == 1 && GLD_U8(g, GS_ALPHA_TEST) && GLD_U8(g, GS_BLEND) &&
                   GLD_U8(g, GS_LIGHTING)) {
            v[QGPU_SK_ALPHA_TEST] = 0;
            v[QGPU_SK_ALPHA_FUNC] = 0x0207;
            v[QGPU_SK_ALPHA_REF] = 0;
            v[QGPU_SK_BLEND] = 0;
            v[QGPU_SK_LIGHTING] = 0;
        }
    }
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
        p->nat_cur_ok &= ~(1UL << QGPU_CUR_COLOR);      /* v18 */
        put_f(a + 1, (const float *)(g + GS_CUR_COLOR), 4);
        send_cmd(p, QGPU_OP_SET_CURRENT, QGPU_LEN_SET_CURRENT, a);
    }
    if (!(fmt & QGPU_VF_NORMAL) && changed(p, g + GS_CUR_NORMAL, p->c_cur[1], 12)) {
        a[0] = QGPU_CUR_NORMAL;
        p->nat_cur_ok &= ~(1UL << QGPU_CUR_NORMAL);
        put_f(a + 1, (const float *)(g + GS_CUR_NORMAL), 3);
        a[4] = 0;
        send_cmd(p, QGPU_OP_SET_CURRENT, QGPU_LEN_SET_CURRENT, a);
    }
    /* La couleur secondaire n'est jamais portée par le sommet : la mettre dans
       le format ferait allumer GL_COLOR_SUM sur l'hôte, ce que l'état GL ne dit
       pas. Elle passe donc toujours en valeur courante. */
    if (changed(p, g + GS_CUR_SECCOLOR, p->c_cur[2], 12)) {
        a[0] = QGPU_CUR_SEC_COLOR;
        p->nat_cur_ok &= ~(1UL << QGPU_CUR_SEC_COLOR);
        put_f(a + 1, (const float *)(g + GS_CUR_SECCOLOR), 3);
        a[4] = 0;
        send_cmd(p, QGPU_OP_SET_CURRENT, QGPU_LEN_SET_CURRENT, a);
    }
    if (!(fmt & QGPU_VF_FOG) && changed(p, g + GS_CUR_FOGCOORD, p->c_cur[3], 4)) {
        a[0] = QGPU_CUR_FOG;
        p->nat_cur_ok &= ~(1UL << QGPU_CUR_FOG);
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
/* Lot 11 (23/09/2026) — sonde « arme noire » d'UT2004 : aux trois premiers
   dessins dont l'unité 0 porte une carte de cube avec au moins trois unités
   actives, vider dans note.txt les blocs d'unité BRUTS de GLEngine (0x7c
   octets chacun), la table des textures liées du contexte (cible et niveau 0
   de chaque objet, identifiant hôte), le bloc texgen de chaque unité, et ce
   que le plugin en a résolu. C'est ce qui départage « sources du combineur
   mal lues » et « textures échangées entre unités » (docs/re/ut2004-arme-
   noire.md §3). POMPPC_GL_CUBEPROBE=0 coupe la sonde. */
static void note_hex(const char *tag, const unsigned char *b, unsigned long n)
{
    unsigned long i;
    char line[3 * 16 + 1];
    for (i = 0; i < n; i += 16) {
        unsigned long j, m = n - i < 16 ? n - i : 16;
        for (j = 0; j < m; j++)
            sprintf(line + 3 * j, "%02x ", b[i + j]);
        line[3 * m] = 0;
        gl_note("   %s+%03lx: %s\n", tag, i, line);
    }
}
static void cube_probe(PCtx *p, const TexInfo *ti, const char *where)
{
    static int shots = 0, on = -1;
    unsigned char *g = gls(p);
    unsigned long units;
    int u, k, n = 0;
    if (on < 0) {
        const char *e = getenv("POMPPC_GL_CUBEPROBE");
        on = !(e && *e == '0');
    }
    if (!on || shots >= 3 || !g || !p->ctx)
        return;
    {   /* armée par le même fichier que le vidage déclenché : la scène voulue
           (arme en main) est à l'écran quand il apparaît */
        static int armed;
        const char *trig = getenv("POMPPC_GL_DUMP_TRIGGER");
        if (!armed) {
            if (trig && *trig && access(trig, F_OK) != 0)
                return;
            armed = 1;
        }
    }
    {
        int cube = 0;
        for (u = 0; u < GL_MAX_TEXUNITS; u++) {
            unsigned long m = GLD_U32(g, GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE + TU_ENABLE) & 0x1f;
            if (m)
                n++;
            if (m & 1)
                cube = 1;
        }
        if (!cube || n < 2)
            return;
    }
    shots++;
    units = GLD_U32(p->ctx, CTX_TEXUNITS);
    gl_note("SONDE-CUBE (%s) image %lu : %d unites actives, table %08lx, cube %d xbar %d\n",
            where, G.n_frames, n, units, G.cube, G.xbar);
    for (u = 0; u < 4; u++) {
        unsigned char *us = g + GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE;
        char tag[16];
        gl_note(" unite %d : enable %02lx env %04lx comb rgb %04lx a %04lx | src rgb %04lx %04lx %04lx"
                " a %04lx %04lx %04lx | op rgb %04lx %04lx %04lx a %04lx %04lx %04lx | scale %g %g\n",
                u, GLD_U32(us, TU_ENABLE), (unsigned long)U16(us, TU_ENV_MODE),
                (unsigned long)U16(us, TU_COMBINE_RGB), (unsigned long)U16(us, TU_COMBINE_A),
                (unsigned long)U16(us, TU_SRC0_RGB), (unsigned long)U16(us, TU_SRC0_RGB + 2),
                (unsigned long)U16(us, TU_SRC0_RGB + 4),
                (unsigned long)U16(us, TU_SRC0_A), (unsigned long)U16(us, TU_SRC0_A + 2),
                (unsigned long)U16(us, TU_SRC0_A + 4),
                (unsigned long)U16(us, TU_OP0_RGB), (unsigned long)U16(us, TU_OP0_RGB + 2),
                (unsigned long)U16(us, TU_OP0_RGB + 4),
                (unsigned long)U16(us, TU_OP0_A), (unsigned long)U16(us, TU_OP0_A + 2),
                (unsigned long)U16(us, TU_OP0_A + 4),
                (double)GLD_F32(us, TU_RGB_SCALE), (double)GLD_F32(us, TU_ALPHA_SCALE));
        sprintf(tag, "u%d", u);
        note_hex(tag, us, GS_TEXUNIT_SIZE);
        for (k = 0; k < 5 && units; k++) {
            unsigned char *dt = (unsigned char *)GLD_U32(units, u * 0x14 + k * 4);
            unsigned char *prm, *lv;
            PTex *t;
            if (!dt)
                continue;
            prm = (unsigned char *)GLD_U32(dt, DT_PARAMS);
            lv = dt + DT_LEVEL0;
            t = find_tex(dt);
            gl_note("   empl %d : dt %08lx cible %d niv0 %dx%d fmt %04lx type %04lx base %04lx qtex %ld\n",
                    k, (unsigned long)dt, prm ? GLD_U8(prm, TP_TARGET) : -1,
                    (int)(short)U16(lv, LV_W), (int)(short)U16(lv, LV_H),
                    (unsigned long)U16(lv, LV_FORMAT), (unsigned long)U16(lv, LV_TYPE),
                    GLD_U32(dt, DT_BASE_FORMAT), t ? t->qtex : -2L);
        }
        sprintf(tag, "tg%d", u);
        note_hex(tag, g + GS_TEXGEN(u), 0x94);
        if (ti)
            gl_note("   resolu : qtex %ld env %04lx combine %08lx src %08lx\n",
                    ti->u[u].t ? ti->u[u].t->qtex : -1L, ti->u[u].env_mode,
                    ti->u[u].combine, ti->u[u].combine_src);
    }
}

/* Lot 11 : après le déclencheur du vidage, une ligne par décision de
   texturage (≤ 200) — unités, textures hôte et tailles, environnement,
   combineur, mélange, éclairage — pour retrouver les dessins de l'arme. */
static void draw_probe(PCtx *p, const TexInfo *ti)
{
    static int armed, lines;
    unsigned char *g = gls(p);
    const char *trig = getenv("POMPPC_GL_DUMP_TRIGGER");
    char buf[400];
    int at = 0, u;
    if (!armed) {
        if (!trig || !*trig || access(trig, F_OK) != 0)
            return;
        armed = 1;
    }
    if (lines >= 200 || !g)
        return;
    lines++;
    at += snprintf(buf + at, sizeof(buf) - at, "DESSIN image %lu light %d blend %d %04x/%04x alpha %d :",
                   G.n_frames, GLD_U8(g, GS_LIGHTING) != 0, GLD_U8(g, GS_BLEND) != 0,
                   (unsigned)U16(g, GS_BLEND_SRC_RGB), (unsigned)U16(g, GS_BLEND_DST_RGB),
                   GLD_U8(g, GS_ALPHA_TEST) != 0);
    for (u = 0; u < QGPU_MAX_UNITS && at < (int)sizeof(buf) - 80; u++) {
        unsigned char *us = g + GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE;
        unsigned long m = GLD_U32(us, TU_ENABLE) & 0x1f;
        const PTex *t = ti->u[u].t;
        if (!m)
            continue;
        at += snprintf(buf + at, sizeof(buf) - at, " u%d[m%02lx tex %ld %dx%d env %04lx cb %08lx src %08lx]",
                       u, m, t ? t->qtex : -1L,
                       t ? (int)(short)U16((unsigned char *)t->drvtex + DT_LEVEL0, LV_W) : 0,
                       t ? (int)(short)U16((unsigned char *)t->drvtex + DT_LEVEL0, LV_H) : 0,
                       ti->u[u].env_mode, ti->u[u].combine, ti->u[u].combine_src);
    }
    gl_note("%s\n", buf);
}

static void *geom_begin(void *ctx, short mode, unsigned long *n)
{
    PCtx *p;
    TexInfo ti;
    unsigned char *gc;
    unsigned long words, slots;

    if (pomppc_tracing())
        pomppc_log("  géométrie : Begin(mode %d, n %lu)\n", mode, n ? *n : 0);
    pthread_mutex_lock(&G.mu);
    stream_ready();                     /* F9 : G.vtx, G.hb, G.win relus après */
    p = find_ctx(ctx);
    /* F2 : on ne referme que NOTRE `pend`. Le poser à 0 globalement écrasait
       celui d'un autre contexte (autre fil), dont la place réservée n'était
       alors jamais rendue et dont le geom_end ne reconnaissait plus rien. */
    pend_close(p, 1);
    if (!p || !geom_ok(p) || !ensure_surface(p)) {
        if (p) {
            no(NO_G_LATE, (unsigned long)(unsigned short)mode, p->geom_on);
            p->geom_lost = 1;           /* exact à partir du prochain dispatch */
            G.n_geomdrop++;
        }
        goto refuse;
    }
    /* Texture pas encore téléversable (atlas de police WC3 : TexImage après
       le dispatch, ou premier lot avant intern). Un flush libère l'arène ;
       si ça échoue encore, on jette CE lot sans tuer le T&L — le suivant
       (menu animé, HUD) reprendra le chemin brut avec la texture prête. */
    if (!texture_ok(p, &ti)) {
        flush();
        if (!texture_ok(p, &ti)) {
            no(NO_G_LATE, (unsigned long)(unsigned short)mode, p->geom_on);
            G.n_geomdrop++;
            goto refuse;
        }
    }
    /* Ce que GLEngine a VRAIMENT retenu : si le pas ou le descripteur ne sont
       pas les nôtres, c'est son sommet interne (déjà transformé) qui arrive —
       le lire comme de la géométrie brute donnerait n'importe quoi. */
    gc = gctx_of(p);
    words = p->geom_words;
    if (!gc || !words || GLD_U16(gc, GC_VTX_STRIDE) != words * 4 ||
        GLD_U32(gc, GC_VTX_DESC) != (unsigned long)p->desc) {
        /* Canal GeForce3 strict (ARRAY=2) : cfg+0x11c est nul, le sommet
           interne n'est pas le nôtre. Un glBegin ici pose array_mix : le
           prochain dispatch republie le descripteur. ARRAY=1 garde 0x11c,
           donc glBegin continue. */
        if (p->cfg && GLD_U32(p->cfg, 0x11c) == 0) {
            p->array_mix = 1;
            p->desc_dirty = 1;
            goto refuse;
        }
        no(NO_G_STRIDE, gc ? GLD_U16(gc, GC_VTX_STRIDE) : 0, words * 4);
        p->geom_lost = 1;
        G.n_geomdrop++;
        goto refuse;
    }
    /* P11 — le FORMAT de sommet a été publié au DISPATCH ; les UNITÉS de
       texture, elles, viennent d'être décidées ici (texture_ok). Une texture
       qui n'est devenue complète qu'APRÈS le dispatch (atlas de polices WC3 :
       glTexImage entre deux glBegin) allume l'unité alors que le descripteur
       que GLEngine vient d'employer ne porte PAS ses coordonnées : l'hôte
       échantillonnait le texel (0,0) sur toute la primitive, et
       geom_send_current n'envoie jamais CUR_TEXCOORD. On arrive ici en
       sachant que GLEngine s'est bien servi de NOTRE descripteur (test
       ci-dessus) : si le format a bougé depuis, on refuse CE lot — le rendu
       d'Apple est exact — et on demande la republication au prochain
       dispatch. Cause probable du texte disparu par le chemin brut. */
    {
        unsigned long now_fmt = geom_format(p);
        if (now_fmt != p->geom_fmt) {
            no(NO_G_LATE, now_fmt, p->geom_fmt);
            p->desc_dirty = 1;
            G.n_geomdrop++;
            goto refuse;
        }
    }
    /* v16 : programmes ARB — compilés, liés et paramétrés sur l'hôte AVANT
       l'état (prog_sync peut vider le flux : c'est avant pend_open). Un
       texte refusé par l'hôte renvoie ce lot à Apple. */
    if (!prog_sync(p)) {
        no(NO_G_LATE, (unsigned long)(unsigned short)mode, p->geom_on);
        G.n_geomdrop++;
        goto refuse;
    }
    /* v16 : les trois premiers lots à attributs génériques, tableaux de
       GLEngine sondés (Colin McRae : sommets reçus aux valeurs courantes) */
    if (p->geom_fmt & QGPU_VF_GEN_MASK) {
        static int gen_probed, gen3_probed;
        unsigned char *V = (unsigned char *)GLD_U32(gls(p), GS_VAO);
        /* la mémoire des tableaux génériques 0/1 AU DÉBUT du déroulage (lots
           de course : générique 3 présent), sommet 0 et sommet 8 */
        if (V && (p->geom_fmt & QGPU_VF_GEN(3)) && gen3_probed < 8) {
            const unsigned long *g0 = (const unsigned long *)GLD_U32(VA_SLOT(V, 16), 0);
            const unsigned long *g1 = (const unsigned long *)GLD_U32(VA_SLOT(V, 17), 0);
            unsigned long st0 = GLD_U32(VA_SLOT(V, 16), 4) / 4;
            static const unsigned long *prev_g0;
            gen3_probed++;
            /* le tampon du lot PRÉCÉDENT, relu maintenant : rempli après coup ? */
            if (prev_g0)
                gl_note("  précédent @%p relu : %08lx %08lx %08lx / s8 %08lx\n", (const void *)prev_g0,
                        prev_g0[0], prev_g0[1], prev_g0[2], prev_g0[8 * st0]);
            prev_g0 = g0;
            if (g0 && g1)
                gl_note("BEGIN gen0 @%p : %08lx %08lx %08lx / s8 %08lx %08lx %08lx  gen1 @%p : %08lx %08lx %08lx (n %lu)\n",
                        (const void *)g0, g0[0], g0[1], g0[2], g0[8 * st0], g0[8 * st0 + 1], g0[8 * st0 + 2],
                        (const void *)g1, g1[0], g1[1], g1[2], n ? *n : 0UL);
        }
        if (gen_probed < 3 && V) {
            gen_probed++;
            gl_note("GEN lot fmt %lx vp %d (id %ld) fp %d desc %08lx/%08lx pas %u\n",
                    p->geom_fmt, p->vp_on, p->vp_rec ? p->vp_rec->id : -1L, p->fp_on,
                    GLD_U32(gc, GC_VTX_DESC), (unsigned long)p->desc, GLD_U16(gc, GC_VTX_STRIDE));
            va_probe(p, V, 0, "génériques (v16)", -1);
        }
    }
    check_draw_buffer(p);
    sync_to_host(p, 1, GLD_U8(gls(p), GS_DEPTH_TEST) || stencil_active(p));
    send_state(p, &ti, 1);
    /* génériques de GLEngine : 4 flottants chacun (descripteur, DESC_ENT) */
    if (gs_stale(p, p->geom_fmt, 0))
        gs_put(reserve(p, QGPU_LEN_SET_STATE), p, 0);
    geom_send_all(p, p->geom_fmt);
    /* Plus aucun vidage entre ici et EndPrimitiveBuffer : GLEngine écrit dans
       la fenêtre partagée pendant ce temps. On fait donc la place maintenant. */
    if (G.ncmd + QGPU_LEN_DRAW_RAW + 4 > CMD_WORDS || geom_slots(words) < GEOM_MIN_SLOTS)
        flush();
    slots = geom_slots(words);
    /* TAMPON = NOMBRE ENTIER DE PRIMITIVES. GLEngine remplit le tampon offert
       jusqu'au bout et, pour GL_TRIANGLES/QUADS/LINES, coupe une primitive à
       cheval sur deux tampons : le suivant commence par la fin du triangle
       précédent. Avec 8 192 places (≢ 0 mod 3), chaque gros maillage arrivait
       ainsi en lots dont les triangles se chevauchent d'un ou deux sommets, et
       tout ce qui suivait la coupure reliait des sommets de triangles voisins
       (échardes dans le texte du logo d'UT2004 — prouvé par rejeu natif le
       22/09/2026, phase 1 dès le premier sommet du second tampon). Les rubans
       et éventails, eux, sont recoupés par GLEngine avec répétition des
       sommets (POMPPC_GL_GEOM_SLOTS l'a établi) : pas d'arrondi nécessaire. */
    {
        unsigned long pm = (unsigned long)(unsigned short)mode;
        unsigned long unit = (pm == QGPU_PRIM_MODE_TRIANGLES) ? 3 :
                             (pm == QGPU_PRIM_MODE_QUADS) ? 4 :
                             (pm == QGPU_PRIM_MODE_LINES) ? 2 : 1;
        slots -= slots % unit;
    }
    if (slots < 4)
        goto refuse;                    /* ne devrait pas arriver : 4 Mio de sommets */
    p->pend_open = 1;
    G.npend++;
    p->pend_half = G.hb;                /* la moitié ne doit plus changer d'ici End */
    p->pend_off = G.vtx;
    p->pend_words = words;
    p->pend_fmt = p->geom_fmt;
    p->pend_slots = slots;
    p->pend_drop = 0;
    /* L'ombrage décide de l'ordre des indices d'un quadrilatère ; il ne peut
       plus changer entre ici et EndPrimitiveBuffer. */
    p->pend_flat = GLD_U32(gls(p), GS_SHADE_MODEL) == GL_FLAT;
    /* Fil de fer ou points : la fusion recollerait les quadrilatères et les
       polygones en TRIANGLES indexés, et l'hôte tracerait alors les diagonales
       de la décomposition — c'est exactement l'information de contour que la v8
       tient pour DRAW_RAW. On garde donc les primitives telles quelles.
       Vu en vrai (scène « polymode ») : sans ceci, la diagonale du
       quadrilatère en fil de fer est tracée, et 3 % de l'image diffère. */
    p->pend_wire = U16(gls(p), GS_POLY_MODE) != GL_FILL ||
                   U16(gls(p), GS_POLY_MODE + 2) != GL_FILL;
    /* la zone est prise tout de suite : un autre fil ne doit pas la réutiliser */
    G.vtx += slots * words * 4;
    if (n)
        *n = slots;
    pthread_mutex_unlock(&G.mu);
    return G.win + VTX_OFF + p->pend_off;

refuse:
    /* P3 — on rend le tampon de SECOURS, et on est précisément dans le cas où
       le pas de GLEngine n'est PAS le nôtre (gctx+0x4880 = 0x100 sur le canal
       GeForce3) : dimensionner le compte de sommets avec NOTRE pas lui faisait
       écrire 1024 × 256 = 256 Kio dans un tampon de 64 Kio — Bus error.
       geom_scratch se compte donc au pas MAXIMUM, celui de GLEngine
       (GLD_VERTEX_SIZE), sans condition. */
    if (n)
        *n = sizeof(geom_scratch) / GLD_VERTEX_SIZE;
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
static int raw_promote(int flat)
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
    G.raw_nidx = tris_emit(o, G.raw_mode, 0, n, flat);
    G.idx += G.raw_nidx * 2;
    G.raw_mode = QGPU_PRIM_MODE_TRIANGLES;
    G.raw_count = G.raw_nidx;
    return 1;
}

/* Ajoute le lot qui vient d'être écrit (mode m, n sommets à l'offset `off` de
 * la zone des sommets) à la série ouverte, en TRIANGLES indexés. 0 =
 * impossible : l'appelant ferme la série et en ouvre une neuve, ce qui est
 * toujours exact. `off` et `flat` viennent du `pend` du CONTEXTE (F2). */
static int merge_batch(unsigned long m, unsigned long n, unsigned long words,
                       unsigned long off, int flat)
{
    unsigned long base = (off - G.raw_start) / (words * 4);
    unsigned long need = tris_nidx(m, n);
    unsigned short *o;

    if (base + n > 65536)               /* les indices sont des u16 */
        return 0;
    if (!G.raw_idx && !raw_promote(flat))
        return 0;
    /* `n` de DRAW_RAW, c'est le nombre d'INDICES quand la série est indexée :
       il est borné par QGPU_MAX_VERTS dans le cœur. */
    if (G.raw_nidx + need > QGPU_MAX_VERTS)
        return 0;
    o = idx_room(need);
    if (!o)
        return 0;
    G.idx += tris_emit(o, m, base, n, flat) * 2;
    G.raw_nidx += need;
    G.raw_count = G.raw_nidx;
    G.raw_lots++;
    return 1;
}

/* ─────────── sonde POMPPC_GL_GEOMDUMP=<fichier> : un lot de géométrie brute ───────────
 *
 * Le texte des menus et les brins d'herbe de Warcraft III sortent justes par le
 * chemin rastérisé et disparaissent par le chemin brut. La sonde ne retient que
 * les lots susceptibles d'être eux — une unité porte une texture GL_ALPHA (les
 * polices) ou le test alpha est armé (les brins) — et écrit l'état qui décide de
 * leur aspect, puis les premiers sommets tels que GLEngine les a rangés. */
static FILE *geom_dump_file(void)
{
    static FILE *f;
    static int init;
    if (!init) {
        const char *path = getenv("POMPPC_GL_GEOMDUMP");
        init = 1;
        if (path && path[0] == '/' && (f = fopen(path, "w")) != 0)
            setvbuf(f, (char *)0, _IOLBF, 0);
    }
    return f;
}

static void geom_probe(PCtx *p, const char *via, unsigned long m, unsigned long n,
                       unsigned long fmt, unsigned long words, const float *v)
{
    static unsigned long left = 400;
    static long from = -1;
    FILE *f = geom_dump_file();
    unsigned char *g;
    const float *mt;
    int u, i, alpha_tex = 0, nv;

    if (!f || !left || !p || !v)
        return;
    if (from < 0) {                     /* image à partir de laquelle on retient */
        const char *e = getenv("POMPPC_GL_GEOMDUMP_AT");
        from = (e && *e) ? atol(e) : 0;
    }
    if ((long)G.n_frames < from)
        return;
    g = gls(p);
    if (!g)
        return;
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        unsigned long mask;
        void *dt = unit_drvtex(p, u, &mask);
        if (dt && GLD_U32(dt, DT_BASE_FORMAT) == 0x1906)
            alpha_tex = 1;
    }
    /* Les glyphes : mélange sans éclairage. Les brins : test alpha. */
    if (!alpha_tex && !GLD_U8(g, GS_ALPHA_TEST) &&
        !(GLD_U8(g, GS_BLEND) && !GLD_U8(g, GS_LIGHTING)))
        return;
    left--;
    fprintf(f, "%s mode %lu n %lu fmt %08lx words %lu | alphatex %d test %d func %x ref %g"
            " | blend %d %x/%x | lighting %d\n",
            via, m, n, fmt, words, alpha_tex, GLD_U8(g, GS_ALPHA_TEST),
            U16(g, GS_ALPHA_FUNC), GLD_F32(g, GS_ALPHA_REF),
            GLD_U8(g, GS_BLEND), U16(g, GS_BLEND_SRC_RGB), U16(g, GS_BLEND_DST_RGB),
            GLD_U8(g, GS_LIGHTING));
    {   /* ce qui décide où le lot atterrit et s'il survit aux tests */
        const long *vp = (const long *)(g + GS_VIEWPORT);
        const float *mv = (const float *)(g + GS_MAT_MODELVIEW);
        const float *pr = (const float *)(g + GS_MAT_PROJ);
        fprintf(f, "   depth %d func %x mask %d | cull %d %x | colormat %d mode %x"
                " | vp %ld %ld %ld %ld | envoye %d\n",
                GLD_U8(g, GS_DEPTH_TEST), U16(g, GS_DEPTH_FUNC), GLD_U8(g, GS_DEPTH_MASK),
                GLD_U8(g, GS_CULL_FACE), U16(g, GS_CULL_MODE),
                GLD_U8(g, GS_COLOR_MATERIAL), U16(g, GS_COLORMAT_MODE),
                vp[0], vp[1], vp[2], vp[3], p->g_sent);
        fprintf(f, "   mv %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g\n",
                mv[0], mv[1], mv[2], mv[3], mv[4], mv[5], mv[6], mv[7],
                mv[8], mv[9], mv[10], mv[11], mv[12], mv[13], mv[14], mv[15]);
        fprintf(f, "   pr %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g\n",
                pr[0], pr[1], pr[2], pr[3], pr[4], pr[5], pr[6], pr[7],
                pr[8], pr[9], pr[10], pr[11], pr[12], pr[13], pr[14], pr[15]);
    }
    {   /* éclairage allumé + COLOR_MATERIAL éteint : couleur ET alpha du
           fragment viennent du matériau, pas du sommet. */
        const unsigned char *mt = g + GS_MATERIAL_FRONT;
        const float *am = (const float *)(mt + MT_AMBIENT);
        const float *di = (const float *)(mt + MT_DIFFUSE);
        const float *em = (const float *)(mt + MT_EMISSION);
        fprintf(f, "   mat amb %g %g %g %g dif %g %g %g %g emi %g %g %g %g"
                " | lumieres %08lx ambiance %g %g %g %g\n",
                am[0], am[1], am[2], am[3], di[0], di[1], di[2], di[3],
                em[0], em[1], em[2], em[3], GLD_U32(g, GS_LIGHT_MASK),
                GLD_F32(g, GS_SCENE_AMBIENT), GLD_F32(g, GS_SCENE_AMBIENT + 4),
                GLD_F32(g, GS_SCENE_AMBIENT + 8), GLD_F32(g, GS_SCENE_AMBIENT + 12));
    }
    for (u = 0; u < QGPU_MAX_UNITS; u++) {
        unsigned long mask;
        void *dt = unit_drvtex(p, u, &mask);
        unsigned char *lv;
        if (!dt)
            continue;
        lv = (unsigned char *)dt + DT_LEVEL0;
        mt = (const float *)(g + GS_MAT_TEXTURE(u));
        fprintf(f, "   unite %d mask %lx base %lx niv0 %dx%d fmt %x/%x env %x"
                " | texmtx %g %g %g %g / %g %g %g %g\n",
                u, mask, GLD_U32(dt, DT_BASE_FORMAT), (int)S16(lv, LV_W), (int)S16(lv, LV_H),
                U16(lv, LV_FORMAT), U16(lv, LV_TYPE),
                U16(g + GS_TEXUNIT0 + u * GS_TEXUNIT_SIZE, TU_ENV_MODE),
                mt[0], mt[5], mt[10], mt[15], mt[12], mt[13], mt[14], mt[3]);
    }
    {
        const float *cur = (const float *)(g + GS_CUR_COLOR);
        unsigned char *V = (unsigned char *)GLD_U32(g, GS_VAO);
        fprintf(f, "   cur %g %g %g %g", cur[0], cur[1], cur[2], cur[3]);
        if (V) {
            const unsigned char *ent = VA_SLOT(V, 2);
            fprintf(f, " | coul tableau %d type %x n %u pas %lu src %08lx",
                    va_enabled(V, 2), U16(ent, 8), U16(ent, 0xa),
                    GLD_U32(ent, 4), GLD_U32(ent, 0));
        }
        fprintf(f, "\n");
    }
    nv = n < 4 ? (int)n : 4;
    for (i = 0; i < nv; i++) {
        const float *s = v + (unsigned long)i * words;
        unsigned long j;
        fprintf(f, "   s%d", i);
        for (j = 0; j < words && j < 16; j++)
            fprintf(f, " %g", s[j]);
        fprintf(f, " |");
        for (j = 0; j < words && j < 16; j++)
            fprintf(f, " %08lx", ((const unsigned long *)s)[j]);
        fprintf(f, "\n");
    }
}

/* +0x54 EndPrimitiveBuffer(ctx, drapeau, mode, n) : GLEngine a écrit n sommets
 * dans le tampon rendu par +0x50. Il n'y a plus rien qui puisse échouer ici. */

/* POMPPC_GL_GEOMCHECK=1 — sonde des sommets aberrants du chemin brut (traînées
   d'UT2004, 22/09/2026) : positions non finies ou > 1e5 (UT2004 ne dépasse pas
   32 768 unités). Appelée à la fin du lot (ce que GLEngine vient d'écrire) et
   au moment de l'envoi de la série (ce que l'hôte va lire) : si l'anomalie
   n'apparaît qu'au second point, quelque chose a écrasé les sommets entre les
   deux. Journal par gl_note (POMPPC_GL_NOTE), 120 lignes au plus. */
static void geom_check(const char *where, PCtx *p, unsigned long mode, unsigned long n,
                       unsigned long words, unsigned long off, unsigned long extra)
{
    static int on = -1;
    static unsigned long left = 120;
    const unsigned char *gc;
    unsigned long i, bad = 0, first = 0;
    float fx = 0, fy = 0, fz = 0;
    if (on < 0) {
        const char *e = getenv("POMPPC_GL_GEOMCHECK");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    if (!on || !left || !words || !n)
        return;
    for (i = 0; i < n; i++) {
        const float *v = (const float *)(G.win + VTX_OFF + off + i * words * 4);
        float x = v[0], y = v[1], z = v[2];
        float w = (QGPU_VF_POS_COUNT(extra & 0xffff) == 4) ? v[3] : 1.0f;
        if (!(x == x && y == y && z == z) || x > 1e5f || x < -1e5f ||
            y > 1e5f || y < -1e5f || z > 1e5f || z < -1e5f ||
            !(w > 0.9f && w < 1.1f)) {
            if (!bad) { first = i; fx = x; fy = y; fz = w; }   /* fz porte w */
            bad++;
        }
    }
    if (!bad)
        return;
    left--;
    gc = p ? gctx_of(p) : 0;
    gl_note("GEOMCHECK %s : %lu/%lu sommets aberrants (x y w), 1er #%lu = %g %g %g | mode %lu words %lu "
            "off %lx pas GLEngine %u extra %lx image %lu\n", where, bad, n, first, fx, fy, fz,
            mode, words, off, gc ? (unsigned)U16(gc, 0x4880) : 0u, extra, G.n_frames);
}
static void geom_end(void *ctx, long flag, short mode, long n)
{
    PCtx *p;
    unsigned long m = (unsigned long)(unsigned short)mode;
    unsigned long words, full;
    int same;

    (void)flag;
    pthread_mutex_lock(&G.mu);
    stream_ready();                     /* F9 : G.hb, G.vtx, G.idx relus après */
    p = find_ctx(ctx);
    if (pomppc_tracing() && p && p->pend_open) {
        unsigned char *gc = gctx_of(p);
        pomppc_log("  géométrie : End(mode %d, n %ld) ouvert %08lx", mode, n,
                   (unsigned long)(G.win + VTX_OFF + p->pend_off));
        if (gc) {
            const float *f = (const float *)(G.win + VTX_OFF + p->pend_off);
            pomppc_log(" gctx 4854=%08lx 4858=%08lx 485c=%08lx 487c=%04x 4880=%04x 4882=%04x"
                       " | s0 %g %g %g %g %g %g %g %g", GLD_U32(gc, 0x4854),
                       GLD_U32(gc, 0x4858), GLD_U32(gc, 0x485c), U16(gc, 0x487c),
                       U16(gc, 0x4880), U16(gc, 0x4882), f[0], f[1], f[2], f[3], f[4],
                       f[5], f[6], f[7]);
        }
        pomppc_log("\n");
    }
    /* F2 : on ne reconnaît que NOTRE tampon. Poser G.pend = 0 ici écrasait
       celui d'un AUTRE contexte, dont la place restait perdue jusqu'au vidage
       suivant et dont le geom_end était ensuite jeté. */
    if (!p || !p->pend_open || p->pend_drop) {
        {   /* GEOMCHECK : End sans pend ouvert = lot PERDU (refermé avant) */
            static int gc_on = -1; static unsigned long seen;
            if (gc_on < 0) { const char *e = getenv("POMPPC_GL_GEOMCHECK"); gc_on = (e && *e && *e != '0') ? 1 : 0; }
            if (gc_on && p && n > 0 && seen++ < 60)
                gl_note("GEOMCHECK End orphelin : mode %lu n %ld (pend_open %d drop %d) image %lu\n",
                        m, n, p->pend_open, p->pend_drop, G.n_frames);
        }
        pend_close(p, 1);
        pthread_mutex_unlock(&G.mu);
        return;
    }
    /* La moitié a changé sous les pieds de GLEngine : impossible par
       construction (flush() n'alterne pas tant qu'un `pend` est ouvert, et
       submit_cur soumet alors en synchrone), mais si cela arrivait, les sommets
       écrits ne sont plus ceux que DRAW_RAW désignerait. On jette. */
    if (G.hb != p->pend_half) {
        no(NO_G_LATE, p->pend_half, G.hb);
        G.n_geomdrop++;
        p->geom_lost = 1;
        pend_close(p, 0);
        pthread_mutex_unlock(&G.mu);
        return;
    }
    if (n <= 0 || (unsigned long)n > p->pend_slots || m > QGPU_PRIM_MODE_POLYGON) {
        if (n > 0) {                    /* GLEngine a écrit hors de ce qu'on offrait */
            no(NO_G_LATE, m, (unsigned long)n);
            G.n_geomdrop++;
            p->geom_lost = 1;
        }
        pend_close(p, 1);               /* la place réservée est rendue */
        pthread_mutex_unlock(&G.mu);
        return;
    }
    words = p->pend_words;
    geom_check("End", p, m, (unsigned long)n, words, p->pend_off, p->pend_fmt);
    {   /* GEOMCHECK : le pas de GLEngine a-t-il changé entre Begin et End ?
           (un attribut apparu en cours de lot réarrangerait le sommet) */
        static int gc_on = -1; static unsigned long seen;
        unsigned char *gc = gctx_of(p);
        if (gc_on < 0) { const char *e = getenv("POMPPC_GL_GEOMCHECK"); gc_on = (e && *e && *e != '0') ? 1 : 0; }
        if (gc_on && gc && seen < 60 &&
            (U16(gc, 0x4880) != words * 4 || U16(gc, 0x487c) != U16(gc, 0x4880) ||
             (unsigned long)n > p->pend_slots)) {
            seen++;
            gl_note("GEOMCHECK pas : End mode %lu n %ld/%lu slots, words %lu (=%lu o), GLEngine 487c=%u 4880=%u 4882=%u, fmt %lx, image %lu\n",
                    m, n, p->pend_slots, words, words * 4, (unsigned)U16(gc, 0x487c),
                    (unsigned)U16(gc, 0x4880), (unsigned)U16(gc, 0x4882), p->pend_fmt, G.n_frames);
        }
    }
    /* On ne rend la queue inutilisée QUE si personne n'a alloué par-dessus
       (autre contexte, autre fil) : sinon on écraserait ses sommets. */
    if (G.vtx == p->pend_off + p->pend_slots * words * 4)
        G.vtx = p->pend_off + (unsigned long)n * words * 4;
    geom_probe(p, "End", m, (unsigned long)n, p->pend_fmt, words,
               (const float *)(G.win + VTX_OFF + p->pend_off));
    /* Les trois conditions de TOUTE fusion : même contexte, même format de
       sommet, sommets CONTIGUS dans la zone partagée. Il n'y a rien à vérifier
       de l'état : tout changement d'état passe par send_cmd → reserve →
       close_raw, donc la série est déjà fermée quand on arrive ici. Et on ne
       fusionne que des lots CONSÉCUTIFS : jamais de réordonnancement, sans quoi
       le mélange et l'égalité de profondeur changeraient l'image. */
    /* PRIMITIVES COMPLÈTES SEULEMENT. GLEngine livre parfois un lot
       GL_TRIANGLES dont n n'est pas multiple de 3 (triangle incomplet en fin
       de lot, lot d'un seul sommet) : mis bout à bout tel quel, le reste
       décalait d'un ou deux sommets TOUS les triangles des lots suivants —
       chaque triangle reliait alors les sommets de ses voisins, en échardes
       à travers le texte du logo d'UT2004. Prouvé le 22/09/2026 par rejeu
       natif d'un vidage (DRAW_RAW de 14 509 sommets, phase 1 dès le sommet 0,
       tests/qgpu_replay.c). Le recollage indexé (tris_emit) ignorait déjà le
       reste ; ici on ne compte que les primitives entières, et la série
       s'arrête après un lot incomplet (G.raw_vend ≠ pend_off suivant). */
    {
        unsigned long unit = (m == QGPU_PRIM_MODE_TRIANGLES) ? 3 :
                             (m == QGPU_PRIM_MODE_QUADS) ? 4 :
                             (m == QGPU_PRIM_MODE_LINES) ? 2 : 1;
        full = (unsigned long)n - (unsigned long)n % unit;
    }
    same = (G.raw_ctx == p && G.raw_fmt == p->pend_fmt && G.raw_words == words &&
            G.raw_vend == p->pend_off &&
            (G.raw_vend - G.raw_start) / (words * 4) + full
                <= GEOM_MAX_MERGE);
    if (same && !G.raw_idx && G.raw_mode == m && mode_mergeable(m)) {
        G.raw_count += full;            /* bout à bout, sans un seul indice */
        G.raw_lots++;
    } else if (same && merge_switch() && !p->pend_wire && mode_tris(m) &&
               (G.raw_idx || mode_tris(G.raw_mode)) &&
               merge_batch(m, (unsigned long)n, words, p->pend_off, p->pend_flat)) {
        /* recollé en TRIANGLES indexés : les sommets n'ont pas bougé */
    } else {
        close_raw();
        G.raw_ctx = p;
        G.raw_start = p->pend_off;
        G.raw_count = full;
        G.raw_fmt = p->pend_fmt;
        G.raw_words = words;
        G.raw_mode = m;
        G.raw_lots = 1;
    }
    /* Fin EXACTE de NOS sommets (primitives entières), pas G.vtx : un autre
       contexte a pu allouer par-dessus, et close_raw() en déduit le `nverts`
       que le cœur relit. */
    G.raw_vend = p->pend_off + full * words * 4;
    G.n_rawverts += full;
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
    pend_close(p, 0);                   /* la place est PRISE : ne rien rendre */
    pthread_mutex_unlock(&G.mu);
}

/* ─────────────────── tableaux de sommets (canal GeForce3) ───────────────────
 *
 * ARRAY=1 pose cfg+0x78 : DrawArrays/DrawElements quittent le déroulement
 * Begin/End et appellent ici, descripteur toujours publié pour glBegin.
 * ARRAY=2 retire cfg+0x11c (GeForce3 strict). On lit GS_VAO, on packe au
 * format DRAW_RAW, et on envoie les indices tels quels.
 * Docs : tableaux-de-sommets.md §2 et §6.
 */

#define VA_GL_BYTE     0x1400
#define VA_GL_UBYTE    0x1401
#define VA_GL_SHORT    0x1402
#define VA_GL_USHORT   0x1403
#define VA_GL_INT      0x1404
#define VA_GL_UINT     0x1405
#define VA_GL_FLOAT    0x1406
#define VA_GL_DOUBLE   0x140A
#define VA_ITYPE_NONE  0x14FF
#define VA_ALLOC_CAP   2048

static unsigned char vb_scratch[VA_ALLOC_CAP * 0x34];

static int va_bpc(unsigned type, int stored)
{
    if (stored > 0)
        return stored;
    switch (type) {
    case VA_GL_BYTE: case VA_GL_UBYTE:     return 1;
    case VA_GL_SHORT: case VA_GL_USHORT:   return 2;
    case VA_GL_INT: case VA_GL_UINT:
    case VA_GL_FLOAT:                      return 4;
    case VA_GL_DOUBLE:                     return 8;
    default:                               return 4;
    }
}

static float va_comp(const unsigned char *p, unsigned type, int norm)
{
    switch (type) {
    case VA_GL_FLOAT:
        return *(const float *)p;
    case VA_GL_DOUBLE: {
        double d = *(const double *)p;
        return (float)d;
    }
    case VA_GL_BYTE: {
        int v = *(const signed char *)p;
        return norm ? (v <= -128 ? -1.0f : v / 127.0f) : (float)v;
    }
    case VA_GL_UBYTE: {
        unsigned v = *p;
        return norm ? v / 255.0f : (float)v;
    }
    case VA_GL_SHORT: {
        int v = *(const short *)p;
        return norm ? (v == -32768 ? -1.0f : v / 32767.0f) : (float)v;
    }
    case VA_GL_USHORT: {
        unsigned v = *(const unsigned short *)p;
        return norm ? v / 65535.0f : (float)v;
    }
    case VA_GL_INT: {
        long v = *(const long *)p;
        return norm ? (v == (long)0x80000000 ? -1.0f : v / 2147483647.0f)
                    : (float)v;
    }
    case VA_GL_UINT: {
        unsigned long v = *(const unsigned long *)p;
        return norm ? v / 4294967295.0f : (float)v;
    }
    default:
        return 0.0f;
    }
}

static const unsigned char *va_src(PCtx *p, const unsigned char *V, int a)
{
    unsigned char *gc = gctx_of(p);
    unsigned long cached, raw, vbo, base;
    raw = GLD_U32(VA_SLOT(V, a), 0);
    vbo = GLD_U32(V, VA_VBO(V, a));
    /* Tableau adossé à un VBO : base de la copie cliente + décalage, TOUJOURS.
       Le cache des pointeurs résolus de GLEngine (H1, gctx+0x48f8) est
       périmé aussi pour un tableau ACTIF : DOOM 3 (24/09/2026, crochet de
       plantage) — position, couleur et génériques 8..11 lus en
       0x190ec000+…, mais texcoord 0 en 0x1b01400c, une autre région, non
       mappée au 10e sommet → SIGSEGV. Les génériques (16..) l'ignoraient déjà. */
    if (vbo) {
        base = GLD_U32((void *)vbo, 0x30);
        /* P5 : copie cliente nulle (Colin McRae, va_probe) → source nulle,
           pas « 0 + décalage » qui passerait va_sources_ok */
        return base ? (const unsigned char *)(base + raw) : 0;
    }
    if (gc && a < 16) {
        cached = GLD_U32(gc, GC_VA_PTRS + 4 * a);
        if (cached)
            return (const unsigned char *)cached;
    }
    return (const unsigned char *)raw;
}

/* Couleur hors tableau. Un sommet sans couleur (0,0,0,0) ne doit pas hériter
 * du matériau : en jeu c'est le vert du terrain, et le panneau du tutoriel
 * — une image du décor — sortait alors vert fluo sur blanc. (1,1,1,1) laisse
 * passer les texels (l'or des menus comme la carte du tutoriel). Une vraie
 * glColor, elle, est conservée. */
static int color_dead(const float *c, int n)
{
    int k;
    for (k = 0; k < n; k++)
        if (c[k] != 0.0f)
            return 0;
    return 1;
}

static void fill_current_color(PCtx *p, float *dst)
{
    const float *cur = (const float *)(gls(p) + GS_CUR_COLOR);
    int k;

    for (k = 0; k < 4; k++)
        dst[k] = cur[k];
    if (color_dead(dst, 4)) {
        dst[0] = dst[1] = dst[2] = dst[3] = 1.0f;
    } else if (dst[3] == 0.0f) {
        dst[3] = 1.0f;
    }
}

static const float *va_current(unsigned char *g, int slot)
{
    switch (slot) {
    case 1:  return (const float *)(g + GS_CUR_NORMAL);
    case 2:  return (const float *)(g + GS_CUR_COLOR);
    case 3:  return (const float *)(g + GS_CUR_FOGCOORD);
    case 4:  return (const float *)(g + GS_CUR_SECCOLOR);
    default:
        if (slot >= 8 && slot < 12)
            return (const float *)(g + GS_CUR_TEXCOORD(slot - 8));
        return 0;
    }
}

/* Écrit dst_n flottants pour l'attribut `slot` du sommet i. last_def est la
 * valeur de bourrage de la dernière composante (1 pour position et texcoord). */
static void va_fetch(float *dst, PCtx *p, const unsigned char *V, int slot,
                     unsigned long i, int dst_n, float last_def)
{
    unsigned char *g = gls(p);
    const unsigned char *ent, *src;
    const float *cur;
    unsigned type;
    int k, src_n, bpc, stride, norm;

    if (slot == 2 && (!V || !va_enabled(V, slot))) {
        fill_current_color(p, dst);
        return;
    }
    if (!V || !va_enabled(V, slot)) {
        cur = va_current(g, slot);
        for (k = 0; k < dst_n; k++)
            dst[k] = cur && k < 4 ? cur[k] : (k == dst_n - 1 ? last_def : 0.0f);
        return;
    }
    ent = VA_SLOT(V, slot);
    type = U16(ent, 8);
    src_n = U16(ent, 0xa);
    norm = ent[0xd] || (type & 0x8000);     /* v16 : bit 0x8000 = normalisé (générique 4ub) */
    type &= 0x7fff;
    bpc = va_bpc(type, ent[0xc]);
    stride = (int)GLD_U32(ent, 4);
    if ((slot == 1 || slot == 2) && type != VA_GL_FLOAT && type != VA_GL_DOUBLE)
        norm = 1;
    src = va_src(p, V, slot);
    if (!src || stride <= 0 || bpc <= 0) {
        if (slot == 2) {
            fill_current_color(p, dst);
            return;
        }
        cur = va_current(g, slot);
        for (k = 0; k < dst_n; k++)
            dst[k] = cur && k < 4 ? cur[k] : (k == dst_n - 1 ? last_def : 0.0f);
        return;
    }
    src += i * (unsigned)stride;
    for (k = 0; k < dst_n; k++) {
        float v;
        if (k < src_n) {
            v = va_comp(src + k * bpc, type, norm);
            if (!(v > -1e9f && v < 1e9f))
                v = (v > 0.0f) ? 1e9f : (v < 0.0f) ? -1e9f : 0.0f;
        } else {
            v = (k == dst_n - 1) ? last_def : 0.0f;
        }
        dst[k] = v;
    }
    /* Tableau de couleurs tout à zéro : WC3 en laisse parfois un, inerte, et
       pose la vraie teinte par glColor / ColorMat. Sans ça, MODULATE × alpha 0
       efface les glyphes. */
    if (slot == 2 && dst_n >= 4 &&
        dst[0] == 0.0f && dst[1] == 0.0f && dst[2] == 0.0f && dst[3] == 0.0f)
        fill_current_color(p, dst);
}

/* Colin McRae (23/09/2026, vidage rejoué) : des maillages ENTIERS arrivaient
 * à l'hôte avec tous leurs sommets égaux aux valeurs courantes — position
 * (0,0,0,1), couleur blanche, texcoord (0,0,0,1) —, parce que va_fetch
 * substitue la valeur courante quand la source d'un tableau ACTIF est nulle
 * ou son pas invalide (VBO dont la copie cliente à +0x30 est nulle, tableau
 * paginé par le memory plugin…). Un maillage effondré en un point n'est jamais
 * ce que l'application demande : on REFUSE le dessin, GLEngine le transforme
 * lui-même (image juste, comme POMPPC_GL_GEOM=0), et on consigne UNE FOIS le
 * descripteur complet pour retrouver d'où la source aurait dû venir. */
static const unsigned long va_fmt_bit[9] = {
    0, QGPU_VF_NORMAL, QGPU_VF_COLOR, QGPU_VF_SEC_COLOR, QGPU_VF_FOG,
    QGPU_VF_TEX(0), QGPU_VF_TEX(1), QGPU_VF_TEX(2), QGPU_VF_TEX(3)
};
static const int va_fmt_slot[9] = { 0, 1, 2, 4, 3, 8, 9, 10, 11 };

static void va_probe(PCtx *p, const unsigned char *V, unsigned long fmt,
                     const char *tag, int bad)
{
    unsigned char *gc = gctx_of(p);
    int i, k;
    gl_note("SONDE tableaux (%s) : V %p en_hi %08lx en_lo %08lx fmt %lx fautif %d image %lu\n",
            tag, (void *)V, GLD_U32(V, VA_EN_HI), GLD_U32(V, VA_EN_LO), fmt, bad, G.n_frames);
    for (i = 0; i < 9; i++) {
        int s = va_fmt_slot[i];
        const unsigned char *ent = VA_SLOT(V, s);
        unsigned long vbo = GLD_U32(V, VA_VBO(V, s));
        PBuf *b = buf_from_vbo(vbo);
        gl_note("  slot %2d en %d ptr %08lx pas %ld type %04x n %u bpc %u norm %u vbo %08lx base %08lx cache %08lx pbuf qid %ld dirty %d\n",
                s, va_enabled(V, s), GLD_U32(ent, 0), (long)GLD_U32(ent, 4),
                U16(ent, 8), U16(ent, 0xa), ent[0xc], ent[0xd], vbo,
                vbo ? GLD_U32((void *)vbo, 0x30) : 0UL,
                gc ? GLD_U32(gc, GC_VA_PTRS + 4 * s) : 0UL,
                b ? b->qid : -2L, b ? b->dirty : -1);
        if (vbo && (s == 0 || s == bad)) {
            char hb[200]; int a = 0;
            for (k = 0; k < 20 && a < (int)sizeof(hb) - 12; k++)
                a += snprintf(hb + a, sizeof(hb) - a, " %lx", GLD_U32((void *)vbo, 4 * k));
            gl_note("  vbo %08lx :%s\n", vbo, hb);
        }
    }
    /* attributs génériques 0..7 (emplacements 16..23) : Colin McRae en course */
    for (i = 16; i < 24; i++) {
        const unsigned char *ent = VA_SLOT(V, i);
        if (!va_enabled(V, i) && !GLD_U32(ent, 0))
            continue;
        gl_note("  générique %d (slot %d) en %d ptr %08lx pas %ld type %04x n %u bpc %u norm %u sig %08lx vbo %08lx\n",
                i - 16, i, va_enabled(V, i), GLD_U32(ent, 0), (long)GLD_U32(ent, 4),
                U16(ent, 8), U16(ent, 0xa), ent[0xc], ent[0xd], GLD_U32(ent, 0x14),
                GLD_U32(V, VA_VBO(V, i)));
    }
}

static int va_sources_ok(PCtx *p, const unsigned char *V, unsigned long fmt)
{
    static int probed_first, probed_bad;
    int i, bad = -1;
    for (i = 0; i < 9; i++) {
        int s = va_fmt_slot[i];
        const unsigned char *ent;
        if ((i && !(fmt & va_fmt_bit[i])) || !va_enabled(V, s))
            continue;
        ent = VA_SLOT(V, s);
        if (!va_src(p, V, s) || (int)GLD_U32(ent, 4) <= 0 ||
            va_bpc(U16(ent, 8), ent[0xc]) <= 0) {
            bad = s;
            break;
        }
    }
    /* v16 : sources des génériques portés par le format, et du générique 0
       quand c'est lui la position */
    if (bad < 0 && G.prog && p->vp_on) {
        for (i = 0; i < QGPU_VF_GEN_MAX; i++) {
            const unsigned char *ent;
            if (i ? !(fmt & QGPU_VF_GEN(i)) : (va_enabled(V, 0) || !va_enabled(V, 16)))
                continue;
            if (!va_enabled(V, 16 + i))
                continue;
            ent = VA_SLOT(V, 16 + i);
            if (!va_src(p, V, 16 + i) || (int)GLD_U32(ent, 4) <= 0 ||
                va_bpc(U16(ent, 8) & 0x7fff, ent[0xc]) <= 0) {
                bad = 16 + i;
                break;
            }
        }
    }
    if (!probed_first) {
        probed_first = 1;
        va_probe(p, V, fmt, "premier dessin", bad);
    }
    if (bad < 0)
        return 1;
    if (!probed_bad) {
        probed_bad = 1;
        va_probe(p, V, fmt, "source nulle", bad);
    }
    return 0;
}

/* 24/09/2026 — empaqueteur PLANIFIÉ. Profil `sample` de DOOM 3 en jeu :
 * va_fetch = 23 % du fil principal (relecture du descripteur, de la source
 * VBO et des valeurs courantes à CHAQUE sommet et CHAQUE attribut). Le plan
 * prend ces décisions une fois par dessin (mêmes règles que va_fetch, y
 * compris la couleur morte de WC3), la boucle par sommet ne fait plus que
 * copier et convertir, avec un chemin direct pour les flottants. */
typedef struct VaAttr {
    const unsigned char *src;   /* base du tableau (sommet 0) ; 0 = constante */
    long           stride;
    int            bpc, src_n, dst_n, norm;
    unsigned       type;
    int            is_color;    /* emplacement 2 : couleur morte → courante */
    float          cur[4];      /* valeur constante (dst_n flottants) */
    float          last_def;
    int            slot;        /* I5 : emplacement lu (clé, propreté des VBO) ;
                                   en DERNIER : le crochet lit les champs 0..6 */
} VaAttr;
typedef struct VaPlan {
    VaAttr a[QGPU_VF_GEN_MAX + QGPU_MAX_UNITS + 5];
    int    n;
} VaPlan;

static void va_plan_attr(VaAttr *at, PCtx *p, const unsigned char *V, int slot,
                         int dst_n, float last_def)
{
    unsigned char *g = gls(p);
    const unsigned char *ent, *src;
    const float *cur;
    int k;

    memset(at, 0, sizeof(*at));
    at->slot = slot;
    at->dst_n = dst_n;
    at->last_def = last_def;
    at->is_color = (slot == 2);
    if (!V || !va_enabled(V, slot))
        goto constant;
    ent = VA_SLOT(V, slot);
    at->type = U16(ent, 8);
    at->src_n = U16(ent, 0xa);
    at->norm = ent[0xd] || (at->type & 0x8000);
    at->type &= 0x7fff;
    at->bpc = va_bpc(at->type, ent[0xc]);
    at->stride = (long)GLD_U32(ent, 4);
    if ((slot == 1 || slot == 2) && at->type != VA_GL_FLOAT && at->type != VA_GL_DOUBLE)
        at->norm = 1;
    src = va_src(p, V, slot);
    if (!src || at->stride <= 0 || at->bpc <= 0)
        goto constant;
    at->src = src;
    if (at->src_n > dst_n)
        at->src_n = dst_n;
    if (at->is_color)
        fill_current_color(p, at->cur);             /* pour la couleur morte */
    return;
constant:
    at->src = 0;
    if (slot == 2) {
        fill_current_color(p, at->cur);
        return;
    }
    cur = va_current(g, slot);
    for (k = 0; k < dst_n; k++)
        at->cur[k] = cur && k < 4 ? cur[k] : (k == dst_n - 1 ? last_def : 0.0f);
}

/* Génériques à taille déclarée : pour chaque générique k >= 1 du format dont
 * le tableau est actif et lisible, sa taille quand elle vaut 1..3 (4 est le
 * code 0). Mêmes conditions que va_plan_attr : un tableau que le plan
 * remplacerait par la valeur courante reste à 4 composantes. 0 sans
 * G.gensizes (tout à 4, le fil d'avant). */
static unsigned long va_gen_sizes(PCtx *p, const unsigned char *V, unsigned long fmt)
{
    unsigned long gs = 0, n;
    const unsigned char *ent;
    int k;

    if (!G.gensizes || !V || !(fmt & QGPU_VF_GEN_MASK))
        return 0;
    for (k = 1; k < QGPU_VF_GEN_MAX; k++) {
        if (!(fmt & QGPU_VF_GEN(k)) || !va_enabled(V, 16 + k))
            continue;
        ent = VA_SLOT(V, 16 + k);
        n = U16(ent, 0xa);
        if (n < 1 || n > 3 || (long)GLD_U32(ent, 4) <= 0 ||
            va_bpc(U16(ent, 8) & 0x7fff, ent[0xc]) <= 0 || !va_src(p, V, 16 + k))
            continue;
        gs |= QGPU_GS(k, n);
    }
    return gs;
}

static void va_plan_build(VaPlan *pl, PCtx *p, const unsigned char *V, unsigned long fmt,
                          unsigned long gs)
{
    int n = QGPU_VF_POS_COUNT(fmt), u;
    pl->n = 0;
    if (G.prog && p->vp_on && !va_enabled(V, 0) && va_enabled(V, 16))
        va_plan_attr(&pl->a[pl->n++], p, V, 16, n, 1.0f);
    else
        va_plan_attr(&pl->a[pl->n++], p, V, 0, n, 1.0f);
    if (fmt & QGPU_VF_NORMAL)    va_plan_attr(&pl->a[pl->n++], p, V, 1, 3, 0.0f);
    if (fmt & QGPU_VF_COLOR)     va_plan_attr(&pl->a[pl->n++], p, V, 2, 4, 1.0f);
    if (fmt & QGPU_VF_SEC_COLOR) va_plan_attr(&pl->a[pl->n++], p, V, 4, 3, 0.0f);
    if (fmt & QGPU_VF_FOG)       va_plan_attr(&pl->a[pl->n++], p, V, 3, 1, 0.0f);
    for (u = 0; u < QGPU_MAX_UNITS; u++)
        if (fmt & QGPU_VF_TEX(u))
            va_plan_attr(&pl->a[pl->n++], p, V, 8 + u, 4, 1.0f);
    /* génériques : à la taille déclarée (gs = 0 : 4 flottants) — le device
       complète y = 0, z = 0, w = 1 comme OpenGL */
    for (u = 1; u < QGPU_VF_GEN_MAX; u++)
        if (fmt & QGPU_VF_GEN(u))
            va_plan_attr(&pl->a[pl->n++], p, V, 16 + u, QGPU_GS_COUNT(gs, u), 1.0f);
}

static void va_pack_planned(float *dst, const VaPlan *pl, unsigned long i)
{
    int j, k;
    for (j = 0; j < pl->n; j++) {
        const VaAttr *at = &pl->a[j];
        int dn = at->dst_n;
        if (!at->src) {
            for (k = 0; k < dn; k++)
                dst[k] = at->cur[k];
            dst += dn;
            continue;
        }
        {
            const unsigned char *src = at->src + (long)i * at->stride;
            if (at->type == VA_GL_FLOAT) {
                const float *f = (const float *)src;
                for (k = 0; k < at->src_n; k++) {
                    float v = f[k];
                    if (!(v > -1e9f && v < 1e9f))
                        v = (v > 0.0f) ? 1e9f : (v < 0.0f) ? -1e9f : 0.0f;
                    dst[k] = v;
                }
            } else {
                for (k = 0; k < at->src_n; k++) {
                    float v = va_comp(src + k * at->bpc, at->type, at->norm);
                    if (!(v > -1e9f && v < 1e9f))
                        v = (v > 0.0f) ? 1e9f : (v < 0.0f) ? -1e9f : 0.0f;
                    dst[k] = v;
                }
            }
            for (k = at->src_n; k < dn; k++)
                dst[k] = (k == dn - 1) ? at->last_def : 0.0f;
            if (at->is_color && dn >= 4 &&
                dst[0] == 0.0f && dst[1] == 0.0f && dst[2] == 0.0f && dst[3] == 0.0f)
                for (k = 0; k < 4; k++)
                    dst[k] = at->cur[k];
        }
        dst += dn;
    }
}

static void __attribute__((unused)) va_pack_vertex(float *dst, PCtx *p, const unsigned char *V,
                           unsigned long fmt, unsigned long i)
{
    int n, u;
    n = QGPU_VF_POS_COUNT(fmt);
    /* v16 : sous programme, le générique 0 est la position (aliasing ARB) */
    if (G.prog && p->vp_on && !va_enabled(V, 0) && va_enabled(V, 16))
        va_fetch(dst, p, V, 16, i, n, 1.0f);
    else
        va_fetch(dst, p, V, 0, i, n, 1.0f);
    dst += n;
    if (fmt & QGPU_VF_NORMAL) {
        va_fetch(dst, p, V, 1, i, 3, 0.0f);
        dst += 3;
    }
    if (fmt & QGPU_VF_COLOR) {
        va_fetch(dst, p, V, 2, i, 4, 1.0f);
        dst += 4;
    }
    if (fmt & QGPU_VF_SEC_COLOR) {
        va_fetch(dst, p, V, 4, i, 3, 0.0f);
        dst += 3;
    }
    if (fmt & QGPU_VF_FOG) {
        va_fetch(dst, p, V, 3, i, 1, 0.0f);
        dst += 1;
    }
    for (u = 0; u < QGPU_MAX_UNITS; u++)
        if (fmt & QGPU_VF_TEX(u)) {
            va_fetch(dst, p, V, 8 + u, i, 4, 1.0f);
            dst += 4;
        }
    /* v16 : attributs génériques 1..7 (emplacements 17..23), 4 flottants */
    for (u = 1; u < QGPU_VF_GEN_MAX; u++)
        if (fmt & QGPU_VF_GEN(u)) {
            va_fetch(dst, p, V, 16 + u, i, 4, 1.0f);
            dst += 4;
        }
}

static int va_scan_idx(const void *idx, unsigned long itype, long count,
                       unsigned long *minv, unsigned long *maxv)
{
    unsigned long mn = ~0UL, mx = 0;
    long i;
    if (count <= 0 || !idx)
        return 0;
    if (itype == VA_GL_UBYTE) {
        const unsigned char *p = idx;
        for (i = 0; i < count; i++) {
            unsigned long v = p[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    } else if (itype == VA_GL_USHORT) {
        const unsigned short *p = idx;
        for (i = 0; i < count; i++) {
            unsigned long v = p[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    } else if (itype == VA_GL_UINT) {
        const unsigned long *p = idx;
        for (i = 0; i < count; i++) {
            unsigned long v = p[i];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
    } else {
        return 0;
    }
    *minv = mn;
    *maxv = mx;
    return 1;
}

static unsigned long va_idx_at(const void *idx, unsigned long itype, long i)
{
    if (itype == VA_GL_UBYTE)
        return ((const unsigned char *)idx)[i];
    if (itype == VA_GL_USHORT)
        return ((const unsigned short *)idx)[i];
    return ((const unsigned long *)idx)[i];
}

/* 24/09/2026 — DOOM 3 et Prey : un lot indexé de triangles dont la plage
 * [vmin, vmax] contient des sommets fous (cache de sommets d'idTech4 : les
 * trous entre les sommets référencés sont de la mémoire quelconque) partait
 * ENTIER chez Apple (« raw:arrays 30/48 », 5 fois par image, chaque fois une
 * relecture et un retéléversement de l'écran). Un sommet non référencé ne
 * dessine rien ; un sommet fou référencé gâche son triangle seulement : on
 * copie les indices en sautant les triangles qui touchent un sommet marqué.
 * Rend le nombre d'indices écrits. */
static long va_copy_idx_tri(void *dst, int use32, const void *idx, unsigned long itype,
                            long count, unsigned long base, const unsigned char *bad)
{
    long i, o = 0;
    for (i = 0; i + 2 < count; i += 3) {
        unsigned long a = va_idx_at(idx, itype, i) - base;
        unsigned long b = va_idx_at(idx, itype, i + 1) - base;
        unsigned long c = va_idx_at(idx, itype, i + 2) - base;
        if (bad[a] || bad[b] || bad[c])
            continue;
        if (use32) {
            ((unsigned long *)dst)[o] = a; ((unsigned long *)dst)[o + 1] = b;
            ((unsigned long *)dst)[o + 2] = c;
        } else {
            ((unsigned short *)dst)[o] = (unsigned short)a;
            ((unsigned short *)dst)[o + 1] = (unsigned short)b;
            ((unsigned short *)dst)[o + 2] = (unsigned short)c;
        }
        o += 3;
    }
    return o;
}

static void va_copy_idx16(unsigned short *dst, const void *idx,
                          unsigned long itype, long count, unsigned long base)
{
    long i;
    if (itype == VA_GL_UBYTE) {
        const unsigned char *p = idx;
        for (i = 0; i < count; i++)
            dst[i] = (unsigned short)(p[i] - base);
    } else if (itype == VA_GL_USHORT) {
        const unsigned short *p = idx;
        for (i = 0; i < count; i++)
            dst[i] = (unsigned short)(p[i] - base);
    } else {
        const unsigned long *p = idx;
        for (i = 0; i < count; i++)
            dst[i] = (unsigned short)(p[i] - base);
    }
}

static void va_copy_idx32(unsigned long *dst, const void *idx,
                          unsigned long itype, long count, unsigned long base)
{
    long i;
    if (itype == VA_GL_UBYTE) {
        const unsigned char *p = idx;
        for (i = 0; i < count; i++)
            dst[i] = p[i] - base;
    } else if (itype == VA_GL_USHORT) {
        const unsigned short *p = idx;
        for (i = 0; i < count; i++)
            dst[i] = p[i] - base;
    } else {
        const unsigned long *p = idx;
        for (i = 0; i < count; i++)
            dst[i] = p[i] - base;
    }
}

static void va_count_prims(unsigned long m, unsigned long n)
{
    switch (m) {
    case QGPU_PRIM_MODE_TRIANGLES:      G.n_tris += n / 3; break;
    case QGPU_PRIM_MODE_TRIANGLE_STRIP:
    case QGPU_PRIM_MODE_TRIANGLE_FAN:
    case QGPU_PRIM_MODE_POLYGON:        G.n_tris += n > 2 ? n - 2 : 0; break;
    case QGPU_PRIM_MODE_QUADS:          G.n_tris += (n / 4) * 2; break;
    case QGPU_PRIM_MODE_QUAD_STRIP:     G.n_tris += n > 3 ? (n / 2 - 1) * 2 : 0; break;
    case QGPU_PRIM_MODE_LINES:          G.n_lines += n / 2; break;
    case QGPU_PRIM_MODE_LINE_STRIP:     G.n_lines += n > 1 ? n - 1 : 0; break;
    case QGPU_PRIM_MODE_LINE_LOOP:      G.n_lines += n; break;
    default:                            G.n_points += n; break;
    }
}

/* P13 — CLÉ DE RÉUTILISATION D'UN TAMPON HÔTE (v14).
 *
 * Elle ne tenait compte que de (fmt, vmin, nverts). Or QGPU_VF_COLOR est
 * TOUJOURS posé, et la couleur vient de glColor quand le tableau de couleurs
 * est inactif : le même VBO redessiné après un glColor ressortait avec la
 * couleur du PREMIER passage. De même, deux maillages rangés dans le même VBO
 * à des pointeurs ou des pas différents, de même compte et de même format, se
 * mélangeaient. On récapitule donc tout ce que va_pack_vertex lit : par
 * attribut, le pointeur résolu, le pas, le type, le nombre de composantes et
 * la normalisation quand le tableau est actif ; les bits IEEE de la valeur
 * COURANTE quand il ne l'est pas.
 *
 * C'est une empreinte 32 bits, pas une égalité : une collision redessinerait
 * un maillage avec l'emballage du précédent. POMPPC_GL_VBO=0 coupe le cache.
 *
 * I5 (relecture du 24/09) : la clé se dérive du PLAN d'empaquetage
 * (va_plan_build), donc de tout ce qui est réellement empaqueté — générique 0
 * quand il tient lieu de position, texcoords 4..7, génériques 1..15 du
 * format — et non plus d'une liste fixe d'emplacements conventionnels. */
static unsigned long va_pack_key(const VaPlan *pl, unsigned long fmt)
{
    unsigned long k = fmt ^ 0x9e3779b9UL;
    int j, c;

    for (j = 0; j < pl->n; j++) {
        const VaAttr *at = &pl->a[j];
        k = k * 33UL + (unsigned long)at->slot;
        k = k * 33UL + (unsigned long)at->dst_n;
        if (at->src) {
            k = k * 33UL + (unsigned long)at->src;
            k = k * 33UL + (unsigned long)at->stride;
            k = k * 33UL + at->type;
            k = k * 33UL + (unsigned long)at->src_n;
            k = k * 33UL + (unsigned long)at->bpc;
            k = k * 33UL + (unsigned long)at->norm;
        }
        /* valeur constante, ou courante de la couleur morte (is_color) ;
           des zéros pour un tableau ordinaire */
        for (c = 0; c < at->dst_n && c < 4; c++)
            k = k * 33UL + fbits(at->cur[c]);
    }
    return k;
}

/* 0 = pas de cache hôte (tableau client). 1 = réutilisable si le paquet match.
 * 2 = VBO, mais il faut re-emballer (FlushBuffer ou premier dessin).
 * I5 (relecture du 24/09) : position = l'emplacement que le plan a
 * réellement lu (16 si le générique 0 la remplace), puis tout attribut du
 * plan lu dans un tableau — génériques et unités 4..7 compris. */
static int va_host_ready(const unsigned char *V, const VaPlan *pl, PBuf **out)
{
    PBuf *pos, *b;
    unsigned long vbo;
    int j, dirty = 0;

    *out = 0;
    if (!G.hostbuf || !V || pl->n < 1)
        return 0;
    vbo = GLD_U32(V, VA_VBO(V, pl->a[0].slot));
    pos = buf_from_vbo(vbo);
    if (!pos)
        return 0;
    if (pos->dirty)
        dirty = 1;
    for (j = 1; j < pl->n; j++) {
        if (!pl->a[j].src)
            continue;                   /* constante : aucun tableau lu */
        vbo = GLD_U32(V, VA_VBO(V, pl->a[j].slot));
        if (!vbo)
            return 0;
        b = buf_from_vbo(vbo);
        if (!b)
            return 0;
        if (b->dirty)
            dirty = 1;
    }
    *out = pos;
    return dirty ? 2 : 1;
}

static void va_host_clean(const unsigned char *V, const VaPlan *pl, PBuf *pos)
{
    PBuf *b;
    int j;
    if (pos)
        pos->dirty = 0;
    if (!V)
        return;
    for (j = 1; j < pl->n; j++) {
        if (!pl->a[j].src)
            continue;
        b = buf_from_vbo(GLD_U32(V, VA_VBO(V, pl->a[j].slot)));
        if (b)
            b->dirty = 0;
    }
}

/* 0 = la commande n'a PAS été écrite (place manquante) : le dessin est perdu,
 * mais rien n'est corrompu. On ne peut pas vider le flux ici — `vtx_off` et
 * `ioff` désignent la moitié COURANTE, un vidage les ferait pointer ailleurs. */
static int emit_draw_client(PCtx *p, unsigned long mode, unsigned long nidx,
                            unsigned long nverts, unsigned long fmt, unsigned long gs,
                            unsigned long vtx_off, unsigned long ioff,
                            unsigned long itype_h, PBuf *hb)
{
    unsigned long *c;
    unsigned long need = (hb && hb->qid >= 0) ? QGPU_LEN_DRAW_RAW_BUF
                                              : QGPU_LEN_DRAW_RAW;
    int put_gs;
    close_raw();
    /* tailles des génériques : juste avant le dessin (close_raw a écrit la
       série Begin/End en attente, qui les lisait à 4) */
    put_gs = gs_stale(p, fmt, gs);
    if (put_gs)
        need += QGPU_LEN_SET_STATE;
    /* Mineur §2 : on écrivait sans vérifier, sur la seule marge résiduelle de
       reserve() — un mot au pire une fois close_raw() passé. */
    if (G.ncmd + need + QGPU_LEN_CTX > CMD_WORDS)
        return 0;
    if (G.bound != p) {
        c = G.cmd + G.ncmd;
        c[0] = QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX);
        c[1] = p->qctx;
        G.ncmd += QGPU_LEN_CTX;
        G.bound = p;
    }
    if (put_gs) {
        gs_put(G.cmd + G.ncmd, p, gs);
        G.ncmd += QGPU_LEN_SET_STATE;
    }
    c = G.cmd + G.ncmd;
    if (hb && hb->qid >= 0) {
        c[0] = QGPU_CMD_HDR(QGPU_OP_DRAW_RAW_BUF, QGPU_LEN_DRAW_RAW_BUF);
        c[1] = mode;
        c[2] = nidx ? nidx : nverts;
        c[3] = (unsigned long)hb->qid;
        c[4] = 0;
        c[5] = 0;
        c[6] = fmt;
        c[7] = QGPU_BUF_SHMEM;
        c[8] = nidx ? G.base + ioff : 0;
        c[9] = itype_h;
        c[10] = 0;
        c[11] = nverts;
        G.ncmd += QGPU_LEN_DRAW_RAW_BUF;
    } else {
        {   /* 23/09/2026 : voir la note jumelle dans close_raw */
            static unsigned long said;
            if (said < 3) {
                const float *v0 = (const float *)(G.win + VTX_OFF + vtx_off);
                said++;
                gl_note("DRAW_RAW tableaux #%lu image %lu : mode %lu n %lu fmt %lx v0 %g %g %g %g\n",
                        said, G.n_frames, mode, nverts, fmt, v0[0], v0[1], v0[2], v0[3]);
            }
        }
        c[0] = QGPU_CMD_HDR(QGPU_OP_DRAW_RAW, QGPU_LEN_DRAW_RAW);
        c[1] = mode;
        c[2] = nidx ? nidx : nverts;
        c[3] = G.base + VTX_OFF + vtx_off;
        c[4] = 0;
        c[5] = fmt;
        c[6] = nidx ? G.base + ioff : 0;
        c[7] = itype_h;
        c[8] = 0;
        c[9] = nverts;
        G.ncmd += QGPU_LEN_DRAW_RAW;
    }
    return 1;
}

static int quads_axis_aligned(const float *v, unsigned long n, unsigned long words)
{
    unsigned long i;
    if (!v || n < 4 || (n % 4) != 0 || words < 2)
        return 0;
    for (i = 0; i < n; i += 4) {
        const float *a = v + (i + 0) * words;
        const float *b = v + (i + 1) * words;
        const float *c = v + (i + 2) * words;
        const float *d = v + (i + 3) * words;
        float dx1 = b[0] - a[0], dy1 = b[1] - a[1];
        float dx2 = c[0] - b[0], dy2 = c[1] - b[1];
        float dy3 = d[1] - c[1];
        float dx4 = a[0] - d[0];
        if (!(dy1 > -1e-3f && dy1 < 1e-3f && dx2 > -1e-3f && dx2 < 1e-3f &&
              dy3 > -1e-3f && dy3 < 1e-3f && dx4 > -1e-3f && dx4 < 1e-3f))
            return 0;
        if ((dx1 > -1e-3f && dx1 < 1e-3f) || (dy2 > -1e-3f && dy2 < 1e-3f))
            return 0;
    }
    return 1;
}

/* ── v18 : DRAW_NATIVE — tableaux adossés à des VBO, sans empaquetage ──────
 *
 * DOOM 3 / Prey : 79 000 sommets idDrawVert (60 octets, entrelacés) par image
 * passaient par va_pack_planned (conversion en flottants qgpu, un sommet à la
 * fois, sur un G4 émulé), puis par BUF_SUBDATA. Ici, chaque VBO lu a sur
 * l'hôte un MIROIR BRUT (tranche d'une réserve, cf. raw_ensure), tenu à jour
 * par recopie memcpy des seules plages que gldFlushBuffer a salies ET que le
 * dessin lit ; le dessin ne transporte que des descripteurs (code, tampon,
 * offset, pas, type, taille|normalisé), l'hôte convertit.
 *
 * Mêmes attributs que l'empaqueteur : le PLAN (va_plan_build) — donc, sous
 * programme, seulement ce que le texte lit, et la position lue au générique 0
 * quand c'est lui qui la porte (décrite en code 0). Un attribut du plan SANS
 * tableau (valeur constante) n'est pas décrit : sa valeur, celle que
 * l'empaqueteur aurait recopiée (couleur morte → blanche…), part en
 * SET_CURRENT. Seules la normale, la couleur et les coordonnées de texture
 * s'y prêtent ; une position, un brouillard, une secondaire ou un générique
 * constants → empaquetage (l'hôte déduit de leur PRÉSENCE l'état GL :
 * GL_FOG_COORDINATE, GL_COLOR_SUM).
 *
 * Écarts assumés avec l'empaquetage, à la charge de l'hôte ou sans objet :
 *   — pas de filtre des sommets fous (raw_scan_nan / va_copy_idx_tri) : les
 *     trous du cache d'idTech4 dans [vmin, vmax] arrivent tels quels, l'hôte
 *     ASSAINIT à la conversion (NaN, infini, |v| ≥ 1e9) au lieu de refuser ;
 *   — pas de « couleur morte » par sommet (0,0,0,0 → couleur courante) : un
 *     rustine de WC3, dont les tableaux sont clients (jamais natifs) ;
 *   — pas de glyphes TRIANGLES → QUADS (WC3 encore, tableaux clients) ;
 *   — la clé QGPU_SK_GEN_SIZES n'est ni posée ni lue : la taille de chaque
 *     générique est dans son descripteur (la clé reste ce qu'elle était pour
 *     DRAW_RAW / DRAW_RAW_BUF, gs_stale la tient à jour là-bas).
 *
 * Rend -1 : pas pour ce chemin (l'appelant empaquette), 1 : dessiné. Tout ce
 * qui lit la mémoire de l'application (recopie, indices) tourne sous la garde
 * pack_jmp de geom_draw_client. */
typedef struct NatBuf {
    PBuf           *b;
    unsigned long   base, size, lo, hi; /* plage que le dessin lit */
} NatBuf;

static int nat_buf_add(NatBuf *nb, int *n, PBuf *b, unsigned long base,
                       unsigned long size, unsigned long lo, unsigned long hi)
{
    int i;
    for (i = 0; i < *n; i++)
        if (nb[i].b == b) {
            if (lo < nb[i].lo) nb[i].lo = lo;
            if (hi > nb[i].hi) nb[i].hi = hi;
            return i;
        }
    if (*n >= QGPU_NATTR_MAX + 1)
        return -1;
    nb[*n].b = b;
    nb[*n].base = base;
    nb[*n].size = size;
    nb[*n].lo = lo;
    nb[*n].hi = hi;
    return (*n)++;
}

static int nat_type_ok(unsigned type, int bpc)
{
    switch (type) {
    case VA_GL_BYTE: case VA_GL_UBYTE:     return bpc == 1;
    case VA_GL_SHORT: case VA_GL_USHORT:   return bpc == 2;
    case VA_GL_INT: case VA_GL_UINT:
    case VA_GL_FLOAT:                      return bpc == 4;
    case VA_GL_DOUBLE:                     return bpc == 8;
    default:                               return 0;
    }
}

/* Code de descripteur de l'entrée j du plan (emplacement GLEngine `slot`). */
static int nat_code(int j, int slot)
{
    if (j == 0)
        return QGPU_NATTR_POSITION;     /* emplacement 0, ou générique 0 (alias) */
    switch (slot) {
    case 1: return QGPU_NATTR_NORMAL;
    case 2: return QGPU_NATTR_COLOR;
    case 3: return QGPU_NATTR_FOG;
    case 4: return QGPU_NATTR_SEC_COLOR;
    default: break;
    }
    if (slot >= 8 && slot < 8 + QGPU_MAX_UNITS)
        return QGPU_NATTR_TEX(slot - 8);
    if (slot > 16 && slot < 16 + QGPU_VF_GEN_MAX)
        return QGPU_NATTR_GEN(slot - 16);
    return -1;
}

/* Valeur courante (QGPU_CUR_*) qui remplace un attribut constant, ou -1. */
static int nat_cur_of(int slot, int *n)
{
    if (slot == 1) {
        *n = 3;
        return QGPU_CUR_NORMAL;
    }
    if (slot == 2) {
        *n = 4;
        return QGPU_CUR_COLOR;
    }
    if (slot >= 8 && slot < 8 + QGPU_MAX_UNITS) {
        *n = 4;
        return QGPU_CUR_TEXCOORD0 + (slot - 8);
    }
    return -1;
}

static int geom_draw_native(PCtx *p, const unsigned char *V, const VaPlan *pl,
                            unsigned long mode, long first, long count,
                            unsigned long itype, const void *indices,
                            unsigned long vmin, unsigned long vmax, unsigned long nidx)
{
    NatBuf nb[QGPU_NATTR_MAX + 1];
    unsigned long d[QGPU_NATTR_MAX][QGPU_NATTR_WORDS];
    int dbuf[QGPU_NATTR_MAX];
    int nd = 0, nn = 0, j, k, ib = -1, seen_vbo = 0, cn, w;
    unsigned long isz = 0, ioff_b = 0, itype_h = QGPU_IDX_NONE, ibuf = QGPU_BUF_SHMEM;
    unsigned long ioff = 0, aoff, dbytes, ibytes, nverts, *c, *dst;
    unsigned long a[QGPU_LEN_SET_CURRENT - 1];

    if (!G.native || !V || !pl || pl->n < 1 || pl->n > QGPU_NATTR_MAX)
        return -1;
    nverts = vmax - vmin + 1;

    /* 1. Descripteurs : chaque attribut lu vient d'un VBO, dans ses bornes */
    for (j = 0; j < pl->n; j++) {
        const VaAttr *at = &pl->a[j];
        unsigned long vbo, base, size, off, stride, end;
        PBuf *b;
        int code;
        if (!at->src) {
            if (j == 0 || nat_cur_of(at->slot, &cn) < 0)
                goto fall;
            continue;
        }
        vbo = GLD_U32(V, VA_VBO(V, at->slot));
        if (!vbo)
            goto fall;                  /* tableau client : l'empaquetage */
        seen_vbo = 1;
        code = nat_code(j, at->slot);
        b = buf_from_vbo(vbo);
        if (code < 0 || !b)
            goto fall;
        base = GLD_U32((unsigned char *)vbo, VBO_DATA);
        size = GLD_U32((unsigned char *)vbo, VBO_SIZE);
        if (!base || !size || size > RAWPOOL_BYTES || (unsigned long)at->src < base)
            goto fall;
        off = (unsigned long)at->src - base;
        stride = (unsigned long)at->stride;
        if (!nat_type_ok(at->type, at->bpc) || at->src_n < 1 || at->src_n > 4 ||
            at->stride <= 0 || off >= size)
            goto fall;
        /* off + vmax·pas + taille ≤ taille logique, sans débordement 32 bits */
        if (vmax && vmax > (size - off) / stride)
            goto fall;
        end = off + vmax * stride + (unsigned long)(at->src_n * at->bpc);
        if (end > size)
            goto fall;
        k = nat_buf_add(nb, &nn, b, base, size, off + vmin * stride, end);
        if (k < 0)
            goto fall;
        dbuf[nd] = k;
        d[nd][0] = (unsigned long)code;
        d[nd][1] = 0;                   /* tampon hôte : après raw_ensure */
        d[nd][2] = off;                 /* + rp_off : après raw_ensure */
        d[nd][3] = stride;
        d[nd][4] = at->type;
        d[nd][5] = (unsigned long)at->src_n | (at->norm ? QGPU_NATTR_NORMALIZED : 0);
        nd++;
    }
    if (!seen_vbo)
        return -1;

    /* 2. Indices : dans un VBO d'éléments lié (décrit tel quel), sinon
          recopiés dans la zone des indices (u8 → u16) */
    if (nidx) {
        unsigned long ev, base, size, p0;
        isz = itype == VA_GL_UINT ? 4 : itype == VA_GL_USHORT ? 2 :
              itype == VA_GL_UBYTE ? 1 : 0;
        if (!isz || !indices)
            goto fall;
        ev = GLD_U32(V, VA_EBO);
        if (ev && isz > 1) {
            PBuf *e = buf_from_vbo(ev);
            base = GLD_U32((unsigned char *)ev, VBO_DATA);
            size = GLD_U32((unsigned char *)ev, VBO_SIZE);
            p0 = (unsigned long)indices;
            if (e && base && size && size <= RAWPOOL_BYTES && p0 >= base &&
                p0 - base < size && !((p0 - base) & (isz - 1)) &&
                nidx <= (size - (p0 - base)) / isz) {
                ioff_b = p0 - base;
                ib = nat_buf_add(nb, &nn, e, base, size, ioff_b, ioff_b + nidx * isz);
                if (ib < 0)
                    goto fall;
            }
        }
        itype_h = isz == 4 ? QGPU_IDX_U32 : QGPU_IDX_U16;
    }

    /* 3. Miroirs : tranche, puis recopie de ce qui est sale ET lu. Peut vider
          le flux (arène, nouvelle réserve) : rien n'est encore écrit pour CE
          dessin. Un échec laisse des recopies justes et rend la main. */
    for (k = 0; k < nn; k++)
        if (!raw_ensure(p, nb[k].b, nb[k].size, nb[k].base) || !G.native ||
            !raw_sync(p, nb[k].b, nb[k].base, nb[k].size, nb[k].lo, nb[k].hi))
            goto fall;

    /* 4. Attributs constants du plan : leur valeur en SET_CURRENT (peut
          vider le flux, lui aussi) */
    for (j = 1; j < pl->n; j++) {
        const VaAttr *at = &pl->a[j];
        if (at->src)
            continue;
        w = nat_cur_of(at->slot, &cn);
        memset(a, 0, sizeof(a));
        a[0] = (unsigned long)w;
        put_f(a + 1, at->cur, cn);
        if ((p->nat_cur_ok & (1UL << w)) && !memcmp(p->nat_cur[w], a + 1, 4 * sizeof(a[0])))
            continue;
        send_cmd(p, QGPU_OP_SET_CURRENT, QGPU_LEN_SET_CURRENT, a);
        memcpy(p->nat_cur[w], a + 1, 4 * sizeof(a[0]));
        p->nat_cur_ok |= 1UL << w;
        /* geom_send_current croit l'hôte à la valeur GL brute : la faire
           renvoyer la prochaine fois qu'elle servira */
        if (w == QGPU_CUR_COLOR)
            memset(p->c_cur[0], 0xff, sizeof(p->c_cur[0]));
        else if (w == QGPU_CUR_NORMAL)
            memset(p->c_cur[1], 0xff, sizeof(p->c_cur[1]));
    }

    /* 5. Place : descripteurs dans la zone des sommets de la moitié courante,
          indices dans la sienne, commande — AUCUN vidage entre les trois (pas
          d'arène ici : arena_alloc peut vider, et les indices écrits avant
          partiraient dans l'autre moitié que la commande). */
    dbytes = (unsigned long)nd * QGPU_NATTR_WORDS * 4;
    ibytes = (nidx && ib < 0) ? nidx * (isz == 4 ? 4UL : 2UL) : 0;
    if (G.ncmd + QGPU_LEN_DRAW_NATIVE + QGPU_LEN_DRAW_RAW + 2 * QGPU_LEN_CTX + 8 > CMD_WORDS ||
        VTX_OFF + G.vtx + dbytes > VTX_LIMIT ||
        (ibytes && ((G.idx + 3) & ~3UL) + ibytes > IDX_SIZE))
        flush();
    if (G.ncmd + QGPU_LEN_DRAW_NATIVE + QGPU_LEN_DRAW_RAW + 2 * QGPU_LEN_CTX + 8 > CMD_WORDS ||
        VTX_OFF + G.vtx + dbytes > VTX_LIMIT ||
        (ibytes && ((G.idx + 3) & ~3UL) + ibytes > IDX_SIZE))
        goto fall;
    /* un vidage ci-dessus a pu rendre un refus de l'hôte (broken_all) :
       DRAW_NATIVE coupé, ou le contexte perdu */
    if (!G.native || p->broken || p->qctx < 0)
        goto fall;
    close_raw();                        /* la série Begin/End en attente d'abord */
    dst = (unsigned long *)(G.win + VTX_OFF + G.vtx);
    for (j = 0; j < nd; j++) {
        const PBuf *b = nb[dbuf[j]].b;
        dst[0] = d[j][0];
        dst[1] = (unsigned long)G.rp_qid[b->rp];
        dst[2] = b->rp_off + d[j][2];
        dst[3] = d[j][3];
        dst[4] = d[j][4];
        dst[5] = d[j][5];
        dst += QGPU_NATTR_WORDS;
    }
    aoff = G.base + VTX_OFF + G.vtx;
    G.vtx += dbytes;
    if (nidx && ib >= 0) {
        ibuf = (unsigned long)G.rp_qid[nb[ib].b->rp];
        ioff = nb[ib].b->rp_off + ioff_b;
    } else if (nidx) {
        unsigned char *di;
        G.idx = (G.idx + 3) & ~3UL;
        di = G.win + IDX_OFF + G.idx;
        if (isz == 1) {                 /* le contrat ne connaît pas u8 */
            const unsigned char *s8 = (const unsigned char *)indices;
            unsigned short *d16 = (unsigned short *)di;
            unsigned long q;
            for (q = 0; q < nidx; q++)
                d16[q] = s8[q];
        } else {
            memcpy(di, indices, ibytes);    /* grand-boutiste tel quel */
        }
        ibuf = QGPU_BUF_SHMEM;
        ioff = G.base + IDX_OFF + G.idx;
        G.idx += ibytes;
    }
    if (G.bound != p) {
        c = G.cmd + G.ncmd;
        c[0] = QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX);
        c[1] = p->qctx;
        G.ncmd += QGPU_LEN_CTX;
        G.bound = p;
    }
    c = G.cmd + G.ncmd;
    c[0] = QGPU_CMD_HDR(QGPU_OP_DRAW_NATIVE, QGPU_LEN_DRAW_NATIVE);
    c[1] = mode;
    c[2] = nidx ? nidx : (unsigned long)count;
    c[3] = ibuf;
    c[4] = ioff;
    c[5] = itype_h;
    c[6] = nidx ? 0 : (unsigned long)first;
    c[7] = (unsigned long)nd;
    c[8] = aoff;
    G.ncmd += QGPU_LEN_DRAW_NATIVE;
    {
        static unsigned long said;
        if (said < 3) {
            const unsigned long *t = (const unsigned long *)(G.win + (aoff - G.base));
            said++;
            gl_note("DRAW_NATIVE #%lu image %lu : mode %lu n %lu ibuf %lx ioff %lx itype %lu "
                    "premier %lu, %d attributs, sommets %lu..%lu\n", said, G.n_frames,
                    c[1], c[2], c[3], c[4], c[5], c[6], nd, vmin, vmax);
            for (j = 0; j < nd; j++, t += QGPU_NATTR_WORDS)
                gl_note("   code %2lu tampon %lu offset %lu pas %lu type %04lx taille %lx\n",
                        t[0], t[1], t[2], t[3], t[4], t[5]);
        }
    }
    G.n_native_draws++;
    G.n_native_verts += nverts;
    va_count_prims(mode, nidx ? nidx : (unsigned long)count);
    p->color = HOST_NEWER;
    if (writes_depth(p))
        p->depth = HOST_NEWER;
    return 1;

fall:
    if (seen_vbo)
        G.n_native_fall++;
    return -1;
}

/* Cœur du canal tableaux. Verrou déjà tenu. 1 = traité (même si n=0). */
static int geom_draw_client_unsafe(PCtx *p, long indexed, unsigned long mode,
                                   long first, long count, unsigned long itype,
                                   const void *indices);

static int geom_draw_client(PCtx *p, long indexed, unsigned long mode,
                            long first, long count, unsigned long itype,
                            const void *indices)
{
    int r;
    if (sigsetjmp(pack_jmp, 0) != 0) {     /* 0 : pas de sigprocmask par dessin (5 % du fil !) ;
                                               SA_NODEFER dans le crochet rend le retour sûr */
        static unsigned long told;
        pack_jmp_on = 0;
        if (told < 5) {
            const VaPlan *pl = (const VaPlan *)dbg_plan;
            int j;
            told++;
            gl_note("ARRAYS faute de lecture en %08lx (lot %ld sommets, mode %lu, indexe %ld, "
                    "premier %ld, i %lu/%lu vmin %lu fmt %lx) : lot JETE\n",
                    pack_fault_addr, count, mode, indexed, first,
                    dbg_vtx_i, dbg_vtx_n, dbg_vmin, dbg_fmt);
            for (j = 0; pl && j < pl->n; j++)
                gl_note("   attr %d : src %08lx pas %ld bpc %d n %d/%d type %x\n", j,
                        (unsigned long)pl->a[j].src, pl->a[j].stride, pl->a[j].bpc,
                        pl->a[j].src_n, pl->a[j].dst_n, pl->a[j].type);
        }
        /* Refuser enverrait le lot au rendu d'Apple, qui meurt (Bus error) sur
           une texture DXT à mipmaps (S3TC annoncé) : le lot est JETÉ. Un lot
           qu'on ne peut pas lire est presque toujours un lot fou. */
        dbg_plan = 0;
        G.n_geomdrop++;
        G.n_dropped_fault++;            /* I9 (relecture du 24/09) */
        return 1;
    }
    pack_thr = pthread_self();          /* P3 (relecture du 24/09) */
    pack_jmp_on = 1;
    r = geom_draw_client_unsafe(p, indexed, mode, first, count, itype, indices);
    pack_jmp_on = 0;
    return r;
}

/* R1 (relecture du 24/09) : POMPPC_GL_TRIFILTER lu une fois, pas à chaque lot. */
static int trifilter_on(void)
{
    static long on = -1;
    if (on < 0) {
        const char *e = getenv("POMPPC_GL_TRIFILTER");
        on = (e && e[0] == '0') ? 0 : 1;
    }
    return (int)on;
}

static int geom_draw_client_unsafe(PCtx *p, long indexed, unsigned long mode,
                                   long first, long count, unsigned long itype,
                                   const void *indices)
{
    unsigned char *V, *g;
    TexInfo ti;
    unsigned long fmt, words, vmin, vmax, nverts, nidx, ioff, itype_h;
    unsigned long vtx_off, packed, *c, key, gs;
    float *dst;
    PBuf *hb;
    int host, reuse, filter_bad = 0;
    long i;
    static VaPlan plan;                 /* verrou tenu : une seule à la fois */

    if (count <= 0)
        return 1;
    if (!p || mode > QGPU_PRIM_MODE_POLYGON)
        return 0;
    g = gls(p);
    V = g ? (unsigned char *)GLD_U32(g, GS_VAO) : 0;
    /* v16 : sous programme, le générique 0 tient lieu de position */
    if (!V || !(va_enabled(V, 0) || (G.prog && p->vp_on && va_enabled(V, 16))))
        return no(NO_G_ARRAY, 0, (unsigned long)count);
    if (!geom_ok(p) || !ensure_surface(p) || !texture_ok(p, &ti))
        return 0;
    if (!prog_sync(p))                  /* v16 : programme refusé par l'hôte */
        return 0;
    fmt = geom_format(p);
    /* génériques à la taille de leurs tableaux (QGPU_CAP_GEN_SIZES) : DOOM 3
       st 2f, normale et tangentes 3f — 11 mots au lieu de 16 */
    gs = va_gen_sizes(p, V, fmt);
    words = QGPU_VF_WORDS_GS(fmt, gs);
    if (!words)
        return 0;
    if (!va_sources_ok(p, V, fmt))
        return no(NO_G_SRC, (unsigned long)V, (unsigned long)count);
    if (indexed && itype != VA_ITYPE_NONE && indices) {
        if (!va_scan_idx(indices, itype, count, &vmin, &vmax))
            return no(NO_G_ARRAY, itype, (unsigned long)count);
        nverts = vmax - vmin + 1;
        nidx = (unsigned long)count;
    } else {
        if (first < 0)
            first = 0;
        vmin = (unsigned long)first;
        nverts = (unsigned long)count;
        vmax = vmin + nverts - 1;
        nidx = 0;
        indices = 0;
    }
    if (nverts == 0 || nverts > QGPU_MAX_VERTS || nidx > QGPU_MAX_VERTS)
        return no(NO_G_ARRAY, nverts, nidx);
    check_draw_buffer(p);
    sync_to_host(p, 1, GLD_U8(g, GS_DEPTH_TEST) || stencil_active(p));
    send_state(p, &ti, 1);
    geom_send_all(p, fmt);
    packed = nverts * words * 4;
    /* I5 (relecture du 24/09) : le plan d'abord — la clé et la propreté des
       VBO en dérivent (générique 0 en position, génériques, unités 4..7) */
    va_plan_build(&plan, p, V, fmt, gs);
    /* v18 : tout le plan dans des VBO → DRAW_NATIVE, sans empaquetage */
    if (G.native) {
        int nr = geom_draw_native(p, V, &plan, mode, first, count,
                                  nidx ? itype : VA_ITYPE_NONE, indices,
                                  vmin, vmax, nidx);
        if (nr >= 0)
            return nr;
    }
    host = va_host_ready(V, &plan, &hb);
    key = va_pack_key(&plan, fmt);      /* P13 : clé COMPLÈTE */
    reuse = host == 1 && hb && hb->qid >= 0 && hb->qsize >= packed &&
            hb->pack_fmt == fmt && hb->pack_gs == gs && hb->pack_vmin == vmin &&
            hb->pack_nverts == nverts && hb->pack_key == key;
    /* VTX_LIMIT est un offset ABSOLU dans la fenêtre (= IDX_OFF) ; G.vtx est
       RELATIF à VTX_OFF. Sans le terme VTX_OFF, ce test laissait les sommets
       déborder de 0x40000 octets dans la zone des indices et les écraser :
       les indices lus devenaient des morceaux de flottants (≥ nverts), le cœur
       refusait le DRAW_RAW (BAD_ARG) et tout le chemin brut tombait en
       rastérisation pour la session (UT2004, toute la géométrie « from arrays »
       ; Marble Blast passe par Begin/End, dont le test était juste). */
    if (G.ncmd + QGPU_LEN_DRAW_RAW_BUF + QGPU_LEN_BUF_SUBDATA +
            QGPU_LEN_BUF_CREATE + QGPU_LEN_SET_STATE + 8 > CMD_WORDS ||
        (!reuse && VTX_OFF + G.vtx + packed > VTX_LIMIT) ||
        (nidx && G.idx + nidx * 4 + 4 > IDX_SIZE))
        flush();
    if ((!reuse && VTX_OFF + G.vtx + packed > VTX_LIMIT) ||
        (nidx && G.idx + nidx * 4 + 4 > IDX_SIZE)) {
        static int told;                /* 24/09 : DOOM 3 et Prey, « raw:arrays 30/48 » */
        if (!told) {
            told = 1;
            gl_note("ARRAYS refus apres flush : nverts %lu nidx %lu packed %lu words %lu "
                    "vtx %lu idx %lu reuse %d host %d ncmd %lu\n",
                    nverts, nidx, packed, words, G.vtx, G.idx, reuse, host, G.ncmd);
        }
        return no(NO_G_ARRAY, nverts, nidx);
    }
    vtx_off = 0;
    if (!reuse) {
        if (host && hb && !buf_host_ensure(p, hb, packed))
            hb = 0;
        /* I3 (relecture du 24/09) : close_raw() rappelle raw_fix_nan, qui
           réécrit raw_bad[] avec le lot Begin/End en attente — il doit donc
           passer AVANT le marquage de ce lot-ci, pas entre le marquage et le
           filtre des triangles. */
        close_raw();
        vtx_off = G.vtx;
        dst = (float *)(G.win + VTX_OFF + vtx_off);
        {
            dbg_plan = &plan; dbg_vtx_n = nverts; dbg_vmin = vmin;
            dbg_words = words; dbg_fmt = fmt; dbg_vao = V;
            for (i = 0; (unsigned long)i < nverts; i++) {
                dbg_vtx_i = (unsigned long)i;
                va_pack_planned(dst + (unsigned long)i * words, &plan,
                                vmin + (unsigned long)i);
            }
            dbg_plan = 0;
        }
        /* P12 — le chemin TABLEAUX n'assainissait pas les sommets. Le cœur
           refuse NaN, Inf et |v| ≥ 1e9, et sur BAD_ARG il fait `break` : TOUT
           le reste du flux est perdu (H4), SURF_PRESENT compris. Le chemin
           Begin/End passe par raw_fix_nan depuis toujours ; celui-ci le fait
           maintenant aussi, w ≈ 0 compris (triangle géant jusqu'à l'origine,
           ciel de Colin McRae). Fait AVANT le BUF_SUBDATA : ce qui monte dans
           le tampon hôte est déjà propre. */
        if (nverts > RAW_NAN_MAX)
            return no(NO_G_ARRAY, nverts, 0);
        if (raw_scan_nan((unsigned long *)dst, nverts, words, fmt,
                         G.prog && p->vp_on)) {
            if (!nidx && mode == QGPU_PRIM_MODE_TRIANGLES) {
                unsigned long o = 0, k;
                for (k = 0; k + 2 < nverts; k += 3)
                    if (!raw_bad[k] && !raw_bad[k + 1] && !raw_bad[k + 2]) {
                        if (o != k)
                            memcpy((unsigned long *)dst + o * words,
                                   (unsigned long *)dst + k * words, words * 12);
                        o += 3;
                    }
                if (o == 0)
                    return 1;           /* rien à dessiner : lot consommé */
                nverts = o;
                packed = nverts * words * 4;
            } else if (nidx && mode == QGPU_PRIM_MODE_TRIANGLES && trifilter_on()) {
                /* maillage indexé de triangles : les triangles qui touchent
                   un sommet fou sont retirés à la copie des indices, les
                   sommets fous (mots remis à 0) restent — inertes */
                filter_bad = 1;
            } else {
                /* Ruban, éventail, quads : un sommet fou gâche des primitives
                   qu'on ne sait pas isoler ici. On jette CE lot,
                   l'accélération reste. */
                return no(NO_G_ARRAY, nverts, nidx);
            }
        }
        G.vtx += packed;
        if (hb && hb->qid >= 0 &&
            G.ncmd + QGPU_LEN_BUF_SUBDATA + QGPU_LEN_DRAW_RAW_BUF + 4 <= CMD_WORDS) {
            close_raw();
            if (G.bound != p) {
                c = G.cmd + G.ncmd;
                c[0] = QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX);
                c[1] = p->qctx;
                G.ncmd += QGPU_LEN_CTX;
                G.bound = p;
            }
            c = G.cmd + G.ncmd;
            c[0] = QGPU_CMD_HDR(QGPU_OP_BUF_SUBDATA, QGPU_LEN_BUF_SUBDATA);
            c[1] = (unsigned long)hb->qid;
            c[2] = 0;
            c[3] = G.base + VTX_OFF + vtx_off;
            c[4] = packed;
            G.ncmd += QGPU_LEN_BUF_SUBDATA;
            hb->pack_fmt = fmt;
            hb->pack_gs = gs;
            hb->pack_vmin = vmin;
            /* I4 : un lot dont des triangles seront retirés ne doit pas être
               réutilisé tel quel au prochain dessin (les indices repartiraient
               entiers) */
            hb->pack_nverts = filter_bad ? 0 : nverts;
            hb->pack_key = key;
            va_host_clean(V, &plan, hb);
            G.n_vbomiss++;
        } else {
            hb = 0;
        }
    } else {
        G.n_vbohits++;
    }
    ioff = 0;
    itype_h = QGPU_IDX_NONE;
    if (nidx) {
        unsigned long need;
        int use32 = (nverts > 65536) || (vmax - vmin > 65535);
        G.idx = (G.idx + 3) & ~3UL;
        need = nidx * (use32 ? 4UL : 2UL);
        if (G.idx + need > IDX_SIZE)
            return no(NO_G_ARRAY, nidx, G.idx);
        ioff = IDX_OFF + G.idx;
        if (filter_bad) {
            long kept = va_copy_idx_tri(G.win + ioff, use32, indices, itype,
                                        (long)nidx, vmin, raw_bad);
            static unsigned long told;
            if (told < 3) {
                unsigned long q, nb = 0;
                const unsigned long *w0 = (const unsigned long *)dst;
                told++;
                for (q = 0; q < nverts; q++)
                    nb += raw_bad[q];
                gl_note("ARRAYS triangles fous retires : %lu/%lu indices gardes (nverts %lu, "
                        "fous %lu, fmt %lx, words %lu, itype %lx, vmin %lu) v0 = %08lx %08lx %08lx %08lx "
                        "%08lx %08lx %08lx %08lx\n",
                        (unsigned long)kept, nidx, nverts, nb, fmt, words, itype, vmin,
                        w0[0], w0[1], w0[2], w0[3], w0[4], w0[5], w0[6], w0[7]);
            }
            G.n_geomdrop += (nidx - (unsigned long)kept) / 3;
            nidx = (unsigned long)kept;
            if (!nidx)
                return 1;               /* rien à dessiner : lot consommé */
            need = nidx * (use32 ? 4UL : 2UL);
        } else if (use32) {
            va_copy_idx32((unsigned long *)(G.win + ioff), indices, itype,
                          (long)nidx, vmin);
        } else {
            va_copy_idx16((unsigned short *)(G.win + ioff), indices, itype,
                          (long)nidx, vmin);
        }
        itype_h = use32 ? QGPU_IDX_U32 : QGPU_IDX_U16;
        G.idx += need;
    }
    /* Glyphes du menu : quelques rectangles courts, annoncés en TRIANGLES.
       Le 4e sommet recoud la lettre suivante et balaie l'atlas. On ne convertit
       pas un maillage entier : le plan du tutoriel est lui aussi en rectangles,
       et le passer en quads en faisait une nappe verte. */
    if (!nidx && mode == QGPU_PRIM_MODE_TRIANGLES && !reuse &&
        nverts <= 96 && !GLD_U8(g, GS_LIGHTING) &&
        quads_axis_aligned((const float *)(G.win + VTX_OFF + vtx_off), nverts, words))
        mode = QGPU_PRIM_MODE_QUADS;
    if (!reuse)
        geom_probe(p, "Array", mode, nverts, fmt, words,
                   (const float *)(G.win + VTX_OFF + vtx_off));
    if (!emit_draw_client(p, mode, nidx, nverts, fmt, gs, vtx_off, ioff, itype_h, hb))
        return no(NO_G_ARRAY, G.ncmd, nverts);
    G.n_rawdraws++;
    G.n_rawverts += nverts;
    G.n_arraydraws++;
    G.n_arrayverts += nverts;
    va_count_prims(mode, nidx ? nidx : nverts);
    p->color = HOST_NEWER;
    if (writes_depth(p))
        p->depth = HOST_NEWER;
    return 1;
}

/* +0x70 RenderVertexArray : VAR et, sans descripteur, glDrawArrays/Elements.
 * 0 = non pris en charge (GLEngine reprend le logiciel — seulement hors T&L). */
static long geom_render_array(void *ctx, long indexed, unsigned long mode,
                              long first, long count, unsigned long itype,
                              const void *indices, void *vtxbuf, void *curr)
{
    PCtx *p;
    long r;
    (void)vtxbuf;
    (void)curr;
    if (pomppc_tracing())
        pomppc_log("  géométrie : Array(idx %ld mode %lu first %ld n %ld type %lx)\n",
                   indexed, mode, first, count, itype);
    pthread_mutex_lock(&G.mu);
    stream_ready();                     /* F9 */
    p = find_ctx(ctx);
    /* F1/F2 : une rastérisation (ou un dessin par tableaux) qui arrive alors
       qu'un BeginPrimitiveBuffer est ouvert veut dire que GLEngine a basculé
       en logiciel (_gleForceToSoftwareTCL) : l'EndPrimitiveBuffer ne viendra
       jamais. On referme NOTRE `pend` et on rend sa place. */
    pend_close(p, 1);
    r = geom_draw_client(p, indexed, mode, first, count,
                         indexed ? itype : VA_ITYPE_NONE,
                         indexed ? indices : 0) ? 1 : 0;
    if (!r && p) {
        G.n_geomdrop++;
        /* Un dessin trop grand ou un tableau incomplet : on jette CE lot.
           Quitter le domaine pour de bon (geom_lost) seulement si l'état a
           bougé hors du prédicat — sinon le premier maillage sparse tuerait
           tout le T&L, y compris le mode immédiat. */
        if (!geom_ok(p)) {
            no(NO_G_LATE, mode, (unsigned long)count);
            p->geom_lost = 1;
        }
    }
    pthread_mutex_unlock(&G.mu);
    return r;
}

/* +0x4c RenderVertexBuffer : GLEngine a recopié les sommets dans `buf` selon
 * un format matériel. On ignore ce tampon et on relit les tableaux clients —
 * le format GeForce3 n'est pas le nôtre, et relire évite de le deviner.
 * `bias` sert au tampon packé ; les indices reçus sont ceux de l'application. */
static void geom_render_vb(void *ctx, void *buf, unsigned long mode, long bias,
                           long count, unsigned long itype, const void *indices)
{
    PCtx *p;
    (void)buf;
    (void)bias;
    if (pomppc_tracing())
        pomppc_log("  géométrie : VBuf(mode %lu n %ld type %lx bias %ld)\n",
                   mode, count, itype, bias);
    pthread_mutex_lock(&G.mu);
    stream_ready();                     /* F9 */
    p = find_ctx(ctx);
    pend_close(p, 1);                   /* F1/F2 : voir geom_render_array */
    if (!geom_draw_client(p, itype != VA_ITYPE_NONE && indices != 0, mode,
                          0, count, itype, indices) && p) {
        G.n_geomdrop++;
        if (!geom_ok(p)) {
            no(NO_G_LATE, mode, (unsigned long)count);
            p->geom_lost = 1;
        }
    }
    pthread_mutex_unlock(&G.mu);
}

static void *geom_alloc_vb(void *ctx, unsigned long id, unsigned long *n)
{
    (void)ctx;
    (void)id;
    if (n) {
        if (*n == 0)
            return 0;
        if (*n > VA_ALLOC_CAP)
            *n = VA_ALLOC_CAP;
    }
    return vb_scratch;
}

static void geom_complete_vb(void *ctx, void *buf, unsigned long used)
{
    (void)ctx;
    (void)buf;
    (void)used;
}

static void geom_free_vb(void *ctx, void *buf)
{
    (void)ctx;
    (void)buf;
}

/* Procédure à installer dans la table de GLEngine, ou 0. */
void *pomppc_geom_proc(int slot)
{
    if (!G.v7)
        return 0;
    switch (slot) {
    case PROC_BeginPrimitiveBuffer: return (void *)geom_begin;
    case PROC_EndPrimitiveBuffer:   return (void *)geom_end;
    case PROC_RenderVertexArray:    return (void *)geom_render_array;
    case PROC_RenderVertexBuffer:   return (void *)geom_render_vb;
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
        TexInfo ti;
        int arrays, force;
        /* Téléverser maintenant : si une police n'est pas encore prête, on
           laisse GLEngine transformer (bit 0 = 0) plutôt que de jeter le
           premier lot de glyphes sous T&L. Le dispatch suivant réessaiera. */
        if (!texture_ok(p, &ti)) {
            flush();
            if (!texture_ok(p, &ti)) {
                p->geom_on = 0;
                pthread_mutex_unlock(&G.mu);
                return 0;
            }
        }
        arrays = geom_va_on(p);
        /* Mixte : 0x78 détourne DrawArrays/DrawElements vers RenderVertexArray
           sans retirer le descripteur (glBegin reste le chemin T&L). ARRAY=2
           retire 0x11c, sauf si un glBegin a déjà forcé le repli mixte. */
        force = arrays && array_switch() >= 2 && !p->array_mix;
        if (p->cfg) {
            unsigned char want78 = (arrays && !p->array_mix) ? 1 : 0;
            if (GLD_U8(p->cfg, 0x78) != want78) {
                GLD_U8(p->cfg, 0x78) = want78;
                p->desc_dirty = 1;
            }
        }
        if (force) {
            if (p->cfg && GLD_U32(p->cfg, 0x11c) != 0) {
                GLD_U32(p->cfg, 0x11c) = 0;
                p->desc_dirty = 1;
            }
            p->geom_fmt = geom_format(p);
            p->geom_words = QGPU_VF_WORDS(p->geom_fmt);
        } else {
            if (geom_publish(p))
                p->desc_dirty = 1;
        }
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
        /* +0x78 : seulement si le canal tableaux est demandé. Posé trop tôt,
           _gleDrawArraysOrElements_Exec quitte Begin/End alors que le
           descripteur est encore là — ou que les jeux n'utilisent que glBegin
           avec GL_VERTEX_ARRAY allumé. */
        /* Identifiants de format de sommet (pas imposés par GLEngine :
           0x10, 0x18, 0x20, 0x14, 0x20|0x24, 0x2c|0x34). Valeurs du GeForce3. */
        {
            static const unsigned short ids[6] = { 3, 2, 1, 6, 5, 4 };
            int i;
            for (i = 0; i < 6; i++)
                *(unsigned short *)(p->cfg + 0x7c + i * 2) = ids[i];
        }
        /* Second verrou (docs/re/capacites-glengine.md §5.3). À 0, une
           coordonnée de texture r ou q donnée ENTRE glBegin et glEnd
           (glTexCoord3f/4f, glMultiTexCoord3f… : gctx+0x486c bit 0) fait
           basculer la primitive vers le logiciel à glEnd
           (_gleForceToSoftwareTCL) — et ce repli-là perd la géométrie avec
           notre pilote : EndPrimitiveBuffer arrive avec un compte calculé
           entre deux tampons différents. À 1, comme le pilote GeForce3,
           GLEngine reste sur nous et le descripteur (4 composantes par unité)
           porte s, t, r, q. Relevé : docs/re/opengl-1.4.md §3. */
        if (!(getenv("POMPPC_GL_RDIRTY") && getenv("POMPPC_GL_RDIRTY")[0] == '0'))
            GLD_U8(p->cfg, 0x7a) = 1;
        /* GeForce3 : +0x7b lu seulement par _glDrawPixels_Exec. Sémantique
           ÉTABLIE en VM le 22/09/2026 (bissect f3c4280, scènes drawpack, mixte,
           v15) : à 1, GLEngine émule glDrawPixels par un quad à TEXTURE
           RECTANGLE envoyé à RenderQuads — jamais par la procédure DrawPixels
           du pilote, même installée — ; ce plugin refuse les cibles rectangle,
           le repli vers le RenderQuads logiciel d'Apple ne dessine RIEN, et
           tout glDrawPixels disparaît (v15 « glWindowPos + glDrawPixels » NON
           TENU, mixte 230/255 d'écart, drawpack 0 pixel). À 0, GLEngine fait le
           DrawPixels lui-même en logiciel, synchronisé par le repli : exact
           (v15 17/17). Le 1 avait été posé pour « polices bitmap muettes sous
           T&L » (jamais prouvé) : POMPPC_GL_DP7B=1 le remet pour l'essayer.
           Le jour où les textures rectangle seront portées sur l'hôte, 1
           deviendra le bon réglage (DrawPixels sur le GPU). */
        GLD_U8(p->cfg, 0x7b) = (getenv("POMPPC_GL_DP7B") &&
                                getenv("POMPPC_GL_DP7B")[0] == '1') ? 1 : 0;
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
    int    ptatt;                       /* 1.4 : points atténués, taille par point */
} Batch;

/* Prépare un lot de triangles ; 0 = passer par le logiciel. Verrou tenu. */
static void begin_common(PCtx *p, Batch *b, const TexInfo *ti)
{
    /* F1 — TOUTE rastérisation qui arrive referme le BeginPrimitiveBuffer
       resté ouvert de ce contexte : GLEngine a basculé en logiciel
       (_gleForceToSoftwareTCL) et EndPrimitiveBuffer ne viendra plus. Sans
       cela, G.npend restait posé pour toujours — flush() ne rendait plus
       jamais la zone des sommets, submit_cur restait synchrone, et prim()
       finissait par écrire hors de la tranche. */
    {   /* POMPPC_GL_PENDCLOSE=0 : A/B des traînées d'UT2004 (22/09) — ne pas
           refermer, comportement d'avant F1 (prim() refuse alors si la place
           manque). POMPPC_GL_GEOMCHECK=1 compte ces fermetures. */
        static int pendclose = -1, gc_on = -1;
        static unsigned long seen;
        if (pendclose < 0) {
            const char *e = getenv("POMPPC_GL_PENDCLOSE");
            pendclose = (e && *e == '0') ? 0 : 1;
            e = getenv("POMPPC_GL_GEOMCHECK");
            gc_on = (e && *e && *e != '0') ? 1 : 0;
        }
        if (p && p->pend_open) {
            if (gc_on && seen++ < 60)
                gl_note("GEOMCHECK pend %s par rastérisation : %lu slots à %lx, image %lu\n",
                        pendclose ? "FERMÉ" : "laissé ouvert", p->pend_slots, p->pend_off,
                        G.n_frames);
            if (pendclose)
                pend_close(p, 1);
        }
    }
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
 * Les paramètres de point (GL 1.4) : `QGPU_SK_POINT_SIZE` est une seule valeur
 * par dessin, et GLEngine ne met pas la taille dérivée dans le sommet. Avec
 * G.tex14, pt_size la calcule point par point depuis les coordonnées œil du
 * sommet ; sans, l'atténuation est refusée (elle sortait à la taille de base,
 * en silence). */
static int begin_lp(PCtx *p, Batch *b, int lines)
{
    unsigned char *g;
    const float *att;
    if (!accel_ok(p) || texturing_on(p) || !ensure_surface(p))
        return 0;
    g = gls(p);
    if (lines ? (GLD_U8(g, GS_LINE_STIPPLE) || GLD_U8(g, GS_LINE_SMOOTH) || GLD_U8(g, GS_POLY_OFS_LINE))
              : (GLD_U8(g, GS_POINT_SMOOTH) || GLD_U8(g, GS_POLY_OFS_PT)))
        return 0;
    att = (const float *)(g + GS_POINT_ATT);
    if (!lines && (att[0] != 1.0f || att[1] != 0.0f || att[2] != 0.0f) &&
        !(G.tex14 && point_params_ok(g)))
        return no(NO_G_POINT, fbits(att[1]), fbits(att[2]));
    begin_common(p, b, 0);
    b->kind = lines ? RK_LINES : RK_POINTS;
    b->ptatt = !lines && !point_att_default(g);
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
    /* v11 : sous la spéculaire séparée, GLEngine laisse la spéculaire HORS de
       la couleur primaire (en V_SEC) ; l'hôte l'ajoute après la texture. Avant
       la v11 elle était perdue — comme sous le rendu d'Apple, qui l'ignore. */
    if (G.q.version >= 11 &&
        (GLD_U8(gls(p), GS_LIGHTING) ? U16(gls(p), GS_COLOR_CONTROL) == 0x81FA
                                     : G.tex14 && GLD_U8(gls(p), GS_COLOR_SUM)))
        b->kind = RK_TRI_SEC0 + rk_units[b->kind];
    return 1;
}

static void put_vertex(const Batch *b, float *o, const unsigned char *v, const unsigned char *col)
{
    const float *c = (const float *)(col + V_COLOR);
    int u, k, nu = rk_units[b->kind];
    for (u = 0; u < nu; u++) {
        const float *t = (const float *)(v + V_TEX(u));
        for (k = 0; k < 4; k++)
            o[8 + 4 * u + k] = sane_f(t[k]);
    }
    o[0] = sane_f(GLD_F32(v, V_X));
    o[1] = sane_f(b->h - GLD_F32(v, V_Y));
    o[2] = clamp01(GLD_F32(v, V_Z) * b->zinv);
    o[3] = b->fog ? clamp01(GLD_F32(v, V_FOG)) : 1.0f;
    o[4] = clamp01(c[0]);
    o[5] = clamp01(c[1]);
    o[6] = clamp01(c[2]);
    o[7] = clamp01(c[3]);
    if (RK_IS_SEC(b->kind)) {
        /* après les coordonnées de texture ; en ombrage plat, celle du sommet
           provoquant, comme la couleur primaire (`col`) */
        const float *sc = (const float *)(col + V_SEC);
        o[8 + 4 * nu] = clamp01(sc[0]);
        o[9 + 4 * nu] = clamp01(sc[1]);
        o[10 + 4 * nu] = clamp01(sc[2]);
    }
}

/* Ajoute une primitive de n sommets (3, 2 ou 1) à la série du lot ;
   `prov` = sommet porteur de la couleur en ombrage plat. */
static void prim(Batch *b, int n, const unsigned char **v, const unsigned char *prov)
{
    float *o;
    unsigned long vw = rk_words[b->kind];
    int i;
    stream_ready();                     /* F9 : G.vtx / G.win relus après */
    /* UT2004 laisse des NaN (positions, texcoords) : un seul ferait rejeter
       tout le DRAW_TRIANGLES_TEX. On saute la primitive, le reste part.
       Colin McRae (menu 3D, ciel) envoie aussi des sommets fenêtre hors de
       tout écran — Apple n'a pas découpé w=0. Un seul hors bande de garde
       (4× le drawable) peint un triangle géant sur le HUD. */
    {
        float xmax = (float)((int)GLD_U32(b->p->ctx, CTX_WIDTH)  * 4);
        float ymax = (float)((int)GLD_U32(b->p->ctx, CTX_HEIGHT) * 4);
        if (!(xmax > 0.0f)) xmax = 4096.0f;
        if (!(ymax > 0.0f)) ymax = 4096.0f;
        for (i = 0; i < n; i++) {
            float x = GLD_F32(v[i], V_X), y = GLD_F32(v[i], V_Y);
            if (!(x > -xmax && x < xmax && y > -ymax && y < ymax))
                return;
        }
    }
    /* VTX_LIMIT, pas VTX_END : le haut de la zone porte les indices de la
       fusion, et les deux chemins cohabitent dans la même image (scène mixte). */
    if (VTX_OFF + G.vtx + (unsigned long)n * vw * 4 > VTX_LIMIT) {
        flush();                        /* l'état GL reste sur le device, par contexte */
        /* F1 — flush() NE REND la zone des sommets que si aucun
           BeginPrimitiveBuffer n'est ouvert (G.npend). Le cas arrive vraiment :
           GLEngine ouvre un tampon puis bascule en logiciel
           (_gleForceToSoftwareTCL) et rastérise sans jamais appeler
           EndPrimitiveBuffer. On écrivait alors au-delà de VTX_LIMIT, puis
           dans les indices, puis dans l'arène, puis hors de la tranche —
           Bus error. On jette le lot : le rendu reste exact à la primitive
           près, et la suivante repartira après le prochain vidage. */
        if (VTX_OFF + G.vtx + (unsigned long)n * vw * 4 > VTX_LIMIT) {
            no(NO_BUFFER, G.vtx, (unsigned long)n * vw * 4);
            G.n_geomdrop++;
            return;
        }
    }
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
    if (rk_units[b->kind])
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

/* Points atténués au chemin hérité : DRAW_POINTS n'a qu'une taille par dessin
 * (QGPU_SK_POINT_SIZE). GLEngine laisse dans le sommet les coordonnées ŒIL
 * (V_EYE, relevé : sonde ptprobe) ; la taille dérivée se calcule donc ici,
 * point par point, avec la règle de l'hôte (qgpu_point_size), et la clé ne
 * change que si la taille arrondie change. */
static void pt_size(Batch *b, const unsigned char *v0)
{
    const unsigned char *g = gls(b->p);
    const float *e = (const float *)(v0 + V_EYE), *att = (const float *)(g + GS_POINT_ATT);
    float d = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    float den = att[0] + att[1] * d + att[2] * d * d, mx = point_max(g);
    float sz = GLD_F32(g, GS_POINT_SIZE), mn = GLD_F32(g, GS_POINT_SIZE_MIN);
    unsigned long *c, bits;
    sz = den > 0.0f ? sz * sqrtf(1.0f / den) : mx;
    if (sz > mx) sz = mx;
    if (sz < mn) sz = mn;
    if (!(sz >= 1.0f)) sz = 1.0f;
    if (sz > 64.0f) sz = 64.0f;
    sz = (float)(int)(sz + 0.5f);
    bits = fbits(sz);
    if (b->p->st_valid && b->p->st[QGPU_SK_POINT_SIZE] == bits)
        return;
    c = reserve(b->p, QGPU_LEN_SET_STATE);     /* ferme la série en cours */
    c[0] = QGPU_CMD_HDR(QGPU_OP_SET_STATE, QGPU_LEN_SET_STATE);
    c[1] = QGPU_SK_POINT_SIZE;
    c[2] = bits;
    b->p->st[QGPU_SK_POINT_SIZE] = bits;
}

static void pt(Batch *b, const unsigned char *v0)
{
    if (b->ptatt)
        pt_size(b, v0);
    prim(b, 1, &v0, v0);
    G.n_points++;
}

static void end_tris(Batch *b)
{
    b->p->color = HOST_NEWER;
    if (writes_depth(b->p))
        b->p->depth = HOST_NEWER;
}

static void lazy_sync(PCtx *p);

/* Repli : synchronise l'invité et marque ce que le logiciel va écrire.
 * `touches_depth` : la procédure lit ou écrit la profondeur. */
static void *fallback(PCtx *p, int slot, int writes_color, int touches_depth)
{
    /* Le rendu d'Apple va travailler : il doit d'abord connaître tous les
       changements d'état que la transmission paresseuse lui a tus. AVANT la
       relecture, parce que c'est ce dispatch qui alloue au besoin son tampon
       de profondeur. Les échanges n'en ont pas besoin (gldSwapBuffers ne lit
       que le tampon de dessin, tenu à jour par le bit 0x80). */
    if (slot != PROC_Swap58 && slot != PROC_Swap5c && slot != PROC_Swap60)
        lazy_sync(p);
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

/* ───────── repli Apple : jamais de sommet fenêtre NaN ou démesuré ─────────
 * Le rastériseur logiciel d'Apple, à qui les a_* rendent ce que l'hôte ne
 * prend pas, boucle SANS FIN sur un sommet fenêtre NaN ou démesuré : Colin
 * McRae, 23/09/2026 (VM figée et lue par le moniteur), ligne de balayage de
 * y = -1,5e9 à 600 sur un polygone dont trois sommets valaient NaN et le
 * quatrième w = 5e-11 — le clipper de GLEngine ne découpe pas w ≈ 0. Le
 * chemin hôte jette ces sommets (prim(), bande de garde) ; on fait pareil
 * avant de donner quoi que ce soit à Apple : les primitives sont recomposées
 * en triangles, segments ou points indépendants, seules les saines partent. */
static int apple_vtx_ok(PCtx *p, const unsigned char *v)
{
    float x = GLD_F32(v, V_X), y = GLD_F32(v, V_Y), z = GLD_F32(v, V_Z);
    float xmax = (float)((int)GLD_U32(p->ctx, CTX_WIDTH)  * 4);
    float ymax = (float)((int)GLD_U32(p->ctx, CTX_HEIGHT) * 4);
    if (!(xmax > 0.0f)) xmax = 4096.0f;
    if (!(ymax > 0.0f)) ymax = 4096.0f;
    /* un NaN échoue à toute comparaison : il est retenu ici */
    return x > -xmax && x < xmax && y > -ymax && y < ymax && z > -1e9f && z < 1e9f;
}

#define APPLE_V(base, ptrs, i) ((ptrs) ? (ptrs)[i] : VTX(base, i))

static int apple_batch_ok(PCtx *p, const void *base, const unsigned char **ptrs, long n)
{
    long i;
    for (i = 0; i < n; i++)
        if (!apple_vtx_ok(p, APPLE_V(base, ptrs, i)))
            return 0;
    return 1;
}

enum { AK_TRIS, AK_STRIP, AK_FAN, AK_QUADS, AK_QUADSTRIP, AK_POLY,
       AK_LINES, AK_LINESTRIP, AK_LINELOOP, AK_POINTS };

/* Recompose le lot en primitives indépendantes de `per` sommets, retire
 * celles qui portent un sommet malsain, et rend le reste à Apple par la
 * procédure indépendante (Triangles, Lines, Points). Verrou tenu à l'appel ;
 * lâché ici, avant le rendu d'Apple. Rend ce que rendent les a_*. */
static long apple_guard(PCtx *p, void *ctx, long flags, int kind,
                        const void *base, const unsigned char **ptrs,
                        const unsigned char *hub, long n)
{
    const unsigned char **v = 0;
    unsigned char *buf = 0;
    long i, np = 0, good = 0, cap;
    int per = (kind >= AK_LINES && kind <= AK_LINELOOP) ? 2 : kind == AK_POINTS ? 1 : 3;
    int slot = per == 3 ? PROC_RenderTriangles : per == 2 ? PROC_RenderLines : PROC_RenderPoints;
    proc4 real;

    cap = (n > 0 ? n * 2 + 2 : 0) * per;
    v = cap ? (const unsigned char **)malloc((size_t)cap * sizeof(*v)) : 0;
    if (v) {
#define ADD3(a, b, c) do { v[np * 3] = (a); v[np * 3 + 1] = (b); v[np * 3 + 2] = (c); np++; } while (0)
#define ADD2(a, b)    do { v[np * 2] = (a); v[np * 2 + 1] = (b); np++; } while (0)
#define VV(i)         APPLE_V(base, ptrs, i)
        switch (kind) {
        case AK_TRIS:
            for (i = 0; i + 2 < n; i += 3) ADD3(VV(i), VV(i + 1), VV(i + 2));
            break;
        case AK_STRIP:
            for (i = 0; i + 2 < n; i++)
                if (i & 1) ADD3(VV(i + 1), VV(i), VV(i + 2));
                else       ADD3(VV(i), VV(i + 1), VV(i + 2));
            break;
        case AK_FAN:
            for (i = 0; i + 1 < n; i++) ADD3(hub, VV(i), VV(i + 1));
            break;
        case AK_QUADS:
            for (i = 0; i + 3 < n; i += 4) {
                ADD3(VV(i), VV(i + 1), VV(i + 2));
                ADD3(VV(i), VV(i + 2), VV(i + 3));
            }
            break;
        case AK_QUADSTRIP:
            for (i = 0; i + 3 < n; i += 2) {
                ADD3(VV(i), VV(i + 1), VV(i + 3));
                ADD3(VV(i), VV(i + 3), VV(i + 2));
            }
            break;
        case AK_POLY:
            for (i = 1; i + 1 < n; i++) ADD3(VV(0), VV(i), VV(i + 1));
            break;
        case AK_LINES:
            for (i = 0; i + 1 < n; i += 2) ADD2(VV(i), VV(i + 1));
            break;
        case AK_LINESTRIP:
        case AK_LINELOOP:
            for (i = 0; i + 1 < n; i++) ADD2(VV(i), VV(i + 1));
            if (kind == AK_LINELOOP && n > 1) ADD2(VV(n - 1), VV(0));
            break;
        case AK_POINTS:
            for (i = 0; i < n; i++) { v[np] = VV(i); np++; }
            break;
        }
#undef ADD3
#undef ADD2
#undef VV
        for (i = 0; i < np; i++) {
            int k, ok = 1;
            for (k = 0; k < per; k++)
                if (!apple_vtx_ok(p, v[i * per + k]))
                    ok = 0;
            if (ok) {
                if (i != good)
                    memcpy(&v[good * per], &v[i * per], (size_t)per * sizeof(*v));
                good++;
            }
        }
    }
    no(NO_APPLE_NAN, (unsigned long)(np - good), (unsigned long)kind);
    G.n_geomdrop += np - good;
    real = (proc4)fallback(p, slot, 1, 1);
    if (good && real)
        buf = (unsigned char *)malloc((size_t)good * per * GLD_VERTEX_SIZE);
    if (buf)
        for (i = 0; i < good * per; i++)
            memcpy(buf + i * GLD_VERTEX_SIZE, v[i], GLD_VERTEX_SIZE);
    pthread_mutex_unlock(&G.mu);
    if (buf)
        real(ctx, buf, good * per, flags);
    free(buf);
    free(v);
    return 0;
}

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
    if (p && !apple_batch_ok(p, verts, 0, n))
        return apple_guard(p, ctx, flags, AK_TRIS, verts, 0, 0, n);
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
    if (p && !apple_batch_ok(p, verts, 0, n))
        return apple_guard(p, ctx, flags, AK_STRIP, verts, 0, 0, n);
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
    if (p && (!apple_vtx_ok(p, (const unsigned char *)hub) || !apple_batch_ok(p, verts, 0, n)))
        return apple_guard(p, ctx, flags, AK_FAN, verts, 0, (const unsigned char *)hub, n);
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
    if (p && !apple_batch_ok(p, verts, 0, n))
        return apple_guard(p, ctx, flags, AK_QUADS, verts, 0, 0, n);
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
    if (p && !apple_batch_ok(p, verts, 0, n))
        return apple_guard(p, ctx, flags, AK_QUADSTRIP, verts, 0, 0, n);
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
    if (p && !apple_batch_ok(p, verts, 0, n))
        return apple_guard(p, ctx, flags, AK_POLY, verts, 0, 0, n);
    real = p ? (proc4)fallback(p, PROC_RenderPolygon, 1, 1) : 0;
    pthread_mutex_unlock(&G.mu);
    return real ? real(ctx, verts, n, flags) : 0;
}

/* Garde de faute des procédures à tableau de pointeurs (voir proc_jmp).
 * P2 (relecture du 24/09) : armée VERROU TENU (sigsetjmp après le lock), et
 * seulement autour des lectures de la mémoire de l'application (calcul des
 * primitives, apple_batch_ok, apple_ptrs_touch) ; jamais autour de
 * begin_*, fallback(), apple_guard() (malloc/free) ni du rendu d'Apple
 * real(...) — une faute là reste la leur. Désarmée avant tout unlock.
 * P3 : le fil qui arme est noté (proc_thr). */
#define PROC_GUARD_ARM()    do { proc_thr = pthread_self(); proc_jmp_on = 1; } while (0)
#define PROC_GUARD_DISARM() do { proc_jmp_on = 0; } while (0)

/* Lit chaque pointeur et les deux bouts de chaque sommet, sous garde, avant
 * apple_guard : apple_batch_ok s'arrête au premier sommet malsain, et
 * apple_guard (hors garde) relit tout le lot. */
static void apple_ptrs_touch(const unsigned char **ptrs, long n)
{
    volatile unsigned char sink = 0;
    long i;
    for (i = 0; i < n; i++) {
        const unsigned char *v = ptrs[i];
        sink = v[0];
        sink = v[GLD_VERTEX_SIZE - 1];
    }
    (void)sink;
}

/* Retour de faute d'une procédure à pointeurs : verrou tenu, garde déjà
 * désarmée par le crochet. Primitive jetée, verrou rendu. I8 (relecture du
 * 24/09) : si le lot hôte était ouvert (`opened`), des primitives ont pu
 * partir avant la faute — l'hôte devient la référence, comme dans end_tris. */
static long proc_fault(PCtx *p, int opened, const char *name, long n)
{
    static unsigned long told;
    proc_jmp_on = 0;
    if (p && opened) {
        p->color = HOST_NEWER;
        if (writes_depth(p))
            p->depth = HOST_NEWER;
    }
    if (told < 5) {
        told++;
        gl_note("PROC %s : faute de lecture (n %ld) : primitive jetee\n", name, n);
    }
    G.n_geomdrop++;
    G.n_dropped_fault++;
    pthread_mutex_unlock(&G.mu);
    return 0;
}

static long a_polygon_ptr(void *ctx, void *vptrs, long n, long flags)
{
    PCtx *p;
    Batch b;
    long i;
    int ok;
    proc4 real;
    const unsigned char **pp = (const unsigned char **)vptrs;
    volatile int opened = 0;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (sigsetjmp(proc_jmp, 0) != 0)
        return proc_fault(p, opened, "a_polygon_ptr", n);
    if (p && begin_tris(p, &b)) {
        opened = 1;
        PROC_GUARD_ARM();
        for (i = 1; i + 1 < n; i++)
            tri(&b, pp[0], pp[i], pp[i + 1], pp[0]);
        PROC_GUARD_DISARM();
        end_tris(&b);
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    if (p) {
        PROC_GUARD_ARM();
        ok = apple_batch_ok(p, 0, pp, n);
        if (!ok)
            apple_ptrs_touch(pp, n);
        PROC_GUARD_DISARM();
        if (!ok)
            return apple_guard(p, ctx, flags, AK_POLY, 0, pp, 0, n);
    }
    real = p ? (proc4)fallback(p, PROC_RenderPolygonPtr, 1, 1) : 0;
    pthread_mutex_unlock(&G.mu);
    return real ? real(ctx, vptrs, n, flags) : 0;
}

/* ───────────────────────────── lignes et points ───────────────────────────── */

typedef long (*lp_fn)(void *, void *, long, long);

/* Enveloppe commune : `body` émet les primitives si le lot a pu s'ouvrir.
   `ptrs` (constante) : tableau de pointeurs, lectures sous la garde proc_jmp
   (P2, relecture du 24/09 : armée verrou tenu, hors fallback/apple_guard/real). */
#define LP_PROC(name, slot, lines, kind, ptrs, body)                            \
static long name(void *ctx, void *verts, long n, long flags)                     \
{                                                                               \
    PCtx *p;                                                                    \
    Batch b;                                                                    \
    long i;                                                                     \
    int ok;                                                                     \
    lp_fn real;                                                                 \
    volatile int opened = 0;                                                    \
    pthread_mutex_lock(&G.mu);                                                  \
    p = find_ctx(ctx);                                                          \
    if (ptrs) {                                                                 \
        if (sigsetjmp(proc_jmp, 0) != 0)                                        \
            return proc_fault(p, opened, #name, n);                             \
    }                                                                           \
    if (p && begin_lp(p, &b, lines)) {                                          \
        opened = 1;                                                             \
        if (ptrs)                                                               \
            PROC_GUARD_ARM();                                                   \
        body                                                                    \
        if (ptrs)                                                               \
            PROC_GUARD_DISARM();                                                \
        p->color = HOST_NEWER;                                                  \
        if (writes_depth(p))                                                    \
            p->depth = HOST_NEWER;                                              \
        pthread_mutex_unlock(&G.mu);                                            \
        return 0;                                                               \
    }                                                                           \
    if (p) {                                                                    \
        if (ptrs)                                                               \
            PROC_GUARD_ARM();                                                   \
        ok = apple_batch_ok(p, (ptrs) ? 0 : verts,                              \
                            (ptrs) ? (const unsigned char **)verts : 0, n);     \
        if (ptrs) {                                                             \
            if (!ok)                                                            \
                apple_ptrs_touch((const unsigned char **)verts, n);             \
            PROC_GUARD_DISARM();                                                \
        }                                                                       \
        if (!ok)                                                                \
            return apple_guard(p, ctx, flags, kind, (ptrs) ? 0 : verts,         \
                               (ptrs) ? (const unsigned char **)verts : 0, 0, n); \
    }                                                                           \
    real = p ? (lp_fn)fallback(p, slot, 1, 1) : 0;                              \
    pthread_mutex_unlock(&G.mu);                                                \
    return real ? real(ctx, verts, n, flags) : 0;                               \
}

#define PP(i) (((const unsigned char **)verts)[i])

/* segments indépendants : la couleur plate est celle du second sommet */
LP_PROC(a_lines, PROC_RenderLines, 1, AK_LINES, 0,
    for (i = 0; i + 1 < n; i += 2)
        seg(&b, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 1));)
LP_PROC(a_linestrip, PROC_RenderLineStrip, 1, AK_LINESTRIP, 0,
    for (i = 0; i + 1 < n; i++)
        seg(&b, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 1));)
/* boucle : le segment de fermeture prend la couleur du premier sommet */
LP_PROC(a_lineloop, PROC_RenderLineLoop, 1, AK_LINELOOP, 0,
    for (i = 0; i + 1 < n; i++)
        seg(&b, VTX(verts, i), VTX(verts, i + 1), VTX(verts, i + 1));
    if (n > 1)
        seg(&b, VTX(verts, n - 1), VTX(verts, 0), VTX(verts, 0));)
/* (ctx, pointeurs, n, mode) : paires de pointeurs */
LP_PROC(a_lines_ptr, PROC_RenderLinesPtr, 1, AK_LINES, 1,
    for (i = 0; i + 1 < n; i += 2)
        seg(&b, PP(i), PP(i + 1), PP(i + 1));)
LP_PROC(a_points, PROC_RenderPoints, 0, AK_POINTS, 0,
    for (i = 0; i < n; i++)
        pt(&b, VTX(verts, i));)
LP_PROC(a_points_ptr, PROC_RenderPointsPtr, 0, AK_POINTS, 1,
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
        if (rest)
            lazy_sync(p);               /* le Clear d'Apple va servir */
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
/* F7 : `ok`, le rectangle (x, y) et la date de la dernière vérification sont
   passés dans la PCtx ; ne restent ici que les propriétés de l'ÉCRAN, qui sont
   bien partagées — et relues à chaque image (Q6). */
static struct {
    int            init, enabled, windowed, over_cursor;
    char           why[96];             /* pourquoi la voie directe est coupée */
    unsigned char *base;
    unsigned long  rowbytes, w, h, bpp;
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
    void         *(*best_mode)(unsigned long, unsigned long, unsigned long, unsigned long, int *);
    int           (*switch_mode)(unsigned long, void *);
} D;

/* P5 — LE JOURNAL D'APPOINT, DERRIÈRE UNE VARIABLE D'ENVIRONNEMENT.
 *
 * gl_note écrivait /tmp/pomppc-gl.log SANS interrupteur, depuis le
 * WindowServer et loginwindow — donc en root, et dans un répertoire 1777 :
 * n'importe quel utilisateur y plaçait un lien symbolique et nous lui
 * écrivions le fichier de son choix. Et chaque appel refaisait
 * fopen/fclose, à chaque gldAttachDrawable.
 *
 * Maintenant : rien n'est écrit sans POMPPC_GL_NOTE=<chemin>, le fichier est
 * ouvert UNE fois (pthread_once), et il est CRÉÉ par nous — O_EXCL refuse un
 * fichier existant, O_NOFOLLOW refuse un lien symbolique. Un chemin déjà pris
 * est donc un refus, pas une écriture ailleurs.
 */
static FILE *note_fp;
static pthread_once_t note_once = PTHREAD_ONCE_INIT;

static void note_open(void)
{
    const char *path = getenv("POMPPC_GL_NOTE");
    int fd;
    if (!path || !*path)
        return;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return;
    note_fp = fdopen(fd, "w");
    if (!note_fp) {
        close(fd);
        return;
    }
    setvbuf(note_fp, 0, _IOLBF, 0);
}

/* 24/09/2026 — crochet de plantage. DOOM 3 attrape SIGBUS/SIGSEGV lui-même,
 * appelle Quit() → notre gldDeleteTexture → verrou déjà tenu par le fil
 * fautif : le processus se fige, sans rapport CrashReporter. Ce crochet
 * écrit d'abord dans le journal (POMPPC_GL_NOTE) le signal, l'adresse
 * fautive, le PC, LR, la base du plugin et une pile brute (chaîne r1 / LR
 * sauvegardé à +8), puis remet le gestionnaire précédent : le jeu reprend
 * la main comme avant. Fonctions sûres en signal seulement (write). */
static struct sigaction crash_prev[2];
static void crash_hex(char *d, unsigned long v)
{
    static const char h[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; i--) { d[i] = h[v & 15]; v >>= 4; }
    d[8] = 0;
}
/* P4 (relecture du 24/09) : base du plugin et chemin du .crash calculés à
   l'installation (ni dyld ni getenv dans le gestionnaire) ; in_crash coupe
   la récursion (SA_NODEFER : une faute DANS le gestionnaire le relance). */
static volatile int in_crash;
static unsigned long crash_base;
static char crash_path[480];
static void crash_handler(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    char line[480], hx[9];
    unsigned long pc = 0, lr = 0, sp = 0, dar = 0, base = crash_base, i, nsp;
    int fd;
    /* P3 (relecture du 24/09) : ne revenir dans une garde que sur le fil qui
       l'a armée ; la faute d'un autre fil suit l'enregistrement normal. */
    if (sig_jmp_on && pthread_equal(pthread_self(), sig_thr)) {
        sig_jmp_on = 0;
        sig_fault_n++;
        siglongjmp(sig_jmp, 1);
    }
    if (proc_jmp_on && pthread_equal(pthread_self(), proc_thr)) {
        proc_jmp_on = 0;
        proc_fault_n++;
        siglongjmp(proc_jmp, 1);
    }
    if (pack_jmp_on && pthread_equal(pthread_self(), pack_thr)) {
        pack_jmp_on = 0;
        pack_fault_addr = (unsigned long)(si ? si->si_addr : 0);
        pack_fault_n++;
        siglongjmp(pack_jmp, 1);
    }
    /* P4 : réentrance — rendre la main au gestionnaire précédent D'ABORD ;
       la faute se reproduit au retour et c'est lui qui la prend. */
    if (in_crash) {
        sigaction(sig, &crash_prev[sig == SIGBUS ? 0 : 1], 0);
        return;
    }
    in_crash = 1;
    sigaction(sig, &crash_prev[sig == SIGBUS ? 0 : 1], 0);   /* le jeu reprendra */
    if (uc && uc->uc_mcontext) {
        pc  = uc->uc_mcontext->ss.srr0;
        lr  = uc->uc_mcontext->ss.lr;
        sp  = uc->uc_mcontext->ss.r1;
        dar = uc->uc_mcontext->es.dar;
    }
    /* Fichier À PART (<note>.crash) : gl_note tient le journal par un FILE*
       en écriture simple, et son tampon, vidé à la sortie du jeu, ÉCRASAIT
       le début de l'enregistrement écrit ici (Prey, 24/09 : « CRASH signal »
       et la pile disparus sous les lignes « attach » de l'arrêt). */
    fd = -1;
    if (crash_path[0])
        fd = open(crash_path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        strcpy(line, "CRASH signal ");
        line[13] = '0' + (sig % 10); line[14] = ' '; line[15] = 0;
        strcat(line, "pc "); crash_hex(hx, pc); strcat(line, hx);
        strcat(line, " lr "); crash_hex(hx, lr); strcat(line, hx);
        strcat(line, " dar "); crash_hex(hx, dar); strcat(line, hx);
        strcat(line, " addr "); crash_hex(hx, (unsigned long)(si ? si->si_addr : 0)); strcat(line, hx);
        strcat(line, " base "); crash_hex(hx, base); strcat(line, hx);
        strcat(line, " off "); crash_hex(hx, pc - base); strcat(line, hx);
        strcat(line, "\n");
        write(fd, line, strlen(line));
        /* pile brute : 16 retours. P4 : r1 aligné à 16 et strictement
           croissant, sinon arrêt (une pile corrompue ne fait plus fauter). */
        strcpy(line, "CRASH pile :");
        for (i = 0; i < 16 && sp > 0x1000 && sp < 0xc0000000UL && (sp & 15) == 0; i++) {
            unsigned long ret = *(unsigned long *)(sp + 8);
            strcat(line, " "); crash_hex(hx, ret); strcat(line, hx);
            if (ret >= base && base && ret < base + 0x100000) {
                strcat(line, "(+"); crash_hex(hx, ret - base); strcat(line, hx); strcat(line, ")");
            }
            nsp = *(unsigned long *)sp;
            if (nsp <= sp)
                break;
            sp = nsp;
            if (strlen(line) > 440) break;
        }
        strcat(line, "\n");
        write(fd, line, strlen(line));
        /* dernier empaquetage de tableaux : où en était-on ? */
        strcpy(line, "CRASH pack i "); crash_hex(hx, dbg_vtx_i); strcat(line, hx);
        strcat(line, " n "); crash_hex(hx, dbg_vtx_n); strcat(line, hx);
        strcat(line, " vmin "); crash_hex(hx, dbg_vmin); strcat(line, hx);
        strcat(line, " words "); crash_hex(hx, dbg_words); strcat(line, hx);
        strcat(line, " fmt "); crash_hex(hx, dbg_fmt); strcat(line, hx);
        strcat(line, " vao "); crash_hex(hx, (unsigned long)dbg_vao); strcat(line, hx);
        strcat(line, "\n"); write(fd, line, strlen(line));
        if (dbg_plan) {
            const unsigned long *pl = (const unsigned long *)dbg_plan;   /* VaAttr[] brut */
            unsigned long na = pl[(sizeof(VaAttr) / 4) * (QGPU_VF_GEN_MAX + QGPU_MAX_UNITS + 5)];
            for (i = 0; i < na && i < 32; i++) {
                const unsigned long *at = pl + i * (sizeof(VaAttr) / 4);
                strcpy(line, "CRASH attr src "); crash_hex(hx, at[0]); strcat(line, hx);
                strcat(line, " stride "); crash_hex(hx, at[1]); strcat(line, hx);
                strcat(line, " bpc "); crash_hex(hx, at[2]); strcat(line, hx);
                strcat(line, " src_n "); crash_hex(hx, at[3]); strcat(line, hx);
                strcat(line, " dst_n "); crash_hex(hx, at[4]); strcat(line, hx);
                strcat(line, " type "); crash_hex(hx, at[6]); strcat(line, hx);
                strcat(line, "\n"); write(fd, line, strlen(line));
            }
        }
        close(fd);
    }
    in_crash = 0;           /* gestionnaire précédent déjà remis pour `sig` */
}
static void crash_hook_install(void)
{
    struct sigaction sa;
    const char *path = getenv("POMPPC_GL_NOTE");
    unsigned long i, n;
    /* P4 (relecture du 24/09) : tout ce qui touche dyld ou l'environnement se
       fait ici, pas dans le gestionnaire. */
    n = _dyld_image_count();
    for (i = 0; i < n; i++) {
        const char *nm = _dyld_get_image_name(i);
        if (nm && strstr(nm, "GLDriver-POMPPC")) {
            crash_base = (unsigned long)_dyld_get_image_header(i);
            break;
        }
    }
    crash_path[0] = 0;
    if (path && strlen(path) < sizeof(crash_path) - 8) {
        strcpy(crash_path, path);
        strcat(crash_path, ".crash");
    }
    /* P1 (relecture du 24/09) : les gardes de faute valent même sans journal ;
       seul l'enregistrement dépend de POMPPC_GL_NOTE. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &sa, &crash_prev[0]);
    sigaction(SIGSEGV, &sa, &crash_prev[1]);
}

static void gl_note(const char *fmt, ...)
{
    va_list ap;
    pthread_once(&note_once, note_open);
    if (!note_fp)
        return;
    va_start(ap, fmt);
    vfprintf(note_fp, fmt, ap);
    va_end(ap);
    /* Les notes sont rares et précieuses : un jeu tué (kill, bus error) sans
       vidage perdait tout ce qui suivait le dernier bloc (23/09/2026). */
    fflush(note_fp);
}

static void direct_noop(void)
{
}

/* P7 : cible de secours d'un trampoline dont le contexte a disparu. Rend 0,
 * la seule valeur sûre pour les procédures qui rendent quelque chose. */
static long proc_dead(void)
{
    return 0;
}

static const char *direct_why(void)
{
    return D.why;
}

/* F5 — direct_init posait D.init = 1 AVANT ses dix-huit dlsym, hors verrou :
 * un autre fil voyait D « initialisé » et appelait D.main_display() encore
 * nul. Elle ne tourne plus qu'une fois (pthread_once, qui garantit aussi que
 * tout est visible au retour), et D.init n'est posé qu'EN DERNIER. */
static pthread_once_t direct_once = PTHREAD_ONCE_INIT;
static void direct_init(void);
static void direct_invalidate(void);    /* verrou tenu */

static void direct_init_once(void)
{
    pthread_once(&direct_once, direct_init);
}

static void direct_init(void)
{
    const char *e = getenv("POMPPC_GL_DIRECT");
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
    D.best_mode       = dlsym(RTLD_DEFAULT, "CGDisplayBestModeForParameters");
    D.switch_mode     = dlsym(RTLD_DEFAULT, "CGDisplaySwitchToMode");
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
    D.init = 1;                         /* F5 : EN DERNIER, et rien après */
}

/* aglSetFullScreen commute QFB en 1555. On préfère rester en 32 bits : la
 * présentation directe et le tampon hôte sont nativement xRGB. */
static int display_switch_32(unsigned long did)
{
    int exact = 0;
    void *mode;
    unsigned long w, h;
    if (!D.bits_per_pixel || D.bits_per_pixel(did) == 32)
        return 1;
    if (!D.best_mode || !D.switch_mode || !D.pixels_wide || !D.pixels_high)
        return 0;
    w = D.pixels_wide(did);
    h = D.pixels_high(did);
    mode = D.best_mode(did, 32, w, h, &exact);
    if (!mode)
        return 0;
    D.switch_mode(did, mode);
    return D.bits_per_pixel(did) == 32;
}

/* Tiger's glsAssignDrawable rejects kind 54 unconditionally. Its kind 53
 * takes {width, height, rowbytes, address} and retains the address, not the
 * descriptor. Use an owned back buffer so software fallbacks never draw into
 * scanout. Apple still owns depth/stencil and all its drawable bookkeeping.
 * ctx+e1 is the double-buffer flag: temporarily clear its offscreen veto;
 * the public pixel format and context keep their double-buffer semantics. */
long pomppc_attach_fullscreen(void *ctx)
{
    PCtx *p;
    unsigned long did, w, h, bits, desc[4];
    unsigned char *buf, double_buffer;
    long result;
    direct_init_once();                 /* F5 : hors G.mu, mais une seule fois */
    if (!D.main_display || !D.pixels_wide || !D.pixels_high ||
        !D.bits_per_pixel || !D.base_address || !D.bytes_per_row)
        return 10005;
    did = D.main_display();
    display_switch_32(did);
    w = D.pixels_wide(did); h = D.pixels_high(did);
    /* aglSetFullScreen choisit la profondeur d'écran d'après le pixel format.
       Si le 32 bits a été refusé, on attache quand même en 16 (1555). */
    bits = GLD_U32(ctx, CTX_COLOR_BITS);
    {
        unsigned long dbpp = D.bits_per_pixel(did);
        if (!w || !h || w > 16384 || h > 16384 ||
            (dbpp != 32 && dbpp != 16) || !D.base_address(did) ||
            bits < 16 || bits > 32)
            return 10005;
        gl_note("fullscreen attach %lux%lu display %lu ctx-bits %lu\n",
                w, h, dbpp, bits);
    }
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    buf = p ? calloc(h, w * 4) : NULL;
    if (!buf) {
        pthread_mutex_unlock(&G.mu);
        return 10016; /* kCGLBadAlloc */
    }
    drain_all();
    desc[0] = w; desc[1] = h; desc[2] = w * 4; desc[3] = (unsigned long)buf;
    double_buffer = GLD_U8(ctx, 0xe1);
    GLD_U8(ctx, 0xe1) = 0;
    /* F4 — SEUL appel au code d'Apple qui se faisait verrou tenu. glsAssignDrawable
       fait des allers-retours Mach avec le WindowServer (des dizaines de
       millisecondes, parfois bien plus) : tous les fils GL du processus y
       restaient bloqués, et une réentrance par gldUpdateDispatch donnait un
       blocage franc (G.mu n'est pas récursif). On relâche donc autour.
       Ce qui est protégé pendant la fenêtre : `p` ne peut pas disparaître
       (détruire un contexte pendant qu'on lui attache un drawable est déjà
       interdit par CGL), mais tout ce qu'on a lu de G est RELU après, et on
       revalide `p` par find_ctx. */
    pthread_mutex_unlock(&G.mu);
    result = pomppc_call_real(GLD_AttachDrawable, (long)ctx, 53, (long)desc, 0, 0, 0, 0, 0);
    pthread_mutex_lock(&G.mu);
    GLD_U8(ctx, 0xe1) = double_buffer;
    p = find_ctx(ctx);                  /* revalidation après le relâchement */
    if (!p) {
        free(buf);
        pthread_mutex_unlock(&G.mu);
        return result;
    }
    /* glsAssignDrawable releases the previous drawable even on failure. */
    free(p->fullscreen_buf);
    p->fullscreen_buf = NULL;
    direct_invalidate();
    if (result >= 0 && result <= 3) {
        p->fullscreen_buf = buf;
        p->fullscreen_w = w; p->fullscreen_h = h;
        /* Le descripteur type 53 est 32 bits ; Apple peut laisser 16 d'après
           le pixel format. Aligner le contexte sur le tampon réel. */
        if (GLD_U32(ctx, CTX_COLOR_BITS) == 16)
            GLD_U32(ctx, CTX_COLOR_BITS) = 32;
        pomppc_log("POMPPC: fullscreen drawable %lux%lu, software back buffer\n", w, h);
        gl_note("fullscreen attach ok %lux%lu ctx-bits now %lu\n",
                w, h, GLD_U32(ctx, CTX_COLOR_BITS));
    } else {
        free(buf);
        gl_note("fullscreen attach FAIL %ld\n", result);
    }
    pthread_mutex_unlock(&G.mu);
    return result;
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
 * Remplit p->d_x / p->d_y (F7). */
static int direct_window_ok(PCtx *p)
{
    unsigned char *gd = (unsigned char *)GLD_U32(p->ctx, 4);
    long cid, wid, sid, list[96], n = 0, i;
    CgRect wr, sr, o;
    CgPoint cur;

#define WHY(...) (snprintf(D.why, sizeof(D.why), __VA_ARGS__), 0)
    if (!D.windowed || !gd)
        return WHY("window: SPI missing or no drawable");
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
                return WHY("surface not found: %ld surfaces, first %lx (wid %lx sid %lx)",
                           sn, sn > 0 ? sl[0] : 0, wid, sid);
        }
        if (e1 || e2)
            return WHY("bounds refused: window %ld, surface %ld (cid %lx wid %lx sid %lx)",
                       e1, e2, cid, wid, sid);
    }
    if ((unsigned long)sr.w != p->sw || (unsigned long)sr.h != p->sh)
        return WHY("surface %gx%g at %g,%g != drawable %lux%lu", sr.w, sr.h, sr.x, sr.y,
                   p->sw, p->sh);
    p->d_x = (long)(wr.x + sr.x);       /* F7 : le rectangle est CELUI DE p */
    p->d_y = (long)(wr.y + sr.y);
    if (p->d_x < 0 || p->d_y < 0 || p->d_x + p->sw > D.w || p->d_y + p->sh > D.h)
        return WHY("off-screen (%ld,%ld)", p->d_x, p->d_y);
    /* fenêtres à l'écran, de l'avant vers l'arrière : rien au-dessus ne doit toucher */
    if (D.onscreen_list(cid, 0, 96, list, &n) != 0 || n <= 0 || n > 96)
        return WHY("window list refused (n %ld)", n);
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
            rects_touch(p->d_x, p->d_y, p->sw, p->sh, &o) && !window_invisible(cid, list[i]))
            return WHY("covered by window %lx (%g,%g %gx%g), rank %ld/%ld, alpha %g",
                       list[i], o.x, o.y, o.w, o.h, i, n, window_alpha(cid, list[i]));
    if (i == n)
        return WHY("window %lx missing from list (%ld windows)", wid, n);
    /* Curseur visible dans la surface : écrire par-dessus l'efface jusqu'à son
       prochain mouvement. Tant qu'il BOUGE (menus, interface), chemin normal ;
       immobile d'une vérification à l'autre (jeu qui le ramène au centre à
       chaque image, comme Marble Blast, ou souris au repos), voie directe. */
    if (!D.over_cursor && D.cursor_visible() && D.cursor_location(cid, &cur) == 0) {
        int still = cur.x == D.cur_x && cur.y == D.cur_y;
        D.cur_x = cur.x; D.cur_y = cur.y;
        o.x = cur.x - 32; o.y = cur.y - 32; o.w = 64; o.h = 64;
        if (!still && rects_touch(p->d_x, p->d_y, p->sw, p->sh, &o))
            return WHY("cursor moving in surface (%g,%g)", cur.x, cur.y);
    }
    D.why[0] = 0;
    return 1;
#undef WHY
}

/* Mémoire vidéo où présenter p, ou 0 (échange normal). Verrou tenu. */
/* Le mode d'écran (ou l'attachement) a changé : plus aucune décision de
   présentation directe n'est valable, pour aucun contexte. Verrou tenu. */
static void direct_invalidate(void)
{
    PCtx *q;
    for (q = G.list; q; q = q->next) {
        q->d_ok = 0;
        q->d_checked_at = 0;
    }
}

static unsigned char *direct_target(PCtx *p, unsigned long *rowbytes)
{
    unsigned long did, rb, bpp, dw, dh;
    unsigned char *base;

    direct_init_once();                 /* F5 */
    if (!D.enabled)
        return 0;
    if (!D.main_display || !D.base_address || !D.bytes_per_row ||
        !D.bits_per_pixel || !D.pixels_wide || !D.pixels_high)
        return 0;
    /* Q6 — BASE, PAS ET PROFONDEUR SONT RELUS À CHAQUE IMAGE. Les garder
       DIRECT_RECHECK images faisait écrire dans l'ANCIENNE mémoire vidéo
       après un changement de mode (aglSetFullScreen, changement de
       résolution, Exposé) : jusqu'à neuf images d'écriture sauvage, hors de
       toute VRAM valide. Les six appels CoreGraphics sont des lectures d'une
       structure déjà en mémoire ; ce sont les vérifications COÛTEUSES (liste
       des fenêtres, processus au premier plan) qui restent à la cadence de
       DIRECT_RECHECK. */
    did = D.main_display();
    base = D.base_address(did);
    rb   = D.bytes_per_row(did);
    bpp  = D.bits_per_pixel(did);
    dw   = D.pixels_wide(did);
    dh   = D.pixels_high(did);
    if (!base || !dw || !dh || (bpp != 32 && bpp != 16) ||
        rb < dw * (bpp == 16 ? 2 : 4)) {
        snprintf(D.why, sizeof(D.why), "display %lu-bit, %lux%lu pitch %lu", bpp, dw, dh, rb);
        direct_invalidate();
        return 0;
    }
    if (base != D.base || rb != D.rowbytes || bpp != D.bpp ||
        dw != D.w || dh != D.h) {
        D.base = base; D.rowbytes = rb; D.bpp = bpp; D.w = dw; D.h = dh;
        direct_invalidate();            /* le mode a changé : tout revérifier */
    }
    /* F7 : le verdict et le rectangle sont PAR CONTEXTE — partagés, l'image de
       B partait dans le rectangle de A. */
    if (!p->d_checked_at || G.n_frames - p->d_checked_at >= DIRECT_RECHECK) {
        Psn front, me;
        unsigned char same = 0;
        int ok, full;
        ok = D.front_process(&front) == 0 && D.current_process(&me) == 0 &&
             D.same_process(&front, &me, &same) == 0 && same;
        if (!ok)
            snprintf(D.why, sizeof(D.why), "not frontmost");
        full = ok && p->sw == D.w && p->sh == D.h &&
               (!D.menubar_visible || !D.menubar_visible() || p->fullscreen_buf);
        if (full) {
            p->d_x = p->d_y = 0;
            ok = 1;
        } else {
            ok = ok && direct_window_ok(p);
            if (ok)
                ok = 2;                 /* en fenêtre */
        }
        if (ok != p->d_ok)
            pomppc_log("POMPPC: présentation directe %s (%lux%lu en %ld,%ld ; écran %lux%lu)\n",
                       ok == 1 ? "plein écran" : ok ? "en fenêtre" : "coupée",
                       p->sw, p->sh, p->d_x, p->d_y, D.w, D.h);
        p->d_ok = ok;
        p->d_checked_at = G.n_frames ? G.n_frames : 1;
    }
    if (!p->d_ok)
        return 0;
    /* Le rectangle a été validé contre l'écran COURANT (juste au-dessus si le
       mode a bougé) : on le revérifie tout de même, c'est une multiplication. */
    if (p->d_x < 0 || p->d_y < 0 ||
        (unsigned long)p->d_x + p->sw > D.w || (unsigned long)p->d_y + p->sh > D.h) {
        p->d_ok = 0;
        return 0;
    }
    /* En fenêtre, la mémoire de la fenêtre côté WindowServer n'est plus à jour :
       un échange normal de temps en temps la rafraîchit (déplacement, Exposé,
       capture d'écran). */
    if (p->d_ok == 2 && G.n_frames % DIRECT_REFRESH == 0)
        return 0;
    *rowbytes = D.rowbytes;
    return D.base + p->d_y * D.rowbytes + p->d_x * (D.bpp == 16 ? 2 : 4);
}

/* xRGB8888 (mot PowerPC 00RRGGBB) → xRGB1555 big-endian, format QFB
 * « milliers de couleurs ». */
static void pack_xrgb32_to_555(unsigned char *dst, unsigned long dpitch,
                               const unsigned char *src, unsigned long spitch,
                               unsigned long w, unsigned long h)
{
    unsigned long y, x;
    for (y = 0; y < h; y++) {
        const unsigned long *s = (const unsigned long *)(src + y * spitch);
        unsigned short *d = (unsigned short *)(dst + y * dpitch);
        for (x = 0; x < w; x++) {
            unsigned long p = s[x];
            d[x] = (unsigned short)(((p >> 9) & 0x7c00u) |
                                    ((p >> 6) & 0x03e0u) |
                                    ((p >> 3) & 0x001fu));
        }
    }
}

/* Tampon arrière 32 bits → scanout (32 bits ou 1555). Verrou tenu. */
static void present_sw_fullscreen(PCtx *p)
{
    unsigned long did, y, pitch, bpp, w, h, spitch, dw, dh, cw, ch;
    unsigned char *dst, *src;
    if (!p->fullscreen_buf || !D.main_display || !D.base_address ||
        !D.bytes_per_row || !D.bits_per_pixel || !D.pixels_wide || !D.pixels_high)
        return;
    did = D.main_display();
    w = p->fullscreen_w;
    h = p->fullscreen_h;
    dst = D.base_address(did);
    pitch = D.bytes_per_row(did);
    bpp = D.bits_per_pixel(did);
    dw = D.pixels_wide(did);
    dh = D.pixels_high(did);
    sync_to_sw_locked(p, 0);
    drain_all();
    src = sw_color(p);
    if (!src)
        src = p->fullscreen_buf;
    spitch = sw_rowbytes(p);
    cw = w < dw ? w : dw;
    ch = h < dh ? h : dh;
    if (!dst || !src || !cw || !ch)
        return;
    if (bpp == 32 && pitch >= cw * 4 && spitch >= cw * 4) {
        for (y = 0; y < ch; ++y)
            memcpy(dst + y * pitch, src + y * spitch, cw * 4);
    } else if (bpp == 16 && pitch >= cw * 2 && spitch >= cw * 4) {
        pack_xrgb32_to_555(dst, pitch, src, spitch, cw, ch);
    }
}

static unsigned char *present_stage;
static unsigned long  present_stage_w, present_stage_h;
static int            present_stage_ready;

static unsigned char *ensure_present_stage(unsigned long w, unsigned long h)
{
    if (present_stage && present_stage_w == w && present_stage_h == h)
        return present_stage;
    free(present_stage);
    present_stage = calloc(h, w * 4);
    present_stage_w = w;
    present_stage_h = h;
    present_stage_ready = 0;
    return present_stage;
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
 * v13 — SURF_PRESENT : l'hôte écrit lui-même dans la VRAM QFB. L'attente
 * de l'image n−1 reste (barrière de la soumission précédente), mais le G4
 * ne recopie plus un pixel. Sans device v13, l'ancien chemin (relecture +
 * memcpy, pack 1555 ici) est inchangé.
 */
static int present_direct(PCtx *p)
{
    unsigned char *vram;
    unsigned long rowbytes, w, h;
    if (G.state <= 0 || p->broken || p->surf < 0 || p->color == SW_NEWER)
        return 0;
    /* L'image PRÉCÉDENTE part maintenant en mémoire vidéo. */
    wait_half(G.cur ^ 1);
    vram = direct_target(p, &rowbytes);
    if (!vram)
        return 0;
    w = p->sw;
    h = p->sh;
    if (G.scanout && D.bpp == 16) {
        queue_present(p, (unsigned long)(vram - D.base), rowbytes, QGPU_PF_RGB1555);
        flush();
        G.n_direct++;
        p->direct_at = G.n_frames;
        return 1;
    }
    if (G.scanout && (D.bpp == 32 || D.bpp == 24)) {
        queue_present(p, (unsigned long)(vram - D.base), rowbytes, QGPU_PF_XRGB8888);
        flush();
        G.n_direct++;
        p->direct_at = G.n_frames;
        return 1;
    }
    if (D.bpp == 16) {
        unsigned char *st = ensure_present_stage(w, h);
        if (!st)
            return 0;
        if (present_stage_ready)
            pack_xrgb32_to_555(vram, rowbytes, st, w * 4, w, h);
        /* P16 : si la relecture n'a pas pu être mise en file (arène pleine),
           on n'a rien présenté — il faut rendre 0 pour qu'Apple échange. */
        if (!queue_readback_to(p, 0, st, w * 4))
            return 0;
        present_stage_ready = 1;
    } else {
        if (!queue_readback_to(p, 0, vram, rowbytes))
            return 0;
    }
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
    return slot == PROC_Proc68 || slot == PROC_Proc6c ||
           slot == PROC_RenderVertexArray || slot == PROC_RenderVertexBuffer;
}

/* CopyTexSubImage2D (ctx, tex, target, level, xoff, yoff, x, y, w, h) :
 * w,h viennent de la pile (9e et 10e, a[8] a[9] via le trampoline).
 * Origine GL en bas à gauche → origine hôte en haut à gauche. */
static int try_copy_tex(PCtx *p, unsigned long *a)
{
    void *drvtex = (void *)a[1];
    unsigned long target = a[2], level = a[3];
    unsigned long xoff = a[4], yoff = a[5];
    long sx = (long)a[6], sy = (long)a[7];
    unsigned long w = a[8], h = a[9];
    unsigned long hy, *c;
    PTex *t;

    /* 24/09/2026, DOOM 3 (_currentRender, 5 copies par image, chacune un
       repli avec relecture) : GLEngine appelle ici avec a[2] = 0 et une
       disposition d'un mot plus longue — (ctx, tex, 0, niveau, xoff, yoff,
       zoff, x, y, w, h) : relevé sur les copies de bord (xoff 640, x 639,
       w 1, h 480). Avec a[2] = GL_TEXTURE_2D c'est la disposition d'origine
       (UT2004, 20/09). */
    if (target == 0) {
        target = 0x0DE1;
        sx = (long)a[7]; sy = (long)a[8]; w = a[9]; h = a[10];
        if (a[6] != 0)
            return 0;                   /* zoff : face de cube ou 3D, à Apple */
    }

    static unsigned long told, told_args;
    if (told_args < 4) {
        told_args++;
        gl_note("COPY_TEX args : %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
                a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
    }
#define COPYTEX_NO(why) do { if (told < 6) { told++; gl_note("COPY_TEX refuse (%s) : tex %p cible %lx niv %lu " \
        "dst %lu,%lu src %ld,%ld %lux%lu surf %lux%lu couleur %d\n", why, drvtex, target, level, xoff, yoff, \
        sx, sy, w, h, p->sw, p->sh, p->color); } return 0; } while (0)
    if (!G.pixops || target != 0x0DE1)
        COPYTEX_NO("pixops/cible");
    if (w == 0 || h == 0)
        return 1;
    if (p->color == SW_NEWER)
        COPYTEX_NO("couleur logicielle plus recente");
    if (!accel_ok(p) || !ensure_surface(p) || p->surf < 0)
        COPYTEX_NO("pas de surface hote");
    if (sx < 0 || sy < 0 ||
        (unsigned long)sx + w > p->sw || (unsigned long)sy + h > p->sh)
        COPYTEX_NO("hors surface");
    t = intern_tex(drvtex);
    if (!t)
        COPYTEX_NO("texture inconnue");
    /* Destination d'une copie d'écran (_currentRender de DOOM 3, reflets
       d'UT2004) : ses niveaux invité ne sont jamais à jour, et souvent pas
       même lisibles (glTexImage2D(NULL)) — on crée des niveaux noirs sur
       l'hôte, la copie les écrase. */
    if (t->qtex < 0 || t->dirty) {
        upload_blank = 1;
        if (!texture_uploadable(t) || !upload_texture(p, t) || t->qtex < 0) {
            upload_blank = 0;
            COPYTEX_NO("texture non televersable");
        }
        upload_blank = 0;
    }
    /* I1 (relecture du 24/09) : l'empreinte calculée sous upload_blank ne
       porte pas les texels ; au dessin suivant elle différait et le niveau
       invité (noir) repartait par-dessus la copie hôte — les vitres de
       DOOM 3 (_currentRender) montraient du noir. */
    t->lv0_sig = tex_lv0_sig(t);
    hy = p->sh - (unsigned long)sy - h;
    c = reserve(p, QGPU_LEN_COPY_TEX);
    c[0] = QGPU_CMD_HDR(QGPU_OP_COPY_TEX, QGPU_LEN_COPY_TEX);
    c[1] = t->qtex;
    c[2] = QGPU_TT_2D;
    c[3] = level;
    c[4] = xoff;
    c[5] = yoff;
    c[6] = 0;
    c[7] = (unsigned long)sx;
    c[8] = hy;
    c[9] = w;
    c[10] = h;
    t->dirty = 0;
    /* P15 : le niveau de l'INVITÉ n'a pas été mis à jour — le contenu à jour
       n'existe que sur l'hôte. Sans ce drapeau, l'éviction LRU (128
       emplacements) détruisait la texture, et le rechargement la remplissait
       avec l'ANCIEN contenu de l'invité : reflets et ombres périmés. */
    t->host_only = 1;
    G.n_copytex++;
    return 1;
}

/* ReadPixels (ctx, x, y, w, h, format, type, pixels) : 8 arguments, tous
 * dans r3–r10. Rectangle : RGBA/RGB octet, profondeur FLOAT, stencil octet.
 *
 * P1 — GARDE INTÉRIMAIRE SUR L'EMPAQUETAGE.
 *
 * On fabrique ici le pas de destination alors que c'est l'APPLICATION qui le
 * fixe, par GL_PACK_ALIGNMENT / GL_PACK_ROW_LENGTH / GL_PACK_SKIP_*. Les
 * offsets CTX_PACK_* ne sont PAS relevés (seuls les CTX_UNPACK_* le sont,
 * :76-80) : tant qu'ils ne le seront pas, on ne peut pas les lire, et il est
 * hors de question de les deviner.
 *
 * En attendant, on n'accepte que les cas où NOTRE pas est le PLUS PETIT
 * possible, c'est-à-dire le pas serré : w·bpp multiple de 4. Le pas réel de
 * l'application est alors ≥ au nôtre quel que soit son alignement (1, 2, 4
 * ou 8), donc on n'écrit JAMAIS au-delà de son tampon. C'était le bogue :
 * GL_RGB avec align 1 (SDL, captures d'écran) débordait de 3·(h−1) octets,
 * et le stencil, écrit serré alors que le défaut d'OpenGL est align 4,
 * décalait l'image sans que l'application ait rien réglé.
 *
 * CE QUI RESTE EXPOSÉ, et qui demande les offsets CTX_PACK_* : une image
 * OBLIQUE (pas fausse en mémoire, mais décalée) si l'application a posé
 * PACK_ALIGNMENT = 8 avec une largeur impaire en RGBA, ou un PACK_ROW_LENGTH
 * plus petit que w. Aucun de ces cas ne sort du tampon.
 */
static int try_read_pixels(PCtx *p, unsigned long *a)
{
    long x = (long)a[1], y = (long)a[2], w = (long)a[3], h = (long)a[4];
    unsigned long fmt = a[5], type = a[6];
    unsigned char *pixels = (unsigned char *)a[7];
    unsigned long hy, off, *c, row, col, bpp, rowb, pix;
    const unsigned long *src;

    if (!G.pixops || !pixels)
        return 0;
    if (!accel_ok(p) || p->surf < 0)
        return 0;
    if (w <= 0 || h <= 0)
        return 1;
    if (x < 0 || y < 0 ||
        (unsigned long)x + (unsigned long)w > p->sw ||
        (unsigned long)y + (unsigned long)h > p->sh)
        return 0;
    hy = p->sh - (unsigned long)y - (unsigned long)h;
    if (type == 0x1406 && fmt == 0x1902) {       /* GL_DEPTH_COMPONENT / FLOAT */
        const float *s;
        float *dst;
        if (p->depth == SW_NEWER)
            return 0;
        rowb = ((unsigned long)w * 4UL + 3UL) & ~3UL;
        if (!arena_alloc((unsigned long)w * (unsigned long)h * 4, &off))
            return 0;
        c = reserve(p, QGPU_LEN_SURF_XFER);
        c[0] = QGPU_CMD_HDR(QGPU_OP_DEPTH_READBACK, QGPU_LEN_SURF_XFER);
        c[1] = p->surf; c[2] = G.q.base + off; c[3] = (unsigned long)w * 4;
        c[4] = (unsigned long)x; c[5] = hy;
        c[6] = (unsigned long)w; c[7] = (unsigned long)h;
        G.n_readbacks++;
        G.n_pixread++;
        flush();
        drain_all();
        for (row = 0; row < (unsigned long)h; row++) {
            s = (const float *)(G.q.win + off +
                                ((unsigned long)h - 1 - row) * (unsigned long)w * 4);
            dst = (float *)(pixels + row * rowb);
            memcpy(dst, s, (unsigned long)w * 4);
        }
        return 1;
    }
    if (type == 0x1401 && fmt == 0x1901) {       /* GL_STENCIL_INDEX / UBYTE */
        const unsigned long *s;
        unsigned char *d;
        if (!p->stencil || p->depth == SW_NEWER)
            return 0;
        if ((unsigned long)w % 4UL)              /* P1 : bpp = 1 */
            return 0;
        if (!arena_alloc((unsigned long)w * (unsigned long)h * 4, &off))
            return 0;
        c = reserve(p, QGPU_LEN_SURF_XFER);
        c[0] = QGPU_CMD_HDR(QGPU_OP_STENCIL_READBACK, QGPU_LEN_SURF_XFER);
        c[1] = p->surf; c[2] = G.q.base + off; c[3] = (unsigned long)w * 4;
        c[4] = (unsigned long)x; c[5] = hy;
        c[6] = (unsigned long)w; c[7] = (unsigned long)h;
        G.n_readbacks++;
        G.n_pixread++;
        flush();
        drain_all();
        for (row = 0; row < (unsigned long)h; row++) {
            s = (const unsigned long *)(G.q.win + off +
                                        ((unsigned long)h - 1 - row) * (unsigned long)w * 4);
            d = pixels + row * (unsigned long)w;
            for (col = 0; col < (unsigned long)w; col++)
                d[col] = (unsigned char)s[col];
        }
        return 1;
    }
    if (p->color == SW_NEWER)
        return 0;
    if (type != 0x1401 || (fmt != 0x1908 && fmt != 0x1907))
        return 0;
    bpp = (fmt == 0x1907) ? 3UL : 4UL;
    if (((unsigned long)w * bpp) % 4UL)          /* P1 : GL_RGB, align 1 */
        return 0;
    rowb = (unsigned long)w * bpp;               /* = le pas serré, par la garde */
    if (!arena_alloc((unsigned long)w * (unsigned long)h * 4, &off))
        return 0;
    c = reserve(p, QGPU_LEN_SURF_XFER);
    c[0] = QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER);
    c[1] = p->surf;
    c[2] = G.q.base + off;
    c[3] = (unsigned long)w * 4;
    c[4] = (unsigned long)x;
    c[5] = hy;
    c[6] = (unsigned long)w;
    c[7] = (unsigned long)h;
    G.n_readbacks++;
    G.n_pixread++;
    flush();
    drain_all();
    src = (const unsigned long *)(G.q.win + off);
    for (row = 0; row < (unsigned long)h; row++) {
        const unsigned long *s = src + ((unsigned long)h - 1 - row) * (unsigned long)w;
        unsigned char *d = pixels + row * rowb;
        for (col = 0; col < (unsigned long)w; col++) {
            pix = s[col];
            d[0] = (unsigned char)(pix >> 16);
            d[1] = (unsigned char)(pix >> 8);
            d[2] = (unsigned char)pix;
            if (bpp == 4)
                d[3] = (unsigned char)(pix >> 24);
            d += bpp;
        }
    }
    return 1;
}

/* Texture 2D jetable (NEAREST, CLAMP_TO_EDGE) pour coller un rectangle
 * de pixels sur l'hôte. Identifiant hors de la table intern_tex : alloc_tex_id
 * n'évince que les PTex, le bit tex_used nous protège. */
static int pix_scratch(PCtx *p, unsigned long w, unsigned long h)
{
    unsigned long *c;
    if (w == 0 || h == 0 || w > QGPU_MAX_TEX_DIM || h > QGPU_MAX_TEX_DIM)
        return 0;
    if (G.pixtex < 0) {
        G.pixtex = alloc_tex_id(p);
        if (G.pixtex < 0)
            return 0;
        c = reserve(p, QGPU_LEN_TEX_CREATE3 + 4 * QGPU_LEN_TEX_PARAM);
        c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_CREATE3, QGPU_LEN_TEX_CREATE3);
        c[1] = G.pixtex;
        c[2] = QGPU_TT_2D;
        c += QGPU_LEN_TEX_CREATE3;
        c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_PARAM, QGPU_LEN_TEX_PARAM);
        c[1] = G.pixtex; c[2] = QGPU_TP_MIN_FILTER; c[3] = 0x2600; /* GL_NEAREST */
        c += QGPU_LEN_TEX_PARAM;
        c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_PARAM, QGPU_LEN_TEX_PARAM);
        c[1] = G.pixtex; c[2] = QGPU_TP_MAG_FILTER; c[3] = 0x2600;
        c += QGPU_LEN_TEX_PARAM;
        c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_PARAM, QGPU_LEN_TEX_PARAM);
        c[1] = G.pixtex; c[2] = QGPU_TP_WRAP_S; c[3] = 0x812F; /* CLAMP_TO_EDGE */
        c += QGPU_LEN_TEX_PARAM;
        c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_PARAM, QGPU_LEN_TEX_PARAM);
        c[1] = G.pixtex; c[2] = QGPU_TP_WRAP_T; c[3] = 0x812F;
        G.pixtex_w = G.pixtex_h = 0;
    }
    return 1;
}

/* Doubles f1..f6 saved by the trampoline at a[12] (FBASE). Bitmap
 * receives xorig/yorig there; integer args stay in r3–r10. */
static float tramp_f(const unsigned long *a, int n)
{
    union { unsigned long w[2]; double d; } u;
    u.w[0] = a[12 + 2 * n];
    u.w[1] = a[13 + 2 * n];
    return (float)u.d;
}

/* Unset bitmap bits must not write. Force GL_GREATER 0 for this draw;
 * the next send_state restores the application's alpha test. */
static void pix_punch_zero(PCtx *p)
{
    unsigned long *c = reserve(p, 3 * QGPU_LEN_SET_STATE);
    c[0] = QGPU_CMD_HDR(QGPU_OP_SET_STATE, QGPU_LEN_SET_STATE);
    c[1] = QGPU_SK_ALPHA_TEST;
    c[2] = 1;
    p->st[QGPU_SK_ALPHA_TEST] = 1;
    c += QGPU_LEN_SET_STATE;
    c[0] = QGPU_CMD_HDR(QGPU_OP_SET_STATE, QGPU_LEN_SET_STATE);
    c[1] = QGPU_SK_ALPHA_FUNC;
    c[2] = 0x0204;                      /* GL_GREATER */
    p->st[QGPU_SK_ALPHA_FUNC] = 0x0204;
    c += QGPU_LEN_SET_STATE;
    c[0] = QGPU_CMD_HDR(QGPU_OP_SET_STATE, QGPU_LEN_SET_STATE);
    c[1] = QGPU_SK_ALPHA_REF;
    c[2] = 0;
    p->st[QGPU_SK_ALPHA_REF] = 0;
}

/* Quad fenêtre : a[1] est le sommet interne déjà transformé (V_X/V_Y pixels,
 * origine en bas). GLEngine le remplit dans _glDrawPixels_Exec (gctx+0x2e0
 * × viewport → r26). REPLACE, pas le texenv de l'appli.
 * punch : test alpha GREATER 0 (bits à 0 de glBitmap). */
static int pix_quad(PCtx *p, const unsigned char *vtx, unsigned long w, unsigned long h,
                    int punch)
{
    TexInfo ti;
    PTex fake;
    Batch b;
    unsigned char v[4][GLD_VERTEX_SIZE];
    const unsigned char *pv[3];
    float x, y, z;
    int i;

    if (!vtx || !accel_ok(p) || !ensure_surface(p) || G.pixtex < 0)
        return 0;
    x = GLD_F32(vtx, V_X);
    y = GLD_F32(vtx, V_Y);
    z = GLD_F32(vtx, V_Z);
    if (!(x > -1e6f && x < 1e6f && y > -1e6f && y < 1e6f))
        return 0;
    memset(&ti, 0, sizeof(ti));
    memset(&fake, 0, sizeof(fake));
    fake.qtex = G.pixtex;
    ti.u[0].t = &fake;
    ti.u[0].env_mode = 0x1E01;          /* GL_REPLACE */
    ti.u[0].combine = QGPU_COMBINE_DEFAULT;
    ti.u[0].combine_src = QGPU_COMBINE_SRC_DEFAULT;
    begin_common(p, &b, &ti);
    if (punch)
        pix_punch_zero(p);
    b.kind = RK_TRI_TEX;
    b.ptatt = 0;
    memset(v, 0, sizeof(v));
    for (i = 0; i < 4; i++) {
        float s = (i == 1 || i == 2) ? 1.0f : 0.0f;
        float t = (i == 0 || i == 1) ? 1.0f : 0.0f; /* bas d'écran = 1re ligne GL */
        GLD_F32(v[i], V_X) = x + (s ? (float)w : 0.0f);
        GLD_F32(v[i], V_Y) = y + ((i >= 2) ? (float)h : 0.0f);
        GLD_F32(v[i], V_Z) = z;
        GLD_F32(v[i], V_COLOR) = 1.0f;
        GLD_F32(v[i], V_COLOR + 4) = 1.0f;
        GLD_F32(v[i], V_COLOR + 8) = 1.0f;
        GLD_F32(v[i], V_COLOR + 12) = 1.0f;
        GLD_F32(v[i], V_FOG) = 1.0f;
        GLD_F32(v[i], V_TEX0) = s;
        GLD_F32(v[i], V_TEX0 + 4) = t;
        GLD_F32(v[i], V_TEX0 + 12) = 1.0f;
    }
    pv[0] = v[0]; pv[1] = v[1]; pv[2] = v[2];
    prim(&b, 3, pv, v[0]);
    pv[0] = v[0]; pv[1] = v[2]; pv[2] = v[3];
    prim(&b, 3, pv, v[0]);
    close_run();
    p->color = HOST_NEWER;
    if (writes_depth(p))
        p->depth = HOST_NEWER;
    return 1;
}

/* Raster pos → pixel rectangle fully on the surface (and inside the scissor). */
static int pix_dest(PCtx *p, const unsigned char *vtx, unsigned long w, unsigned long h,
                    unsigned long *dx, unsigned long *dy)
{
    float fx, fy;
    long x, y;
    unsigned char *g;

    if (!vtx)
        return 0;
    fx = GLD_F32(vtx, V_X);
    fy = GLD_F32(vtx, V_Y);
    if (!(fx > -1e6f && fx < 1e6f && fy > -1e6f && fy < 1e6f))
        return 0;
    x = (long)fx;
    y = (long)fy;
    if (x < 0 || y < 0)
        return 0;
    if ((unsigned long)x + w > p->sw || (unsigned long)y + h > p->sh)
        return 0;
    g = gls(p);
    if (GLD_U8(g, GS_SCISSOR)) {
        long sx = I32(g, GS_SCISSOR_RECT), sy = I32(g, GS_SCISSOR_RECT + 4);
        long sw = I32(g, GS_SCISSOR_RECT + 8), sh = I32(g, GS_SCISSOR_RECT + 12);
        if (x < sx || y < sy || x + (long)w > sx + sw || y + (long)h > sy + sh)
            return 0;
    }
    *dx = (unsigned long)x;
    *dy = (unsigned long)y;
    return 1;
}

/* Host blit of depth or stencil. Readback then upload in one stream so
 * overlapping source/dest stay correct. Protocol y is top-left. */
static int copy_ds_rect(PCtx *p, unsigned long sx, unsigned long sy,
                        unsigned long dx, unsigned long dy,
                        unsigned long w, unsigned long h, int sten)
{
    unsigned long off, *c;
    unsigned long shy = p->sh - sy - h, dhy = p->sh - dy - h;

    if (!arena_alloc(w * h * 4, &off))
        return 0;
    c = reserve(p, QGPU_LEN_SURF_XFER);
    c[0] = QGPU_CMD_HDR(sten ? QGPU_OP_STENCIL_READBACK : QGPU_OP_DEPTH_READBACK,
                        QGPU_LEN_SURF_XFER);
    c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
    c[4] = sx; c[5] = shy; c[6] = w; c[7] = h;
    c = reserve(p, QGPU_LEN_SURF_XFER);
    c[0] = QGPU_CMD_HDR(sten ? QGPU_OP_STENCIL_UPLOAD : QGPU_OP_DEPTH_UPLOAD,
                        QGPU_LEN_SURF_XFER);
    c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
    c[4] = dx; c[5] = dhy; c[6] = w; c[7] = h;
    G.n_readbacks++;
    G.n_uploads++;
    return 1;
}

/* DrawPixels DEPTH_COMPONENT FLOAT or STENCIL_INDEX UNSIGNED_BYTE.
 * DEPTH_UPLOAD writes with GL_ALWAYS: only when the app's depth test is
 * off or ALWAYS, otherwise Apple. Same idea for stencil. */
static int try_draw_ds(PCtx *p, unsigned long *a)
{
    const unsigned char *vtx = (const unsigned char *)a[1];
    unsigned long w = a[2], h = a[3], fmt = a[4], type = a[5];
    const unsigned char *pixels = (const unsigned char *)a[6];
    unsigned long dx, dy, dhy, off, *c, x, y, align, row_px, skip_rows, skip_px, rowb;
    unsigned char *g;

    if (fmt != 0x1902 && fmt != 0x1901)
        return 0;
    if (fmt == 0x1902 && type != 0x1406)
        return 0;
    if (fmt == 0x1901 && type != 0x1401)
        return 0;
    if (!G.pixops || !G.v10)
        return 0;
    if (w == 0 || h == 0)
        return 1;
    if (!pixels || !vtx)
        return 0;
    if (!accel_ok(p) || !ensure_surface(p) || p->surf < 0)
        return 0;
    g = gls(p);
    if (fmt == 0x1902) {
        if (!GLD_U8(g, GS_DEPTH_MASK))
            return 1;
        if (GLD_U8(g, GS_DEPTH_TEST) && U16(g, GS_DEPTH_FUNC) != 0x0207)
            return 0;
        if (p->depth == SW_NEWER)
            sync_to_host(p, 0, 1);
    } else {
        unsigned long wmask;
        if (!p->stencil)
            return 0;
        wmask = GLD_U32(g, GS_STENCIL_WMASK) & 0xFF;
        if (wmask == 0)
            return 1;
        if (wmask != 0xFF)
            return 0;
        if (stencil_active(p) && U16(g, GS_STENCIL_FUNC) != 0x0207)
            return 0;
        if (p->depth == SW_NEWER)
            sync_to_host(p, 0, 1);
    }
    if (!pix_dest(p, vtx, w, h, &dx, &dy))
        return 0;
    align = GLD_U32(p->ctx, CTX_UNPACK_ALIGNMENT);
    if (align != 1 && align != 2 && align != 4 && align != 8)
        align = 4;
    row_px = GLD_U32(p->ctx, CTX_UNPACK_ROW_LENGTH);
    if (row_px == 0)
        row_px = w;
    if (row_px < w)                     /* l'appli décrit moins large que le rectangle */
        return 0;
    skip_rows = GLD_U32(p->ctx, CTX_UNPACK_SKIP_ROWS);
    skip_px = GLD_U32(p->ctx, CTX_UNPACK_SKIP_PIXELS);
    if (fmt == 0x1902)
        rowb = (row_px * 4UL + align - 1UL) & ~(align - 1UL);
    else
        rowb = (row_px + align - 1UL) & ~(align - 1UL);
    if (rowb == 0)
        return 0;
    if (!arena_alloc(w * h * 4, &off))
        return 0;
    dhy = p->sh - dy - h;
    if (fmt == 0x1902) {
        for (y = 0; y < h; y++) {
            const float *s = (const float *)(pixels + (skip_rows + (h - 1 - y)) * rowb) + skip_px;
            memcpy(G.q.win + off + y * w * 4, s, w * 4);
        }
        c = reserve(p, QGPU_LEN_SURF_XFER);
        c[0] = QGPU_CMD_HDR(QGPU_OP_DEPTH_UPLOAD, QGPU_LEN_SURF_XFER);
        c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
        c[4] = dx; c[5] = dhy; c[6] = w; c[7] = h;
        G.n_uploads++;
        p->depth = HOST_NEWER;
    } else {
        for (y = 0; y < h; y++) {
            const unsigned char *s = pixels + (skip_rows + (h - 1 - y)) * rowb + skip_px;
            unsigned long *d = (unsigned long *)(G.q.win + off + y * w * 4);
            for (x = 0; x < w; x++)
                d[x] = s[x];
        }
        c = reserve(p, QGPU_LEN_SURF_XFER);
        c[0] = QGPU_CMD_HDR(QGPU_OP_STENCIL_UPLOAD, QGPU_LEN_SURF_XFER);
        c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
        c[4] = dx; c[5] = dhy; c[6] = w; c[7] = h;
        G.n_uploads++;
        p->sten_used = 1;
        p->depth = HOST_NEWER;
    }
    G.n_pixdraw++;
    return 1;
}

/* DrawPixels (ctx, sommet-fenêtre, w, h, format, type, pixels, 0).
 * Le sommet est r4 : _glDrawPixels_Exec y écrit x/y/z fenêtre avant l'appel. */
static int try_draw_pixels(PCtx *p, unsigned long *a)
{
    const unsigned char *vtx = (const unsigned char *)a[1];
    unsigned long w = a[2], h = a[3], fmt = a[4], type = a[5];
    const unsigned char *pixels = (const unsigned char *)a[6];
    unsigned long bpp, rowb, srcrow, align, row_px, skip_rows, skip_px, off, *c, y;
    unsigned char *dst;

    if (try_draw_ds(p, a))
        return 1;
    if (!G.pixops || !G.v10)
        return 0;
    if (w == 0 || h == 0)
        return 1;
    if (!pixels || !vtx)
        return 0;
    if (type != 0x1401 || (fmt != 0x1908 && fmt != 0x1907))
        return 0;
    if (p->color == SW_NEWER || !accel_ok(p) || !ensure_surface(p))
        return 0;
    if (!pix_scratch(p, w, h))
        return 0;
    bpp = (fmt == 0x1907) ? 3UL : 4UL;
    /* MESURÉ en VM le 22/09/2026 (scène drawpack, sonde gl_note) : l'application
       pose GL_UNPACK_ALIGNMENT = 1 et ce mot vaut encore 4 — les offsets
       CTX_UNPACK_* ne sont pas (ou plus) ceux que _glPixelStorei_Exec écrit pour
       DrawPixels, et lire 40 octets par ligne de 39 brouille l'image (38/0/0/24/1
       pixels par couleur). Tant qu'ils ne sont pas relevés (diffstate sur un
       glPixelStorei, comme pour CTX_PACK_*), le chemin hôte ne prend que les
       images dont la ligne SERRÉE est déjà multiple de 4 : quel que soit
       l'alignement réel (1, 2, 4 ou 8), le pas est alors le même. Les autres
       (GL_RGB de largeur non multiple de 4) partent en logiciel : exact. */
    if ((w * bpp) & 3UL)
        return 0;
    /* P2 — LA SOURCE A LE PAS DE L'APPLICATION, PAS LE NÔTRE. On lisait le
       tampon de l'application avec un pas aligné sur 4 codé en dur, alors que
       GL_UNPACK_ALIGNMENT / ROW_LENGTH / SKIP_* le fixent — try_draw_ds et
       try_bitmap les lisent depuis toujours, pas ce chemin-ci. Résultat :
       image oblique, et lecture au-delà du tampon source dès que l'application
       pose align 1 en GL_RGB (le cas de SDL). Le pas de DESTINATION, lui, est
       à nous : c'est l'arène. */
    align = GLD_U32(p->ctx, CTX_UNPACK_ALIGNMENT);
    if (align != 1 && align != 2 && align != 4 && align != 8)
        align = 4;
    row_px = GLD_U32(p->ctx, CTX_UNPACK_ROW_LENGTH);
    if (row_px == 0)
        row_px = w;
    if (row_px < w)
        return 0;
    skip_rows = GLD_U32(p->ctx, CTX_UNPACK_SKIP_ROWS);
    skip_px = GLD_U32(p->ctx, CTX_UNPACK_SKIP_PIXELS);
    srcrow = (row_px * bpp + align - 1UL) & ~(align - 1UL);
    if (srcrow == 0)
        return 0;
    rowb = (w * bpp + 3UL) & ~3UL;      /* destination : l'arène */
    if (!arena_alloc(h * rowb, &off))
        return 0;
    dst = G.q.win + off;
    /* Première ligne GL = bas de l'image → haut de la texture hôte. */
    for (y = 0; y < h; y++) {
        const unsigned char *s = pixels + (skip_rows + (h - 1 - y)) * srcrow
                                 + skip_px * bpp;
        memcpy(dst + y * rowb, s, w * bpp);
    }
    c = reserve(p, QGPU_LEN_TEX_IMAGE3);
    c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE3, QGPU_LEN_TEX_IMAGE3);
    c[1] = G.pixtex; c[2] = QGPU_TT_2D; c[3] = 0;
    c[4] = w; c[5] = h; c[6] = 1;
    c[7] = fmt; c[8] = fmt; c[9] = type;
    c[10] = G.q.base + off; c[11] = rowb; c[12] = 0;
    G.n_texuploads++;
    G.pixtex_w = w;
    G.pixtex_h = h;
    if (!pix_quad(p, vtx, w, h, 0))
        return 0;
    G.n_pixdraw++;
    return 1;
}

/* CopyPixels (ctx, sommet-fenêtre, x, y, w, h, type). COLOR, DEPTH, STENCIL. */
static int try_copy_pixels(PCtx *p, unsigned long *a)
{
    const unsigned char *vtx = (const unsigned char *)a[1];
    long sx = (long)a[2], sy = (long)a[3];
    unsigned long w = a[4], h = a[5], kind = a[6];
    unsigned long hy, off, *c, dx, dy;
    unsigned char *g;

    if (!G.pixops || !G.v10)
        return 0;
    if (w == 0 || h == 0)
        return 1;
    if (!vtx)
        return 0;
    if (kind == 0x1801 || kind == 0x1802) {     /* GL_DEPTH / GL_STENCIL */
        int sten = (kind == 0x1802);
        if (!accel_ok(p) || !ensure_surface(p) || p->surf < 0)
            return 0;
        if (sx < 0 || sy < 0 ||
            (unsigned long)sx + w > p->sw || (unsigned long)sy + h > p->sh)
            return 0;
        if (p->depth == SW_NEWER)
            return 0;
        g = gls(p);
        if (sten) {
            unsigned long wmask;
            if (!p->stencil)
                return 0;
            wmask = GLD_U32(g, GS_STENCIL_WMASK) & 0xFF;
            if (wmask == 0)
                return 1;
            if (wmask != 0xFF)
                return 0;
            if (stencil_active(p) && U16(g, GS_STENCIL_FUNC) != 0x0207)
                return 0;
        } else {
            if (!GLD_U8(g, GS_DEPTH_MASK))
                return 1;
            if (GLD_U8(g, GS_DEPTH_TEST) && U16(g, GS_DEPTH_FUNC) != 0x0207)
                return 0;
        }
        if (!pix_dest(p, vtx, w, h, &dx, &dy))
            return 0;
        if (!copy_ds_rect(p, (unsigned long)sx, (unsigned long)sy, dx, dy, w, h, sten))
            return 0;
        p->depth = HOST_NEWER;
        if (sten)
            p->sten_used = 1;
        G.n_pixdraw++;
        return 1;
    }
    if (kind != 0x1800)                 /* GL_COLOR */
        return 0;
    if (p->color == SW_NEWER || !accel_ok(p) || !ensure_surface(p) || p->surf < 0)
        return 0;
    if (sx < 0 || sy < 0 ||
        (unsigned long)sx + w > p->sw || (unsigned long)sy + h > p->sh)
        return 0;
    if (!pix_scratch(p, w, h))
        return 0;
    /* Niveau existant : une image noire suffit, COPY_TEX l'écrase. */
    if (G.pixtex_w != w || G.pixtex_h != h) {
        if (!arena_alloc(w * h * 4, &off))
            return 0;
        memset(G.q.win + off, 0, w * h * 4);
        c = reserve(p, QGPU_LEN_TEX_IMAGE3);
        c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE3, QGPU_LEN_TEX_IMAGE3);
        c[1] = G.pixtex; c[2] = QGPU_TT_2D; c[3] = 0;
        c[4] = w; c[5] = h; c[6] = 1;
        c[7] = 0x1908; c[8] = 0x1908; c[9] = 0x1401;
        c[10] = G.q.base + off; c[11] = w * 4; c[12] = 0;
        G.n_texuploads++;
        G.pixtex_w = w;
        G.pixtex_h = h;
    }
    hy = p->sh - (unsigned long)sy - h;
    c = reserve(p, QGPU_LEN_COPY_TEX);
    c[0] = QGPU_CMD_HDR(QGPU_OP_COPY_TEX, QGPU_LEN_COPY_TEX);
    c[1] = G.pixtex;
    c[2] = QGPU_TT_2D;
    c[3] = 0;
    c[4] = 0;
    c[5] = 0;
    c[6] = 0;
    c[7] = (unsigned long)sx;
    c[8] = hy;
    c[9] = w;
    c[10] = h;
    G.n_copytex++;
    if (!pix_quad(p, vtx, w, h, 0))
        return 0;
    G.n_pixdraw++;
    return 1;
}

/* RenderBitmap (ctx, sommet-fenêtre, w, h, bits) + f1/f2 = xorig/yorig.
 * _glBitmap_Exec remplit le sommet comme DrawPixels, passe xorig/yorig
 * en flottants, et ajoute xmove/ymove à gctx+0x2e0 APRÈS le retour :
 * consommer l'appel n'empêche pas l'avance de la position raster.
 * Bits à 1 → fragment de la couleur raster ; bits à 0 → rien. */
static int try_bitmap(PCtx *p, unsigned long *a)
{
    const unsigned char *vtx = (const unsigned char *)a[1];
    unsigned long w = a[2], h = a[3];
    const unsigned char *bits = (const unsigned char *)a[6];
    unsigned char vtx2[GLD_VERTEX_SIZE];
    unsigned long align, row_px, skip_rows, skip_px, rowb, off, x, y, *c;
    unsigned char *dst, lsb, cr, cg, cb, ca;
    float xorig, yorig;

    if (!G.pixops || !G.v10)
        return 0;
    if (w == 0 || h == 0)
        return 1;
    if (!vtx)
        return 0;
    if (!bits)
        return 1;
    if (p->color == SW_NEWER || !accel_ok(p) || !ensure_surface(p))
        return 0;
    if (!pix_scratch(p, w, h))
        return 0;

    align = GLD_U32(p->ctx, CTX_UNPACK_ALIGNMENT);
    if (align != 1 && align != 2 && align != 4 && align != 8)
        align = 4;
    row_px = GLD_U32(p->ctx, CTX_UNPACK_ROW_LENGTH);
    if (row_px == 0)
        row_px = w;
    skip_rows = GLD_U32(p->ctx, CTX_UNPACK_SKIP_ROWS);
    skip_px = GLD_U32(p->ctx, CTX_UNPACK_SKIP_PIXELS);
    lsb = GLD_U8(p->ctx, CTX_UNPACK_LSB_FIRST);
    rowb = ((row_px + 7UL) / 8UL + align - 1UL) & ~(align - 1UL);
    if (rowb == 0)
        return 0;

    xorig = tramp_f(a, 0);
    yorig = tramp_f(a, 1);
    memcpy(vtx2, vtx, GLD_VERTEX_SIZE);
    GLD_F32(vtx2, V_X) = GLD_F32(vtx, V_X) - xorig;
    GLD_F32(vtx2, V_Y) = GLD_F32(vtx, V_Y) - yorig;
    /* Color is captured at RasterPos into the vertex at gctx+0x4858−0x200,
     * not into the Bitmap scratch at 0x4854. Current glColor is gctx+0x2a0. */
    {
        unsigned long rp = GLD_U32(p->ctx, 0x4858);
        const float *rgba = (rp >= 0x200)
            ? (const float *)((const unsigned char *)(rp - 0x200) + V_COLOR)
            : (const float *)((const unsigned char *)p->ctx + 0x2a0);
        cr = (unsigned char)to_u8(rgba[0]);
        cg = (unsigned char)to_u8(rgba[1]);
        cb = (unsigned char)to_u8(rgba[2]);
        ca = (unsigned char)to_u8(rgba[3]);
    }

    if (!arena_alloc(w * h * 4, &off))
        return 0;
    dst = G.q.win + off;
    /* First GL row = bottom of bitmap → top of host texture. */
    for (y = 0; y < h; y++) {
        const unsigned char *src = bits + (skip_rows + (h - 1 - y)) * rowb;
        unsigned char *row = dst + y * w * 4;
        for (x = 0; x < w; x++) {
            unsigned long bi = skip_px + x;
            unsigned char byte = src[bi / 8];
            unsigned long bp = lsb ? (bi & 7) : (7 - (bi & 7));
            int on = (byte >> bp) & 1;
            row[x * 4 + 0] = cr;
            row[x * 4 + 1] = cg;
            row[x * 4 + 2] = cb;
            row[x * 4 + 3] = on ? ca : 0;
        }
    }
    c = reserve(p, QGPU_LEN_TEX_IMAGE3);
    c[0] = QGPU_CMD_HDR(QGPU_OP_TEX_IMAGE3, QGPU_LEN_TEX_IMAGE3);
    c[1] = G.pixtex; c[2] = QGPU_TT_2D; c[3] = 0;
    c[4] = w; c[5] = h; c[6] = 1;
    c[7] = 0x1908; c[8] = 0x1908; c[9] = 0x1401;
    c[10] = G.q.base + off; c[11] = w * 4; c[12] = 0;
    G.n_texuploads++;
    G.pixtex_w = w;
    G.pixtex_h = h;
    if (!pix_quad(p, vtx2, w, h, 1))
        return 0;
    G.n_pixdraw++;
    return 1;
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
        /* P7 — il y avait ici un abort(). Un pilote graphique ne tue JAMAIS le
           processus de son application : le cas se produisait vraiment, parce
           que pomppc_context_destroyed libérait la PCtx sans désinstaller les
           trampolines (c'est corrigé plus bas, mais le filet reste). On rend
           une cible inerte : le trampoline y saute, elle rend 0 — ce qui
           convient aux deux formes de procédure, celles qui ne rendent rien et
           celles où 0 veut dire « non pris en charge ». */
        static unsigned n_lost;
        pthread_mutex_unlock(&G.mu);
        if (n_lost++ < 4)
            fprintf(stderr, "POMPPC GL: proc %s for an unknown context %08lx, "
                    "call dropped\n", pomppc_proc_name(slot), a[0]);
        pomppc_log("POMPPC: procédure %s pour un contexte inconnu %08lx : appel jeté\n",
                   pomppc_proc_name(slot), a[0]);
        return (void *)proc_dead;
    }
    target = p->real[slot];
    /* Apple's RenderVertexArray can be only `li r3,0; blr`: a refusal,
       not a software draw. Synchronizing color/depth for that probe makes
       every indexed draw read back the surface, then upload it again in
       geom_begin. Check the actual PPC instructions rather than assume all
       OS revisions have the same implementation. Keep an A/B override. */
    if (slot == PROC_RenderVertexArray && target) {
        const unsigned long *code = (const unsigned long *)target;
        static int force_sync = -1;
        if (force_sync < 0)
            force_sync = getenv("POMPPC_GL_ARRAY_STUB_SYNC") != NULL;
        if (!force_sync && code[0] == 0x38600000UL && code[1] == 0x4e800020UL)
            kind = K_NONE;
    }
    /* Une image = un échange (0x60). Les vidages 0x58/0x5c (glFlush, glFinish)
       n'en sont pas : Marble Blast en fait un par image, et les compter doublait
       le débit affiché (vu en vrai). */
    if (slot == PROC_Swap60)
        stats_frame(p->ctx);
    if (slot == PROC_Swap60 && present_direct(p)) {
        pthread_mutex_unlock(&G.mu);
        return direct_noop;
    }
    if (slot == PROC_Swap60 && p->fullscreen_buf) {
        present_sw_fullscreen(p);
        pthread_mutex_unlock(&G.mu);
        return direct_noop;
    }
    /* glFlush / glFinish d'un contexte qui présente directement : l'application
       attend la fin du dessin, pas une copie dans le tampon invité (l'image
       part en mémoire vidéo à l'échange). Marble Blast fait un glFinish par
       image : sans ceci, chaque image était relue deux fois (vu en vrai).
       Plein écran 16 bits : present_direct est coupée, même règle. */
    if ((slot == PROC_Swap58 || slot == PROC_Swap5c) && p->color == HOST_NEWER &&
        (p->fullscreen_buf || (p->direct_at && G.n_frames - p->direct_at <= 1))) {
        if (G.ncmd)
            flush();
        pthread_mutex_unlock(&G.mu);
        return direct_noop;
    }
    if (slot == PROC_CopyTexSubImage && try_copy_tex(p, a)) {
        pthread_mutex_unlock(&G.mu);
        return direct_noop;
    }
    if (slot == PROC_ReadPixels && try_read_pixels(p, a)) {
        pthread_mutex_unlock(&G.mu);
        return direct_noop;
    }
    if (slot == PROC_DrawPixels && try_draw_pixels(p, a)) {
        pthread_mutex_unlock(&G.mu);
        return direct_noop;
    }
    if (slot == PROC_CopyPixels && try_copy_pixels(p, a)) {
        pthread_mutex_unlock(&G.mu);
        return direct_noop;
    }
    if (slot == PROC_RenderBitmap && try_bitmap(p, a)) {
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
            intern_tex((void *)a[1]);
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
static void hook_locked(PCtx *p, void **procs)
{
    int k;
    p->procs = procs;
    for (k = 0; k < PROC_COUNT; k++) {
        void *mine = install_for(k);
        if (!mine || procs[k] == mine)
            continue;
        if (!procs[k] && !proc_standalone(k))
            continue;                   /* rien à quoi se replier : on s'abstient */
        p->real[k] = procs[k];
        p->mine[k] = mine;
        procs[k] = mine;
    }
}

static void unhook_locked(PCtx *p, void **procs)
{
    int k;
    for (k = 0; k < PROC_COUNT; k++)
        if (p->mine[k] && procs[k] == p->mine[k])
            procs[k] = p->real[k];
}

void pomppc_hook_procs(void *ctx, void **procs)
{
    PCtx *p;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p)
        hook_locked(p, procs);
    pthread_mutex_unlock(&G.mu);
}

void pomppc_unhook_procs(void *ctx, void **procs)
{
    PCtx *p;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p)
        unhook_locked(p, procs);
    pthread_mutex_unlock(&G.mu);
}

/* ─────────── transmission paresseuse des dispatches au GLDriver d'Apple ───────────
 *
 * docs/re/dispatch-paresseux.md. gldUpdateDispatch (Apple, 10.4.6) ne fait
 * QUE préparer son propre rendu logiciel : recharger et convertir les
 * textures liées (gldLoadCurrentTexture → glgProcessPixels, S3TC décompressé),
 * recalculer ses données de rastérisation (ctx+0x22c..0x2a8, 0x6b4..0x6fc,
 * glrSetFunctions du module GLRaster) et, sous le bit 0x80, les pointeurs du
 * tampon de dessin et les trois procédures d'échange. Il ne lit que les mots
 * +0x00, +0x04 et +0x10 du bloc de changements, n'y écrit rien, et rend 4
 * (0x2720 si une allocation de tampon échoue). Rien de ce qu'il calcule n'est
 * lu hors de ses procédures de rastérisation — ni par GLEngine, ni par ses
 * autres points d'entrée gld*, ni par le plugin.
 *
 * Tant que le plugin dessine tout sur l'hôte, ce travail est perdu. On cumule
 * donc les masques (OU bit à bit, mot par mot) et on ne transmet que :
 *   — tout de suite, si le bit 0x80 (tampon de dessin) est là : le plugin lit
 *     ctx+0x94 aussitôt après (pomppc_after_draw_buffer_change), et check_
 *     draw_buffer à chaque dessin hôte ;
 *   — juste avant qu'une procédure d'Apple ne travaille : fallback(), le
 *     Clear partiel de a_clear. Tous les chemins vers une procédure d'Apple
 *     qui lit l'état passent par là (les emplacements que le plugin ne
 *     crochète pas ne portent chez Apple que des bouchons : Noop,
 *     BufferSubData, Begin/EndPrimitiveBuffer, RenderVertexBuffer/Array,
 *     ModifyTexSubImage, GenerateTexMipmaps, CopyTexSubImage) ;
 *   — avant gldInitDispatch et gldAttachDrawable (pomppc_lazy_flush).
 * Le verrou de géométrie (bit 0 du retour) ne dépend pas d'Apple :
 * pomppc_geom_dispatch tourne à chaque dispatch, transmis ou non.
 *
 * Tout se fait sous G.mu, dans le fil qui appelle pour CE contexte : jamais
 * de transmission pour le compte d'un autre contexte (GLEngine ne sérialise
 * qu'au sein d'un contexte). */
static long lazy_call(PCtx *p, const unsigned long *m)
{
    long r;
    void **procs = p->procs;
    /* Sous 0x80, Apple réécrit les cases d'échange 0x58/0x5c/0x60 : on lui
       rend sa table, puis on recrochète, comme gldUpdateDispatch. */
    if (procs)
        unhook_locked(p, procs);
    r = pomppc_call_real(GLD_UpdateDispatch, (long)p->ctx, (long)procs, (long)m, 0, 0, 0, 0, 0);
    if (procs)
        hook_locked(p, procs);
    if (r == 4) {
        p->lazy_ret = r;
    } else {
        static int told;
        if (told < 4) {
            told++;
            gl_note("LAZYAPPLE: gldUpdateDispatch d'Apple rend 0x%lx (masque %08lx)\n",
                    (unsigned long)r, m[0]);
        }
    }
    return r;
}

/* G.mu tenu. Rattrape Apple : un seul dispatch, masques cumulés. */
static void lazy_sync(PCtx *p)
{
    unsigned long m[LAZY_WORDS];
    if (!p->lazy_pending)
        return;
    memcpy(m, p->lazy_m, sizeof(m));
    memset(p->lazy_m, 0, sizeof(p->lazy_m));
    p->lazy_pending = 0;
    G.n_lazy_sync++;
    lazy_call(p, m);
}

/* gldUpdateDispatch. Rend 0 si la transmission paresseuse n'est pas en jeu
 * (l'appelant fait comme avant), 1 sinon, avec dans *ret ce qu'Apple a rendu
 * — ou ce qu'il rendrait : 4, constant hors échec d'allocation. L'appelant
 * garde pomppc_after_draw_buffer_change, le crochetage et le verdict. */
int pomppc_lazy_update(void *ctx, void **procs, unsigned long *chg, long *ret)
{
    PCtx *p;
    int k;
    if (!G.lazy || G.state <= 0 || !chg || !procs)
        return 0;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (!p || !p->procs) {
        /* pas encore de gldInitDispatch vu pour ce contexte : comme avant */
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    for (k = 0; k < LAZY_WORDS; k++)
        p->lazy_m[k] |= chg[k];
    p->lazy_pending = 1;
    p->procs = procs;
    if (chg[0] & 0x80) {
        unsigned long m[LAZY_WORDS];
        memcpy(m, p->lazy_m, sizeof(m));
        memset(p->lazy_m, 0, sizeof(p->lazy_m));
        p->lazy_pending = 0;
        G.n_lazy_eager++;
        *ret = lazy_call(p, m);
    } else {
        G.n_lazy_defer++;
        *ret = p->lazy_ret;
    }
    pthread_mutex_unlock(&G.mu);
    return 1;
}

/* Avant gldInitDispatch, gldAttachDrawable : Apple rattrape ce qu'on lui a tu. */
void pomppc_lazy_flush(void *ctx)
{
    PCtx *p;
    if (!G.lazy || G.state <= 0)
        return;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && p->procs)
        lazy_sync(p);
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
    p->lazy_ret = 4;                    /* ce que rend le gldInitDispatch d'Apple */
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
        /* P7 — LES TRAMPOLINES D'ABORD. gldDestroyContext nous appelle AVANT le
           destructeur d'Apple, et seul gldUpdateDispatch désinstallait nos
           procédures : la table de GLEngine pointait encore sur elles alors
           que la PCtx venait d'être libérée. Tout rappel de rastérisation
           tombait sur pomppc_proc_pre, find_ctx échouait, abort(). */
        if (p->procs) {
            int k;
            for (k = 0; k < PROC_COUNT; k++)
                if (p->mine[k] && p->procs[k] == p->mine[k])
                    p->procs[k] = p->real[k];
            p->procs = 0;
        }
        if (p->cfg)
            GLD_U32(p->cfg, 0x11c) = 0;     /* le descripteur meurt avec le contexte */
        /* v16 : les programmes de ce contexte meurent avec lui côté hôte
           (CTX_DESTROY) ; nos entrées ne doivent plus le désigner. */
        {
            int k;
            for (k = 0; k < PPROG_MAX; k++)
                if (pprog[k].ctx == ctx)
                    memset(&pprog[k], 0, sizeof(pprog[k]));
        }
        pend_close(p, 1);                   /* F2 */
        /* Mineur §8.1 : la série DRAW_RAW ouverte appartient peut-être à CE
           contexte. L'abandonner (raw_ctx = 0) perdait sa dernière primitive ;
           la FERMER l'émet tant que le contexte hôte existe encore. */
        if (G.raw_ctx == p || G.run_ctx == p) {
            close_run();
            close_raw();
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
        /* P7 : plus une seule référence à la PCtx qu'on libère. */
        if (G.run_ctx == p)
            G.run_ctx = 0;
        if (G.raw_ctx == p) {
            G.raw_ctx = 0;
            G.raw_count = 0;
        }
        if (G.bound == p)
            G.bound = 0;
        free(p->fullscreen_buf);
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
                any = queue_readback(p, 0, p->draw_seen);   /* P16 */
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
    gl_note("attach kind %ld -> %ld ctx %lux%lu bits %lu\n", kind, result,
            ctx ? GLD_U32(ctx, CTX_WIDTH) : 0, ctx ? GLD_U32(ctx, CTX_HEIGHT) : 0,
            ctx ? GLD_U32(ctx, CTX_COLOR_BITS) : 0);
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p && kind != 54 && result >= 0 && result <= 3) {
        drain_all();
        free(p->fullscreen_buf);
        p->fullscreen_buf = NULL;
        direct_invalidate();            /* F7/Q6 : plus aucun verdict n'est valable */
    }
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
 *   RendererInfo : +0x04 identifiant, +0x08 drapeaux
 *   pixel format : +0x00 suivant, +0x04 identifiant, +0x08 drapeaux (même codage)
 *
 * Drapeaux +0x08 (relevé GLDriver 0x65D / Rage128 0x2513 / GeForce3 0xA513,
 * docs/re/capacites-glengine.md §6) :
 *   0x0002 plein écran   — présent sur les vrais GPU, absent du logiciel
 *   0x0004 hors écran    — le logiciel l'a, on le garde
 *   0x0100 accéléré
 *   0x2000 fenêtre       — présent sur les vrais GPU, absent du logiciel
 *
 * Le GLDriver d'Apple, logiciel, refuse kCGLPFAAccelerated (73),
 * kCGLPFAFullScreen (54) et kCGLPFANoRecovery (72). On les retire de la
 * copie qu'il reçoit, et on pose les drapeaux correspondants sur nos
 * formats. Sans le bit plein écran, CGL n'appelle même pas
 * gldChoosePixelFormat pour une demande NSOpenGLPFAFullScreen : c'est le
 * « Failed creating OpenGL pixel format » d'UT2004 (SDL 1.2 Quartz).
 *
 * kCGLPFADoubleBuffer (5) est un booléen. Le traiter comme une valeur
 * mangeait l'attribut suivant — ScreenMask (84) dans la liste de SDL.
 */
#define RI_ID          0x04
#define RI_FLAGS       0x08
#define RI_VRAM        0x30     /* kCGLRPVideoMemory, octets */
#define RI_TEXMEM      0x34     /* kCGLRPTextureMemory */
#define RI_FULLSCREEN  0x2
#define RI_ACCELERATED 0x100
#define RI_WINDOW      0x2000
#define RI_OURS        (RI_ACCELERATED | RI_FULLSCREEN | RI_WINDOW)
#define RI_VRAM_BYTES  (64UL * 1024UL * 1024UL)
#define PF_NEXT        0x00
#define PF_ID          0x04
#define PF_FLAGS       0x08
#define CGL_FULLSCREEN_ATTR  54
#define CGL_NORECOVERY_ATTR  72
#define CGL_RENDERER_ID_ATTR 70
#define CGL_ACCELERATED_ATTR 73
/* Attributs CGL/AGL suivis d'une valeur (CGLTypes.h et agl.h de 10.4).
 * AGL_PIXEL_SIZE (50), AGL_GREEN/BLUE_SIZE (9/10) et AGL_BUFFER_SIZE (2)
 * manquaient : une liste Warcraft III se décalait, ChoosePixelFormat
 * recevait n'importe quoi et rendait « unable to initialize OpenGL ». */
static int attr_has_value(long a)
{
    switch (a) {
    case 2:  /* AGL_BUFFER_SIZE */
    case 3:  /* AGL_LEVEL */
    case 7:  /* kCGLPFAAuxBuffers */
    case 8:  /* kCGLPFAColorSize / AGL_RED_SIZE */
    case 9:  /* AGL_GREEN_SIZE */
    case 10: /* AGL_BLUE_SIZE */
    case 11: /* kCGLPFAAlphaSize */
    case 12: /* kCGLPFADepthSize */
    case 13: /* kCGLPFAStencilSize */
    case 14: /* kCGLPFAAccumSize / AGL_ACCUM_RED_SIZE */
    case 15: /* AGL_ACCUM_GREEN_SIZE */
    case 16: /* AGL_ACCUM_BLUE_SIZE */
    case 17: /* AGL_ACCUM_ALPHA_SIZE */
    case 50: /* AGL_PIXEL_SIZE */
    case 55: /* kCGLPFASampleBuffers */
    case 56: /* kCGLPFASamples */
    case 70: /* kCGLPFARendererID */
    case 82: /* AGL_VIRTUAL_SCREEN */
    case 84: /* kCGLPFADisplayMask / NSOpenGLPFAScreenMask */
        return 1;
    default:
        return 0;
    }
}

/* L'hôte et le framebuffer QEMU sont 32 bits xRGB. Un contexte 16 bits
 * (Warcraft III « milliers de couleurs ») passait ChoosePixelFormat puis
 * dessinait dans un tampon 32 bits : hors domaine, écran noir, son OK.
 * On élève la demande : 16 ≥ 16 est toujours vrai pour l'application. */
static long promote_pf_value(long attr, long v)
{
    if (v <= 0)
        return v;
    if ((attr == 50 || attr == 2) && v < 32)    /* AGL_PIXEL_SIZE, BUFFER_SIZE */
        return 32;
    if (attr == 8 && v == 16)                   /* kCGLPFAColorSize 16, pas RED 5/8 */
        return 32;
    if ((attr == 8 || attr == 9 || attr == 10) && v < 8)  /* 555 → 888 */
        return 8;
    if (attr == 12 && v < 24)                   /* DEPTH_SIZE 16 → 32 */
        return 32;
    return v;
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
    if (pomppc_accel_enabled()) {
        GLD_U32(info, RI_FLAGS) |= RI_OURS;
        /* Le GLDriver logiciel laisse 0 : CGLDescribeRenderer rend alors
           kCGLRPVideoMemory = 0, et Warcraft III (comme d'autres jeux 2002)
           refuse d'initialiser OpenGL. GeForce3 y copie la VRAM IOAccelerator. */
        if (GLD_U32(info, RI_VRAM) == 0)
            GLD_U32(info, RI_VRAM) = RI_VRAM_BYTES;
        if (GLD_U32(info, RI_TEXMEM) == 0)
            GLD_U32(info, RI_TEXMEM) = RI_VRAM_BYTES;
    }
}

/* Copie traduite pour le GLDriver d'Apple ; 0 si la liste est trop longue. */
int pomppc_translate_attribs(const long *a, long *out, int max)
{
    int i, n = 0;
    for (i = 0; a[i]; i++) {
        if (n + 3 > max)
            return 0;
        if (a[i] == CGL_ACCELERATED_ATTR ||
            a[i] == CGL_FULLSCREEN_ATTR ||
            a[i] == CGL_NORECOVERY_ATTR)
            continue;
        if (a[i] == CGL_RENDERER_ID_ATTR) {
            out[n++] = a[i++];
            out[n++] = to_apple(a[i]);
            continue;
        }
        out[n++] = a[i];
        if (attr_has_value(a[i])) {
            long attr = a[i];
            out[n++] = promote_pf_value(attr, a[i + 1]);
            i++;
        }
    }
    out[n] = 0;
    return 1;
}

void pomppc_patch_pixel_format(void *pf)
{
    if (pf) {
        GLD_U32(pf, PF_ID) = to_ours(GLD_U32(pf, PF_ID));
        if (pomppc_accel_enabled())
            GLD_U32(pf, PF_FLAGS) |= RI_OURS;
    }
}

void pomppc_unpatch_pixel_format(void *pf)
{
    if (pf) {
        GLD_U32(pf, PF_ID) = to_apple(GLD_U32(pf, PF_ID));
        GLD_U32(pf, PF_FLAGS) &= ~RI_OURS;
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
    /* POMPPC_GL_TRYCUBE=n : même sonde pour les cartes de cube (cfg+0xc2) */
    e = getenv("POMPPC_GL_TRYCUBE");
    if (e && *e) {
        unsigned long n = (unsigned long)atoi(e);
        if (n < 1 || n > 4096)
            n = 256;
        GLD_U16(cfg, 0xc2) = (unsigned short)n;     /* GL_MAX_CUBE_MAP_TEXTURE_SIZE */
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
 * Depuis le 19/09/2026 (device v11) : GL_EXT_separate_specular_color et
 * GL_SGIS_texture_lod, et les textures 3D (cfg+0xbe, qui n'est pas un bit).
 * Puis, pour OpenGL 1.3 (scènes cube et tex13, docs/re/bordure-et-compression.md) :
 *   bit 6  GL_ARB_texture_cube_map       G.cube (et cfg+0xc2) ; sans repli ;
 *   bit 3  GL_ARB_texture_border_clamp   G.tex13 : répétition et couleur
 *   bit 11 GL_ARB_texture_mirrored_repeat  relayées (le rendu d'Apple les ignore) ;
 *   bit 10 GL_ARB_texture_compression    G.tex13 : AUCUN format listé
 *                                        (GL_NUM_COMPRESSED_TEXTURE_FORMATS = 0),
 *                                        les formats génériques sont compressés
 *                                        en DXT1 par GLEngine et relayés ; le
 *                                        rendu d'Apple les tient aussi (repli sûr).
 *
 * GL_EXT_texture_compression_s3tc (bit 43) : longtemps NON annoncée, parce que
 * le rendu d'Apple PLANTE (Bus error) en dessinant une texture à mipmaps
 * chargée par glCompressedTexImage2D (le moindre repli ferait tomber
 * l'application). Annoncée depuis la nuit du 23/09/2026 : sans elle, Prey
 * n'ouvre pas ses .dds précompressés et rend tout le niveau avec ses images
 * par défaut 16×16 RGB565 (normales fausses, écran noir) ; l'hôte tient
 * DXT1/3/5 depuis la v10, et sous programme ARB tout repli tue déjà le jeu.
 * POMPPC_GL_S3TC=0 revient à l'ancienne annonce.
 * Ce que l'on N'AJOUTE PAS : le multiéchantillonnage. Tout cela est mesuré,
 * pas supposé.
 * (La couleur secondaire, les textures de profondeur et l'ombre, longtemps
 * dans cette liste, sont tenues depuis le 19/09/2026 : voir plus haut.)
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
    /* Textures 3D (OpenGL 1.2, protocole v10) : sans repli possible, donc
       seulement si l'hôte les tient (G.tex3d). cfg+0xc0, voisine et encore
       non identifiée, n'est PAS touchée. */
    if (G.tex3d)
        GLD_U16(cfg, 0xbe) = QGPU_MAX_TEX_3D_DIM;
    if (G.cube)
        GLD_U16(cfg, 0xc2) = QGPU_MAX_TEX_DIM;     /* GL_MAX_CUBE_MAP_TEXTURE_SIZE */
    if (G.v8)
        w0 |= 1UL << 17;                /* GL_ARB_occlusion_query */
    w0 |= 1UL << 20;                    /* GL_ARB_vertex_buffer_object */
    w1 |= 1UL << (39 - 32);             /* GL_EXT_blend_func_separate */
    /* 19/09/2026 (scènes texlod et sepspec, docs/re/textures-3d.md) : */
    if (G.q.version >= 11)
        w1 |= 1UL << (37 - 32);         /* GL_EXT_separate_specular_color : les
                                           deux chemins la tiennent (v11) */
    if (G.v10 && (G.q.caps & QGPU_CAP_GL14))
        GLD_U32(cfg, 0x12c) |= 1UL << (77 - 64);   /* GL_SGIS_texture_lod */
    if (G.cube)
        w0 |= 1UL << 6;                 /* GL_ARB_texture_cube_map */
    if (G.tex13)
        w0 |= (1UL << 3) | (1UL << 10) | (1UL << 11);   /* border_clamp,
                                           texture_compression, mirrored_repeat */
    if (G.tex13 && !(getenv("POMPPC_GL_S3TC") && getenv("POMPPC_GL_S3TC")[0] == '0'))
        w1 |= 1UL << (43 - 32);         /* GL_EXT_texture_compression_s3tc (Prey) */
    /* OpenGL 1.4 (scène tex14, docs/re/opengl-1.4.md) */
    w1 |= 1UL << (33 - 32);             /* GL_EXT_stencil_wrap : tenu au pixel par
                                           les deux chemins ET par le rendu d'Apple */
    if (G.tex14) {
        GLD_F32(cfg, 0xb0) = QGPU_MAX_LOD_BIAS;   /* GL_MAX_TEXTURE_LOD_BIAS (Apple : 0) */
        w0 |= (1UL << 12) | (1UL << 13);          /* GL_ARB_shadow, GL_ARB_depth_texture */
        w1 |= 1UL << (40 - 32);         /* GL_EXT_shadow_funcs : les huit fonctions
                                           (scène gl15), par l'hôte */
        w0 |= 1UL << 1;                 /* GL_ARB_point_parameters : l'hôte dérive la
                                           taille au chemin brut (le rendu d'Apple
                                           ignore l'atténuation) */
        if (G.q.version >= 11)
            w1 |= 1UL << (38 - 32);     /* GL_EXT_secondary_color : les deux chemins */
    }
    if (G.xbar)
        w0 |= 1UL << 2;                 /* GL_ARB_texture_env_crossbar (v12) */
    /* v16 : programmes ARB. GL_ARB_vertex_program est déjà annoncé par Apple ;
       GL_ARB_fragment_program (bit 15, docs/re/version-extensions.md §4) ne
       l'est que si l'hôte compile. Les LIMITES (cfg+0xec.., relevé
       docs/re/programmes-arb.md §3) ne gouvernent que glGetProgramivARB :
       un bloc de 16 octets par cible (sommets +0xec, fragments +0xfc :
       instructions, attributs, paramètres, temporaires), puis les compteurs
       ALU / TEX / indirections des fragments et les registres d'adresse. Nulles
       chez Apple, ce qui fait répondre 0 à toute question sur les limites. */
    if (G.prog) {
        int t;
        for (t = 0; t < 2; t++) {
            unsigned char *b = cfg + 0xec + 16 * t;
            GLD_U16(b, 0) = 1024;       /* GL_MAX_PROGRAM_INSTRUCTIONS_ARB */
            GLD_U16(b, 2) = 16;         /* GL_MAX_PROGRAM_ATTRIBS_ARB */
            GLD_U16(b, 4) = QGPU_MAX_PROG_PARAMS;   /* GL_MAX_PROGRAM_PARAMETERS_ARB */
            GLD_U16(b, 6) = 32;         /* GL_MAX_PROGRAM_TEMPORARIES_ARB */
        }
        GLD_U16(cfg, 0x10c) = 1024;     /* fragments : ALU, TEX, indirections */
        GLD_U16(cfg, 0x10e) = 256;
        GLD_U16(cfg, 0x110) = 64;
        GLD_U16(cfg, 0x112) = 1024;     /* idem, « natifs » */
        GLD_U16(cfg, 0x114) = 256;
        GLD_U16(cfg, 0x116) = 64;
        GLD_U16(cfg, 0x118) = 1;        /* sommets : registres d'adresse */
        w0 |= 1UL << 15;                /* GL_ARB_fragment_program */
    }
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
           haute version dont TOUTES les fonctions sont tenues. Le suffixe dit
           qui rend. Détail fonction par fonction : docs/re/version-extensions.md.
             1.5 : objets tampon (GLEngine, vérifiés jusqu'à glMapBuffer et
                   glGetBufferSubData), requêtes d'occlusion (v8, par nous),
                   les huit fonctions d'ombre (hôte v10) — scène gl15 ;
             1.4 : biais de LOD, textures de profondeur et ombre, couleur
                   secondaire, paramètres de point (G.tex14), crossbar
                   (G.xbar, device v12) — MIRRORED_REPEAT, stencil wrap,
                   mélange séparé et constant, brouillard par coordonnée,
                   glWindowPos, mipmaps automatiques l'étaient déjà
                   (docs/re/opengl-1.4.md) ;
             1.3 : cartes de cube (G.cube), CLAMP_TO_BORDER et compression
                   (G.tex13 ; aucun format listé, ce que 1.3 permet ; les
                   formats génériques relayés) — multitexture, combinaisons,
                   matrices transposées l'étaient déjà ; multiéchantillonnage
                   avec GL_SAMPLE_BUFFERS = 0, ce que 1.3 permet aussi ;
             1.2 : textures 3D (v10, G.tex3d), niveaux et bornes de LOD (v10,
                   relayés), spéculaire séparée sur les deux chemins (v11) —
                   tout le reste de 1.2 l'était déjà ;
             1.1 : sinon (device plus ancien, ou hôte sans QGPU_CAP_GL14). */
        if (getenv("POMPPC_GL_ANNOUNCE") && getenv("POMPPC_GL_ANNOUNCE")[0] == '0')
            return apple;
        if (!G.tex3d || G.q.version < 11)
            return "1.1 POMPPC-1.0";
        if (!G.cube || !G.tex13)
            return "1.2 POMPPC-1.0";
        if (!G.tex14 || !G.xbar)
            return "1.3 POMPPC-1.0";
        return G.v8 ? "1.5 POMPPC-1.0" : "1.4 POMPPC-1.0";
    default:
        return apple;
    }
}
