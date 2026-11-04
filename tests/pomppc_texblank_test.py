#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Niveaux vierges : vraie branche plugin et vrai decodeur, sans VM."""
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / 'guest/gldriver/pomppc_accel.c').read_text()
def function(name):
    start = source.index('static ', source.index(name) - 30)
    end = source.index('\n}\n', start) + 3
    return source[start:end]
helper = function('int texture_zero_argb(')
byte_count = function('unsigned long host_texel_bytes(')
start = source.index('            if (G.v10) {', source.index('if (t->dirty && !tex_is_cube(t))'))
end = source.index('                continue;\n            }', start) + len('                continue;\n            }')
branch = source[start:end]
macros = '\n'.join(re.findall(r'^#define LV_.*', source, re.M))
pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include "qgpu-core.h"
#define U16(p,o) (*(unsigned short *)((unsigned char *)(p)+(o)))
#define S16(p,o) (*(short *)((unsigned char *)(p)+(o)))
#define GLD_U32(p,o) (*(unsigned long *)((unsigned char *)(p)+(o)))
typedef int PCtx;
typedef struct {long qtex; int host_only;} PTex;
static struct {int v10,tex14; struct {unsigned char *win;unsigned long base;} q;
    unsigned long n_texuploads,n_texblack;} G;
static unsigned long command[QGPU_LEN_TEX_IMAGE3];
static unsigned long capacity,allocated;
static int allocations,refused,upload_blank;
static sigjmp_buf sig_jmp;
static int sig_jmp_on;static unsigned long sig_thr;
static int no(int why,unsigned long a,unsigned long b) {(void)why;(void)a;(void)b;refused++;return 0;}
#define NO_TEX_SIZE 1
#define NO_TEX_FORMAT 2
static int arena_alloc(unsigned long n,unsigned long *off)
{allocations++;if(n>capacity)return 0;*off=4096;allocated=n;return 1;}
static unsigned long *reserve(PCtx *p,unsigned long n) {(void)p;assert(n==QGPU_LEN_TEX_IMAGE3);return command;}
static int dxt_format(unsigned f,unsigned t) {return !t&&f>=0x83f0&&f<=0x83f3;}
static int dxt_ok(unsigned b,unsigned f) {(void)b;(void)f;return 1;}
static unsigned long dxt_bytes(unsigned f,unsigned long w,unsigned long h) {(void)f;return ((w+3)/4)*((h+3)/4)*8;}
static unsigned long host_base(unsigned long b) {return b;}
static int depth_pair_ok(unsigned long b,unsigned f) {return (b==0x1902)==(f==0x1902);}
static void tex_level_depth(PTex *t,int l,unsigned long *d,unsigned long *h) {(void)t;(void)l;*d=2;(void)h;}
static void crash_hook_fresh(void) {}
static unsigned long self_thr(void) {return 0;}
static void gl_note(const char *f,...) {(void)f;}
static int pomppc_tracing(void) {return 0;}
static int pomppc_ws_process(void) {return 0;}
static void pomppc_log(const char *f,...) {(void)f;}
'''
wrapper = r'''
static int emit(unsigned long w,unsigned long h,unsigned fmt,unsigned type,
                int blank,int t3,int rect,unsigned base,int pad)
{
    unsigned char level[64]={0},*lv=level;
    PCtx ctx=0,*p=&ctx;PTex tex={1,0},*t=&tex;unsigned long off=0,*c;
    upload_blank=blank;G.v10=G.tex14=1;allocations=refused=0;allocated=0;
    memset(command,0,sizeof(command));
    U16(lv,LV_W)=w;U16(lv,LV_H)=h;U16(lv,LV_ROWPIX)=w+pad;
    U16(lv,LV_FORMAT)=fmt;U16(lv,LV_TYPE)=type;
    GLD_U32(lv,LV_DATA)=(unsigned long)(G.q.win+4096);
    for(int l=(rect?0:2);l<(rect?1:3);l++) {
BRANCH
    }
    return 1;
}
'''
post = r'''
static void execute(QgpuCore *core,const unsigned long *words,unsigned n)
{
    for(unsigned i=0;i<n;i++)qgpu_st32(G.q.win+64+4*i,(uint32_t)words[i]);
    assert(qgpu_core_execute(core,64,n*4)==QGPU_ST_OK);
}
int main(void)
{
    const unsigned types[]={0x1401,0x8035,0x8367,0x8033,0x8034,0x8365,0x8366};
    const unsigned formats[]={0x1908,0x80e1};
    QgpuCore *core=calloc(1,sizeof(*core));assert(core);
    G.q.win=calloc(1,16u<<20);assert(G.q.win);G.q.base=0;capacity=0x5ff000;
    assert(qgpu_core_init(core,"soft",G.q.win,16u<<20));
    for(unsigned f=0;f<2;f++)for(unsigned t=0;t<7;t++)for(unsigned rect=0;rect<2;rect++)for(unsigned base=0x1907;base<=0x1908;base++) {
        unsigned long nodata[QGPU_LEN_TEX_IMAGE3],zeros[QGPU_LEN_TEX_IMAGE3];
        unsigned target=rect?QGPU_TT_RECTANGLE:QGPU_TT_2D;
        unsigned long create[]={QGPU_CMD_HDR(QGPU_OP_TEX_CREATE3,QGPU_LEN_TEX_CREATE3),1,target};
        execute(core,create,3);create[1]=2;execute(core,create,3);
        assert(emit(13,7,formats[f],types[t],1,0,rect,base,3));
        assert(!allocations&&!refused&&command[10]==QGPU_TEX_NO_DATA);
        memcpy(nodata,command,sizeof(nodata));memcpy(zeros,command,sizeof(zeros));
        zeros[1]=2;zeros[10]=4096;memset(G.q.win+4096,0,4096);
        execute(core,nodata,QGPU_LEN_TEX_IMAGE3);execute(core,zeros,QGPU_LEN_TEX_IMAGE3);
        assert(!memcmp(qgpu_core_tex_px(core,1,0,rect?0:2),qgpu_core_tex_px(core,2,0,rect?0:2),13*7*4));
        for(unsigned i=0;i<QGPU_LEN_TEX_IMAGE3;i++)if(i!=1&&i!=10)assert(nodata[i]==zeros[i]);
        unsigned long destroy[]={QGPU_CMD_HDR(QGPU_OP_TEX_DESTROY,2),1};
        execute(core,destroy,2);destroy[1]=2;execute(core,destroy,2);
    }
    /* L'ancien chemin demandait >6 Mio. Aucun transfert ni lecture source. */
    assert(emit(1920,1080,0x80e1,0x8367,1,0,1,0x1908,0));
    assert(!allocations&&!refused&&command[10]==QGPU_TEX_NO_DATA);
    /* Niveaux ordinaires et formats non prouvés : chemin précédent intact. */
    assert(emit(8,8,0x1908,0x1401,0,0,0,0x1908,0));assert(allocations==1&&command[10]==4096);
    assert(emit(8,8,0x1907,0x1401,1,0,0,0x1907,0));assert(allocations==1&&command[10]==4096);
    assert(emit(8,8,0x1902,0x1405,1,0,0,0x1902,0));assert(allocations==1&&command[10]==4096);
    assert(emit(8,8,0x1908,0x1401,1,1,0,0x1908,0));assert(allocations==1&&command[10]==4096);
    assert(emit(8,8,0x83f1,0,1,0,0,0x1908,0));assert(allocations==1&&command[10]==4096);
    assert(!emit(1920,1080,0x1908,0x1401,0,0,1,0x1908,0));assert(allocations==1&&refused==1);
    assert(!emit(1920,1080,0x1907,0x1401,1,0,1,0x1907,2048));assert(allocations==1&&refused==1);
    assert(!texture_zero_argb(0x83f1,0)&&!texture_zero_argb(0x1902,0x1405));
    assert(!texture_zero_argb(0x1908,0x1406));
    qgpu_core_fini(core);free(core);free(G.q.win);
    puts("TEXBLANK : 56 conversions exactes contre vrai core, commande identique sauf off, 1080 sans arene, refus/formats precedents preserves");
}
'''
with tempfile.TemporaryDirectory(prefix='pomppc-texblank-') as tmp:
    tmp=pathlib.Path(tmp)
    common=['cc','-std=gnu11','-O1','-pthread','-I',str(ROOT/'patches/qgpu')]
    objects=[]
    for name in ('core','soft','gl'):
        obj=tmp/(name+'.o')
        subprocess.run(common+['-c',str(ROOT/('patches/qgpu/qgpu-'+name+'.c')),'-o',str(obj)],check=True)
        objects.append(str(obj))
    libs=['-lm']
    if sys.platform=='darwin': libs+=['-framework','OpenGL']
    else:
        import shlex
        try:
            flags=subprocess.run(['pkg-config','--libs','egl','gl'],capture_output=True,text=True)
            if flags.returncode==0: libs+=shlex.split(flags.stdout)
        except FileNotFoundError:
            pass
    def run(name,code,negative=False):
        if negative: code=code.replace('#include <assert.h>','#include <assert.h>\n#include <stdlib.h>\n#undef assert\n#define assert(x) do {if(!(x)) exit(1);} while(0)')
        c=tmp/(name+'.c');c.write_text(code);exe=tmp/name
        subprocess.run(common+[str(c)]+objects+libs+['-o',str(exe)],check=True)
        r=subprocess.run([str(exe)],capture_output=negative,text=True)
        assert (r.returncode!=0) if negative else (r.returncode==0),name
        if negative: print('TEXBLANK mutant detecte : '+name)
    def body(b,h=helper):return pre+macros+'\n'+byte_count+h+wrapper.replace('BRANCH',b)+post
    old=branch.replace('zero_level = upload_blank && !t3 && texture_zero_argb(fmt, type);','zero_level = 0;')
    negative_size=r'''
int main(void) {
    G.q.win=calloc(1,16u<<20);assert(G.q.win);capacity=0x5ff000;
    assert(!emit(1920,1080,0x80e1,0x8367,1,0,1,0x1908,0));
    assert(allocations==1&&refused==1&&!G.n_texuploads);
    free(G.q.win);
    puts("TEXBLANK avant : vrai chemin allocation refuse 1920x1080 dans arene 0x5ff000, aucune commande");
}
'''
    run('before-size',body(old).replace(post,negative_size))
    run('texblank',body(branch))
    run('old-arena',body(old),True)
    run('all-formats',body(branch,helper.replace('if (fmt != 0x1908 && fmt != 0x80E1)','if (0)')),True)
    run('ordinary-level',body(branch.replace('zero_level = upload_blank && !t3','zero_level = !t3')),True)
