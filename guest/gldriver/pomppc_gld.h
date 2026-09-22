/*
 * pomppc_gld.h — déclarations internes du plugin OpenGL POMPPC.
 * Liste GLD_LIST : ordre de la table glep_gld_names de GLEngine (Tiger 10.4.6),
 * suivie de InitializeLibrary/TerminateLibrary. Doit rester alignée sur
 * tools/gld/gen_tramp.py (qui génère gld_tramp.s).
 */
#ifndef POMPPC_GLD_H
#define POMPPC_GLD_H

#define GLD_LIST \
    X(GetVersion) \
    X(ChoosePixelFormat) \
    X(DestroyPixelFormat) \
    X(GetRendererInfo) \
    X(CreateShared) \
    X(DestroyShared) \
    X(CreateContext) \
    X(DestroyContext) \
    X(ReclaimContext) \
    X(AttachDrawable) \
    X(GetInteger) \
    X(SetInteger) \
    X(InitDispatch) \
    X(UpdateDispatch) \
    X(CreateTexture) \
    X(CreateTextureLevel) \
    X(ModifyTexture) \
    X(ModifyTextureLevel) \
    X(GetTextureLevelInfo) \
    X(GetTextureLevel) \
    X(DeleteTextureLevel) \
    X(DeleteTexture) \
    X(IsTextureResident) \
    X(ReclaimTexture) \
    X(Flush) \
    X(Finish) \
    X(GetString) \
    X(GetError) \
    X(AllocVertexBuffer) \
    X(CompleteVertexBuffer) \
    X(FreeVertexBuffer) \
    X(CreatePipelineProgram) \
    X(ModifyPipelineProgram) \
    X(RelatePipelineProgram) \
    X(GetPipelineProgramInfo) \
    X(DestroyPipelineProgram) \
    X(CreateVertexArray) \
    X(ModifyVertexArray) \
    X(FlushVertexArray) \
    X(DestroyVertexArray) \
    X(ReclaimVertexArray) \
    X(CreateFence) \
    X(DestroyFence) \
    X(TestObject) \
    X(FinishObject) \
    X(CreateQuery) \
    X(DestroyQuery) \
    X(GetQueryInfo) \
    X(CreateBuffer) \
    X(DestroyBuffer) \
    X(FlushBuffer) \
    X(ReclaimBuffer) \
    X(PageoffBuffer) \
    X(GetMemoryPluginData) \
    X(SetMemoryPluginData) \
    X(FinishMemoryPluginData) \
    X(TestMemoryPluginData) \
    X(DestroyMemoryPluginData) \
    X(CreateFramebuffer) \
    X(ReclaimFramebuffer) \
    X(DestroyFramebuffer) \
    X(InitializeLibrary) \
    X(TerminateLibrary)

enum {
#define X(n) GLD_##n,
    GLD_LIST
#undef X
    GLD_COUNT
};

/* Table de procédures remplie par gldInitDispatch (offsets relevés dans le
   GLDriver d'Apple, 10.4.6). */
enum {
    PROC_Accum = 0,  /* +0x00 */
    PROC_Clear = 1,  /* +0x04 */
    PROC_ReadPixels = 2,  /* +0x08 */
    PROC_DrawPixels = 3,  /* +0x0c */
    PROC_CopyPixels = 4,  /* +0x10 */
    PROC_RenderBitmap = 5,  /* +0x14 */
    PROC_RenderPoints = 6,  /* +0x18 */
    PROC_RenderLines = 7,  /* +0x1c */
    PROC_RenderLineStrip = 8,  /* +0x20 */
    PROC_RenderLineLoop = 9,  /* +0x24 */
    PROC_RenderPolygon = 10,  /* +0x28 */
    PROC_RenderTriangles = 11,  /* +0x2c */
    PROC_RenderTriangleFan = 12,  /* +0x30 */
    PROC_RenderTriangleStrip = 13,  /* +0x34 */
    PROC_RenderQuads = 14,  /* +0x38 */
    PROC_RenderQuadStrip = 15,  /* +0x3c */
    PROC_RenderPointsPtr = 16,  /* +0x40 */
    PROC_RenderLinesPtr = 17,  /* +0x44 */
    PROC_RenderPolygonPtr = 18,  /* +0x48 */
    PROC_RenderVertexBuffer = 19,  /* +0x4c */
    PROC_BeginPrimitiveBuffer = 20,  /* +0x50 */
    PROC_EndPrimitiveBuffer = 21,  /* +0x54 */
    PROC_Swap58 = 22,  /* +0x58 */
    PROC_Swap5c = 23,  /* +0x5c */
    PROC_Swap60 = 24,  /* +0x60 */
    PROC_Noop64 = 25,  /* +0x64 */
    PROC_Proc68 = 26,  /* +0x68 */
    PROC_Proc6c = 27,  /* +0x6c */
    PROC_RenderVertexArray = 28,  /* +0x70 */
    PROC_CopyTexSubImage = 29,  /* +0x74 */
    PROC_ModifyTexSubImage = 30,  /* +0x78 */
    PROC_GenerateTexMipmaps = 31,  /* +0x7c */
    PROC_BufferSubData = 32,  /* +0x80 */
    PROC_Proc84 = 33,  /* +0x84 */
    PROC_Proc88 = 34,  /* +0x88 */
    PROC_Proc8c = 35,  /* +0x8c */
    PROC_COUNT = 36
};

#define GLD_VERTEX_SIZE      0x100     /* un sommet GLEngine */
#define POMPPC_PLUGIN_ID     0x7700    /* octet 0xff00 unique parmi les plugins */
#define APPLE_GENERIC_ID     0x0200

#define GLD_U32(p, off)  (*(unsigned long *)((unsigned char *)(p) + (off)))
#define GLD_U16(p, off)  (*(unsigned short *)((unsigned char *)(p) + (off)))
#define GLD_U8(p, off)   (*((unsigned char *)(p) + (off)))
#define GLD_F32(p, off)  (*(float *)((unsigned char *)(p) + (off)))

extern const char *const pomppc_gld_names[GLD_COUNT];
extern void *pomppc_real[GLD_COUNT];

/* pomppc_gld.c */
int  pomppc_tracing(void);
void pomppc_log(const char *fmt, ...);
unsigned long pomppc_dump(const char *tag, const void *p, unsigned long len);
int  pomppc_load_real(void);
long pomppc_call_real(int idx, long a, long b, long c, long d, long e, long f, long g, long h);

/* pomppc_accel.c */
void pomppc_backend_init(void);
void pomppc_backend_fini(void);
/* P8 — handlers de pthread_atfork, posés par pomppc_gld.c. `prepare` prend
   G.mu dans le fil qui appelle fork(), `parent` le rend, et `forget` (côté
   ENFANT) coupe l'accélération, oublie port Mach et tranche mappée, puis rend
   le verrou. Sans cela l'enfant d'un fork() sans exec (Safari/WebKit) écrit
   dans le MÊME flux que son père. */
void pomppc_backend_prepare_fork(void);
void pomppc_backend_parent_fork(void);
void pomppc_backend_forget(void);
void pomppc_patch_renderer_info(unsigned char *info);
int  pomppc_translate_attribs(const long *attribs, long *out, int max);
void pomppc_patch_pixel_list(void *head);
void pomppc_unpatch_pixel_list(void *head);
void pomppc_patch_pixel_format(void *pf);
void pomppc_unpatch_pixel_format(void *pf);
void pomppc_context_created(void *ctx);
void pomppc_context_destroyed(void *ctx);
void pomppc_drawable_attached(void *ctx, long kind, long result);
long pomppc_attach_fullscreen(void *ctx);
void pomppc_before_buffers_change(void *ctx);
void pomppc_after_draw_buffer_change(void *ctx);
void pomppc_texture_created(void *drvtex);
void pomppc_texture_deleted(void *drvtex);
void pomppc_texture_changed(void *drvtex, int levels);
int  pomppc_accel_enabled(void);
void pomppc_hook_procs(void *ctx, void **procs);
void *pomppc_proc_pre(int slot, unsigned long *args);
const char *pomppc_proc_name(int slot);
void pomppc_unhook_procs(void *ctx, void **procs);
void pomppc_sync_to_sw(void *ctx);
const char *pomppc_override_string(long name, const char *apple);
/* Entrée gld que le plugin réalise lui-même (requêtes d'occlusion v8), ou 0 :
   pomppc_pre la rend au trampoline à la place de celle du rendu d'Apple. */
void *pomppc_gld_override(int id);
/* Ajuste le bloc de configuration (5e argument de gldCreateContext) : limites et
   tableau de bits d'extensions, pour n'annoncer QUE ce que la chaîne tient. */
void pomppc_patch_caps(void *cfg);
/* chemin brut (v7) : la géométrie non transformée part sur le GPU de l'hôte */
void *pomppc_geom_proc(int slot);       /* Begin/End, RenderVertexArray/Buffer */
long pomppc_geom_dispatch(void *ctx);   /* bits à ajouter au retour du dispatch */
void pomppc_geom_context(void *ctx, void *cfg);

#endif
