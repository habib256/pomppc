/*
 * pomppc_gld.c — plugin OpenGL (« GLD ») de Tiger pour le GPU paravirtuel qgpu.
 *
 * GLEngine (OpenGL.framework) charge ce bundle comme un rendu « GLDriver* »
 * et l'appelle par les 63 points d'entrée gld* (docs/gpu-3d-tiger.md §5).
 * Le plugin est un MANDATAIRE du rendu logiciel d'Apple (GLDriver.bundle),
 * chargé ici en module privé : tout ce que le plugin ne sait pas accélérer
 * reste exact, parce que c'est le code d'Apple qui le fait.
 *
 * Étape 1 (ce fichier, POMPPC_GLTRACE=dossier) : traceur. Chaque appel gld*
 * et chaque procédure de rastérisation est journalisé, avec des vidages
 * binaires des structures utiles (contexte, sommets, état GL) pour en établir
 * la disposition sur pièces.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <mach-o/dyld.h>

#include "pomppc_gld.h"

#define GLD_REAL_PATH \
    "/System/Library/Frameworks/OpenGL.framework/Versions/A/Resources/GLDriver.bundle/GLDriver"

const char *const pomppc_gld_names[GLD_COUNT] = {
#define X(n) #n,
    GLD_LIST
#undef X
};

void *pomppc_real[GLD_COUNT];

static pthread_once_t load_once = PTHREAD_ONCE_INIT;
static int loaded_ok;

/* ────────────────────────────── trace ────────────────────────────── */

static FILE *trace_fp;
static char trace_dir[256];
static int trace_on = -1;
static unsigned long dump_seq;
static pthread_mutex_t trace_mu = PTHREAD_MUTEX_INITIALIZER;

int pomppc_tracing(void)
{
    if (trace_on < 0) {
        const char *d = getenv("POMPPC_GLTRACE");
        trace_on = 0;
        if (d && *d) {
            char p[300];
            snprintf(trace_dir, sizeof(trace_dir), "%s", d);
            snprintf(p, sizeof(p), "%s/trace.txt", d);
            trace_fp = fopen(p, "a");
            if (trace_fp) {
                setvbuf(trace_fp, 0, _IOLBF, 0);
                trace_on = 1;
            }
        }
    }
    return trace_on;
}

void pomppc_log(const char *fmt, ...)
{
    va_list ap;
    if (!pomppc_tracing())
        return;
    pthread_mutex_lock(&trace_mu);
    va_start(ap, fmt);
    vfprintf(trace_fp, fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&trace_mu);
}

/* Vidage binaire dans le dossier de trace ; renvoie le numéro de vidage. */
unsigned long pomppc_dump(const char *tag, const void *p, unsigned long len)
{
    char path[400];
    FILE *f;
    unsigned long n;
    if (!pomppc_tracing() || !p)
        return 0;
    pthread_mutex_lock(&trace_mu);
    n = ++dump_seq;
    pthread_mutex_unlock(&trace_mu);
    snprintf(path, sizeof(path), "%s/%04lu-%s.bin", trace_dir, n, tag);
    f = fopen(path, "wb");
    if (f) {
        fwrite(p, 1, len, f);
        fclose(f);
    }
    pomppc_log("  [vidage %04lu %s %lu octets @%p]\n", n, tag, len, p);
    return n;
}

const char *pomppc_proc_name(int slot);
#define proc_name pomppc_proc_name
const char *pomppc_proc_name(int slot)
{
    static const char *n[PROC_COUNT] = {
        "Accum", "Clear", "ReadPixels", "DrawPixels", "CopyPixels", "RenderBitmap",
        "RenderPoints", "RenderLines", "RenderLineStrip", "RenderLineLoop",
        "RenderPolygon", "RenderTriangles", "RenderTriangleFan", "RenderTriangleStrip",
        "RenderQuads", "RenderQuadStrip", "RenderPointsPtr", "RenderLinesPtr",
        "RenderPolygonPtr", "RenderVertexBuffer", "BeginPrimitiveBuffer",
        "EndPrimitiveBuffer", "Swap58", "Swap5c", "Swap60", "Noop64", "Proc68",
        "Proc6c", "RenderVertexArray", "CopyTexSubImage", "ModifyTexSubImage",
        "GenerateTexMipmaps", "BufferSubData", "Proc84", "Proc88", "Proc8c",
    };
    return (slot >= 0 && slot < PROC_COUNT) ? n[slot] : "?";
}

static void dump_vertices(const char *tag, const unsigned char *v, unsigned long n)
{
    unsigned long i;
    if (n > 8)
        n = 8;
    if (getenv("POMPPC_GLTRACE_VERTS"))
        pomppc_dump(tag, v, n * GLD_VERTEX_SIZE);
    for (i = 0; i < n; i++) {
        const float *f = (const float *)(v + i * GLD_VERTEX_SIZE);
        pomppc_log("    v%lu xyzw=(%g %g %g %g) +30 rgba=(%g %g %g %g) +40=(%g %g %g %g)\n",
                   i, f[0], f[1], f[2], f[3], f[12], f[13], f[14], f[15],
                   f[16], f[17], f[18], f[19]);
    }
}

static void trace_proc(int id, unsigned long *a);

/* Crochet appelé par chaque trampoline ; renvoie la fonction à appeler. */
void *pomppc_pre(int id, unsigned long *a)
{
    if (id < 1000) {
        if (!pomppc_load_real())
            return 0;            /* impossible : GLEngine ne nous appelle qu'après Initialize */
        if (pomppc_tracing())
            pomppc_log("gld%s(%08lx %08lx %08lx %08lx %08lx %08lx)\n",
                       id < GLD_COUNT ? pomppc_gld_names[id] : "?",
                       a[0], a[1], a[2], a[3], a[4], a[5]);
        {
            void *sonde = pomppc_tcl_gld(id);
            if (sonde)
                return sonde;
        }
        return pomppc_real[id];
    }
    id -= 1000;
    if (pomppc_tracing())
        trace_proc(id, a);
    return pomppc_proc_pre(id, a);
}

static void trace_proc(int id, unsigned long *a)
{
    pomppc_log("  proc %s(%08lx %08lx %08lx %08lx %08lx)\n", proc_name(id),
               a[0], a[1], a[2], a[3], a[4]);
    switch (id) {
    case PROC_RenderTriangles:
    case PROC_RenderTriangleStrip:
    case PROC_RenderQuads:
    case PROC_RenderPolygon:
        dump_vertices("tri", (const unsigned char *)a[1], a[2]);
        break;
    case PROC_RenderTriangleFan:
        dump_vertices("fan-hub", (const unsigned char *)a[1], 1);
        dump_vertices("fan", (const unsigned char *)a[2], a[3]);
        break;
    case PROC_RenderQuadStrip:
        dump_vertices("qstrip", (const unsigned char *)a[1], a[2]);
        break;
    case PROC_RenderLines: case PROC_RenderLineStrip: case PROC_RenderLineLoop:
    case PROC_RenderPoints:
        dump_vertices("lp", (const unsigned char *)a[1], a[2]);
        break;
    case PROC_RenderLinesPtr: case PROC_RenderPointsPtr: {
        unsigned long i, *pp = (unsigned long *)a[1];
        for (i = 0; i < a[2] && i < 8; i++)
            dump_vertices("lpptr", (const unsigned char *)pp[i], 1);
        break;
    }
    case PROC_RenderPolygonPtr: {
        unsigned long i, *pp = (unsigned long *)a[1];
        for (i = 0; i < a[2] && i < 8; i++)
            dump_vertices("polyptr", (const unsigned char *)pp[i], 1);
        break;
    }
    case PROC_Clear: {
        static int once;
        const unsigned char *ctx = (const unsigned char *)a[0];
        unsigned long gls = GLD_U32(ctx, 0x0c);
        if (!once && gls) {
            /* Contrôle du verrou T&L au moment du dessin : gctx+0x468c est le
               bloc de configuration retenu par GLEngine, gctx+0x7580/0x7581 la
               copie qu'en fait _gleInitGLIState (capacites-glengine.md §5.1). */
            const unsigned char *g = (const unsigned char *)(gls - 0x360);
            unsigned long cfg = GLD_U32(g, 0x468c);
            pomppc_log("  [verrou] gctx=%p cfg(468c)=%08lx cfg+78..7b=%02x %02x %02x %02x"
                       " gctx+7580=%u gctx+7581=%u\n", (void *)g, cfg,
                       cfg ? GLD_U8(cfg, 0x78) : 0, cfg ? GLD_U8(cfg, 0x79) : 0,
                       cfg ? GLD_U8(cfg, 0x7a) : 0, cfg ? GLD_U8(cfg, 0x7b) : 0,
                       GLD_U8(g, 0x7580), GLD_U8(g, 0x7581));
        }
        if (once++ < 1 || getenv("POMPPC_GLTRACE_STATE")) {
            unsigned long units = GLD_U32(ctx, 0x10);
            /* 0x5400 : le bloc va au moins jusqu'à GS+0x4310, et les sondes de
               docs/re/tableaux-de-sommets.md §7 lisent GS+0x4700/0x4a70/0x50c0. */
            pomppc_dump("clear-glstate", (void *)gls, 0x5400);
            pomppc_dump("clear-ctx", ctx, 0x704);
            /* les 0x360 octets qui PRÉCÈDENT le bloc pilote (gctx+0x000 …
               gctx+0x35f) : attributs courants hors glBegin — coordonnées de
               texture gctx+0x120+u·0x10, couleur gctx+0x2a0, normale gctx+0x2b0
               (tableaux-de-sommets.md §5.9). Trace seulement. */
            pomppc_dump("clear-gctxlow", (void *)(gls - 0x360), 0x360);
            /* objets désignés par des pointeurs du bloc : tableau de sommets
               courant (GS+0x4700), matériaux avant/arrière (GS+0x4a70/0x4a74),
               pipeline program courant (GS+0x50c0). */
            if (gls) {
                if (GLD_U32(gls, 0x4700))
                    pomppc_dump("clear-vao", (void *)GLD_U32(gls, 0x4700), 0x600);
                if (GLD_U32(gls, 0x4a70))
                    pomppc_dump("clear-matf", (void *)GLD_U32(gls, 0x4a70), 0x240);
                if (GLD_U32(gls, 0x4a74))
                    pomppc_dump("clear-matb", (void *)GLD_U32(gls, 0x4a74), 0x240);
                if (GLD_U32(gls, 0x50c0))
                    pomppc_dump("clear-pp", (void *)GLD_U32(gls, 0x50c0), 0x520);
            }
            /* objet texture GLEngine lié à l'unité 0, cible 2D (indice 3) */
            if (units && GLD_U32(units, 3 * 4)) {
                unsigned long t = GLD_U32(units, 3 * 4);
                pomppc_dump("clear-tex2d", (void *)t, 0x200);
                pomppc_log("  texobj unité0/2D = %08lx\n", t);
            }
        }
        break;
    }
    case PROC_Swap58: {
        const unsigned char *ctx = (const unsigned char *)a[0];
        unsigned long w = GLD_U32(ctx, 0x1c), h = GLD_U32(ctx, 0x20);
        pomppc_dump("swap-ctx", ctx, 0x704);
        pomppc_dump("swap-glsdrawable", (void *)GLD_U32(ctx, 4), 0x100);
        if (GLD_U32(ctx, 0x88))
            pomppc_dump("swap-depth", (void *)GLD_U32(ctx, 0x88), w * h * 4);
        if (GLD_U32(ctx, 0x74))
            pomppc_dump("swap-color", (void *)GLD_U32(ctx, 0x74), w * h * 4);
        break;
    }
    default:
        break;
    }
}

/* ───────────── sonde « T&L matérielle » (POMPPC_GL_TCL=1) ─────────────
 *
 * L'octet +0x79 du bloc de configuration (5ᵉ argument de gldCreateContext) est
 * recopié par _gleInitGLIState dans gctx+0x7580 ; à 1, GLEngine cesse de
 * transformer lui-même. Mais ce n'est que la valeur INITIALE : vérifié dans
 * l'invité le 18/09/2026 (docs/re/verification-tcl.md), il faut en plus
 *   — rendre le bit 0 (et le bit 1) depuis gldInitDispatch/gldUpdateDispatch,
 *     que _gleUpdateDispatchCodeChange relit à chaque changement d'état ;
 *   — publier un descripteur de sortie de sommet en cfg+0x11c, sans quoi
 *     GLEngine prend le chemin T&L mais perd la géométrie en silence.
 * Alors seulement +0x50/+0x54 (Begin/EndPrimitiveBuffer) reçoivent les sommets,
 * en coordonnées d'OBJET.
 *
 * Tout ce bloc ne sert qu'au RELEVÉ : il n'est actif que si POMPPC_GL_TCL=1,
 * et il se contente de journaliser puis de rendre « non pris en charge ».
 * Sans la variable, le plugin se comporte exactement comme avant.
 *
 * ⚠ Poser le verrou sans installer +0x50 ferait écrire GLEngine à l'adresse 0
 * (le bouchon d'Apple rend un pointeur nul) : les deux vont ensemble.
 */

int pomppc_tcl(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("POMPPC_GL_TCL");
        on = (e && *e) ? atoi(e) : 0;
        if (on < 0)
            on = 0;
    }
    return on;                    /* 1 = par cfg+0x79 ; 2 = + forçage gctx+0x7580 */
}

/* Second verrou (§5.3 du relevé) : à 0, le moindre changement d'état en cours
   de primitive renvoie GLEngine au chemin logiciel (_gleForceToSoftwareTCL).
   POMPPC_GL_TCL_7A=0 permet d'observer ce repli. */
static int pomppc_tcl_7a(void)
{
    const char *e = getenv("POMPPC_GL_TCL_7A");
    return (e && *e) ? (*e != '0') : 1;
}

/* Tampon rendu à GLEngine par BeginPrimitiveBuffer : c'est lui qui y écrit
   les sommets. Assez grand pour 1024 sommets au pas maximal observé. */
#define TCL_BUF_BYTES (512u * 1024u)
static unsigned char tcl_buf[TCL_BUF_BYTES];
static unsigned long tcl_stride, tcl_slots;

/* Le bloc d'état vu par le pilote est gctx+0x360 (tableaux-de-sommets.md §0) :
   on remonte au contexte GLEngine pour lire matrices, pas de sommet, etc. */
static unsigned char *tcl_gctx(void *drvctx)
{
    unsigned long gls = drvctx ? GLD_U32(drvctx, 0x0c) : 0;
    return gls ? (unsigned char *)(gls - 0x360) : 0;
}

/* Matrice m du tableau des 24 (gctx+0x1bc0 + m·0x40 ; 0 = MVP, 3 = projection,
   4 = modèle-vue, 16+u = texture de l'unité u). */
static void tcl_log_matrix(const unsigned char *g, const char *nom, int m)
{
    const float *f = (const float *)(g + 0x1bc0 + m * 0x40);
    pomppc_log("    %-9s[%2d] %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
               nom, m, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
               f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
}

static void tcl_log_state(const char *quoi, void *drvctx)
{
    const unsigned char *g = tcl_gctx(drvctx);
    if (!g) {
        pomppc_log("  [%s] contexte GLEngine introuvable\n", quoi);
        return;
    }
    pomppc_log("  [%s] gctx=%p 7580=%u 7581=%u mode(4a10)=%d desc(48d0)=%08lx "
               "vao(4a60)=%08lx fmt(498c)=%08lx pasfmt(4990)=%08lx\n",
               quoi, (void *)g, GLD_U8(g, 0x7580), GLD_U8(g, 0x7581),
               (int)(short)GLD_U16(g, 0x4a10), GLD_U32(g, 0x48d0),
               GLD_U32(g, 0x4a60), GLD_U32(g, 0x498c), GLD_U32(g, 0x4990));
    pomppc_log("        4854=%08lx 4858=%08lx 485c=%08lx 487c=%08lx 4880=%04x 4882=%04x\n",
               GLD_U32(g, 0x4854), GLD_U32(g, 0x4858), GLD_U32(g, 0x485c),
               GLD_U16(g, 0x487c), GLD_U16(g, 0x4880), GLD_U16(g, 0x4882));
    tcl_log_matrix(g, "MVP", 0);
    tcl_log_matrix(g, "PROJ", 3);
    tcl_log_matrix(g, "MODELVIEW", 4);
}

/* Journal des 32 emplacements d'attribut de l'objet « tableau de sommets »
   courant (A = V+0x10, emplacement a en A+0x20+0x18a — §2 du relevé). */
static void tcl_log_vao(const unsigned char *g)
{
    unsigned long v = g ? GLD_U32(g, 0x4a60) : 0;
    const unsigned char *A;
    unsigned long hi, lo;
    int a;
    if (!v)
        return;
    A = (const unsigned char *)(v + 0x10);
    hi = GLD_U32(A, 0x320);
    lo = GLD_U32(A, 0x324);
    pomppc_log("    vao V=%08lx actifs=%08lx:%08lx vbo=%08lx:%08lx sale=%08lx:%08lx\n",
               v, hi, lo, GLD_U32(A, 0x328), GLD_U32(A, 0x32c),
               GLD_U32(A, 0x340), GLD_U32(A, 0x344));
    for (a = 0; a < 32; a++) {
        const unsigned char *s = A + 0x20 + a * 0x18;
        /* bit de l'attribut a = 16+a dans le masque 64 bits (mot haut d'abord) */
        unsigned long m = (16 + a) < 32 ? (hi >> (16 + a)) : (lo >> (16 + a - 32));
        if (!(m & 1))
            continue;
        pomppc_log("      attr %2d ptr=%08lx pas=%lu type=%04x taille=%u octets=%u sig=%08lx\n",
                   a, GLD_U32(s, 0x00), GLD_U32(s, 0x04), GLD_U16(s, 0x08),
                   GLD_U16(s, 0x0a), GLD_U8(s, 0x0c), GLD_U32(s, 0x14));
    }
    pomppc_dump("tcl-vao", (const void *)v, 0x600);
}

/* +0x50 BeginPrimitiveBuffer(ctx, mode, &nSommets) -> tampon.
   Rendre 0 ferait écrire GLEngine à l'adresse 0 : on rend un vrai tampon. */
static void *tcl_begin_primitive_buffer(void *ctx, short mode, unsigned long *n)
{
    const unsigned char *g = tcl_gctx(ctx);
    /* le pas d'un sommet est en gctx+0x4880 (u16), posé par
       _gleSelectVertexSubmitFunc à partir de l'octet 2 du descripteur × 4 */
    unsigned long pas = g ? GLD_U16(g, 0x4880) : 0;
    pomppc_log("TCL BeginPrimitiveBuffer(ctx=%p mode=%d *n=%lu) pas=%lu\n",
               ctx, (int)mode, n ? *n : 0, pas);
    tcl_log_state("begin", ctx);
    tcl_stride = (pas && pas <= 0x400) ? pas : GLD_VERTEX_SIZE;
    tcl_slots = TCL_BUF_BYTES / tcl_stride;
    if (tcl_slots > 1024)
        tcl_slots = 1024;
    /* motif témoin : ce que GLEngine n'écrit pas reste 0xEE dans le vidage */
    memset(tcl_buf, 0xEE, tcl_slots * tcl_stride < 0x4000 ? tcl_slots * tcl_stride : 0x4000);
    if (n)
        *n = tcl_slots;
    pomppc_log("  -> tampon %p, %lu emplacements de %lu octets\n",
               (void *)tcl_buf, tcl_slots, tcl_stride);
    return tcl_buf;
}

/* +0x54 EndPrimitiveBuffer(ctx, drapeau, mode, nSommets) : GLEngine a écrit
   nSommets sommets dans le tampon rendu par +0x50. On ne dessine rien. */
static void tcl_end_primitive_buffer(void *ctx, long flag, short mode, long n)
{
    unsigned long i, bytes;
    pomppc_log("TCL EndPrimitiveBuffer(ctx=%p drapeau=%ld mode=%d n=%ld) pas=%lu\n",
               ctx, flag, (int)mode, n, tcl_stride);
    if (n <= 0)
        return;
    bytes = (unsigned long)n * tcl_stride;
    if (bytes > 0x4000)
        bytes = 0x4000;
    pomppc_dump("tcl-prim", tcl_buf, bytes);
    /* Un sommet par ligne, TOUS les mots du pas (jusqu'à 32) : c'est ce
       vidage qui identifie ce que GLEngine a écrit pour chaque code du
       descripteur. Les mots jamais écrits gardent le motif témoin 0xEEEEEEEE
       posé par BeginPrimitiveBuffer, journalisé « - ». */
    for (i = 0; i < (unsigned long)n && i < 8; i++) {
        const unsigned char *v = tcl_buf + i * tcl_stride;
        unsigned long nf = tcl_stride / 4, j;
        if (nf > 32)
            nf = 32;
        pomppc_log("    s%lu :", i);
        for (j = 0; j < nf; j++) {
            unsigned long u = GLD_U32(v, j * 4);
            if (u == 0xEEEEEEEEul)
                pomppc_log(" -");
            else
                pomppc_log(" %g", *(const float *)(v + j * 4));
        }
        pomppc_log("\n");
    }
}

/* +0x4c RenderVertexBuffer(ctx, tampon, mode, biais, n, typeIndices, indices).
   0 = « non pris en charge » (le GLDriver d'Apple ne fait que blr). */
static long tcl_render_vertex_buffer(void *ctx, void *buf, long mode, long biais,
                                     long n, long itype, const void *idx)
{
    pomppc_log("TCL RenderVertexBuffer(ctx=%p tampon=%p mode=%ld biais=%ld n=%ld "
               "typeidx=%lx idx=%p)\n", ctx, buf, mode, biais, n, itype, idx);
    tcl_log_state("rvb", ctx);
    if (buf)
        pomppc_dump("tcl-vbuf", buf, 0x800);
    if (idx)
        pomppc_dump("tcl-vbuf-idx", idx, (n > 0 && n < 512) ? (unsigned long)n * 4 : 0x400);
    return 0;
}

/* +0x70 RenderVertexArray : 9 arguments, le 9ᵉ sur la pile (ABI Mach-O PPC,
   56(r1)) — le compilateur s'en charge dès qu'on le déclare. */
static long tcl_render_vertex_array(void *ctx, long indexe, long mode, long premier,
                                    long nombre, long itype, const void *indices,
                                    void *tamponSommets, void *attributsCourants)
{
    const unsigned char *g = tcl_gctx(ctx);
    pomppc_log("TCL RenderVertexArray(ctx=%p indexe=%ld mode=%ld premier=%ld nombre=%ld "
               "typeidx=%lx idx=%p vbuf=%p attrs=%p)\n",
               ctx, indexe, mode, premier, nombre, itype, indices,
               tamponSommets, attributsCourants);
    tcl_log_state("rva", ctx);
    tcl_log_vao(g);
    if (attributsCourants)
        pomppc_dump("tcl-curattr", attributsCourants, 0x140);
    if (indices && nombre > 0 && nombre < 4096)
        pomppc_dump("tcl-idx", indices, (unsigned long)nombre * 4);
    return 0;
}

/* Le VRAI verrou, relevé sur pièces le 18/09/2026 : _gleUpdateDispatchCodeChange
 * (GLEngine 0xc8d20) refait gctx+0x7580 à partir du BIT 0 de la valeur rendue par
 * gldInitDispatch / gldUpdateDispatch, à chaque changement d'état :
 *     lbz r0,0x7580 / rlwinm r2,r29,0,30,31 / cmpw / beq
 *     rlwinm r0,r29,0,31,31 / stb r0,0x7580
 * (bit 2 -> gctx+0x7581 = CGLGetParameter 311 ; bit 3 -> gctx+0x759d, opérations
 * de tampon d'image). Le GLDriver d'Apple rend 4 : bit 0 à zéro, donc T&L
 * logicielle, quoi qu'on ait mis dans cfg+0x79. */
long pomppc_tcl_dispatch_ret(long r, const char *quand)
{
    /* bit 0 = T&L au pilote ; bit 1 = « refaire le chemin » (sans lui,
       _gleUpdateDispatchCodeChange compare (retour & 3) à gctx+0x7580, trouve
       égal et saute tout le bloc, dont la relecture de cfg+0x11c en gctx+0x48d0).
       POMPPC_GL_TCL_BITS permet d'essayer d'autres combinaisons. */
    const char *b;
    long bits;
    if (!pomppc_tcl())
        return r;
    b = getenv("POMPPC_GL_TCL_BITS");
    bits = (b && *b) ? strtol(b, 0, 0) : 3;
    pomppc_log("  [verrou %s] retour d'Apple %ld -> %ld (bits %ld)\n",
               quand, r, r | bits, bits);
    return r | bits;
}

/* Appelé à chaque (re)construction de la table de procédures, c'est-à-dire
   après la création du contexte et à chaque changement d'état : c'est le seul
   endroit où l'on voit, du plugin, la valeur que GLEngine a réellement retenue. */
void pomppc_tcl_check(void *drvctx, const char *quand)
{
    unsigned char *g = tcl_gctx(drvctx);
    unsigned long cfg;
    if (!g)
        return;
    cfg = GLD_U32(g, 0x468c);
    pomppc_log("  [verrou %s] gctx=%p cfg(468c)=%08lx cfg+78..7b=%02x %02x %02x %02x"
               " 7580=%u 7581=%u\n", quand, (void *)g, cfg,
               cfg ? GLD_U8(cfg, 0x78) : 0, cfg ? GLD_U8(cfg, 0x79) : 0,
               cfg ? GLD_U8(cfg, 0x7a) : 0, cfg ? GLD_U8(cfg, 0x7b) : 0,
               GLD_U8(g, 0x7580), GLD_U8(g, 0x7581));
    /* POMPPC_GL_TCL=2 : on pose directement la copie que _gleInitGLIState a
       faite, pour savoir si le chemin T&L fonctionne même quand le bloc de
       configuration a été lu trop tôt. Sonde uniquement. */
    if (pomppc_tcl() >= 2 && GLD_U8(g, 0x7580) != 1) {
        GLD_U8(g, 0x7580) = 1;
        GLD_U8(g, 0x7581) = 1;
        pomppc_log("  [verrou %s] forcé gctx+0x7580 = 1\n", quand);
    }
}

/* Le chemin « tableaux de sommets » passe par gldAllocVertexBuffer : GLEngine
 * y écrit les sommets au format que le pilote a publié dans cfg+0x7c..0x87,
 * puis appelle la procédure +0x4c RenderVertexBuffer. Le bouchon d'Apple rend
 * 0 : sans ces trois entrées, GLEngine perd la géométrie en silence.
 * Relevé GLEngine 0xda5a8-0xda65c : l'identifiant de format vient de
 * cfg+0x7c/0x7e/0x80/0x82/0x84/0x86 et le PAS est imposé par GLEngine
 * (0x10, 0x18, 0x20, 0x14, 0x20|0x24, 0x2c|0x34 selon cfg+0x94 & 8). */
#define TCL_VB_BYTES (512u * 1024u)
static unsigned char tcl_vb[TCL_VB_BYTES];
static unsigned long tcl_vb_stride, tcl_vb_slots;

static void *tcl_alloc_vertex_buffer(void *ctx, unsigned long idfmt, unsigned long *n)
{
    const unsigned char *g = tcl_gctx(ctx);
    unsigned long pas = g ? GLD_U16(g, 0x4990) : 0;
    tcl_vb_stride = (pas && pas <= 0x400) ? pas : 0x10;
    tcl_vb_slots = TCL_VB_BYTES / tcl_vb_stride;
    if (tcl_vb_slots > 2048)
        tcl_vb_slots = 2048;                 /* comme GeForce3 et Rage 128 */
    pomppc_log("TCL AllocVertexBuffer(ctx=%p idfmt=%08lx *n=%lu) fmt(498c)=%08lx pas(4990)=%lu"
               " -> %p x%lu\n", ctx, idfmt, n ? *n : 0,
               g ? GLD_U32(g, 0x498c) : 0, tcl_vb_stride,
               (void *)tcl_vb, tcl_vb_slots);
    memset(tcl_vb, 0xEE, 0x4000);
    if (n && *n > tcl_vb_slots)
        *n = tcl_vb_slots;
    return tcl_vb;
}

static long tcl_complete_vertex_buffer(void *ctx, void *buf, unsigned long used)
{
    pomppc_log("TCL CompleteVertexBuffer(ctx=%p tampon=%p utilises=%lu) pas=%lu\n",
               ctx, buf, used, tcl_vb_stride);
    return 0;
}

static long tcl_free_vertex_buffer(void *ctx, void *buf)
{
    pomppc_log("TCL FreeVertexBuffer(ctx=%p tampon=%p)\n", ctx, buf);
    return 0;
}

/* Entrées gld* surchargées par la sonde (le trampoline demande la cible à
   pomppc_pre, on n'a donc pas à toucher gld_tramp.s). */
void *pomppc_tcl_gld(int id)
{
    if (!pomppc_tcl())
        return 0;
    switch (id) {
    case GLD_AllocVertexBuffer:    return (void *)tcl_alloc_vertex_buffer;
    case GLD_CompleteVertexBuffer: return (void *)tcl_complete_vertex_buffer;
    case GLD_FreeVertexBuffer:     return (void *)tcl_free_vertex_buffer;
    default:                       return 0;
    }
}

void *pomppc_tcl_proc(int slot)
{
    if (!pomppc_tcl())
        return 0;
    switch (slot) {
    case PROC_BeginPrimitiveBuffer: return (void *)tcl_begin_primitive_buffer;
    case PROC_EndPrimitiveBuffer:   return (void *)tcl_end_primitive_buffer;
    case PROC_RenderVertexBuffer:   return (void *)tcl_render_vertex_buffer;
    case PROC_RenderVertexArray:    return (void *)tcl_render_vertex_array;
    default:                        return 0;
    }
}

/* ─────────────────────── chargement du rendu réel ─────────────────────── */

static void load_real(void)
{
    NSObjectFileImage img;
    NSModule mod;
    int i;

    if (NSCreateObjectFileImageFromFile(GLD_REAL_PATH, &img) != NSObjectFileImageSuccess) {
        pomppc_log("POMPPC: impossible d'ouvrir %s\n", GLD_REAL_PATH);
        return;
    }
    mod = NSLinkModule(img, GLD_REAL_PATH,
                       NSLINKMODULE_OPTION_PRIVATE | NSLINKMODULE_OPTION_RETURN_ON_ERROR);
    if (!mod) {
        pomppc_log("POMPPC: NSLinkModule a échoué\n");
        return;
    }
    for (i = 0; i < GLD_COUNT; i++) {
        char sym[64];
        NSSymbol s;
        snprintf(sym, sizeof(sym), "_gld%s", pomppc_gld_names[i]);
        s = NSLookupSymbolInModule(mod, sym);
        pomppc_real[i] = s ? NSAddressOfSymbol(s) : 0;
        if (!pomppc_real[i]) {
            pomppc_log("POMPPC: symbole réel manquant %s\n", sym);
            return;
        }
    }
    loaded_ok = 1;
    pomppc_log("POMPPC: rendu logiciel Apple chargé en privé (%d points d'entrée)\n", GLD_COUNT);
}

int pomppc_load_real(void)
{
    pthread_once(&load_once, load_real);
    return loaded_ok;
}

/* ────────────────────────────── surcharges C ────────────────────────────── */

typedef long (*gld8_fn)(long, long, long, long, long, long, long, long);

long pomppc_call_real(int idx, long a, long b, long c, long d, long e, long f, long g, long h)
{
    if (!pomppc_load_real())
        return 0x2710;                       /* kCGLBadAttribute : plugin inutilisable */
    return ((gld8_fn)pomppc_real[idx])(a, b, c, d, e, f, g, h);
}

#define FWD8(idx) pomppc_call_real(idx, a, b, c, d, e, f, g, h)

long gldInitializeLibrary(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r;
    pomppc_log("gldInitializeLibrary(%08lx %08lx %08lx %08lx %08lx) pid %d\n",
               a, b, c, d, e, (int)getpid());
    r = FWD8(GLD_InitializeLibrary);
    pomppc_backend_init();
    return r;
}

long gldTerminateLibrary(long a, long b, long c, long d, long e, long f, long g, long h)
{
    pomppc_log("gldTerminateLibrary()\n");
    return FWD8(GLD_TerminateLibrary);
}

long gldGetVersion(long *maj, long *min, long *rev, long *id)
{
    long r = pomppc_call_real(GLD_GetVersion, (long)maj, (long)min, (long)rev, (long)id, 0, 0, 0, 0);
    /* L'identifiant (octet 0xff00) doit être unique parmi les plugins chargés
       (_glepValidatePlugin) : 0x200 est celui du GLDriver d'Apple. */
    *id = (*id & ~0xff00L) | POMPPC_PLUGIN_ID;
    pomppc_log("gldGetVersion -> %ld.%ld.%ld id 0x%lx\n", *maj, *min, *rev, *id);
    return r;
}

long gldGetRendererInfo(unsigned char *info, long mask, long c, long d, long e, long f, long g, long h)
{
    long a = (long)info, b = mask;
    long r = FWD8(GLD_GetRendererInfo);
    if (r == 0)
        pomppc_patch_renderer_info(info);
    pomppc_log("gldGetRendererInfo(mask %lx) -> %ld id %08lx flags %08lx\n", mask, r,
               GLD_U32(info, 4), GLD_U32(info, 8));
    return r;
}

long gldChoosePixelFormat(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r;
    pomppc_log("gldChoosePixelFormat(%08lx %08lx %08lx %08lx %08lx)\n", a, b, c, d, e);
    if (b) {
        const long *at = (const long *)b;
        int i;
        pomppc_log("  attributs :");
        for (i = 0; i < 40 && at[i]; i++)
            pomppc_log(" %ld", at[i]);
        pomppc_log("\n");
    }
    /* Le GLDriver d'Apple ne connaît ni nos identifiants ni la demande
       « accéléré » : il reçoit une copie traduite, jamais le tableau de GLEngine. */
    {
        long copy[256];
        if (b && pomppc_translate_attribs((const long *)b, copy, 256))
            b = (long)copy;
        r = FWD8(GLD_ChoosePixelFormat);
    }
    if (r == 0 && a)
        pomppc_patch_pixel_list(*(void **)a);
    pomppc_log("  -> %ld\n", r);
    return r;
}

long gldDestroyPixelFormat(long a, long b, long c, long d, long e, long f, long g, long h)
{
    pomppc_log("gldDestroyPixelFormat(%08lx)\n", a);
    pomppc_unpatch_pixel_list((void *)a);
    return FWD8(GLD_DestroyPixelFormat);
}

long gldCreateContext(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r;
    pomppc_unpatch_pixel_format((void *)b);
    r = FWD8(GLD_CreateContext);
    pomppc_patch_pixel_format((void *)b);
    pomppc_log("gldCreateContext(%08lx %08lx %08lx %08lx %08lx %08lx %08lx) -> %ld ctx %08lx\n",
               a, b, c, d, e, f, g, r, a ? *(long *)a : 0);
    /* 5ᵉ argument = bloc de configuration (docs/re/capacites-glengine.md §4) :
       GLEngine l'a mis à zéro, le GLDriver d'Apple vient d'y poser les limites,
       les 79 bits d'extensions et les quatre octets de capacités +0x78..+0x7b.
       On le vide APRÈS l'appel, sinon on ne lirait que des zéros. */
    if (r == 0 && e && pomppc_tracing()) {
        const unsigned char *cfg = (const unsigned char *)e;
        pomppc_dump("cfg", cfg, 0x200);
        pomppc_log("  cfg +78..7b = %02x %02x %02x %02x ; +b4 unités=%u +b6=%u +ba=%u ; "
                   "+bc taille max=%u\n",
                   GLD_U8(cfg, 0x78), GLD_U8(cfg, 0x79), GLD_U8(cfg, 0x7a), GLD_U8(cfg, 0x7b),
                   GLD_U16(cfg, 0xb4), GLD_U16(cfg, 0xb6), GLD_U16(cfg, 0xba),
                   GLD_U16(cfg, 0xbc));
        pomppc_log("  cfg ext +124=%08lx +128=%08lx +12c=%08lx ; +00=%08lx +08=%08lx "
                   "+0c=%08lx +10=%08lx\n",
                   GLD_U32(cfg, 0x124), GLD_U32(cfg, 0x128), GLD_U32(cfg, 0x12c),
                   GLD_U32(cfg, 0x00), GLD_U32(cfg, 0x08), GLD_U32(cfg, 0x0c),
                   GLD_U32(cfg, 0x10));
    }
    /* Sonde T&L : un seul octet décide que GLEngine ne transforme plus. Les
       procédures +0x4c/+0x50/+0x54/+0x70 sont posées par pomppc_hook_procs à
       chaque gldInitDispatch/gldUpdateDispatch, donc avant tout dessin. */
    if (r == 0 && e && pomppc_tcl()) {
        unsigned char *cfg = (unsigned char *)e;
        int i;
        GLD_U8(cfg, 0x79) = 1;
        GLD_U8(cfg, 0x7a) = (unsigned char)pomppc_tcl_7a();
        /* cfg+0x78 : posé à 1 par le Rage 128, le GeForce3 et le Radeon, à 0
           par le GLDriver d'Apple. Sonde POMPPC_GL_TCL_78 (défaut 0, inchangé). */
        {
            const char *s78 = getenv("POMPPC_GL_TCL_78");
            if (s78 && *s78)
                GLD_U8(cfg, 0x78) = (unsigned char)atoi(s78);
        }
        /* Six identifiants de format de sommet (GLEngine 0xda5a8-0xda65c) :
           Apple laisse 0, le GeForce3 publie {3,2,1,6,5,4}. Sans eux, GLEngine
           n'a rien à passer à gldAllocVertexBuffer. */
        for (i = 0; i < 6; i++)
            GLD_U16(cfg, 0x7c + i * 2) = (unsigned short)(i + 1);
        /* cfg+0x11c -> gctx+0x48d0 (_gleUpdateDispatchCodeChange 0xc8d40) : c'est
           le DESCRIPTEUR DE SORTIE DE SOMMET, garde du chemin Begin/EndPrimitiveBuffer
           (_gleUpdatePrimitiveData 0x6780, _gleSelectVertexSubmitFunc 0x9e1c).
           Disposition lue en 0x1d360 : octet 0 = nombre d'entrées n, octet 2 = pas
           en MOTS de 4 octets, puis n entrées de 16 bits (2 par mot). GeForce3 y
           laisse 0 ; on n'en pose un que sur demande explicite, c'est une sonde. */
        {
            const char *d = getenv("POMPPC_GL_TCL_DESC");
            if (d && *d) {
                static unsigned long desc[16];
                static unsigned short ent[24];
                int nent = 0, pasm = 4;
                const char *q = d;
                pasm = (int)strtol(q, (char **)&q, 0);
                /* Chaque entrée s'écrit soit en brut (0x0300), soit
                   « code:décalage[:composantes] » — l'entrée de 16 bits vaut
                   (code << 10) | ((composantes − 1) << 8) | décalageEnMots
                   (GLEngine 0x39e14-0x3a03c et 0xb4c00-0xb4c24). */
                while (*q == ',' && nent < 24) {
                    long v0 = strtol(q + 1, (char **)&q, 0);
                    if (*q == ':') {
                        long off = strtol(q + 1, (char **)&q, 0);
                        long nc = 4;
                        if (*q == ':')
                            nc = strtol(q + 1, (char **)&q, 0);
                        if (nc < 1 || nc > 4)
                            nc = 4;
                        v0 = ((v0 & 0x3f) << 10) | (((nc - 1) & 3) << 8) | (off & 0xff);
                    }
                    ent[nent++] = (unsigned short)v0;
                }
                if (pasm <= 0 || pasm > 64)
                    pasm = 4;
                memset(desc, 0, sizeof desc);
                ((unsigned char *)desc)[0] = (unsigned char)nent;
                ((unsigned char *)desc)[2] = (unsigned char)pasm;
                {
                    int i2;
                    for (i2 = 0; i2 < nent; i2++)
                        ((unsigned short *)desc)[2 + i2] = ent[i2];
                }
                GLD_U32(cfg, 0x11c) = (unsigned long)desc;
                pomppc_log("  POMPPC_GL_TCL_DESC : %d entrée(s), pas %d mots, mot0=%08lx\n",
                           nent, pasm, desc[0]);
                {
                    int i3;
                    for (i3 = 0; i3 < nent; i3++)
                        pomppc_log("    entrée %d = %04x (code %d, %d composante(s),"
                                   " mot %d)\n", i3, ent[i3], (ent[i3] >> 10) & 0x3f,
                                   ((ent[i3] >> 8) & 3) + 1, ent[i3] & 0xff);
                }
            }
        }
        pomppc_log("  POMPPC_GL_TCL : cfg+0x79 = 1, cfg+0x7a = %u, cfg+0x7c..0x87 = 1..6,"
                   " cfg+0x94 = %08lx, cfg+0x11c = %08lx, cfg+0x120 = %08lx\n",
                   GLD_U8(cfg, 0x7a), GLD_U32(cfg, 0x94), GLD_U32(cfg, 0x11c),
                   GLD_U32(cfg, 0x120));
    }
    if (r == 0 && a && *(long *)a)
        pomppc_context_created((void *)*(long *)a);
    return r;
}

long gldDestroyContext(long a, long b, long c, long d, long e, long f, long g, long h)
{
    pomppc_log("gldDestroyContext(%08lx)\n", a);
    pomppc_context_destroyed((void *)a);
    return FWD8(GLD_DestroyContext);
}

long gldAttachDrawable(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r;
    pomppc_before_buffers_change((void *)a);
    r = FWD8(GLD_AttachDrawable);
    pomppc_log("gldAttachDrawable(%08lx %08lx %08lx %08lx) -> %ld\n", a, b, c, d, r);
    pomppc_drawable_attached((void *)a, b, r);
    return r;
}

long gldInitDispatch(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r = FWD8(GLD_InitDispatch);
    pomppc_log("gldInitDispatch(%08lx %08lx %08lx) -> %ld\n", a, b, c, r);
    if (pomppc_tcl()) {
        pomppc_tcl_check((void *)a, "initdispatch");
        r = pomppc_tcl_dispatch_ret(r, "initdispatch");
    }
    pomppc_hook_procs((void *)a, (void **)b);
    return r;
}

long gldUpdateDispatch(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r;
    /* 0x80 : le tampon de dessin change (gldSetDrawBufferPtrs chez Apple) */
    int buf = c && (*(unsigned long *)c & 0x80);
    pomppc_unhook_procs((void *)a, (void **)b);
    r = FWD8(GLD_UpdateDispatch);
    pomppc_log("gldUpdateDispatch(%08lx %08lx changes %08lx) -> %ld\n", a, b,
               c ? *(unsigned long *)c : 0, r);
    if (buf)
        pomppc_after_draw_buffer_change((void *)a);
    if (pomppc_tcl()) {
        pomppc_tcl_check((void *)a, "updatedispatch");
        r = pomppc_tcl_dispatch_ret(r, "updatedispatch");
    }
    pomppc_hook_procs((void *)a, (void **)b);
    return r;
}

long gldFlush(long a, long b, long c, long d, long e, long f, long g, long h)
{
    pomppc_log("gldFlush(%08lx)\n", a);
    pomppc_sync_to_sw((void *)a);
    return FWD8(GLD_Flush);
}

long gldFinish(long a, long b, long c, long d, long e, long f, long g, long h)
{
    pomppc_log("gldFinish(%08lx)\n", a);
    pomppc_sync_to_sw((void *)a);
    return FWD8(GLD_Finish);
}

/* ───── textures : le plugin suit leurs modifications pour les recopier sur l'hôte ───── */

long gldCreateTexture(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r = FWD8(GLD_CreateTexture);
    if (r == 0 && b && *(long *)b)
        pomppc_texture_created((void *)*(long *)b);
    return r;
}

long gldDeleteTexture(long a, long b, long c, long d, long e, long f, long g, long h)
{
    pomppc_texture_deleted((void *)b);
    return FWD8(GLD_DeleteTexture);
}

long gldCreateTextureLevel(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r = FWD8(GLD_CreateTextureLevel);
    pomppc_texture_changed((void *)b, 1);
    return r;
}

long gldModifyTextureLevel(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r = FWD8(GLD_ModifyTextureLevel);
    pomppc_texture_changed((void *)b, 1);
    return r;
}

long gldDeleteTextureLevel(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r = FWD8(GLD_DeleteTextureLevel);
    pomppc_texture_changed((void *)b, 1);
    return r;
}

/* (ctx, texture, quoi, valeur) : 0x80 = un paramètre (filtre, répétition),
   relu à chaque dessin ; tout le reste peut toucher aux niveaux. */
long gldModifyTexture(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r = FWD8(GLD_ModifyTexture);
    pomppc_texture_changed((void *)b, c != 0x80);
    return r;
}

long gldGetInteger(long a, long b, long c, long d, long e, long f, long g, long h)
{
    long r = FWD8(GLD_GetInteger);
    pomppc_log("gldGetInteger(%08lx %08lx %08lx) -> %ld\n", a, b, c, r);
    return r;
}

const char *gldGetString(long a, long name, long c, long d, long e, long f, long g, long h)
{
    long b = name;
    const char *s = (const char *)FWD8(GLD_GetString);
    const char *o = pomppc_override_string(name, s);
    pomppc_log("gldGetString(%08lx %lx) -> %s\n", a, name, o ? o : "(nul)");
    return o;
}
