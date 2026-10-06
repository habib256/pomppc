/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * errpeer.c — client qgpu FAUTIF, pour l'épreuve des erreurs par client
 * (QGPU_CAP_CLIENT_ERRORS, 07/10/2026 ; tools/guest/jobs/regerr).
 *
 * Ouvre le kext POMPPCGPU comme un second processus GL le ferait, et soumet
 * N flux refusés (opcode inconnu, QGPU_ST_BAD_OPCODE) espacés de MS ms. Chacun
 * fait avancer QGPU_REG_ERRORS, global au device : un plugin qui ne lit que
 * lui repassait en synchrone et invalidait ses miroirs. Avec le compteur par
 * tranche, seul celui de CE client bouge, ce que le programme vérifie.
 *
 *   ./errpeer [N=200] [MS=20]    → 0 si les N refus sont comptés à sa tranche
 *
 * SIGTERM / SIGINT arrêtent la boucle proprement (bilan compris) : le job le
 * lance en tâche de fond pendant qu'un autre client dessine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "qgpu_proto.h"

static volatile sig_atomic_t halt_req;
static void on_sig(int sig) { (void)sig; halt_req = 1; }

static unsigned int rd(io_connect_t c, unsigned int off)
{
    unsigned int v = 0xFFFFFFFFu;
    IOConnectMethodScalarIScalarO(c, QGPU_UC_READ_REG, 1, 1, off, &v);
    return v;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200;
    int ms = argc > 2 ? atoi(argv[2]) : 20;
    mach_port_t master = MACH_PORT_NULL;
    io_service_t svc;
    io_connect_t conn = 0;
    vm_address_t addr = 0;
    vm_size_t size = 0;
    unsigned int version, caps, slot, fence, idx, base, cb, sb, st, spc;
    unsigned int g0, c0, g1, c1;
    volatile unsigned int *win;
    int i, refused = 0;

    IOMasterPort(MACH_PORT_NULL, &master);
    svc = IOServiceGetMatchingService(master, IOServiceMatching("POMPPCGPU"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
        printf("errpeer: POMPPCGPU indisponible\n");
        return 2;
    }
    IOObjectRelease(svc);
    if (IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_INFO, 0, 4,
                                      &version, &caps, &slot, &fence) != KERN_SUCCESS ||
        IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_SLOT, 0, 4,
                                      &idx, &base, &cb, &sb) != KERN_SUCCESS ||
        IOConnectMapMemory(conn, QGPU_UC_MEM_SHMEM, mach_task_self(),
                           &addr, &size, kIOMapAnywhere) != KERN_SUCCESS) {
        printf("errpeer: GET_INFO / GET_SLOT / MapMemory refusé\n");
        return 2;
    }
    win = (volatile unsigned int *)addr;
    win[0] = QGPU_CMD_HDR(0x7777, 1);   /* opcode inconnu */
    g0 = rd(conn, QGPU_REG_ERRORS);
    c0 = rd(conn, QGPU_REG_CLIENT_ERRORS(idx));
    printf("errpeer: tranche %u, caps 0x%x, %d refus toutes les %d ms ; "
           "ERRORS %u, tranche %u\n", idx, caps, n, ms, g0, c0);
    fflush(stdout);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    for (i = 0; i < n && !halt_req; i++) {
        if (IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3, 0u, 4u,
                                          &fence, &st, &spc) == KERN_SUCCESS &&
            st == QGPU_ST_BAD_OPCODE)
            refused++;
        if (ms > 0)
            usleep(ms * 1000);
    }
    g1 = rd(conn, QGPU_REG_ERRORS);
    c1 = rd(conn, QGPU_REG_CLIENT_ERRORS(idx));
    n = i;                              /* soumis (moins si arrêté) */
    printf("errpeer: %d/%d refusés ; ERRORS %u -> %u (+%u), tranche %u : %u -> %u (+%u)\n",
           refused, n, g0, g1, g1 - g0, idx, c0, c1, c1 - c0);
    IOConnectUnmapMemory(conn, QGPU_UC_MEM_SHMEM, mach_task_self(), addr);
    IOServiceClose(conn);
    if (!(caps & QGPU_CAP_CLIENT_ERRORS))
        return refused == n ? 0 : 1;
    return refused == n && (int)(c1 - c0) == n ? 0 : 1;
}
