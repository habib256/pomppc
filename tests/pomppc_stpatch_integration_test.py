#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
"""Helper contre le vrai décodeur et stpatch_try réentrant, avec mutants (sans VM)."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
source = (ROOT / 'guest/gldriver/pomppc_accel.c').read_text()
start = source.index('static int stpatch_try(')
end = source.index('\n}\n', start) + 3
actual = source[start:end]
start = source.index('static unsigned long *reserve(PCtx *p, unsigned long words)\n{')
end = source.index('\n}\n', start) + 3
actual_reserve = source[start:end]

pre = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "qgpu-core.h"
#include "pomppc_stpatch.h"
typedef int TexInfo;
typedef struct PCtx {
    int st_valid, stp_valid;
    unsigned long qctx;
    unsigned long stp_gen;
    uint32_t stp_snap[POMPPC_STPATCH_WORDS];
    unsigned long st[QGPU_SK_COUNT];
} PCtx;
static struct {
    unsigned long ncmd, n_frames, n_stp_chk, n_stp_diff, n_stp_told;
    unsigned long n_stp_race, n_stp_take, n_stp_keys;
    unsigned long n_stp_cache, n_stp_header, n_stp_other, n_stp_enum;
    int stblk_check;
    int dead;
    PCtx *bound;
    unsigned long cmd[128];
} G;
static unsigned char st_is_hot[QGPU_SK_COUNT];
static unsigned long *cmd = G.cmd;
#define CMD_WORDS 128
static uint32_t fixture[POMPPC_STPATCH_WORDS];
static PCtx *owner;
static int reserve_action, reserve_calls, pending_run, pending_raw;
static int closed_run, closed_raw;
static void stpatch_snapshot(uint32_t *out, unsigned long fl, PCtx *p,
                             unsigned long sbits, const unsigned char *g)
{ (void)fl; (void)p; (void)sbits; (void)g; memcpy(out,fixture,sizeof(fixture)); }
static int state_ranges(int *lo,int *hi) { lo[0]=1;hi[0]=QGPU_SK_COUNT;return 1; }
static const int *hot_list(int *n) { *n=0;return NULL; }
static void gl_note(const char *fmt,...) { (void)fmt; }
static void compute_state(PCtx *p,const TexInfo *ti,unsigned long *out,int raw)
{
    QgpuState cur; uint32_t ref[QGPU_SK_COUNT];unsigned i;
    (void)ti;(void)raw;
    for(i=0;i<QGPU_SK_COUNT;i++)cur.v[i]=p->st[i];
    qgpu_state_block_compute(fixture,&cur,ref);
    for(i=0;i<QGPU_SK_COUNT;i++)out[i]=ref[i];
}
/* Le vrai reserve ferme les dessins différés AVANT de lier le contexte.
   Les fermetures restent des doubles ; le corps extrait impose leur ordre. */
static void stream_ready(void)
{
    reserve_calls++;
    if(reserve_action==1) {owner->stp_valid=0;owner->stp_gen++;}
    if(reserve_action==2) owner->stp_gen++;
    if(reserve_action==3) owner->st_valid=0;
}
static void close_run(void)
{ if(pending_run) {cmd[G.ncmd++]=0xd001;pending_run=0;closed_run++;} }
static void close_raw(void)
{ if(pending_raw) {cmd[G.ncmd++]=0xd002;pending_raw=0;closed_raw++;} }
static void flush(void) { assert(!"vidage inattendu du petit flux"); }
'''
post = r'''
static void init(PCtx *p)
{
    QgpuState cur;uint32_t ref[QGPU_SK_COUNT];unsigned i;
    memset(p,0,sizeof(*p));memset(&G,0,sizeof(G));
    p->st_valid=p->stp_valid=1;p->stp_gen=19;p->qctx=7;G.bound=p;
    p->stp_snap[0]=QGPU_SB_F_VALID|QGPU_SB_F_V7|QGPU_SB_F_STENCIL;
    p->stp_snap[1]=640;p->stp_snap[2]=480;p->stp_snap[3]=8;
    memcpy(fixture,p->stp_snap,sizeof(fixture));
    qgpu_state_init(&cur);qgpu_state_block_compute(fixture,&cur,ref);
    for(i=0;i<QGPU_SK_COUNT;i++)p->st[i]=ref[i];
    reserve_calls=reserve_action=pending_run=pending_raw=0;
    closed_run=closed_raw=0;owner=p;
}
static void delta(void)
{
    unsigned i=QGPU_SB_HDR+QGPU_SB_W0_N+QGPU_SB_W1_N+QGPU_SB_W2_N+
               (0x3174-QGPU_SB_W3_OFF)/4;
    fixture[i]=0x09000404; /* CW/FRONT au lieu de CCW/BACK */
}
int main(void)
{
    PCtx p;unsigned action;unsigned long gen;
    init(&p);delta();
    assert(stpatch_try(&p,NULL,1,fixture[0],8,NULL));
    assert(reserve_calls==1&&G.ncmd==6&&G.n_stp_take==1&&G.n_stp_keys==2);
    assert(cmd[1]==QGPU_SK_CULL_MODE&&cmd[2]==0x0404);
    assert(cmd[4]==QGPU_SK_FRONT_FACE&&cmd[5]==0x0900);
    assert(!memcmp(p.stp_snap,fixture,sizeof(fixture))&&p.stp_gen==20);
    /* Les vrais mots réservés sont rendus ; le CTX_BIND du reserve reste. */
    for(action=1;action<=3;action++) {
        init(&p);delta();reserve_action=action;G.bound=NULL;gen=p.stp_gen;
        assert(!stpatch_try(&p,NULL,1,fixture[0],8,NULL));
        assert(G.ncmd==2&&G.n_stp_race==1&&G.n_stp_take==0);
        assert(cmd[0]==QGPU_CMD_HDR(QGPU_OP_CTX_BIND,QGPU_LEN_CTX)&&cmd[1]==7&&cmd[2]==0);
        assert(memcmp(p.stp_snap,fixture,sizeof(fixture)));
        assert(p.stp_gen==gen+(action!=3));
    }
    /* Overrides froids (point/bitmap) : st_valid peut rester vrai, mais le
       snapshot explicitement invalidé doit suffire à imposer le bloc. */
    init(&p);delta();p.stp_valid=0;p.stp_gen++;
    assert(!stpatch_try(&p,NULL,1,fixture[0],8,NULL)&&!reserve_calls);
    init(&p);p.st_valid=0;
    assert(!stpatch_try(&p,NULL,1,fixture[0],8,NULL)&&!reserve_calls);
    /* Zéro clé conserve la frontière du bloc complet : ferme les dessins
       différés puis lie p, même si un autre contexte était courant. */
    init(&p);pending_run=pending_raw=1;G.bound=NULL;
    assert(stpatch_try(&p,NULL,1,fixture[0],8,NULL));
    assert(reserve_calls==1&&closed_run==1&&closed_raw==1&&G.bound==&p);
    assert(G.ncmd==4&&cmd[0]==0xd001&&cmd[1]==0xd002);
    assert(cmd[2]==QGPU_CMD_HDR(QGPU_OP_CTX_BIND,QGPU_LEN_CTX)&&cmd[3]==7);
    assert(G.n_stp_take==1&&G.n_stp_keys==0);
    for(action=1;action<=3;action++) {
        init(&p);reserve_action=action;pending_raw=1;G.bound=NULL;
        assert(!stpatch_try(&p,NULL,1,fixture[0],8,NULL));
        assert(reserve_calls==1&&closed_raw==1&&G.ncmd==3);
        assert(G.n_stp_race==1&&!G.n_stp_take&&G.bound==&p);
    }
    /* Sans dessin en attente ni changement de contexte, aucun mot émis. */
    init(&p);
    assert(stpatch_try(&p,NULL,1,fixture[0],8,NULL)&&reserve_calls==1&&!G.ncmd);
    assert(G.n_stp_take==1&&G.n_stp_keys==0);
    init(&p);G.stblk_check=1;
    assert(!stpatch_try(&p,NULL,1,fixture[0],8,NULL));
    assert(!reserve_calls&&G.n_stp_chk==1&&!G.n_stp_diff&&!G.n_stp_take);
    init(&p);delta();G.stblk_check=1;
    assert(!stpatch_try(&p,NULL,1,fixture[0],8,NULL));
    assert(!reserve_calls&&G.n_stp_chk==1&&!G.n_stp_diff);
    puts("STPATCH integration : delta, reserve reel, pertes de reference, overrides, frontiere RUN/RAW/CTX zero-cle et controle OK");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='pomppc-stpatch-integration-') as temp:
    temp = pathlib.Path(temp)
    common = ['cc', '-std=gnu11', '-O1', '-pthread', '-I', str(temp),
              '-I', str(ROOT / 'patches/qgpu'), '-I', str(ROOT / 'guest/gldriver')]
    objects = []
    for name in ('core', 'soft', 'gl'):
        obj = temp / (name + '.o')
        subprocess.run(common + ['-c', str(ROOT / ('patches/qgpu/qgpu-' + name + '.c')),
                                 '-o', str(obj)], check=True)
        objects.append(str(obj))
    libs = ['-lm']
    import sys
    if sys.platform == 'darwin':
        libs += ['-framework', 'OpenGL']
    else:
        import shlex
        try:
            flags = subprocess.run(['pkg-config', '--libs', 'egl', 'gl'], capture_output=True, text=True)
            if flags.returncode == 0:
                libs += shlex.split(flags.stdout)
        except FileNotFoundError:
            pass

    def run(name, body, killed=False):
        c = temp / (name + '.c')
        if killed:
            # Un mutant attendu en faute rend 1 sans lancer CrashReporter.
            body = body.replace('#include <assert.h>', '#include <assert.h>\n#include <stdlib.h>\n'
                                '#undef assert\n#define assert(x) do { if (!(x)) exit(1); } while (0)')
        c.write_text(body)
        binary = temp / name
        subprocess.run(common + [str(c)] + objects + libs + ['-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=killed, text=True)
        if killed:
            assert result.returncode != 0, 'mutant non détecté : ' + name
            print('STPATCH mutant détecté : ' + name)
        else:
            result.check_returncode()

    original = (ROOT / 'guest/gldriver/pomppc_stpatch.h').read_text()
    header = temp / 'pomppc_stpatch.h'
    header.write_text(original)
    unit = (ROOT / 'tests/pomppc_stpatch_test.c').read_text().replace(
        '#include "../guest/gldriver/pomppc_stpatch.h"', '#include "pomppc_stpatch.h"')
    run('oracle', unit)
    run('integration', pre + actual_reserve + actual + post)
    mutations = [
        ('ref-off', 'v[2] = on ? stp_word(a, 0x319c) & 0xff : 0;',
         'v[2] = on ? stp_word(a, 0x319c) & 0xff : 1;'),
        ('cull-byte', '& 0xffff00ffu)', '& 0u)'),
        ('back-front', 'v[6] = on ? stp_u16(a, 0x31a4) : QGPU_SOP_KEEP;',
         'v[6] = on ? stp_u16(a, 0x31bc) : QGPU_SOP_KEEP;'),
    ]
    for name, before, after in mutations:
        assert before in original
        header.write_text(original.replace(before, after, 1))
        run(name, unit, killed=True)
    header.write_text(original)
    assert 'p->stp_gen != gen' in actual
    run('reserve-generation', pre + actual_reserve + actual.replace('p->stp_gen != gen', '0', 1) + post, killed=True)
    assert 'G.ncmd -= patch.n * QGPU_LEN_SET_STATE;' in actual
    run('reserve-rollback', pre + actual_reserve + actual.replace('G.ncmd -= patch.n * QGPU_LEN_SET_STATE;', ';', 1) + post,
        killed=True)
    before = 'c = reserve(p, patch.n * QGPU_LEN_SET_STATE);'
    assert before in actual
    run('zero-reserve', pre + actual_reserve + actual.replace(before,
        'c = patch.n ? reserve(p, patch.n * QGPU_LEN_SET_STATE) : G.cmd + G.ncmd;', 1) + post,
        killed=True)
