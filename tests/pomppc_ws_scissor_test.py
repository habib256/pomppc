#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Vraies cles compute_state, region, snapshot/paquet et decodeur qgpu, sans VM."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
src = (ROOT/'guest/gldriver/pomppc_accel.c').read_text()
def function(signature):
    a=src.index(signature+'\n{');b=src.index('\n}\n',a)+3
    return src[a:b]
scissor=function('static void compute_scissor(PCtx *p, unsigned long *v)')
encode=function('static void stateblk_ws_scissor(PCtx *p, uint32_t *args)')
snapshot=function('static void stpatch_snapshot(uint32_t *a, unsigned long fl, PCtx *p,\n                             unsigned long sbits, const unsigned char *g)')
region=function('void pomppc_ws_region(void *ctx, const unsigned long *desc)')
a=src.index('static void compute_state(PCtx *p, const TexInfo *ti, unsigned long *v, int raw)\n{')
b=src.index('    /* textures : liaison',a)
state=src[a:b]+'}\n'
a=src.index('static void send_state_blk(PCtx *p, const TexInfo *ti, int raw)\n{')
a=src.index('        c[0] = QGPU_CMD_HDR(QGPU_OP_STATE_BLOCK,',a)
b=src.index('        if (G.stpatch) {',a)
payload=src[a:b]
a=src.index('static void send_state(PCtx *p, const TexInfo *ti, int raw)\n{')
a=src.index('    skip = ',a);b=src.index(';',a)+1
skip=src[a:b]
# Definitions GS_* are the real offsets. Models use native uint32 words,
# byte/halfword reads below emulate PPC big endian for native host execution.
import re
macros='\n'.join(re.findall(r'^#define GS_.*',re.sub(r'/\*.*?\*/','',src,flags=re.S),re.M))
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "qgpu-core.h"
#include "pomppc_stpatch.h"
typedef uint32_t UInt32;typedef int32_t SInt32;typedef int TexInfo;
#define KERN_SUCCESS 0
#define MD_VIEWPORT 1
static uint32_t gs[0x5000/4];
#define GLD_U32(p,o) (*(uint32_t *)((unsigned char *)(p)+(o)))
#define GLD_U8(p,o) ((GLD_U32(p,(o)&~3)>>((3-((o)&3))*8))&255)
#define U16(p,o) ((GLD_U32(p,(o)&~3)>>((2-((o)&2))*8))&65535)
#define I32(p,o) ((int32_t)GLD_U32(p,o))
typedef struct PCtx {
    unsigned long sw,sh,fullscreen_w,fullscreen_h;
    int fullscreen_buf,ws_rgn,st_valid,st_known,st_dirty,stp_valid;
    unsigned long ws_bx,ws_by,ws_bw,ws_bh,stp_gen,md_bits,c_vp[4];
    long ws_ox,ws_oy;
    int st_raw,st_stencil,stencil;
    unsigned long st_sw,st_sh,st_sbits;
    uint32_t stp_snap[POMPPC_STPATCH_WORDS];
} PCtx;
static struct {int stskip,stblk_check,mu;} G;
static struct {int open,conn;} WS={1,1};
static PCtx *owner;static uint32_t box[4];
static unsigned char *gls(PCtx *p) {(void)p;return (unsigned char *)gs;}
static PCtx *find_ctx(void *p) {return p==owner?owner:NULL;}
static void pthread_mutex_lock(int *p) {(void)p;}
static void pthread_mutex_unlock(int *p) {(void)p;}
static int IOConnectMethodScalarIScalarO(int c,int s,int n,int m,unsigned long d,UInt32 *x,UInt32 *y,UInt32 *w,UInt32 *h)
{(void)c;(void)s;(void)n;(void)m;(void)d;*x=box[0];*y=box[1];*w=box[2];*h=box[3];return 0;}
'''
post=r'''
static uint32_t packet[QGPU_LEN_STATE_BLOCK_CHECK];
static void emit_packet(PCtx *p)
{
    uint32_t *c=packet,*sbargs;int i;
    unsigned long fl=QGPU_SB_F_VALID|QGPU_SB_F_V7|QGPU_SB_F_V8|QGPU_SB_F_RAW,sbits=8;
    const unsigned char *g=gls(p);
PAYLOAD
}
static int should_skip(PCtx *p)
{
    int raw=1,skip;unsigned long sbits=8;
SKIP
    return skip;
}
static unsigned checks;
static void check(PCtx *p)
{
    uint32_t raw[POMPPC_STPATCH_WORDS],snap[POMPPC_STPATCH_WORDS];
    uint32_t before[QGPU_SK_COUNT],after[QGPU_SK_COUNT];
    unsigned long ref[QGPU_SK_COUNT];QgpuState cur;
    unsigned i,off=0;
    memset(raw,0,sizeof(raw));raw[0]=QGPU_SB_F_VALID|QGPU_SB_F_V7|QGPU_SB_F_V8|QGPU_SB_F_RAW;
    raw[1]=p->sw;raw[2]=p->sh;raw[3]=8;off=QGPU_SB_HDR;
    memcpy(raw+off,(unsigned char *)gs+QGPU_SB_W0_OFF,QGPU_SB_W0_N*4);off+=QGPU_SB_W0_N;
    memcpy(raw+off,(unsigned char *)gs+QGPU_SB_W1_OFF,QGPU_SB_W1_N*4);off+=QGPU_SB_W1_N;
    memcpy(raw+off,(unsigned char *)gs+QGPU_SB_W2_OFF,QGPU_SB_W2_N*4);off+=QGPU_SB_W2_N;
    memcpy(raw+off,(unsigned char *)gs+QGPU_SB_W3_OFF,QGPU_SB_W3_N*4);
    qgpu_state_init(&cur);cur.v[QGPU_SK_FOG_COLOR]=0x12345678;
    cur.v[QGPU_SK_POLY_FACTOR]=0x3f800000;cur.v[QGPU_SK_POLY_UNITS]=0x40000000;
    qgpu_state_block_compute(raw,&cur,before);
    stpatch_snapshot(snap,raw[0],p,8,gls(p));emit_packet(p);
    assert(!memcmp(snap,packet+1,sizeof(snap))); /* Les DEUX sites d'encodage. */
    for(i=0;i<POMPPC_STPATCH_WORDS;i++) {
        unsigned begin=QGPU_SB_HDR+QGPU_SB_W0_N+QGPU_SB_W1_N+QGPU_SB_W2_N;
        unsigned rect=begin+(GS_SCISSOR_RECT-QGPU_SB_W3_OFF)/4;
        unsigned enable=begin+(GS_SCISSOR-QGPU_SB_W3_OFF)/4;
        if(i!=enable&&(i<rect||i>rect+3))assert(snap[i]==raw[i]);
        if(i==enable)assert((snap[i]&0x00ffffffu)==(raw[i]&0x00ffffffu));
    }
    qgpu_state_block_compute(snap,&cur,after);
    compute_state(p,NULL,ref,1); /* Vrai prefixe compute_state jusqu'aux cles19. */
    for(i=QGPU_SK_SCISSOR;i<=QGPU_SK_SCISSOR_H;i++)assert(after[i]==ref[i]);
    for(i=1;i<QGPU_SK_COUNT;i++)if(i<QGPU_SK_SCISSOR||i>QGPU_SK_SCISSOR_H)assert(before[i]==after[i]);
    assert(after[QGPU_SK_FOG_COLOR]==cur.v[QGPU_SK_FOG_COLOR]);
    assert(after[QGPU_SK_POLY_FACTOR]==cur.v[QGPU_SK_POLY_FACTOR]);
    assert(after[QGPU_SK_POLY_UNITS]==cur.v[QGPU_SK_POLY_UNITS]);
    if(!p->ws_rgn)assert(!memcmp(raw,snap,sizeof(raw)));
    checks++;
}
static void ready(PCtx *p)
{
    p->st_valid=p->st_known=p->stp_valid=1;p->st_dirty=0;
    p->st_raw=1;p->st_sw=p->sw;p->st_sh=p->sh;p->st_sbits=8;
    p->st_stencil=p->stencil;G.stskip=1;
}
static void change(PCtx *p,int x,int y,int w,int h)
{
    unsigned long desc[3]={0,0,1},gen=p->stp_gen;
    box[0]=x;box[1]=y;box[2]=w;box[3]=h;
    ready(p);assert(should_skip(p));
    pomppc_ws_region(p,desc);
    assert(p->st_valid); /* Ne pas perdre les cles canoniques conservees. */
    assert(p->st_dirty&&!p->stp_valid&&p->stp_gen==gen+1&&!should_skip(p));
    check(p);
}
static void scissor_values(PCtx *p,int enabled,int x,int y,int w,int h,const uint32_t *want)
{
    unsigned long v[QGPU_SK_COUNT];
    gs[GS_SCISSOR/4]=enabled?0x010000a5:0x000000a5;
    gs[GS_SCISSOR_RECT/4]=x;gs[(GS_SCISSOR_RECT+4)/4]=y;
    gs[(GS_SCISSOR_RECT+8)/4]=w;gs[(GS_SCISSOR_RECT+12)/4]=h;
    compute_state(p,NULL,v,1);
    for(unsigned i=0;i<5;i++)assert(v[QGPU_SK_SCISSOR+i]==want[i]);
    check(p);
}
int main(void)
{
    PCtx p;unsigned long desc[3]={0,0,1},gen;
    const uint32_t off[]={1,10,5,80,40},inside[]={1,15,27,20,11};
    const uint32_t lower[]={1,10,34,15,11},upper[]={1,80,5,10,5},empty[]={1,128,0,0,0};
    memset(&p,0,sizeof(p));memset(gs,0,sizeof(gs));owner=&p;
    p.sw=p.fullscreen_w=128;p.sh=p.fullscreen_h=96;p.fullscreen_buf=1;
    change(&p,10,5,80,40);
    scissor_values(&p,0,0,0,0,0,off);
    uint32_t stable[POMPPC_STPATCH_WORDS];PomppcStatePatch stable_patch;
    stpatch_snapshot(stable,QGPU_SB_F_VALID|QGPU_SB_F_V7|QGPU_SB_F_V8|QGPU_SB_F_RAW,&p,8,gls(&p));
    assert(pomppc_stpatch_make(stable,stable,&stable_patch)&&!stable_patch.n);
    scissor_values(&p,1,5,7,20,11,inside);
    scissor_values(&p,1,-15,-9,30,20,lower);
    scissor_values(&p,1,70,35,30,30,upper);
    scissor_values(&p,1,1000,1000,5,5,empty);
    /* GS identique : origine, taille, bords et hauteur d'ecran changent. */
    change(&p,11,5,80,40);change(&p,11,6,80,40);
    change(&p,11,6,81,40);change(&p,11,6,81,41);
    p.sh=p.fullscreen_h=120;change(&p,11,6,81,41);
    /* Inchangée : aucun faux positif ni génération nouvelle. */
    ready(&p);gen=p.stp_gen;pomppc_ws_region(&p,desc);
    assert(!p.st_dirty&&p.stp_valid&&p.stp_gen==gen&&should_skip(&p));
    /* Sortie de WS -> bloc brut/STPATCH neuf, pas référence région périmée. */
    change(&p,0,0,128,120);assert(!p.ws_rgn);
    gs[GS_SCISSOR/4]=0;check(&p);
    uint32_t fresh[POMPPC_STPATCH_WORDS];PomppcStatePatch patch;
    stpatch_snapshot(fresh,QGPU_SB_F_VALID|QGPU_SB_F_V7|QGPU_SB_F_V8|QGPU_SB_F_RAW,&p,8,gls(&p));
    memcpy(p.stp_snap,fresh,sizeof(fresh));ready(&p);
    assert(pomppc_stpatch_make(p.stp_snap,fresh,&patch)&&!patch.n);
    /* Etendue bornees, ciseaux on/off, rectangle relatif parfois vide. */
    uint32_t rng=1;
    for(unsigned n=0;n<8192;n++) {
        rng=rng*1664525u+1013904223u;box[0]=(rng>>16)%160-16;
        rng=rng*1664525u+1013904223u;box[1]=(rng>>16)%140-16;
        rng=rng*1664525u+1013904223u;box[2]=(rng>>16)%180;
        rng=rng*1664525u+1013904223u;box[3]=(rng>>16)%160;
        pomppc_ws_region(&p,desc);
        for(unsigned j=0;j<4;j++){rng=rng*1664525u+1013904223u;gs[GS_SCISSOR_RECT/4+j]=(rng>>16)%512-128;}
        gs[GS_SCISSOR/4]=(n&1)?0x010000a5:0xa5;check(&p);
    }
    printf("WS SCISSOR : %u paquets/snapshots exacts contre decodeur reel, transitions region/GS identique et retour STPATCH OK\n",checks);
}
'''
with tempfile.TemporaryDirectory(prefix='pomppc-ws-scissor-') as tmp:
    tmp=pathlib.Path(tmp)
    flags=['cc','-std=gnu11','-O1','-pthread','-I',str(ROOT/'patches/qgpu'),'-I',str(ROOT/'guest/gldriver')]
    objects=[]
    for name in ('core','soft','gl'):
        obj=tmp/(name+'.o');subprocess.run(flags+['-c',str(ROOT/('patches/qgpu/qgpu-'+name+'.c')),'-o',str(obj)],check=True);objects.append(str(obj))
    libs=['-lm']
    if sys.platform=='darwin':libs+=['-framework','OpenGL']
    else:
        import shlex
        try:
            result=subprocess.run(['pkg-config','--libs','egl','gl'],capture_output=True,text=True)
            if result.returncode==0:libs+=shlex.split(result.stdout)
        except FileNotFoundError:pass
    def body(enc=encode,reg=region,pay=payload,snap=snapshot):
        return pre+macros+'\n'+scissor+enc+state+snap+reg+post.replace('PAYLOAD',pay).replace('SKIP',skip)
    def run(name,code,mutant=False):
        if mutant:code=code.replace('#include <assert.h>','#include <assert.h>\n#include <stdlib.h>\n#undef assert\n#define assert(x) do {if(!(x)) exit(1);} while(0)')
        c=tmp/(name+'.c');c.write_text(code);exe=tmp/name
        subprocess.run(flags+[str(c)]+objects+libs+['-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=mutant,text=True)
        assert result.returncode!=0 if mutant else result.returncode==0,name
        if mutant:print('WS SCISSOR mutant detecte : '+name)
    run('oracle',body())
    run('old-block',body(pay=payload.replace('stateblk_ws_scissor(p, (uint32_t *)sbargs);',';')),True)
    run('old-snapshot',body(snap=snapshot.replace('stateblk_ws_scissor(p, a);',';')),True)
    run('old-dirty',body(reg=region.replace('p->st_dirty = 1;',';')),True)
    run('old-stpatch-reference',body(reg=region.replace('p->stp_valid = 0;',';')),True)
    run('lost-canonical',body(reg=region.replace('p->st_dirty = 1;','p->st_dirty = 1; p->st_valid = 0;')),True)
