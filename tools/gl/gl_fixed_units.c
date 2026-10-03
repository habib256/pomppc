/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * gl_fixed_units.c - unités de texture du pipeline FIXE de l hôte (EGL, Linux) :
 * pour N = 1..7, unité 0 blanche en REPLACE, unité N verte en MODULATE, et le
 * pixel relu dit si l unité N est appliquée. NVIDIA annonce 4 et ignore les
 * suivantes sans erreur GL (docs/backend-gl-unites-fixes.md).
 *   cc -o gl_fixed_units tools/gl/gl_fixed_units.c -lEGL -lGL && ./gl_fixed_units
 */
#include <EGL/egl.h>
#include <GL/gl.h>
#include <stdio.h>
typedef void (*PFN1)(GLenum);
static PFN1 AT, CAT;
typedef void (*PGEN)(GLsizei, GLuint*); typedef void (*PBIND)(GLenum, GLuint);
typedef void (*PFT)(GLenum,GLenum,GLenum,GLuint,GLint); typedef GLenum (*PCHK)(GLenum);
static GLuint tex1(unsigned char r, unsigned char g, unsigned char b) {
    GLuint t; unsigned char px[4] = { r, g, b, 255 };
    glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return t;
}
int main(void) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(d, 0, 0);
    EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_NONE }, n; EGLConfig c;
    eglChooseConfig(d, ca, &c, 1, &n); eglBindAPI(EGL_OPENGL_API);
    EGLint pa[] = { EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d, c, pa);
    EGLContext x = eglCreateContext(d, c, EGL_NO_CONTEXT, NULL);
    eglMakeCurrent(d, s, s, x);
    AT = (PFN1)eglGetProcAddress("glActiveTexture"); CAT = (PFN1)eglGetProcAddress("glClientActiveTexture");
    for (int N = 1; N < 8; N++) {
        GLuint w = tex1(255,255,255), gr = tex1(0,255,0);
        for (int u = 0; u < 8; u++) { AT(GL_TEXTURE0+u); glDisable(GL_TEXTURE_2D); }
        AT(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, w); glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        AT(GL_TEXTURE0+N); glBindTexture(GL_TEXTURE_2D, gr); glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glViewport(0,0,4,4); glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        for (int k = 0; k < 3; k++) {
            float vx[3] = {-1,3,-1}, vy[3] = {-1,-1,3};
            for (int u = 0; u < 8; u++) { void (*MT)(GLenum,float,float) = (void(*)(GLenum,float,float))eglGetProcAddress("glMultiTexCoord2f"); MT(GL_TEXTURE0+u, 0.5f, 0.5f); }
            glVertex2f(vx[k], vy[k]);
        }
        glEnd();
        unsigned char px[4]; glReadPixels(1,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
        printf("unité %d : pixel %3d %3d %3d -> %s  err=0x%x\n", N, px[0], px[1], px[2],
               px[0] < 30 && px[1] > 200 ? "APPLIQUÉE" : "ignorée", glGetError());
        glDeleteTextures(1,&w); glDeleteTextures(1,&gr);
    }
    return 0;
}
