#!/usr/bin/env python3
"""Compile the real Screamer callback with a deterministic audio/DMA sink."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / 'patches/screamer/screamer.c').read_text()
callback = source[source.index('static void screamerspk_callback('):source.index('static void screamer_update_settings(')]
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
typedef struct { DBDMA_channel *channel; unsigned len; } DBDMA_io;
typedef struct { void *voice; unsigned samples,shift,wpos,rpos,regs[6]; uint8_t *mixbuf; DBDMA_io io; } ScreamerState;
static uint8_t output[64];
static unsigned written, limit = 64, kicks;
static size_t AUD_write(void *v, void *data, size_t n) {
  (void)v; n=MIN(n,limit); assert(written+n<=sizeof(output));
  memcpy(output+written,data,n); written+=n; return n;
}
static void pmac_screamer_tx_transfer(ScreamerState *s) {
  unsigned n=MIN(s->io.len, s->samples*4);
  memset(s->mixbuf,0x22,n); s->wpos=n/4; s->io.len-=n;
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
  assert(written==16 && s.rpos==0 && s.wpos==0 && s.regs[5]==4);
  for (unsigned i=0;i<16;i++) assert(output[i]==0x11);
  written=0; s.wpos=4; s.io.len=16; s.regs[5]=0;
  screamerspk_callback(&s,32);
  assert(written==32 && s.io.len==0 && s.regs[5]==8 && kicks==1);
  for (unsigned i=0;i<32;i++) assert(output[i]==(i<16?0x11:0x22));
  screamerspk_callback(&s,32); assert(written==32); // empty queue
  s.wpos=4; screamerspk_callback(&s,0); assert(written==32);
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
print('Screamer : écritures partielles, contre-pression, fragments DMA et profils audio OK')
