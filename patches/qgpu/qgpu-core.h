/*
 * qgpu-core.h — exécuteur du flux de commandes « qgpu », indépendant de QEMU.
 *
 * Le cœur analyse le flux (mots big-endian, cf. qgpu_proto.h), tient les
 * objets (contextes, surfaces) et délègue le rendu à un backend :
 *   - qgpu_backend_soft : rasteriseur logiciel de référence (toujours présent) ;
 *   - qgpu_backend_gl   : OpenGL hors écran (CGL sur macOS, EGL sur Linux),
 *                         c'est LUI qui fait travailler le GPU hôte.
 *
 * Aucune dépendance QEMU : le même code est lié dans le device qgpu-pci.c et
 * dans tests/qgpu_core_test.c, qui l'exécute nativement sur l'hôte.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#ifndef QGPU_CORE_H
#define QGPU_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "qgpu_proto.h"

typedef struct QgpuCore QgpuCore;

enum { QGPU_PRIM_TRIANGLES = 0, QGPU_PRIM_LINES = 1, QGPU_PRIM_POINTS = 2 };

typedef struct QgpuSurface {
    bool     used;
    bool     has_depth;            /* v2 : QGPU_FMT_FLAG_DEPTH */
    bool     has_stencil;          /* v6 : QGPU_FMT_FLAG_STENCIL (implique has_depth) */
    uint32_t width, height, format;
    void    *priv;                 /* propriété du backend */
} QgpuSurface;

/* État GL d'un contexte (v2), indexé par QGPU_SK_*. ALPHA_REF est gardé en
   bits IEEE, comme sur le fil. */
typedef struct QgpuState {
    uint32_t v[QGPU_SK_COUNT];
} QgpuState;

/* Texture (v3, v10). Les niveaux sont tenus par le cœur, en ARGB hôte-natif
   — ou, pour une texture de profondeur, en flottants hôte-natifs rangés bit à
   bit dans les mêmes mots (qgpu_u2f) : le backend logiciel les échantillonne
   directement, le backend GL les recopie dans ses objets texture (dirty). Les
   conversions de format et la décompression S3TC sont faites par le cœur :
   un backend ne voit jamais que ces deux formes. */
#define QGPU_TEX_FACES 6            /* cartes de cube ; les autres cibles : face 0 */

typedef struct QgpuTexLevel {
    uint32_t  w, h, d;              /* d = 1 sauf en 3D */
    uint32_t  fmt;                  /* v10 : format de base de CE niveau */
    uint32_t *px;                   /* NULL si le niveau n'est pas défini ;
                                       w·h·d mots, tranche par tranche */
} QgpuTexLevel;

typedef struct QgpuTexture {
    bool         used;
    uint32_t     target;            /* v10 : QGPU_TT_* (QGPU_TT_2D en v3–v9) */
    uint32_t     nfaces;            /* 6 pour une carte de cube, 1 sinon */
    uint32_t     base_format;       /* format de base du niveau de base (face 0) */
    uint32_t     min_filter, mag_filter, wrap_s, wrap_t;
    /* v10 */
    uint32_t     wrap_r;
    uint32_t     border;            /* 0xAARRGGBB */
    float        min_lod, max_lod, lod_bias;
    uint32_t     base_level, max_level;
    uint32_t     compare_mode, compare_func, depth_mode;
    bool         gen_mipmap;
    QgpuTexLevel level[QGPU_TEX_FACES][QGPU_MAX_TEX_LEVELS];
    uint32_t     dirty[QGPU_TEX_FACES];  /* bit n : niveau n de la face modifié
                                            depuis la dernière synchro */
    bool         params_dirty;
    void        *priv;              /* propriété du backend */
} QgpuTexture;

/* Nombre de niveaux utilisables À PARTIR DU NIVEAU DE BASE (0 = texture
   incomplète : le texturage de l'unité est coupé, comme en OpenGL). */
uint32_t qgpu_texture_levels(const QgpuTexture *t);

/* v10 : vrai si la texture rend une valeur de profondeur (format de base
   GL_DEPTH_COMPONENT) ; son environnement la voit alors avec le format
   qgpu_texture_env_format(). */
static inline bool qgpu_texture_is_depth(const QgpuTexture *t)
{
    return t->base_format == 0x1902;
}

/* v10 : format de base VU PAR L'ENVIRONNEMENT de texture : le format de base,
   sauf pour une texture de profondeur, qui se présente selon son
   QGPU_TP_DEPTH_MODE (luminance, intensité ou alpha). */
static inline uint32_t qgpu_texture_env_format(const QgpuTexture *t)
{
    return qgpu_texture_is_depth(t) ? t->depth_mode : t->base_format;
}

/* v8 : requête d'occlusion. Le cœur tient l'état d'ouverture (une seule active
   par contexte, cf. qgpu_proto.h) ; le backend tient le COMPTE, parce que lui
   seul sait quels fragments passent : le backend de référence incrémente
   `samples` au fil de la rastérisation, le backend OpenGL le lit dans son objet
   de requête à la fermeture. */
typedef struct QgpuQuery {
    bool     used;                 /* un QUERY_BEGIN a déjà eu lieu sur cet id */
    bool     active;               /* entre BEGIN et END */
    uint64_t samples;              /* fragments passés (saturé sur le fil) */
    void    *priv;                 /* propriété du backend */
} QgpuQuery;

/* v8 : motif de pointillé de polygone d'un contexte, 32 lignes de 32 bits,
   ligne 0 = yw 0 = BAS de l'image (convention glPolygonStipple). */
typedef struct QgpuStipple {
    uint32_t row[32];
} QgpuStipple;

/* ── v7 : état de l'étage géométrique, par contexte ──────────────────────────
 *
 * Tout est en flottants hôte-natifs, déjà validé par le cœur (pas de NaN, pas
 * d'infini, bornes d'OpenGL respectées) : un backend lit et applique.
 * Les matrices sont dans l'ORDRE COLONNE d'OpenGL (m[0..3] = 1re colonne),
 * comme sur le fil et comme glLoadMatrixf les veut. */
typedef struct QgpuLight {
    bool  enabled;
    float ambient[4], diffuse[4], specular[4];
    float position[4];             /* EN COORDONNÉES ŒIL ; w = 0 : directionnelle */
    float spot_dir[3];             /* idem, coordonnées œil */
    float spot_exp, spot_cutoff;   /* cutoff : 0..90, ou 180 = pas de spot */
    float att[3];                  /* constante, linéaire, quadratique */
} QgpuLight;

typedef struct QgpuMaterial {
    float ambient[4], diffuse[4], specular[4], emission[4];
    float shininess;
} QgpuMaterial;

typedef struct QgpuTexgen {
    bool     enabled;
    uint32_t mode;                 /* QGPU_TG_* */
    float    obj_plane[4];
    float    eye_plane[4];         /* déjà en coordonnées œil */
} QgpuTexgen;

typedef struct QgpuClipPlane {
    bool  enabled;
    float eq[4];                   /* coordonnées œil ; garde eq·p >= 0 */
} QgpuClipPlane;

typedef struct QgpuGeom {
    float mtx[QGPU_MTX_COUNT][16];         /* modèle-vue, projection, textures 0..3 */
    QgpuLight     light[QGPU_MAX_LIGHTS];
    QgpuMaterial  mat[2];                  /* 0 = face avant, 1 = face arrière */
    float         lm_ambient[4];           /* ambiante du modèle d'éclairage */
    QgpuTexgen    texgen[QGPU_MAX_UNITS][4];   /* [unité][S,T,R,Q] */
    QgpuClipPlane clip[QGPU_MAX_CLIP_PLANES];
    /* valeurs courantes des attributs absents du format de sommet */
    float cur_normal[3], cur_color[4], cur_sec[3], cur_fog;
    float cur_tex[QGPU_MAX_UNITS][4];
    int32_t vp[4];                         /* viewport GL : origine EN BAS à gauche */
    bool    vp_set;                        /* un VIEWPORT a été posé */
    float   depth_near, depth_far;
} QgpuGeom;

typedef struct QgpuContext {
    bool        used;
    int32_t     surf;              /* surface liée, -1 si aucune */
    QgpuState   st;
    QgpuGeom    gm;                /* v7 */
    QgpuStipple stip;              /* v8 : pointillé de polygone */
    int32_t     query;             /* v8 : requête ouverte, -1 si aucune */
} QgpuContext;

void qgpu_state_init(QgpuState *st);
/* v10 : paramètres de point (chemin brut). */
bool  qgpu_points_plain(const QgpuState *st);
float qgpu_point_size(const QgpuState *st, float d);
void qgpu_geom_init(QgpuGeom *gm);              /* v7 : valeurs initiales d'OpenGL */
void qgpu_stipple_init(QgpuStipple *sp);        /* v8 : tout à 1, comme en OpenGL */

/* v8 : mot du motif de pointillé à employer pour la ligne de SURFACE `ys`
   d'une surface de hauteur `h`. Le motif est indexé par la coordonnée fenêtre
   OpenGL yw = h − ys (cf. qgpu_proto.h) ; la fonction est ici pour que les deux
   backends ne puissent pas en avoir deux idées. */
static inline uint32_t qgpu_stipple_row(const QgpuStipple *sp, uint32_t h, int ys)
{
    return sp->row[(uint32_t)(((int)h - ys) % 32 + 32) % 32];
}

/* v8 : le bit du motif pour la colonne x (bit 31 = x 0, cf. qgpu_proto.h). */
static inline bool qgpu_stipple_bit(uint32_t row, int x)
{
    return (row >> (31 - (uint32_t)(((x) % 32 + 32) % 32))) & 1u;
}

/* v7 : offset (en mots) de l'attribut `bit` (QGPU_VF_*) dans un sommet de
 * format `fmt`, ou -1 s'il est absent. La position est à l'offset 0. */
int qgpu_vf_offset(uint32_t fmt, uint32_t bit);

/*
 * Contrat d'un backend. Les pixels échangés avec le cœur sont des uint32_t
 * HÔTE-NATIFS 0xAARRGGBB, ligne par ligne, sans padding ; les profondeurs des
 * float dans [0,1] ; les sommets des float hôte-natifs, QGPU_VERTEX_WORDS par
 * sommet (x y z w r g b a, x/y en pixels, origine en haut à gauche, z dans
 * [0,1]). `st` est l'état GL du contexte courant (v2). Le cœur a déjà validé
 * toutes les bornes et toutes les valeurs d'état : un backend ne revérifie
 * rien, il renvoie false sur erreur interne.
 */
typedef struct QgpuBackend {
    const char *name;              /* 'soft', 'gl' */
    uint32_t    cap;               /* QGPU_CAP_* */
    bool (*init)(QgpuCore *c);     /* false = backend indisponible sur cet hôte */
    void (*fini)(QgpuCore *c);
    bool (*surf_create)(QgpuCore *c, QgpuSurface *s);
    void (*surf_destroy)(QgpuCore *c, QgpuSurface *s);
    bool (*clear)(QgpuCore *c, QgpuSurface *s, const QgpuState *st,
                  uint32_t mask, uint32_t argb, float depth);
    /* Dessin (v5). prim : QGPU_PRIM_*. `words` mots par sommet (8 + 4 par
       unité) : x y z f r g b a, puis s t r q de chaque unité, cf.
       qgpu_proto.h. tex[u] (u < QGPU_MAX_UNITS) : texture à appliquer par
       l'unité u (liée, complète, texturage actif) ou NULL ; ses coordonnées
       sont aux mots 8 + 4u … 11 + 4u. */
    bool (*draw)(QgpuCore *c, QgpuSurface *s, const QgpuState *st, uint32_t prim,
                 QgpuTexture *const *tex,
                 const float *verts, uint32_t nverts, uint32_t words);
    /* Dessin de sommets BRUTS (v7). Le cœur a tout validé et tout remis à plat :
       `verts` tient `nverts` sommets SERRÉS de `words` = QGPU_VF_WORDS(fmt)
       flottants hôte-natifs, dans l'ordre fixe du format ; `idx` tient `count`
       indices hôte-natifs déjà bornés à nverts, ou vaut NULL et le dessin est
       séquentiel de `first` à `first + count - 1`. `gm` porte matrices,
       lumières, matériaux, texgen et plans de découpe ; `mode` est le mode GL.
       C'est au backend de faire (ou de faire faire) transformation, éclairage,
       découpe, division perspective et viewport. */
    bool (*draw_raw)(QgpuCore *c, QgpuSurface *s, const QgpuState *st,
                     const QgpuGeom *gm, QgpuTexture *const *tex,
                     uint32_t mode, uint32_t fmt, const float *verts,
                     uint32_t nverts, uint32_t words,
                     const uint32_t *idx, uint32_t count, uint32_t first);
    bool (*readback)(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h, uint32_t *dst);
    bool (*upload)(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h, const uint32_t *src);
    bool (*depth_readback)(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, float *dst);
    bool (*depth_upload)(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, const float *src);
    /* v6 : stencil, un OCTET par pixel côté cœur (le fil en fait un mot de 32
       bits, cf. qgpu_proto.h). Appelés seulement si s->has_stencil. */
    bool (*stencil_readback)(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                             uint32_t w, uint32_t h, uint8_t *dst);
    bool (*stencil_upload)(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, const uint8_t *src);
    void (*tex_destroy)(QgpuCore *c, QgpuTexture *t);    /* libère t->priv */
    /* v8 : requêtes d'occlusion. Le cœur a validé l'identifiant et
       l'imbrication ; le backend remet le compte à zéro (begin), l'arrête
       (end) et le publie dans q->samples (result, synchrone). Absents ou
       init() n'ayant pas annoncé QGPU_CAP_OCCLUSION : les opcodes QUERY_*
       répondent QGPU_ST_BACKEND, ils ne plantent pas. */
    bool (*query_begin)(QgpuCore *c, QgpuQuery *q);
    bool (*query_end)(QgpuCore *c, QgpuQuery *q);
    bool (*query_result)(QgpuCore *c, QgpuQuery *q);
    void (*query_destroy)(QgpuCore *c, QgpuQuery *q);    /* libère q->priv */
} QgpuBackend;

struct QgpuCore {
    const QgpuBackend *be;
    void              *be_priv;
    /* QGPU_CAP_* réellement tenus : ceux du backend, plus ce que son init() a
       pu résoudre à chaud (v8 : QGPU_CAP_OCCLUSION). */
    uint32_t           caps;

    uint8_t  *shmem;               /* fenêtre partagée (BAR0), côté hôte */
    uint32_t  shmem_size;

    QgpuContext ctx[QGPU_MAX_CTX];
    QgpuSurface surf[QGPU_MAX_SURF];
    QgpuTexture tex[QGPU_MAX_TEX];
    QgpuQuery   query[QGPU_MAX_QUERIES];   /* v8 */
    int32_t     cur_ctx;           /* -1 si aucun */

    /* v8 : posés par le cœur juste avant chaque dessin, pour que le backend de
       référence n'ait pas à remonter au contexte courant. */
    const QgpuStipple *cur_stip;   /* motif de pointillé de polygone */
    QgpuQuery         *cur_query;  /* requête ouverte, ou NULL */
    int32_t            cur_sec;    /* v11 : mot de la couleur secondaire dans les
                                      sommets du dessin hérité en cours, ou -1 */

    uint32_t status;               /* QGPU_ST_* de la dernière exécution */
    uint32_t status_pc;            /* index (mots) de la commande fautive */
    uint32_t ncmds;                /* commandes exécutées en tout (stats) */
    bool     trace;                /* journalise chaque commande sur stderr */

    /* v13 : cible de SURF_PRESENT (VRAM qfb côté QEMU, tampon de test en
       natif). N'appartient pas au cœur : reset ne la touche pas. */
    uint8_t *scanout;
    uint32_t scanout_size;
    void   (*scanout_dirty)(void *opaque, uint32_t off, uint32_t len);
    void    *scanout_opaque;

    /* tampons de travail, agrandis à la demande */
    float    *vbuf; uint32_t vbuf_cap;   /* en floats */
    uint32_t *pbuf; uint32_t pbuf_cap;   /* en pixels */
    float    *dbuf; uint32_t dbuf_cap;   /* en pixels (profondeur) */
    uint8_t  *sbuf; uint32_t sbuf_cap;   /* en pixels (stencil, v6) */
    uint32_t *ibuf; uint32_t ibuf_cap;   /* en indices (v7, DRAW_RAW) */

    /* v14 : tampons persistants hors BAR0 */
    struct {
        bool     used;
        uint8_t *data;
        uint32_t size;
    } buf[QGPU_MAX_BUF];
};

/* Accès big-endian, sans dépendre des helpers QEMU. */
static inline uint32_t qgpu_ld32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline void qgpu_st32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static inline float qgpu_u2f(uint32_t u)
{
    union { uint32_t u; float f; } x; x.u = u; return x.f;
}
static inline uint32_t qgpu_f2u(float f)
{
    union { uint32_t u; float f; } x; x.f = f; return x.u;
}

/* Choisit et initialise le backend : "soft", "gl", ou "auto" (gl si
 * disponible, sinon soft). Renvoie false si aucun backend ne démarre. */
bool     qgpu_core_init(QgpuCore *c, const char *backend,
                        uint8_t *shmem, uint32_t shmem_size);
void     qgpu_core_fini(QgpuCore *c);
void     qgpu_core_reset(QgpuCore *c);       /* détruit tous les objets */

/* v13 : pose (ou retire, ram == NULL) la cible de SURF_PRESENT. Le pointeur
 * n'est pas copié : l'appelant en reste propriétaire. */
void     qgpu_core_set_scanout(QgpuCore *c, uint8_t *ram, uint32_t size,
                               void (*dirty)(void *opaque, uint32_t off,
                                             uint32_t len),
                               void *opaque);

/* Exécute un flux ; renvoie le statut (aussi dans c->status / status_pc).
 * off/len en octets dans la fenêtre partagée. */
uint32_t qgpu_core_execute(QgpuCore *c, uint32_t off, uint32_t len);

/* Nom du backend actif, empaqueté pour QGPU_REG_BACKEND_NAME. */
uint32_t qgpu_core_backend_tag(const QgpuCore *c);

extern const QgpuBackend qgpu_backend_soft;
extern const QgpuBackend qgpu_backend_gl;    /* stub si non compilé avec GL */

#endif /* QGPU_CORE_H */
