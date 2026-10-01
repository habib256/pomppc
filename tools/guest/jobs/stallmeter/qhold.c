/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 VERHILLE Arnaud
 *
 * qhold — ouvre N clients du kext POMPPCGPU et les garde SECONDES secondes
 * (29/09/2026). Simule des tranches perdues (« 5e client refusé », K3/K9 des
 * bug hunts) pour voir ce qu'une application GL lancée ensuite rencontre.
 *
 *   qhold N SECONDES
 *
 * gcc -o qhold qhold.c -framework IOKit
 */
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 4, secs = argc > 2 ? atoi(argv[2]) : 60, i, ok = 0;
    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("POMPPCGPU"));
    io_connect_t c[16];
    if (!svc) {
        fprintf(stderr, "POMPPCGPU introuvable\n");
        return 1;
    }
    for (i = 0; i < n && i < 16; i++) {
        kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &c[i]);
        printf("client %d : %s (0x%x)\n", i, kr == KERN_SUCCESS ? "ouvert" : "refusé", kr);
        ok += kr == KERN_SUCCESS;
    }
    fflush(stdout);
    sleep(secs);
    printf("%d clients tenus %d s\n", ok, secs);
    return 0;
}
