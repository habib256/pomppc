/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/*
 * qfbmode.c — fait programmer un mode à POMPPCQFB SANS WindowServer
 * (single-user). Sert à l'épreuve K6/KT4 (docs/epreuve-gpu-bloque.md) :
 * changer la géométrie de qfb-pci pendant que le device qgpu est « cassé ».
 *
 *   qfbmode [SECS]            ouvre la connexion « serveur » d'IOFramebuffer
 *                             (kIOFBServerConnectType, celle du WindowServer) :
 *                             IOFramebuffer::open → enableController → le mode
 *                             par défaut est programmé (1024x768, 32 bits) ;
 *                             la garde ouverte SECS secondes (défaut 0)
 *   qfbmode SECS MODE DEPTH   puis passe au mode MODE (1..N) en profondeur
 *                             DEPTH (sélecteur 4, extSetDisplayMode)
 *
 * PAS d'IOFramebufferOpen d'IOGraphicsLib : le 14/10/2026, en single-user, il
 * a programmé le mode puis ne s'est jamais rendu, clavier de la console perdu
 * avec lui (VM à redémarrer, fsck). La connexion brute suffit. alarm() borne
 * le programme. Code 0 si tout a été accepté, 1 sinon, 2 si POMPPCQFB absent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#define FB_SERVER_CONNECT   0       /* kIOFBServerConnectType */
#define FB_SET_DISPLAY_MODE 4       /* IOFramebufferUserClient : extSetDisplayMode */

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
    mach_port_t master = MACH_PORT_NULL;
    io_service_t svc;
    io_connect_t conn = 0;
    int secs = argc > 1 ? atoi(argv[1]) : 0;
    kern_return_t kr;
    double t0;

    setvbuf(stdout, 0, _IOLBF, 0);
    alarm(secs + 30);
    IOMasterPort(MACH_PORT_NULL, &master);
    svc = IOServiceGetMatchingService(master, IOServiceMatching("POMPPCQFB"));
    if (!svc) {
        printf("qfbmode: POMPPCQFB absent\n");
        return 2;
    }
    t0 = now();
    kr = IOServiceOpen(svc, mach_task_self(), FB_SERVER_CONNECT, &conn);
    printf("qfbmode: t=%.3f ouverture serveur 0x%x en %.3f s\n", now(), kr, now() - t0);
    if (kr != KERN_SUCCESS) {
        return 1;
    }
    if (argc > 3) {
        unsigned int mode = atoi(argv[2]), depth = atoi(argv[3]);
        t0 = now();
        kr = IOConnectMethodScalarIScalarO(conn, FB_SET_DISPLAY_MODE, 2, 0, mode, depth);
        printf("qfbmode: t=%.3f mode %u profondeur %u : 0x%x en %.3f s\n", now(), mode,
               depth, kr, now() - t0);
    }
    if (secs > 0) {
        sleep(secs);
    }
    IOServiceClose(conn);
    printf("qfbmode: t=%.3f fermé\n", now());
    return kr == KERN_SUCCESS ? 0 : 1;
}
