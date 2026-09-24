/*
 * POMPPCGPU.cpp — pilote Tiger du GPU paravirtuel « qgpu » : voir POMPPCGPU.h.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSByteOrder.h>

#include "POMPPCGPU.h"

#define GPULog(fmt, args...) IOLog("POMPPCGPU: " fmt, ## args)

/* v9 : période du chien de garde des attentes de barrière, et donc résolution
   du délai maximal de QGPU_UC_WAIT_FENCE. 10 ms : assez fin pour un délai qui
   se compte en secondes, assez gros pour ne rien coûter quand personne
   n'attend (le timer n'est armé QUE tant qu'il y a des dormeurs). */
#define WAIT_TICK_MS    10

/* K10 : plafond du REPLI par scrutation (IOSleep, non interruptible). Ne
   s'applique qu'au mode dégradé sans chien de garde, où rien ne peut
   légitimement attendre longtemps — l'attente normale dort sur la gate et
   garde le délai demandé par le client. */
#define QGPU_POLL_MAX_MS        2000

/* K5 : délai maximal d'une barrière de destruction. Au-delà, l'hôte ne répond
   plus ; on le dit et on rend la tranche quand même. */
#define DESTROY_WAIT_MS         2000
/* v19 : nombre de fois où l'on réessaie QGPU_REG_CLIENT_RESET sur file pleine
   (chaque essai attend d'abord que tout ce qui est accepté soit terminé). */
#define DESTROY_RETRIES         8

#define super IOService
OSDefineMetaClassAndStructors(POMPPCGPU, IOService)

/* ────────────────────────────── cycle de vie ────────────────────────────── */

bool POMPPCGPU::start(IOService * provider)
{
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) {
        return false;
    }
    if (!super::start(provider)) {
        return false;
    }

    fShmemRange = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    fRegsRange  = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1);
    if (!fShmemRange || !fRegsRange) {
        GPULog("BAR0 (shared window) or BAR1 (registers) missing - Open "
               "Firmware did not assign PCI resources?\n");
        return false;
    }
    fShmemRange->retain();
    fRegsRange->retain();

    fRegsMap = fRegsRange->map();
    if (!fRegsMap) {
        GPULog("cannot map registers\n");
        return false;
    }
    fRegs = (volatile UInt32 *) fRegsMap->getVirtualAddress();

    fPCI->setMemoryEnable(true);

    if (regRead(QGPU_REG_MAGIC) != QGPU_MAGIC) {
        GPULog("invalid signature (0x%08lx, expected 'qgp1')\n",
               (unsigned long) regRead(QGPU_REG_MAGIC));
        fPCI->setMemoryEnable(false);
        return false;
    }
    fVersion   = regRead(QGPU_REG_VERSION);
    fCaps      = regRead(QGPU_REG_CAPS);
    fShmemSize = regRead(QGPU_REG_SHMEM_SIZE);
    /* v19 : ce kext ne sait plus détruire les objets d'un client lui-même ; il
       lui faut un device qui publie ses tranches et tient CLIENT_RESET. La
       version du protocole (QGPU_REG_VERSION) ne le regarde plus : elle est
       publiée dans ioreg et rendue par GET_INFO, c'est le plugin qui en juge. */
    if (!(fCaps & QGPU_CAP_CLIENTS)) {
        GPULog("protocol v%lu without QGPU_CAP_CLIENTS: this kext (v19 transport) "
               "needs a device that owns the client layout - refusing\n",
               (unsigned long) fVersion);
        fPCI->setMemoryEnable(false);
        return false;
    }
    fClientCount = regRead(QGPU_REG_CLIENTS);
    if (fClientCount == 0 || fClientCount > POMPPC_KEXT_MAX_CLIENTS) {
        GPULog("device offers %lu client slots, this kext holds 1..%d: refusing\n",
               (unsigned long) fClientCount, POMPPC_KEXT_MAX_CLIENTS);
        fPCI->setMemoryEnable(false);
        return false;
    }
    if (fShmemSize > fShmemRange->getLength()) {
        /* le device et le BAR doivent dire la même chose */
        fShmemSize = fShmemRange->getLength();
    }
    /* Les tranches sont contiguës depuis 0 et alignées sur une page. Le device
       ne connaît pas les tranches de BAR0 (il ne borne que sur la fenêtre) :
       ce découpage-là est celui du kext seul ; le découpage des IDENTIFIANTS,
       lui, est celui du device (QGPU_REG_LAYOUT), et le kext n'en sait rien.
       Plus de page de service (K8) : le kext n'écrit plus rien dans BAR0. */
    fSlotSize   = (fShmemSize / fClientCount) & ~(UInt32) 0xFFF;
    if (fSlotSize == 0) {
        GPULog("shared window too small (%lu bytes)\n", (unsigned long) fShmemSize);
        fPCI->setMemoryEnable(false);
        return false;
    }

    /* Work loop privé : sérialise doorbell/lecture des fences entre clients
       et avec l'interruption. */
    fWorkLoop = IOWorkLoop::workLoop();
    fGate = fWorkLoop ? IOCommandGate::commandGate(this) : 0;
    if (!fWorkLoop || !fGate ||
        fWorkLoop->addEventSource(fGate) != kIOReturnSuccess) {
        GPULog("work loop / command gate failed\n");
        return false;
    }

    fIRQSource = IOFilterInterruptEventSource::filterInterruptEventSource(
                     this,
                     (IOInterruptEventSource::Action) &POMPPCGPU::irqAction,
                     (IOFilterInterruptEventSource::Filter) &POMPPCGPU::irqFilter,
                     provider, 0);
    if (fIRQSource && fWorkLoop->addEventSource(fIRQSource) != kIOReturnSuccess) {
        fIRQSource->release();
        fIRQSource = 0;
    }
    if (fIRQSource) {
        fIRQSource->enable();
        regWrite(QGPU_REG_IRQ, QGPU_IRQ_DONE);        /* acquitte un reliquat */
        regWrite(QGPU_REG_IRQ_MASK, QGPU_IRQ_DONE);
    } else {
        GPULog("no interrupt source: polling\n");
    }

    /* Chien de garde des attentes de barrière (v9). Sans lui, pas de sommeil
       sur la command gate : on retombe sur la scrutation d'avant la v9. */
    fTimer = IOTimerEventSource::timerEventSource(this,
                 (IOTimerEventSource::Action) &POMPPCGPU::timerAction);
    if (fTimer && fWorkLoop->addEventSource(fTimer) != kIOReturnSuccess) {
        fTimer->release();
        fTimer = 0;
    }

    /* v9 : le doorbell asynchrone n'est posé que si le device l'annonce. Sans
       le bit, QGPU_DOORBELL_ASYNC serait traité comme un doorbell synchrone
       par le device — correct, mais le kext rendrait alors une « barrière »
       déjà atteinte et un statut de rendu là où le client attend une
       acceptation. On préfère le dire une fois pour toutes ici. */
    fAsync = ((fCaps & QGPU_CAP_ASYNC) && fTimer) ? 1 : 0;

    /* Profondeur de la file de CE device (le protocole dit de lire le registre
       plutôt que de croire QGPU_QUEUE_DEPTH) : publiée dans ioreg. */
    fQueueDepth = regRead(QGPU_REG_QUEUE_DEPTH);
    if (fQueueDepth == 0) {
        fQueueDepth = QGPU_QUEUE_DEPTH;
    }

    /* Propriétés visibles dans ioreg, pour le diagnostic et pour que le
       userland sache ce qu'il a en face avant même d'ouvrir le service. */
    setProperty("QGPUVersion",   (unsigned long long) fVersion, 32);
    setProperty("QGPUCaps",      (unsigned long long) fCaps, 32);
    /* K6 : ce que le DEVICE annonce (ci-dessus) et ce que le kext TIENT
       (ci-dessous, ce que QGPU_UC_GET_INFO rend) peuvent différer — c'est tout
       l'objet du masquage d'ASYNC. Les deux dans ioreg, pour que la sonde
       n'ait pas à deviner. */
    setProperty("QGPUCapsClient", (unsigned long long) caps(), 32);
    setProperty("QGPUShmemSize", (unsigned long long) fShmemSize, 32);
    setProperty("QGPUQueueDepth", (unsigned long long) fQueueDepth, 32);
    setProperty("QGPUSlotSize",  (unsigned long long) fSlotSize, 32);
    setProperty("QGPUClients",   (unsigned long long) fClientCount, 32);   /* v19 */
    setProperty("QGPUAsync",     (unsigned long long) fAsync, 32);
    {
        UInt32 tag = regRead(QGPU_REG_BACKEND_NAME);
        char   name[5];
        name[0] = tag >> 24; name[1] = tag >> 16; name[2] = tag >> 8;
        name[3] = tag; name[4] = 0;
        /* IOLog goes to the Darwin console (MacRoman): ASCII English only.
           Backend name is a 4-byte tag ('gl' plus padding): trim it. */
        {
            int i = 3;
            while (i >= 0 && (unsigned char)name[i] <= ' ')
                name[i--] = 0;
        }
        setProperty("QGPUBackend", name);
        GPULog("started: protocol v%lu, host backend '%s', caps 0x%lx, "
               "window %lu MiB, %lu clients x %lu KiB, %s\n",
               (unsigned long) fVersion, name,
               (unsigned long) fCaps, (unsigned long) (fShmemSize >> 20),
               (unsigned long) fClientCount, (unsigned long) (fSlotSize >> 10),
               fAsync ? "async submit ready" : "sync submit only");
    }

    registerService();
    publishAccelerator();
    return true;
}

void POMPPCGPU::stop(IOService * provider)
{
    unpublishAccelerator();

    /* K4 — RÉVEILLER LES DORMEURS AVANT DE TOUT RETIRER. waitGated dort en
       THREAD_UNINT ; ses deux seules sources de réveil sont l'IRQ DONE et le
       chien de garde, que les lignes suivantes suppriment. Sans ce drapeau :
       gel inkillable, puis free() qui fait IORecursiveLockFree sur la gate
       SOUS un dormeur (commandSleep a relâché le verrou, removeEventSource
       progresse donc sans attendre personne) — panic.
       On attend ensuite, borné, que les dormeurs soient sortis de la gate :
       c'est la seule façon de retirer les sources sans leur tirer le tapis. */
    fStopping = 1;
    if (fGate) {
        UInt32 spins = 0;
        fGate->commandWakeup(&fIRQCount, false);   /* false = tous les dormeurs */
        while (fSleepers && spins < 1000) {        /* ≤ 1 s */
            IOSleep(1);
            spins++;
            fGate->commandWakeup(&fIRQCount, false);
        }
        if (fSleepers) {
            GPULog("stop: %lu waiter(s) still asleep\n", (unsigned long) fSleepers);
        }
    }

    if (fRegs) {
        regWrite(QGPU_REG_IRQ_MASK, 0);
    }
    if (fIRQSource) {
        fIRQSource->disable();
        if (fWorkLoop) {
            fWorkLoop->removeEventSource(fIRQSource);
        }
        fIRQSource->release();
        fIRQSource = 0;
    }
    if (fTimer) {
        fTimer->cancelTimeout();
        if (fWorkLoop) {
            fWorkLoop->removeEventSource(fTimer);
        }
        fTimer->release();
        fTimer = 0;
    }
    if (fGate && fWorkLoop) {
        fWorkLoop->removeEventSource(fGate);
    }
    if (fPCI) {
        fPCI->setMemoryEnable(false);
    }
    super::stop(provider);
}

void POMPPCGPU::free(void)
{
    if (fAccelPath)  { fAccelPath->release();  fAccelPath = 0; }
    if (fFBLock)     { IOLockFree(fFBLock);    fFBLock = 0; }
    if (fGate)       { fGate->release();       fGate = 0; }
    if (fWorkLoop)   { fWorkLoop->release();   fWorkLoop = 0; }
    if (fRegsMap)    { fRegsMap->release();    fRegsMap = 0; }
    if (fRegsRange)  { fRegsRange->release();  fRegsRange = 0; }
    if (fShmemRange) { fShmemRange->release(); fShmemRange = 0; }
    super::free();
}

/* ──────────────────────── accélérateur publié (4.2) ────────────────────────
 *
 * Voir POMPPCGPU.h et docs/re/accelerateur-iokit.md. Le nub porte
 * IOGLBundleName ; chaque framebuffer publié reçoit IOAccelTypes (le chemin du
 * nub) et IOAccelIndex (son rang). Un échec ici laisse le transport intact,
 * mais GLEngine ne trouvera plus le plugin, qui ne vit plus dans Resources :
 * chaque échec est donc dit dans le journal du noyau. */

void POMPPCGPU::publishAccelerator(void)
{
    char path[512];
    int  len = sizeof(path);

    fAccel = new POMPPCAccelerator;
    if (!fAccel || !fAccel->init()) {
        if (fAccel) { fAccel->release(); fAccel = 0; }
        GPULog("accelerator: allocation failed\n");
        return;
    }
    fAccel->setProperty("IOGLBundleName", POMPPC_GL_BUNDLE_NAME);
    if (!fAccel->attach(this)) {
        fAccel->release();
        fAccel = 0;
        GPULog("accelerator: attach failed\n");
        return;
    }
    if (!fAccel->getPath(path, &len, gIOServicePlane) ||
        !(fAccelPath = OSString::withCString(path))) {
        GPULog("accelerator: path not found, framebuffers not bound\n");
        fAccel->registerService();
        return;
    }
    fAccel->registerService();

    fFBLock    = IOLockAlloc();
    fLinkedFBs = OSArray::withCapacity(2);
    if (!fFBLock || !fLinkedFBs) {
        GPULog("accelerator: framebuffers not bound (memory)\n");
        return;
    }
    /* Appelé aussi, tout de suite, pour les framebuffers déjà publiés : le kext
       peut être chargé à chaud (devloop) comme au démarrage. addNotification
       consomme le dictionnaire. */
    fFBNotifier = addNotification(gIOPublishNotification,
                                  serviceMatching("IOFramebuffer"),
                                  &POMPPCGPU::framebufferPublished, this, 0);
    /* … et leur disparition : sans cela fLinkedFBs RETIENT un framebuffer
       terminé (OSArray::setObject retient) et unpublishAccelerator écrirait
       dans un objet mort ; un écran débranché garderait sa place et son
       index. Chaque addNotification consomme SON dictionnaire : deux appels
       à serviceMatching. */
    fFBTermNotifier = addNotification(gIOTerminatedNotification,
                                      serviceMatching("IOFramebuffer"),
                                      &POMPPCGPU::framebufferTerminated, this, 0);
    GPULog("accelerator published: %s (%s)\n", path, POMPPC_GL_BUNDLE_NAME);
}

bool POMPPCGPU::framebufferPublished(void * target, void * ref, IOService * fb)
{
    ((POMPPCGPU *) target)->linkFramebuffer(fb);
    return true;
}

bool POMPPCGPU::framebufferTerminated(void * target, void * ref, IOService * fb)
{
    ((POMPPCGPU *) target)->unlinkFramebuffer(fb);
    return true;
}

void POMPPCGPU::linkFramebuffer(IOService * fb)
{
    OSString * cur;
    OSNumber * index;
    OSNumber * glIndex;
    IOService * fbProvider;
    UInt32     n;

    if (!fFBLock || !fLinkedFBs || !fAccelPath) {
        return;                     /* publication incomplète : rien à lier */
    }
    IOLockLock(fFBLock);
    if (fLinkedFBs->getNextIndexOfObject(fb, 0) != (unsigned int) -1) {
        IOLockUnlock(fFBLock);
        return;
    }
    /* Un framebuffer qui désigne déjà un AUTRE accélérateur a un vrai pilote
       de carte derrière lui : on ne le lui prend pas. */
    cur = OSDynamicCast(OSString, fb->getProperty(kIOAccelTypesKey));
    if (cur && !cur->isEqualTo(fAccelPath)) {
        IOLockUnlock(fFBLock);
        GPULog("%s already names %s: left unchanged\n", fb->getName(),
               cur->getCStringNoCopy());
        return;
    }
    /* Q16 — L'INDEX N'EST PLUS L'ORDRE DE PUBLICATION. IOAccelIndex est le
       RANG DU FRAMEBUFFER pour cet accélérateur (IOAccelFindAccelerator) et
       CGL le garde par écran : le prendre dans l'ordre d'arrivée des
       notifications, c'est le faire dépendre de l'ordre de chargement des
       pilotes — avec deux écrans (VGA + qfb), VGA et QFB pouvaient être
       croisés d'un démarrage à l'autre, et un retrait renumérotait les
       autres. Ordre de préférence :
         1. l'index qu'IOGraphics a déjà attribué au framebuffer pour OpenGL,
            s'il est publié (c'est celui que CGL emploie pour ses masques) ;
         2. 0 pour l'écran de DÉMARRAGE (nœud Open Firmware « AAPL,boot-display »),
            c'est-à-dire celui que CGMainDisplayID rend et sur lequel le plugin
            lit base/pitch ;
         3. sinon un rang monotone à partir de 1, JAMAIS recyclé. */
    glIndex = OSDynamicCast(OSNumber, fb->getProperty("IOFramebufferOpenGLIndex"));
    fbProvider = fb->getProvider();
    if (glIndex) {
        n = (UInt32) glIndex->unsigned32BitValue();
    } else if (fb->getProperty("AAPL,boot-display") ||
               (fbProvider && fbProvider->getProperty("AAPL,boot-display"))) {
        n = 0;
    } else {
        n = ++fNextFBIndex;
    }
    index = OSNumber::withNumber((unsigned long long) n, 32);
    fb->setProperty(kIOAccelTypesKey, fAccelPath);
    if (index) {
        fb->setProperty(kIOAccelIndexKey, index);
        index->release();
    }
    fLinkedFBs->setObject(fb);
    IOLockUnlock(fFBLock);
    GPULog("framebuffer %s bound to accelerator (index %lu)\n", fb->getName(),
           (unsigned long) n);
}

/* Un framebuffer qui s'en va : lui rendre ses propriétés et le lâcher. Appelé
   sur la terminaison de TOUT IOFramebuffer — d'où le test d'appartenance. */
void POMPPCGPU::unlinkFramebuffer(IOService * fb)
{
    unsigned int i;

    if (!fFBLock || !fLinkedFBs) {
        return;
    }
    IOLockLock(fFBLock);
    if (!fLinkedFBs) {                  /* unpublishAccelerator a pu passer */
        IOLockUnlock(fFBLock);
        return;
    }
    i = fLinkedFBs->getNextIndexOfObject(fb, 0);
    if (i != (unsigned int) -1) {
        OSString * cur = OSDynamicCast(OSString, fb->getProperty(kIOAccelTypesKey));
        if (cur && fAccelPath && cur->isEqualTo(fAccelPath)) {
            fb->removeProperty(kIOAccelTypesKey);
            fb->removeProperty(kIOAccelIndexKey);
        }
        fLinkedFBs->removeObject(i);
        GPULog("framebuffer %s unbound (terminated)\n", fb->getName());
    }
    IOLockUnlock(fFBLock);
}

void POMPPCGPU::unpublishAccelerator(void)
{
    unsigned int i;

    /* remove() attend la fin d'un appel de framebufferPublished en cours : le
       faire SANS tenir fFBLock, que ce rappel prend (sinon interblocage). */
    if (fFBNotifier) {
        fFBNotifier->remove();
        fFBNotifier = 0;
    }
    if (fFBTermNotifier) {
        fFBTermNotifier->remove();
        fFBTermNotifier = 0;
    }
    if (fFBLock) {
        IOLockLock(fFBLock);
    }
    if (fLinkedFBs) {
        for (i = 0; i < fLinkedFBs->getCount(); i++) {
            IOService * fb = (IOService *) fLinkedFBs->getObject(i);
            OSString *  cur = OSDynamicCast(OSString, fb->getProperty(kIOAccelTypesKey));
            if (cur && fAccelPath && cur->isEqualTo(fAccelPath)) {
                fb->removeProperty(kIOAccelTypesKey);
                fb->removeProperty(kIOAccelIndexKey);
            }
        }
        fLinkedFBs->release();
        fLinkedFBs = 0;
    }
    if (fFBLock) {
        IOLockUnlock(fFBLock);
    }
    /* Le nub est un client de POMPPCGPU : quand c'est notre terminaison qui
       nous arrête, il est déjà terminé et terminate() ne fait rien. */
    if (fAccel) {
        fAccel->terminate();
        fAccel->release();
        fAccel = 0;
    }
}

/* ─────────────────────────────── interruption ───────────────────────────── */

bool POMPPCGPU::irqFilter(OSObject * owner, IOFilterInterruptEventSource * src)
{
    POMPPCGPU * self = (POMPPCGPU *) owner;
    UInt32 pending = self->regRead(QGPU_REG_IRQ) & QGPU_IRQ_DONE;

    if (!pending) {
        return false;                        /* ligne INTx partagée : pas à nous */
    }
    self->regWrite(QGPU_REG_IRQ, pending);   /* acquittement */
    return true;
}

void POMPPCGPU::irqAction(OSObject * owner, IOInterruptEventSource * src, int count)
{
    POMPPCGPU * self = (POMPPCGPU *) owner;

    self->fIRQCount += count;
    /* Réveille les dormeurs de waitFence (contexte gated). L'IRQ DONE est de
       NIVEAU et se COALESCE : une seule peut couvrir plusieurs soumissions
       terminées. Le dormeur ne compte donc pas les interruptions, il RELIT
       FENCE à chaque réveil. */
    if (self->fGate) {
        self->fGate->commandWakeup(&self->fIRQCount, false);
    }
}

/* Chien de garde des attentes : bat tant qu'il y a des dormeurs. Il fournit la
   base de temps du délai maximal ET le réveil de secours si une interruption
   DONE se perd — un kext qui dort sans réveil garanti fige la VM. */
void POMPPCGPU::timerAction(OSObject * owner, IOTimerEventSource * src)
{
    POMPPCGPU * self = (POMPPCGPU *) owner;

    self->fTicks++;
    if (self->fSleepers) {
        self->fGate->commandWakeup(&self->fIRQCount, false);
        src->setTimeoutMS(WAIT_TICK_MS);
    }
}

/* ─────────────────────────────── soumission ───────────────────────────────
 *
 * v9. Deux modes, choisis PAR SOUMISSION par le drapeau POMPPC_SUB_ASYNC que
 * le client pose dans les bits hauts de `len` (voir POMPPCGPU.h : l'ABI de
 * Darwin 8 interdit d'ajouter un scalaire sans casser tous les appelants) :
 *
 *   sans drapeau  doorbell 1 : l'écriture MMIO ne rend la main qu'une fois la
 *                 soumission TERMINÉE. Inchangé au bit près depuis la v1.
 *   ASYNC         doorbell 3 : la soumission est mise en file et l'écriture
 *                 rend la main tout de suite. On rend la BARRIÈRE de cette
 *                 soumission et l'ACCEPTATION, pas le résultat du rendu.
 *
 * Sur QGPU_ST_QUEUE_FULL rien n'est mis en file : le kext RÉPOND, il ne boucle
 * pas. La command gate sérialise tous les clients ; y attendre une place
 * bloquerait les trois autres et le gestionnaire d'interruption avec eux.
 * C'est au client de décider (attendre une barrière et réessayer, ou se
 * replier sur le doorbell synchrone, qui lui n'est jamais refusé).
 */

struct SubmitArgs {
    UInt32 off, len, flags;
    UInt32 fence, status, statusPC;
    IOReturn result;
};

IOReturn POMPPCGPU::submitGated(OSObject * owner, void * a0, void *, void *, void *)
{
    POMPPCGPU * self = (POMPPCGPU *) owner;
    SubmitArgs * a = (SubmitArgs *) a0;

    a->result = kIOReturnSuccess;
    if (a->flags & POMPPC_SUB_PEEK) {
        /* Rien à soumettre : de quoi nommer la dernière terminée en erreur.
           FENCE d'abord serait inutile ici — c'est ERRORS qui fait foi. */
        a->fence    = self->regRead(QGPU_REG_ERRORS);
        a->status   = self->regRead(QGPU_REG_STATUS);
        a->statusPC = self->regRead(QGPU_REG_STATUS_PC);
        return kIOReturnSuccess;
    }
    if (a->flags & POMPPC_SUB_QUEUE) {
        a->fence    = self->regRead(QGPU_REG_DOORBELL);      /* en vol */
        a->status   = self->regRead(QGPU_REG_QUEUE_FREE);
        a->statusPC = self->regRead(QGPU_REG_QUEUE_DEPTH);
        return kIOReturnSuccess;
    }

    self->regWrite(QGPU_REG_SUBMIT_OFF, a->off);
    self->regWrite(QGPU_REG_SUBMIT_LEN, a->len);
    if ((a->flags & POMPPC_SUB_ASYNC) && self->fAsync) {
        self->regWrite(QGPU_REG_DOORBELL, QGPU_DOORBELL_GO | QGPU_DOORBELL_ASYNC);
        /* Le statut d'ACCEPTATION d'abord : sur QGPU_ST_QUEUE_FULL,
           FENCE_SUBMITTED n'a pas bougé et la valeur rendue désigne une
           soumission PRÉCÉDENTE — le client regarde le statut avant elle. */
        a->status   = self->regRead(QGPU_REG_SUBMIT_ST);
        a->fence    = self->regRead(QGPU_REG_FENCE_SUBMITTED);
        a->statusPC = self->regRead(QGPU_REG_ERRORS);
    } else {
        self->regWrite(QGPU_REG_DOORBELL, QGPU_DOORBELL_GO);
        /* Exécution terminée au retour de l'écriture. FENCE d'abord : l'hôte
           la publie EN DERNIER, après le statut et après les relectures. */
        a->fence    = self->regRead(QGPU_REG_FENCE);
        a->status   = self->regRead(QGPU_REG_STATUS);
        a->statusPC = self->regRead(QGPU_REG_STATUS_PC);
    }
    return kIOReturnSuccess;
}

IOReturn POMPPCGPU::submit(int slot, UInt32 off, UInt32 len,
                           UInt32 * fence, UInt32 * status, UInt32 * statusPC)
{
    SubmitArgs a;
    IOReturn   kr;

    /* K2 : TOUT initialiser avant runAction. Si l'action ne s'exécute pas, on
       rendait au userland trois mots de pile noyau — dont un « fence »
       aléatoire, que le client attendait ensuite jusqu'au délai maximal.
       waitFence et allocSlot le faisaient déjà, submit non. */
    a.off = 0; a.len = 0;
    a.fence = 0; a.status = QGPU_ST_BACKEND; a.statusPC = 0;
    a.result = kIOReturnNotReady;
    a.flags = len & POMPPC_SUB_FLAGS;
    len    &= ~POMPPC_SUB_FLAGS;
    if (slot < 0 || (UInt32) slot >= fClientCount) {
        return kIOReturnBadArgument;
    }
    /* Un bit de `len` que ce kext ne connaît pas (tel le LAYOUT du 24/09/2026,
       retiré en v19) reste dans `len` et déborde la tranche : refusé plus bas. */
    /* K1/K4 : plus rien ne part une fois stop() commencé — la gate a pu être
       retirée du work loop, et runAction ferme la gate AVANT tout test. */
    if (!fGate || fStopping) {
        return kIOReturnNotReady;
    }
    if (a.flags & (POMPPC_SUB_PEEK | POMPPC_SUB_QUEUE)) {
        a.off = 0; a.len = 0;           /* on ne soumet rien : pas de bornes à vérifier */
    } else {
        /* Le device revérifie, mais un flux hors de la tranche du client ne doit
           même pas partir : c'est un bug du client, pas un état du matériel. */
        if ((off & 3) || (len & 3) || len == 0 || off > fSlotSize ||
            len > fSlotSize - off) {
            return kIOReturnBadArgument;
        }
        a.off = off + (UInt32) slot * fSlotSize; a.len = len;
    }
    /* K9 : le retour de runAction est un verdict, pas un détail — l'action a
       pu ne jamais s'exécuter (gate fermée, work loop retiré). */
    kr = fGate->runAction(&POMPPCGPU::submitGated, &a);
    *fence = a.fence; *status = a.status; *statusPC = a.statusPC;
    if (kr != kIOReturnSuccess) {
        return kr;
    }
    return a.result;
}

/* ───────────────────────── attente de barrière (v9) ─────────────────────────
 *
 * Avant la v9, la fence était déjà atteinte au retour du doorbell et la
 * scrutation à IOSleep(1) ne servait à rien ; avec la file elle coûterait
 * jusqu'à une milliseconde par attente — c'est-à-dire tout le gain. On dort
 * donc sur la command gate, réveillé par irqAction (IRQ DONE).
 *
 * Deux pièges, tous deux vus dans la conception :
 *   — l'IRQ DONE se COALESCE : on relit FENCE à chaque réveil, on ne compte
 *     jamais les interruptions ;
 *   — Tiger n'a pas commandSleep(event, DEADLINE, …), arrivé en 10.5. Le délai
 *     maximal vient donc du chien de garde (fTicks), qui garantit aussi un
 *     réveil si une interruption se perd : un kext qui dort sans réveil
 *     garanti fige la VM, et `-x` n'est pas disponible pour la dépanner.
 *
 * commandSleep relâche le verrou du work loop : pendant qu'un client attend,
 * les autres soumettent et l'interruption est servie.
 */

struct WaitArgs {
    UInt32   target, timeoutMs, current;
    IOReturn result;
};

/* Contexte GATED, et seulement gated : dort jusqu'à ce que FENCE atteigne
   `target`. Sert à QGPU_UC_WAIT_FENCE et aux barrières des destructions (K5).
   Rend kIOReturnNotReady si le kext s'arrête, kIOReturnTimeout au délai. */
IOReturn POMPPCGPU::sleepForFence(UInt32 target, UInt32 timeoutMs, UInt32 * current)
{
    UInt32   start = fTicks;
    IOReturn result = kIOReturnTimeout;

    /* K4 : en TÊTE aussi. stop() a pu retirer le chien de garde et l'IRQ :
       s'endormir ici, c'est s'endormir pour toujours. */
    if (fStopping || !fGate || !fTimer || !fRegs) {
        if (current && fRegs) {
            *current = regRead(QGPU_REG_FENCE);
        }
        return kIOReturnNotReady;
    }
    fSleepers++;
    if (fSleepers == 1) {
        fTimer->setTimeoutMS(WAIT_TICK_MS);
    }
    for (;;) {
        UInt32 f = regRead(QGPU_REG_FENCE);
        if (current) {
            *current = f;
        }
        /* Compteurs de 32 bits qui bouclent : comparer par différence signée. */
        if ((SInt32) (f - target) >= 0) {
            result = kIOReturnSuccess;
            break;
        }
        /* K4 : … et à chaque tour. stop() pose le drapeau puis réveille tout
           le monde ; c'est ici qu'on rend la main avant qu'il ne retire la
           gate sous nos pieds. */
        if (fStopping) {
            result = kIOReturnNotReady;
            break;
        }
        if ((fTicks - start) * WAIT_TICK_MS >= timeoutMs) {
            result = kIOReturnTimeout;
            break;
        }
        /* THREAD_UNINT : un signal ne doit pas rendre la main au client avec
           une moitié de tampon encore en vol. Le chien de garde borne.
           Le retour est celui de IORecursiveLockSleep : THREAD_AWAKENED vaut
           0, comme kIOReturnSuccess. Tout le reste (réveil impossible, gate
           déjà retirée) doit sortir de la boucle au lieu d'y tourner. */
        if (fGate->commandSleep(&fIRQCount, THREAD_UNINT) != kIOReturnSuccess) {
            result = kIOReturnNotReady;
            break;
        }
    }
    if (fSleepers && --fSleepers == 0 && fTimer) {
        fTimer->cancelTimeout();
    }
    return result;
}

IOReturn POMPPCGPU::waitGated(OSObject * owner, void * a0, void *, void *, void *)
{
    POMPPCGPU * self = (POMPPCGPU *) owner;
    WaitArgs * a = (WaitArgs *) a0;

    a->result = self->sleepForFence(a->target, a->timeoutMs, &a->current);
    return kIOReturnSuccess;
}

IOReturn POMPPCGPU::waitFence(UInt32 target, UInt32 timeoutMs, UInt32 * current)
{
    UInt32 f;
    UInt32 waited = 0;

    *current = 0;
    if (!fRegs) {
        return kIOReturnNotReady;
    }
    f = regRead(QGPU_REG_FENCE);
    /* Cas le plus fréquent, et le seul qui existait avant la v9 : rien à
       attendre. Aucune entrée dans la gate, aucun réveil. */
    *current = f;
    if ((SInt32) (f - target) >= 0) {
        return kIOReturnSuccess;
    }
    if (fStopping) {
        return kIOReturnNotReady;
    }
    /* K10 : le chien de garde SUFFIT — il réveille toutes les WAIT_TICK_MS,
       interruption ou pas. Exiger aussi fIRQSource envoyait tout un device
       sans INTx dans le repli à IOSleep(1) ci-dessous, c'est-à-dire des
       milliers de sommeils d'une milliseconde par attente. */
    if (fGate && fTimer) {
        WaitArgs a;
        IOReturn kr;
        a.target = target; a.timeoutMs = timeoutMs;
        a.current = f; a.result = kIOReturnTimeout;
        kr = fGate->runAction(&POMPPCGPU::waitGated, &a);   /* K9 : vérifié */
        *current = a.current;
        return (kr == kIOReturnSuccess) ? a.result : kr;
    }
    /* Repli : ni chien de garde ni gate — on scrute comme avant la v9. Hors
       gate : la lecture d'un registre est atomique et ne gêne personne.
       K10 : PLAFONNÉ. IOSleep n'est pas interruptible : 60 000 tours, c'était
       une minute de processus inkillable. Sans timer, caps() n'annonce pas
       QGPU_CAP_ASYNC (K6) : personne n'a de soumission en vol, et une attente
       qui dépasse ce plafond dit que l'hôte ne répond plus. */
    if (timeoutMs > QGPU_POLL_MAX_MS) {
        timeoutMs = QGPU_POLL_MAX_MS;
    }
    for (;;) {
        f = regRead(QGPU_REG_FENCE);
        *current = f;
        if ((SInt32) (f - target) >= 0) {
            return kIOReturnSuccess;
        }
        if (waited >= timeoutMs || fStopping) {
            return (waited >= timeoutMs) ? kIOReturnTimeout : kIOReturnNotReady;
        }
        IOSleep(1);
        waited++;
    }
}

/* ─────────────────────────── tranches des clients ─────────────────────────── */

/* K3/K7 — LA TRANCHE APPARTIENT À UN CLIENT, ET LUI SEUL PEUT LA RENDRE.
 *
 * `clientClose` et `clientDied` peuvent courir (processus tué pendant un
 * dessin, deux vCPU) : l'un pouvait rendre la tranche pendant que l'autre la
 * rendait encore, et entre les deux `allocSlot` avait pu la redonner à un
 * autre processus — dont les objets étaient alors détruits par l'ancien
 * propriétaire. D'où `client` dans TOUTES les opérations et l'invariant
 * `fClients[slot] == client` vérifié DANS la gate. Le même invariant règle K7
 * (QGPU_UC_RESET détruisait les objets puis reprenait la PREMIÈRE tranche
 * libre, pas la sienne : flux écrit dans l'ancienne, offsets de la nouvelle).
 */

#define SLOT_OP_ALLOC   0
#define SLOT_OP_FREE    1         /* détruit les objets ET rend la tranche */
#define SLOT_OP_RESET   2         /* détruit les objets, garde la tranche */

struct SlotArgs {
    int op;
    POMPPCGPUUserClient * client;
    int slot;
    IOReturn result;
};

IOReturn POMPPCGPU::slotGated(OSObject * owner, void * a0, void *, void *, void *)
{
    POMPPCGPU * self = (POMPPCGPU *) owner;
    SlotArgs * a = (SlotArgs *) a0;
    int i;

    a->result = kIOReturnSuccess;
    if (a->op == SLOT_OP_ALLOC) {
        a->slot = -1;
        for (i = 0; i < (int) self->fClientCount; i++) {
            /* fSlotBusy : une tranche dont les objets sont encore en cours de
               destruction n'est pas libre, même si son client est parti. */
            if (!self->fClients[i] && !self->fSlotBusy[i]) {
                self->fClients[i] = a->client;
                a->slot = i;
                break;
            }
        }
        if (a->slot < 0) {
            a->result = kIOReturnNoResources;
        }
        return kIOReturnSuccess;
    }

    if (a->slot < 0 || (UInt32) a->slot >= self->fClientCount) {
        a->result = kIOReturnBadArgument;
        return kIOReturnSuccess;
    }
    if (self->fClients[a->slot] != a->client) {
        /* Tranche déjà rendue, et peut-être déjà redonnée : ne RIEN détruire. */
        a->result = kIOReturnNotPermitted;
        return kIOReturnSuccess;
    }
    if (self->fSlotBusy[a->slot]) {
        /* L'autre fil (clientClose ⊥ clientDied) s'en occupe déjà. */
        a->result = kIOReturnBusy;
        return kIOReturnSuccess;
    }
    self->fSlotBusy[a->slot] = 1;
    self->destroyClientObjects(a->slot);        /* peut dormir : gate relâchée */
    self->fSlotBusy[a->slot] = 0;
    if (a->op == SLOT_OP_FREE && self->fClients[a->slot] == a->client) {
        self->fClients[a->slot] = 0;
    }
    return kIOReturnSuccess;
}

int POMPPCGPU::allocSlot(POMPPCGPUUserClient * client)
{
    SlotArgs a;
    IOReturn kr;

    a.op = SLOT_OP_ALLOC; a.client = client; a.slot = -1;
    a.result = kIOReturnNotReady;
    if (!fGate || fStopping) {
        return -1;
    }
    kr = fGate->runAction(&POMPPCGPU::slotGated, &a);   /* K9 */
    if (kr != kIOReturnSuccess) {
        GPULog("allocSlot: gate refused (0x%x)\n", (unsigned int) kr);
        return -1;
    }
    return a.slot;
}

IOReturn POMPPCGPU::freeSlot(int slot, POMPPCGPUUserClient * client)
{
    SlotArgs a;
    IOReturn kr;

    a.op = SLOT_OP_FREE; a.client = client; a.slot = slot;
    a.result = kIOReturnNotReady;
    if (slot < 0 || (UInt32) slot >= fClientCount) {
        return kIOReturnBadArgument;
    }
    /* K9 — une tranche non rendue est perdue POUR TOUJOURS (« 5e client
       refusé »). Si la gate ne veut plus de nous, on rend au moins la tranche
       dans la comptabilité, sans toucher au device. */
    if (!fGate || fStopping) {
        forgetSlot(slot, client);
        return kIOReturnNotReady;
    }
    kr = fGate->runAction(&POMPPCGPU::slotGated, &a);
    if (kr != kIOReturnSuccess) {
        GPULog("freeSlot(%d): gate refused (0x%x)\n", slot, (unsigned int) kr);
        forgetSlot(slot, client);
        return kr;
    }
    return a.result;
}

IOReturn POMPPCGPU::resetSlot(int slot, POMPPCGPUUserClient * client)
{
    SlotArgs a;
    IOReturn kr;

    a.op = SLOT_OP_RESET; a.client = client; a.slot = slot;
    a.result = kIOReturnNotReady;
    if (slot < 0 || (UInt32) slot >= fClientCount) {
        return kIOReturnBadArgument;
    }
    if (!fGate || fStopping) {
        return kIOReturnNotReady;
    }
    kr = fGate->runAction(&POMPPCGPU::slotGated, &a);
    return (kr == kIOReturnSuccess) ? a.result : kr;
}

/* K1 — COMPTABILITÉ SILENCIEUSE : ni gate, ni MMIO, ni attente. Appelée par le
   fil de terminaison (kextunload, terminate), pour qui entrer dans la gate est
   une panic (removeEventSource a fait setWorkLoop(0), et runAction ferme la
   gate AVANT tout test) et parler au device une écriture dans le vide. Une
   écriture de pointeur alignée est atomique sur PowerPC ; le pire effet d'une
   course avec allocSlot est qu'un nouveau client prenne la tranche un
   instant plus tôt, ce qui est précisément ce qu'on veut. */
void POMPPCGPU::forgetSlot(int slot, POMPPCGPUUserClient * client)
{
    if (slot < 0 || (UInt32) slot >= fClientCount) {
        return;
    }
    if (fClients[slot] == client) {
        fClients[slot] = 0;
    }
}

IODeviceMemory * POMPPCGPU::slotRange(int slot)
{
    if (slot < 0 || (UInt32) slot >= fClientCount) {
        return 0;
    }
    return IODeviceMemory::withSubRange(fShmemRange, (IOPhysicalAddress) slot * fSlotSize,
                                        fSlotSize);
}

/* Contexte gated. Détruit les objets de la tranche `slot` — PAR LE DEVICE.
 *
 * v19 (chantier A1). Jusqu'ici le kext écrivait lui-même un flux de
 * destruction dans une page de service de BAR0 : un doorbell par identifiant,
 * pour les classes d'objets et les tailles de plages qu'il avait COMPILÉES
 * (1 108 doorbells par client ; jamais les requêtes). Un kext d'un autre
 * en-tête laissait des objets vivants derrière lui, et le plugin suivant
 * recréait des identifiants encore pris (23/09/2026 : BAD_ARG en cascade,
 * créneaux perdus). Le device connaît ses plages mieux que personne : on lui
 * écrit l'index de la tranche dans QGPU_REG_CLIENT_RESET, il met la
 * destruction EN FILE derrière tout ce qui est en vol (FIFO — donc aussi
 * derrière les dernières soumissions du client mort, qui lisaient encore sa
 * tranche), et on dort sur la barrière comme pour n'importe quelle soumission.
 * Le kext n'écrit plus un mot dans BAR0 et ne connaît aucun opcode.
 *
 * File pleine : rien n'a été mis en file ; on attend que tout ce qui est
 * accepté soit terminé, puis on réécrit (borné par DESTROY_RETRIES).
 *
 * La barrière : au retour, la tranche du client n'est plus lue par personne
 * et peut être redonnée. Sans chien de garde (fAsync = 0), on scrute FENCE
 * comme avant la v9, borné par DESTROY_WAIT_MS. */
IOReturn POMPPCGPU::waitDestroy(UInt32 target, int async)
{
    UInt32 spins = 0;

    if (async) {
        return sleepForFence(target, DESTROY_WAIT_MS, 0);
    }
    while ((SInt32) (regRead(QGPU_REG_FENCE) - target) < 0) {
        if (fStopping) {
            return kIOReturnNotReady;
        }
        if (++spins > (UInt32) DESTROY_WAIT_MS * 5) {   /* 200 µs par tour */
            return kIOReturnTimeout;
        }
        IODelay(200);
    }
    return kIOReturnSuccess;
}

void POMPPCGPU::destroyClientObjects(int slot)
{
    UInt32 want = 0, tries = 0;
    int    async;

    if (fStopping || !fRegs) {
        return;                     /* plus personne à qui parler */
    }
    async = (fAsync && fTimer && fGate) ? 1 : 0;

    for (;;) {
        UInt32 st;

        regWrite(QGPU_REG_CLIENT_RESET, (UInt32) slot);
        st = regRead(QGPU_REG_SUBMIT_ST);
        if (st == QGPU_ST_OK) {
            want = regRead(QGPU_REG_FENCE_SUBMITTED);
            break;
        }
        if (st != QGPU_ST_QUEUE_FULL || ++tries > DESTROY_RETRIES) {
            GPULog("slot %d: device refused CLIENT_RESET (status %lu, try %lu): "
                   "objects left behind\n", slot, (unsigned long) st,
                   (unsigned long) tries);
            return;
        }
        if (waitDestroy(regRead(QGPU_REG_FENCE_SUBMITTED), async) != kIOReturnSuccess) {
            GPULog("slot %d: host did not drain in %d ms before CLIENT_RESET\n",
                   slot, DESTROY_WAIT_MS);
            return;
        }
    }
    if (waitDestroy(want, async) != kIOReturnSuccess) {
        GPULog("slot %d: host did not finish CLIENT_RESET in %d ms\n",
               slot, DESTROY_WAIT_MS);
    }
}

/* v19 : lecture d'un registre pour le userland. Bornée sur le BAR, pas sur
   QGPU_CTRL_TOPADDR : un device qui publie un registre de plus n'a pas besoin
   d'un kext de plus (au-delà de ce qu'il tient, il rend 0xFFFFFFFF). Sans
   effet de bord, par contrat de qgpu_abi.h. */
IOReturn POMPPCGPU::readReg(UInt32 offset, UInt32 * value)
{
    *value = 0;
    if (!fRegs || !fRegsRange || fStopping) {
        return kIOReturnNotReady;
    }
    if ((offset & 3) || offset + 4 > fRegsRange->getLength()) {
        return kIOReturnBadArgument;
    }
    *value = regRead(offset);
    return kIOReturnSuccess;
}

/* ─────────────────────────────── user client ────────────────────────────── */

IOReturn POMPPCGPU::newUserClient(task_t owningTask, void * securityID,
                                  UInt32 type, IOUserClient ** handler)
{
    POMPPCGPUUserClient * client;

    /* K4 : plus de nouveau client une fois la terminaison commencée — il
       prendrait une tranche que personne ne rendra et entrerait dans une gate
       en train d'être retirée. */
    if (fStopping || !fGate) {
        return kIOReturnNotReady;
    }
    client = new POMPPCGPUUserClient;
    if (!client) {
        return kIOReturnNoMemory;
    }
    if (!client->initWithTask(owningTask, securityID, type)) {
        client->release();
        return kIOReturnBadArgument;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnExclusiveAccess;
    }
    *handler = client;
    return kIOReturnSuccess;
}

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(POMPPCGPUUserClient, IOUserClient)

bool POMPPCGPUUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type)
{
    if (!super::initWithTask(owningTask, securityID, type)) {
        return false;
    }
    fTask = owningTask;
    fOwner = 0;
    fSlot = -1;
    return true;
}

bool POMPPCGPUUserClient::start(IOService * provider)
{
    fOwner = OSDynamicCast(POMPPCGPU, provider);
    if (!fOwner) {
        return false;
    }
    if (!super::start(provider)) {
        fOwner = 0;
        return false;
    }
    /* Une tranche de fenêtre et une plage d'identifiants par client. */
    fSlot = fOwner->allocSlot(this);
    if (fSlot < 0) {
        GPULog("already %lu clients: open refused\n", (unsigned long) fOwner->clients());
        /* Mineur : un start() qui échoue APRÈS super::start doit défaire ce
           que super::start a fait — IOService ne rappellera pas stop() pour
           un démarrage refusé. */
        super::stop(provider);
        fOwner = 0;
        return false;
    }
    return true;
}

/* K1 — TERMINAISON PAR LE HAUT (kextunload, terminate) AVEC UN CLIENT OUVERT.
 *
 * On ne rendait ni fOwner ni la tranche : le prochain SUBMIT passait le test
 * `if (!fOwner)` et entrait dans une gate déjà retirée du work loop. Or
 * removeEventSource fait setWorkLoop(0) — donc workLoop = NULL — et
 * IOCommandGate::runAction fait closeGate() AVANT tout test : panic sur un
 * déréférencement nul, déterministe, à chaque boucle de dev.
 *
 * Ce que ce stop() fait est donc VOLONTAIREMENT SILENCIEUX : effacer fOwner
 * (tout appel suivant rend kIOReturnNotAttached) et rendre la tranche dans la
 * seule comptabilité. PAS de freeSlot() : il entrerait dans la gate depuis le
 * fil de terminaison et parlerait à un device peut-être déjà parti. Les objets
 * de l'hôte partent avec le device ou avec le processus ; ce qui compte ici,
 * c'est que plus personne ne touche au matériel. */
void POMPPCGPUUserClient::stop(IOService * provider)
{
    POMPPCGPU * owner = fOwner;
    int         slot  = fSlot;

    fSlot  = -1;
    fOwner = 0;
    if (owner && slot >= 0) {
        owner->forgetSlot(slot, this);
    }
    super::stop(provider);
}

/* K3 : capturer ET EFFACER fSlot AVANT l'appel. clientClose et clientDied
   peuvent courir (processus tué pendant un dessin, deux vCPU) : sans cela, le
   second voyait encore `fSlot >= 0`, rendait une deuxième fois la même
   tranche — que allocSlot avait pu, entre-temps, redonner à un autre
   processus dont les objets étaient alors détruits. L'invariant
   `fClients[slot] == client` de slotGated ferme la course pour de bon. */
IOReturn POMPPCGPUUserClient::clientClose(void)
{
    POMPPCGPU * owner = fOwner;
    int         slot  = fSlot;

    fSlot = -1;
    if (owner && slot >= 0) {
        owner->freeSlot(slot, this);
    }
    terminate();
    fOwner = 0;
    return kIOReturnSuccess;
}

IOReturn POMPPCGPUUserClient::clientDied(void)
{
    return clientClose();
}

IOReturn POMPPCGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options,
                                                  IOMemoryDescriptor ** memory)
{
    IODeviceMemory * range;
    POMPPCGPU *      owner = fOwner;
    int              slot  = fSlot;

    if (type != QGPU_UC_MEM_SHMEM || !owner || slot < 0) {
        return kIOReturnBadArgument;
    }
    /* withSubRange rend un objet déjà retenu : IOUserClient::mapClientMemory
       le relâchera après le mapping. */
    range = owner->slotRange(slot);
    if (!range) {
        return kIOReturnNoMemory;
    }
    *memory  = range;
    *options = 0;
    return kIOReturnSuccess;
}

IOExternalMethod * POMPPCGPUUserClient::getTargetAndMethodForIndex(IOService ** target,
                                                                   UInt32 index)
{
    static const IOExternalMethod methods[QGPU_UC_METHOD_COUNT] = {
        { 0, (IOMethod) &POMPPCGPUUserClient::ucGetInfo,   kIOUCScalarIScalarO, 0, 4 },
        { 0, (IOMethod) &POMPPCGPUUserClient::ucSubmit,    kIOUCScalarIScalarO, 2, 3 },
        { 0, (IOMethod) &POMPPCGPUUserClient::ucWaitFence, kIOUCScalarIScalarO, 2, 1 },
        { 0, (IOMethod) &POMPPCGPUUserClient::ucReset,     kIOUCScalarIScalarO, 0, 0 },
        { 0, (IOMethod) &POMPPCGPUUserClient::ucGetSlot,   kIOUCScalarIScalarO, 0, 4 },
        { 0, (IOMethod) &POMPPCGPUUserClient::ucReadReg,   kIOUCScalarIScalarO, 1, 1 },  /* v19 */
    };

    if (index >= QGPU_UC_METHOD_COUNT) {
        return 0;
    }
    *target = this;
    return (IOExternalMethod *) &methods[index];
}

IOReturn POMPPCGPUUserClient::ucGetInfo(UInt32 * version, UInt32 * caps, UInt32 * shmem,
                                        UInt32 * fence, void *, void *)
{
    POMPPCGPU * owner = fOwner;

    *version = 0; *caps = 0; *shmem = 0; *fence = 0;
    if (!owner) {
        return kIOReturnNotAttached;
    }
    *version = owner->version();
    *caps    = owner->caps();
    *shmem   = owner->slotSize();
    *fence   = owner->fence();
    return kIOReturnSuccess;
}

IOReturn POMPPCGPUUserClient::ucSubmit(UInt32 off, UInt32 len, UInt32 * fence,
                                       UInt32 * status, UInt32 * statusPC, void *)
{
    POMPPCGPU * owner = fOwner;      /* K1 : une seule lecture — stop() l'efface */
    int         slot  = fSlot;

    /* Sorties toujours définies, même sur le chemin d'erreur (K2). */
    *fence = 0; *status = QGPU_ST_BACKEND; *statusPC = 0;
    if (!owner || slot < 0) {
        return kIOReturnNotAttached;
    }
    return owner->submit(slot, off, len, fence, status, statusPC);
}

IOReturn POMPPCGPUUserClient::ucWaitFence(UInt32 target, UInt32 timeoutMs, UInt32 * current,
                                          void *, void *, void *)
{
    POMPPCGPU * owner = fOwner;

    *current = 0;
    if (!owner) {
        return kIOReturnNotAttached;
    }
    if (timeoutMs > 60000) {
        timeoutMs = 60000;
    }
    return owner->waitFence(target, timeoutMs, current);
}

/* Détruit les objets du client (et seulement les siens) ; il GARDE sa tranche.
   K7 : il rendait la tranche puis reprenait la PREMIÈRE libre — pas forcément
   la sienne. Le client continuait d'écrire dans la tranche qu'il a mappée (le
   mapping, lui, ne bouge pas) avec les offsets d'une autre : flux lu ailleurs,
   et deux processus sur la même tranche. resetSlot détruit les objets sans
   jamais toucher à l'index. */
IOReturn POMPPCGPUUserClient::ucReset(void *, void *, void *, void *, void *, void *)
{
    if (!fOwner || fSlot < 0) {
        return kIOReturnNotAttached;
    }
    return fOwner->resetSlot(fSlot, this);
}

IOReturn POMPPCGPUUserClient::ucGetSlot(UInt32 * index, UInt32 * base, UInt32 * ctxBase,
                                        UInt32 * surfBase, void *, void *)
{
    POMPPCGPU * owner = fOwner;
    int         slot  = fSlot;

    *index = 0; *base = 0; *ctxBase = 0; *surfBase = 0;
    if (!owner || slot < 0) {
        return kIOReturnNotAttached;
    }
    *index    = slot;
    *base     = (UInt32) slot * owner->slotSize();
    /* v19 : recopiés de la table du device (classes 0 et 1 par convention
       d'ABI), plus compilés. Les autres plages : QGPU_UC_READ_REG. */
    {
        UInt32 n = 0;
        owner->readReg(QGPU_REG_LAYOUT_CLASS(0), &n);
        *ctxBase  = (UInt32) slot * n;
        owner->readReg(QGPU_REG_LAYOUT_CLASS(1), &n);
        *surfBase = (UInt32) slot * n;
    }
    return kIOReturnSuccess;
}

IOReturn POMPPCGPUUserClient::ucReadReg(UInt32 offset, UInt32 * value,
                                        void *, void *, void *, void *)
{
    POMPPCGPU * owner = fOwner;

    *value = 0;
    if (!owner) {
        return kIOReturnNotAttached;
    }
    return owner->readReg(offset, value);
}

/* ───────────────────── nub IOAccelerator (tâche 4.2) ───────────────────── */

#undef super
#define super IOAccelerator
OSDefineMetaClassAndStructors(POMPPCAccelerator, IOAccelerator)

/* Aucune surface tant que Quartz Extreme n'est pas fait (4.4) : CGL (pbuffers,
   CGLSetPBuffer) et le WindowServer reçoivent l'échec d'IOServiceOpen, et
   prennent le même chemin que sans accélérateur (IOAccelCreateSurface échoue
   aussi sur un accélérateur nul). Le transport, lui, s'ouvre sur POMPPCGPU. */
IOReturn POMPPCAccelerator::newUserClient(task_t owningTask, void * securityID,
                                          UInt32 type, IOUserClient ** handler)
{
    return kIOReturnUnsupported;
}
