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
 * Migration et retrait à chaud : HORS PÉRIMÈTRE, et maintenant dit à voix
 * haute. Les objets hôte (contextes GL, surfaces, textures) ne migrent pas, et
 * post_load remettait QGPU_REG_IRQ_MASK à zéro sans que le kext le réarme
 * jamais : après un `loadvm`, la 3D était morte en silence. Un blocage de
 * migration (migrate_add_blocker) refuse maintenant savevm/loadvm/migrate avec
 * une raison lisible, plutôt que de rendre une VM à moitié vivante.
 *
 * Cible de présentation. SURF_PRESENT écrit dans la VRAM d'un écran, choisi
 * par la propriété `scanout` :
 *
 *   auto  (défaut)  VGA d'abord, qfb-pci sinon ;
 *   qfb             qfb-pci seulement ;
 *   vga             framebuffer VGA seulement ;
 *   none            aucune (SURF_PRESENT refusé, l'invité relit ses images).
 *
 * VGA en premier et non qfb-pci, contrairement à la v13 : run_tiger.sh ajoute
 * qfb-pci en SECOND moniteur, et le plugin invité lit l'adresse de sa fenêtre
 * sur CGMainDisplayID, qui est le VGA. Présenter dans la VRAM de qfb revenait
 * à écrire sur l'autre écran, au pas d'un autre écran : l'image du jeu restait
 * figée, statut OK, aucun message (rapport du 22/09/2026, §8.3, Q1).
 *
 *   -device qgpu-pci[,shmem_mb=64][,backend=auto|soft|gl][,trace=on]
 *                   [,scanout=auto|qfb|vga|none]
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/atomic.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "migration/blocker.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "qom/object.h"
#include "sysemu/sysemu.h"

#include "qgpu-core.h"

/* ── Ce que qfb-pci.c publie (v16) ───────────────────────────────────────────
 *
 * ATTENTION : cette déclaration est DUPLIQUÉE à l'identique dans qfb-pci.c.
 * scripts/build_qemu_qfb.sh ne recopie que des .c dans hw/display/ — il n'y a
 * pas d'en-tête partagé où la mettre. Toute modification se fait des deux
 * côtés, dans le même commit.
 */
typedef struct QfbScanoutInfo {
    uint8_t      *ram;        /* origine de la VRAM (BAR0 du device) */
    uint32_t      vram_size;  /* taille totale de la VRAM, en octets */
    uint32_t      base;       /* offset de la première ligne VISIBLE */
    uint32_t      stride;     /* octets par ligne */
    uint32_t      width;      /* pixels */
    uint32_t      height;     /* lignes réellement contenues dans la VRAM */
    uint32_t      depth;      /* bits par pixel programmés (1, 2, 4, 8, 16, 24) */
    MemoryRegion *mr;         /* les offsets de memory_region_set_dirty sont
                                 relatifs à CETTE région, donc à `ram` */
} QfbScanoutInfo;

int qfb_scanout_info(QfbScanoutInfo *info);
void qfb_scanout_set_notifier(void (*cb)(void *opaque), void *opaque);

/* Le framebuffer VGA standard : sur mac99 c'est l'écran principal (pci_vga_init,
 * piloté dans Tiger par qemu_vga.ndrv), celui que le plugin invité interroge
 * (CGMainDisplayID) et donc celui dans lequel il attend son image.
 *
 * Sa VRAM s'atteint sans toucher au code amont : toute MemoryRegion nommée est
 * un enfant QOM de son propriétaire (memory_region_do_init), donc « vga.vram »
 * devient l'enfant « vga.vram[0] » du device « VGA ». Les pixels y sont gros
 * boutistes sur PowerPC, ce que SURF_PRESENT écrit déjà.
 *
 * La géométrie, elle, n'est pas publiée par le device amont. On la lit sur la
 * SURFACE de la console : quand le backend d'affichage sait prendre le format
 * de la VRAM, QEMU pose la surface directement dessus (vga_draw_graphic,
 * `!allocate_surface`), et l'écart entre surface_data() et l'origine de la
 * région donne alors l'offset RÉEL de la fenêtre visible — c'est la seule
 * façon de ne pas supposer que cet offset vaut 0. Quand la surface est une
 * copie (mode indexé, format refusé par l'interface), rien n'est observable :
 * on retombe sur « toute la VRAM », ce que faisait la v13, et `stride` reste à
 * 0 pour dire au cœur qu'il n'a pas de pas à vérifier.
 */
static bool qgpu_vga_scanout_info(QfbScanoutInfo *info)
{
    Object *dev = object_resolve_path_type("", "VGA", NULL);
    Object *child = dev ? object_resolve_path_component(dev, "vga.vram[0]")
                        : NULL;
    MemoryRegion *m = child ? MEMORY_REGION(object_dynamic_cast(child,
                                            TYPE_MEMORY_REGION)) : NULL;
    QemuConsole *con;
    DisplaySurface *surface;
    uint8_t *ram;

    if (!m || !memory_region_is_ram(m)) {
        return false;
    }
    ram = memory_region_get_ram_ptr(m);
    if (!ram) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    info->ram       = ram;
    info->vram_size = (uint32_t)memory_region_size(m);
    info->base      = 0;
    info->mr        = m;

    con = qemu_console_lookup_by_device(DEVICE(dev), 0);
    surface = con ? qemu_console_surface(con) : NULL;
    if (surface && !surface_is_allocated(surface) &&
        !surface_is_placeholder(surface)) {
        uint8_t *data = surface_data(surface);

        if (data >= ram && data < ram + info->vram_size) {
            info->base   = (uint32_t)(data - ram);
            info->stride = (uint32_t)surface_stride(surface);
            info->width  = (uint32_t)surface_width(surface);
            info->height = (uint32_t)surface_height(surface);
            info->depth  = (uint32_t)surface_bits_per_pixel(surface);
        }
    }
    return true;
}

#define TYPE_QGPU_PCI "qgpu-pci"
OBJECT_DECLARE_SIMPLE_TYPE(QgpuPCIState, QGPU_PCI)

#define QGPU_NUM_REGS (QGPU_CTRL_TOPADDR / sizeof(uint32_t))

/*
 * D2 — échéance d'une attente tenue BQL PRIS, en millisecondes.
 *
 * Choisie entre les deux délais que l'invité s'impose déjà, pour que ce soit
 * toujours le device qui rende la main le premier, avec un statut, plutôt que
 * le chien de garde de l'invité qui conclue tout seul :
 *   - le kext borne son drainage de file à 1 s (drainQueue : 5000 × IODelay
 *     de 200 µs) ;
 *   - le plugin abandonne une barrière au bout de WAIT_MS = 5 s
 *     (pomppc_accel.c) et coupe alors l'accélération pour de bon.
 * 2 s laissent donc passer la pire image légitime (compilation de shaders,
 * téléversement de texture sur un GPU hôte chargé) tout en garantissant qu'une
 * boucle d'affichage bloquée ne gèle pas QEMU plus de deux secondes.
 */
#define QGPU_SYNC_WAIT_MS 2000

/* Cible de SURF_PRESENT effectivement liée. `base` existe pour que le
   marquage « sale » ne suppose PAS que la fenêtre visible commence à
   l'offset 0 de la région : le cœur reçoit `ram + base` et compte à partir de
   là, la région, elle, compte à partir de son origine. */
typedef struct QgpuScanout {
    MemoryRegion *mr;      /* VRAM ; RÉFÉRENCÉE tant qu'elle est liée */
    QemuConsole  *con;     /* VGA seulement : d'où se relit la géométrie */
    const char   *which;   /* "qfb-pci" ou "VGA", pour les messages */
    uint8_t      *ram;     /* origine de la fenêtre visible (déjà + base) */
    uint32_t      base;    /* offset de cette fenêtre DANS mr */
    uint32_t      size;    /* octets de la fenêtre visible */
    uint32_t      stride;  /* 0 = pas inconnu (fenêtre = toute la VRAM) */
    uint32_t      width, height, depth;
    /* Ce à quoi ressemblait la surface de `con` au moment du lien : de quoi
       repérer un changement de mode VGA sans refaire une résolution QOM. */
    uint8_t      *surf_data;
    uint32_t      surf_stride, surf_height;
} QgpuScanout;

/* Valeurs de la propriété `scanout`. */
enum {
    QGPU_SCANOUT_AUTO,
    QGPU_SCANOUT_QFB,
    QGPU_SCANOUT_VGA,
    QGPU_SCANOUT_NONE,
};

/* Une soumission en attente : ce que le doorbell a lu dans les registres.
   v19 : ou une destruction de tranche (QGPU_REG_CLIENT_RESET), qui prend la
   même file et la même barrière — `off` porte alors l'index de la tranche. */
enum { QGPU_JOB_STREAM = 0, QGPU_JOB_CLIENT_RESET = 1 };
typedef struct QgpuJob {
    uint32_t off, len;
    uint32_t kind;
} QgpuJob;

struct QgpuPCIState {
    PCIDevice parent_obj;

    MemoryRegion mem_shmem;
    MemoryRegion mem_ctrl;
    uint8_t *shmem;
    uint32_t shmem_mb;
    char *backend;
    char *scanout_pref;            /* propriété : auto | qfb | vga | none */
    int  scanout_mode;             /* QGPU_SCANOUT_*, résolu au realize */
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
    Notifier   machine_done;       /* v13 : lier l'écran une fois tous les devices nés */
    QgpuScanout scanout;           /* cible de SURF_PRESENT (v16) */
    Error     *mig_blocker;        /* D3 : la 3D ne survit pas à loadvm */
};

/*
 * ISOLATION ENTRE CLIENTS — état des lieux, assumé.
 *
 * Le kext donne à chaque processus invité une tranche de BAR0 et une plage
 * d'identifiants (qgpu_proto.h, « Interface du kext »), mais rien ici ni dans
 * le cœur ne l'y CONTRAINT : les offsets écrits dans le flux sont absolus dans
 * la fenêtre partagée, et les identifiants sont bornés sur tout l'espace, pas
 * sur la tranche du client. Un processus invité peut donc lire la surface d'un
 * autre, ou détruire ses objets.
 *
 * Ce n'est pas corrigé, et c'est un choix : il n'existe aucun registre
 * QGPU_REG_SUBMIT_BASE / QGPU_REG_ID_BASE que le kext pourrait poser avant
 * chaque doorbell pour que le device recopie ces bornes dans le job — les
 * ajouter change qgpu_proto.h, donc le kext, le plugin et le harnais en même
 * temps. Tant qu'ils n'existent pas, LES CLIENTS SE FONT CONFIANCE : la
 * frontière de sécurité est celle de l'invité tout entier (une VM de jeu), pas
 * celle d'un processus. Rien de ce qu'un client peut faire ne sort de BAR0 ni
 * ne touche l'hôte — le cœur borne tout sur la fenêtre partagée.
 */

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

    if (s->core_ok && job->kind == QGPU_JOB_CLIENT_RESET) {
        st = qgpu_core_client_reset(&s->core, job->off);
        pc = 0;
    } else if (s->core_ok) {
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
    /* Les relectures (SURF_READBACK, QUERY_RESULT, DEPTH_READBACK…) sont
       écrites DANS BAR0 par l'hôte, derrière le dos de QEMU : sans ce
       marquage, le suivi des pages sales ignore ces octets. Seul le cœur sait
       quels rectangles il a écrits, et il ne les rapporte pas — on marque donc
       toute la fenêtre, ce qui coûte quelques centaines de mots de bitmap
       (~1 µs pour 64 Mio) contre une soumission qui en coûte des centaines de
       fois plus. C'est aussi ce marquage qui impose rcu_register_thread() en
       tête de ce thread (D1) : il finit dans ram_list.dirty_memory, lu sous
       RCU. */
    memory_region_set_dirty(&s->mem_shmem, 0, (uint64_t)s->shmem_mb * MiB);
    if (s->trace && job->kind == QGPU_JOB_CLIENT_RESET) {
        fprintf(stderr, "qgpu-pci: destruction de la tranche %u -> statut %u, "
                "fence %u\n", job->off, st,
                qatomic_read(&s->regs[QGPU_REG_FENCE >> 2]));
    } else if (s->trace) {
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

    /* D1 — ce thread appelle memory_region_set_dirty (relectures dans BAR0 et
       SURF_PRESENT dans la VRAM), qui lit ram_list.dirty_memory SOUS RCU. Un
       thread inconnu du domaine RCU est invisible pour synchronize_rcu() : le
       lecteur peut alors travailler sur un tableau déjà libéré. Deux lignes,
       et la règle de QEMU est explicite — tout thread qui touche à la mémoire
       invité s'enregistre. */
    rcu_register_thread();

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
    rcu_unregister_thread();
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

/* Idem, mais borné : rend false si la file n'est pas vide à l'échéance. Sert
   partout où l'attente se fait BQL PRIS (D2). `lock` pris. */
static bool qgpu_drain_timed_locked(QgpuPCIState *s, int ms)
{
    int64_t deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + ms;

    while (s->q_count > 0) {
        int64_t left = deadline - qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

        if (left <= 0) {
            return false;
        }
        qemu_cond_timedwait(&s->cond_done, &s->lock, (int)left);
    }
    return true;
}

static void qgpu_scanout_recheck(QgpuPCIState *s);

/*
 * D2 — l'échéance d'un doorbell SYNCHRONE est passée : rendre la main à
 * l'invité avec une erreur, plutôt que de tenir QEMU gelé.
 *
 * `queued` dit si la soumission est bien partie (et finira donc par faire
 * avancer FENCE, plus tard) ou si elle n'a jamais trouvé de place.
 *
 * QGPU_REG_STATUS et non seulement QGPU_REG_SUBMIT_ST : le kext ne lit
 * SUBMIT_ST que dans le chemin ASYNCHRONE (POMPPCGPU.cpp, submitGated) ; après
 * un doorbell synchrone il relit FENCE, STATUS et STATUS_PC. Sans écrire
 * STATUS, l'invité relirait le statut de la soumission PRÉCÉDENTE et conclurait
 * « tout va bien » sur une image qui n'est pas rendue. QGPU_REG_ERRORS, en
 * revanche, ne bouge pas : il compte les soumissions TERMINÉES en erreur, et
 * celle-ci n'est pas terminée — c'est le thread qui l'incrémentera s'il y a
 * lieu. Les deux écritures sont atomiques : le thread de rendu écrit les mêmes
 * registres sans verrou.
 */
static void qgpu_sync_gave_up(QgpuPCIState *s, uint32_t target, bool queued,
                              const char *why)
{
    qatomic_set(&s->regs[QGPU_REG_STATUS_PC >> 2], 0);
    qatomic_set(&s->regs[QGPU_REG_STATUS >> 2], QGPU_ST_BACKEND);
    s->regs[QGPU_REG_SUBMIT_ST >> 2] = QGPU_ST_BACKEND;
    if (queued) {
        s->regs[QGPU_REG_FENCE_SUBMITTED >> 2] = target;
    }
    warn_report_once("qgpu-pci: doorbell synchrone abandonné après %d ms (%s) ;"
                     " l'invité reprend la main avec QGPU_ST_BACKEND. Le GPU"
                     " hôte est-il bloqué ?", QGPU_SYNC_WAIT_MS, why);
    if (s->trace) {
        fprintf(stderr, "qgpu-pci: échéance %d ms dépassée (%s), barrière"
                " visée %u, atteinte %u\n", QGPU_SYNC_WAIT_MS, why, target,
                qatomic_read(&s->regs[QGPU_REG_FENCE >> 2]));
    }
}

/* Écriture de QGPU_REG_DOORBELL. Appelée par le vCPU, BQL pris. */
static void qgpu_doorbell(QgpuPCIState *s, bool async)
{
    QgpuJob job;
    uint32_t target;

    job.off = s->regs[QGPU_REG_SUBMIT_OFF >> 2];
    job.len = s->regs[QGPU_REG_SUBMIT_LEN >> 2];
    job.kind = QGPU_JOB_STREAM;

    if (!s->thread_ok) {
        /* Pas de thread (realize incomplet) : v8 à l'identique. */
        s->regs[QGPU_REG_FENCE_SUBMITTED >> 2]++;
        qgpu_run_job(s, &job);
        s->regs[QGPU_REG_IRQ >> 2] |= QGPU_IRQ_DONE;
        qgpu_update_irq(s);
        s->regs[QGPU_REG_SUBMIT_ST >> 2] = QGPU_ST_OK;
        return;
    }

    /* La géométrie de l'écran a-t-elle bougé depuis la dernière soumission ?
       (VGA n'a pas de notification, cf. qgpu_scanout_recheck.) Avant de
       prendre le verrou : le rebinding draine la file. */
    qgpu_scanout_recheck(s);

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
    /* Synchrone : jamais de refus, on attend une place — mais BORNÉ, comme
       l'attente de fin plus bas, et pour la même raison. */
    if (s->q_count == QGPU_QUEUE_DEPTH &&
        !qgpu_drain_timed_locked(s, QGPU_SYNC_WAIT_MS)) {
        if (s->q_count == QGPU_QUEUE_DEPTH) {
            qemu_mutex_unlock(&s->lock);
            qgpu_sync_gave_up(s, 0, false, "aucune place dans la file");
            return;
        }
    }
    s->queue[(s->q_head + s->q_count) % QGPU_QUEUE_DEPTH] = job;
    s->q_count++;
    target = ++s->q_submitted;
    qemu_cond_signal(&s->cond_work);
    if (!async) {
        /*
         * D2 — LE BQL EST CONSERVÉ PENDANT CETTE ATTENTE, ET C'EST VOULU.
         *
         * Le relâcher (bql_unlock/bql_lock autour du wait) semble évident et
         * serait faux sur QEMU 9.2 : ce chemin part d'un accès MMIO, donc
         * s'exécute avec `mem_reentrancy_guard.engaged_in_io` armé sur ce
         * device. Dès que le BQL est rendu, un second vCPU peut entrer dans le
         * même device ; memory_region_dispatch_write le refuse alors
         * (« Blocked re-entrant IO », MEMTX_ACCESS_ERROR) et la soumission de
         * l'autre cœur est perdue sans un mot. Pire pour un reset ou un
         * loadvm, qui deviennent réentrants. Relâcher le BQL suppose d'abord
         * `disable_reentrancy_guard` et une preuve qu'aucun chemin réentrant
         * n'existe : ce n'est pas ce correctif-ci.
         *
         * Ce qui est corrigé ici, c'est l'ABSENCE DE DÉLAI MAXIMAL : la v9
         * attendait indéfiniment. Un pilote GL hôte bloqué figeait QEMU en
         * entier — plus de rafraîchissement d'écran, plus de timers, plus de
         * monitor, second vCPU arrêté à son premier MMIO. Maintenant, au pire
         * QGPU_SYNC_WAIT_MS, puis l'invité repart avec QGPU_ST_BACKEND et se
         * replie sur le rendu logiciel.
         *
         * La vraie sortie reste la suivante, côté invité : supprimer le
         * doorbell synchrone (soumission asynchrone + barrière), qui endort le
         * fil invité au lieu de bloquer le vCPU. Cela suppose K5 et K6.
         */
        int64_t deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                         + QGPU_SYNC_WAIT_MS;

        while ((int32_t)(qatomic_read(&s->regs[QGPU_REG_FENCE >> 2]) - target)
               < 0) {
            int64_t left = deadline - qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

            if (left <= 0) {
                qemu_mutex_unlock(&s->lock);
                qgpu_sync_gave_up(s, target, true, "rendu non terminé");
                return;
            }
            qemu_cond_timedwait(&s->cond_done, &s->lock, (int)left);
        }
    }
    qemu_mutex_unlock(&s->lock);
    /* FENCE_SUBMITTED n'est écrit que par le vCPU, sous BQL : pas d'atomique. */
    s->regs[QGPU_REG_FENCE_SUBMITTED >> 2] = target;
    s->regs[QGPU_REG_SUBMIT_ST >> 2] = QGPU_ST_OK;
}

/* v19 — écriture de QGPU_REG_CLIENT_RESET : détruire les objets d'une
 * tranche. Toujours EN FILE (jamais d'attente BQL pris : un kext qui ferme un
 * client dort ensuite sur la barrière, il n'a pas besoin que l'écriture
 * bloque), FIFO derrière tout ce qui est en vol — y compris les dernières
 * soumissions du client mort, qui lisaient encore sa tranche. Mêmes registres
 * de réponse qu'un doorbell asynchrone : SUBMIT_ST (OK, QUEUE_FULL, ou
 * BAD_ARG pour un index hors table — rien n'est alors mis en file) et
 * FENCE_SUBMITTED. Sans thread, exécuté sur place, comme un doorbell v8. */
static void qgpu_client_reset(QgpuPCIState *s, uint32_t slot)
{
    QgpuJob job;
    uint32_t target;

    if (slot >= s->regs[QGPU_REG_CLIENTS >> 2]) {
        s->regs[QGPU_REG_SUBMIT_ST >> 2] = QGPU_ST_BAD_ARG;
        return;
    }
    job.off = slot;
    job.len = 0;
    job.kind = QGPU_JOB_CLIENT_RESET;
    if (!s->thread_ok) {
        s->regs[QGPU_REG_FENCE_SUBMITTED >> 2]++;
        qgpu_run_job(s, &job);
        s->regs[QGPU_REG_IRQ >> 2] |= QGPU_IRQ_DONE;
        qgpu_update_irq(s);
        s->regs[QGPU_REG_SUBMIT_ST >> 2] = QGPU_ST_OK;
        return;
    }
    qemu_mutex_lock(&s->lock);
    if (s->q_count == QGPU_QUEUE_DEPTH) {
        qemu_mutex_unlock(&s->lock);
        s->regs[QGPU_REG_SUBMIT_ST >> 2] = QGPU_ST_QUEUE_FULL;
        return;
    }
    s->queue[(s->q_head + s->q_count) % QGPU_QUEUE_DEPTH] = job;
    s->q_count++;
    target = ++s->q_submitted;
    qemu_cond_signal(&s->cond_work);
    qemu_mutex_unlock(&s->lock);
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
    /* v19 : la disposition des clients, lue par le kext (CLIENTS) et par le
       plugin (LAYOUT). Un cœur absent ne publie rien : le kext refuse alors
       de démarrer, ce qui vaut mieux qu'un transport vers le vide. */
    if (s->core_ok) {
        uint32_t k;
        s->regs[QGPU_REG_CLIENTS >> 2] = QGPU_MAX_CLIENTS;
        for (k = 0; k < QGPU_REG_LAYOUT_CLASSES; k++) {
            s->regs[QGPU_REG_LAYOUT_CLASS(k) >> 2] = qgpu_core_client_ids(k);
        }
    }
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
    case QGPU_REG_CLIENT_RESET:
        qgpu_client_reset(s, v);        /* v19 */
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

/* Marque sale ce que le thread de rendu vient d'écrire dans la VRAM. `off` est
   relatif à la FENÊTRE VISIBLE (c'est ce que le cœur compte), la région, elle,
   compte depuis son origine : d'où `base`. Appelé sans BQL, depuis le thread
   de rendu — d'où rcu_register_thread() (D1). */
static void qgpu_scanout_dirty(void *opaque, uint32_t off, uint32_t len)
{
    QgpuScanout *sc = opaque;

    if (sc->mr && len) {
        memory_region_set_dirty(sc->mr, (uint64_t)sc->base + off, len);
    }
}

/* Détache la cible courante. File drainée, BQL pris. */
static void qgpu_unbind_scanout(QgpuPCIState *s)
{
    if (!s->scanout.mr) {
        return;
    }
    if (s->core_ok) {
        qgpu_core_set_scanout(&s->core, NULL, 0, NULL, NULL);
    }
    /* Symétrique du memory_region_ref() du lien : c'est lui qui garantit que
       la VRAM ne peut pas être libérée pendant que le thread de rendu y
       écrit. On ne retire PAS le journal DIRTY_MEMORY_VGA : il appartient au
       propriétaire de la région, qui peut en avoir besoin. */
    memory_region_unref(s->scanout.mr);
    memset(&s->scanout, 0, sizeof(s->scanout));
    s->core.caps &= ~QGPU_CAP_SCANOUT;
    if (s->core_ok) {
        s->regs[QGPU_REG_CAPS >> 2] = s->core.caps |
            (s->thread_ok ? QGPU_CAP_ASYNC : 0);
    }
}

/* Interroge la cible demandée par la propriété `scanout`. */
static bool qgpu_probe_scanout(QgpuPCIState *s, QfbScanoutInfo *info,
                               const char **which, bool *is_vga)
{
    memset(info, 0, sizeof(*info));
    switch (s->scanout_mode) {
    case QGPU_SCANOUT_NONE:
        return false;
    case QGPU_SCANOUT_QFB:
        *which = "qfb-pci";
        *is_vga = false;
        return qfb_scanout_info(info) != 0;
    case QGPU_SCANOUT_VGA:
        *which = "VGA";
        *is_vga = true;
        return qgpu_vga_scanout_info(info);
    default:
        /* `auto` : VGA D'ABORD. C'est l'écran principal de la mac99, donc
           celui que le plugin invité interroge (Q1). qfb-pci ne vient qu'à
           défaut de VGA. */
        if (qgpu_vga_scanout_info(info)) {
            *which = "VGA";
            *is_vga = true;
            return true;
        }
        memset(info, 0, sizeof(*info));
        if (qfb_scanout_info(info)) {
            *which = "qfb-pci";
            *is_vga = false;
            return true;
        }
        return false;
    }
}

/*
 * (Re)lie la cible de SURF_PRESENT. `complain` : seulement depuis le notifier
 * machine-done ; au realize, les autres devices peuvent ne pas être nés, ne
 * rien trouver n'y veut encore rien dire.
 *
 * Appelé aussi à chaque changement de géométrie : le mode de l'écran change,
 * la fenêtre visible bouge, et présenter à l'ancien pas donnerait une image
 * décalée en silence. C'est pour cela que la fenêtre est bornée sur
 * `stride × height` et non sur les 32 Mio de VRAM (Q3).
 */
static void qgpu_bind_scanout(QgpuPCIState *s, bool complain)
{
    QfbScanoutInfo info;
    const char *which = NULL;
    bool is_vga = false;
    uint64_t win;
    uint32_t base, size;
    bool same_device;

    if (!qgpu_probe_scanout(s, &info, &which, &is_vga)) {
        if (s->scanout.mr) {
            /* La cible a disparu (elle ne devrait pas : hotpluggable=false).
               Drainage borné : on tient le BQL (D2). */
            bool drained = true;

            if (s->thread_ok) {
                qemu_mutex_lock(&s->lock);
                drained = qgpu_drain_timed_locked(s, QGPU_SYNC_WAIT_MS);
                qemu_mutex_unlock(&s->lock);
            }
            if (drained) {
                qgpu_unbind_scanout(s);
            }
        }
        if (complain && s->scanout_mode != QGPU_SCANOUT_NONE) {
            warn_report("qgpu-pci: aucun écran à présenter (scanout=%s) :"
                        " SURF_PRESENT sera refusé et l'invité relira ses"
                        " images", s->scanout_pref);
        }
        return;
    }

    /* Fenêtre visible. Sans géométrie observable (repli VGA en mode indexé,
       par exemple), on garde le comportement de la v13 : toute la VRAM, et
       `stride` à 0 pour dire au cœur qu'il n'a pas de pas à comparer. */
    base = info.base < info.vram_size ? info.base : 0;
    win = (uint64_t)info.stride * info.height;
    if (win == 0 || win > info.vram_size - base) {
        base = 0;
        win = info.vram_size;
        info.stride = 0;
    }
    size = (uint32_t)win;
    if (size == 0) {
        return;                              /* VRAM vide : rien à présenter */
    }

    if (s->scanout.mr == info.mr && s->scanout.base == base &&
        s->scanout.size == size && s->scanout.stride == info.stride) {
        return;                              /* rien n'a bougé */
    }

    /* La cible est lue SANS VERROU par le thread de rendu : la déplacer sous
       ses pieds, c'est lui faire écrire une ligne à l'ancien pas et la
       suivante au nouveau, voire hors de la région. On vide donc la file
       d'abord — borné, parce qu'on tient le BQL (D2). À l'échéance, on GARDE
       l'ancienne fenêtre : elle est périmée, donc fausse, mais elle reste dans
       la VRAM, alors qu'une fenêtre à moitié changée peut en sortir. */
    if (s->thread_ok) {
        bool drained;

        qemu_mutex_lock(&s->lock);
        drained = qgpu_drain_timed_locked(s, QGPU_SYNC_WAIT_MS);
        qemu_mutex_unlock(&s->lock);
        if (!drained) {
            warn_report_once("qgpu-pci: file encore pleine après %d ms, la"
                             " géométrie d'écran n'a pas pu être remise à"
                             " jour ; l'image présentée peut être décalée",
                             QGPU_SYNC_WAIT_MS);
            return;
        }
    }

    same_device = (s->scanout.mr == info.mr);
    qgpu_unbind_scanout(s);

    memory_region_ref(info.mr);
    /* Explicite, et pas « hérité » du propriétaire : VGA arrête son journal
       dès que sa console ne sert plus (vga_dirty_log_stop), et sans journal
       DIRTY_MEMORY_VGA nos memory_region_set_dirty ne réveillent aucun
       rafraîchissement — l'image serait écrite et jamais montrée. */
    memory_region_set_log(info.mr, true, DIRTY_MEMORY_VGA);

    s->scanout.mr     = info.mr;
    s->scanout.which  = which;
    s->scanout.base   = base;
    s->scanout.size   = size;
    s->scanout.stride = info.stride;
    s->scanout.width  = info.width;
    s->scanout.height = info.height;
    s->scanout.depth  = info.depth;
    s->scanout.ram    = info.ram + base;
    s->scanout.con    = NULL;
    s->scanout.surf_data = NULL;
    s->scanout.surf_stride = 0;
    s->scanout.surf_height = 0;
    if (is_vga) {
        Object *dev = object_resolve_path_type("", "VGA", NULL);
        DisplaySurface *surface;

        s->scanout.con = dev ? qemu_console_lookup_by_device(DEVICE(dev), 0)
                             : NULL;
        surface = s->scanout.con ? qemu_console_surface(s->scanout.con) : NULL;
        if (surface && !surface_is_allocated(surface) &&
            !surface_is_placeholder(surface)) {
            s->scanout.surf_data   = surface_data(surface);
            s->scanout.surf_stride = (uint32_t)surface_stride(surface);
            s->scanout.surf_height = (uint32_t)surface_height(surface);
        }
    }

    qgpu_core_set_scanout(&s->core, s->scanout.ram, size, qgpu_scanout_dirty,
                          &s->scanout);
#ifdef QGPU_CORE_HAS_SCANOUT_GEOM
    /* Q3 — de quoi refuser un SURF_PRESENT dont le pas n'est pas celui de
       l'écran (le symptôme de Q1 : une image écrite au pas d'un autre écran,
       donc en diagonale ou figée, sans erreur). `stride` à 0 veut dire « pas
       inconnu, ne compare rien » : c'est le cas du repli VGA quand la surface
       de la console est une copie. Activé par qgpu-core.h le jour où le cœur
       expose cette fonction ; sans elle, le bornage sur la fenêtre visible
       ci-dessus reste acquis. */
    qgpu_core_set_scanout_geom(&s->core, s->scanout.stride, s->scanout.width,
                               s->scanout.height, s->scanout.depth);
#endif
    s->core.caps |= QGPU_CAP_SCANOUT;
    if (s->core_ok) {
        s->regs[QGPU_REG_CAPS >> 2] = s->core.caps |
            (s->thread_ok ? QGPU_CAP_ASYNC : 0);
    }

    /* Q1 : dire QUEL écran, pas seulement qu'il y en a un. C'est la seule
       façon de voir depuis les journaux que la présentation part au bon
       endroit — le symptôme, lui, est un écran figé sans message. */
    if (!same_device) {
        warn_report("qgpu-pci: SURF_PRESENT présentera sur %s (scanout=%s),"
                    " fenêtre %ux%u, pas %u octets, base 0x%x", which,
                    s->scanout_pref, info.width, info.height, info.stride,
                    base);
    }
    if (s->trace) {
        /* « scanout sur » : cette formule est SONDÉE par scripts/caps.sh
           (qemu_qgpu_has_scanout) et par build_qemu_qfb.sh pour savoir si le
           binaire tient QGPU_CAP_SCANOUT. Ne pas la reformuler sans mettre le
           sondage à jour dans le même commit. */
        fprintf(stderr, "qgpu-pci: scanout sur %s, fenêtre %ux%u, pas %u,"
                " base 0x%x, %u Kio\n", which, info.width, info.height,
                info.stride, base, size / 1024u);
    }
}

/*
 * Le mode VGA a-t-il changé ? VGA ne prévient personne (qfb-pci, lui, appelle
 * qgpu_qfb_geometry_changed). Comparer la surface de la console coûte trois
 * déréférencements et se fait à chaque doorbell ; refaire la résolution QOM en
 * coûterait mille fois plus.
 */
static void qgpu_scanout_recheck(QgpuPCIState *s)
{
    DisplaySurface *surface;
    uint8_t *data = NULL;
    uint32_t stride = 0, height = 0;

    if (!s->scanout.con) {
        return;
    }
    surface = qemu_console_surface(s->scanout.con);
    if (surface && !surface_is_allocated(surface) &&
        !surface_is_placeholder(surface)) {
        data   = surface_data(surface);
        stride = (uint32_t)surface_stride(surface);
        height = (uint32_t)surface_height(surface);
    }
    if (data != s->scanout.surf_data || stride != s->scanout.surf_stride ||
        height != s->scanout.surf_height) {
        qgpu_bind_scanout(s, false);
        /* Retenir l'observation MÊME si le lien n'a pas bougé : une géométrie
           que l'on ne sait pas exploiter (surface convertie) ferait sinon
           re-sonder l'arbre QOM à chaque doorbell. */
        if (s->scanout.con) {
            s->scanout.surf_data   = data;
            s->scanout.surf_stride = stride;
            s->scanout.surf_height = height;
        }
    }
}

/* qfb-pci vient de changer de mode, de profondeur ou de base (Q3). */
static void qgpu_qfb_geometry_changed(void *opaque)
{
    qgpu_bind_scanout(opaque, false);
}

static void qgpu_machine_done(Notifier *n, void *unused)
{
    QgpuPCIState *s = container_of(n, QgpuPCIState, machine_done);

    qgpu_bind_scanout(s, true);
    /* Après le premier lien : qfb-pci existe maintenant s'il doit exister. */
    qfb_scanout_set_notifier(qgpu_qfb_geometry_changed, s);
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

    if (!s->scanout_pref || !strcmp(s->scanout_pref, "auto")) {
        s->scanout_mode = QGPU_SCANOUT_AUTO;
    } else if (!strcmp(s->scanout_pref, "qfb")) {
        s->scanout_mode = QGPU_SCANOUT_QFB;
    } else if (!strcmp(s->scanout_pref, "vga")) {
        s->scanout_mode = QGPU_SCANOUT_VGA;
    } else if (!strcmp(s->scanout_pref, "none")) {
        s->scanout_mode = QGPU_SCANOUT_NONE;
    } else {
        error_setg(errp, "qgpu-pci: scanout doit valoir auto, qfb, vga ou none"
                   " (et non « %s »)", s->scanout_pref);
        return;
    }

    /* &error_abort tuait QEMU sans un mot utilisable quand la fenêtre ne
       tenait pas en mémoire (shmem_mb=512 sur une petite machine) : ici c'est
       une erreur de configuration, elle se rend à qui a écrit la ligne de
       commande. */
    if (!memory_region_init_ram(&s->mem_shmem, OBJECT(dev), "qgpu.shmem",
                                (uint64_t)s->shmem_mb * MiB, errp)) {
        return;
    }
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
    /* Gardé : ce BH lève une interruption depuis un bottom half, donc en
       dehors de tout accès MMIO. La variante gardée est celle que QEMU 9.2
       attend d'un device (elle porte la garde de réentrance du device au lieu
       de la laisser à zéro), et elle ne coûte rien ici. */
    s->irq_bh = qemu_bh_new_guarded(qgpu_irq_bh, s,
                                    &DEVICE(dev)->mem_reentrancy_guard);
    s->thread_ok = true;
    qemu_thread_create(&s->render_thread, "qgpu-render", qgpu_render_thread, s,
                       QEMU_THREAD_JOINABLE);
    s->exit_notifier.notify = qgpu_exit_notify;
    qemu_add_exit_notifier(&s->exit_notifier);
    s->machine_done.notify = qgpu_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);

    qgpu_soft_reset(s);
    qgpu_bind_scanout(s, false);

    /* D3 — en DERNIER : aucune sortie en erreur ne peut plus laisser le
       blocage derrière elle (pci_qdev_realize n'appelle pas ->exit quand
       ->realize échoue).
     *
     * Pourquoi bloquer : post_load repart d'un soft_reset. QGPU_REG_IRQ_MASK
     * y retombe à 0 et le kext ne le réarme jamais (il le pose une fois, à
     * l'attachement) : après un loadvm, l'invité retombe sur la scrutation à
     * 10 ms. Surtout, contextes GL, surfaces et textures sont des objets de
     * l'HÔTE : ils ne sont pas dans le flux de migration, et les identifiants
     * que l'invité garde ne désignent plus rien. La 3D était donc morte en
     * silence après une reprise d'instantané. Tant que ce n'est pas résolu,
     * refuser vaut mieux que faire semblant. */
    error_setg(&s->mig_blocker,
               "qgpu-pci: la 3D ne survit pas à une migration ni à un loadvm"
               " (objets hôte non migrables, IRQ_MASK perdu). Retirer le"
               " -device qgpu-pci pour sauver ou migrer cette VM.");
    if (migrate_add_blocker(&s->mig_blocker, errp) < 0) {
        /* migrate_add_blocker a déjà libéré s->mig_blocker et l'a mis à NULL. */
        return;
    }
}

static void qgpu_pci_exit(PCIDevice *dev)
{
    QgpuPCIState *s = QGPU_PCI(dev);

    migrate_del_blocker(&s->mig_blocker);
    qfb_scanout_set_notifier(NULL, NULL);
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
    /* Le thread est arrêté : plus personne ne lit la cible de présentation. */
    qgpu_unbind_scanout(s);
    /* Normalement déjà libéré par le thread (q_fini). Reste le cas d'un
       thread jamais créé, ou déjà arrêté à la sortie de QEMU sans libération. */
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
       d'un device vide, FENCE conservée pour ne pas dérouter un client.
       Inatteignable depuis D3 (migrate_add_blocker au realize) : gardé pour
       le jour où la migration deviendra possible, et parce qu'un VMState
       incohérent serait pire qu'un VMState inutile. */
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
    /* auto | qfb | vga | none — cf. en-tête. Chaîne et non enum qapi : le
       device n'est pas amont, et un enum imposerait un type qapi dans un
       fichier que build_qemu_qfb.sh recopie tel quel. */
    DEFINE_PROP_STRING("scanout", QgpuPCIState, scanout_pref),
    DEFINE_PROP_BOOL("trace", QgpuPCIState, trace, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void qgpu_pci_instance_init(Object *obj)
{
    QgpuPCIState *s = QGPU_PCI(obj);

    s->backend = g_strdup("auto");
    s->scanout_pref = g_strdup("auto");
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
    /* Q9 — pas de retrait à chaud : le thread de rendu possède un contexte GL
       hôte, tient une référence sur la VRAM d'un autre device, et l'invité a
       BAR0 projetée dans plusieurs processus (le kext ne saurait pas la leur
       retirer). Le retrait est séquençable, mais il n'est pas séquencé : tant
       qu'il ne l'est pas, mieux vaut le refuser que le faire à moitié. */
    dc->hotpluggable = false;
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
