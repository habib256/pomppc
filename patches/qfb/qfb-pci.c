/*
 * "Qemu FrameBuffer" (QFB) — PCI variant, for PowerPC Macintosh machines.
 *
 * Port of the NuBus "qfb1" paravirtualised framebuffer to the PCI bus, so that
 * it can be used on the mac99 (New World Power Macintosh) machine.
 *
 * The register interface is bit-for-bit the one defined by:
 *                 Copyright (c) 2022 Solra Bizna
 *   https://github.com/SolraBizna/mac_qfb_driver  (declaration ROM + driver)
 *   https://github.com/SolraBizna/qemu            (hw/display/mac_qfb.c)
 *
 * large parts from QEMU Motorola 680x0 Macintosh Video Card Emulation.
 *                 Copyright (c) 2012-2018 Laurent Vivier
 * some parts from QEMU G364 framebuffer Emulator.
 *                 Copyright (c) 2007-2011 Herve Poussineau
 *
 * PCI port for POMPPC, 2026.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "qom/object.h"

#define QFB_VRAM_SIZE       (32 * MiB) /* enough for 3840x2160 at 32-bit */
#define QFB_CTRL_BAR_SIZE   4096       /* one page, registers live in the first 0x40 */

#define QFB_CTRL_TOPADDR    0x40
#define QFB_NUM_REGS        (QFB_CTRL_TOPADDR / sizeof(uint32_t))

#define QFB_VERSION         0x0  /* reads 'qfb1', writing this resets the QFB */
#define QFB_MODE_WIDTH      0x4  /* width in pixels */
#define QFB_MODE_HEIGHT     0x8  /* height in pixels */
#define QFB_MODE_DEPTH      0xC  /* depth (one of 1, 2, 4, 8, 16, 24) */
#define QFB_MODE_BASE       0x10 /* offset within VRAM, 4-byte-aligned */
#define QFB_MODE_STRIDE     0x14 /* not writable */
/* gap */
#define QFB_PAL_INDEX       0x1C /* index into palette */
#define QFB_PAL_COLOR       0x20 /* xxRRGGBB palette entry */
#define QFB_LUT_INDEX       0x24 /* index into gamma LUTs */
#define QFB_LUT_COLOR       0x28 /* xxRRGGBB gamma LUT entry */
#define QFB_IRQ_MASK        0x2C /* irq mask */
#define QFB_IRQ             0x30 /* read irq status, write irq ack */
#define QFB_CUSTOM_WIDTH    0x34 /* r/o: user-specified width, if any */
#define QFB_CUSTOM_HEIGHT   0x38 /* r/o: user-specified height, if any */
#define QFB_CUSTOM_DEPTH    0x3C /* user-specified depth, write = stderr */

#define QFB_IRQ_VBL         0x1

/* QuickDraw gets very cranky if your rowbytes is >= 16382 */
#define QFB_MAX_WIDTH       3840
#define QFB_MAX_HEIGHT      2160

/* Vertical Blank period (60.00Hz) */
#define QFB_IRQ_VBL_PERIOD_NS 16666667

/* Local (non-upstream) PCI id, under the QEMU vendor id. */
#define PCI_DEVICE_ID_QEMU_QFB 0x0fb1

typedef struct QfbState {
    MemoryRegion mem_vram;
    MemoryRegion mem_ctrl;
    QemuConsole *con;

    uint8_t *vram;
    uint32_t palette_current;
    uint8_t palette_red[256];
    uint8_t palette_green[256];
    uint8_t palette_blue[256];
    uint32_t gamma_current;
    uint8_t gamma_red[256];
    uint8_t gamma_green[256];
    uint8_t gamma_blue[256];
    uint32_t width, height; /* in pixels */
    uint32_t stride;        /* in bytes */
    uint8_t depth;

    uint32_t regs[QFB_NUM_REGS];

    /* Palette/gamma changed since the last redraw: the guest rewrites the
       whole CLUT one register at a time (256 entries), and each write used to
       mark all 32 MiB dirty. Coalesced into one invalidation per frame. */
    bool pal_dirty;

    QEMUTimer *vbl_timer;
    qemu_irq irq;
} QfbState;

#define TYPE_QFB_PCI "qfb-pci"
OBJECT_DECLARE_SIMPLE_TYPE(QfbPCIState, QFB_PCI)

struct QfbPCIState {
    PCIDevice parent_obj;

    QfbState qfb;
};

typedef void qfb_draw_line_func(QfbState *s, uint8_t *d, uint32_t addr,
                                int width);

static inline uint8_t qfb_read_byte(QfbState *s, uint32_t addr)
{
    return s->vram[addr % QFB_VRAM_SIZE];
}

/* 1-bit color */
static void qfb_draw_line1(QfbState *s, uint8_t *d, uint32_t addr, int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        int bit = x & 7;
        int idx = (qfb_read_byte(s, addr) >> (7 - bit)) & 1;
        r = s->gamma_red[s->palette_red[idx]];
        g = s->gamma_green[s->palette_green[idx]];
        b = s->gamma_blue[s->palette_blue[idx]];
        addr += (bit == 7);

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}

/* 2-bit color */
static void qfb_draw_line2(QfbState *s, uint8_t *d, uint32_t addr, int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        int bit = x & 3;
        int idx = (qfb_read_byte(s, addr) >> ((3 - bit) << 1)) & 3;
        r = s->gamma_red[s->palette_red[idx]];
        g = s->gamma_green[s->palette_green[idx]];
        b = s->gamma_blue[s->palette_blue[idx]];
        addr += (bit == 3);

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}

/* 4-bit color */
static void qfb_draw_line4(QfbState *s, uint8_t *d, uint32_t addr, int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        int bit = x & 1;
        int idx = (qfb_read_byte(s, addr) >> ((1 - bit) << 2)) & 15;
        r = s->gamma_red[s->palette_red[idx]];
        g = s->gamma_green[s->palette_green[idx]];
        b = s->gamma_blue[s->palette_blue[idx]];
        addr += (bit == 1);

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}

/* 8-bit color */
static void qfb_draw_line8(QfbState *s, uint8_t *d, uint32_t addr, int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        int idx = qfb_read_byte(s, addr);
        r = s->gamma_red[s->palette_red[idx]];
        g = s->gamma_green[s->palette_green[idx]];
        b = s->gamma_blue[s->palette_blue[idx]];
        addr++;

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}

/* 16-bit color (Macintosh "Thousands", xRRRRRGGGGGBBBBB, big endian) */
static void qfb_draw_line16(QfbState *s, uint8_t *d, uint32_t addr, int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        uint16_t pixel;
        pixel = (qfb_read_byte(s, addr) << 8) | qfb_read_byte(s, addr + 1);
        r = ((pixel >> 10) & 0x1f) << 3;
        r = r | (r >> 5);
        g = ((pixel >> 5) & 0x1f) << 3;
        g = g | (g >> 5);
        b = (pixel & 0x1f) << 3;
        b = b | (b >> 5);
        addr += 2;

        *(uint32_t *)d = rgb_to_pixel32(s->gamma_red[r], s->gamma_green[g],
                                        s->gamma_blue[b]);
        d += 4;
    }
}

/* 24-bit color, stored 32 bits per pixel (xRGB, big endian) */
static void qfb_draw_line24(QfbState *s, uint8_t *d, uint32_t addr, int width)
{
    uint8_t r, g, b;
    int x;

    for (x = 0; x < width; x++) {
        r = s->gamma_red[qfb_read_byte(s, addr + 1)];
        g = s->gamma_green[qfb_read_byte(s, addr + 2)];
        b = s->gamma_blue[qfb_read_byte(s, addr + 3)];
        addr += 4;

        *(uint32_t *)d = rgb_to_pixel32(r, g, b);
        d += 4;
    }
}

enum {
    QFB_DRAW_LINE1,
    QFB_DRAW_LINE2,
    QFB_DRAW_LINE4,
    QFB_DRAW_LINE8,
    QFB_DRAW_LINE16,
    QFB_DRAW_LINE24,
    QFB_DRAW_LINE_NB,
};

static qfb_draw_line_func * const qfb_draw_line_table[QFB_DRAW_LINE_NB] = {
    qfb_draw_line1,
    qfb_draw_line2,
    qfb_draw_line4,
    qfb_draw_line8,
    qfb_draw_line16,
    qfb_draw_line24,
};

/*
 * Number of scanlines that actually fit in VRAM below the current MODE_BASE.
 *
 * QFB_MODE_BASE accepts any offset inside the 32 MiB while width/height are
 * clamped independently, so a guest can program a scanout that runs past the
 * end of VRAM. Reads are wrapped by qfb_read_byte(), but the per-line dirty
 * query in qfb_draw_graphic() is not: QEMU asserts
 * (start + length <= snap->end, system/physmem.c) and the *host* aborts.
 * Everything that walks the scanout must therefore go through this bound.
 */
static uint32_t qfb_visible_lines(QfbState *s)
{
    uint32_t base = s->regs[QFB_MODE_BASE >> 2];
    uint32_t lines;

    if (s->stride == 0 || base >= QFB_VRAM_SIZE) {
        return 0;
    }
    lines = (QFB_VRAM_SIZE - base) / s->stride;

    return lines < s->height ? lines : s->height;
}

static void qfb_invalidate_display(void *opaque)
{
    QfbState *s = opaque;
    uint32_t lines = qfb_visible_lines(s);

    /* Only the scanout window is ever displayed. Marking the whole 32 MiB
       dirty burnt 8192 page bits per call for nothing: a 1024x768x8 mode is
       768 KiB, i.e. 2% of the VRAM. */
    if (lines) {
        memory_region_set_dirty(&s->mem_vram, s->regs[QFB_MODE_BASE >> 2],
                                (uint64_t)lines * s->stride);
    }
}

static void qfb_draw_graphic(QfbState *s)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    DirtyBitmapSnapshot *snap = NULL;
    ram_addr_t page;
    uint32_t v = 0;
    uint32_t base, lines;
    int y, ymin;
    int qfb_stride = s->stride;
    qfb_draw_line_func *qfb_draw_line;

    switch (s->depth) {
    case 1:
        v = QFB_DRAW_LINE1;
        break;
    case 2:
        v = QFB_DRAW_LINE2;
        break;
    case 4:
        v = QFB_DRAW_LINE4;
        break;
    case 8:
        v = QFB_DRAW_LINE8;
        break;
    case 16:
        v = QFB_DRAW_LINE16;
        break;
    case 24:
        v = QFB_DRAW_LINE24;
        break;
    }

    qfb_draw_line = qfb_draw_line_table[v];
    assert(qfb_draw_line != NULL);

    base  = s->regs[QFB_MODE_BASE >> 2];
    lines = qfb_visible_lines(s);
    if (lines == 0) {
        return;
    }

    /* Snapshot only the scanout, never the whole 32 MiB: past the end of the
       region memory_region_snapshot_get_dirty() asserts and takes QEMU down. */
    snap = memory_region_snapshot_and_clear_dirty(&s->mem_vram, base,
                                             (uint64_t)lines * qfb_stride,
                                             DIRTY_MEMORY_VGA);

    ymin = -1;
    page = base;
    for (y = 0; y < lines; y++, page += qfb_stride) {
        if (memory_region_snapshot_get_dirty(&s->mem_vram, snap, page,
                                             qfb_stride)) {
            uint8_t *data_display;

            data_display = surface_data(surface) + y * surface_stride(surface);
            qfb_draw_line(s, data_display, page, s->width);

            if (ymin < 0) {
                ymin = y;
            }
        } else {
            if (ymin >= 0) {
                dpy_gfx_update(s->con, 0, ymin, s->width, y - ymin);
                ymin = -1;
            }
        }
    }

    if (ymin >= 0) {
        dpy_gfx_update(s->con, 0, ymin, s->width, y - ymin);
    }

    g_free(snap);
}

/* this must mirror the calculation in the guest driver */
static uint32_t qfb_calculate_stride(uint32_t width, uint32_t depth)
{
    /* always return a 4-byte aligned rowbytes */
    if (depth == 24) {
        depth = 32;
    }
    return ((width * depth + 31) / 8) & ~(uint32_t)3;
}

static void qfb_update_mode(QfbState *s)
{
    s->regs[QFB_MODE_WIDTH >> 2] = s->width;
    s->regs[QFB_MODE_HEIGHT >> 2] = s->height;
    s->regs[QFB_MODE_STRIDE >> 2] = s->stride
        = qfb_calculate_stride(s->width, s->depth);
    s->regs[QFB_MODE_DEPTH >> 2] = s->depth;
    qfb_invalidate_display(s);
}

static void qfb_update_display(void *opaque)
{
    QfbState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);

    qemu_flush_coalesced_mmio_buffer();

    if (s->width == 0 || s->height == 0) {
        return;
    }

    if (s->width != surface_width(surface) ||
        s->height != surface_height(surface)) {
        qemu_console_resize(s->con, s->width, s->height);
        qfb_invalidate_display(s);
    }

    if (s->pal_dirty) {
        s->pal_dirty = false;
        qfb_invalidate_display(s);
    }

    qfb_draw_graphic(s);
}

static void qfb_update_irq(QfbState *s)
{
    uint32_t irq_state = s->regs[QFB_IRQ >> 2] &
                         s->regs[QFB_IRQ_MASK >> 2] &
                         QFB_IRQ_VBL;

    if (irq_state) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static int64_t qfb_next_vbl(void)
{
    return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + QFB_IRQ_VBL_PERIOD_NS) /
            QFB_IRQ_VBL_PERIOD_NS * QFB_IRQ_VBL_PERIOD_NS;
}

static void qfb_vbl_timer(void *opaque)
{
    QfbState *s = opaque;

    s->regs[QFB_IRQ >> 2] |= QFB_IRQ_VBL;
    qfb_update_irq(s);

    timer_mod(s->vbl_timer, qfb_next_vbl());
}

static void qfb_reset(QfbState *s)
{
    int i;

    timer_del(s->vbl_timer);
    s->regs[QFB_IRQ >> 2] = 0;
    s->regs[QFB_IRQ_MASK >> 2] = 0;
    qemu_irq_lower(s->irq);
    s->palette_current = 0;
    s->gamma_current = 0;
    for (i = 0; i < 256; i++) {
        s->palette_red[i] = 255 - i;
        s->palette_green[i] = 255 - i;
        s->palette_blue[i] = 255 - i;
        s->gamma_red[i] = i;
        s->gamma_green[i] = i;
        s->gamma_blue[i] = i;
    }
    memset(s->vram, 0, QFB_VRAM_SIZE);
    s->width = s->regs[QFB_CUSTOM_WIDTH >> 2];
    s->height = s->regs[QFB_CUSTOM_HEIGHT >> 2];
    s->depth = 1;
    s->regs[QFB_MODE_BASE >> 2] = 0;
    qfb_update_mode(s);
}

static uint64_t qfb_ctrl_read(void *opaque, hwaddr addr, unsigned int size)
{
    QfbState *s = opaque;
    uint64_t val = 0;

    switch (addr) {
    case QFB_VERSION:
        val = 0x71666231; /* 'qfb1' */
        break;
    case QFB_PAL_COLOR:
        val = ((uint32_t)s->palette_red[s->palette_current] << 16)
            | ((uint32_t)s->palette_green[s->palette_current] << 8)
            | ((uint32_t)s->palette_blue[s->palette_current]);
        break;
    case QFB_LUT_COLOR:
        val = ((uint32_t)s->gamma_red[s->gamma_current] << 16)
            | ((uint32_t)s->gamma_green[s->gamma_current] << 8)
            | ((uint32_t)s->gamma_blue[s->gamma_current]);
        break;
    default:
        if (addr < QFB_CTRL_TOPADDR) {
            val = s->regs[addr >> 2];
        } else {
            val = 0xFFFFFFFF;
        }
        break;
    }

    return val;
}

static void qfb_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned int size)
{
    QfbState *s = opaque;

    if (addr >= QFB_CTRL_TOPADDR) {
        return;
    }

    switch (addr) {
    case QFB_VERSION:
        qfb_reset(s);
        break;
    case QFB_MODE_WIDTH:
        s->width = (val > QFB_MAX_WIDTH ? QFB_MAX_WIDTH : val);
        qfb_update_mode(s);
        break;
    case QFB_MODE_HEIGHT:
        s->height = (val > QFB_MAX_HEIGHT ? QFB_MAX_HEIGHT : val);
        qfb_update_mode(s);
        break;
    case QFB_MODE_DEPTH:
        switch (val) {
        case 1: case 2: case 4: case 8: case 16: case 24:
            s->depth = val;
            break;
        case 32:
            /* the PCI variant also accepts 32 as an alias of 24 */
            s->depth = 24;
            break;
        default:
            s->depth = 1;
            break;
        }
        qfb_update_mode(s);
        break;
    case QFB_MODE_BASE:
        s->regs[addr >> 2] = (val % QFB_VRAM_SIZE) & ~(uint32_t)3;
        qfb_update_mode(s);
        break;
    case QFB_MODE_STRIDE:
        /* reject write */
        break;
    case QFB_PAL_INDEX:
        s->palette_current = val % 256;
        s->regs[addr >> 2] = s->palette_current;
        break;
    case QFB_PAL_COLOR:
        s->palette_red[s->palette_current] = (val >> 16) & 255;
        s->palette_green[s->palette_current] = (val >> 8) & 255;
        s->palette_blue[s->palette_current] = val & 255;
        s->pal_dirty = true;
        break;
    case QFB_LUT_INDEX:
        s->gamma_current = val % 256;
        s->regs[addr >> 2] = s->gamma_current;
        break;
    case QFB_LUT_COLOR:
        s->gamma_red[s->gamma_current] = (val >> 16) & 255;
        s->gamma_green[s->gamma_current] = (val >> 8) & 255;
        s->gamma_blue[s->gamma_current] = val & 255;
        s->pal_dirty = true;
        break;
    case QFB_IRQ_MASK:
        s->regs[addr >> 2] = val & QFB_IRQ_VBL;
        if (val & QFB_IRQ_VBL) {
            timer_mod(s->vbl_timer, qfb_next_vbl());
        } else {
            timer_del(s->vbl_timer);
        }
        qfb_update_irq(s);
        break;
    case QFB_IRQ:
        s->regs[addr >> 2] = s->regs[addr >> 2] & ~val;
        qfb_update_irq(s);
        break;
    case QFB_CUSTOM_DEPTH:
        /* support debug output from the guest driver */
        if (val <= 255) {
            fputc(val, stderr);
        }
        break;
    default:
        /* ignore all other writes */
        break;
    }
}

static const MemoryRegionOps qfb_ctrl_ops = {
    .read = qfb_ctrl_read,
    .write = qfb_ctrl_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static int qfb_post_load(void *opaque, int version_id)
{
    qfb_update_mode(opaque);
    return 0;
}

static const VMStateDescription vmstate_qfb = {
    .name = "qfb",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = qfb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(palette_current, QfbState),
        VMSTATE_UINT8_ARRAY(palette_red, QfbState, 256),
        VMSTATE_UINT8_ARRAY(palette_green, QfbState, 256),
        VMSTATE_UINT8_ARRAY(palette_blue, QfbState, 256),
        VMSTATE_UINT32(gamma_current, QfbState),
        VMSTATE_UINT8_ARRAY(gamma_red, QfbState, 256),
        VMSTATE_UINT8_ARRAY(gamma_green, QfbState, 256),
        VMSTATE_UINT8_ARRAY(gamma_blue, QfbState, 256),
        VMSTATE_UINT32(width, QfbState),
        VMSTATE_UINT32(height, QfbState),
        VMSTATE_UINT32(stride, QfbState),
        VMSTATE_UINT8(depth, QfbState),
        VMSTATE_UINT32_ARRAY(regs, QfbState, QFB_NUM_REGS),
        VMSTATE_TIMER_PTR(vbl_timer, QfbState),
        VMSTATE_END_OF_LIST()
    }
};

static const GraphicHwOps qfb_ops = {
    .invalidate = qfb_invalidate_display,
    .gfx_update = qfb_update_display,
};

static void qfb_pci_set_irq(void *opaque, int n, int level)
{
    QfbPCIState *s = QFB_PCI(opaque);

    pci_set_irq(PCI_DEVICE(s), level);
}

static void qfb_pci_realize(PCIDevice *dev, Error **errp)
{
    QfbPCIState *s = QFB_PCI(dev);
    QfbState *qfb = &s->qfb;
    DisplaySurface *surface;
    uint8_t *pci_conf = dev->config;

    if (qfb->width < 32) {
        qfb->width = 640;
    } else if (qfb->width > QFB_MAX_WIDTH) {
        qfb->width = QFB_MAX_WIDTH;
    }
    if (qfb->height < 32) {
        qfb->height = 480;
    } else if (qfb->height > QFB_MAX_HEIGHT) {
        qfb->height = QFB_MAX_HEIGHT;
    }
    qfb->regs[QFB_CUSTOM_WIDTH >> 2] = qfb->width;
    qfb->regs[QFB_CUSTOM_HEIGHT >> 2] = qfb->height;

    switch (qfb->depth) {
    case 1: case 2: case 4: case 8: case 16: case 24:
        break;
    case 32:
        qfb->depth = 24;
        break;
    default:
        qfb->depth = 8;
        break;
    }
    qfb->regs[QFB_CUSTOM_DEPTH >> 2] = qfb->depth;

    qfb->con = graphic_console_init(DEVICE(dev), 0, &qfb_ops, qfb);
    surface = qemu_console_surface(qfb->con);
    if (surface_bits_per_pixel(surface) != 32) {
        error_setg(errp, "unknown host depth %d",
                   surface_bits_per_pixel(surface));
        return;
    }

    memory_region_init_ram(&qfb->mem_vram, OBJECT(dev), "qfb.vram",
                           QFB_VRAM_SIZE, &error_abort);
    memory_region_set_log(&qfb->mem_vram, true, DIRTY_MEMORY_VGA);
    qfb->vram = memory_region_get_ram_ptr(&qfb->mem_vram);

    memory_region_init_io(&qfb->mem_ctrl, OBJECT(dev), &qfb_ctrl_ops, qfb,
                          "qfb.ctrl", QFB_CTRL_BAR_SIZE);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &qfb->mem_vram);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &qfb->mem_ctrl);

    pci_conf[PCI_INTERRUPT_PIN] = 1; /* INTA */
    qfb->irq = qemu_allocate_irq(qfb_pci_set_irq, s, 0);

    qfb->vbl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, qfb_vbl_timer, qfb);
    qfb_update_mode(qfb);
}

static void qfb_pci_exit(PCIDevice *dev)
{
    QfbPCIState *s = QFB_PCI(dev);
    QfbState *qfb = &s->qfb;

    timer_free(qfb->vbl_timer);
    qemu_free_irq(qfb->irq);
}

static void qfb_pci_reset_handler(DeviceState *d)
{
    QfbPCIState *s = QFB_PCI(d);

    qfb_reset(&s->qfb);
}

static Property qfb_pci_properties[] = {
    DEFINE_PROP_UINT32("width", QfbPCIState, qfb.width, 1024),
    DEFINE_PROP_UINT32("height", QfbPCIState, qfb.height, 768),
    DEFINE_PROP_UINT8("depth", QfbPCIState, qfb.depth, 8),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_qfb_pci = {
    .name = "qfb-pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, QfbPCIState),
        VMSTATE_STRUCT(qfb, QfbPCIState, 1, vmstate_qfb, QfbState),
        VMSTATE_END_OF_LIST()
    }
};

static void qfb_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = qfb_pci_realize;
    k->exit = qfb_pci_exit;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = PCI_DEVICE_ID_QEMU_QFB;
    k->class_id = PCI_CLASS_DISPLAY_OTHER;

    dc->desc = "PCI \"Qemu FrameBuffer\" (qfb1) for Macintosh";
    dc->vmsd = &vmstate_qfb_pci;
    device_class_set_legacy_reset(dc, qfb_pci_reset_handler);
    device_class_set_props(dc, qfb_pci_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo qfb_pci_info = {
    .name          = TYPE_QFB_PCI,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(QfbPCIState),
    .class_init    = qfb_pci_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void qfb_pci_register_types(void)
{
    type_register_static(&qfb_pci_info);
}

type_init(qfb_pci_register_types)
