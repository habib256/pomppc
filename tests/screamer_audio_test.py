#!/usr/bin/env python3
# GPL3 - Copyleft VERHILLE Arnaud
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
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define FRAME_CNT_REG 5
#define DBDMA_STATUS 0
#define RUN 1
#define SCREAMER_DPRINTF(...) ((void)0)
typedef struct { unsigned regs[1]; int channel; } DBDMA_channel;
typedef struct { DBDMA_channel channels[1]; } DBDMAState;
#define container_of(p,t,m) ((t *)(p))
typedef struct { DBDMA_channel *channel; int len; } DBDMA_io;
typedef struct {
  void *voice; int samples; unsigned shift;
  uint32_t wpos, rpos, rate; int running;
  unsigned regs[6]; uint8_t *mixbuf; DBDMA_io io;
} ScreamerState;
static uint8_t output[512];
static unsigned written, limit = 64, kicks;
static size_t AUD_write(void *v, void *data, size_t n) {
  (void)v; n=MIN(n,limit); assert(written+n<=sizeof(output));
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
  s->io.len -= frames << s->shift;
}
static void DBDMA_kick(DBDMAState *s) { (void)s; ++kicks; }
'''
test = r'''
int main(void) {
  uint8_t pcm[16]; memset(pcm,0x11,sizeof(pcm));
  DBDMA_channel channel={{RUN},0};
  ScreamerState s={.samples=4,.shift=2,.wpos=4,.mixbuf=pcm,.io={&channel,0}};
  limit=4; screamerspk_callback(&s,16);
  assert(written==4 && s.rpos==1 && s.regs[5]==1); // no discarded samples
  limit=0; screamerspk_callback(&s,12);
  assert(written==4 && s.rpos==1); // backpressure must not spin or advance
  limit=64; screamerspk_callback(&s,12);
  assert(written==16 && s.rpos==4 && s.wpos==4 && s.regs[5]==4);
  for (unsigned i=0;i<16;i++) assert(output[i]==0x11);
  written=0; s.rpos=0; s.wpos=4; s.io.len=16; s.regs[5]=0;
  screamerspk_callback(&s,32);
  assert(written==32 && s.io.len==0 && s.regs[5]==8 && kicks==1);
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

  uint8_t ring[256];
  memset(ring, 0x33, sizeof(ring));
  ScreamerState w={.samples=64,.shift=2,.rate=44100,.wpos=16,.mixbuf=ring,.io={&channel,0}};
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
  ScreamerState wr={.samples=4,.shift=2,.wpos=5,.rpos=3,.mixbuf=wrapb,.io={&channel,0}};
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
typedef struct { void *voice; uint32_t rate; uint32_t regs[1]; uint32_t codec_ctrl_regs[8]; } ScreamerState;
static int settings_calls;
static void screamer_update_settings(ScreamerState *s) { (void)s; settings_calls++; }
static int vol_mute, vol_l, vol_r, vol_calls;
static void AUD_set_volume_out(void *v, int mute, int l, int r) {
  (void)v; vol_mute=mute; vol_l=l; vol_r=r; vol_calls++;
}
'''
reg_test = r'''
int main(void) {
  ScreamerState s = {0};
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
xfer = source[source.index('static void pmac_screamer_tx_transfer('):source.index('static void pmac_screamer_tx(')]
ring_stub = r'''
#include <assert.h>
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
typedef struct {
  int samples; unsigned shift; uint32_t wpos, rpos; uint8_t *mixbuf; DBDMA_io io;
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
  assert(s.wpos==10 && s.io.len==0 && ends==1);
  assert(memcmp(mix + 24, guest, 8)==0);
  assert(memcmp(mix, guest + 8, 8)==0);
  ends=0; s.wpos=8; s.rpos=0; s.io.len=16; s.io.addr=0;
  pmac_screamer_tx_transfer(&s);
  assert(s.wpos==8 && s.io.len==16 && ends==0);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='screamer-ring-') as d:
    c = Path(d)/'test.c'
    exe = Path(d)/'test'
    c.write_text(ring_stub + xfer + ring_test)
    subprocess.run([os.environ.get('CC','cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Screamer : anneau, réserve, écritures partielles, silence, débit, volume et profils audio OK')
