/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * unloadpeer.c — client qgpu qui reste ouvert pendant un `kextunload`
 * (épreuve en VM de K1/K4, KG4, KT1 ; docs/bug-hunt-2026-09-29-passe4.md,
 * « Épreuves en VM du 07/10 » ; docs/architecture.md L4).
 *
 * Deux fils sur la même connexion :
 *   - un DORMEUR, en WAIT_FENCE sur une barrière qui n'arrivera pas (60 s) :
 *     c'est le dormeur de commandSleep que stop() doit réveiller (K4, KG4) ;
 *   - un SOUMETTEUR, qui enchaîne des SUBMIT synchrones d'un NOP : un appel
 *     en vol au moment du stop() (KT1, fCallers).
 * Quand le kext s'arrête, chacun doit rendre la main avec une erreur, pas
 * dormir pour toujours ni paniquer. Puis, kext arrêté, chaque sélecteur doit
 * rendre une erreur (« SUBMIT après déchargement → erreur propre ») : soit
 * kIOReturnNotAttached, rendu par le user client encore en vie dont stop() a
 * effacé fOwner (K1), soit kIOReturnBadArgument, rendu par IOKit lui-même
 * quand la terminaison a détaché le port de la connexion — c'est ce que fait
 * Tiger 10.4.11 (vu le 07/10/2026) : l'appel n'atteint plus le kext, ce qui
 * permet au module de se décharger malgré des clients ouverts.
 *
 *   ./unloadpeer [SECONDES=120]
 *
 * Les lignes « t=… » portent l'heure absolue (secondes.ms) : à rapprocher de
 * l'heure du kextunload. Code de sortie : 0 si tout est tenu, 1 sinon, 2 si
 * le kext est absent au départ.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "qgpu_proto.h"

static io_connect_t conn;
static volatile unsigned int *win;
static volatile int done_wait, done_sub;
static volatile kern_return_t wait_kr, sub_kr;
static volatile double wait_t, sub_t, wait_t0;
static volatile unsigned long n_sub_ok, n_wait_timeout;

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static const char *krname(kern_return_t kr)
{
    switch (kr) {
    case KERN_SUCCESS:          return "succès";
    case kIOReturnNotAttached:  return "kIOReturnNotAttached";
    case kIOReturnNotReady:     return "kIOReturnNotReady";
    case kIOReturnTimeout:      return "kIOReturnTimeout";
    case kIOReturnBadArgument:  return "kIOReturnBadArgument";
    case kIOReturnNotPermitted: return "kIOReturnNotPermitted";
    case kIOReturnAborted:      return "kIOReturnAborted";
    case MACH_SEND_INVALID_DEST:return "MACH_SEND_INVALID_DEST";
    default:                    return "?";
    }
}

/* Erreur propre après l'arrêt : voir l'en-tête. */
static int refus_propre(kern_return_t kr)
{
    return kr == kIOReturnNotAttached || kr == kIOReturnBadArgument;
}

static void *dormeur(void *arg)
{
    (void)arg;
    for (;;) {
        unsigned int cur = 0, info[4];
        kern_return_t kr;

        IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_INFO, 0, 4,
                                      &info[0], &info[1], &info[2], &info[3]);
        wait_t0 = now();
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_WAIT_FENCE, 2, 1,
                                           info[3] + 0x1000000u, 60000u, &cur);
        if (kr == kIOReturnTimeout) {
            n_wait_timeout++;
            continue;
        }
        wait_kr = kr;
        wait_t = now();
        done_wait = 1;
        return 0;
    }
}

static void *soumetteur(void *arg)
{
    (void)arg;
    win[0] = QGPU_CMD_HDR(QGPU_OP_NOP, QGPU_LEN_NOP);
    for (;;) {
        unsigned int fence, st, spc;
        kern_return_t kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                                         0u, 4u, &fence, &st, &spc);
        if (kr != KERN_SUCCESS) {
            sub_kr = kr;
            sub_t = now();
            done_sub = 1;
            return 0;
        }
        n_sub_ok++;
        usleep(2000);
    }
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 120;
    mach_port_t master = MACH_PORT_NULL;
    io_service_t svc;
    vm_address_t addr = 0;
    vm_size_t size = 0;
    unsigned int version, caps, slotsz, fence, idx, base, cb, sb, v, st, spc, cur;
    pthread_t ta, tb;
    kern_return_t kr;
    int i, ko = 0;
    double t0;

    IOMasterPort(MACH_PORT_NULL, &master);
    svc = IOServiceGetMatchingService(master, IOServiceMatching("POMPPCGPU"));
    if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &conn) != KERN_SUCCESS) {
        printf("unloadpeer: POMPPCGPU indisponible\n");
        return 2;
    }
    IOObjectRelease(svc);
    if (IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_INFO, 0, 4,
                                      &version, &caps, &slotsz, &fence) != KERN_SUCCESS ||
        IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_SLOT, 0, 4,
                                      &idx, &base, &cb, &sb) != KERN_SUCCESS ||
        IOConnectMapMemory(conn, QGPU_UC_MEM_SHMEM, mach_task_self(),
                           &addr, &size, kIOMapAnywhere) != KERN_SUCCESS) {
        printf("unloadpeer: GET_INFO / GET_SLOT / MapMemory refusé\n");
        return 2;
    }
    win = (volatile unsigned int *)addr;
    printf("unloadpeer: t=%.3f PRÊT version %u caps 0x%x tranche %u fence %u — "
           "kextunload attendu dans les %d s\n", now(), version, caps, idx, fence, secs);
    fflush(stdout);

    pthread_create(&ta, 0, dormeur, 0);
    pthread_create(&tb, 0, soumetteur, 0);
    t0 = now();
    while ((!done_wait || !done_sub) && now() - t0 < secs) {
        usleep(100000);
    }
    if (!done_wait || !done_sub) {
        printf("unloadpeer: ÉCHEC : au bout de %d s, dormeur %s, soumetteur %s "
               "(%lu SUBMIT réussis)\n", secs, done_wait ? "sorti" : "ENCORE BLOQUÉ",
               done_sub ? "sorti" : "toujours servi", n_sub_ok);
        return 1;
    }
    printf("unloadpeer: t=%.3f dormeur sorti : WAIT_FENCE 0x%x (%s) après %.3f s "
           "d'attente, %lu délai(s) de 60 s avant\n", wait_t, wait_kr, krname(wait_kr),
           wait_t - wait_t0, n_wait_timeout);
    printf("unloadpeer: t=%.3f soumetteur sorti : SUBMIT 0x%x (%s) après %lu SUBMIT réussis\n",
           sub_t, sub_kr, krname(sub_kr), n_sub_ok);
    if (wait_kr == KERN_SUCCESS) {
        printf("  ÉCHEC : la barrière impossible a été déclarée atteinte\n");
        ko++;
    }
    /* Kext arrêté : chaque sélecteur doit refuser proprement, sorties nulles. */
    for (i = 0; i < 3; i++) {
        fence = st = spc = 0xDEAD;
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3, 0u, 4u, &fence, &st, &spc);
        printf("unloadpeer: après arrêt, SUBMIT #%d : 0x%x (%s), fence %u statut %u pc %u\n",
               i + 1, kr, krname(kr), fence, st, spc);
        /* NotAttached : sorties remises à zéro par le kext (K2) ; BadArgument :
           IOKit n'a rien écrit, les sorties gardent leur valeur d'avant. */
        if (!refus_propre(kr) ||
            (kr == kIOReturnNotAttached && (fence != 0 || st != QGPU_ST_BACKEND)))
            ko++;
    }
    cur = 0xDEAD;
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_WAIT_FENCE, 2, 1, 1u, 1000u, &cur);
    printf("unloadpeer: après arrêt, WAIT_FENCE : 0x%x (%s), courante %u\n", kr, krname(kr), cur);
    if (!refus_propre(kr)) ko++;
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_INFO, 0, 4, &version, &caps, &slotsz, &fence);
    printf("unloadpeer: après arrêt, GET_INFO : 0x%x (%s), version %u caps 0x%x\n",
           kr, krname(kr), version, caps);
    if (!refus_propre(kr) || (kr == kIOReturnNotAttached && version != 0)) ko++;
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_SLOT, 0, 4, &idx, &base, &cb, &sb);
    printf("unloadpeer: après arrêt, GET_SLOT : 0x%x (%s)\n", kr, krname(kr));
    if (!refus_propre(kr)) ko++;
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_RESET, 0, 0);
    printf("unloadpeer: après arrêt, RESET : 0x%x (%s)\n", kr, krname(kr));
    if (!refus_propre(kr)) ko++;
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_READ_REG, 1, 1, (unsigned int)QGPU_REG_FENCE, &v);
    printf("unloadpeer: après arrêt, READ_REG : 0x%x (%s)\n", kr, krname(kr));
    if (!refus_propre(kr)) ko++;

    IOConnectUnmapMemory(conn, QGPU_UC_MEM_SHMEM, mach_task_self(), addr);
    kr = IOServiceClose(conn);
    printf("unloadpeer: t=%.3f IOServiceClose : 0x%x ; VERDICT : %d échec(s)\n", now(), kr, ko);
    return ko ? 1 : 0;
}
