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
 * C'est le squelette de ce que fera le plugin OpenGL (GLDriver) : mêmes
 * appels IOKit, même fenêtre, même flux.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "../../kext/POMPPCGPU/qgpu_proto.h"

#define CMD_OFF   0x1000
#define VTX_OFF   0x4000
#define RB_OFF    0x10000
#define W         64
#define H         64
#define STRIDE    (W * 4)

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

    {
        unsigned int idx, cb, sb;
        kr = IOConnectMethodScalarIScalarO(conn, QGPU_UC_GET_SLOT, 0, 4,
                                           &idx, &base, &cb, &sb);
        CHECK(kr == KERN_SUCCESS, "GET_SLOT : tranche %u à 0x%x, ids ctx %u surf %u",
              idx, base, cb, sb);
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

    IOConnectUnmapMemory(conn, QGPU_UC_MEM_SHMEM, mach_task_self(), addr);
    IOServiceClose(conn);
    printf("%s (%d échec(s))\n", failures ? "ÉCHEC" : "OK", failures);
    return failures ? 1 : 0;
}
