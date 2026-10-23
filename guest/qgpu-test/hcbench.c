/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * hcbench.c — micro-banc de la page d'appel synchrone (QGPU_CAP_HOSTCALL),
 * phase 0 du plugin hôte (docs/plan-plugin-hote.md §3 et §5). Demande un
 * device lancé avec x-hostcall=on et le kext du 23/10/2026.
 *
 *   hcbench bench       coût d'un appel vide (page d'appel), d'une fonction
 *                       invitée de ~200 instructions, d'un aller-retour par le
 *                       kext (QGPU_UC_READ_REG) et de la sonnette actuelle
 *                       (QGPU_UC_SUBMIT synchrone d'un NOP) ; débit de
 *                       QGPU_HC_READ_VA (64 o, 1 Kio, 64 Kio sur 17 pages)
 *                       contre memcpy de l'invité ; replis (page jamais
 *                       touchée, adresse nulle) ; lecture de 256 Kio
 *                       aléatoires comparée octet à octet.
 *   hcbench ctx K N     ouvrier de l'épreuve de changement de processus : N
 *                       fois, réécrit 4 pages À LA MÊME ADRESSE VIRTUELLE que
 *                       les autres ouvriers (motif propre à K et au tour),
 *                       cède le processeur (sched_yield), puis relit ces pages
 *                       par la page d'appel et compare. Trois ouvriers en
 *                       même temps sur deux vCPU : chaque lecture suit un
 *                       changement de contexte probable.
 *
 * Code de sortie : 0 tout juste, 1 écart ou statut inattendu, 2 pas de page
 * d'appel (device sans x-hostcall, kext ancien).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <IOKit/IOKitLib.h>

#include "qgpu_proto.h"

static io_connect_t conn;
static unsigned char *win;              /* notre tranche de BAR0 */
static unsigned int slot_base, slot_size;
static volatile unsigned int *callp;    /* la page d'appel */
static volatile unsigned int *blk;      /* bloc d'appel, au début de la tranche + 64 Kio */
static unsigned int blk_abs;
static double us_per_tick;
static int bad;

#define BLK_OFF   0x10000u              /* bloc d'appel dans la tranche */
#define DST_OFF   0x100000u             /* destination des lectures */
#define NOP_OFF   0x0u                  /* flux NOP de la sonnette */

static uint64_t now(void) { return mach_absolute_time(); }
static double us(uint64_t t) { return t * us_per_tick; }

static int open_client(void)
{
    mach_port_t master = MACH_PORT_NULL;
    io_service_t svc;
    vm_address_t addr = 0;
    vm_size_t size = 0;
    unsigned int version, caps, slotsz, fence, idx, base, cb, sb;
    kern_return_t kr;
    mach_timebase_info_data_t tb;

    mach_timebase_info(&tb);
    us_per_tick = (double)tb.numer / tb.denom / 1000.0;
    IOMasterPort(MACH_PORT_NULL, &master);
    svc = IOServiceGetMatchingService(master, IOServiceMatching("POMPPCGPU"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
        printf("hcbench : POMPPCGPU absent ou ouverture refusée\n");
        return -1;
    }
    IOObjectRelease(svc);
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_INFO, 0, 4,
                                       &version, &caps, &slotsz, &fence);
    if (kr != KERN_SUCCESS || !(caps & QGPU_CAP_HOSTCALL)) {
        printf("hcbench : device sans QGPU_CAP_HOSTCALL (caps 0x%x) : x-hostcall=on ?\n", caps);
        return -1;
    }
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_SLOT, 0, 4, &idx, &base, &cb, &sb);
    if (kr != KERN_SUCCESS)
        return -1;
    kr = IOConnectMapMemory(conn, QGPU_UC_MEM_SHMEM, mach_task_self(), &addr, &size,
                            kIOMapAnywhere);
    if (kr != KERN_SUCCESS)
        return -1;
    win = (unsigned char *)addr;
    slot_base = base;
    slot_size = slotsz;
    addr = 0;
    size = 0;
    kr = IOConnectMapMemory(conn, QGPU_UC_MEM_CALL, mach_task_self(), &addr, &size,
                            kIOMapAnywhere | kIOMapInhibitCache);
    if (kr != KERN_SUCCESS || !addr) {
        printf("hcbench : page d'appel refusée par le kext (0x%x)\n", kr);
        return -1;
    }
    callp = (volatile unsigned int *)addr;
    blk = (volatile unsigned int *)(win + BLK_OFF);
    blk_abs = slot_base + BLK_OFF;
    printf("hcbench : tranche %u (base 0x%x, %u Kio), page d'appel à %p (%lu o)\n",
           idx, slot_base, slot_size >> 10, (void *)callp, (unsigned long)size);
    return 0;
}

static unsigned int call(void)
{
    blk[QGPU_HC_OFF_ST / 4] = 0xdeadbeef;
    *callp = blk_abs;                   /* rendu quand le stw rend */
    return blk[QGPU_HC_OFF_ST / 4];
}

static unsigned int read_va(const void *va, unsigned int len, unsigned int dst_off,
                            unsigned int *done)
{
    unsigned int st;
    blk[QGPU_HC_OFF_OP / 4] = QGPU_HC_READ_VA;
    blk[QGPU_HC_OFF_A0 / 4] = (unsigned int)va;
    blk[QGPU_HC_OFF_A1 / 4] = len;
    blk[QGPU_HC_OFF_A2 / 4] = slot_base + dst_off;
    st = call();
    if (done)
        *done = blk[QGPU_HC_OFF_A3 / 4];
    return st;
}

/* ~200 instructions invitées (5 par tour, 40 tours, plus l'entrée) : l'étalon
   « petit appel de fonction » du §3. otool -tv hcbench pour le compte exact. */
__attribute__((noinline)) unsigned int f200(unsigned int x)
{
    int i;
    for (i = 0; i < 40; i++)
        x = (x * 33u + (unsigned int)i) ^ (x >> 3);
    return x;
}

static void bench(void)
{
    unsigned int i, n, st, done, sink = 0;
    uint64_t t0, t1;
    unsigned int fence, status, pc, v;
    unsigned char *buf, *mbuf, *page;
    vm_address_t a = 0;
    static const struct { unsigned int len, off, reps; } rd[] = {
        { 64, 100, 20000 }, { 1024, 3500, 10000 }, { 4096, 0, 5000 }, { 65536, 1000, 1000 },
    };

    /* 1. appel vide */
    n = 200000;
    blk[QGPU_HC_OFF_OP / 4] = QGPU_HC_NOP;
    st = call();
    t0 = now();
    for (i = 0; i < n; i++)
        *callp = blk_abs;
    t1 = now();
    if (st != QGPU_HC_ST_OK || blk[QGPU_HC_OFF_ST / 4] != QGPU_HC_ST_OK) {
        printf("NOP : statut %u\n", st);
        bad = 1;
    }
    printf("NOP page d'appel        : %.3f µs/appel (%u appels ; gestionnaire %.3f µs)\n",
           us(t1 - t0) / n, n, blk[QGPU_HC_OFF_NS / 4] / 1000.0);
    t0 = now();
    for (i = 0; i < n; i++)
        sink += f200(i);
    t1 = now();
    printf("fonction invitée f200   : %.3f µs/appel\n", us(t1 - t0) / n);
    t0 = now();
    for (i = 0; i < n; i++)
        sink += *(volatile unsigned int *)(win + BLK_OFF + 64);
    t1 = now();
    printf("boucle vide (lecture BAR0) : %.3f µs/tour\n", us(t1 - t0) / n);
    /* 1 bis. appel FROID : un appel isolé après un passage par le noyau
       (getppid : changement de segments), ou après un balayage de 4 Mio qui
       chasse les entrées du TLB de TCG. Le coût des deux lectures d'heure est
       mesuré à part et retiré. */
    {
        uint64_t ta = 0, tb = 0, tc = 0, td = 0, te = 0, x;
        unsigned char *sweep = malloc(0x400000);
        unsigned int m = 2000, j;
        memset(sweep, 1, 0x400000);
        for (i = 0; i < m; i++) {
            getppid();
            x = now(); ta += now() - x;                 /* deux lectures d'heure */
            getppid();
            x = now(); *callp = blk_abs; tb += now() - x;
            getppid();
            x = now(); sink += f200(i); tc += now() - x;
            for (j = 0; j < 0x400000; j += 4096)
                sink += sweep[j];
            x = now(); *callp = blk_abs; td += now() - x;
            for (j = 0; j < 0x400000; j += 4096)
                sink += sweep[j];
            x = now(); sink += f200(i); te += now() - x;
        }
        printf("NOP froid après getppid : %.3f µs (f200 froide %.3f µs) ; après balayage de "
               "4 Mio : %.3f µs (f200 %.3f µs) ; lecture d'heure retirée %.3f µs\n",
               us(tb - ta) / m, us(tc - ta) / m, us(td - ta) / m, us(te - ta) / m, us(ta) / m);
        free(sweep);
    }

    /* 2. le kext : un aller-retour sans doorbell, puis la sonnette actuelle */
    n = 20000;
    t0 = now();
    for (i = 0; i < n; i++)
        IOConnectMethodScalarIScalarO(conn, QGPU_UC_READ_REG, 1, 1, QGPU_REG_VERSION, &v);
    t1 = now();
    printf("kext READ_REG           : %.3f µs/appel\n", us(t1 - t0) / n);
    ((unsigned int *)(win + NOP_OFF))[0] = QGPU_CMD_HDR(QGPU_OP_NOP, QGPU_LEN_NOP);
    n = 5000;
    t0 = now();
    for (i = 0; i < n; i++)
        IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3, NOP_OFF, 4u,
                                      &fence, &status, &pc);
    t1 = now();
    printf("sonnette SUBMIT NOP sync: %.3f µs/appel (statut %u)\n", us(t1 - t0) / n, status);

    /* 3. LIRE_VA contre memcpy */
    if (vm_allocate(mach_task_self(), &a, 0x80000, TRUE) != KERN_SUCCESS)
        return;
    buf = (unsigned char *)a;
    for (i = 0; i < 0x80000; i++)
        buf[i] = (unsigned char)(rand() >> 7);
    mbuf = malloc(0x80000);
    memset(mbuf, 0, 0x80000);
    for (i = 0; i < sizeof(rd) / sizeof(rd[0]); i++) {
        unsigned int k, len = rd[i].len, reps = rd[i].reps;
        const unsigned char *src = buf + rd[i].off;
        double tn, tg, tw;
        st = read_va(src, len, DST_OFF, &done);
        if (st != QGPU_HC_ST_OK || done != len || memcmp(win + DST_OFF, src, len)) {
            printf("READ_VA %u : statut %u, %u octets, contenu %s\n", len, st, done,
                   memcmp(win + DST_OFF, src, len) ? "FAUX" : "juste");
            bad = 1;
        }
        t0 = now();
        for (k = 0; k < reps; k++)
            read_va(src, len, DST_OFF, 0);
        t1 = now();
        tn = us(t1 - t0) / reps;
        t0 = now();
        for (k = 0; k < reps; k++)
            memcpy(mbuf + (k & 1) * 0x20000, src, len);
        t1 = now();
        tg = us(t1 - t0) / reps;
        t0 = now();
        for (k = 0; k < reps; k++)
            memcpy(win + DST_OFF, src, len);
        t1 = now();
        tw = us(t1 - t0) / reps;
        printf("READ_VA %6u o (%2u pages) : natif %.3f µs (%.0f Mo/s ; gestionnaire %.3f µs),"
               " memcpy invité %.3f µs (%.0f Mo/s), memcpy vers BAR0 %.3f µs, x%.1f\n", len,
               blk[QGPU_HC_OFF_PAGES / 4], tn, len / tn, blk[QGPU_HC_OFF_NS / 4] / 1000.0,
               tg, len / tg, tw, tg / tn);
    }

    /* 4. replis : page jamais touchée, adresse nulle, puis la page touchée */
    a = 0;
    vm_allocate(mach_task_self(), &a, 0x1000, TRUE);
    page = (unsigned char *)a;
    st = read_va(page, 64, DST_OFF, &done);
    printf("repli : page jamais touchée -> statut %u (%u octets)%s\n", st, done,
           st == QGPU_HC_ST_FALLBACK ? "" : "  (attendu : 1, repli)");
    st = read_va((void *)0, 64, DST_OFF, &done);
    printf("repli : adresse nulle -> statut %u (%u octets)\n", st, done);
    if (st != QGPU_HC_ST_FALLBACK)
        bad = 1;
    page[5] = 0x5a;
    st = read_va(page, 64, DST_OFF, &done);
    printf("page touchée -> statut %u, octet 5 = 0x%02x\n", st, win[DST_OFF + 5]);
    if (st != QGPU_HC_ST_OK || win[DST_OFF + 5] != 0x5a)
        bad = 1;

    /* 5. 256 Kio aléatoires (64 pages, au-delà du cache de 8) d'un coup */
    st = read_va(buf + 7, 0x40000, DST_OFF, &done);
    printf("lecture 256 Kio : statut %u, %u octets, %u pages traduites, contenu %s\n",
           st, done, blk[QGPU_HC_OFF_PAGES / 4],
           memcmp(win + DST_OFF, buf + 7, 0x40000) ? "FAUX" : "juste");
    if (st != QGPU_HC_ST_OK || memcmp(win + DST_OFF, buf + 7, 0x40000))
        bad = 1;
    /* 6. CMP_LIST : égal, puis un octet changé */
    memcpy(mbuf, buf, 4096);
    blk[QGPU_HC_OFF_OP / 4] = QGPU_HC_CMP_LIST;
    blk[QGPU_HC_OFF_A0 / 4] = 2;
    blk[QGPU_HC_OFF_A1 / 4] = 3;
    blk[QGPU_HC_OFF_LIST / 4 + 0] = (unsigned int)buf;
    blk[QGPU_HC_OFF_LIST / 4 + 1] = (unsigned int)mbuf;
    blk[QGPU_HC_OFF_LIST / 4 + 2] = 4096;
    blk[QGPU_HC_OFF_LIST / 4 + 3] = (unsigned int)(buf + 4000);
    blk[QGPU_HC_OFF_LIST / 4 + 4] = (unsigned int)(mbuf + 4000);
    blk[QGPU_HC_OFF_LIST / 4 + 5] = 96;
    st = call();
    v = blk[QGPU_HC_OFF_A2 / 4];
    mbuf[4050] ^= 1;
    call();
    printf("CMP_LIST : égal -> statut %u masque %x ; un octet changé -> masque %x\n",
           st, v, blk[QGPU_HC_OFF_A2 / 4]);
    if (st != QGPU_HC_ST_OK || v != 0 || blk[QGPU_HC_OFF_A2 / 4] != 3)
        bad = 1;
    if (sink == 1)
        printf(" ");
}

static int ctx_worker(unsigned int k, unsigned int n)
{
    vm_address_t a = 0x50000000;
    unsigned int *w, it, j, st, done, wrong = 0, fall = 0;
    task_events_info_data_t ev0, ev1;
    mach_msg_type_number_t cnt;
    uint64_t t0, t1;

    if (vm_allocate(mach_task_self(), &a, 0x4000, FALSE) != KERN_SUCCESS) {
        a = 0;
        if (vm_allocate(mach_task_self(), &a, 0x4000, TRUE) != KERN_SUCCESS)
            return 1;
    }
    w = (unsigned int *)a;
    cnt = TASK_EVENTS_INFO_COUNT;
    memset(&ev0, 0, sizeof(ev0));
    task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ev0, &cnt);
    t0 = now();
    for (it = 0; it < n; it++) {
        for (j = 0; j < 0x1000; j++)
            w[j] = (k << 28) ^ (it << 12) ^ j;
        sched_yield();                  /* un autre ouvrier passe, probablement */
        st = read_va(w, 0x4000, DST_OFF, &done);
        if (st == QGPU_HC_ST_FALLBACK) {
            fall++;
            continue;
        }
        if (st != QGPU_HC_ST_OK || memcmp(win + DST_OFF, w, 0x4000)) {
            if (wrong++ < 3)
                printf("ctx %u tour %u : statut %u, mot 0 lu %08x attendu %08x\n", k, it, st,
                       *(unsigned int *)(win + DST_OFF), w[0]);
        }
    }
    t1 = now();
    cnt = TASK_EVENTS_INFO_COUNT;
    memset(&ev1, 0, sizeof(ev1));
    task_info(mach_task_self(), TASK_EVENTS_INFO, (task_info_t)&ev1, &cnt);
    /* csw : changements de contexte de la tâche (Mach) ; l'intervalle
       [début, fin] en ms d'horloge commune montre que les ouvriers se
       chevauchent (3 processus prêts pour 2 vCPU). */
    printf("ctx %u : va %p, %u lectures de 4 pages, %u fausses, %u replis, "
           "%ld changements de contexte, de %.1f à %.1f ms\n", k, (void *)w, n, wrong,
           fall, (long)(ev1.csw - ev0.csw), us(t0) / 1000, us(t1) / 1000);
    return wrong ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage : hcbench bench | ctx K N\n");
        return 2;
    }
    if (open_client() != 0)
        return 2;
    if (!strcmp(argv[1], "bench")) {
        bench();
        return bad;
    }
    if (!strcmp(argv[1], "ctx") && argc >= 4)
        return ctx_worker((unsigned int)atoi(argv[2]), (unsigned int)atoi(argv[3]));
    return 2;
}
