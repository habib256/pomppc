/*
 * pomppc_qgpu.c — client userland du kext POMPPCGPU (voir pomppc_qgpu.h).
 */
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "qgpu_proto.h"
#include "pomppc_qgpu.h"

int qgpu_read_reg(QgpuClient *q, unsigned long offset, unsigned long *value)
{
    unsigned int v = 0;
    kern_return_t kr = IOConnectMethodScalarIScalarO(q->conn, QGPU_UC_READ_REG, 1, 1,
                                                     (unsigned int)offset, &v);
    if (kr != KERN_SUCCESS)
        return -1;
    *value = v;
    return 0;
}

int qgpu_open(QgpuClient *q, const char **why)
{
    mach_port_t master;
    io_service_t svc;
    kern_return_t kr;
    unsigned int version, caps, size, fence, idx, base, cb, sb;
    unsigned long n, k;
    vm_address_t addr = 0;
    vm_size_t len = 0;
    /* Ce que CE qgpu_proto.h attend du device, classe par classe. */
    static const unsigned long want[QGPU_CLASS_COUNT] = {
        QGPU_CLIENT_CTX_IDS, QGPU_CLIENT_SURF_IDS, QGPU_CLIENT_TEX_IDS,
        QGPU_CLIENT_QUERY_IDS, QGPU_CLIENT_BUF_IDS
    };
    unsigned long got[QGPU_CLASS_COUNT];

    q->conn = 0;
    q->win = 0;
    if (IOMasterPort(MACH_PORT_NULL, &master) != KERN_SUCCESS) {
        *why = "IOMasterPort";
        return -1;
    }
    svc = IOServiceGetMatchingService(master, IOServiceMatching("POMPPCGPU"));
    if (!svc) {
        *why = "service POMPPCGPU absent (kext non chargé ?)";
        return -1;
    }
    kr = IOServiceOpen(svc, mach_task_self(), 0, &q->conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        *why = "IOServiceOpen refusé (4 clients déjà ?)";
        return -1;
    }
    kr = IOConnectMethodScalarIScalarO(q->conn, QGPU_UC_GET_INFO, 0, 4,
                                       &version, &caps, &size, &fence);
    /* le device accepte les flux des versions antérieures à la sienne ;
       SURF_PRESENT (v13) est optionnel : on s'attache dès QGPU_PROTO_MIN. */
    if (kr != KERN_SUCCESS || version < QGPU_PROTO_MIN) {
        *why = "version de protocole incompatible";
        goto fail;
    }
    /* v19 — TROIS VÉRIFICATIONS, dans l'ordre des reproches possibles.
       1. Le kext : sait-il lire un registre ? Un kext d'avant la v19 refuse le
          sélecteur ; il compilait la disposition des identifiants et
          détruisait les objets lui-même — c'est lui qu'il faut remplacer. */
    if (qgpu_read_reg(q, QGPU_REG_CLIENTS, &n) != 0) {
        *why = "kext POMPPCGPU d'avant la v19 (pas de QGPU_UC_READ_REG) : "
               "reconstruire et réinstaller le kext (install.sh), redémarrer";
        goto fail;
    }
    /* 2. Le device : publie-t-il ses tranches ? (Un kext v19 refuse déjà de
          démarrer sans QGPU_CAP_CLIENTS ; ceci n'est là que pour le dire.) */
    if (!(caps & QGPU_CAP_CLIENTS) || n == 0) {
        *why = "device qgpu sans QGPU_CAP_CLIENTS : QEMU d'avant la v19";
        goto fail;
    }
    q->nclients = n;
    /* 3. La disposition : le device découpe ses identifiants avec SES
          constantes ; les nôtres doivent être les mêmes, sinon deux clients
          se marchent dessus ou le device ne nettoie pas ce que nous créons.
          Jusqu'au 24/09/2026 ce reproche visait le kext (POMPPC_SUB_LAYOUT) ;
          il vise maintenant QEMU, le seul autre à compiler qgpu_proto.h. */
    for (k = 0; k < QGPU_CLASS_COUNT; k++) {
        if (qgpu_read_reg(q, QGPU_REG_LAYOUT_CLASS(k), &got[k]) != 0 || got[k] != want[k]) {
            *why = "device qgpu d'un autre qgpu_proto.h que le plugin (tranches "
                   "d'identifiants) : reconstruire QEMU, ou le plugin";
            goto fail;
        }
    }
    kr = IOConnectMethodScalarIScalarO(q->conn, QGPU_UC_GET_SLOT, 0, 4,
                                       &idx, &base, &cb, &sb);
    if (kr != KERN_SUCCESS) {
        *why = "GET_SLOT";
        goto fail;
    }
    if (cb != idx * got[QGPU_CLASS_CTX] || sb != idx * got[QGPU_CLASS_SURF]) {
        *why = "GET_SLOT : plages du kext ≠ table du device";
        goto fail;
    }
    kr = IOConnectMapMemory(q->conn, QGPU_UC_MEM_SHMEM, mach_task_self(),
                            &addr, &len, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || !addr || len < size) {
        *why = "IOConnectMapMemory";
        goto fail;
    }
    q->win = (unsigned char *)addr;
    q->size = size;
    q->base = base;
    q->ctx_base = cb;
    q->surf_base = sb;
    q->tex_base = idx * got[QGPU_CLASS_TEX];
    q->query_base = idx * got[QGPU_CLASS_QUERY];
    q->buf_base = idx * got[QGPU_CLASS_BUF];
    q->index = idx;
    q->version = version;
    q->caps = caps;
    return 0;

fail:
    IOServiceClose(q->conn);
    q->conn = 0;
    return -1;
}

void qgpu_close(QgpuClient *q)
{
    if (q->win)
        IOConnectUnmapMemory(q->conn, QGPU_UC_MEM_SHMEM, mach_task_self(),
                             (vm_address_t)q->win);
    if (q->conn)
        IOServiceClose(q->conn);
    q->win = 0;
    q->conn = 0;
}

/* P8 : voir pomppc_qgpu.h. Rien d'autre qu'un oubli — pas d'IOServiceClose, pas
   d'IOConnectUnmapMemory, pas de free : après fork() l'enfant n'a le droit
   d'appeler ni malloc, ni les verrous, et surtout pas le user client du père.
   La tâche enfant sera nettoyée par le noyau quand elle se terminera ou
   exec()era. */
void qgpu_forget(QgpuClient *q)
{
    q->conn = 0;
    q->win = 0;
    q->size = 0;
    q->base = 0;
    q->ctx_base = 0;
    q->surf_base = 0;
    q->tex_base = 0;
    q->query_base = 0;
    q->buf_base = 0;
    q->index = 0;
    q->nclients = 0;
    q->version = 0;
    q->caps = 0;
}

long qgpu_submit(QgpuClient *q, unsigned long off, unsigned long len, unsigned long *pc)
{
    unsigned int fence, status, spc;
    kern_return_t kr = IOConnectMethodScalarIScalarO(q->conn, QGPU_UC_SUBMIT, 2, 3,
                                                     (unsigned int)off, (unsigned int)len,
                                                     &fence, &status, &spc);
    if (kr != KERN_SUCCESS)
        return -1;
    if (pc)
        *pc = spc;
    return status;
}

/* ─────────────────────────── v9 : doorbell asynchrone ───────────────────────
 *
 * Même sélecteur, même nombre d'arguments : seuls les bits hauts de `len`
 * changent (voir pomppc_qgpu.h, « pourquoi dans len »). Un kext d'avant la v9
 * voit alors un `len` démesuré et rend kIOReturnBadArgument — c'est ce qui
 * sert de sonde à qgpu_async_ok(), et c'est ce qui garantit qu'aucune
 * soumission ne part en asynchrone sans que l'autre bout sache la traiter.
 */

long qgpu_submit_async(QgpuClient *q, unsigned long off, unsigned long len,
                       unsigned long *fence, unsigned long *errors)
{
    unsigned int f, status, err;
    kern_return_t kr = IOConnectMethodScalarIScalarO(q->conn, QGPU_UC_SUBMIT, 2, 3,
                                                     (unsigned int)off,
                                                     (unsigned int)(len | POMPPC_SUB_ASYNC),
                                                     &f, &status, &err);
    if (kr != KERN_SUCCESS)
        return -1;
    if (fence)
        *fence = f;
    if (errors)
        *errors = err;
    return status;
}

int qgpu_peek(QgpuClient *q, unsigned long *errors, unsigned long *status,
              unsigned long *pc)
{
    unsigned int e, st, spc;
    kern_return_t kr = IOConnectMethodScalarIScalarO(q->conn, QGPU_UC_SUBMIT, 2, 3,
                                                     0u, (unsigned int)POMPPC_SUB_PEEK,
                                                     &e, &st, &spc);
    if (kr != KERN_SUCCESS)
        return -1;
    if (errors) *errors = e;
    if (status) *status = st;
    if (pc)     *pc = spc;
    return 0;
}

int qgpu_queue(QgpuClient *q, unsigned long *inflight, unsigned long *freeslots,
               unsigned long *depth)
{
    unsigned int a, b, c;
    kern_return_t kr = IOConnectMethodScalarIScalarO(q->conn, QGPU_UC_SUBMIT, 2, 3,
                                                     0u, (unsigned int)POMPPC_SUB_QUEUE,
                                                     &a, &b, &c);
    if (kr != KERN_SUCCESS)
        return -1;
    if (inflight)  *inflight = a;
    if (freeslots) *freeslots = b;
    if (depth)     *depth = c;
    return 0;
}

int qgpu_wait(QgpuClient *q, unsigned long fence, unsigned long ms)
{
    unsigned int cur = 0;
    kern_return_t kr = IOConnectMethodScalarIScalarO(q->conn, QGPU_UC_WAIT_FENCE, 2, 1,
                                                     (unsigned int)fence,
                                                     (unsigned int)ms, &cur);
    if (kr != KERN_SUCCESS)
        return -1;
    /* Compteurs de 32 bits qui bouclent : comparer par différence signée. */
    return ((long)(cur - fence) >= 0) ? 0 : -1;
}

int qgpu_async_ok(QgpuClient *q)
{
    unsigned long e, st, pc;
    if (q->version < 9 || !(q->caps & QGPU_CAP_ASYNC))
        return 0;
    return qgpu_peek(q, &e, &st, &pc) == 0;
}
