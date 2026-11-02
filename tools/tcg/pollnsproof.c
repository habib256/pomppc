/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
/* Contrat de tcg/0058, contre le vrai g_poll de l'hôte. pollnsproof.py joint
 * les fonctions du qemu-timer.c construit, sans réécrire leur algorithme. */
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>

/* [D] QEMU v11.1.2, include/qemu/{timer.h,osdep.h} : unités et arrondi. */
#define SCALE_MS 1000000
#define SCALE_US 1000
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

static unsigned calls, cases, failures;

static int proof_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    calls++;
    return select(n, r, w, e, tv);
}

#define select proof_select
#include "pollns-target.inc"
#undef select

static void compare(const char *name, const GPollFD *input, unsigned n,
                    int64_t ns, unsigned expected_calls)
{
    GPollFD ref[8], test[8];
    int nr, nt, er, et;
    unsigned i;
    g_assert(n <= G_N_ELEMENTS(ref));
    if (n) {
        memcpy(ref, input, n * sizeof(*input));
        memcpy(test, input, n * sizeof(*input));
    }
    errno = 0;
    nr = g_poll(ref, n, qemu_timeout_ns_to_ms(ns));
    er = errno;
    calls = 0;
    errno = 0;
    nt = qemu_poll_ns(test, n, ns);
    et = errno;
    cases++;
    if (nr != nt || (nr < 0 && er != et) || calls != expected_calls) {
        fprintf(stderr, "%s : g_poll=%d/%d qemu=%d/%d select=%u attendu=%u\n",
                name, nr, er, nt, et, calls, expected_calls);
        failures++;
    }
    /* revents n'a pas de contrat après une erreur système. */
    for (i = 0; nr >= 0 && i < n; i++) {
        if (ref[i].revents != test[i].revents) {
            fprintf(stderr, "%s fd[%u] : g_poll=0x%x qemu=0x%x\n",
                    name, i, ref[i].revents, test[i].revents);
            failures++;
        }
    }
}

#ifdef HAVE_POLL_IDLE
static gpointer other_poll_thread(gpointer arg)
{
    GPollFD f = {*(int *)arg, G_IO_IN, 0xffff};
    compare("autre fil : aucun état idle hérité", &f, 1, 1000, 0);
    return NULL;
}
#endif

int main(void)
{
    const int64_t delays[] = {-1, 0, 1, 999999, 1000000, 1000001, 1000000000000LL};
    /* Routes spécifiées : infini, zéro, sous 1 ms, frontière exacte et au-delà. */
    const unsigned routes[2][7] = {{0, 0, 1, 1, 1, 1, 1},
                                    {0, 0, 1, 1, 0, 0, 0}};
    int p[2], s[2], closed;
    const char *mode = getenv("POMPPC_POLL_NS");
    unsigned i, full = mode && !strcmp(mode, "1");
    unsigned short_only = mode && !strcmp(mode, "2");
#ifdef HAVE_POLL_IDLE
    unsigned adaptive = mode && !strcmp(mode, "3");
    const char *idle_env = getenv("POMPPC_PROOF_IDLE");
    bool idle = idle_env && !strcmp(idle_env, "1");
    qemu_poll_ns_set_cpus_idle(idle);
    short_only |= adaptive && idle;
#endif
    unsigned on = full || short_only;
    GPollFD f[3];
    alarm(10);
    g_assert(pipe(p) == 0);
    g_assert(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    g_assert(write(p[1], "x", 1) == 1);
    g_assert(write(s[1], "x", 1) == 1);
    for (i = 0; i < G_N_ELEMENTS(delays); i++) {
        f[0] = (GPollFD){p[0], G_IO_IN, 0xffff};
        compare("pipe prêt / frontières de délai", f, 1, delays[i],
                full ? routes[0][i] : short_only ? routes[1][i] : 0);
    }
    f[0] = (GPollFD){s[0], G_IO_IN | G_IO_OUT | G_IO_PRI, 0xffff};
    compare("socket IN+OUT", f, 1, 1000, on);
    f[0] = (GPollFD){p[0], G_IO_IN, 0xffff};
    f[1] = f[0];
    f[2] = (GPollFD){-1, G_IO_IN, 0xffff};
    compare("doublons et fd ignoré", f, 3, 1000, on);
#ifdef HAVE_POLL_IDLE
    if (adaptive) {
        GThread *thread = g_thread_new("poll-proof", other_poll_thread, &p[0]);
        g_thread_join(thread);
    }
#endif
    compare("aucun fd", NULL, 0, 1, on);
    f[0] = (GPollFD){p[0], 0, 0xffff};
    compare("aucun événement demandé", f, 1, 1, on);
    f[0] = (GPollFD){FD_SETSIZE + 7, G_IO_IN, 0xffff};
    compare("hors fd_set : repli", f, 1, 1, 0);
    close(p[1]);
    f[0] = (GPollFD){p[0], G_IO_IN, 0xffff};
    compare("fin de pipe", f, 1, 1, on);
    closed = s[1];
    close(s[1]);
    f[0] = (GPollFD){closed, G_IO_IN, 0xffff};
    compare("fd fermé : erreur propagée", f, 1, 1, on);
    close(p[0]);
    close(s[0]);
    printf("POMPPC_POLL_NS=%s : %u cas, %u écart(s)\n",
           mode ? mode : "(absent)", cases, failures);
    return failures != 0;
}
