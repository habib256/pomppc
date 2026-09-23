/* gltrap.c — mouchard des appels OpenGL d'une application Tiger (programmes ARB,
 * attributs génériques, erreurs GL), par INTERPOSITION dyld comme exitwatch.c.
 * Écrit pour comprendre Colin McRae (23/09/2026) : quels programmes de sommets
 * il charge, comment il lie ses attributs, et d'où sort l'erreur GL sur laquelle
 * il s'arrête (COpenGLFragmentProgram.cpp:210).
 * Journal : $POMPPC_GLTRAP (défaut /tmp/gltrap.txt).
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

static FILE *lf;
static unsigned long n_calls;
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
    static unsigned long n; if (n++ < 200) lg("glBindProgramARB(%s, %u)\n", tname(target), (unsigned)id);
    glBindProgramARB(target, id);
}
static void my_glGenProgramsARB(GLsizei n, GLuint *ids)
{
    glGenProgramsARB(n, ids); lg("glGenProgramsARB(%d) -> %u\n", (int)n, ids ? (unsigned)ids[0] : 0);
}
static void my_glEnable(GLenum cap)
{
    static unsigned long n;
    if (cap == 0x8620 || cap == 0x8804 || cap == 0x8200 || cap == 0x8513 || cap == 0x84f5) { if (n++ < 200) lg("glEnable(%x %s)\n", cap, tname(cap)); }
    glEnable(cap);
}
static void my_glDisable(GLenum cap)
{
    static unsigned long n;
    if (cap == 0x8620 || cap == 0x8804 || cap == 0x8200) { if (n++ < 200) lg("glDisable(%x %s)\n", cap, tname(cap)); }
    glDisable(cap);
}
static void my_glVertexAttribPointerARB(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const GLvoid *p)
{
    static unsigned long n; if (n++ < 60) lg("glVertexAttribPointerARB(%u, %d, %x, %d, %d, %p)\n", (unsigned)i, (int)size, type, (int)norm, (int)stride, p);
    glVertexAttribPointerARB(i, size, type, norm, stride, p);
}
static void my_glEnableVertexAttribArrayARB(GLuint i)
{
    static unsigned long n; if (n++ < 60) lg("glEnableVertexAttribArrayARB(%u)\n", (unsigned)i);
    glEnableVertexAttribArrayARB(i);
}
static void my_glDisableVertexAttribArrayARB(GLuint i)
{
    static unsigned long n; if (n++ < 60) lg("glDisableVertexAttribArrayARB(%u)\n", (unsigned)i);
    glDisableVertexAttribArrayARB(i);
}
static void my_glProgramLocalParameter4fvARB(GLenum t, GLuint i, const GLfloat *v)
{
    static unsigned long n; if (n++ < 30) lg("glProgramLocalParameter4fvARB(%s, %u, %g %g %g %g)\n", tname(t), (unsigned)i, v[0], v[1], v[2], v[3]);
    glProgramLocalParameter4fvARB(t, i, v);
}
static void my_glProgramEnvParameter4fvARB(GLenum t, GLuint i, const GLfloat *v)
{
    static unsigned long n; if (n++ < 30) lg("glProgramEnvParameter4fvARB(%s, %u, %g %g %g %g)\n", tname(t), (unsigned)i, v[0], v[1], v[2], v[3]);
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
    static unsigned long n; if (n++ < 40) lg("glDrawArrays(%x, %d, %d)\n", mode, (int)first, (int)count);
    glDrawArrays(mode, first, count);
}
static void my_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *idx)
{
    static unsigned long n; if (n++ < 40) lg("glDrawElements(%x, %d, %x, %p)\n", mode, (int)count, type, idx);
    glDrawElements(mode, count, type, idx);
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
    { (const void *)my_glTexImage2D, (const void *)glTexImage2D },
    { (const void *)my_glGetString, (const void *)glGetString },
};
