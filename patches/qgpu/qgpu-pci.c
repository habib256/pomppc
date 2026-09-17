/*
 * qgpu-pci.c — GPU paravirtuel « qgpu » pour l'invité Mac OS X Tiger PPC
 *              (machine mac99), côté QEMU.
 *
 * Transport uniquement : BAR0 = fenêtre partagée (RAM), BAR1 = registres
 * big-endian, doorbell, compteur de fences, IRQ INTx. Le contenu du flux est
 * exécuté par qgpu-core.c sur un backend (qgpu-soft.c ou qgpu-gl.c) — le
 * device ne connaît pas les opcodes. Contrat complet : qgpu_proto.h.
 *
 * Exécution SYNCHRONE dans l'écriture MMIO du doorbell : quand le vCPU
 * revient de son `stw`, FENCE, STATUS et STATUS_PC sont déjà à jour. C'est
 * volontairement simple (et déterministe pour les tests) ; un thread de rendu
 * asynchrone viendra si la mesure le justifie — l'IRQ DONE et le registre
 * FENCE sont déjà là pour ça, l'invité n'aura rien à changer.
 *
 *   -device qgpu-pci[,shmem_mb=64][,backend=auto|soft|gl][,trace=on]
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qom/object.h"

#include "qgpu-core.h"

#define TYPE_QGPU_PCI "qgpu-pci"
OBJECT_DECLARE_SIMPLE_TYPE(QgpuPCIState, QGPU_PCI)

#define QGPU_NUM_REGS (QGPU_CTRL_TOPADDR / sizeof(uint32_t))

struct QgpuPCIState {
    PCIDevice parent_obj;

    MemoryRegion mem_shmem;
    MemoryRegion mem_ctrl;
    uint8_t *shmem;
    uint32_t shmem_mb;
    char *backend;
    bool trace;

    uint32_t regs[QGPU_NUM_REGS];
    qemu_irq irq;

    QgpuCore core;
    bool core_ok;
};

static void qgpu_update_irq(QgpuPCIState *s)
{
    uint32_t pending = s->regs[QGPU_REG_IRQ >> 2] &
                       s->regs[QGPU_REG_IRQ_MASK >> 2] & QGPU_IRQ_DONE;
    qemu_set_irq(s->irq, pending != 0);
}

static void qgpu_doorbell(QgpuPCIState *s)
{
    uint32_t off = s->regs[QGPU_REG_SUBMIT_OFF >> 2];
    uint32_t len = s->regs[QGPU_REG_SUBMIT_LEN >> 2];
    uint32_t st;

    if (s->core_ok) {
        st = qgpu_core_execute(&s->core, off, len);
        s->regs[QGPU_REG_STATUS_PC >> 2] = s->core.status_pc;
    } else {
        st = QGPU_ST_BACKEND;
        s->regs[QGPU_REG_STATUS_PC >> 2] = 0;
    }
    s->regs[QGPU_REG_STATUS >> 2] = st;
    s->regs[QGPU_REG_FENCE >> 2]++;
    if (s->trace) {
        fprintf(stderr, "qgpu-pci: soumission off=0x%x len=%u -> statut %u "
                "(pc %u), fence %u\n", off, len, st,
                s->regs[QGPU_REG_STATUS_PC >> 2], s->regs[QGPU_REG_FENCE >> 2]);
    }
    s->regs[QGPU_REG_IRQ >> 2] |= QGPU_IRQ_DONE;
    qgpu_update_irq(s);
}

static void qgpu_soft_reset(QgpuPCIState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[QGPU_REG_MAGIC >> 2] = QGPU_MAGIC;
    s->regs[QGPU_REG_VERSION >> 2] = QGPU_PROTO_VERSION;
    s->regs[QGPU_REG_CAPS >> 2] = s->core_ok ? s->core.be->cap : 0;
    s->regs[QGPU_REG_SHMEM_SIZE >> 2] = s->shmem_mb * MiB;
    s->regs[QGPU_REG_BACKEND_NAME >> 2] = qgpu_core_backend_tag(&s->core);
    if (s->core_ok) {
        qgpu_core_reset(&s->core);
    }
    qemu_irq_lower(s->irq);
}

static uint64_t qgpu_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    QgpuPCIState *s = opaque;

    if (addr >= QGPU_CTRL_TOPADDR) {
        return 0xFFFFFFFF;
    }
    switch (addr) {
    case QGPU_REG_DOORBELL:
        return 0;                       /* jamais occupé : exécution synchrone */
    case QGPU_REG_DEBUG:
        return 0;
    default:
        return s->regs[addr >> 2];
    }
}

static void qgpu_ctrl_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    QgpuPCIState *s = opaque;
    uint32_t v = val;

    if (addr >= QGPU_CTRL_TOPADDR) {
        return;
    }
    if (s->trace && addr != QGPU_REG_SUBMIT_OFF && addr != QGPU_REG_SUBMIT_LEN
        && addr != QGPU_REG_DEBUG) {
        fprintf(stderr, "qgpu-pci: reg[0x%02x] <- 0x%08x\n", (unsigned)addr, v);
    }
    switch (addr) {
    case QGPU_REG_MAGIC:
        qgpu_soft_reset(s);
        break;
    case QGPU_REG_SUBMIT_OFF:
    case QGPU_REG_SUBMIT_LEN:
        s->regs[addr >> 2] = v;
        break;
    case QGPU_REG_DOORBELL:
        if (v & 1) {
            qgpu_doorbell(s);
        }
        break;
    case QGPU_REG_IRQ_MASK:
        s->regs[addr >> 2] = v & QGPU_IRQ_DONE;
        qgpu_update_irq(s);
        break;
    case QGPU_REG_IRQ:
        s->regs[addr >> 2] &= ~v;       /* acquittement des bits écrits */
        qgpu_update_irq(s);
        break;
    case QGPU_REG_DEBUG:
        if (v <= 255) {
            fputc(v, stderr);
        }
        break;
    default:
        /* registres en lecture seule : ignoré */
        break;
    }
}

static const MemoryRegionOps qgpu_ctrl_ops = {
    .read = qgpu_ctrl_read,
    .write = qgpu_ctrl_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void qgpu_pci_set_irq(void *opaque, int n, int level)
{
    pci_set_irq(PCI_DEVICE(opaque), level);
}

static void qgpu_pci_realize(PCIDevice *dev, Error **errp)
{
    QgpuPCIState *s = QGPU_PCI(dev);
    uint8_t *pci_conf = dev->config;

    if (s->shmem_mb < QGPU_SHMEM_MIN_MB || s->shmem_mb > QGPU_SHMEM_MAX_MB ||
        (s->shmem_mb & (s->shmem_mb - 1))) {
        error_setg(errp, "qgpu-pci: shmem_mb doit être une puissance de 2 "
                   "entre %d et %d", QGPU_SHMEM_MIN_MB, QGPU_SHMEM_MAX_MB);
        return;
    }

    memory_region_init_ram(&s->mem_shmem, OBJECT(dev), "qgpu.shmem",
                           (uint64_t)s->shmem_mb * MiB, &error_abort);
    s->shmem = memory_region_get_ram_ptr(&s->mem_shmem);
    memory_region_init_io(&s->mem_ctrl, OBJECT(dev), &qgpu_ctrl_ops, s,
                          "qgpu.ctrl", QGPU_CTRL_BAR_SIZE);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->mem_shmem);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mem_ctrl);

    pci_conf[PCI_INTERRUPT_PIN] = 1; /* INTA */
    s->irq = qemu_allocate_irq(qgpu_pci_set_irq, s, 0);

    s->core.trace = s->trace;
    s->core_ok = qgpu_core_init(&s->core, s->backend, s->shmem,
                                s->shmem_mb * MiB);
    s->core.trace = s->trace;
    if (!s->core_ok) {
        error_setg(errp, "qgpu-pci: aucun backend '%s' disponible sur cet hôte",
                   s->backend);
        return;
    }
    if (s->trace) {
        fprintf(stderr, "qgpu-pci: backend %s, fenêtre %u Mio\n",
                s->core.be->name, s->shmem_mb);
    }
    qgpu_soft_reset(s);
}

static void qgpu_pci_exit(PCIDevice *dev)
{
    QgpuPCIState *s = QGPU_PCI(dev);

    if (s->core_ok) {
        qgpu_core_fini(&s->core);
        s->core_ok = false;
    }
    qemu_free_irq(s->irq);
}

static void qgpu_pci_reset_handler(DeviceState *d)
{
    qgpu_soft_reset(QGPU_PCI(d));
}

static int qgpu_post_load(void *opaque, int version_id)
{
    /* Les objets hôte (contextes, surfaces) ne migrent pas : l'invité repart
       d'un device vide, FENCE conservée pour ne pas dérouter un client. */
    QgpuPCIState *s = opaque;
    uint32_t fence = s->regs[QGPU_REG_FENCE >> 2];

    qgpu_soft_reset(s);
    s->regs[QGPU_REG_FENCE >> 2] = fence;
    return 0;
}

static const VMStateDescription vmstate_qgpu_pci = {
    .name = "qgpu-pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = qgpu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, QgpuPCIState),
        VMSTATE_UINT32_ARRAY(regs, QgpuPCIState, QGPU_NUM_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static Property qgpu_pci_properties[] = {
    DEFINE_PROP_UINT32("shmem_mb", QgpuPCIState, shmem_mb, QGPU_SHMEM_DEFAULT_MB),
    DEFINE_PROP_STRING("backend", QgpuPCIState, backend),
    DEFINE_PROP_BOOL("trace", QgpuPCIState, trace, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void qgpu_pci_instance_init(Object *obj)
{
    QgpuPCIState *s = QGPU_PCI(obj);

    s->backend = g_strdup("auto");
}

static void qgpu_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = qgpu_pci_realize;
    k->exit = qgpu_pci_exit;
    k->vendor_id = QGPU_PCI_VENDOR_ID;
    k->device_id = QGPU_PCI_DEVICE_ID;
    /* Coprocesseur, PAS un contrôleur d'affichage : ni IONDRVFramebuffer ni
       aucun pilote graphique d'Apple ne doit tenter de le revendiquer. */
    k->class_id = PCI_CLASS_PROCESSOR_CO;

    dc->desc = "POMPPC paravirtual GPU command processor (qgpu)";
    dc->vmsd = &vmstate_qgpu_pci;
    device_class_set_legacy_reset(dc, qgpu_pci_reset_handler);
    device_class_set_props(dc, qgpu_pci_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo qgpu_pci_info = {
    .name          = TYPE_QGPU_PCI,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(QgpuPCIState),
    .instance_init = qgpu_pci_instance_init,
    .class_init    = qgpu_pci_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void qgpu_pci_register_types(void)
{
    type_register_static(&qgpu_pci_info);
}

type_init(qgpu_pci_register_types)
