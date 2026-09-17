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
 *   POMPPC_GLTRACE=dir    trace (voir pomppc_gld.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <dlfcn.h>

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

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_FLAT             0x1d00
#define GL_FILL             0x1b02

#define U16(p, o) (*(unsigned short *)((unsigned char *)(p) + (o)))
#define S16(p, o) (*(short *)((unsigned char *)(p) + (o)))
#define I32(p, o) (*(long *)((unsigned char *)(p) + (o)))
#define F64(p, o) (*(double *)((unsigned char *)(p) + (o)))

/* ───────────────────────────── disposition de la tranche ───────────────────────────── */
#define CMD_WORDS   (0x40000 / 4)       /* flux : 256 Kio */
#define VTX_OFF     0x40000             /* sommets : jusqu'à 4 Mio */
#define VTX_END     0x400000
#define ARENA_OFF   0x400000            /* transferts : le reste de la tranche */
#define MAX_POST    16

enum { SYNCED = 0, HOST_NEWER = 1, SW_NEWER = 2 };

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

typedef struct Post {                   /* copie à faire après la soumission */
    int            depth;               /* 0 couleur, 1 profondeur, 2 stencil */
    int            packed;              /* profondeur 24 bits + stencil 8 bits dans le mot */
    unsigned long  off;                 /* dans l'arène */
    unsigned char *dst;
    unsigned long  w, h, rowbytes;
    float          scale;
} Post;

static struct {
    pthread_mutex_t mu;
    int             state;              /* 0 inconnu, 1 actif, -1 désactivé */
    QgpuClient      q;
    unsigned long  *cmd;                /* = q.win */
    unsigned long   ncmd;
    unsigned long   vtx;                /* octets utilisés depuis VTX_OFF */
    unsigned long   arena;              /* octets utilisés depuis ARENA_OFF */
    PCtx           *bound;              /* contexte lié dans le flux en cours */
    PCtx           *run_ctx;            /* série de triangles ouverte */
    unsigned long   run_start, run_count;
    int             run_kind;           /* genre de la série : RK_* */
    Post            post[MAX_POST];
    int             npost;
    unsigned long   ctx_used, surf_used;
    unsigned long   tex_used[(QGPU_CLIENT_TEX_IDS + 31) / 32];
    PCtx           *list;
    PTex           *textures;
    /* statistiques */
    unsigned long   n_tris, n_clears, n_submits, n_uploads, n_readbacks, n_fallback;
    unsigned long   n_textris, n_texuploads, n_lines, n_points;
    unsigned long   n_frames, n_direct, n_tex_incomplete;
    double          t_submit, t_copy, t_upload;   /* secondes cumulées */
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
    NO_TEX_BASE, NO_TEX_SIZE, NO_TEX_FORMAT, NO_TEX_ID, NO_TEX_COMBINE, NO_STENCIL, NO_COUNT
};
static const char *const no_name[NO_COUNT] = {
    "tampon", "stencil/logicop/stipple", "brouillard", "polygonmode", "profondeur",
    "melange", "alphatest", "surface", "unites>2", "cible-texture", "texenv",
    "texture-inconnue", "format-base", "taille-texture", "format-texels", "id-texture",
    "combine", "stencil",
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

/* Bilan périodique (POMPPC_GL_STATS=<fichier>), appelé à chaque échange. */
static void stats_frame(void)
{
    static const char *path;
    static int init;
    static double t0;
    static unsigned long f0, tr0, rb0, up0, fb0, sub0, tu0, di0;
    static double ts0, tc0, tu_0;
    double t;

    if (!init) {
        const char *e = getenv("POMPPC_GL_STATS");
        init = 1;
        path = (e && e[0] == '/') ? e : 0;
        t0 = now_s();
    }
    G.n_frames++;
    if (!path)
        return;
    t = now_s();
    if (t - t0 >= 5.0) {
        FILE *f = fopen(path, "a");
        if (f) {
            double dt = t - t0;
            fprintf(f, "%.1f img/s | tri %lu/img | relect %lu | televers %lu (tex %lu) | "
                    "replis %lu | soumissions %lu | submit %.0f ms/img | copie %.0f ms/img | "
                    "prep televers %.0f ms/img | directes %lu\n",
                    (G.n_frames - f0) / dt,
                    (G.n_tris - tr0) / (G.n_frames - f0 ? G.n_frames - f0 : 1),
                    G.n_readbacks - rb0, G.n_uploads - up0, G.n_texuploads - tu0,
                    G.n_fallback - fb0, G.n_submits - sub0,
                    (G.t_submit - ts0) * 1000 / (G.n_frames - f0),
                    (G.t_copy - tc0) * 1000 / (G.n_frames - f0),
                    (G.t_upload - tu_0) * 1000 / (G.n_frames - f0),
                    G.n_direct - di0);
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
    if (getenv("POMPPC_GL_STATS"))
        fprintf(stderr, "POMPPC GL : %lu triangles (%lu texturés), %lu segments, %lu points "
                "et %lu effacements sur l'hôte, %lu soumissions, %lu téléversements, "
                "%lu niveaux de texture, %lu relectures, %lu appels logiciels synchronisés\n",
                G.n_tris, G.n_textris, G.n_lines, G.n_points, G.n_clears, G.n_submits,
                G.n_uploads, G.n_texuploads, G.n_readbacks, G.n_fallback);
}

int pomppc_accel_enabled(void)
{
    return G.state > 0;
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
            G.cmd = (unsigned long *)G.q.win;
            atexit(on_exit_stats);
        } else {
            G.state = -1;
        }
        if (G.state > 0)
            pomppc_log("POMPPC: qgpu actif (tranche %lu à 0x%lx, %lu Mio, caps 0x%lx)\n",
                       G.q.index, G.q.base, G.q.size >> 20, G.q.caps);
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
        c[2] = G.q.base + VTX_OFF + G.run_start;
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

/* Réserve `words` mots de flux pour le contexte p (CTX_BIND inclus au besoin). */
static unsigned long *reserve(PCtx *p, unsigned long words)
{
    unsigned long *c;
    close_run();
    if (G.ncmd + words + 2 + QGPU_LEN_DRAW_N > CMD_WORDS)
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

/* Réserve n octets d'arène (alignés sur 4) ; peut vider le flux. 0 = impossible. */
static long arena_alloc(unsigned long n, unsigned long *off)
{
    n = (n + 3) & ~3UL;
    if (ARENA_OFF + G.arena + n > G.q.size || G.npost >= MAX_POST) {
        flush();
        if (ARENA_OFF + n > G.q.size)
            return 0;
    }
    *off = ARENA_OFF + G.arena;
    G.arena += n;
    return 1;
}

static void broken_all(const char *why, long st, unsigned long pc)
{
    PCtx *p;
    pomppc_log("POMPPC: soumission refusée (%s, statut %ld, commande %lu) : "
               "accélération coupée\n", why, st, pc);
    fprintf(stderr, "POMPPC GL : soumission refusée (statut %ld, commande %lu), "
            "retour au rendu logiciel\n", st, pc);
    for (p = G.list; p; p = p->next)
        p->broken = 1;
}

static void flush(void)
{
    unsigned long pc = 0;
    long st;
    int i;

    close_run();
    if (G.ncmd) {
        double t = now_s(), t2;
        st = qgpu_submit(&G.q, 0, G.ncmd * 4, &pc);
        t2 = now_s();
        G.t_submit += t2 - t;
        G.n_submits++;
        if (st != QGPU_ST_OK)
            broken_all("flush", st, pc);
        for (i = 0; i < G.npost && st == QGPU_ST_OK; i++) {
            Post *po = &G.post[i];
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
        G.t_copy += now_s() - t2;
    }
    G.ncmd = 0;
    G.vtx = 0;
    G.arena = 0;
    G.npost = 0;
    G.bound = 0;
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

/* Téléverse les niveaux et les paramètres de t si besoin ; 0 = logiciel. */
static int upload_texture(PCtx *p, PTex *t)
{
    unsigned char *dt = t->drvtex;
    unsigned char *gp = (unsigned char *)GLD_U32(dt, DT_PARAMS);
    unsigned long base = GLD_U32(dt, DT_BASE_FORMAT), prm[4], off, *c;
    int l, k;

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
        /* le stencil de l'invité vit dans les 8 bits bas des mêmes mots */
        if (src && p->stencil && arena_alloc(w * h * 4, &off)) {
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
    G.post[G.npost].depth = depth;
    G.post[G.npost].packed = p->stencil;
    G.post[G.npost].off = off;
    G.post[G.npost].dst = dst;
    G.post[G.npost].w = w;
    G.post[G.npost].h = h;
    G.post[G.npost].rowbytes = rowbytes;
    G.post[G.npost].scale = GLD_F32(p->ctx, CTX_DEPTH_SCALE);
    G.npost++;
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
            if (p->stencil)
                queue_readback(p, 2, dst);
            any = 1;
        }
        p->depth = SYNCED;
    }
    if (any || G.ncmd)
        flush();
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
    return f <= 1 || (f >= 0x300 && f <= 0x308);
}

static int blend_eq_ok(unsigned long e)
{
    return e == 0x8006 || e == 0x800A || e == 0x800B;
}

static int stencil_op_ok(unsigned long op)
{
    return op == 0 || op == 0x150A || (op >= 0x1E00 && op <= 0x1E03) ||
           op == 0x8507 || op == 0x8508;
}

/* Test de stencil actif ET tampon présent (sinon le test n'a aucun effet). */
static int stencil_active(PCtx *p)
{
    return (GLD_U32(gls(p), GS_STENCIL) & 1) && GLD_U32(p->ctx, CTX_STENCIL_BITS) != 0;
}

/* L'état courant relève-t-il du domaine rendu par l'hôte ? */
static int accel_ok(PCtx *p)
{
    unsigned char *g;

    if (G.state <= 0 || p->broken || p->qctx < 0)
        return 0;
    g = gls(p);
    if (!g || !sw_color(p) || GLD_U32(p->ctx, CTX_COLOR_BITS) != 32 ||
        GLD_U32(p->ctx, CTX_ROWPIX) < GLD_U32(p->ctx, CTX_WIDTH))
        return no(NO_BUFFER, GLD_U32(p->ctx, CTX_COLOR_BITS), GLD_U32(p->ctx, CTX_ROWPIX));
    if (GLD_U8(g, GS_LOGIC_OP) || GLD_U8(g, GS_POLY_STIPPLE) || GLD_U8(g, GS_POLY_SMOOTH))
        return no(NO_RASTER, GLD_U8(g, GS_LOGIC_OP), GLD_U8(g, GS_POLY_STIPPLE));
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
    if (GLD_U8(g, GS_FOG) && U16(g, GS_FOG_HINT) == 0x1102)
        return no(NO_FOG, U16(g, GS_FOG_MODE), 0);
    if (U16(g, GS_POLY_MODE) != GL_FILL || U16(g, GS_POLY_MODE + 2) != GL_FILL)
        return no(NO_POLYMODE, U16(g, GS_POLY_MODE), U16(g, GS_POLY_MODE + 2));
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

static void compute_state(PCtx *p, const TexInfo *ti, unsigned long *v)
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
}

static void send_state(PCtx *p, const TexInfo *ti)
{
    unsigned long v[QGPU_SK_COUNT], *c;
    int k;
    compute_state(p, ti, v);
    for (k = 1; k < QGPU_SK_COUNT; k++) {
        if (p->st_valid && p->st[k] == v[k])
            continue;
        c = reserve(p, QGPU_LEN_SET_STATE);
        c[0] = QGPU_CMD_HDR(QGPU_OP_SET_STATE, QGPU_LEN_SET_STATE);
        c[1] = k;
        c[2] = v[k];
        p->st[k] = v[k];
    }
    p->st_valid = 1;
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
    send_state(p, ti);
    b->p = p;
    b->h = (float)p->sh;
    b->zinv = 1.0f / GLD_F32(p->ctx, CTX_DEPTH_SCALE);
    b->flat = GLD_U32(gls(p), GS_SHADE_MODEL) == GL_FLAT;
    b->fog = GLD_U8(gls(p), GS_FOG) != 0;
}

/* Lignes et points : ni texture, ni stipple, ni lissage, ni décalage. */
static int begin_lp(PCtx *p, Batch *b, int lines)
{
    unsigned char *g;
    if (!accel_ok(p) || GLD_U8(p->ctx, CTX_TEXTURING) || !ensure_surface(p))
        return 0;
    g = gls(p);
    if (lines ? (GLD_U8(g, GS_LINE_STIPPLE) || GLD_U8(g, GS_LINE_SMOOTH) || GLD_U8(g, GS_POLY_OFS_LINE))
              : (GLD_U8(g, GS_POINT_SMOOTH) || GLD_U8(g, GS_POLY_OFS_PT)))
        return 0;
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
    if (VTX_OFF + G.vtx + n * vw * 4 > VTX_END)
        flush();                        /* l'état GL reste sur le device, par contexte */
    if (G.run_ctx != b->p || G.bound != b->p || G.run_kind != b->kind) {
        reserve(b->p, 0);               /* ferme la série précédente, lie le contexte */
        G.run_ctx = b->p;
        G.run_start = G.vtx;
        G.run_count = 0;
        G.run_kind = b->kind;
    }
    o = (float *)(G.q.win + VTX_OFF + G.vtx);
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
        if (touches_depth && writes_depth(p))
            p->depth = SW_NEWER;
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
        GLD_U32(p->ctx, CTX_DEPTH_BITS) == 32)
        host |= GL_STENCIL_BUFFER_BIT;
    rest = mask & ~host;
    if (host && accel_ok(p) && ensure_surface(p)) {
        const float *cc;
        int full, color_full, depth_full, ds_written;
        g = gls(p);
        check_draw_buffer(p);
        compute_state(p, 0, v);
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
        send_state(p, 0);
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
 * Seulement si l'hôte a l'image la plus récente (sinon, chemin normal). */
static int present_direct(PCtx *p)
{
    unsigned char *vram;
    unsigned long rowbytes;
    if (G.state <= 0 || p->broken || p->surf < 0 || p->color == SW_NEWER)
        return 0;
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
    case PROC_RenderVertexArray: case PROC_Proc68: case PROC_Proc6c:
    case PROC_Proc84: case PROC_Proc88: case PROC_Proc8c:
        return K_READ | K_WRITE | K_DEPTH;
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
    default:                       return 0;
    }
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
            /* Sonde T&L (POMPPC_GL_TCL=1) : les quatre entrées « hautes » sont
               prioritaires, et doivent être réinstallées à chaque
               gldUpdateDispatch comme toutes les autres. */
            if (pomppc_tcl_proc(i))
                tab[i] = pomppc_tcl_proc(i);
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
            if (!procs[k] || !mine || procs[k] == mine)
                continue;
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
        if (G.state > 0 && p->qctx >= 0) {
            destroy_surface(p);
            c = reserve(p, QGPU_LEN_CTX);
            c[0] = QGPU_CMD_HDR(QGPU_OP_CTX_DESTROY, QGPU_LEN_CTX);
            c[1] = p->qctx;
            flush();
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
            if (p->color == HOST_NEWER && p->draw_seen)
                queue_readback(p, 0, p->draw_seen);
            if (G.ncmd)
                flush();
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
    default:
        return apple;
    }
}
