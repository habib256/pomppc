/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * slotpeer.c — client qgpu minimal pour les épreuves « GPU hôte bloqué »
 * (GL3, KT4, K6) et « créneau rendu malgré un nettoyage échoué » (A6, L2 de
 * docs/architecture.md §5), avec les propriétés de test du device
 * x-test-stall-ms et x-test-refuse-reset (docs/epreuve-gpu-bloque.md).
 *
 *   slotpeer fill SECS [keep]
 *                         ouvre, crée un contexte (CTX_CREATE, au premier
 *                         identifiant de SA tranche), puis enchaîne des NOP
 *                         ASYNCHRONES jusqu'au refus QUEUE_FULL et dort SECS
 *                         secondes : à tuer (kill -9) pendant que ses
 *                         soumissions sont encore en file. `keep` : au lieu de
 *                         dormir, remet un NOP dès qu'une place se libère —
 *                         la file est PLEINE au moment où il meurt ;
 *   slotpeer create       ouvre et crée le même contexte : c'est la PREMIÈRE
 *                         création d'un nouvel occupant. Code 0 si acceptée,
 *                         3 si QGPU_ST_LIMIT (identifiant laissé pris par
 *                         l'occupant précédent), 4 pour un autre refus.
 *                         Détruit ensuite son contexte et ferme ;
 *   slotpeer hold SECS    ouvre et garde sa tranche SECS secondes ;
 *   slotpeer async1 SECS  ouvre, soumet UN NOP asynchrone (qui reste en vol si
 *                         le GPU est bloqué) et garde sa tranche SECS secondes ;
 *   slotpeer reset        ouvre, QGPU_UC_RESET, rend son code, ferme.
 *
 * Les créations passent en ASYNCHRONE puis WAIT_FENCE (60 s) et le statut est
 * relu par PEEK : un GPU ralenti (x-test-stall-ms) ne fait pas abandonner un
 * doorbell synchrone à 2 s. PEEK rend le statut de la DERNIÈRE soumission
 * terminée : l'épreuve n'a qu'un soumetteur à ce moment-là.
 *
 * Chaque ligne porte l'heure absolue (t=secondes.ms) et la tranche reçue.
 * Code de sortie 2 : kext absent ou ouverture refusée.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "qgpu_proto.h"

static io_connect_t conn;
static volatile unsigned int *win;
static unsigned int idx, ctx_id;

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static const char *stname(unsigned int st)
{
    switch (st) {
    case QGPU_ST_OK:         return "OK";
    case QGPU_ST_BAD_ARG:    return "BAD_ARG";
    case QGPU_ST_LIMIT:      return "LIMIT";
    case QGPU_ST_BACKEND:    return "BACKEND";
    case QGPU_ST_QUEUE_FULL: return "QUEUE_FULL";
    case QGPU_ST_NO_MEM:     return "NO_MEM";
    default:                 return "?";
    }
}

static int open_client(const char *mode)
{
    mach_port_t master = MACH_PORT_NULL;
    io_service_t svc;
    vm_address_t addr = 0;
    vm_size_t size = 0;
    unsigned int version, caps, slotsz, fence, base, cb, sb;
    kern_return_t kr;
    double t0 = now();

    IOMasterPort(MACH_PORT_NULL, &master);
    svc = IOServiceGetMatchingService(master, IOServiceMatching("POMPPCGPU"));
    if (!svc) {
        printf("slotpeer %s: t=%.3f POMPPCGPU absent\n", mode, now());
        return -1;
    }
    kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        printf("slotpeer %s: t=%.3f ouverture REFUSÉE 0x%x après %.3f s\n", mode,
               now(), kr, now() - t0);
        return -1;
    }
    if (IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_INFO, 0, 4,
                                      &version, &caps, &slotsz, &fence) != KERN_SUCCESS ||
        IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_SLOT, 0, 4,
                                      &idx, &base, &cb, &sb) != KERN_SUCCESS ||
        IOConnectMapMemory(conn, QGPU_UC_MEM_SHMEM, mach_task_self(),
                           &addr, &size, kIOMapAnywhere) != KERN_SUCCESS) {
        printf("slotpeer %s: GET_INFO / GET_SLOT / MapMemory refusé\n", mode);
        return -1;
    }
    win = (volatile unsigned int *)addr;
    ctx_id = cb;
    printf("slotpeer %s: t=%.3f ouvert en %.3f s : tranche %u, contexte %u, "
           "caps 0x%x, fence %u\n", mode, now(), now() - t0, idx, ctx_id, caps, fence);
    fflush(stdout);
    return 0;
}

/* Flux de `n` mots à l'offset `off` (mots), ASYNCHRONE, puis attente de sa
   barrière et statut relu par PEEK. Rend le statut, 0xFFFFFFFF si refusé. */
static unsigned int run_async(unsigned int off, unsigned int n, const char *what)
{
    unsigned int fence = 0, st = 0xDEAD, pc = 0, cur = 0, errs = 0;
    kern_return_t kr;
    double t0 = now();

    for (;;) {
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3, off * 4,
                                           (n * 4) | (unsigned int) POMPPC_SUB_ASYNC,
                                           &fence, &st, &pc);
        if (kr != KERN_SUCCESS || st != QGPU_ST_QUEUE_FULL || now() - t0 > 60) {
            break;
        }
        usleep(10000);                  /* file pleine : on réessaie */
    }
    if (kr != KERN_SUCCESS || st != QGPU_ST_OK) {
        printf("slotpeer: t=%.3f tranche %u %s refusé à la soumission : kr 0x%x, statut %u (%s)\n",
               now(), idx, what, kr, st, stname(st));
        return 0xFFFFFFFFu;
    }
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_WAIT_FENCE, 2, 1, fence, 60000u, &cur);
    IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3, 0u,
                                  (unsigned int) POMPPC_SUB_PEEK, &errs, &st, &pc);
    printf("slotpeer: t=%.3f tranche %u %s : barrière %u %s en %.3f s, statut %u (%s)\n",
           now(), idx, what, fence, kr == KERN_SUCCESS ? "atteinte" : "NON atteinte",
           now() - t0, st, stname(st));
    fflush(stdout);
    return kr == KERN_SUCCESS ? st : 0xFFFFFFFFu;
}

static unsigned int ctx_create(void)
{
    win[0] = QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX);
    win[1] = ctx_id;
    return run_async(0, 2, "CTX_CREATE");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    int secs = argc > 2 ? atoi(argv[2]) : 60;
    unsigned int st, pc, fence, n;
    kern_return_t kr;

    if (strcmp(mode, "fill") && strcmp(mode, "create") && strcmp(mode, "hold") &&
        strcmp(mode, "async1") && strcmp(mode, "reset")) {
        printf("usage: slotpeer fill|create|hold|async1|reset [SECS]\n");
        return 2;
    }
    setvbuf(stdout, 0, _IOLBF, 0);
    if (open_client(mode) < 0) {
        return 2;
    }
    if (!strcmp(mode, "create")) {
        st = ctx_create();
        if (st == QGPU_ST_OK) {
            win[0] = QGPU_CMD_HDR(QGPU_OP_CTX_DESTROY, QGPU_LEN_CTX);
            win[1] = ctx_id;
            run_async(0, 2, "CTX_DESTROY");
            st = QGPU_ST_OK;
        }
        IOServiceClose(conn);
        return st == QGPU_ST_OK ? 0 : st == QGPU_ST_LIMIT ? 3 : 4;
    }
    if (!strcmp(mode, "reset")) {
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_RESET, 0, 0);
        printf("slotpeer reset: t=%.3f tranche %u QGPU_UC_RESET : 0x%x\n", now(), idx, kr);
        IOServiceClose(conn);
        return kr == KERN_SUCCESS ? 0 : 1;
    }
    if (!strcmp(mode, "fill")) {
        if (ctx_create() != QGPU_ST_OK) {
            return 4;
        }
    }
    if (!strcmp(mode, "fill") || !strcmp(mode, "async1")) {
        /* NOP asynchrones, à l'offset 64 mots de la tranche */
        win[64] = QGPU_CMD_HDR(QGPU_OP_NOP, QGPU_LEN_NOP);
        for (n = 0; n < 4096; n++) {
            kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3, 64u * 4,
                                               4u | (unsigned int) POMPPC_SUB_ASYNC,
                                               &fence, &st, &pc);
            if (kr != KERN_SUCCESS || st != QGPU_ST_OK) {
                break;
            }
            if (!strcmp(mode, "async1")) {
                n++;
                break;
            }
        }
        printf("slotpeer %s: t=%.3f tranche %u : %u NOP asynchrones acceptés, puis kr 0x%x "
               "statut %u (%s), barrière %u\n", mode, now(), idx, n, kr, st, stname(st), fence);
        if (!strcmp(mode, "fill") && argc > 3 && !strcmp(argv[3], "keep")) {
            /* file tenue pleine jusqu'au kill */
            printf("slotpeer fill: file tenue pleine jusqu'à la mort\n");
            fflush(stdout);
            for (;;) {
                kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3, 64u * 4,
                                                   4u | (unsigned int) POMPPC_SUB_ASYNC,
                                                   &fence, &st, &pc);
                if (kr != KERN_SUCCESS) {
                    break;
                }
                if (st != QGPU_ST_OK) {
                    usleep(200);
                }
            }
        }
    }
    printf("slotpeer %s: t=%.3f tranche %u tenue %d s\n", mode, now(), idx, secs);
    sleep(secs);
    IOServiceClose(conn);
    printf("slotpeer %s: t=%.3f fermé\n", mode, now());
    return 0;
}
