/* Tiger fullscreen lifecycle and scanout regression. Run in the GUI session.
 * gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk \
 *   fullscreen.c -framework Carbon -framework OpenGL -o fullscreen
 * fullscreen [800 600] ; repeat with POMPPC_GL_DIRECT=0 / POMPPC_GL_ASYNC=0.
 */
#include <Carbon/Carbon.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int w = argc > 1 ? atoi(argv[1]) : 800, h = argc > 2 ? atoi(argv[2]) : 600;
    CGDirectDisplayID display = CGMainDisplayID();
    CFDictionaryRef original = CGDisplayCurrentMode(display), mode;
    boolean_t exact;
    CGLPixelFormatObj pf = NULL;
    CGLContextObj ctx = NULL;
    GLint count;
    CGLError error;
    int pass = 0, round, captured = 0;
    ProcessSerialNumber psn = {0, kCurrentProcess};
    CGLPixelFormatAttribute attrs[] = {kCGLPFAFullScreen, kCGLPFADoubleBuffer,
        kCGLPFAColorSize, 32, kCGLPFADepthSize, 24,
        kCGLPFADisplayMask, 0, 0};
    attrs[7] = (CGLPixelFormatAttribute)CGDisplayIDToOpenGLDisplayMask(display);
    CFRetain(original);
    SetFrontProcess(&psn);
    mode = CGDisplayBestModeForParameters(display, 32, w, h, &exact);
    if (!mode || !exact || CGDisplayCapture(display)) goto done;
    captured = 1;
    if (CGDisplaySwitchToMode(display, mode)) goto done;
    HideMenuBar();
    CGDisplayHideCursor(display);
    error = CGLChoosePixelFormat(attrs, &pf, &count);
    if (error || !pf) { printf("pixel format: %d\n", error); goto done; }
    error = CGLCreateContext(pf, NULL, &ctx);
    if (error) { printf("context: %d\n", error); goto done; }
    CGLSetCurrentContext(ctx);
    printf("renderer: %s; resolution: %lux%lu\n", glGetString(GL_RENDERER),
        (unsigned long)CGDisplayPixelsWide(display), (unsigned long)CGDisplayPixelsHigh(display));
    for (round = 0; round < 3; ++round) {
        unsigned long x, y, stride, pixel;
        unsigned char *base;
        error = CGLSetFullScreen(ctx);
        printf("attach %d: %d\n", round, error);
        if (error) goto done;
        glViewport(0, 0, w, h);
        glClearColor(round == 0, round == 1, round == 2, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        /* Async presentation drains at the following swap/finish. */
        CGLFlushDrawable(ctx); CGLFlushDrawable(ctx); glFinish();
        usleep(50000);
        base = CGDisplayBaseAddress(display); stride = CGDisplayBytesPerRow(display);
        x = w / 2; y = h / 2;
        pixel = *(volatile unsigned long *)(base + y * stride + x * 4) & 0xffffff;
        printf("scanout %d: %06lx expected %06lx\n", round, pixel,
            0xff0000UL >> (round * 8));
        if (pixel != (0xff0000UL >> (round * 8)) || glGetError()) goto done;
        {
            /* DrawPixels falls back to Apple and must still reach scanout. */
            const unsigned char yellow[4] = {255, 255, 0, 255};
            glMatrixMode(GL_PROJECTION); glLoadIdentity();
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glRasterPos2f(0, 0);
            glDrawPixels(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, yellow);
            CGLFlushDrawable(ctx); CGLFlushDrawable(ctx); glFinish();
            pixel = *(volatile unsigned long *)(base + (h - 1 - y) * stride + x * 4) & 0xffffff;
            printf("software pixel %d: %06lx expected ffff00\n", round, pixel);
            if (pixel != 0xffff00 || glGetError()) goto done;
        }
        error = CGLClearDrawable(ctx);
        if (error) { printf("detach: %d\n", error); goto done; }
    }
    pass = 1;
done:
    CGLSetCurrentContext(NULL);
    if (ctx) CGLDestroyContext(ctx);
    if (pf) CGLDestroyPixelFormat(pf);
    if (captured) {
        CGDisplaySwitchToMode(display, original);
        CGDisplayShowCursor(display);
        ShowMenuBar();
        CGDisplayRelease(display);
    }
    CFRelease(original);
    printf("fullscreen %dx%d: %s\n", w, h, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
