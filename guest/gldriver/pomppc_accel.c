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
 *   POMPPC_GLTRACE=dir    trace (voir pomppc_gld.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

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
#define GS_STENCIL       0x31c0   /* bit 0 */
#define GS_TEXUNIT0      0x31c4   /* unité i : +i·0x7c */
#define GS_TEXUNIT_SIZE  0x7c
#define TU_ENV_COLOR     0x00     /* float ×4 */
#define TU_ENABLE        0x10     /* bits : 1 cube, 2 3D, 4 rectangle, 8 2D, 0x10 1D */
#define TU_ENV_MODE      0x14     /* u16 */
#define GL_MAX_TEXUNITS  8
/* Sommet GLEngine (0x100 octets) : */
#define V_X 0x00
#define V_Y 0x04
#define V_Z 0x08
#define V_COLOR 0x30              /* r g b a */
#define V_FOG   0x4c              /* facteur de brouillard f (1 = pas de brouillard) */
#define V_TEX0  0x80              /* s t r q de l'unité 0, déjà divisés par w */
#define V_TEX1  0x90              /* s t r q de l'unité 1 */

#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
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
enum { RK_TRI = 0, RK_TRI_TEX = 1, RK_TRI_TEX2 = 2, RK_LINES = 3, RK_POINTS = 4 };
static const unsigned long rk_words[5] = {
    QGPU_VERTEX_WORDS, QGPU_VERTEX_TEX_WORDS, QGPU_VERTEX_TEX2_WORDS,
    QGPU_VERTEX_WORDS, QGPU_VERTEX_WORDS,
};

typedef struct PCtx {
    struct PCtx   *next;
    void          *ctx;                 /* contexte du GLDriver d'Apple */
    void         **procs;               /* table de procédures de GLEngine */
    void          *real[PROC_COUNT];    /* procédures d'Apple pour ce contexte */
    long           qctx;                /* identifiant qgpu, -1 si aucun */
    long           surf;                /* surface qgpu, -1 si aucune */
    unsigned long  sw, sh;              /* taille de la surface */
    int            color, depth;        /* fraîcheur */
    int            broken;              /* plus jamais d'accélération */
    unsigned long  st[QGPU_SK_COUNT];   /* état envoyé au device */
    int            st_valid;
    unsigned char *draw_seen;           /* tampon de dessin connu */
} PCtx;

typedef struct PTex {                   /* texture du GLDriver suivie par le plugin */
    struct PTex   *next;
    void          *drvtex;
    long           qtex;                /* identifiant hôte, -1 si aucun */
    int            dirty;               /* niveaux à (re)téléverser */
    unsigned long  prm[4];              /* paramètres envoyés : min, mag, wrap s, wrap t */
    int            prm_valid;
} PTex;

typedef struct TexUnit {
    PTex          *t;                   /* 0 : unité inactive */
    unsigned long  env_mode, env_color;
} TexUnit;

typedef struct TexInfo {                /* textures à appliquer pour le dessin en cours */
    TexUnit        u[2];
} TexInfo;

typedef struct Post {                   /* copie à faire après la soumission */
    int            depth;
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
} G = { PTHREAD_MUTEX_INITIALIZER };

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
        static const unsigned long ops[5] = {
            QGPU_OP_DRAW_TRIANGLES, QGPU_OP_DRAW_TRIANGLES_TEX, QGPU_OP_DRAW_TRIANGLES_TEX2,
            QGPU_OP_DRAW_LINES, QGPU_OP_DRAW_POINTS,
        };
        c[0] = QGPU_CMD_HDR(ops[G.run_kind], QGPU_LEN_DRAW);
        c[1] = G.run_count;
        c[2] = G.q.base + VTX_OFF + G.run_start;
        G.ncmd += QGPU_LEN_DRAW;
    }
    G.run_ctx = 0;
    G.run_count = 0;
}

/* Réserve `words` mots de flux pour le contexte p (CTX_BIND inclus au besoin). */
static unsigned long *reserve(PCtx *p, unsigned long words)
{
    unsigned long *c;
    close_run();
    if (G.ncmd + words + 2 + QGPU_LEN_DRAW > CMD_WORDS)
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
        st = qgpu_submit(&G.q, 0, G.ncmd * 4, &pc);
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
                    for (x = 0; x < po->w; x++) {
                        float f = *(float *)(s + x);
                        dd[x] = (unsigned long)(clamp01(f) * po->scale + 0.5f);
                    }
                }
            }
        }
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

static PTex *find_tex(void *drvtex)
{
    PTex *t;
    for (t = G.textures; t; t = t->next)
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

    if (!gp || !base_format_ok(base))
        return 0;
    if (t->qtex < 0) {
        t->qtex = alloc_tex_id();
        if (t->qtex < 0)
            return 0;
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
                return 0;
            if (!convert_level(lv, (unsigned long *)(G.q.win + off)))
                return 0;
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
        return 0;
    if (env != 0x2100 && env != 0x2101 && env != 0x0BE2 && env != 0x1E01 && env != 0x0104)
        return 0;                                   /* GL_COMBINE… */
    dt = (void *)GLD_U32(units, u * 0x14 + ((mask & 8) ? 3 * 4 : 4 * 4));
    tu->t = dt ? find_tex(dt) : 0;
    if (!tu->t || !upload_texture(p, tu->t))
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

    ti->u[0].t = ti->u[1].t = 0;
    if (!GLD_U8(p->ctx, CTX_TEXTURING))
        return 1;                                   /* pas de texture : dessin simple */
    for (i = 2; i < GL_MAX_TEXUNITS; i++)
        if (GLD_U32(g, GS_TEXUNIT0 + i * GS_TEXUNIT_SIZE + TU_ENABLE) & 0x1f)
            return 0;                               /* 3 unités ou plus : logiciel */
    return texture_unit_ok(p, 0, &ti->u[0]) && texture_unit_ok(p, 1, &ti->u[1]);
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

    if (p->qctx < 0 || w == 0 || h == 0 || w > QGPU_MAX_SURF_DIM || h > QGPU_MAX_SURF_DIM)
        return 0;
    if (p->surf >= 0 && p->sw == w && p->sh == h)
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
    c[4] = QGPU_FMT_XRGB8888 | QGPU_FMT_FLAG_DEPTH;
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
                for (x = 0; x < w; x++)
                    d[x] = clamp01(s[x] * inv);
            }
            c = reserve(p, QGPU_LEN_SURF_XFER);
            c[0] = QGPU_CMD_HDR(QGPU_OP_DEPTH_UPLOAD, QGPU_LEN_SURF_XFER);
            c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
            c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
            G.n_uploads++;
        }
        p->depth = SYNCED;
    }
}

static void queue_readback(PCtx *p, int depth, unsigned char *dst)
{
    unsigned long off, *c;
    unsigned long w = p->sw, h = p->sh;
    if (!arena_alloc(w * h * 4, &off))
        return;
    c = reserve(p, QGPU_LEN_SURF_XFER);
    c[0] = QGPU_CMD_HDR(depth ? QGPU_OP_DEPTH_READBACK : QGPU_OP_SURF_READBACK,
                        QGPU_LEN_SURF_XFER);
    c[1] = p->surf; c[2] = G.q.base + off; c[3] = w * 4;
    c[4] = 0; c[5] = 0; c[6] = w; c[7] = h;
    G.post[G.npost].depth = depth;
    G.post[G.npost].off = off;
    G.post[G.npost].dst = dst;
    G.post[G.npost].w = w;
    G.post[G.npost].h = h;
    G.post[G.npost].rowbytes = sw_rowbytes(p);
    G.post[G.npost].scale = GLD_F32(p->ctx, CTX_DEPTH_SCALE);
    G.npost++;
    G.n_readbacks++;
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

/* L'état courant relève-t-il du domaine rendu par l'hôte ? */
static int accel_ok(PCtx *p)
{
    unsigned char *g;

    if (G.state <= 0 || p->broken || p->qctx < 0)
        return 0;
    g = gls(p);
    if (!g || !sw_color(p) || GLD_U32(p->ctx, CTX_COLOR_BITS) != 32 ||
        GLD_U32(p->ctx, CTX_ROWPIX) < GLD_U32(p->ctx, CTX_WIDTH))
        return 0;
    if ((GLD_U32(g, GS_STENCIL) & 1) || GLD_U8(g, GS_LOGIC_OP) ||
        GLD_U8(g, GS_POLY_STIPPLE) || GLD_U8(g, GS_POLY_SMOOTH))
        return 0;
    /* brouillard : GLEngine fournit le facteur par sommet, sauf en GL_NICEST
       où le GLDriver le calcule par fragment */
    if (GLD_U8(g, GS_FOG) && U16(g, GS_FOG_HINT) == 0x1102)
        return 0;
    if (U16(g, GS_POLY_MODE) != GL_FILL || U16(g, GS_POLY_MODE + 2) != GL_FILL)
        return 0;
    if (GLD_U8(g, GS_DEPTH_TEST) &&
        (GLD_U32(p->ctx, CTX_DEPTH_BITS) != 32 || U16(g, GS_DEPTH_FUNC) < 0x200 ||
         U16(g, GS_DEPTH_FUNC) > 0x207))
        return 0;
    if (GLD_U8(g, GS_BLEND) &&
        (!blend_factor_ok(U16(g, GS_BLEND_SRC_RGB)) || !blend_factor_ok(U16(g, GS_BLEND_DST_RGB)) ||
         !blend_factor_ok(U16(g, GS_BLEND_SRC_A)) || !blend_factor_ok(U16(g, GS_BLEND_DST_A)) ||
         !blend_eq_ok(U16(g, GS_BLEND_EQ_RGB)) || !blend_eq_ok(U16(g, GS_BLEND_EQ_A))))
        return 0;
    if (GLD_U8(g, GS_ALPHA_TEST) && (U16(g, GS_ALPHA_FUNC) < 0x200 || U16(g, GS_ALPHA_FUNC) > 0x207))
        return 0;
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
        static const int keys[2][4] = {
            { QGPU_SK_TEXTURE, QGPU_SK_TEX_BIND, QGPU_SK_TEX_ENV_MODE, QGPU_SK_TEX_ENV_COLOR },
            { QGPU_SK_TEXTURE1, QGPU_SK_TEX1_BIND, QGPU_SK_TEX1_ENV_MODE, QGPU_SK_TEX1_ENV_COLOR },
        };
        int u;
        for (u = 0; u < 2; u++) {
            const TexUnit *tu = ti ? &ti->u[u] : 0;
            v[keys[u][0]] = tu && tu->t;
            v[keys[u][1]] = p->st_valid ? p->st[keys[u][1]] : 0;
            v[keys[u][2]] = p->st_valid ? p->st[keys[u][2]] : 0x2100;
            v[keys[u][3]] = p->st_valid ? p->st[keys[u][3]] : 0;
            if (tu && tu->t) {
                v[keys[u][1]] = tu->t->qtex;
                v[keys[u][2]] = tu->env_mode;
                v[keys[u][3]] = tu->env_color;
            }
        }
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
    sync_to_host(p, 1, GLD_U8(gls(p), GS_DEPTH_TEST));
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
    if (!accel_ok(p) || !ensure_surface(p) || !texture_ok(p, &ti))
        return 0;
    begin_common(p, b, &ti);
    b->kind = ti.u[1].t ? RK_TRI_TEX2 : ti.u[0].t ? RK_TRI_TEX : RK_TRI;
    return 1;
}

static void put_vertex(const Batch *b, float *o, const unsigned char *v, const unsigned char *col)
{
    const float *c = (const float *)(col + V_COLOR);
    if (b->kind == RK_TRI_TEX || b->kind == RK_TRI_TEX2)
        memcpy(o + 8, v + V_TEX0, 4 * sizeof(float));
    if (b->kind == RK_TRI_TEX2)
        memcpy(o + 12, v + V_TEX1, 4 * sizeof(float));
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
    long rest = mask & ~host;
    proc8 real = 0;

    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (!p) {
        pthread_mutex_unlock(&G.mu);
        return 0;
    }
    real = (proc8)p->real[PROC_Clear];
    if (host && accel_ok(p) && ensure_surface(p)) {
        const float *cc;
        int full, color_full, depth_full;
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
        if ((host & GL_DEPTH_BUFFER_BIT) && depth_full && p->depth == SW_NEWER)
            p->depth = SYNCED;
        sync_to_host(p, (host & GL_COLOR_BUFFER_BIT) != 0,
                     (host & GL_DEPTH_BUFFER_BIT) != 0);
        send_state(p, 0);
        cc = (const float *)(g + GS_CLEAR_COLOR);
        cmd = reserve(p, QGPU_LEN_CLEAR);
        cmd[0] = QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR);
        cmd[1] = ((host & GL_COLOR_BUFFER_BIT) ? QGPU_CLEAR_COLOR : 0) |
                 ((host & GL_DEPTH_BUFFER_BIT) ? QGPU_CLEAR_DEPTH : 0);
        cmd[2] = (to_u8(cc[3]) << 24) | (to_u8(cc[0]) << 16) | (to_u8(cc[1]) << 8) | to_u8(cc[2]);
        *(float *)&cmd[3] = clamp01((float)F64(g, GS_CLEAR_DEPTH));
        if (host & GL_COLOR_BUFFER_BIT)
            p->color = HOST_NEWER;
        if ((host & GL_DEPTH_BUFFER_BIT) && v[QGPU_SK_DEPTH_WRITE])
            p->depth = HOST_NEWER;
        G.n_clears++;
        pthread_mutex_unlock(&G.mu);
        /* stencil / accumulation : toujours par le logiciel, sans rapport avec
           nos deux tampons */
        return rest ? real(ctx, rest, c, d, e, f, g8, h) : 0;
    }
    fallback(p, PROC_Clear, (mask & GL_COLOR_BUFFER_BIT) != 0,
             (mask & GL_DEPTH_BUFFER_BIT) != 0);
    if ((mask & GL_DEPTH_BUFFER_BIT) && GLD_U8(gls(p), GS_DEPTH_MASK) && p->surf >= 0)
        p->depth = SW_NEWER;
    pthread_mutex_unlock(&G.mu);
    return real(ctx, mask, c, d, e, f, g8, h);
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

static int is_ours(void *f)
{
    int k;
    for (k = 0; k < PROC_COUNT; k++)
        if (f == proc_tramps[k] || (f && f == accel_proc(k)))
            return 1;
    return 0;
}

void pomppc_hook_procs(void *ctx, void **procs)
{
    PCtx *p;
    int k;
    pthread_mutex_lock(&G.mu);
    p = find_ctx(ctx);
    if (p) {
        p->procs = procs;
        for (k = 0; k < PROC_COUNT; k++) {
            if (!procs[k] || is_ours(procs[k]))
                continue;
            p->real[k] = procs[k];
            if (G.state > 0 && accel_proc(k))
                procs[k] = accel_proc(k);
            else if (pomppc_tracing() || (G.state > 0 && proc_kind(k) != K_NONE))
                procs[k] = proc_tramps[k];
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
            if (procs[k] && is_ours(procs[k]))
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
