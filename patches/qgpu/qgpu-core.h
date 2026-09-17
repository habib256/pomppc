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
    uint32_t width, height, format;
    void    *priv;                 /* propriété du backend */
} QgpuSurface;

/* État GL d'un contexte (v2), indexé par QGPU_SK_*. ALPHA_REF est gardé en
   bits IEEE, comme sur le fil. */
typedef struct QgpuState {
    uint32_t v[QGPU_SK_COUNT];
} QgpuState;

/* Texture (v3). Les niveaux sont tenus par le cœur, en ARGB hôte-natif :
   le backend logiciel les échantillonne directement, le backend GL les
   recopie dans ses objets texture (tex->dirty). */
typedef struct QgpuTexLevel {
    uint32_t  w, h;
    uint32_t *px;                  /* NULL si le niveau n'est pas défini */
} QgpuTexLevel;

typedef struct QgpuTexture {
    bool         used;
    uint32_t     base_format;      /* format de base du niveau 0 */
    uint32_t     min_filter, mag_filter, wrap_s, wrap_t;
    QgpuTexLevel level[QGPU_MAX_TEX_LEVELS];
    uint32_t     dirty;            /* bit n : niveau n modifié depuis la dernière synchro */
    bool         params_dirty;
    void        *priv;             /* propriété du backend */
} QgpuTexture;

/* Nombre de niveaux utilisables (0 = texture incomplète). */
uint32_t qgpu_texture_levels(const QgpuTexture *t);

typedef struct QgpuContext {
    bool      used;
    int32_t   surf;                /* surface liée, -1 si aucune */
    int32_t   vp[4];               /* viewport (réservé) */
    QgpuState st;
} QgpuContext;

void qgpu_state_init(QgpuState *st);

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
    bool (*readback)(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                     uint32_t w, uint32_t h, uint32_t *dst);
    bool (*upload)(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h, const uint32_t *src);
    bool (*depth_readback)(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h, float *dst);
    bool (*depth_upload)(QgpuCore *c, QgpuSurface *s, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, const float *src);
    void (*tex_destroy)(QgpuCore *c, QgpuTexture *t);    /* libère t->priv */
} QgpuBackend;

struct QgpuCore {
    const QgpuBackend *be;
    void              *be_priv;

    uint8_t  *shmem;               /* fenêtre partagée (BAR0), côté hôte */
    uint32_t  shmem_size;

    QgpuContext ctx[QGPU_MAX_CTX];
    QgpuSurface surf[QGPU_MAX_SURF];
    QgpuTexture tex[QGPU_MAX_TEX];
    int32_t     cur_ctx;           /* -1 si aucun */

    uint32_t status;               /* QGPU_ST_* de la dernière exécution */
    uint32_t status_pc;            /* index (mots) de la commande fautive */
    uint32_t ncmds;                /* commandes exécutées en tout (stats) */
    bool     trace;                /* journalise chaque commande sur stderr */

    /* tampons de travail, agrandis à la demande */
    float    *vbuf; uint32_t vbuf_cap;   /* en floats */
    uint32_t *pbuf; uint32_t pbuf_cap;   /* en pixels */
    float    *dbuf; uint32_t dbuf_cap;   /* en pixels (profondeur) */
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

/* Exécute un flux ; renvoie le statut (aussi dans c->status / status_pc).
 * off/len en octets dans la fenêtre partagée. */
uint32_t qgpu_core_execute(QgpuCore *c, uint32_t off, uint32_t len);

/* Nom du backend actif, empaqueté pour QGPU_REG_BACKEND_NAME. */
uint32_t qgpu_core_backend_tag(const QgpuCore *c);

extern const QgpuBackend qgpu_backend_soft;
extern const QgpuBackend qgpu_backend_gl;    /* stub si non compilé avec GL */

#endif /* QGPU_CORE_H */
