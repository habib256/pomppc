/* Benchmark-only fixed simulation, seed and telemetry; never installed in UT. */
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <mach/mach.h>
extern void SDL_GL_SwapBuffers(void);
static void (*real_tick)(void *, float);
static float *engine_delta;
static int tick_hook;
static unsigned long ticks;
static void fixed_tick(void *engine, float ignored)
{
    *engine_delta = 0.2f;
    ++ticks;
    real_tick(engine, 0.2f);
}
/* This Mac build's CMainLoop ignores GUseFixedTimeStep. Intercept the virtual
 * UGameEngine::Tick call instead, in this process only. Resolve symbols and
 * verify the original function pointer; never patch instructions or disk. */
__attribute__((constructor)) static void install_tick(void)
{
    void **vtable = dlsym(RTLD_DEFAULT, "_ZTV11UGameEngine");
    unsigned i;
    real_tick = dlsym(RTLD_DEFAULT, "_ZN11UGameEngine4TickEf");
    engine_delta = dlsym(RTLD_DEFAULT, "GDeltaTime");
    if (vtable && real_tick && engine_delta) {
        for (i = 0; i < 64; ++i) {
            if (vtable[i] == (void *)real_tick) {
                vm_address_t page = (vm_address_t)&vtable[i] & ~(vm_page_size - 1);
                if (vm_protect(mach_task_self(), page, vm_page_size, 0,
                               VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) == KERN_SUCCESS) {
                    vtable[i] = (void *)fixed_tick;
                    tick_hook = 1;
                    fprintf(stderr, "utflyby: fixed Tick(0.2), verified vtable slot %u\n", i);
                    return;
                }
            }
        }
    }
    fprintf(stderr, "utflyby: cannot install verified fixed Tick hook\n");
    _exit(78);
}
static void fixed_srand(unsigned int ignored) { srand(0); }
/* A signature of 64 spread-out scanout pixels: cheap, and enough to tell one
 * presented frame from another. */
static unsigned long scanout_sig(const unsigned char *src, unsigned long w,
                                 unsigned long h, unsigned long stride)
{
    unsigned long sig = 0, i, x, y;
    for (i = 0; i < 64; ++i) {
        x = (w * ((i * 7) % 63 + 1)) / 65;
        y = (h * ((i * 11) % 63 + 1)) / 65;
        sig = sig * 131 + *(volatile unsigned long *)(src + y * stride + x * 4);
    }
    return sig;
}
/* Presentation is ASYNCHRONOUS: when SwapBuffers returns, the scanout may
 * still hold the previous frame, and the capture would be of that one (bug
 * hunt T12). Drain the pipeline, then wait for the scanout to stop changing —
 * at most a second, so a frozen screen never hangs the run. */
static void wait_presented(const unsigned char *src, unsigned long w,
                           unsigned long h, unsigned long stride)
{
    void (*finish)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "glFinish");
    unsigned long prev, cur;
    int i, same = 0;
    if (finish) finish();
    prev = scanout_sig(src, w, h, stride);
    for (i = 0; i < 50 && same < 2; ++i) {
        usleep(20000);
        cur = scanout_sig(src, w, h, stride);
        same = (cur == prev) ? same + 1 : 0;
        prev = cur;
    }
    fprintf(stderr, "utflyby: scanout settled after %d ms (%s)\n",
            i * 20, same >= 2 ? "stable" : "still changing");
}
/* Quartz screencapture sees WindowServer's stale black buffer while this
 * driver presents directly. Save the actual 32-bit scanout, after measurement. */
static void capture_scanout(const char *path)
{
    unsigned long (*main_display)(void) = dlsym(RTLD_DEFAULT, "CGMainDisplayID");
    unsigned long (*width)(unsigned long) = dlsym(RTLD_DEFAULT, "CGDisplayPixelsWide");
    unsigned long (*height)(unsigned long) = dlsym(RTLD_DEFAULT, "CGDisplayPixelsHigh");
    unsigned long (*pitch)(unsigned long) = dlsym(RTLD_DEFAULT, "CGDisplayBytesPerRow");
    unsigned long (*bpp)(unsigned long) = dlsym(RTLD_DEFAULT, "CGDisplayBitsPerPixel");
    unsigned char *(*base)(unsigned long) = dlsym(RTLD_DEFAULT, "CGDisplayBaseAddress");
    unsigned long display, w, h, stride, x, y;
    unsigned char *src, *row;
    FILE *file;
    if (!main_display || !width || !height || !pitch || !bpp || !base) return;
    display = main_display(); w = width(display); h = height(display);
    stride = pitch(display); src = base(display);
    if (!src || bpp(display) != 32 || !w || !h || w > 16384 || stride < w*4) return;
    wait_presented(src, w, h, stride);
    row = malloc(w*3);
    if (!row) return;
    file = fopen(path, "wb");
    if (file) {
        fprintf(file, "P6\n%lu %lu\n255\n", w, h);
        for (y = 0; y < h; ++y) {
            for (x = 0; x < w; ++x) {
                unsigned long pixel = *(volatile unsigned long *)(src + y*stride + x*4);
                row[x*3] = pixel >> 16;
                row[x*3+1] = pixel >> 8;
                row[x*3+2] = pixel;
            }
            if (fwrite(row, 3, w, file) != w) break;
        }
        fclose(file);
    }
    free(row);
}
static void trace_swap(void)
{
    static FILE *out;
    static int init, *benchmark;
    static float *delta;
    static unsigned long frame;
    SDL_GL_SwapBuffers();
    if (!init) {
        const char *path = getenv("UT_FLYBY_CLOCK");
        init = 1;
        benchmark = dlsym(RTLD_DEFAULT, "GIsBenchmarking");
        delta = dlsym(RTLD_DEFAULT, "GDeltaTime");
        if (path && tick_hook && benchmark && delta) {
            out = fopen(path, "w");
            if (out)
                fprintf(out, "frame,tick,fixed,benchmark,step,delta\n");
        }
    }
    if (out) {
        fprintf(out, "%lu,%lu,%d,%d,%.9g,%.9g\n", ++frame, ticks, tick_hook, *benchmark, 0.2, *delta);
        fflush(out);
        /* After the measured window, capture the same simulation frame in
         * each run. Never include capture in a measured interval. */
        if (frame == 74) {
            const char *path = getenv("UT_FLYBY_CAPTURE");
            if (path) capture_scanout(path);
        }
    }
}
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement; const void *original; } hooks[] = {
    { (const void *)fixed_srand, (const void *)srand },
    { (const void *)trace_swap, (const void *)SDL_GL_SwapBuffers }
};
