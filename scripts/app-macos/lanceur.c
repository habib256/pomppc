// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 VERHILLE Arnaud
// lanceur.c — exécutable principal de POMPPC.app : passe la main à pomppc-app.sh.
// Un binaire arm64 et non un script : macOS ne déduit pas l'architecture d'un script
// et peut alors réclamer Rosetta sur Apple Silicon. exec conserve le processus que
// LaunchServices a enregistré : le Dock montre bien l'app, jusqu'au frontal.
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char exe[PATH_MAX], real[PATH_MAX], script[PATH_MAX + 64];
    uint32_t size = sizeof(exe);

    (void)argc; (void)argv;
    if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, real)) {
        return 1;
    }
    snprintf(script, sizeof(script), "%s/../Resources/pomppc-app.sh", dirname(real));
    execl("/bin/bash", "bash", script, (char *)NULL);
    perror("bash");
    return 1;
}
