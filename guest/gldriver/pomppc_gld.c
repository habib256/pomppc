/*
 * pomppc_gld.c — plugin OpenGL (« GLD ») de Tiger pour le GPU paravirtuel qgpu.
 *
 * GLEngine (OpenGL.framework) charge ce bundle comme un rendu « GLDriver* »
 * et l'appelle par les 63 points d'entrée gld* (docs/gpu-3d-tiger.md §5).
 * Le plugin est un MANDATAIRE du rendu logiciel d'Apple (GLDriver.bundle),
 * chargé ici en module privé : tout ce que le plugin ne sait pas accélérer
 * reste exact, parce que c'est le code d'Apple qui le fait.
 *
 * Trace (POMPPC_GLTRACE=dossier) : chaque appel gld* et chaque procédure de
 * rastérisation est journalisé, avec des vidages binaires des structures utiles
 * (contexte, sommets, état GL) pour en établir la disposition sur pièces.
 *
 * gldInitDispatch et gldUpdateDispatch portent en plus LE VERROU de la
 * géométrie : le bit 0 de leur valeur de retour décide, à chaque changement
 * d'état, si GLEngine transforme lui-même ou nous remet les sommets bruts
 * (docs/re/verification-tcl.md). Le verdict est rendu par pomppc_geom_dispatch.
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
        void *ours;
        if (!pomppc_load_real())
            return 0;            /* impossible : GLEngine ne nous appelle qu'après Initialize */
        if (pomppc_tracing())
            pomppc_log("gld%s(%08lx %08lx %08lx %08lx %08lx %08lx)\n",
                       id < GLD_COUNT ? pomppc_gld_names[id] : "?",
                       a[0], a[1], a[2], a[3], a[4], a[5]);
        /* Quelques entrées sont RÉALISÉES par le plugin et non transmises : le
           rendu d'Apple n'y répond que par un bouchon (requêtes d'occlusion).
           Le trampoline saute dans ce qu'on rend ici, avec les arguments
           d'origine — pas besoin d'une fonction C exportée, donc pas besoin de
           toucher à gld_tramp.s, qui est engendré. */
        ours = pomppc_gld_override(id);
        return ours ? ours : pomppc_real[id];
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
        if (getenv("POMPPC_GL_T3DDUMP")) {
            /* Sonde des cibles de texture (docs/re/textures-3d.md) : état GL
               entier, table des textures liées de l'unité 0 (5 cibles), et
               chaque objet texture lié avec ses paramètres et ses données. */
            const unsigned char *units = (const unsigned char *)GLD_U32(ctx, 0x10);
            int k, l;
            pomppc_dump("swap-glstate", (void *)GLD_U32(ctx, 0x0c), 0x5400);
            for (k = 0; units && k < 5; k++) {
                unsigned char *dt = (unsigned char *)GLD_U32(units, k * 4);
                char tag[32];
                pomppc_log("  unité 0, emplacement %d : %08lx\n", k, (unsigned long)dt);
                if (!dt)
                    continue;
                sprintf(tag, "swap-dt%d", k);
                pomppc_dump(tag, dt, 0x600);
                if (GLD_U32(dt, 0))
                    { sprintf(tag, "swap-prm%d", k); pomppc_dump(tag, (void *)GLD_U32(dt, 0), 0x1000); }
                for (l = 0; l < 2; l++) {
                    const unsigned char *lv = dt + 0x78 + l * 0x74;
                    if (GLD_U32(lv, 0x10))
                        { sprintf(tag, "swap-dt%d-niv%d", k, l);
                          pomppc_dump(tag, (void *)GLD_U32(lv, 0x10), 0x100); }
                }
            }
        }
        break;
    }
    default:
        break;
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
    /* Le bloc de configuration est rempli par le GLDriver d'Apple pendant
       l'appel ci-dessus : c'est APRÈS qu'on l'ajuste à ce que la chaîne tient
       (docs/re/version-extensions.md). */
    if (r == 0 && e)
        pomppc_patch_caps((void *)e);
    if (r == 0 && a && *(long *)a) {
        pomppc_context_created((void *)*(long *)a);
        /* 5ᵉ argument = bloc de configuration. Le chemin brut y pose cfg+0x79
           (valeur initiale du verrou T&L) et y publiera son descripteur de
           sortie de sommet en cfg+0x11c (docs/re/verification-tcl.md §8). */
        pomppc_geom_context((void *)*(long *)a, (void *)e);
    }
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
    pomppc_hook_procs((void *)a, (void **)b);
    /* LE VERROU de la géométrie : GLEngine relit le bit 0 de cette valeur à
       chaque changement d'état (_gleUpdateDispatchCodeChange, GLEngine 0xc8d20).
       Hors domaine, on rend le retour d'Apple tel quel et il reprend tout le
       travail — c'est le repli, vérifié exact. */
    {
        long bits = pomppc_geom_dispatch((void *)a);
        if (bits)
            pomppc_log("  géométrie brute : +%ld\n", bits);
        return r | bits;
    }
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
    pomppc_hook_procs((void *)a, (void **)b);
    {
        long bits = pomppc_geom_dispatch((void *)a);
        if (bits)
            pomppc_log("  géométrie brute : +%ld\n", bits);
        return r | bits;
    }
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
    long r;
    if (getenv("POMPPC_GL_T3DDUMP")) {
        /* Sonde (docs/re/textures-3d.md) : arguments bruts, et 0x80 octets
           derrière chacun qui ressemble à un pointeur. */
        long v[8] = { a, b, c, d, e, f, g, h };
        int k;
        char tag[32];
        pomppc_log("gldCreateTextureLevel(%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx)\n",
                   a, b, c, d, e, f, g, h);
        for (k = 0; k < 8; k++)
            if ((unsigned long)v[k] > 0x10000UL && (unsigned long)v[k] < 0x80000000UL &&
                !((unsigned long)v[k] & 3)) {
                sprintf(tag, "ctl-arg%d", k);
                pomppc_dump(tag, (void *)v[k], 0x80);
            }
    }
    r = FWD8(GLD_CreateTextureLevel);
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
