/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 VERHILLE Arnaud
 *
 * POMPPCGA.c — plugin GA (« graphics accelerator », CFPlugIn d'IOKit) de
 * POMPPC, pour Quartz Extreme (docs/quartz-extreme.md, étape P0 bis).
 *
 * _CGXMappedDisplayStart (CoreGraphics) ne marque un écran accélérable, et le
 * WindowServer n'essaie le compositeur GL, que si IOPSAllocateBlitEngine
 * (IOKit) charge le plugin GA du framebuffer (IOCFPlugInTypes, posé par le
 * kext en mode QE) et en obtient deux blitters : copie de rectangles dans la
 * VRAM et remplissage de couleur unie. Les cartes d'Apple les font faire au
 * moteur 2D (ATIRadeonGA.plugin, GeForceGA.plugin) ; ici, c'est le processeur
 * de l'invité, sur la VRAM du framebuffer mappée par le client 2D du nub
 * POMPPCAccelerator — exactement ce que le WindowServer ferait sans eux.
 *
 * Contrat : IOKit/graphics/IOGraphicsInterface.h. IOPSAllocateBlitEngine
 * appelle SetDestination (+0x50), puis GetBlitter (+0x54) pour (CopyRects,
 * défaut) et (Rects, couleur unie) — obligatoires — puis (CopyRegion,
 * framebuffer) et (CopyRects, mémoire), facultatifs : refusés ici.
 *
 * Trace : si /var/tmp/pomppc-gltrace existe, /var/tmp/pomppc-ga.log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/graphics/IOGraphicsInterface.h>

/* kext/POMPPCGPU/POMPPCGPU.h */
#define POMPPC_GA_CLIENT_TYPE   0x504F4741UL    /* 'POGA' */
#define POMPPC_GA_MEM_VRAM      0
#define POMPPC_GA_GET_INFO      0

/* 504F4741-0000-0000-0000-504F4D505043 ('POGA' … 'POMPPC') */
#define POMPPC_GA_FACTORY_ID    CFUUIDGetConstantUUIDWithBytes(NULL, \
        0x50, 0x4F, 0x47, 0x41, 0x00, 0x00, 0x00, 0x00, \
        0x00, 0x00, 0x50, 0x4F, 0x4D, 0x50, 0x50, 0x43)

typedef struct {
    IOGraphicsAcceleratorInterface * vt;    /* doit rester le premier champ */
    UInt32         refs;
    io_connect_t   conn;
    unsigned char *vram;
    vm_size_t      vram_size;
    UInt32         width, height, row_bytes, bpp;
    unsigned long  n_copy, n_fill;
} GA;

static IOGraphicsAcceleratorInterface ga_vtbl;

static FILE *ga_log_fp(void)
{
    static int tried;
    static FILE *fp;
    if (!tried) {
        tried = 1;
        if (access("/var/tmp/pomppc-gltrace", R_OK) == 0) {
            fp = fopen("/var/tmp/pomppc-ga.log", "a");
            if (fp)
                setvbuf(fp, 0, _IOLBF, 0);
        }
    }
    return fp;
}

#define GA_LOG(...) do { FILE *f_ = ga_log_fp(); \
        if (f_) { fprintf(f_, "[%d] ", (int)getpid()); fprintf(f_, __VA_ARGS__); } } while (0)

/* ── IUnknown ─────────────────────────────────────────────────────────────── */

static ULONG ga_addref(void *self)
{
    return ++((GA *)self)->refs;
}

static ULONG ga_release(void *self)
{
    GA *g = self;
    if (--g->refs)
        return g->refs;
    if (g->vram)
        IOConnectUnmapMemory(g->conn, POMPPC_GA_MEM_VRAM, mach_task_self(),
                             (vm_address_t)g->vram);
    if (g->conn)
        IOServiceClose(g->conn);
    free(g);
    CFPlugInRemoveInstanceForFactory(POMPPC_GA_FACTORY_ID);
    return 0;
}

static HRESULT ga_query(void *self, REFIID iid, LPVOID *ppv)
{
    CFUUIDRef u = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(u, IUnknownUUID) || CFEqual(u, kIOCFPlugInInterfaceID) ||
        CFEqual(u, kIOGraphicsAcceleratorInterfaceID)) {
        ga_addref(self);
        *ppv = self;
        r = S_OK;
    } else {
        *ppv = 0;
    }
    CFRelease(u);
    return r;
}

/* ── IOCFPlugInInterface ──────────────────────────────────────────────────── */

static IOReturn ga_probe(void *self, CFDictionaryRef props, io_service_t service,
                         SInt32 *order)
{
    if (order)
        *order = 0;
    return kIOReturnSuccess;
}

static IOReturn ga_geometry(GA *g)
{
    kern_return_t kr;
    kr = IOConnectMethodScalarIScalarO(g->conn, POMPPC_GA_GET_INFO, 0, 4,
                                       &g->width, &g->height, &g->row_bytes, &g->bpp);
    if (kr == KERN_SUCCESS && (g->bpp == 8 || g->bpp == 16 || g->bpp == 32) &&
        (unsigned long)g->row_bytes * g->height <= g->vram_size)
        return kIOReturnSuccess;
    GA_LOG("géométrie refusée : kr %x, %lux%lu, %lu octets/ligne, %lu bpp, VRAM %lu\n",
           kr, (unsigned long)g->width, (unsigned long)g->height,
           (unsigned long)g->row_bytes, (unsigned long)g->bpp, (unsigned long)g->vram_size);
    return kIOReturnUnsupported;
}

static IOReturn ga_start(void *self, CFDictionaryRef props, io_service_t service)
{
    GA *g = self;
    io_service_t accel = 0;
    UInt32 index = 0;
    vm_address_t addr = 0;
    vm_size_t size = 0;
    kern_return_t kr;

    kr = IOAccelFindAccelerator(service, &accel, &index);
    if (kr != KERN_SUCCESS || !accel) {
        GA_LOG("Start : pas d'accélérateur (kr %x)\n", kr);
        return kIOReturnNoDevice;
    }
    kr = IOServiceOpen(accel, mach_task_self(), POMPPC_GA_CLIENT_TYPE, &g->conn);
    IOObjectRelease(accel);
    if (kr != KERN_SUCCESS) {
        GA_LOG("Start : ouverture du client 2D refusée (kr %x)\n", kr);
        g->conn = 0;
        return kr;
    }
    kr = IOConnectMapMemory(g->conn, POMPPC_GA_MEM_VRAM, mach_task_self(), &addr, &size,
                            kIOMapAnywhere);
    if (kr != KERN_SUCCESS || !addr) {
        GA_LOG("Start : VRAM non mappée (kr %x)\n", kr);
        return kr ? kr : kIOReturnNoMemory;
    }
    g->vram = (unsigned char *)addr;
    g->vram_size = size;
    kr = ga_geometry(g);
    GA_LOG("Start : VRAM %p (%lu octets), %lux%lu, %lu octets/ligne, %lu bpp -> %x\n",
           g->vram, (unsigned long)size, (unsigned long)g->width, (unsigned long)g->height,
           (unsigned long)g->row_bytes, (unsigned long)g->bpp, kr);
    return kr;
}

static IOReturn ga_stop(void *self)
{
    GA *g = self;
    GA_LOG("Stop : %lu copies, %lu remplissages\n", g->n_copy, g->n_fill);
    return kIOReturnSuccess;
}

/* ── IOGraphicsAcceleratorInterface ───────────────────────────────────────── */

static IOReturn ga_reset(void *self, IOOptionBits options)
{
    return ga_geometry(self);   /* le mode a pu changer */
}

static IOReturn ga_ok(void *self, IOOptionBits options)
{
    return kIOReturnSuccess;
}

static IOReturn ga_sync(void *self, UInt32 options, UInt32 x, UInt32 y, UInt32 w, UInt32 h)
{
    return kIOReturnSuccess;
}

static IOReturn ga_set_destination(void *self, IOOptionBits options, IOBlitSurface *surface)
{
    /* NULL : le framebuffer — la seule destination tenue */
    return surface ? kIOReturnUnsupported : kIOReturnSuccess;
}

/* Rectangle borné à l'écran ; 0 s'il est vide. */
static int ga_clip(GA *g, SInt32 *x, SInt32 *y, SInt32 *w, SInt32 *h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > (SInt32)g->width)  *w = (SInt32)g->width - *x;
    if (*y + *h > (SInt32)g->height) *h = (SInt32)g->height - *y;
    return *w > 0 && *h > 0;
}

static IOReturn ga_copy(void *self, IOOptionBits options, IOBlitType type,
                        IOBlitSourceType sourceType, IOBlitOperation *operation, void *source)
{
    GA *g = self;
    IOBlitCopyRectangles *op = (IOBlitCopyRectangles *)operation;
    UInt32 bypp = g->bpp / 8, i;

    for (i = 0; i < op->count; i++) {
        IOBlitCopyRectangle r = op->rects[i];
        SInt32 dx = r.x - r.sourceX, dy = r.y - r.sourceY;
        SInt32 sx = r.sourceX, sy = r.sourceY, w = r.width, h = r.height, row;
        /* source et destination bornées ensemble */
        if (!ga_clip(g, &sx, &sy, &w, &h))
            continue;
        {
            SInt32 tx = sx + dx, ty = sy + dy;
            if (!ga_clip(g, &tx, &ty, &w, &h))
                continue;
            sx = tx - dx;
            sy = ty - dy;
        }
        if (dy > 0) {               /* vers le bas : de la dernière ligne */
            for (row = h - 1; row >= 0; row--)
                memmove(g->vram + (sy + dy + row) * g->row_bytes + (sx + dx) * bypp,
                        g->vram + (sy + row) * g->row_bytes + sx * bypp, w * bypp);
        } else {
            for (row = 0; row < h; row++)
                memmove(g->vram + (sy + dy + row) * g->row_bytes + (sx + dx) * bypp,
                        g->vram + (sy + row) * g->row_bytes + sx * bypp, w * bypp);
        }
    }
    g->n_copy++;
    return kIOReturnSuccess;
}

static IOReturn ga_fill(void *self, IOOptionBits options, IOBlitType type,
                        IOBlitSourceType sourceType, IOBlitOperation *operation, void *source)
{
    GA *g = self;
    IOBlitRectangles *op = (IOBlitRectangles *)operation;
    UInt32 color = (UInt32)(unsigned long)source, i;

    for (i = 0; i < op->count; i++) {
        SInt32 x = op->rects[i].x, y = op->rects[i].y, w = op->rects[i].width;
        SInt32 h = op->rects[i].height, row, col;
        if (!ga_clip(g, &x, &y, &w, &h))
            continue;
        for (row = 0; row < h; row++) {
            unsigned char *p = g->vram + (y + row) * g->row_bytes;
            if (g->bpp == 32) {
                UInt32 *q = (UInt32 *)p + x;
                for (col = 0; col < w; col++)
                    q[col] = color;
            } else if (g->bpp == 16) {
                UInt16 *q = (UInt16 *)p + x;
                for (col = 0; col < w; col++)
                    q[col] = (UInt16)color;
            } else {
                memset(p + x, (int)(color & 0xff), w);
            }
        }
    }
    g->n_fill++;
    return kIOReturnSuccess;
}

static IOReturn ga_get_blitter(void *self, IOOptionBits options, IOBlitType type,
                               IOBlitSourceType sourceType, IOBlitterPtr *blitter)
{
    IOBlitSourceType src = sourceType & ~(IOBlitSourceType)kIOBlitDestFramebuffer;
    IOReturn r = kIOReturnUnsupported;

    /* l'opération doit être la copie simple, sans clé de couleur */
    if ((type & ~kIOBlitTypeVerbMask) == 0) {
        if (type == kIOBlitTypeCopyRects &&
            (src == kIOBlitSourceDefault || src == kIOBlitSourceFramebuffer)) {
            *blitter = ga_copy;
            r = kIOReturnSuccess;
        } else if (type == kIOBlitTypeRects && src == kIOBlitSourceSolid) {
            *blitter = ga_fill;
            r = kIOReturnSuccess;
        }
    }
    GA_LOG("GetBlitter(options %lx, type %lx, source %lx) -> %x\n", (unsigned long)options,
           (unsigned long)type, (unsigned long)sourceType, r);
    return r;
}

static IOReturn ga_unsupported(void)
{
    return kIOReturnUnsupported;
}

/* ── Fabrique (CFPlugInFactories de l'Info.plist) ─────────────────────────── */

void *POMPPCGAFactory(CFAllocatorRef allocator, CFUUIDRef typeID)
{
    GA *g;
    if (!CFEqual(typeID, kIOGraphicsAcceleratorTypeID))
        return 0;
    if (!ga_vtbl.QueryInterface) {
        ga_vtbl.QueryInterface    = ga_query;
        ga_vtbl.AddRef            = ga_addref;
        ga_vtbl.Release           = ga_release;
        ga_vtbl.version           = 1;
        ga_vtbl.revision          = 0;
        ga_vtbl.Probe             = ga_probe;
        ga_vtbl.Start             = ga_start;
        ga_vtbl.Stop              = ga_stop;
        ga_vtbl.Reset             = ga_reset;
        ga_vtbl.CopyCapabilities  = (void *)ga_unsupported;
        ga_vtbl.GetBlitProc       = (void *)ga_unsupported;
        ga_vtbl.Flush             = ga_ok;
        ga_vtbl.WaitForCompletion = (void *)ga_ok;
        ga_vtbl.Synchronize       = ga_sync;
        ga_vtbl.GetBeamPosition   = (void *)ga_unsupported;
        ga_vtbl.AllocateSurface   = (void *)ga_unsupported;
        ga_vtbl.FreeSurface       = (void *)ga_unsupported;
        ga_vtbl.LockSurface       = (void *)ga_unsupported;
        ga_vtbl.UnlockSurface     = (void *)ga_unsupported;
        ga_vtbl.SwapSurface       = (void *)ga_unsupported;
        ga_vtbl.SetDestination    = ga_set_destination;
        ga_vtbl.GetBlitter        = ga_get_blitter;
        ga_vtbl.WaitComplete      = ga_ok;
    }
    g = calloc(1, sizeof(*g));
    if (!g)
        return 0;
    g->vt = &ga_vtbl;
    g->refs = 1;
    CFPlugInAddInstanceForFactory(POMPPC_GA_FACTORY_ID);
    GA_LOG("instance créée\n");
    return g;
}
