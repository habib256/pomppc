/*
 * qgpu-pci.c — GPU paravirtuel « qgpu » pour l'invité Mac OS X Tiger PPC
 *              (machine mac99), côté QEMU.
 *
 * Transport uniquement : BAR0 = fenêtre partagée (RAM), BAR1 = registres
 * big-endian, doorbell, compteur de fences, IRQ INTx. Le contenu du flux est
 * exécuté par qgpu-core.c sur un backend (qgpu-soft.c ou qgpu-gl.c) — le
 * device ne connaît pas les opcodes. Contrat complet : qgpu_proto.h.
 *
 * v9 — THREAD DE RENDU. Un seul thread possède le backend (donc le contexte
 * GL) pour toute la vie du device, et une file bornée le nourrit :
 *
 *   - doorbell ASYNCHRONE (QGPU_DOORBELL_GO|QGPU_DOORBELL_ASYNC) : la
 *     soumission est mise en file, l'écriture MMIO rend la main tout de suite,
 *     le vCPU repart pendant que le GPU hôte dessine. C'est le gain visé.
 *   - doorbell SYNCHRONE (QGPU_DOORBELL_GO, le défaut de v1–v8) : la
 *     soumission passe par LA MÊME file, et l'écriture MMIO attend qu'elle soit
 *     terminée. Au retour du `stw`, FENCE, STATUS, STATUS_PC et les relectures
 *     sont à jour : un invité v1–v8 ne voit aucune différence.
 *
 * Pourquoi tout passer par le thread, y compris le synchrone, plutôt que de
 * garder l'exécution en place quand la file est vide : un contexte GL peut
 * être rendu courant sur des threads différents à des moments différents,
 * mais JAMAIS sur deux threads à la fois — et EGL exige en plus qu'un thread
 * l'ait RELÂCHÉ avant qu'un autre le prenne. Le reset du cœur et la libération
 * du backend passent donc eux aussi par le thread (q_reset, q_fini) : faits
 * sur le vCPU, ils laissaient le contexte courant sur ce vCPU, et le thread de
 * rendu ne pouvait plus le prendre (vu sur NVIDIA : toutes les soumissions
 * après un reset répondaient QGPU_ST_BACKEND). Exécuter
 * parfois sur le vCPU et parfois sur le thread de rendu obligerait à prouver
 * cette exclusion à chaque chemin, pour économiser deux réveils de condition
 * (quelques microsecondes) sur une opération qui en coûte des centaines. Un
 * seul propriétaire du contexte, un seul ordre d'exécution : c'est aussi ce qui
 * rend gratuite la garantie « l'ordre d'exécution est l'ordre de soumission »,
 * y compris quand un invité mélange les deux modes ou plusieurs clients.
 *
 * Publication des résultats. Le thread de rendu ne prend JAMAIS le BQL :
 *   - FENCE, STATUS, STATUS_PC et ERRORS sont écrits par qatomic_*, FENCE en
 *     dernier et en RELEASE — c'est la barrière qui publie aussi les
 *     relectures écrites dans BAR0 (de la RAM QEMU ordinaire, accessible depuis
 *     n'importe quel thread) ;
 *   - l'interruption est levée par un bottom half (qemu_bh_schedule est sûr
 *     depuis n'importe quel thread), jamais par pci_set_irq depuis le thread.
 * Le vCPU, lui, tient le BQL puis le mutex de la file ; le thread de rendu ne
 * prend que le mutex : pas d'inversion possible.
 *
 * Hors périmètre : migration et retrait à chaud (la file est drainée avant de
 * sauver l'état, les objets hôte ne migrent toujours pas).
 *
 *   -device qgpu-pci[,shmem_mb=64][,backend=auto|soft|gl][,trace=on]
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "hw/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qom/object.h"
#include "sysemu/sysemu.h"

#include "qgpu-core.h"

/* Câblé par qfb-pci.c : VRAM QFB comme cible de SURF_PRESENT (v13). */
int qfb_scanout_info(uint8_t **ram, uint32_t *size, MemoryRegion **mr);

/* À défaut de qfb-pci, le framebuffer VGA standard : sur mac99 c'est l'écran
 * réellement utilisé (pci_vga_init, piloté dans Tiger par qemu_vga.ndrv), et
 * sans lui SURF_PRESENT n'avait aucune cible — la v13 restait inerte dans la
 * configuration de tous les jours, sans que rien ne le dise.
 *
 * Sa VRAM s'atteint sans toucher au code amont : toute MemoryRegion nommée est
 * un enfant QOM de son propriétaire (memory_region_do_init), donc « vga.vram »
 * devient l'enfant « vga.vram[0] » du device « VGA ». Les pixels y sont gros
 * boutistes sur PowerPC, ce que SURF_PRESENT écrit déjà. */
static bool qgpu_vga_scanout_info(uint8_t **ram, uint32_t *size,
                                  MemoryRegion **mr)
{
    Object *dev = object_resolve_path_type("", "VGA", NULL);
    Object *child = dev ? object_resolve_path_component(dev, "vga.vram[0]")
                        : NULL;
    MemoryRegion *m = child ? MEMORY_REGION(object_dynamic_cast(child,
                                            TYPE_MEMORY_REGION)) : NULL;

    if (!m || !memory_region_is_ram(m)) {
        return false;
    }
    *ram = memory_region_get_ram_ptr(m);
    *size = (uint32_t)memory_region_size(m);
    *mr = m;
    return *ram != NULL;
}

#define TYPE_QGPU_PCI "qgpu-pci"
OBJECT_DECLARE_SIMPLE_TYPE(QgpuPCIState, QGPU_PCI)

#define QGPU_NUM_REGS (QGPU_CTRL_TOPADDR / sizeof(uint32_t))

/* Une soumission en attente : ce que le doorbell a lu dans les registres. */
typedef struct QgpuJob {
    uint32_t off, len;
} QgpuJob;

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

    /* v9 : file de soumissions et thread de rendu. `lock` protège la file et
       les compteurs qui la décrivent ; `cond_work` réveille le thread,
       `cond_done` réveille le vCPU qui attend (doorbell synchrone, drainage). */
    QemuThread render_thread;
    QemuMutex  lock;
    QemuCond   cond_work;
    QemuCond   cond_done;
    QgpuJob    queue[QGPU_QUEUE_DEPTH];
    unsigned   q_head;             /* première soumission non terminée */
    unsigned   q_count;            /* en attente + celle en cours */
    bool       q_running;          /* queue[q_head] est entre les mains du thread */
    uint32_t   q_submitted;        /* copie interne de FENCE_SUBMITTED */
    bool       q_stop;             /* demande d'arrêt du thread */
    bool       q_fini;             /* … en libérant d'abord le backend */
    bool       q_reset;            /* demande de reset du cœur, par le thread */
    bool       thread_ok;
    QEMUBH    *irq_bh;
    Notifier   exit_notifier;
    Notifier   machine_done;       /* v13 : lier qfb une fois tous les devices nés */
};

static void qgpu_update_irq(QgpuPCIState *s)
{
    uint32_t pending = s->regs[QGPU_REG_IRQ >> 2] &
                       s->regs[QGPU_REG_IRQ_MASK >> 2] & QGPU_IRQ_DONE;
    qemu_set_irq(s->irq, pending != 0);
}

/* Bottom half : lève QGPU_IRQ_DONE depuis le thread principal, sous BQL. Le
   thread de rendu ne fait que le planifier — pci_set_irq n'est appelé qu'ici et
   depuis les chemins vCPU. L'interruption est de NIVEAU et se coalesce : un
   seul BH peut couvrir plusieurs soumissions terminées, l'invité relit FENCE. */
static void qgpu_irq_bh(void *opaque)
{
    QgpuPCIState *s = opaque;

    s->regs[QGPU_REG_IRQ >> 2] |= QGPU_IRQ_DONE;
    qgpu_update_irq(s);
}

/* Exécute UNE soumission et publie son résultat. Appelé par le seul thread de
   rendu : c'est lui qui possède le backend (et donc le contexte GL). */
static void qgpu_run_job(QgpuPCIState *s, const QgpuJob *job)
{
    uint32_t st, pc;

    if (s->core_ok) {
        st = qgpu_core_execute(&s->core, job->off, job->len);
        pc = s->core.status_pc;
    } else {
        st = QGPU_ST_BACKEND;
        pc = 0;
    }
    qatomic_set(&s->regs[QGPU_REG_STATUS_PC >> 2], pc);
    qatomic_set(&s->regs[QGPU_REG_STATUS >> 2], st);
    if (st != QGPU_ST_OK) {
        qatomic_set(&s->regs[QGPU_REG_ERRORS >> 2],
                    qatomic_read(&s->regs[QGPU_REG_ERRORS >> 2]) + 1);
    }
    /* FENCE EN DERNIER, en release : c'est la publication. Elle ordonne le
       statut ci-dessus ET tout ce que la soumission a écrit dans BAR0 (les
       relectures) avant que l'invité ne puisse voir la barrière avancer. */
    qatomic_store_release(&s->regs[QGPU_REG_FENCE >> 2],
                          qatomic_read(&s->regs[QGPU_REG_FENCE >> 2]) + 1);
    if (s->trace) {
        fprintf(stderr, "qgpu-pci: soumission off=0x%x len=%u -> statut %u "
                "(pc %u), fence %u\n", job->off, job->len, st, pc,
                qatomic_read(&s->regs[QGPU_REG_FENCE >> 2]));
    }
    if (s->irq_bh) {
        qemu_bh_schedule(s->irq_bh);
    }
}

static void *qgpu_render_thread(void *opaque)
{
    QgpuPCIState *s = opaque;
    bool fini;

    qemu_mutex_lock(&s->lock);
    for (;;) {
        QgpuJob job;

        while (!s->q_stop && !s->q_reset && s->q_count == 0) {
            qemu_cond_wait(&s->cond_work, &s->lock);
        }
        if (s->q_stop) {
            break;
        }
        if (s->q_reset) {
            /* Demandé file vide (qgpu_soft_reset draine d'abord) : aucune
               soumission ne peut s'intercaler. */
            qemu_mutex_unlock(&s->lock);
            qgpu_core_reset(&s->core);
            qemu_mutex_lock(&s->lock);
            s->q_reset = false;
            qemu_cond_broadcast(&s->cond_done);
            continue;
        }
        /* La soumission reste DANS la file pendant son exécution : c'est ce qui
           fait que QGPU_REG_DOORBELL lu vaut « en attente + en cours ». */
        job = s->queue[s->q_head];
        s->q_running = true;
        qemu_mutex_unlock(&s->lock);

        qgpu_run_job(s, &job);          /* hors verrou, hors BQL */

        qemu_mutex_lock(&s->lock);
        s->q_head = (s->q_head + 1) % QGPU_QUEUE_DEPTH;
        s->q_count--;
        s->q_running = false;
        qemu_cond_broadcast(&s->cond_done);
    }
    fini = s->q_fini;
    qemu_mutex_unlock(&s->lock);
    /* Retrait du device : le backend est libéré par le thread qui le possède.
       Pas à la sortie de QEMU (q_fini faux) : les bibliothèques GL ont pu être
       démontées avant les notificateurs de sortie, et la v8 n'y touchait pas. */
    if (fini && s->core_ok) {
        qgpu_core_fini(&s->core);
        s->core_ok = false;
    }
    return NULL;
}

/* Attend que la file soit vide. `discard` jette ce qui n'a pas commencé (reset)
   et n'attend alors que la soumission en cours. À appeler `lock` pris. */
static void qgpu_drain_locked(QgpuPCIState *s, bool discard)
{
    if (discard) {
        /* Ne reste que celle qui est DÉJÀ entre les mains du thread : les
           autres n'avanceront jamais FENCE — c'est dit dans qgpu_proto.h. */
        s->q_count = s->q_running ? 1 : 0;
    }
    while (s->q_count > 0) {
        qemu_cond_wait(&s->cond_done, &s->lock);
    }
}

/* Écriture de QGPU_REG_DOORBELL. Appelée par le vCPU, BQL pris. */
static void qgpu_doorbell(QgpuPCIState *s, bool async)
{
    QgpuJob job;
    uint32_t target;

    job.off = s->regs[QGPU_REG_SUBMIT_OFF >> 2];
    job.len = s->regs[QGPU_REG_SUBMIT_LEN >> 2];

    if (!s->thread_ok) {
        /* Pas de thread (realize incomplet) : v8 à l'identique. */
        s->regs[QGPU_REG_FENCE_SUBMITTED >> 2]++;
        qgpu_run_job(s, &job);
        s->regs[QGPU_REG_IRQ >> 2] |= QGPU_IRQ_DONE;
        qgpu_update_irq(s);
        s->regs[QGPU_REG_SUBMIT_ST >> 2] = QGPU_ST_OK;
        return;
    }

    qemu_mutex_lock(&s->lock);
    if (async && s->q_count == QGPU_QUEUE_DEPTH) {
        /* Refus : rien n'est mis en file, aucun compteur n'avance. */
        qemu_mutex_unlock(&s->lock);
        s->regs[QGPU_REG_SUBMIT_ST >> 2] = QGPU_ST_QUEUE_FULL;
        if (s->trace) {
            fprintf(stderr, "qgpu-pci: file pleine, soumission asynchrone "
                    "off=0x%x len=%u refusée\n", job.off, job.len);
        }
        return;
    }
    /* Synchrone : jamais de refus, on attend une place. Un invité v1–v8 ne
       peut de toute façon pas remplir la file — il en vide une à la fois. */
    while (s->q_count == QGPU_QUEUE_DEPTH) {
        qemu_cond_wait(&s->cond_done, &s->lock);
    }
    s->queue[(s->q_head + s->q_count) % QGPU_QUEUE_DEPTH] = job;
    s->q_count++;
    target = ++s->q_submitted;
    qemu_cond_signal(&s->cond_work);
    if (!async) {
        /* Le BQL reste pris pendant l'attente : c'est exactement ce que
           faisait la v8, donc aucune régression de comportement pour un
           invité synchrone. */
        while ((int32_t)(qatomic_read(&s->regs[QGPU_REG_FENCE >> 2]) - target)
               < 0) {
            qemu_cond_wait(&s->cond_done, &s->lock);
        }
    }
    qemu_mutex_unlock(&s->lock);
    /* FENCE_SUBMITTED n'est écrit que par le vCPU, sous BQL : pas d'atomique. */
    s->regs[QGPU_REG_FENCE_SUBMITTED >> 2] = target;
    s->regs[QGPU_REG_SUBMIT_ST >> 2] = QGPU_ST_OK;
}

static void qgpu_soft_reset(QgpuPCIState *s)
{
    if (s->thread_ok) {
        qemu_mutex_lock(&s->lock);
        qgpu_drain_locked(s, true);
        s->q_head = 0;
        s->q_submitted = 0;
        qemu_mutex_unlock(&s->lock);
    }
    if (s->irq_bh) {
        /* Un BH encore en attente lèverait DONE APRÈS le reset, pour une
           soumission qui n'existe plus du point de vue de l'invité. */
        qemu_bh_cancel(s->irq_bh);
    }
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[QGPU_REG_MAGIC >> 2] = QGPU_MAGIC;
    s->regs[QGPU_REG_VERSION >> 2] = QGPU_PROTO_VERSION;
    /* core.caps et non be->cap : ce que init() a résolu à chaud (v8 : requêtes
       d'occlusion ; v10 : textures) doit atteindre l'invité. */
    s->regs[QGPU_REG_CAPS >> 2] = s->core_ok ? s->core.caps : 0;
    if (s->thread_ok) {
        s->regs[QGPU_REG_CAPS >> 2] |= QGPU_CAP_ASYNC;
    }
    s->regs[QGPU_REG_SHMEM_SIZE >> 2] = s->shmem_mb * MiB;
    s->regs[QGPU_REG_BACKEND_NAME >> 2] = qgpu_core_backend_tag(&s->core);
    s->regs[QGPU_REG_QUEUE_DEPTH >> 2] = QGPU_QUEUE_DEPTH;
    if (s->core_ok && s->thread_ok) {
        /* Par le thread de rendu, qui possède le contexte (cf. en tête). */
        qemu_mutex_lock(&s->lock);
        s->q_reset = true;
        qemu_cond_signal(&s->cond_work);
        while (s->q_reset) {
            qemu_cond_wait(&s->cond_done, &s->lock);
        }
        qemu_mutex_unlock(&s->lock);
    } else if (s->core_ok) {
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
    case QGPU_REG_QUEUE_FREE: {
        unsigned busy = 0;

        if (s->thread_ok) {
            qemu_mutex_lock(&s->lock);
            busy = s->q_count;
            qemu_mutex_unlock(&s->lock);
        }
        /* Section critique minuscule, et le thread de rendu ne prend jamais le
           BQL : ce verrou pris sous BQL ne peut pas s'inverser. */
        return addr == QGPU_REG_DOORBELL ? busy : QGPU_QUEUE_DEPTH - busy;
    }
    case QGPU_REG_FENCE:
        /* Pendant du release de qgpu_run_job : voir FENCE avancer, c'est voir
           tout ce que la soumission a publié avant. */
        return qatomic_load_acquire(&s->regs[QGPU_REG_FENCE >> 2]);
    case QGPU_REG_STATUS:
    case QGPU_REG_STATUS_PC:
    case QGPU_REG_ERRORS:
        return qatomic_read(&s->regs[addr >> 2]);
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
        /* Sans le bit 0, rien ne part (v1). Avec le bit 1 en plus, la
           soumission est mise en file au lieu d'être attendue (v9). */
        if (v & QGPU_DOORBELL_GO) {
            qgpu_doorbell(s, (v & QGPU_DOORBELL_ASYNC) != 0);
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

static void qgpu_qfb_dirty(void *opaque, uint32_t off, uint32_t len)
{
    MemoryRegion *mr = opaque;

    if (mr && len) {
        memory_region_set_dirty(mr, off, len);
    }
}

/* `complain` : seulement depuis le notifier machine-done. Au realize, les autres
   devices peuvent ne pas être nés — ne rien trouver n'y veut encore rien dire. */
static void qgpu_bind_scanout(QgpuPCIState *s, bool complain)
{
    uint8_t *ram = NULL;
    uint32_t size = 0;
    MemoryRegion *mr = NULL;
    const char *which;

    if (s->core.scanout) {
        return;
    }
    if (qfb_scanout_info(&ram, &size, &mr) && ram) {
        which = "qfb-pci";
    } else if (qgpu_vga_scanout_info(&ram, &size, &mr)) {
        which = "VGA";
    } else {
        if (complain) {
            warn_report("qgpu-pci: aucun écran à présenter (ni qfb-pci ni VGA) :"
                        " SURF_PRESENT sera refusé et l'invité relira ses"
                        " images");
        }
        return;
    }
    qgpu_core_set_scanout(&s->core, ram, size, qgpu_qfb_dirty, mr);
    s->core.caps |= QGPU_CAP_SCANOUT;
    if (s->core_ok) {
        s->regs[QGPU_REG_CAPS >> 2] = s->core.caps |
            (s->thread_ok ? QGPU_CAP_ASYNC : 0);
    }
    if (s->trace) {
        fprintf(stderr, "qgpu-pci: scanout sur %s, %u Mio\n", which,
                size / (unsigned)MiB);
    }
}

static void qgpu_machine_done(Notifier *n, void *unused)
{
    QgpuPCIState *s = container_of(n, QgpuPCIState, machine_done);

    qgpu_bind_scanout(s, true);
}

/* Arrête le thread de rendu après avoir laissé finir ce qui est en cours.
   Idempotent : appelé à la sortie de QEMU ET à la destruction du device. */
static void qgpu_stop_thread(QgpuPCIState *s, bool fini)
{
    if (!s->thread_ok) {
        return;
    }
    qemu_mutex_lock(&s->lock);
    qgpu_drain_locked(s, true);
    s->q_stop = true;
    s->q_fini = fini;
    qemu_cond_signal(&s->cond_work);
    qemu_mutex_unlock(&s->lock);
    qemu_thread_join(&s->render_thread);
    s->thread_ok = false;
}

static void qgpu_exit_notify(Notifier *n, void *data)
{
    qgpu_stop_thread(container_of(n, QgpuPCIState, exit_notifier), false);
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

    /* v9 : la file et son thread. Créés AVANT le premier reset, pour que
       QGPU_REG_CAPS annonce QGPU_CAP_ASYNC dès la première lecture. */
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->cond_work);
    qemu_cond_init(&s->cond_done);
    s->irq_bh = qemu_bh_new(qgpu_irq_bh, s);
    s->thread_ok = true;
    qemu_thread_create(&s->render_thread, "qgpu-render", qgpu_render_thread, s,
                       QEMU_THREAD_JOINABLE);
    s->exit_notifier.notify = qgpu_exit_notify;
    qemu_add_exit_notifier(&s->exit_notifier);
    s->machine_done.notify = qgpu_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);

    qgpu_soft_reset(s);
    qgpu_bind_scanout(s, false);
}

static void qgpu_pci_exit(PCIDevice *dev)
{
    QgpuPCIState *s = QGPU_PCI(dev);

    if (s->thread_ok) {
        qemu_remove_exit_notifier(&s->exit_notifier);
        qemu_remove_machine_init_done_notifier(&s->machine_done);
        qgpu_stop_thread(s, true);
        qemu_cond_destroy(&s->cond_done);
        qemu_cond_destroy(&s->cond_work);
        qemu_mutex_destroy(&s->lock);
    }
    if (s->irq_bh) {
        qemu_bh_delete(s->irq_bh);
        s->irq_bh = NULL;
    }
    /* Normalement déjà libéré par le thread (q_fini). Reste le cas d'un
       thread jamais créé, ou déjà arrêté à la sortie de QEMU sans libération. */
    if (s->core_ok) {
        qgpu_core_set_scanout(&s->core, NULL, 0, NULL, NULL);
        qgpu_core_fini(&s->core);
        s->core_ok = false;
    }
    qemu_free_irq(s->irq);
}

static void qgpu_pci_reset_handler(DeviceState *d)
{
    qgpu_soft_reset(QGPU_PCI(d));
}

static int qgpu_pre_save(void *opaque)
{
    /* Hors périmètre, mais au moins cohérent : rien ne doit être en vol
       pendant qu'on recopie les registres et BAR0. */
    QgpuPCIState *s = opaque;

    if (s->thread_ok) {
        qemu_mutex_lock(&s->lock);
        qgpu_drain_locked(s, false);
        qemu_mutex_unlock(&s->lock);
    }
    return 0;
}

static int qgpu_post_load(void *opaque, int version_id)
{
    /* Les objets hôte (contextes, surfaces) ne migrent pas : l'invité repart
       d'un device vide, FENCE conservée pour ne pas dérouter un client. */
    QgpuPCIState *s = opaque;
    uint32_t fence = s->regs[QGPU_REG_FENCE >> 2];

    qgpu_soft_reset(s);
    s->regs[QGPU_REG_FENCE >> 2] = fence;
    s->regs[QGPU_REG_FENCE_SUBMITTED >> 2] = fence;
    s->q_submitted = fence;
    return 0;
}

/* version_id 2 : la fenêtre de registres est passée de 0x40 à 0x50 octets
   (v9), donc le tableau sauvé n'a plus la même taille qu'en v8. */
static const VMStateDescription vmstate_qgpu_pci = {
    .name = "qgpu-pci",
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save = qgpu_pre_save,
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
