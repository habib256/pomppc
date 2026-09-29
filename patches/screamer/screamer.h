/* GPL3 - Copyleft VERHILLE Arnaud */
/*
 * QEMU PowerMac Awacs Screamer device support
 *
 * Copyright (c) 2016 Mark Cave-Ayland
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_AUDIO_SCREAMER_H
#define HW_AUDIO_SCREAMER_H

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/ppc/mac_dbdma.h"
#include "audio/audio.h"

#define TYPE_SCREAMER "screamer"
OBJECT_DECLARE_SIMPLE_TYPE(ScreamerState, SCREAMER)

/* Anneau fixe, indépendant du tampon hôte : 8192 trames stéréo S16
 * font environ 186 ms à 44,1 kHz. L'ancienne taille 0x4000 n'était pas utilisée. */
#define SCREAMER_RING_FRAMES 8192

struct ScreamerState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mem;
    qemu_irq irq;
    void *dbdma;
    qemu_irq dma_tx_irq;
    qemu_irq dma_rx_irq;

    QEMUSoundCard card;
    SWVoiceOut *voice;
    uint8_t *mixbuf;
    int samples;
    int shift;

    uint32_t wpos;
    uint32_t rpos;
    /* 0 : on attend la réserve avant de jouer. 1 : on joue jusqu'à la vidange. */
    int running;

    uint32_t bpos;
    uint32_t ppos;
    uint32_t rate;
    DBDMA_io io;
    /* CMDPTR de la commande dont io est le fragment : un CMDPTR réécrit
       pendant une pause abandonne le fragment (bug hunt 3, n° 4). */
    uint32_t io_cmdptr;
    /* posé juste avant io->dma_end() par pmac_screamer_tx_transfer : le
       callback sait ainsi que dbdma_end() a tourné (bug hunt 4, n° 1). */
    bool io_ended;
    /* canaux DBDMA de sortie et d'entrée, pour le reset (bug hunt 3, n° 5) */
    int dma_tx_ch;
    int dma_rx_ch;

    uint32_t regs[6];
    uint32_t codec_ctrl_regs[8];
};

void macio_screamer_register_dma(ScreamerState *s, void *dbdma, int txchannel, int rxchannel);

#endif
