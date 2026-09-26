/* gltrap.c — mouchard des appels OpenGL d'une application Tiger (programmes ARB,
 * attributs génériques, erreurs GL), par INTERPOSITION dyld comme exitwatch.c.
 * Écrit pour comprendre Colin McRae (23/09/2026) : quels programmes de sommets
 * il charge, comment il lie ses attributs, et d'où sort l'erreur GL sur laquelle
 * il s'arrête (COpenGLFragmentProgram.cpp:210).
 * Journal : $POMPPC_GLTRAP (défaut /tmp/gltrap.txt).
 *
 * v16 (nuit du 23/09) : FENÊTRE de trace complète. POMPPC_GLTRAP_WINDOW="s:n"
 * journalise TOUS les appels interposés (tableaux, états client, tampons,
 * verrous, dessins) à partir du s-ième dessin (glDraw*), pendant n dessins :
 * c'est la séquence exacte qui entoure un lot en course — ce que le journal
 * plafonné par fonction ne pouvait pas montrer.
 *
 * 26/09 : POMPPC_GLTRAP_MEM=1 — suivi de la MÉMOIRE des attributs génériques
 * (polygones éclatés de Colin McRae) : tampons (glBufferData/SubData/Map/Unmap,
 * VAR, barrières APPLE) dans la fenêtre, et à chaque glDrawElements le premier
 * sommet référencé relu AVANT et APRÈS l'appel réel ; tout free() d'un bloc qui
 * contient un pointeur d'attribut encore en service est journalisé avec la pile
 * de l'appelant (chaîne des cadres PowerPC).
 *
 *   /usr/bin/gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -O1 -dynamiclib -framework OpenGL -o libgltrap.dylib gltrap.c
 *   DYLD_INSERT_LIBRARIES=libgltrap.dylib POMPPC_GLTRAP=/tmp/g.txt ./jeu
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <malloc/malloc.h>

static FILE *lf;
static unsigned long n_calls;
static unsigned long n_draws, win_start = ~0UL, win_n;
static int win_init;
static void lg(const char *fmt, ...)
{
    va_list ap;
    if (!lf) {
        const char *p = getenv("POMPPC_GLTRAP");
        lf = fopen(p && *p ? p : "/tmp/gltrap.txt", "a");
        if (!lf) return;
        setvbuf(lf, NULL, _IOLBF, 0);
    }
    va_start(ap, fmt); vfprintf(lf, fmt, ap); va_end(ap);
}
/* dans la fenêtre ? (tout se journalise alors) */
static unsigned long big_from;          /* "big<seuil>:n" : fenêtre au premier dessin d'au moins seuil sommets */
static int win(void)
{
    if (!win_init) {
        const char *w = getenv("POMPPC_GLTRAP_WINDOW");
        unsigned long s = 0, n = 0;
        win_init = 1;
        if (w && sscanf(w, "big%lu:%lu", &s, &n) == 2) { big_from = s ? s : 500; win_n = n; }
        else if (w && sscanf(w, "%lu:%lu", &s, &n) == 2) { win_start = s; win_n = n; }
    }
    return n_draws >= win_start && n_draws < win_start + win_n;
}
static void draw_seen(unsigned long count)
{
    win();
    n_draws++;
    if (big_from && win_start == ~0UL && count >= big_from) {
        win_start = n_draws;
        lg("=== FENÊTRE : dessin %lu (%lu sommets)\n", n_draws, count);
    } else if (n_draws == win_start)
        lg("=== FENÊTRE : dessin %lu\n", n_draws);
}
static const char *tname(GLenum t)
{
    switch (t) {
    case 0x8620: return "VERTEX_PROGRAM_ARB";
    case 0x8804: return "FRAGMENT_PROGRAM_ARB";
    case 0x8200: return "TEXT_FRAGMENT_SHADER_ATI";
    default: return "?";
    }
}
static void my_glProgramStringARB(GLenum target, GLenum format, GLsizei len, const GLvoid *s)
{
    static int n;
    lg("glProgramStringARB(%s, fmt %x, len %d)\n", tname(target), format, (int)len);
    if (n++ < 40 && s && len > 0 && len < 20000) {
        const char *t = (const char *)s; int i, k = 0; char line[240];
        for (i = 0; i < len; i++) {
            if (t[i] == '\n' || k >= 236) { line[k] = 0; lg("  | %s\n", line); k = 0; if (t[i] != '\n') line[k++] = t[i]; }
            else if (t[i] != '\r') line[k++] = t[i];
        }
        if (k) { line[k] = 0; lg("  | %s\n", line); }
    }
    glProgramStringARB(target, format, len, s);
    lg("  -> glGetError %x\n", glGetError());
}
static void my_glBindProgramARB(GLenum target, GLuint id)
{
    static unsigned long n; if (n++ < 200 || win()) lg("glBindProgramARB(%s, %u)\n", tname(target), (unsigned)id);
    glBindProgramARB(target, id);
}
static void my_glGenProgramsARB(GLsizei n, GLuint *ids)
{
    glGenProgramsARB(n, ids); lg("glGenProgramsARB(%d) -> %u\n", (int)n, ids ? (unsigned)ids[0] : 0);
}
static void my_glEnable(GLenum cap)
{
    static unsigned long n;
    if (win()) lg("glEnable(%x)\n", cap);
    else if (cap == 0x8620 || cap == 0x8804 || cap == 0x8200 || cap == 0x8513 || cap == 0x84f5) { if (n++ < 200) lg("glEnable(%x %s)\n", cap, tname(cap)); }
    glEnable(cap);
}
static void my_glDisable(GLenum cap)
{
    static unsigned long n;
    if (win()) lg("glDisable(%x)\n", cap);
    else if (cap == 0x8620 || cap == 0x8804 || cap == 0x8200) { if (n++ < 200) lg("glDisable(%x %s)\n", cap, tname(cap)); }
    glDisable(cap);
}
static GLuint cur_abuf, cur_ebuf;
static const unsigned char *at_ptr[16];
static int at_stride[16], at_size[16];
static GLenum at_type[16];
static GLuint at_buf[16];
static void my_glVertexAttribPointerARB(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const GLvoid *p)
{
    static unsigned long n; if (n++ < 60 || win()) lg("glVertexAttribPointerARB(%u, %d, %x, %d, %d, %p)%s\n", (unsigned)i, (int)size, type, (int)norm, (int)stride, p, cur_abuf ? " [VBO]" : "");
    if (i < 16) { at_ptr[i] = (const unsigned char *)p; at_stride[i] = stride; at_size[i] = size; at_type[i] = type; at_buf[i] = cur_abuf; }
    glVertexAttribPointerARB(i, size, type, norm, stride, p);
}
static void my_glEnableVertexAttribArrayARB(GLuint i)
{
    static unsigned long n; if (n++ < 60 || win()) lg("glEnableVertexAttribArrayARB(%u)\n", (unsigned)i);
    glEnableVertexAttribArrayARB(i);
}
static void my_glDisableVertexAttribArrayARB(GLuint i)
{
    static unsigned long n; if (n++ < 60 || win()) lg("glDisableVertexAttribArrayARB(%u)\n", (unsigned)i);
    glDisableVertexAttribArrayARB(i);
}
static void my_glProgramLocalParameter4fvARB(GLenum t, GLuint i, const GLfloat *v)
{
    static unsigned long n; if (n++ < 30 || win()) lg("glProgramLocalParameter4fvARB(%s, %u, %g %g %g %g)\n", tname(t), (unsigned)i, v[0], v[1], v[2], v[3]);
    glProgramLocalParameter4fvARB(t, i, v);
}
static void my_glProgramEnvParameter4fvARB(GLenum t, GLuint i, const GLfloat *v)
{
    static unsigned long n; if (n++ < 30 || win()) lg("glProgramEnvParameter4fvARB(%s, %u, %g %g %g %g)\n", tname(t), (unsigned)i, v[0], v[1], v[2], v[3]);
    glProgramEnvParameter4fvARB(t, i, v);
}
static GLenum my_glGetError(void)
{
    GLenum e = glGetError();
    static unsigned long n;
    n_calls++;
    if (e != GL_NO_ERROR && n++ < 100) lg("glGetError -> %x (appel #%lu)\n", e, n_calls);
    return e;
}
static void my_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    static unsigned long n; draw_seen((unsigned long)count); if (n++ < 40 || win()) lg("glDrawArrays(%x, %d, %d)\n", mode, (int)first, (int)count);
    glDrawArrays(mode, first, count);
}
static void at_dump(const char *when, GLuint i, unsigned long idx);
static int mem(void);
static void my_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *idx)
{
    static unsigned long n;
    int w;
    unsigned long i0 = 0;
    draw_seen((unsigned long)count);
    w = win();
    if (n++ < 40 || w) lg("glDrawElements(%x, %d, %x, %p)%s\n", mode, (int)count, type, idx, cur_ebuf ? " [EBO]" : "");
    if (w && mem() && !cur_ebuf && idx && count > 0)
        i0 = type == GL_UNSIGNED_SHORT ? ((const GLushort *)idx)[0] : type == GL_UNSIGNED_INT ? ((const GLuint *)idx)[0] : ((const GLubyte *)idx)[0];
    if (w && mem()) { at_dump("avant", 0, i0); at_dump("avant", 1, i0); }
    glDrawElements(mode, count, type, idx);
    if (w && mem()) { at_dump("après", 0, i0); at_dump("après", 1, i0); }
}
static void my_glDrawRangeElements(GLenum mode, GLuint a, GLuint b, GLsizei count, GLenum type, const GLvoid *idx)
{
    static unsigned long n; draw_seen((unsigned long)count); if (n++ < 40 || win()) lg("glDrawRangeElements(%x, %u..%u, %d, %x, %p)\n", mode, (unsigned)a, (unsigned)b, (int)count, type, idx);
    glDrawRangeElements(mode, a, b, count, type, idx);
}
static void my_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const GLvoid *px)
{
    static unsigned long n;
    if ((target >= 0x8515 && target <= 0x851a) && n++ < 30) lg("glTexImage2D(cube %x, niveau %d, %dx%d, ifmt %x)\n", target, (int)level, (int)w, (int)h, ifmt);
    glTexImage2D(target, level, ifmt, w, h, border, fmt, type, px);
}
static const GLubyte *my_glGetString(GLenum name)
{
    const GLubyte *s = glGetString(name);
    if (name == GL_EXTENSIONS) lg("glGetString(EXTENSIONS) : %s\n", s ? (const char *)s : "(nul)");
    else lg("glGetString(%x) : %s\n", name, s ? (const char *)s : "(nul)");
    return s;
}
/* ── fenêtre : tableaux, états client, tampons, verrous ── */
static void my_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p)
{ if (win()) lg("glVertexPointer(%d, %x, %d, %p)\n", (int)size, type, (int)stride, p); glVertexPointer(size, type, stride, p); }
static void my_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p)
{ if (win()) lg("glColorPointer(%d, %x, %d, %p)\n", (int)size, type, (int)stride, p); glColorPointer(size, type, stride, p); }
static void my_glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p)
{ if (win()) lg("glNormalPointer(%x, %d, %p)\n", type, (int)stride, p); glNormalPointer(type, stride, p); }
static void my_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p)
{ if (win()) lg("glTexCoordPointer(%d, %x, %d, %p)\n", (int)size, type, (int)stride, p); glTexCoordPointer(size, type, stride, p); }
static void my_glEnableClientState(GLenum a)
{ if (win()) lg("glEnableClientState(%x)\n", a); glEnableClientState(a); }
static void my_glDisableClientState(GLenum a)
{ if (win()) lg("glDisableClientState(%x)\n", a); glDisableClientState(a); }
static void my_glClientActiveTexture(GLenum u)
{ if (win()) lg("glClientActiveTexture(%x)\n", u); glClientActiveTexture(u); }
static void my_glActiveTexture(GLenum u)
{ if (win()) lg("glActiveTexture(%x)\n", u); glActiveTexture(u); }
static void my_glBindTexture(GLenum t, GLuint id)
{ if (win()) lg("glBindTexture(%x, %u)\n", t, (unsigned)id); glBindTexture(t, id); }
static void my_glBindBufferARB(GLenum t, GLuint id)
{ if (win()) lg("glBindBufferARB(%x, %u)\n", t, (unsigned)id); if (t == GL_ARRAY_BUFFER_ARB) cur_abuf = id; else if (t == GL_ELEMENT_ARRAY_BUFFER_ARB) cur_ebuf = id; glBindBufferARB(t, id); }
static void my_glLockArraysEXT(GLint first, GLsizei count)
{ static unsigned long n; if (n++ < 20 || win()) lg("glLockArraysEXT(%d, %d)\n", (int)first, (int)count); glLockArraysEXT(first, count); }
static void my_glUnlockArraysEXT(void)
{ static unsigned long n; if (n++ < 20 || win()) lg("glUnlockArraysEXT()\n"); glUnlockArraysEXT(); }
static void my_glPushClientAttrib(GLbitfield m)
{ if (win()) lg("glPushClientAttrib(%x)\n", (unsigned)m); glPushClientAttrib(m); }
static void my_glPopClientAttrib(void)
{ if (win()) lg("glPopClientAttrib()\n"); glPopClientAttrib(); }
static void my_glBindVertexArrayAPPLE(GLuint id)
{ static unsigned long n; if (n++ < 20 || win()) lg("glBindVertexArrayAPPLE(%u)\n", (unsigned)id); glBindVertexArrayAPPLE(id); }
static void my_glVertexArrayRangeAPPLE(GLsizei len, const GLvoid *p)
{ static unsigned long n; if (n++ < 20 || win()) lg("glVertexArrayRangeAPPLE(%d, %p)\n", (int)len, p); glVertexArrayRangeAPPLE(len, p); }
static void my_glVertexAttrib4fvARB(GLuint i, const GLfloat *v)
{ if (win()) lg("glVertexAttrib4fvARB(%u, %g %g %g %g)\n", (unsigned)i, v[0], v[1], v[2], v[3]); glVertexAttrib4fvARB(i, v); }
static void my_glBegin(GLenum m)
{ draw_seen(0); if (win()) lg("glBegin(%x)\n", m); glBegin(m); }
static void my_glEnd(void)
{ if (win()) lg("glEnd()\n"); glEnd(); }
static void my_glMatrixMode(GLenum m)
{ if (win()) lg("glMatrixMode(%x)\n", m); glMatrixMode(m); }
static void my_glLoadMatrixf(const GLfloat *m)
{ if (win()) lg("glLoadMatrixf(%g %g %g %g ...)\n", m[0], m[5], m[10], m[15]); glLoadMatrixf(m); }
static void my_glDepthMask(GLboolean b)
{ if (win()) lg("glDepthMask(%d)\n", (int)b); glDepthMask(b); }
static void my_glBlendFunc(GLenum a, GLenum b)
{ if (win()) lg("glBlendFunc(%x, %x)\n", a, b); glBlendFunc(a, b); }

/* ── POMPPC_GLTRAP_MEM : mémoire des attributs (26/09) ── */
static int mem_on = -1;
static int mem(void) { if (mem_on < 0) { const char *e = getenv("POMPPC_GLTRAP_MEM"); mem_on = e && *e == '1'; } return mem_on; }
static unsigned long n_frees;
static void bt(void)
{
    unsigned long *fp = (unsigned long *)__builtin_frame_address(0);
    int i;
    lg("   pile :");
    for (i = 0; i < 10 && fp; i++) {
        unsigned long *up = (unsigned long *)fp[0];
        if (!up || (unsigned long)up <= (unsigned long)fp || ((unsigned long)up & 3)) break;
        lg(" %08lx", up[2]);            /* LR sauvé à 8(sp) du cadre de l'appelant */
        fp = up;
    }
    lg("\n");
}
static void at_dump(const char *when, GLuint i, unsigned long idx)
{
    const unsigned char *b;
    unsigned long st;
    if (!at_ptr[i] || at_buf[i]) return;
    st = at_stride[i] ? (unsigned long)at_stride[i] : 4UL * (unsigned long)at_size[i];
    b = at_ptr[i] + idx * st;
    if (at_type[i] == GL_FLOAT)
        lg("   %s attr%u[%lu] @%p : %g %g %g\n", when, (unsigned)i, idx, (const void *)b,
           ((const float *)b)[0], ((const float *)b)[1], ((const float *)b)[2]);
    else
        lg("   %s attr%u[%lu] @%p : %02x%02x%02x%02x\n", when, (unsigned)i, idx, (const void *)b, b[0], b[1], b[2], b[3]);
}
static void my_free(void *p)
{
    if (p && mem()) {
        size_t sz = malloc_size(p);
        int i;
        for (i = 0; i < 16; i++)
            if (at_ptr[i] && !at_buf[i] && (const unsigned char *)p <= at_ptr[i] &&
                at_ptr[i] < (const unsigned char *)p + (sz ? sz : 1)) {
                n_frees++;
                if (win() || n_frees < 20) {
                    lg("free(%p, %lu) contient attr%d (%p) — dessin %lu\n", p, (unsigned long)sz, i, (const void *)at_ptr[i], n_draws);
                    bt();
                }
                break;
            }
    }
    free(p);
}
static void my_glBufferDataARB(GLenum t, GLsizeiptrARB n, const GLvoid *d, GLenum u)
{ if (win()) lg("glBufferDataARB(%x, %ld, %p, %x)\n", t, (long)n, d, u); glBufferDataARB(t, n, d, u); }
static void my_glBufferSubDataARB(GLenum t, GLintptrARB o, GLsizeiptrARB n, const GLvoid *d)
{ if (win()) lg("glBufferSubDataARB(%x, %ld, %ld, %p)\n", t, (long)o, (long)n, d); glBufferSubDataARB(t, o, n, d); }
static GLvoid *my_glMapBufferARB(GLenum t, GLenum a)
{ GLvoid *r = glMapBufferARB(t, a); if (win()) lg("glMapBufferARB(%x, %x) -> %p\n", t, a, r); return r; }
static GLboolean my_glUnmapBufferARB(GLenum t)
{ if (win()) lg("glUnmapBufferARB(%x)\n", t); return glUnmapBufferARB(t); }
static void my_glDeleteBuffersARB(GLsizei n, const GLuint *b)
{ if (win()) lg("glDeleteBuffersARB(%d, %u)\n", (int)n, b ? (unsigned)b[0] : 0); glDeleteBuffersARB(n, b); }
static void my_glFlushVertexArrayRangeAPPLE(GLsizei len, GLvoid *p)
{ static unsigned long n; if (n++ < 20 || win()) lg("glFlushVertexArrayRangeAPPLE(%d, %p)\n", (int)len, p); glFlushVertexArrayRangeAPPLE(len, p); }
static void my_glVertexArrayParameteriAPPLE(GLenum pn, GLint v)
{ static unsigned long n; if (n++ < 20 || win()) lg("glVertexArrayParameteriAPPLE(%x, %x)\n", pn, (unsigned)v); glVertexArrayParameteriAPPLE(pn, v); }
static void my_glSetFenceAPPLE(GLuint f)
{ if (win()) lg("glSetFenceAPPLE(%u)\n", (unsigned)f); glSetFenceAPPLE(f); }
static void my_glFinishFenceAPPLE(GLuint f)
{ if (win()) lg("glFinishFenceAPPLE(%u)\n", (unsigned)f); glFinishFenceAPPLE(f); }
static GLboolean my_glTestFenceAPPLE(GLuint f)
{ GLboolean r = glTestFenceAPPLE(f); if (win()) lg("glTestFenceAPPLE(%u) -> %d\n", (unsigned)f, (int)r); return r; }
static void my_glFinishObjectAPPLE(GLenum o, GLint n)
{ if (win()) lg("glFinishObjectAPPLE(%x, %d)\n", o, (int)n); glFinishObjectAPPLE(o, n); }
static void my_glFlush(void)
{ if (win()) lg("glFlush()\n"); glFlush(); }
static void my_glFinish(void)
{ if (win()) lg("glFinish()\n"); glFinish(); }
static void my_glElementPointerAPPLE(GLenum t, const GLvoid *p)
{ if (win()) lg("glElementPointerAPPLE(%x, %p)\n", t, p); glElementPointerAPPLE(t, p); }
static void my_glDrawRangeElementArrayAPPLE(GLenum m, GLuint a, GLuint b, GLint f, GLsizei c)
{ draw_seen((unsigned long)c); if (win()) lg("glDrawRangeElementArrayAPPLE(%x, %u..%u, %d, %d)\n", m, (unsigned)a, (unsigned)b, (int)f, (int)c); glDrawRangeElementArrayAPPLE(m, a, b, f, c); }
static void my_glDrawElementArrayAPPLE(GLenum m, GLint f, GLsizei c)
{ draw_seen((unsigned long)c); if (win()) lg("glDrawElementArrayAPPLE(%x, %d, %d)\n", m, (int)f, (int)c); glDrawElementArrayAPPLE(m, f, c); }
static void my_glMultiDrawElementsEXT(GLenum m, const GLsizei *c, GLenum t, const GLvoid **i, GLsizei n)
{ draw_seen(c && n ? (unsigned long)c[0] : 0); if (win()) lg("glMultiDrawElementsEXT(%x, n %d)\n", m, (int)n); glMultiDrawElementsEXT(m, c, t, i, n); }

/* 26/09 : requêtes d'état (IndirectX choisit ses chemins d'après elles) */
static void my_glGetIntegerv(GLenum pn, GLint *v)
{
    static unsigned long n;
    glGetIntegerv(pn, v);
    if (n++ < 300 || win()) lg("glGetIntegerv(%x) -> %d %d %d %d\n", pn, (int)v[0], (int)v[1], (int)v[2], (int)v[3]);
}
static void my_glGetFloatv(GLenum pn, GLfloat *v)
{
    static unsigned long n;
    glGetFloatv(pn, v);
    if (n++ < 300 || win()) lg("glGetFloatv(%x) -> %g %g %g %g\n", pn, v[0], v[1], v[2], v[3]);
}
static void my_glGetBooleanv(GLenum pn, GLboolean *v)
{
    static unsigned long n;
    glGetBooleanv(pn, v);
    if (n++ < 300 || win()) lg("glGetBooleanv(%x) -> %d\n", pn, (int)v[0]);
}
static void my_glGenVertexArraysAPPLE(GLsizei n, GLuint *a)
{ glGenVertexArraysAPPLE(n, a); lg("glGenVertexArraysAPPLE(%d) -> %u\n", (int)n, a ? (unsigned)a[0] : 0); }
static void my_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, GLvoid *px)
{ static unsigned long n; if (n++ < 50 || win()) lg("glReadPixels(%d,%d %dx%d %x %x %p)\n", (int)x, (int)y, (int)w, (int)h, f, t, px); glReadPixels(x, y, w, h, f, t, px); }
typedef struct { const void *replacement, *replacee; } interpose_t;
__attribute__((used)) static const interpose_t interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)my_glProgramStringARB, (const void *)glProgramStringARB },
    { (const void *)my_glBindProgramARB, (const void *)glBindProgramARB },
    { (const void *)my_glGenProgramsARB, (const void *)glGenProgramsARB },
    { (const void *)my_glEnable, (const void *)glEnable },
    { (const void *)my_glDisable, (const void *)glDisable },
    { (const void *)my_glVertexAttribPointerARB, (const void *)glVertexAttribPointerARB },
    { (const void *)my_glEnableVertexAttribArrayARB, (const void *)glEnableVertexAttribArrayARB },
    { (const void *)my_glDisableVertexAttribArrayARB, (const void *)glDisableVertexAttribArrayARB },
    { (const void *)my_glProgramLocalParameter4fvARB, (const void *)glProgramLocalParameter4fvARB },
    { (const void *)my_glProgramEnvParameter4fvARB, (const void *)glProgramEnvParameter4fvARB },
    { (const void *)my_glGetError, (const void *)glGetError },
    { (const void *)my_glDrawArrays, (const void *)glDrawArrays },
    { (const void *)my_glDrawElements, (const void *)glDrawElements },
    { (const void *)my_glDrawRangeElements, (const void *)glDrawRangeElements },
    { (const void *)my_glTexImage2D, (const void *)glTexImage2D },
    { (const void *)my_glGetString, (const void *)glGetString },
    { (const void *)my_glVertexPointer, (const void *)glVertexPointer },
    { (const void *)my_glColorPointer, (const void *)glColorPointer },
    { (const void *)my_glNormalPointer, (const void *)glNormalPointer },
    { (const void *)my_glTexCoordPointer, (const void *)glTexCoordPointer },
    { (const void *)my_glEnableClientState, (const void *)glEnableClientState },
    { (const void *)my_glDisableClientState, (const void *)glDisableClientState },
    { (const void *)my_glClientActiveTexture, (const void *)glClientActiveTexture },
    { (const void *)my_glActiveTexture, (const void *)glActiveTexture },
    { (const void *)my_glBindTexture, (const void *)glBindTexture },
    { (const void *)my_glBindBufferARB, (const void *)glBindBufferARB },
    { (const void *)my_glLockArraysEXT, (const void *)glLockArraysEXT },
    { (const void *)my_glUnlockArraysEXT, (const void *)glUnlockArraysEXT },
    { (const void *)my_glPushClientAttrib, (const void *)glPushClientAttrib },
    { (const void *)my_glPopClientAttrib, (const void *)glPopClientAttrib },
    { (const void *)my_glBindVertexArrayAPPLE, (const void *)glBindVertexArrayAPPLE },
    { (const void *)my_glVertexArrayRangeAPPLE, (const void *)glVertexArrayRangeAPPLE },
    { (const void *)my_glVertexAttrib4fvARB, (const void *)glVertexAttrib4fvARB },
    { (const void *)my_glBegin, (const void *)glBegin },
    { (const void *)my_glEnd, (const void *)glEnd },
    { (const void *)my_glMatrixMode, (const void *)glMatrixMode },
    { (const void *)my_glLoadMatrixf, (const void *)glLoadMatrixf },
    { (const void *)my_glDepthMask, (const void *)glDepthMask },
    { (const void *)my_glBlendFunc, (const void *)glBlendFunc },
    { (const void *)my_free, (const void *)free },
    { (const void *)my_glGetIntegerv, (const void *)glGetIntegerv },
    { (const void *)my_glGetFloatv, (const void *)glGetFloatv },
    { (const void *)my_glGetBooleanv, (const void *)glGetBooleanv },
    { (const void *)my_glGenVertexArraysAPPLE, (const void *)glGenVertexArraysAPPLE },
    { (const void *)my_glReadPixels, (const void *)glReadPixels },
    { (const void *)my_glBufferDataARB, (const void *)glBufferDataARB },
    { (const void *)my_glBufferSubDataARB, (const void *)glBufferSubDataARB },
    { (const void *)my_glMapBufferARB, (const void *)glMapBufferARB },
    { (const void *)my_glUnmapBufferARB, (const void *)glUnmapBufferARB },
    { (const void *)my_glDeleteBuffersARB, (const void *)glDeleteBuffersARB },
    { (const void *)my_glFlushVertexArrayRangeAPPLE, (const void *)glFlushVertexArrayRangeAPPLE },
    { (const void *)my_glVertexArrayParameteriAPPLE, (const void *)glVertexArrayParameteriAPPLE },
    { (const void *)my_glSetFenceAPPLE, (const void *)glSetFenceAPPLE },
    { (const void *)my_glFinishFenceAPPLE, (const void *)glFinishFenceAPPLE },
    { (const void *)my_glTestFenceAPPLE, (const void *)glTestFenceAPPLE },
    { (const void *)my_glFinishObjectAPPLE, (const void *)glFinishObjectAPPLE },
    { (const void *)my_glFlush, (const void *)glFlush },
    { (const void *)my_glFinish, (const void *)glFinish },
    { (const void *)my_glElementPointerAPPLE, (const void *)glElementPointerAPPLE },
    { (const void *)my_glDrawRangeElementArrayAPPLE, (const void *)glDrawRangeElementArrayAPPLE },
    { (const void *)my_glDrawElementArrayAPPLE, (const void *)glDrawElementArrayAPPLE },
    { (const void *)my_glMultiDrawElementsEXT, (const void *)glMultiDrawElementsEXT },
};
