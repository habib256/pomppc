/*
 * qfb_regs.h — registres du framebuffer paravirtualisé "qfb1".
 *
 * Contrat identique au device NuBus d'origine (Solra Bizna,
 * https://github.com/SolraBizna/mac_qfb_driver), transposé sur PCI par le
 * device QEMU hw/display/qfb-pci.c de POMPPC.
 *
 * BAR0 : VRAM linéaire, 32 Mio.
 * BAR1 : ces registres (32 bits, big endian natif côté PowerPC).
 */

#ifndef QFB_REGS_H
#define QFB_REGS_H

#define QFB_MAGIC           0x71666231UL /* 'qfb1' */

#define QFB_VERSION         0x00 /* lecture : 'qfb1' ; écriture : reset */
#define QFB_MODE_WIDTH      0x04 /* largeur en pixels */
#define QFB_MODE_HEIGHT     0x08 /* hauteur en pixels */
#define QFB_MODE_DEPTH      0x0C /* 1, 2, 4, 8, 16, 24 (24 = 32 bpp xRGB) */
#define QFB_MODE_BASE       0x10 /* offset du scanout dans la VRAM (align. 4) */
#define QFB_MODE_STRIDE     0x14 /* rowbytes du mode courant (lecture seule) */
#define QFB_PAL_INDEX       0x1C /* index dans la palette */
#define QFB_PAL_COLOR       0x20 /* entrée de palette, xxRRGGBB */
#define QFB_LUT_INDEX       0x24 /* index dans les LUT gamma */
#define QFB_LUT_COLOR       0x28 /* entrée gamma, xxRRGGBB */
#define QFB_IRQ_MASK        0x2C /* interruptions activées */
#define QFB_IRQ             0x30 /* lecture : en attente ; écriture : ack */
#define QFB_CUSTOM_WIDTH    0x34 /* largeur demandée sur la ligne de commande */
#define QFB_CUSTOM_HEIGHT   0x38 /* hauteur demandée */
#define QFB_CUSTOM_DEPTH    0x3C /* profondeur demandée ; écriture = stderr */

#define QFB_IRQ_VBL         0x01

#define QFB_VRAM_SIZE       (32UL * 1024UL * 1024UL)
#define QFB_MAX_WIDTH       3840
#define QFB_MAX_HEIGHT      2160

/* Doit refléter exactement qfb_calculate_stride() côté QEMU. */
#define QFB_STRIDE(w, d)    ((((w) * ((d) == 24 ? 32 : (d)) + 31) / 8) & ~3UL)

#endif /* QFB_REGS_H */
