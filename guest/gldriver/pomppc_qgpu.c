/*
 * pomppc_qgpu.c — client userland du kext POMPPCGPU (voir pomppc_qgpu.h).
 */
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "qgpu_proto.h"
#include "pomppc_qgpu.h"

int qgpu_open(QgpuClient *q, const char **why)
{
    mach_port_t master;
    io_service_t svc;
    kern_return_t kr;
    unsigned int version, caps, size, fence, idx, base, cb, sb;
    vm_address_t addr = 0;
    vm_size_t len = 0;

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
    /* le device accepte les flux des versions antérieures à la sienne */
    if (kr != KERN_SUCCESS || version < QGPU_PROTO_VERSION) {
        *why = "version de protocole incompatible";
        goto fail;
    }
    kr = IOConnectMethodScalarIScalarO(q->conn, QGPU_UC_GET_SLOT, 0, 4,
                                       &idx, &base, &cb, &sb);
    if (kr != KERN_SUCCESS) {
        *why = "GET_SLOT";
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
    q->tex_base = idx * QGPU_CLIENT_TEX_IDS;
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
