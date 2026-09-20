/*
 * accelprobe.c — tâche 4.2 : ce que le système voit de l'accélérateur publié
 * par POMPPCGPU.kext (docs/re/accelerateur-iokit.md).
 *
 *   accelprobe            framebuffers → accélérateur (IOAccelFindAccelerator,
 *                         comme CGL et le WindowServer), IOGLBundleName,
 *                         ouverture refusée, renderers vus par CGL, et l'état
 *                         de Quartz Extreme (en session graphique seulement)
 *
 * Code de sortie : 0 si chaque framebuffer mène à un accélérateur de classe
 * IOAccelerator portant IOGLBundleName, 1 sinon.
 *
 *   gcc-4.0 -arch ppc -isysroot /Developer/SDKs/MacOSX10.4u.sdk -o accelprobe \
 *       accelprobe.c -framework IOKit -framework CoreFoundation \
 *       -framework OpenGL -framework ApplicationServices
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsInterface.h>
#include <ApplicationServices/ApplicationServices.h>
#include <OpenGL/OpenGL.h>

static void cfstr(CFTypeRef v, char *buf, int len)
{
    buf[0] = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef) v, buf, len, kCFStringEncodingUTF8);
}

static int probe_framebuffers(void)
{
    mach_port_t master;
    io_iterator_t it;
    io_service_t fb;
    int n = 0, ok = 0;

    if (IOMasterPort(MACH_PORT_NULL, &master) != KERN_SUCCESS ||
        IOServiceGetMatchingServices(master, IOServiceMatching("IOFramebuffer"), &it)
            != KERN_SUCCESS) {
        printf("IOFramebuffer : énumération impossible\n");
        return 0;
    }
    while ((fb = IOIteratorNext(it))) {
        io_name_t cls, acls;
        io_service_t accel = 0;
        UInt32 index = 0;
        IOReturn kr;
        char types[512], bundle[128];
        CFTypeRef v;

        IOObjectGetClass(fb, cls);
        v = IORegistryEntryCreateCFProperty(fb, CFSTR(kIOAccelTypesKey),
                                            kCFAllocatorDefault, 0);
        cfstr(v, types, sizeof(types));
        if (v) CFRelease(v);
        printf("framebuffer %d : %s\n  IOAccelTypes = %s\n", n, cls,
               types[0] ? types : "(absent)");
        kr = IOAccelFindAccelerator(fb, &accel, &index);
        if (kr != kIOReturnSuccess || !accel) {
            printf("  IOAccelFindAccelerator -> 0x%x, pas d'accélérateur\n", kr);
        } else {
            io_connect_t c = 0;
            IOObjectGetClass(accel, acls);
            v = IORegistryEntryCreateCFProperty(accel, CFSTR("IOGLBundleName"),
                                                kCFAllocatorDefault, 0);
            cfstr(v, bundle, sizeof(bundle));
            if (v) CFRelease(v);
            printf("  IOAccelFindAccelerator -> accélérateur %s, index %lu, IOGLBundleName = %s,"
                   " IOAccelerator : %s\n", acls, (unsigned long) index,
                   bundle[0] ? bundle : "(absent)",
                   IOObjectConformsTo(accel, "IOAccelerator") ? "oui" : "NON");
            /* type 0 = kIOAccelSurfaceClientType : ce que CGL (pbuffers) et le
               WindowServer ouvriraient ; le nub doit refuser tant que 4.4
               n'est pas faite. */
            kr = IOServiceOpen(accel, mach_task_self(), 0, &c);
            printf("  IOServiceOpen(accélérateur, type 0) -> 0x%x%s\n", kr,
                   kr == kIOReturnUnsupported ? " (refus attendu)" : "");
            if (kr == KERN_SUCCESS)
                IOServiceClose(c);
            if (bundle[0] && IOObjectConformsTo(accel, "IOAccelerator"))
                ok++;
            IOObjectRelease(accel);
        }
        IOObjectRelease(fb);
        n++;
    }
    IOObjectRelease(it);
    printf("framebuffers : %d, reliés à un accélérateur GL : %d%s\n", n, ok,
           n ? "" : " (aucun framebuffer : normal en single-user, IONDRVSupport"
                    " n'est chargé que par kextd)");
    return n > 0 && ok == n;
}

static void probe_renderers(void)
{
    CGLRendererInfoObj info;
    long nr = 0, i;
    CGLError e = CGLQueryRendererInfo(0xffffffff, &info, &nr);

    if (e) {
        printf("CGLQueryRendererInfo -> %d (%s)\n", e, CGLErrorString(e));
        return;
    }
    for (i = 0; i < nr; i++) {
        long id = 0, acc = 0, mask = 0, vram = 0, fs = 0, win = 0, off = 0;
        CGLPixelFormatAttribute attrs[12];
        CGLPixelFormatObj pix = NULL;
        long npix = 0;
        int k = 0;
        CGLError pe;

        CGLDescribeRenderer(info, i, kCGLRPRendererID, &id);
        CGLDescribeRenderer(info, i, kCGLRPAccelerated, &acc);
        CGLDescribeRenderer(info, i, kCGLRPFullScreen, &fs);
        CGLDescribeRenderer(info, i, kCGLRPWindow, &win);
        CGLDescribeRenderer(info, i, kCGLRPOffScreen, &off);
        CGLDescribeRenderer(info, i, kCGLRPDisplayMask, &mask);
        CGLDescribeRenderer(info, i, kCGLRPVideoMemory, &vram);
        printf("renderer %ld : id 0x%08lx accéléré %ld plein écran %ld fenêtre %ld"
               " hors écran %ld masque d'écrans 0x%lx mémoire %ld\n",
               i, id, acc, fs, win, off, mask, vram);

        /* Liste de SDL 1.2 Quartz (UT2004) : FullScreen + ColorSize + DepthSize
           + DoubleBuffer + ScreenMask. */
        attrs[k++] = kCGLPFAFullScreen;
        attrs[k++] = kCGLPFAColorSize; attrs[k++] = 32;
        attrs[k++] = kCGLPFADepthSize; attrs[k++] = 16;
        attrs[k++] = kCGLPFADoubleBuffer;
        attrs[k++] = kCGLPFADisplayMask; attrs[k++] = (CGLPixelFormatAttribute) mask;
        attrs[k] = 0;
        pe = CGLChoosePixelFormat(attrs, &pix, &npix);
        printf("  pixel format UT (plein écran 32/16) : err=%d (%s) pix=%p n=%ld\n",
               pe, CGLErrorString(pe), (void *) pix, npix);
        if (pix)
            CGLDestroyPixelFormat(pix);
    }
    CGLDestroyRendererInfo(info);
}

int main(int argc, char **argv)
{
    int ok = probe_framebuffers();

    /* Sans WindowServer (single-user), CGL ne compte aucun écran : la liste
       des renderers et l'état de Quartz Extreme n'ont de sens qu'en session. */
    if (!getenv("GLTEST_NOWS")) {
        CGDirectDisplayID d;
        probe_renderers();
        d = CGMainDisplayID();
        printf("écran principal 0x%lx : masque OpenGL 0x%lx, Quartz Extreme %s\n",
               (unsigned long) d, (unsigned long) CGDisplayIDToOpenGLDisplayMask(d),
               CGDisplayUsesOpenGLAcceleration(d) ? "ACTIF" : "inactif");
    }
    printf("accelprobe : %s\n", ok ? "OK" : "ÉCHEC");
    return ok ? 0 : 1;
}
