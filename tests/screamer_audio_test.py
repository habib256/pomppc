#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Compile the real Screamer callback with a deterministic audio/DMA sink."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / 'patches/screamer/screamer.c').read_text()
callback = source[source.index('static void screamer_pull_deferred('):source.index('static void screamer_update_settings(')]
stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define FRAME_CNT_REG 5
#define DBDMA_STATUS 0
#define DBDMA_CMDPTR_LO 1
#define RUN 0x8000
#define ACTIVE 0x0400
#define SCREAMER_DPRINTF(...) ((void)0)
#define le16_to_cpu(x) (x)
#define cpu_to_le16(x) (x)
#define MEMTXATTRS_UNSPECIFIED 0
static int address_space_memory;
typedef struct { uint16_t req_count, command; uint32_t phy_addr, cmd_dep;
                 uint16_t res_count, xfer_status; } dbdma_cmd;
typedef struct { int processing; } DBDMA_chan_io;
typedef struct { unsigned regs[2]; int channel; DBDMA_chan_io io; dbdma_cmd current; } DBDMA_channel;
/* Mémoire invitée simulée (les descripteurs y vivent) et témoin des écritures
   du callback : adresse et valeur du dernier xfer_status. */
static uint8_t gmem[0x3000];
static uint32_t wr_addr; static uint16_t wr_val; static unsigned wr_n;
#define dma_memory_write(as, addr, buf, len, attr) \
  do { (void)(as); (void)(attr); assert((len)==2); wr_addr=(addr); \
       memcpy(&wr_val,(buf),2); memcpy(gmem+(addr),(buf),2); wr_n++; } while (0)
#define dma_memory_read(as, addr, buf, len, attr) \
  do { (void)(as); (void)(attr); memcpy((buf), gmem+(addr), (len)); } while (0)
typedef struct { DBDMA_channel channels[1]; } DBDMAState;
#define container_of(p,t,m) ((t *)(p))
typedef struct { DBDMA_channel *channel; int len; } DBDMA_io;
typedef struct {
  void *audio_be; void *voice; int samples; unsigned shift;
  uint32_t wpos, rpos, rate; int running;
  unsigned regs[6]; uint8_t *mixbuf; DBDMA_io io; uint32_t io_cmdptr; bool io_ended;
} ScreamerState;
static uint8_t output[512];
static unsigned written, limit = 64, kicks;
/* QEMU 11.1.2 : audio_be_write(s->audio_be, voix, …) a remplacé AUD_write ;
   le témoin vérifie que le PCM part bien vers le backend du device. */
static int be_token;
#define BE ((void *)&be_token)
static size_t audio_be_write(void *be, void *v, void *data, size_t n) {
  (void)v; assert(be==BE); n=MIN(n,limit); assert(written+n<=sizeof(output));
  memcpy(output+written,data,n); written+=n; return n;
}
static void pmac_screamer_tx_transfer(ScreamerState *s) {
  int queued, space, frames, idx, first;
  if (s->samples <= 0) return;
  queued = (int)(s->wpos - s->rpos);
  space = s->samples - queued;
  if (space < 0) space = 0;
  frames = MIN((int)(s->io.len >> s->shift), space);
  if (frames <= 0) return;
  idx = (int)(s->wpos % (uint32_t)s->samples);
  first = s->samples - idx;
  if (first > frames) first = frames;
  memset(s->mixbuf + (idx << s->shift), 0x22, (size_t)first << s->shift);
  if (frames > first)
    memset(s->mixbuf, 0x22, (size_t)(frames - first) << s->shift);
  s->wpos += (uint32_t)frames;
  s->regs[FRAME_CNT_REG] += (unsigned)frames;   /* comme screamer_tx_copy */
  s->io.len -= frames << s->shift;
  if (s->io.len == 0) {
    /* dbdma_end() fidèle à mac_dbdma.c : statut du canal (sans RUN, retiré
       par l'appelant) recopié dans le descripteur, descripteur SAUVÉ en
       mémoire à CMDPTR, puis conditional_branch() → next() : CMDPTR avance
       et ch->current est RECHARGÉ avec le descripteur suivant. */
    DBDMA_channel *ch = s->io.channel;
    uint32_t cp = ch->regs[DBDMA_CMDPTR_LO];
    s->io_ended = true;
    ch->current.xfer_status = (uint16_t)ch->regs[DBDMA_STATUS];
    memcpy(gmem + cp, &ch->current, sizeof(dbdma_cmd));
    ch->regs[DBDMA_CMDPTR_LO] = cp + sizeof(dbdma_cmd);
    memcpy(&ch->current, gmem + ch->regs[DBDMA_CMDPTR_LO], sizeof(dbdma_cmd));
    /* le BH lancera la commande suivante : pmac_screamer_tx() note son CMDPTR */
    s->io_cmdptr = ch->regs[DBDMA_CMDPTR_LO];
  }
}
static void DBDMA_kick(DBDMAState *s) { (void)s; ++kicks; }
'''
test = r'''
int main(void) {
  uint8_t pcm[16]; memset(pcm,0x11,sizeof(pcm));
  DBDMA_channel channel={{RUN|ACTIVE,0},0,{1},{0,0,0,0,0,0}};
  ScreamerState s={.audio_be=BE,.samples=4,.shift=2,.wpos=4,.mixbuf=pcm,.io={&channel,0}};
  limit=4; screamerspk_callback(&s,16);
  /* Le compteur de trames suit le DMA, pas la sortie : jouer ce qui est
     déjà dans l'anneau ne le fait pas avancer (saccades DOOM 3, 29/09). */
  assert(written==4 && s.rpos==1 && s.regs[5]==0); // no discarded samples
  limit=0; screamerspk_callback(&s,12);
  assert(written==4 && s.rpos==1); // backpressure must not spin or advance
  limit=64; screamerspk_callback(&s,12);
  assert(written==16 && s.rpos==4 && s.wpos==4 && s.regs[5]==0);
  for (unsigned i=0;i<16;i++) assert(output[i]==0x11);
  written=0; s.rpos=0; s.wpos=4; s.io.len=16; s.regs[5]=0;
  screamerspk_callback(&s,32);
  assert(written==32 && s.io.len==0 && s.regs[5]==4 && kicks==1); // 4 trames tirées du DMA
  for (unsigned i=0;i<32;i++) assert(output[i]==(i<16?0x11:0x22));
  unsigned frames = s.regs[5];
  screamerspk_callback(&s,32); // empty queue: silence, frame counter stays
  assert(written==64 && s.regs[5]==frames && kicks==1);
  for (unsigned i=32;i<64;i++) assert(output[i]==0);
  limit=0; screamerspk_callback(&s,16);
  assert(written==64 && s.regs[5]==frames); // silence must not spin on a zero write
  s.wpos=4; s.rpos=0; screamerspk_callback(&s,0);
  assert(written==64 && s.rpos==0 && s.regs[5]==frames);
  written=0; kicks=0; s.regs[5]=0; s.wpos=0; s.rpos=0; s.io.len=8; limit=64;
  screamerspk_callback(&s,16); // deferred fragment, then silence for the rest
  assert(kicks==1 && s.io.len==0 && s.regs[5]==2 && written==16);
  for (unsigned i=0;i<8;i++) assert(output[i]==0x22);
  for (unsigned i=8;i<16;i++) assert(output[i]==0);
  /* Canal arrêté (RUN retiré) ou commande abandonnée (processing) : le
     fragment différé ne doit plus être tiré depuis l'ancienne adresse. */
  written=0; kicks=0; s.wpos=0; s.rpos=0; s.io.len=8;
  channel.regs[0]=ACTIVE; screamerspk_callback(&s,16);
  assert(kicks==0 && s.io.len==8);
  channel.regs[0]=RUN|ACTIVE; channel.io.processing=0; screamerspk_callback(&s,16);
  assert(kicks==0 && s.io.len==8);
  channel.io.processing=1; s.io.len=0;
  /* Fin de commande par le callback : le descripteur porte RUN|ACTIVE, comme
     sur le DBDMA réel, et non le statut sans RUN du transfert. */
  /* Le descripteur SUIVANT porte en mémoire un statut remis à 0 par le
     pilote : c'est lui que ch->current contient après dbdma_end(), et ce
     n'est pas lui qu'il faut recopier (bug hunt 4, n° 1). */
  written=0; kicks=0; wr_n=0; s.wpos=0; s.rpos=0; s.io.len=8;
  memset(gmem, 0, sizeof(gmem));
  channel.regs[DBDMA_CMDPTR_LO]=0x1000; s.io_cmdptr=0x1000; channel.current.xfer_status=0x1234;
  screamerspk_callback(&s,16);
  assert(kicks==1 && s.io.len==0 && wr_n==1);
  assert(wr_addr==0x1000+offsetof(dbdma_cmd,xfer_status) && wr_val==(RUN|ACTIVE));
  { uint16_t st; memcpy(&st, gmem+0x1000+offsetof(dbdma_cmd,xfer_status), 2);
    assert(st==(RUN|ACTIVE)); }                        /* 0x8400 en mémoire */
  { uint16_t st; memcpy(&st, gmem+0x1010+offsetof(dbdma_cmd,xfer_status), 2);
    assert(st==0); }                                   /* suivant intact */
  assert(channel.regs[DBDMA_CMDPTR_LO]==0x1010 && channel.current.xfer_status==0);
  assert(channel.regs[DBDMA_STATUS]==(RUN|ACTIVE));
  /* Descripteur suivant dont le statut en mémoire vaut 0xFFFF (anneau non
     initialisé) : la sauvegarde est prouvée par CMDPTR qui a bougé, RUN est
     reposé quand même, et ch->current (le suivant) n'est pas touché. */
  wr_n=0; kicks=0; s.wpos=0; s.rpos=0; s.io.len=8;
  memset(gmem+0x1020, 0xff, sizeof(dbdma_cmd));
  screamerspk_callback(&s,16);
  assert(wr_n==1 && wr_addr==0x1010+offsetof(dbdma_cmd,xfer_status) && wr_val==(RUN|ACTIVE));
  assert(channel.regs[DBDMA_CMDPTR_LO]==0x1020 && channel.current.xfer_status==0xffff);
  channel.current.xfer_status=0x1234;
  /* Fragment en cours sans fin de commande : témoin rendu, rien d'écrit. */
  wr_n=0; kicks=0; s.wpos=0; s.rpos=0; s.io.len=64; channel.current.xfer_status=0x1234;
  screamerspk_callback(&s,4);
  assert(wr_n==0 && channel.current.xfer_status==0x1234 && s.io.len>0);
  /* CMDPTR réécrit pendant une pause : fragment abandonné, rien tiré. */
  unsigned w0=s.wpos; channel.regs[DBDMA_CMDPTR_LO]=0x2000; kicks=0;
  screamerspk_callback(&s,16);
  assert(s.io.len==0 && s.io.channel==0 && channel.io.processing==0 && kicks==1 && s.wpos==w0);
  channel.io.processing=1; s.io.channel=&channel; channel.regs[DBDMA_CMDPTR_LO]=0; s.io_cmdptr=0;

  uint8_t ring[256];
  memset(ring, 0x33, sizeof(ring));
  ScreamerState w={.audio_be=BE,.samples=64,.shift=2,.rate=44100,.wpos=16,.mixbuf=ring,.io={&channel,0}};
  written=0; limit=512;
  screamerspk_callback(&w, 64); /* 16 trames < réserve 32 : silence, PCM conservé */
  assert(w.rpos==0 && w.wpos==16 && w.running==0 && written==64);
  for (unsigned i=0;i<64;i++) assert(output[i]==0);
  w.wpos=32; written=0;
  screamerspk_callback(&w, 16);
  assert(w.running==1 && w.rpos==4 && written==16);
  for (unsigned i=0;i<16;i++) assert(output[i]==0x33);

  uint8_t wrapb[16];
  memset(wrapb, 0x11, sizeof(wrapb));
  memset(wrapb + 12, 0xAB, 4);
  memset(wrapb, 0xCD, 4);
  ScreamerState wr={.audio_be=BE,.samples=4,.shift=2,.wpos=5,.rpos=3,.mixbuf=wrapb,.io={&channel,0}};
  written=0; limit=64;
  screamerspk_callback(&wr, 8);
  assert(written==8 && wr.rpos==5 && output[0]==0xAB && output[4]==0xCD);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='screamer-audio-') as d:
    c = Path(d)/'test.c'
    exe = Path(d)/'test'
    c.write_text(stub + callback + test)
    subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

env = {k:v for k,v in os.environ.items() if k != 'POMPPC_AUDIO_PROFILE'}
for backend, profile, expected, code in (
        ('coreaudio','stable','buffer-count=8',0),
        ('coreaudio','default','coreaudio,id=snd0',0),
        ('pa','stable','pa,id=snd0',0),
        ('coreaudio','typo','',2)):
    p=subprocess.run(['bash','-c','source "$1"; tiger_audio_device','bash',str(ROOT/'scripts/tiger_audio.sh')],
                     env=dict(env, HOST_AUDIODEV=backend,POMPPC_AUDIO_PROFILE=profile),capture_output=True,text=True)
    assert p.returncode==code and expected in p.stdout, p
volume = source[source.index('static void screamer_update_volume('):source.index('static void screamer_reset(')]
control = source[source.index('static void screamer_control_write('):source.index('static void screamer_codec_write(')]
codec = source[source.index('static void screamer_codec_write('):source.index('static uint64_t screamer_read(')]
reg_stub = r'''
#include <assert.h>
#include <stdint.h>
#define SCREAMER_DPRINTF(...) ((void)0)
#define CODEC_CTRL1_RECALIBRATE 0x4
typedef unsigned long hwaddr;
typedef struct { void *audio_be; void *voice; uint32_t rate; uint32_t regs[1]; uint32_t codec_ctrl_regs[8]; } ScreamerState;
static int settings_calls;
static void screamer_update_settings(ScreamerState *s) { (void)s; settings_calls++; }
static int vol_mute, vol_l, vol_r, vol_calls;
static int be_token;
/* QEMU 11.1.2 : audio_be_set_volume_out_lr a remplacé AUD_set_volume_out. */
static void audio_be_set_volume_out_lr(void *be, void *v, int mute, int l, int r) {
  (void)v; assert(be==&be_token); vol_mute=mute; vol_l=l; vol_r=r; vol_calls++;
}
'''
reg_test = r'''
int main(void) {
  ScreamerState s = {0};
  s.audio_be = &be_token;
  s.rate = 22050;
  screamer_control_write(&s, 0x000);
  assert(settings_calls==1 && s.rate==44100);
  screamer_control_write(&s, 0x000);
  assert(settings_calls==1 && s.rate==44100);
  screamer_control_write(&s, 0x200);
  assert(settings_calls==2 && s.rate==22050);
  screamer_codec_write(&s, 1, 0x80);
  assert(vol_calls==1 && vol_mute==1);
  screamer_codec_write(&s, 4, 0x1 | (2u << 6));
  assert(vol_calls==2 && vol_mute==1 && vol_l==224 && vol_r==208);
  screamer_codec_write(&s, 1, 0x84);
  assert(vol_mute==1 && (s.codec_ctrl_regs[1] & 0x4)==0);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='screamer-regs-') as d:
    c = Path(d)/'test.c'
    exe = Path(d)/'test'
    c.write_text(reg_stub + volume + control + codec + reg_test)
    subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-Wno-unused-parameter', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
xfer = source[source.index('static void screamer_tx_copy('):source.index('static void pmac_screamer_tx(')]
ring_stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define SCREAMER_DPRINTF(...) ((void)0)
#define MEMTXATTRS_UNSPECIFIED 0
static int address_space_memory;
typedef struct DBDMA_io DBDMA_io;
typedef void (*DBDMA_end)(DBDMA_io *);
struct DBDMA_io {
  void *opaque; void *channel; uint64_t addr; int len;
  int is_last; int is_dma_out; DBDMA_end dma_end;
};
#define FRAME_CNT_REG 5
typedef struct {
  int samples; unsigned shift; uint32_t wpos, rpos; uint8_t *mixbuf; DBDMA_io io;
  bool io_ended; unsigned regs[6];
} ScreamerState;
static uint8_t guest[64];
static int ends;
static void endfn(DBDMA_io *io) { (void)io; ends++; }
#define dma_memory_read(as, addr, buf, len, attr) \
  do { (void)(as); (void)(attr); memcpy((buf), guest + (size_t)(addr), (size_t)(len)); } while (0)
'''
ring_test = r'''
int main(void) {
  uint8_t mix[32];
  memset(mix, 0, sizeof(mix));
  for (int i = 0; i < 16; i++) guest[i] = (uint8_t)(0xA0 + i);
  ScreamerState s = {.samples=8,.shift=2,.wpos=6,.rpos=4,.mixbuf=mix,
                     .io={0,0,0,16,0,0,endfn}};
  pmac_screamer_tx_transfer(&s);
  assert(s.wpos==10 && s.io.len==0 && ends==1 && s.io_ended);
  assert(s.regs[FRAME_CNT_REG]==4);   /* compteur = trames lues par le DMA */
  assert(memcmp(mix + 24, guest, 8)==0);
  assert(memcmp(mix, guest + 8, 8)==0);
  ends=0; s.wpos=8; s.rpos=0; s.io.len=16; s.io.addr=0;
  pmac_screamer_tx_transfer(&s);
  assert(s.wpos==8 && s.io.len==16 && ends==0 && s.regs[FRAME_CNT_REG]==4); /* anneau plein */
  /* Longueur non multiple de 4 : le reliquat est consommé et dma_end part. */
  ends=0; s.wpos=0; s.rpos=0; s.io.len=10; s.io.addr=0;
  pmac_screamer_tx_transfer(&s);
  assert(s.wpos==2 && s.io.len==0 && ends==1);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='screamer-ring-') as d:
    c = Path(d)/'test.c'
    exe = Path(d)/'test'
    c.write_text(ring_stub + xfer + ring_test)
    subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# Écriture du compteur de trames (registre 5) : le pilote de Tiger le remet à 0
# au démarrage du moteur. Ignorée, elle décalait la tête d'effacement de
# l'invité par rapport au DMA (saccades de DOOM 3, 29/09/2026).
write_fn = source[source.index('static void screamer_write('):source.index('static const MemoryRegionOps screamer_ops')]
write_stub = r"""
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#define SCREAMER_DPRINTF(...) ((void)0)
#define HWADDR_PRIx PRIx64
#define HWADDR_FMT_plx "%016" PRIx64
#define LOG_UNIMP 0
#define SND_CTRL_REG 0
#define CODEC_CTRL_REG 1
#define CODEC_STAT_REG 2
#define CLIP_CNT_REG 3
#define BYTE_SWAP_REG 4
#define FRAME_CNT_REG 5
typedef uint64_t hwaddr;
typedef struct { uint32_t regs[6]; } ScreamerState;
static int unimp;
#define qemu_log_mask(m, ...) ((void)(m), unimp++)
static void screamer_control_write(ScreamerState *s, uint32_t v) { s->regs[0] = v; }
static void screamer_codec_write(ScreamerState *s, hwaddr a, uint64_t v) { (void)s; (void)a; (void)v; }
"""
write_test = r"""
int main(void) {
  ScreamerState s = {{0}};
  s.regs[FRAME_CNT_REG] = 123456;
  screamer_write(&s, FRAME_CNT_REG << 4, 0, 4);
  assert(s.regs[FRAME_CNT_REG] == 0 && unimp == 0);
  screamer_write(&s, FRAME_CNT_REG << 4, 77, 4);
  assert(s.regs[FRAME_CNT_REG] == 77);
  screamer_write(&s, 7 << 4, 1, 4);
  assert(unimp == 1);
  return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='screamer-fcw-') as d:
    c = Path(d)/'test.c'
    exe = Path(d)/'test'
    c.write_text(write_stub + write_fn + write_test)
    subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', '-Wno-unused-parameter', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Screamer : anneau, réserve, écritures partielles, silence, débit, compteur de trames, volume et profils audio OK')
