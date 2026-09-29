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

/*
 * Vendu dans POMPPC depuis la branche « screamer-v9.1.0 » du fork
 * https://github.com/mcayland/qemu — copié ici pour que le dépôt puisse
 * reconstruire son binaire de référence SANS dépendre d'un fork tiers.
 * Seule modification : portage de dc->reset vers device_class_set_legacy_reset
 * (API changée entre QEMU 9.1 et 9.2). Voir patches/README.md.
 */

#include "qemu/osdep.h"
#include "audio/audio.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "audio/audio.h"
#include "hw/audio/screamer.h"
#include "hw/qdev-properties.h"
#include "qemu/timer.h"
#include "hw/ppc/mac_dbdma.h"
#include "sysemu/sysemu.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"

/* debug screamer */
//#define DEBUG_SCREAMER

#ifdef DEBUG_SCREAMER
#define SCREAMER_DPRINTF(fmt, ...)                                  \
    do { printf("SCREAMER: " fmt , ## __VA_ARGS__); } while (0)
#else
#define SCREAMER_DPRINTF(fmt, ...)
#endif

/* chip registers */
#define SND_CTRL_REG   0x0
#define CODEC_CTRL_REG 0x1
#define CODEC_STAT_REG 0x2
#define CLIP_CNT_REG   0x3
#define BYTE_SWAP_REG  0x4
#define FRAME_CNT_REG  0x5

#define CODEC_CTRL_MASKECMD        (0x1 << 24)
#define CODEC_CTRL1_RECALIBRATE    0x4

#define CODEC_STAT_MANUFACTURER_CRYSTAL    0x100
#define CODEC_STAT_AWACS_REVISION          0x3000
#define CODEC_STAT_MASK_VALID              (0x1 << 22) 

/* Audio */
static const char *s_spk = "screamer";

static void screamer_tx_copy(ScreamerState *s, DBDMA_io *io, int samples)
{
    int idx, first;

    idx = (int)(s->wpos % (uint32_t)s->samples);
    first = s->samples - idx;
    if (first > samples) {
        first = samples;
    }

    dma_memory_read(&address_space_memory, io->addr,
                    &s->mixbuf[idx << s->shift], first << s->shift,
                    MEMTXATTRS_UNSPECIFIED);
    if (samples > first) {
        dma_memory_read(&address_space_memory,
                        io->addr + (first << s->shift),
                        s->mixbuf, (samples - first) << s->shift,
                        MEMTXATTRS_UNSPECIFIED);
    }

    SCREAMER_DPRINTF("DMA actually transferred 0x%x, wpos is %d\n", samples << s->shift, s->wpos);

    io->addr += (samples << s->shift);
    io->len -= (samples << s->shift);
    s->wpos += samples;
    /*
     * Compteur de trames = position de lecture du DMA, comme sur le matériel
     * (la FIFO du Screamer ne tient que quelques trames). Apple02DBDMAAudio
     * en tire getCurrentSampleFrame() : la tête d'effacement d'IOAudioEngine
     * efface derrière lui, quatre fois par tour du tampon invité.
     *
     * Il comptait les trames sorties de l'anneau vers AUD_write, 8192 trames
     * (l'anneau plein) derrière le DMA : la moitié exacte des 16384 trames du
     * tampon de Tiger. « Derrière le compteur » tombait alors 8192 trames
     * DEVANT le DMA, là où le HAL venait d'écrire. Un client à petit tampon
     * d'E/S (512 trames) écrit plus près du DMA et n'était pas touché ;
     * DOOM 3 (tampon de 4096 trames) perdait la fin de chaque tampon :
     * ~47 ms de zéros toutes les 93 ms, la saccade (29/09/2026,
     * docs/audio-stabilite.md).
     */
    s->regs[FRAME_CNT_REG] += samples;
}

static void pmac_screamer_tx_transfer(ScreamerState *s)
{
    DBDMA_io *io = &s->io;
    int queued, space, samples;

    if (s->samples <= 0) {
        return;
    }

    /* wpos et rpos sont monotones : la place libre est tout l'anneau moins
     * ce qui n'a pas encore été joué, y compris le préfixe déjà consommé. */
    queued = (int)(s->wpos - s->rpos);
    space = s->samples - queued;
    if (space < 0) {
        space = 0;
    }
    samples = MIN(io->len >> s->shift, space);
    if (samples > 0) {
        screamer_tx_copy(s, io, samples);
    }

    /*
     * Reliquat de moins d'une trame (commande DBDMA dont la longueur n'est
     * pas multiple de 4) : il ne formera jamais un échantillon, et le garder
     * figerait le canal — dma_end() ne serait jamais appelé. On le consomme.
     */
    if (io->len > 0 && io->len < (1 << s->shift)) {
        io->addr += io->len;
        io->len = 0;
    }

    /* Continue DBDMA if we have completed the transfer, otherwise defer */
    if (io->len == 0) {
        SCREAMER_DPRINTF("-> End of transfer\n");
        s->io_ended = true;
        io->dma_end(io);
    }
}

static void pmac_screamer_tx(DBDMA_io *io)
{
    ScreamerState *s = io->opaque;

    SCREAMER_DPRINTF("DMA TX transfer: addr %" HWADDR_PRIx
                     " len: %x\n", io->addr, io->len);

    memcpy(&s->io, io, sizeof(DBDMA_io));
    s->io_cmdptr = ((DBDMA_channel *)io->channel)->regs[DBDMA_CMDPTR_LO];

    /*
     * Pas de garde « la requête ne tient pas, on abandonne ». Le transfert
     * copie ce que l'anneau peut recevoir, y compris à cheval sur la fin du
     * tampon, et laisse le reliquat dans io->len. Le callback le reprendra
     * dès qu'une lecture aura libéré de la place. Abandonner ici figerait le
     * canal : dma_end() ne serait jamais appelé.
     */
    pmac_screamer_tx_transfer(s);
}

/*
 * RUN et PAUSE tels qu'ils seront APRÈS l'écriture de CONTROL en cours.
 * dbdma_control_write() appelle flush avant de ranger le nouveau statut :
 * ch->regs[DBDMA_STATUS] porte encore l'ancien, on lui applique l'écriture.
 */
static uint32_t screamer_dma_next_status(DBDMA_channel *ch)
{
    uint32_t status = ch->regs[DBDMA_STATUS];
    uint16_t mask = (ch->regs[DBDMA_CONTROL] >> 16) & 0xffff;
    uint16_t value = ch->regs[DBDMA_CONTROL] & 0xffff;

    if (mask & RUN) {
        status = (status & ~RUN) | (value & RUN);
    }
    if (mask & PAUSE) {
        status = (status & ~PAUSE) | (value & PAUSE);
    }
    return status;
}

static void pmac_screamer_tx_flush(DBDMA_io *io)
{
    ScreamerState *s = io->opaque;
    DBDMA_channel *ch = io->channel;
    dbdma_cmd *current = &ch->current;
    uint32_t next = screamer_dma_next_status(ch);
    uint16_t cmd;

    SCREAMER_DPRINTF("DMA TX flush!\n");
#if 0
    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == OUTPUT_MORE || cmd == OUTPUT_LAST ||
        cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

        dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }
#endif

    if ((next & RUN) && !(next & PAUSE)) {
        /*
         * FLUSH seul, canal toujours en marche : rien à vider côté sortie.
         * Remettre processing à false ici laissait DBDMA_run relancer la
         * MÊME commande (CMDPTR n'a pas avancé) : pmac_screamer_tx écrasait
         * s->io et rejouait la partie déjà copiée — son dupliqué.
         */
        return;
    }

    if ((next & RUN) && s->io.len && s->io.channel == ch) {
        /*
         * PAUSE avec un fragment différé : on le garde, et processing reste
         * vrai pour que la reprise continue ce fragment au lieu de relancer
         * la commande depuis le début. screamer_pull_deferred ne tire rien
         * tant que le canal n'est pas ACTIVE.
         */
        return;
    }

    /*
     * Arrêt (RUN effacé) : le fragment différé est abandonné. Le callback
     * audio le tirait encore depuis une adresse que le pilote a pu recycler,
     * puis appelait dbdma_end() sur un canal arrêté — statut écrit dans le
     * descripteur courant (peut-être celui d'un NOUVEAU programme), IRQ, et
     * CMDPTR avancé : la première commande du programme suivant sautée.
     */
    s->io.len = 0;
    s->io.channel = NULL;
    ch->io.processing = false;

    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

            dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }

}

static void pmac_screamer_rx(DBDMA_io *io)
{
    SCREAMER_DPRINTF("DMA RX transfer: addr %" HWADDR_PRIx
                     " len: %x\n", io->addr, io->len);

    //ScreamerState *s = io->opaque;
    DBDMA_channel *ch = io->channel;
        
    /* FIXME: stop channel after updating with status to stop MacOS 9 freezing */
    ch->regs[DBDMA_STATUS] &= ~RUN;

    io->dma_end(io);
}

static void pmac_screamer_rx_flush(DBDMA_io *io)
{
    DBDMA_channel *ch = io->channel;
    dbdma_cmd *current = &ch->current;
    uint16_t cmd;

    SCREAMER_DPRINTF("DMA RX flush!\n");
#if 1
    cmd = le16_to_cpu(current->command) & COMMAND_MASK;
    if (cmd == INPUT_MORE || cmd == INPUT_LAST) {
        current->xfer_status = cpu_to_le16(ch->regs[DBDMA_STATUS]);
        current->res_count = cpu_to_le16(io->len);

        dma_memory_write(&address_space_memory, ch->regs[DBDMA_CMDPTR_LO],
                         &ch->current, sizeof(dbdma_cmd),
                         MEMTXATTRS_UNSPECIFIED);
    }
#endif
}

void macio_screamer_register_dma(ScreamerState *s, void *dbdma, int txchannel, int rxchannel)
{
    s->dbdma = dbdma;
    s->dma_tx_ch = txchannel;
    s->dma_rx_ch = rxchannel;
    DBDMA_register_channel(dbdma, txchannel, s->dma_tx_irq,
                           pmac_screamer_tx, pmac_screamer_tx_flush, s);
    DBDMA_register_channel(dbdma, rxchannel, s->dma_rx_irq,
                           pmac_screamer_rx, pmac_screamer_rx_flush, s);
}

/* Pull a DBDMA fragment that did not fit in the ring. A zero-length or missing
 * channel is a no-op, so silence padding cannot complete a transfer by itself. */
static void screamer_pull_deferred(ScreamerState *s)
{
    DBDMA_io *io = &s->io;
    DBDMA_channel *ch;
    uint32_t run, cp;
    uint16_t old_status;
    bool saved;

    if (!io->len || !io->channel) {
        return;
    }
    ch = io->channel;
    /* Canal arrêté ou en pause : le matériel ne transfère rien. */
    if (!ch->io.processing ||
        (ch->regs[DBDMA_STATUS] & (RUN | ACTIVE)) != (RUN | ACTIVE)) {
        return;
    }
    /*
     * CMDPTR réécrit pendant une pause (dbdma_write ne le protège que si
     * ACTIVE, et PAUSE efface ACTIVE) : le fragment appartient à l'ancien
     * programme. Le continuer écrirait son statut dans le premier
     * descripteur du nouveau, lèverait l'IRQ et le sauterait (S1 par le
     * chemin de la pause). On l'abandonne et le canal repart du nouveau.
     */
    cp = ch->regs[DBDMA_CMDPTR_LO];
    if (cp != s->io_cmdptr) {
        io->len = 0;
        io->channel = NULL;
        ch->io.processing = false;
        DBDMA_kick(container_of(ch, DBDMAState, channels[ch->channel]));
        return;
    }
    /*
     * RUN est retiré le temps du transfert pour que dbdma_end() ne relance
     * pas channel_run() depuis le callback audio (le BH s'en charge), puis
     * reposé SEUL : restaurer tout le statut effaçait ce que dbdma_end()
     * venait d'y faire (FLUSH retiré sur is_last, BT posé ou effacé).
     *
     * Mais dbdma_end() recopie ce statut SANS RUN dans le xfer_status du
     * descripteur, là où le DBDMA réel écrit RUN|ACTIVE (0x8400). En régime
     * établi presque toute commande OUTPUT finit par ici : on repose RUN
     * dans le descripteur, EN MÉMOIRE INVITÉ à `cp`.
     *
     * Pas dans ch->current (bug hunt 4, n° 1) : après la sauvegarde,
     * dbdma_end() fait conditional_branch(), qui RECHARGE ch->current avec
     * le descripteur SUIVANT. Relire ch->current recopiait le statut du
     * suivant sur celui qui vient de finir (ACTIVE, BT et DEVSTAT perdus).
     *
     * Savoir si la sauvegarde a eu lieu : io_ended dit que dbdma_end() a
     * été appelé ; il peut encore être sorti par conditional_wait() sans
     * rien écrire ni avancer. CMDPTR qui bouge prouve la sauvegarde ; s'il
     * ne bouge pas (branche sur soi-même, ou attente), le témoin 0xFFFF
     * posé dans ch->current le tranche : un statut sans RUN ne peut pas le
     * valoir, et le rechargement du même descripteur l'a remplacé.
     */
    old_status = ch->current.xfer_status;
    ch->current.xfer_status = 0xffff;
    s->io_ended = false;
    run = ch->regs[DBDMA_STATUS] & RUN;
    ch->regs[DBDMA_STATUS] &= ~RUN;
    pmac_screamer_tx_transfer(s);
    ch->regs[DBDMA_STATUS] |= run;
    saved = s->io_ended &&
            (ch->regs[DBDMA_CMDPTR_LO] != cp || ch->current.xfer_status != 0xffff);
    if (!saved) {
        if (ch->current.xfer_status == 0xffff) {
            ch->current.xfer_status = old_status;  /* rien d'écrit */
        }
    } else if (run) {
        uint16_t st;

        dma_memory_read(&address_space_memory,
                        cp + offsetof(dbdma_cmd, xfer_status),
                        &st, sizeof(uint16_t), MEMTXATTRS_UNSPECIFIED);
        st = cpu_to_le16(le16_to_cpu(st) | RUN);
        dma_memory_write(&address_space_memory,
                         cp + offsetof(dbdma_cmd, xfer_status),
                         &st, sizeof(uint16_t), MEMTXATTRS_UNSPECIFIED);
        if (ch->regs[DBDMA_CMDPTR_LO] == cp) {
            ch->current.xfer_status = st;          /* branche sur soi-même */
        }
    }
    DBDMA_kick(container_of(ch, DBDMAState, channels[ch->channel]));
}

static int screamer_queued(const ScreamerState *s)
{
    return (int)(s->wpos - s->rpos);
}

/* Réserve avant de démarrer ou de reprendre : ~20 ms, plafonnée à la moitié
 * de l'anneau. Un débit nul (tests, voix pas encore ouverte) joue dès la
 * première trame pour ne pas rester muet. */
static int screamer_prime(const ScreamerState *s)
{
    int half, prime;

    if (s->samples < 2) {
        return 1;
    }
    half = s->samples / 2;
    prime = (int)(s->rate / 50);
    if (prime < 1) {
        return 1;
    }
    return prime < half ? prime : half;
}

static void screamer_write_silence(ScreamerState *s, int *free_b)
{
    uint8_t silence[256];
    unsigned frame = 1u << s->shift;

    memset(silence, 0, sizeof(silence));
    while (*free_b >= (int)frame) {
        size_t chunk = sizeof(silence);
        size_t accepted;

        if (chunk > (size_t)*free_b) {
            chunk = (size_t)*free_b;
        }
        chunk &= ~(size_t)(frame - 1);
        if (!chunk) {
            break;
        }
        accepted = AUD_write(s->voice, silence, chunk);
        if (!accepted) {
            break;
        }
        *free_b -= (int)accepted;
    }
}

static void screamerspk_callback(void *opaque, int free_b)
{
    ScreamerState *s = opaque;

    if (s->samples <= 0) {
        return;
    }

    for (;;) {
        int before = screamer_queued(s);
        int before_len = s->io.len;

        if (s->io.len && before < s->samples) {
            screamer_pull_deferred(s);
        }

        if (!s->running) {
            if (screamer_queued(s) < screamer_prime(s)) {
                break;
            }
            s->running = 1;
        }

        if (free_b <= 0 || screamer_queued(s) <= 0) {
            break;
        }

        while (free_b > 0 && screamer_queued(s) > 0) {
            int cap = s->samples;
            int idx = (int)(s->rpos % (uint32_t)cap);
            int contig = cap - idx;
            unsigned samples = MIN((unsigned)free_b >> s->shift,
                                   (unsigned)screamer_queued(s));
            size_t requested, accepted;

            if ((int)samples > contig) {
                samples = (unsigned)contig;
            }
            requested = (size_t)samples << s->shift;
            if (!requested) {
                return;
            }
            accepted = AUD_write(s->voice, s->mixbuf + (idx << s->shift),
                                 requested);
            /* AUD_write may accept less than requested (including zero). Only
             * retire PCM actually accepted, otherwise samples vanish. Writes
             * are whole stereo frames. A short write is backpressure: keep the
             * rest, do not replace it with silence, and do not spin. The frame
             * counter follows the DMA (screamer_tx_copy), not this output. */
            samples = (unsigned)(accepted >> s->shift);
            s->rpos += samples;
            free_b -= (int)accepted;
            if (accepted < requested) {
                return;
            }
        }

        if (screamer_queued(s) == before && s->io.len == before_len) {
            break;
        }
    }

    /* Silence only when playback has not started, or the ring is really empty.
     * Queued PCM below the reserve stays put: padding it here would open a gap
     * in a stream whose next DBDMA command is only a moment late. Silence is
     * not guest audio, so the frame counter stays put. */
    if (!s->running || screamer_queued(s) == 0) {
        if (screamer_queued(s) == 0) {
            s->running = 0;
        }
        screamer_write_silence(s, &free_b);
    }
}

static void screamer_update_settings(ScreamerState *s)
{
    struct audsettings as = { s->rate, 2, AUDIO_FORMAT_S16,
        1 };

    s->voice = AUD_open_out(&s->card, s->voice, s_spk, s, screamerspk_callback, &as);
    if (!s->voice) {
        AUD_log(s_spk, "Could not open voice\n");
        return;
    }

    s->wpos = 0;
    s->rpos = 0;
    s->running = 0;
    g_free(s->mixbuf);
    s->mixbuf = NULL;
    s->shift = 2;
    /* Taille fixe : la caler sur un seul tampon hôte supprime toute réserve
     * et bloque l'écriture dès que la lecture a avancé sans tout vider. */
    s->samples = SCREAMER_RING_FRAMES;
    s->mixbuf = g_malloc0((size_t)s->samples << s->shift);

    AUD_set_active_out(s->voice, true);
}

static void screamer_update_volume(ScreamerState *s)
{
    uint8_t muted = s->codec_ctrl_regs[0x1] & 0x80 ? 1 : 0;
    uint8_t att_left = (s->codec_ctrl_regs[0x4] & 0xf);
    uint8_t att_right = (s->codec_ctrl_regs[0x4] & 0x3c0) >> 6;

    SCREAMER_DPRINTF("setting mute: %d, attenuation L: %d R: %d\n",
                     muted, att_left, att_right);

    AUD_set_volume_out(s->voice, muted, (0xf - att_left) << 4,
                       (0xf - att_right) << 4);
}

static void screamer_reset(DeviceState *dev)
{
    ScreamerState *s = SCREAMER(dev);
    
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->codec_ctrl_regs, 0, sizeof(s->codec_ctrl_regs));
    memset(&s->io, 0, sizeof(DBDMA_io));
    s->io_cmdptr = 0;
    s->io_ended = false;
    /*
     * mac_dbdma_reset ne remet à zéro que les registres des canaux, pas
     * io.processing : un reset pendant la lecture (fragment différé) le
     * laissait vrai, et DBDMA_run sautait le canal tant que l'invité
     * n'effaçait pas RUN — muet pour un invité qui pose RUN directement.
     */
    if (s->dbdma) {
        DBDMAState *d = s->dbdma;

        d->channels[s->dma_tx_ch].io.processing = false;
        d->channels[s->dma_rx_ch].io.processing = false;
    }

    s->rate = 44100;
    screamer_update_settings(s);

    s->bpos = 0;
    s->ppos = 0;

    return;
}

static void screamer_realizefn(DeviceState *dev, Error **errp)
{
    ScreamerState *s = SCREAMER(dev);

    if (!AUD_register_card(s_spk, &s->card, errp)) {
        return;
    }

    s->rate = 44100;
    screamer_update_settings(s);
}

static void screamer_control_write(ScreamerState *s, uint32_t val)
{
    uint32_t rate = s->rate;

    SCREAMER_DPRINTF("%s: val %" PRId32 "\n", __func__, val);

    /* Basic rate selection */
    switch ((val & 0x700) >> 8) {
    case 0x00:
        rate = 44100;
        break;
    case 0x1:
        rate = 29400;
        break;
    case 0x2:
        rate = 22050;
        break;
    case 0x3:
        rate = 17640;
        break;
    case 0x4:
        rate = 14700;
        break;
    case 0x5:
        rate = 11025;
        break;
    case 0x6:
        rate = 8820;
        break;
    case 0x7:
        rate = 7350;
        break;
    }

    s->regs[0] = val;
    if (rate == s->rate) {
        return;
    }
    SCREAMER_DPRINTF("basic rate: %d\n", rate);
    s->rate = rate;
    screamer_update_settings(s);
}

static void screamer_codec_write(ScreamerState *s, hwaddr addr, uint64_t val)
{
    //SCREAMER_DPRINTF("%s: addr " HWADDR_PRIx " val %" PRIx64 "\n", __func__, addr, val);

    if (addr == 0x1) {
        /* Clear recalibrate if set */
        val = val & ~CODEC_CTRL1_RECALIBRATE;
    }

    /* Store first: update_volume reads codec_ctrl_regs. Calling it before the
     * store applied mute and attenuation one write late. */
    s->codec_ctrl_regs[addr] = val;
    if (addr == 0x1 || addr == 0x4) {
        screamer_update_volume(s);
    }
}

static uint64_t screamer_read(void *opaque, hwaddr addr, unsigned size)
{
    ScreamerState *s = opaque;
    uint32_t val;

    addr = addr >> 4;
    switch (addr) {
    case SND_CTRL_REG:
        val = s->regs[addr];
        break;
    case CODEC_CTRL_REG:
        val = s->regs[addr] & ~CODEC_CTRL_MASKECMD;
        break;
    case CODEC_STAT_REG:
        if (s->codec_ctrl_regs[7] & 1) {
            /* Read back mode */
            /* Sélecteur en bits 3:1 du registre 7 (kReadBackRegisterMask
             * 0xE). « & 0xe » après le décalage prenait le registre pair
             * voisin et, bit 4 posé, lisait hors du tableau (index 8..14). */
            val = s->codec_ctrl_regs[(s->codec_ctrl_regs[7] >> 1) & 7];
        } else {
            /* Return status register */
            val = s->regs[addr] & ~0xff00;
            val |= CODEC_STAT_MANUFACTURER_CRYSTAL | CODEC_STAT_AWACS_REVISION |
                   CODEC_STAT_MASK_VALID;
        }
        break;
    case CLIP_CNT_REG:
    case BYTE_SWAP_REG:
    case FRAME_CNT_REG:
        val = s->regs[addr];
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                  "screamer: Unimplemented register read "
                  "reg 0x%" HWADDR_PRIx " size 0x%x\n",
                  addr, size);
        val = 0;
        break;
    }

    SCREAMER_DPRINTF("%s: addr " HWADDR_FMT_plx " -> %x\n", __func__, addr, val);

    return val;
}

static void screamer_write(void *opaque, hwaddr addr,
                           uint64_t val, unsigned size)
{
    ScreamerState *s = opaque;
    uint32_t codec_addr;

    addr = addr >> 4;

    SCREAMER_DPRINTF("%s: addr " HWADDR_FMT_plx " val %" PRIx64 "\n", __func__, addr, val);

    switch (addr) {
    case SND_CTRL_REG:
        screamer_control_write(s, val & 0xffffffff);
        break;
    case CODEC_CTRL_REG:
        s->regs[addr] = val & 0xffffffff;
        codec_addr = (val & 0x7fff) >> 12;
        screamer_codec_write(s, codec_addr, val & 0xfff);
        break;
    case CODEC_STAT_REG:
    case CLIP_CNT_REG:
    case BYTE_SWAP_REG:
    /*
     * Le pilote de Tiger remet le compteur de trames à 0 au démarrage du
     * moteur, juste avant de lancer le DMA au début de son tampon. Ignorée,
     * cette écriture laissait le compte de toutes les lectures précédentes :
     * la tête d'effacement tombait n'importe où par rapport au DMA (voir
     * screamer_tx_copy).
     */
    case FRAME_CNT_REG:
        s->regs[addr] = val & 0xffffffff;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                  "screamer: Unimplemented register write "
                  "reg 0x%" HWADDR_PRIx " size 0x%x value 0x%" PRIx64 "\n",
                  addr, size, val);
        break;
    }

    return;
}

static const MemoryRegionOps screamer_ops = {
    .read = screamer_read,
    .write = screamer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    }
};

static void screamer_initfn(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    ScreamerState *s = SCREAMER(obj);

    memory_region_init_io(&s->mem, obj, &screamer_ops, s, "screamer", 0x1000);
    sysbus_init_mmio(d, &s->mem);
    sysbus_init_irq(d, &s->irq);
    sysbus_init_irq(d, &s->dma_tx_irq);
    sysbus_init_irq(d, &s->dma_rx_irq);
}

static Property screamer_properties[] = {
    DEFINE_AUDIO_PROPERTIES(ScreamerState, card),
    DEFINE_PROP_END_OF_LIST()
};

static void screamer_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = screamer_realizefn;
    /* POMPPC : la branche amont vise QEMU 9.1, où dc->reset existait encore.
       9.2 l'a retiré au profit de device_class_set_legacy_reset() (même
       sémantique, cf. hw/display/qfb-pci.c du même dépôt). */
    device_class_set_legacy_reset(dc, screamer_reset);
    device_class_set_props(dc, screamer_properties);
}

static const TypeInfo screamer_type_info = {
    .name = TYPE_SCREAMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ScreamerState),
    .instance_init = screamer_initfn,
    .class_init = screamer_class_init,
};

static void screamer_register_types(void)
{
    type_register_static(&screamer_type_info);
}

type_init(screamer_register_types)
