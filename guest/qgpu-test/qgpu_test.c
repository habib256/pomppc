/*
 * qgpu_test.c — programme de test invité (Tiger PPC, userland) du GPU
 * paravirtuel qgpu : ouvre le kext POMPPCGPU, mappe la fenêtre partagée,
 * soumet la scène de référence (triangle rouge sur fond bleu, relecture) et
 * vérifie les pixels témoins — les mêmes que tests/qgpu_core_test.c (hôte) et
 * tests/qgpu_smoke.py (Open Firmware). Écrit aussi la surface dans out.ppm.
 *
 *   make            (gcc 4.0, SDK 10.4u)
 *   ./qgpu_test     → code 0 si tout passe
 *
 * Une capacité ANNONCÉE et non tenue est un échec, pas un test sauté : si
 * QGPU_CAP_ASYNC est dans `caps`, le kext doit répondre à la sonde v9
 * (bug hunt T18 — c'est K6 rendu visible).
 *
 * C'est le squelette de ce que fera le plugin OpenGL (GLDriver) : mêmes
 * appels IOKit, même fenêtre, même flux.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "qgpu_proto.h"      /* -I../../patches/qgpu ; inclut qgpu_abi.h (POMPPC_SUB_*, QGPU_UC_*) */

#define CMD_OFF   0x1000
#define VTX_OFF   0x4000
#define RB_OFF    0x10000
#define W         64
#define H         64
#define STRIDE    (W * 4)
/* v9 : surface large et relecture d'un mégaoctet, pour que la rafale
   asynchrone remplisse vraiment la file du device (voir plus bas). */
#define BIG       512
#define BIG_OFF   0x100000

static int failures;

#define CHECK(cond, fmt, args...) do { \
    if (cond) printf("  ok   " fmt "\n", ## args); \
    else { printf("  FAIL " fmt "\n", ## args); failures++; } } while (0)

/* Le flux est en mots big-endian : sur PPC, un store natif suffit. */
static volatile unsigned int *win;
static unsigned int pc;
static unsigned int base;              /* début de notre tranche dans BAR0 */
static unsigned int ctx_id, surf_id;   /* premiers identifiants de notre plage */

static void emit(unsigned int v)  { win[pc++] = v; }
static void emitf(float f)
{
    union { float f; unsigned int u; } x; x.f = f; emit(x.u);
}
static void vertex(float x, float y, float r, float g, float b)
{
    emitf(x); emitf(y); emitf(0.0f); emitf(1.0f);
    emitf(r); emitf(g); emitf(b); emitf(1.0f);
}
static unsigned int px(int x, int y)       /* RGB : l'octet haut est l'alpha */
{
    return win[(RB_OFF + y * STRIDE + x * 4) / 4] & 0xFFFFFF;
}
static unsigned int bigpx(int x, int y)
{
    return win[(BIG_OFF + y * BIG * 4 + x * 4) / 4] & 0xFFFFFF;
}

int main(void)
{
    mach_port_t   master = MACH_PORT_NULL;
    io_service_t  svc;
    io_connect_t  conn = 0;
    kern_return_t kr;
    vm_address_t  addr = 0;
    vm_size_t     size = 0;
    unsigned int  version, caps, shmem, fence, status, spc, len;

    kr = IOMasterPort(MACH_PORT_NULL, &master);
    svc = IOServiceGetMatchingService(master, IOServiceMatching("POMPPCGPU"));
    if (!svc) {
        printf("FAIL service POMPPCGPU introuvable (kext chargé ? ioreg -c POMPPCGPU)\n");
        return 1;
    }
    kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    CHECK(kr == KERN_SUCCESS, "IOServiceOpen : 0x%x", kr);
    if (kr != KERN_SUCCESS) return 1;

    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_INFO, 0, 4,
                                       &version, &caps, &shmem, &fence);
    CHECK(kr == KERN_SUCCESS, "GET_INFO : version %u caps 0x%x tranche %u Mio fence %u",
          version, caps, shmem >> 20, fence);

    /* ── v19 : le kext ne compile plus la disposition, il la LIT dans le
       device et la laisse lire (QGPU_UC_READ_REG). Un kext d'avant la v19
       refuse le sélecteur : c'est un ÉCHEC ici, pas un test sauté — ce
       programme est compilé contre le contrat v19. */
    {
        unsigned int idx, cb, sb, nclients = 0, magic = 0, v = 0, k;
        static const unsigned int want[QGPU_CLASS_COUNT] = {
            QGPU_CLIENT_CTX_IDS, QGPU_CLIENT_SURF_IDS, QGPU_CLIENT_TEX_IDS,
            QGPU_CLIENT_QUERY_IDS, QGPU_CLIENT_BUF_IDS
        };
        unsigned int got[QGPU_CLASS_COUNT];
        int bad = 0;

        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_READ_REG, 1, 1,
                                           (unsigned int)QGPU_REG_MAGIC, &magic);
        CHECK(kr == KERN_SUCCESS && magic == QGPU_MAGIC,
              "READ_REG(MAGIC) : kr 0x%x, 0x%08x", kr, magic);
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_READ_REG, 1, 1,
                                           (unsigned int)QGPU_REG_VERSION, &v);
        CHECK(kr == KERN_SUCCESS && v == version,
              "READ_REG(VERSION) = %u = GET_INFO", v);
        CHECK((caps & QGPU_CAP_CLIENTS) != 0, "device v19 : QGPU_CAP_CLIENTS (caps 0x%x)", caps);
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_READ_REG, 1, 1,
                                           (unsigned int)QGPU_REG_CLIENTS, &nclients);
        CHECK(kr == KERN_SUCCESS && nclients == QGPU_MAX_CLIENTS,
              "READ_REG(CLIENTS) = %u tranches", nclients);
        for (k = 0; k < QGPU_CLASS_COUNT; k++) {
            got[k] = 0;
            kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_READ_REG, 1, 1,
                                               (unsigned int)QGPU_REG_LAYOUT_CLASS(k), &got[k]);
            if (kr != KERN_SUCCESS || got[k] != want[k])
                bad++;
        }
        CHECK(bad == 0, "table LAYOUT = ce contrat : ctx %u surf %u tex %u query %u buf %u",
              got[0], got[1], got[2], got[3], got[4]);
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_READ_REG, 1, 1,
                                           (unsigned int)QGPU_REG_LAYOUT_CLASS(QGPU_REG_LAYOUT_CLASSES - 1), &v);
        CHECK(kr == KERN_SUCCESS && v == 0, "classe inconnue : 0 (%u)", v);
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_READ_REG, 1, 1, 2u, &v);
        CHECK(kr == kIOReturnBadArgument, "READ_REG non aligné refusé : 0x%x", kr);
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_READ_REG, 1, 1,
                                           (unsigned int)QGPU_CTRL_BAR_SIZE, &v);
        CHECK(kr == kIOReturnBadArgument, "READ_REG hors du BAR refusé : 0x%x", kr);
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_READ_REG, 1, 1,
                                           (unsigned int)QGPU_CTRL_TOPADDR, &v);
        CHECK(kr == KERN_SUCCESS && v == 0xFFFFFFFFu,
              "READ_REG au-delà de TOPADDR : 0xFFFFFFFF (0x%08x)", v);

        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_SLOT, 0, 4,
                                           &idx, &base, &cb, &sb);
        CHECK(kr == KERN_SUCCESS, "GET_SLOT : tranche %u à 0x%x, ids ctx %u surf %u",
              idx, base, cb, sb);
        CHECK(cb == idx * got[QGPU_CLASS_CTX] && sb == idx * got[QGPU_CLASS_SURF],
              "GET_SLOT recopie la table du device (ctx %u surf %u)", cb, sb);
        ctx_id = cb; surf_id = sb;
    }

    kr = IOConnectMapMemory(conn, QGPU_UC_MEM_SHMEM, mach_task_self(),
                            &addr, &size, kIOMapAnywhere);
    CHECK(kr == KERN_SUCCESS && addr, "IOConnectMapMemory : 0x%x, %lu octets à %p",
          kr, (unsigned long) size, (void *) addr);
    if (kr != KERN_SUCCESS) return 1;
    win = (volatile unsigned int *) addr;

    /* sommets */
    pc = VTX_OFF / 4;
    vertex(4, 4, 1, 0, 0); vertex(60, 4, 1, 0, 0); vertex(4, 60, 1, 0, 0);

    /* scène */
    pc = CMD_OFF / 4;
    /* offsets DANS le flux : absolus dans BAR0 (base + relatif) */
    emit(QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(ctx_id);
    emit(QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(ctx_id);
    emit(QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
    emit(surf_id); emit(W); emit(H); emit(QGPU_FMT_XRGB8888);
    emit(QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(surf_id);
    emit(QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR));
    emit(QGPU_CLEAR_COLOR); emit(0x0000FF); emitf(1.0f);
    emit(QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW)); emit(3); emit(base + VTX_OFF);
    emit(QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
    emit(surf_id); emit(base + RB_OFF); emit(STRIDE); emit(0); emit(0); emit(W); emit(H);
    len = pc * 4 - CMD_OFF;

    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                       CMD_OFF, len, &fence, &status, &spc);
    CHECK(kr == KERN_SUCCESS, "SUBMIT : kr 0x%x", kr);
    CHECK(status == QGPU_ST_OK, "statut %u (pc %u), fence %u", status, spc, fence);
    CHECK(px(8, 8) == 0xFF0000, "intérieur du triangle rouge : %06x", px(8, 8));
    CHECK(px(60, 60) == 0x0000FF, "fond bleu : %06x", px(60, 60));
    CHECK(px(2, 2) == 0x0000FF, "coin bleu : %06x", px(2, 2));

    /* opcode inconnu : signalé au bon index, fence avancée quand même */
    pc = CMD_OFF / 4;
    emit(QGPU_CMD_HDR(QGPU_OP_NOP, QGPU_LEN_NOP));
    emit(QGPU_CMD_HDR(0x7777, 1));
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                       CMD_OFF, 8, &fence, &status, &spc);
    CHECK(kr == KERN_SUCCESS && status == QGPU_ST_BAD_OPCODE && spc == 1,
          "opcode inconnu : statut %u pc %u fence %u", status, spc, fence);

    /* soumission hors fenêtre : refusée par le kext avant le device */
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                       shmem - 4, 8, &fence, &status, &spc);
    CHECK(kr == kIOReturnBadArgument, "hors fenêtre refusé par le kext : 0x%x", kr);

    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_WAIT_FENCE, 2, 1,
                                       fence, 1000, &spc);
    CHECK(kr == KERN_SUCCESS && spc >= fence, "WAIT_FENCE : kr 0x%x, fence %u", kr, spc);

    /* Un bit de `len` inconnu du kext (le LAYOUT du 24/09/2026, retiré en
       v19) déborde la tranche : refusé, sans rien soumettre. */
    kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                       0u, 0x10000000u, &fence, &status, &spc);
    CHECK(kr == kIOReturnBadArgument, "bit de len inconnu refusé par le kext : 0x%x", kr);

    /* ─────────────────────────── v9 : doorbell asynchrone ───────────────────
     *
     * Même sélecteur, même nombre d'arguments : le drapeau voyage dans les bits
     * hauts de `len` (voir pomppc_qgpu.h). Un kext d'avant la v9 refuse
     * proprement — c'est ce qui sert de sonde. Tout ce qui suit est donc
     * conditionné à cette sonde, et les appels SYNCHRONES ci-dessus, eux, sont
     * restés exactement ceux de la v8 : les deux formes cohabitent.
     */
    {
        unsigned int errors0 = 0, st0 = 0, pc0 = 0;
        int async = 0;

        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                           0u, (unsigned int)POMPPC_SUB_PEEK,
                                           &errors0, &st0, &pc0);
        async = (kr == KERN_SUCCESS) && (caps & QGPU_CAP_ASYNC);
        CHECK(kr == KERN_SUCCESS || kr == kIOReturnBadArgument,
              "sonde du kext (peek) : kr 0x%x", kr);
        /* Ce qui est ANNONCÉ doit être TENU (bug hunt T18). Sauter les tests
           v9 « parce que le kext n'a pas répondu » alors que le bit ASYNC est
           posé dans caps, et rendre 0, c'est ce qui rendait K6 invisible : le
           plugin, lui, croit l'annonce, passe en asynchrone contre un kext
           synchrone, confond statut et PC, et resoumet la trame. */
        CHECK(!(caps & QGPU_CAP_ASYNC) || async,
              "ASYNC annoncé (caps 0x%x) et tenu par le kext", caps);
        if (!async) {
            printf("  --   doorbell asynchrone non disponible (kext ou device) : "
                   "tests v9 sautés\n");
        } else {
            unsigned int f, stat, err, last = 0, nfull = 0, naccept = 0;
            unsigned int dbell = 0, freeslots = 0, depth = 0;
            int i;

            CHECK(pc0 <= 4 && st0 == QGPU_ST_BAD_OPCODE,
                  "peek : %u erreur(s), dernier statut %u pc %u", errors0, st0, pc0);
            kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                               0u, (unsigned int)POMPPC_SUB_QUEUE,
                                               &dbell, &freeslots, &depth);
            CHECK(kr == KERN_SUCCESS && depth >= 2 && dbell + freeslots == depth,
                  "file : %u en vol, %u libres, profondeur %u", dbell, freeslots, depth);

            /* La scène de référence, mais en BLEU inversé : on la soumet en
               asynchrone et on ne regarde les pixels qu'APRÈS la barrière. */
            pc = CMD_OFF / 4;
            emit(QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(ctx_id);
            emit(QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(surf_id);
            emit(QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR));
            emit(QGPU_CLEAR_COLOR); emit(0x00FF00); emitf(1.0f);
            emit(QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW));
            emit(3); emit(base + VTX_OFF);
            emit(QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
            emit(surf_id); emit(base + RB_OFF); emit(STRIDE);
            emit(0); emit(0); emit(W); emit(H);
            len = pc * 4 - CMD_OFF;

            kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                               CMD_OFF, len | POMPPC_SUB_ASYNC,
                                               &f, &stat, &err);
            CHECK(kr == KERN_SUCCESS && stat == QGPU_ST_OK,
                  "SUBMIT asynchrone : acceptation %u, barrière %u, erreurs %u",
                  stat, f, err);
            kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_WAIT_FENCE, 2, 1,
                                               f, 5000, &spc);
            CHECK(kr == KERN_SUCCESS && spc >= f,
                  "barrière %u atteinte (fence %u)", f, spc);
            /* Relecture visible SEULEMENT après la barrière (contrat mémoire). */
            CHECK(px(8, 8) == 0xFF0000, "après barrière, triangle rouge : %06x", px(8, 8));
            CHECK(px(60, 60) == 0x00FF00, "après barrière, fond vert : %06x", px(60, 60));

            /* Rafale. Pour que la file se remplisse vraiment, il faut que
               l'hôte soit plus lent que l'invité : une surface de 512×512 et
               une relecture d'un mégaoctet par soumission suffisent — avec la
               scène de 64×64 le GPU hôte rattrapait chaque appel système et la
               file ne se remplissait jamais. Un refus ne met RIEN en file :
               ni FENCE ni FENCE_SUBMITTED n'avancent. C'est LA preuve que le
               device n'exécute pas dans l'écriture MMIO. */
            pc = CMD_OFF / 4;
            emit(QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(ctx_id);
            emit(QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
            emit(surf_id + 1); emit(BIG); emit(BIG); emit(QGPU_FMT_XRGB8888);
            len = pc * 4 - CMD_OFF;
            kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                               CMD_OFF, len, &fence, &status, &spc);
            CHECK(kr == KERN_SUCCESS && status == QGPU_ST_OK,
                  "surface %dx%d pour la rafale : statut %u", BIG, BIG, status);
            pc = CMD_OFF / 4;
            emit(QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(ctx_id);
            emit(QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(surf_id + 1);
            emit(QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR));
            emit(QGPU_CLEAR_COLOR); emit(0x123456); emitf(1.0f);
            emit(QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
            emit(surf_id + 1); emit(base + BIG_OFF); emit(BIG * 4);
            emit(0); emit(0); emit(BIG); emit(BIG);
            len = pc * 4 - CMD_OFF;

            last = f;
            for (i = 0; i < 64; i++) {
                kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                                   CMD_OFF, len | POMPPC_SUB_ASYNC,
                                                   &f, &stat, &err);
                if (kr != KERN_SUCCESS)
                    break;
                if (stat == QGPU_ST_QUEUE_FULL) {
                    nfull++;
                    /* FENCE_SUBMITTED est GLOBAL au device (les autres clients
                       du kext comptent dedans) : on vérifie qu'un refus ne la
                       fait pas reculer, pas qu'elle est figée. */
                    CHECK(f >= last, "refus %u : barrière non avancée (%u)", nfull, f);
                    IOConnectMethodScalarIScalarO(conn, QGPU_UC_WAIT_FENCE, 2, 1,
                                                  last, 5000, &spc);
                } else if (stat == QGPU_ST_OK) {
                    naccept++;
                    if ((int)(f - last) <= 0)
                        CHECK(0, "barrière non croissante : %u après %u", f, last);
                    last = f;
                } else {
                    CHECK(0, "acceptation inattendue : %u", stat);
                    break;
                }
            }
            CHECK(kr == KERN_SUCCESS && naccept + nfull == 64,
                  "rafale : %u acceptées, %u refusées (file pleine)", naccept, nfull);
            CHECK(nfull > 0, "la file s'est remplie au moins une fois (%u refus)", nfull);
            kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_WAIT_FENCE, 2, 1,
                                               last, 5000, &spc);
            CHECK(kr == KERN_SUCCESS && (int)(spc - last) >= 0,
                  "la barrière rattrape la rafale : %u pour %u", spc, last);
            CHECK(bigpx(4, 4) == 0x123456 && bigpx(BIG - 1, BIG - 1) == 0x123456,
                  "rafale terminée, relecture %dx%d exacte : %06x", BIG, BIG,
                  bigpx(4, 4));
            pc = CMD_OFF / 4;
            emit(QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(ctx_id);
            emit(QGPU_CMD_HDR(QGPU_OP_SURF_DESTROY, QGPU_LEN_SURF)); emit(surf_id + 1);
            IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                          CMD_OFF, (unsigned int)(pc * 4 - CMD_OFF),
                                          &fence, &status, &spc);

            /* Une soumission fautive au milieu : elle avance FENCE comme les
               autres et incrémente ERRORS, que le peek rapporte. C'est ERRORS
               qui fait foi, pas STATUS, quand plusieurs sont en vol. */
            pc = CMD_OFF / 4;
            emit(QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(ctx_id);
            emit(QGPU_CMD_HDR(0x7777, 1));
            kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                               CMD_OFF, 12u | POMPPC_SUB_ASYNC,
                                               &f, &stat, &err);
            CHECK(kr == KERN_SUCCESS && stat == QGPU_ST_OK,
                  "soumission fautive acceptée (l'erreur se voit à la barrière)");
            IOConnectMethodScalarIScalarO(conn, QGPU_UC_WAIT_FENCE, 2, 1, f, 5000, &spc);
            {
                unsigned int e1, s1, p1;
                kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                                   0u, (unsigned int)POMPPC_SUB_PEEK,
                                                   &e1, &s1, &p1);
                /* ERRORS est GLOBAL au device : ≥ et non = (un autre client
                   du kext peut avoir échoué entre-temps). */
                CHECK(kr == KERN_SUCCESS && e1 >= errors0 + 1 &&
                      s1 == QGPU_ST_BAD_OPCODE && p1 == 2,
                      "ERRORS %u→%u, statut %u pc %u", errors0, e1, s1, p1);
            }

            /* Et le chemin SYNCHRONE marche toujours, après tout cela : c'est
               la compatibilité des deux formes d'appel, vérifiée. */
            pc = CMD_OFF / 4;
            emit(QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(ctx_id);
            emit(QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(surf_id);
            emit(QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR));
            emit(QGPU_CLEAR_COLOR); emit(0x0000FF); emitf(1.0f);
            emit(QGPU_CMD_HDR(QGPU_OP_DRAW_TRIANGLES, QGPU_LEN_DRAW));
            emit(3); emit(base + VTX_OFF);
            emit(QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
            emit(surf_id); emit(base + RB_OFF); emit(STRIDE);
            emit(0); emit(0); emit(W); emit(H);
            len = pc * 4 - CMD_OFF;
            kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                               CMD_OFF, len, &fence, &status, &spc);
            CHECK(kr == KERN_SUCCESS && status == QGPU_ST_OK,
                  "SUBMIT synchrone après l'asynchrone : statut %u", status);
            CHECK(px(60, 60) == 0x0000FF,
                  "synchrone : relecture déjà là au retour (%06x)", px(60, 60));
        }
    }

    /* la surface, pour les yeux */
    {
        FILE *f = fopen("out.ppm", "wb");
        int x, y;
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (y = 0; y < H; y++)
                for (x = 0; x < W; x++) {
                    unsigned int p = px(x, y);
                    fputc(p >> 16, f); fputc(p >> 8, f); fputc(p, f);
                }
            fclose(f);
        }
    }

    /* ── v19 : la destruction des objets d'un client est faite par le DEVICE
       (QGPU_UC_RESET → QGPU_REG_CLIENT_RESET). Avant : recréer notre contexte
       vaut LIMIT (il existe). Après : OK (il a été détruit), et la surface
       aussi (recréée, puis relue : bleu). En dernier, après out.ppm : la
       sonde « peek » de la section v9 lit le statut de la DERNIÈRE soumission
       terminée, et ceci en ferait une autre. */
    {
        unsigned int f2, st2, pc2;
        pc = CMD_OFF / 4;
        emit(QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(ctx_id);
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                           CMD_OFF, 8, &f2, &st2, &pc2);
        CHECK(kr == KERN_SUCCESS && st2 == QGPU_ST_LIMIT,
              "avant RESET : recréer le contexte %u = LIMIT (%u)", ctx_id, st2);
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_RESET, 0, 0);
        CHECK(kr == KERN_SUCCESS, "QGPU_UC_RESET : kr 0x%x", kr);
        pc = CMD_OFF / 4;
        emit(QGPU_CMD_HDR(QGPU_OP_CTX_CREATE, QGPU_LEN_CTX)); emit(ctx_id);
        emit(QGPU_CMD_HDR(QGPU_OP_CTX_BIND, QGPU_LEN_CTX)); emit(ctx_id);
        emit(QGPU_CMD_HDR(QGPU_OP_SURF_CREATE, QGPU_LEN_SURF_CREATE));
        emit(surf_id); emit(W); emit(H); emit(QGPU_FMT_XRGB8888);
        emit(QGPU_CMD_HDR(QGPU_OP_SURF_BIND, QGPU_LEN_SURF)); emit(surf_id);
        emit(QGPU_CMD_HDR(QGPU_OP_CLEAR, QGPU_LEN_CLEAR));
        emit(QGPU_CLEAR_COLOR); emit(0x0000FF); emitf(1.0f);
        emit(QGPU_CMD_HDR(QGPU_OP_SURF_READBACK, QGPU_LEN_SURF_XFER));
        emit(surf_id); emit(base + RB_OFF); emit(STRIDE); emit(0); emit(0); emit(W); emit(H);
        len = pc * 4 - CMD_OFF;
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_SUBMIT, 2, 3,
                                           CMD_OFF, len, &f2, &st2, &pc2);
        CHECK(kr == KERN_SUCCESS && st2 == QGPU_ST_OK,
              "après RESET : contexte et surface recréés par le device (statut %u pc %u)",
              st2, pc2);
        CHECK(px(8, 8) == 0x0000FF, "surface recréée relue : %06x", px(8, 8));
    }

    IOConnectUnmapMemory(conn, QGPU_UC_MEM_SHMEM, mach_task_self(), addr);
    IOServiceClose(conn);
    printf("%s (%d échec(s))\n", failures ? "ÉCHEC" : "OK", failures);
    return failures ? 1 : 0;
}
