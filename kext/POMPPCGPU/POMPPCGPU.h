/*
 * POMPPCGPU.h — pilote Tiger du GPU paravirtuel « qgpu » (device QEMU qgpu-pci).
 *
 * Cible : Mac OS X 10.4 (Tiger) PowerPC, machine QEMU mac99, toolchain gcc 4.0.
 *
 * Rôle du kext : le TRANSPORT, rien d'autre. Il mappe les registres (BAR1),
 * publie à chaque processus client SA tranche de la fenêtre partagée (BAR0)
 * via un IOUserClient, frappe le doorbell et lit fence/statut. Il ne connaît
 * les opcodes que pour détruire les objets d'un client qui s'en va : le flux
 * est produit en userland (guest/qgpu-test, le plugin OpenGL
 * guest/gldriver) et exécuté côté hôte. Partage entre clients : voir
 * qgpu_proto.h, « Interface du kext ».
 *
 * Note ABI Darwin 8 : les méthodes du IOUserClient passent par la table
 * IOExternalMethod / getTargetAndMethodForIndex (l'externalMethod() moderne
 * n'existe qu'à partir de 10.5) ; les scalaires sont des mots de 32 bits.
 */

#ifndef POMPPCGPU_H
#define POMPPCGPU_H

#include <IOKit/IOCommandGate.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/graphics/IOGraphicsInterfaceTypes.h>   /* kIOAccelTypesKey… */
#include <IOKit/pci/IOPCIDevice.h>

#include "qgpu_proto.h"

/* ── L'accélérateur publié (tâche 4.2) ───────────────────────────────────────
 *
 *   C'est ainsi qu'un vrai pilote de carte se fait connaître d'OpenGL.framework
 *   (docs/re/accelerateur-iokit.md) :
 *
 *   1. CGL, pour chaque écran, lit sur le FRAMEBUFFER les propriétés
 *      IOAccelTypes (chemin, dans le plan IOService, d'un objet de classe
 *      IOAccelerator) et IOAccelIndex (IOAccelFindAccelerator, IOKit.framework) ;
 *   2. GLEngine lit sur cet accélérateur IOGLBundleName et charge
 *      /System/Library/Extensions/<nom>.bundle/Contents/MacOS/<nom> AVANT les
 *      GLDriver* du dossier Resources d'OpenGL.framework, avec pour masque
 *      d'écrans ceux dont le framebuffer le désigne.
 *
 *   L'accélérateur est un NUB ENFANT de POMPPCGPU, et non POMPPCGPU lui-même :
 *   le plugin et qgpu_test ouvrent POMPPCGPU avec le type 0, qui est aussi
 *   kIOAccelSurfaceClientType — le type qu'ouvrent CGL (pbuffers) et le
 *   WindowServer (Quartz Extreme) sur un accélérateur. Le transport garde donc
 *   son ABI, et le nub refuse toute ouverture tant qu'il n'y a pas de surfaces
 *   (tâche 4.4).
 *
 *   Il ne publie PAS AccelCaps : le WindowServer tente Quartz Extreme dès que
 *   ce masque est non nul (CGXGLDisplayContextInitialize, CoreGraphics). */
#define POMPPC_GL_BUNDLE_NAME   "GLDriver-POMPPC"

class POMPPCAccelerator : public IOAccelerator
{
    OSDeclareDefaultStructors(POMPPCAccelerator)

public:
    virtual IOReturn newUserClient(task_t owningTask, void * securityID,
                                   UInt32 type, IOUserClient ** handler);
};

/* ── v9 : drapeaux passés DANS `len` à QGPU_UC_SUBMIT ────────────────────────
 *
 *   COPIE IDENTIQUE dans guest/gldriver/pomppc_qgpu.h (userland). Les deux
 *   doivent rester au bit près, comme les deux copies de qgpu_proto.h.
 *
 *   POURQUOI DANS `len` ET PAS UN SCALAIRE DE PLUS. Le protocole fige
 *   QGPU_UC_METHOD_COUNT et les sélecteurs QGPU_UC_* : on ne peut ni ajouter
 *   une méthode, ni (c'est le point à retenir) ajouter un argument à
 *   QGPU_UC_SUBMIT. L'ABI de Darwin 8 compare le NOMBRE d'arguments au bit
 *   près — is_io_connect_method_scalarI_scalarO refuse l'appel dès que
 *   inputCount ≠ IOExternalMethod::count0 — donc passer count0 de 2 à 3
 *   ferait rendre kIOReturnBadArgument à TOUS les appelants existants
 *   (qgpu-test compilé avant, un plugin plus ancien encore installé…).
 *   `len` est un multiple de 4 borné par la tranche (16 Mio sur une fenêtre de
 *   64) : ses trois bits hauts sont libres, et un appelant qui ne les connaît
 *   pas les laisse à zéro — c'est-à-dire exactement le comportement v8.
 *
 *   ASYNC : doorbell asynchrone. Rend alors, au lieu de (fence, statut, pc) :
 *     out0 = QGPU_REG_FENCE_SUBMITTED  barrière de CETTE soumission ;
 *     out1 = QGPU_REG_SUBMIT_ST        acceptation (OK ou QGPU_ST_QUEUE_FULL) ;
 *     out2 = QGPU_REG_ERRORS           compteur d'erreurs — status_pc n'a pas
 *                                      de sens à la soumission, et c'est
 *                                      ERRORS qui fait foi quand plusieurs
 *                                      soumissions sont en vol.
 *   PEEK  : ne soumet RIEN ; rend (ERRORS, STATUS, STATUS_PC) — de quoi
 *           nommer la dernière soumission terminée en erreur.
 *   QUEUE : ne soumet RIEN ; rend (DOORBELL, QUEUE_FREE, QUEUE_DEPTH) — pour
 *           le bilan du plugin (profondeur de file moyenne).
 */
#define POMPPC_SUB_ASYNC        0x80000000UL
#define POMPPC_SUB_PEEK         0x40000000UL
#define POMPPC_SUB_QUEUE        0x20000000UL
/* 24/09/2026 : LAYOUT — le kext répond, SANS rien soumettre, avec les constantes
   de tranches qu'il a compilées : fence = QGPU_CLIENT_TEX_IDS | BUF_IDS << 16,
   status = QGPU_CLIENT_SURF_IDS | CTX_IDS << 16, pc = QGPU_MAX_TEX. Un kext qui
   ignore ce bit le laisse dans `len`, qui déborde alors la tranche : appel
   refusé — c'est le signal « kext d'un autre en-tête ». Le 23/09, un kext à
   128 textures par client sous un plugin à 1024 a coûté deux plantages et des
   créneaux de clients perdus. */
#define POMPPC_SUB_LAYOUT       0x10000000UL
#define POMPPC_SUB_FLAGS        (POMPPC_SUB_ASYNC | POMPPC_SUB_PEEK | POMPPC_SUB_QUEUE | \
                                 POMPPC_SUB_LAYOUT)

class POMPPCGPUUserClient;

class POMPPCGPU : public IOService
{
    OSDeclareDefaultStructors(POMPPCGPU)

public:
    virtual bool     start(IOService * provider);
    virtual void     stop(IOService * provider);
    virtual void     free(void);
    virtual IOReturn newUserClient(task_t owningTask, void * securityID,
                                   UInt32 type, IOUserClient ** handler);

    /* services rendus au user client */
    UInt32   slotSize(void)   { return fSlotSize; }
    UInt32   version(void)    { return fVersion; }
    /* K6 : ne JAMAIS annoncer QGPU_CAP_ASYNC si le kext n'a pas de quoi le
       tenir (timer du chien de garde absent). Le client passerait en
       asynchrone contre un kext synchrone : il lirait un statut de rendu là
       où il attend une acceptation, et sur st != OK il resoumettrait le même
       flux — trame exécutée deux fois — pendant que check_errors reboucle. */
    UInt32   caps(void)       { return fAsync ? fCaps : (fCaps & ~(UInt32) QGPU_CAP_ASYNC); }
    UInt32   fence(void)      { return fRegs ? regRead(QGPU_REG_FENCE) : 0; }
    int      allocSlot(POMPPCGPUUserClient * client);        /* -1 si complet */
    /* K3/K7 : le propriétaire est exigé — une tranche déjà rendue puis
       réattribuée ne doit pas être détruite par son ancien client. */
    IOReturn freeSlot(int slot, POMPPCGPUUserClient * client);   /* détruit ses objets, rend la tranche */
    IOReturn resetSlot(int slot, POMPPCGPUUserClient * client);  /* détruit ses objets, GARDE la tranche */
    /* K1 : comptabilité SILENCIEUSE, sans gate ni MMIO — pour le fil de
       terminaison, qui n'a plus le droit d'entrer dans la gate (elle a pu
       être retirée du work loop) ni de parler à un device peut-être disparu. */
    void     forgetSlot(int slot, POMPPCGPUUserClient * client);
    IODeviceMemory * slotRange(int slot);                    /* retenu par l'appelant */
    /* `len` porte les drapeaux POMPPC_SUB_* dans ses bits hauts (voir plus haut). */
    IOReturn submit(int slot, UInt32 off, UInt32 len,
                    UInt32 * fence, UInt32 * status, UInt32 * statusPC);
    IOReturn waitFence(UInt32 target, UInt32 timeoutMs, UInt32 * current);

private:
    /* registres big-endian côté QEMU, CPU big-endian : pas d'échange d'octets */
    UInt32 regRead(UInt32 offset) { return fRegs[offset >> 2]; }
    void   regWrite(UInt32 offset, UInt32 value)
    {
        fRegs[offset >> 2] = value;
        OSSynchronizeIO();
    }

    static IOReturn submitGated(OSObject * owner, void * a0, void * a1,
                                void * a2, void * a3);
    static IOReturn waitGated(OSObject * owner, void * a0, void * a1,
                              void * a2, void * a3);
    static IOReturn slotGated(OSObject * owner, void * a0, void * a1,
                              void * a2, void * a3);
    /* Contexte gated : dort jusqu'à ce que FENCE atteigne `target`. */
    IOReturn sleepForFence(UInt32 target, UInt32 timeoutMs, UInt32 * current);
    void     destroyClientObjects(int slot);
    void     drainQueue(void);
    static bool     irqFilter(OSObject * owner, IOFilterInterruptEventSource * src);
    static void     irqAction(OSObject * owner, IOInterruptEventSource * src, int count);
    static void     timerAction(OSObject * owner, IOTimerEventSource * src);
    /* 4.2 : accélérateur publié et désigné par les framebuffers */
    void            publishAccelerator(void);
    void            unpublishAccelerator(void);
    static bool     framebufferPublished(void * target, void * ref, IOService * fb);
    static bool     framebufferTerminated(void * target, void * ref, IOService * fb);
    void            linkFramebuffer(IOService * fb);
    void            unlinkFramebuffer(IOService * fb);

    IOPCIDevice *     fPCI;
    IODeviceMemory *  fShmemRange;
    IODeviceMemory *  fRegsRange;
    IOMemoryMap *     fRegsMap;
    volatile UInt32 * fRegs;

    UInt32   fShmemSize;
    UInt32   fSlotSize;
    /* K8 : offset, dans BAR0, de la page de service — hors de toute tranche.
       C'est là qu'est écrit le flux de destruction des objets d'un client :
       sa tranche à lui peut être encore mappée et vivante (clientDied pendant
       un dessin, QGPU_UC_RESET). Elle n'est touchée que dans la gate. */
    UInt32   fServiceOff;
    UInt32   fQueueDepth;         /* profondeur de file annoncée par le device */
    UInt32   fVersion;
    UInt32   fCaps;
    UInt32   fAsync;              /* le device tient QGPU_CAP_ASYNC (v9) */
    /* K4 : posé par stop() avant de retirer les sources d'événements. Tout
       dormeur le relit à chaque réveil et rend la main (kIOReturnNotReady) ;
       sans lui, free() libère la gate sous un dormeur THREAD_UNINT. */
    UInt32   fStopping;
    POMPPCGPUUserClient * fClients[QGPU_MAX_CLIENTS];
    /* K5 : destruction des objets d'une tranche EN COURS (elle dort dans la
       gate entre deux paquets). Interdit une seconde destruction concurrente
       (clientClose ⊥ clientDied) et toute réattribution de la tranche. */
    UInt32   fSlotBusy[QGPU_MAX_CLIENTS];

    IOWorkLoop *                   fWorkLoop;
    IOCommandGate *                fGate;
    IOFilterInterruptEventSource * fIRQSource;
    UInt32                         fIRQCount;
    /* v9 : attente de barrière sur la command gate. Tiger n'a PAS
       commandSleep(event, deadline, …) — il date de 10.5 — et dormir sans
       échéance dans un kext, c'est figer la VM le jour où une interruption se
       perd. Ce timer bat donc tant qu'il y a des dormeurs et leur sert de base
       de temps (fTicks) pour le délai maximal. */
    IOTimerEventSource *           fTimer;
    UInt32                         fTicks;
    UInt32                         fSleepers;

    POMPPCAccelerator *            fAccel;
    OSString *                     fAccelPath;    /* valeur d'IOAccelTypes */
    IONotifier *                   fFBNotifier;
    IONotifier *                   fFBTermNotifier; /* framebuffers qui s'en vont */
    IOLock *                       fFBLock;       /* protège fLinkedFBs */
    OSArray *                      fLinkedFBs;    /* framebuffers désignant fAccel */
    UInt32                         fNextFBIndex;  /* repli d'IOAccelIndex (Q16) */
};

class POMPPCGPUUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(POMPPCGPUUserClient)

public:
    virtual bool     initWithTask(task_t owningTask, void * securityID, UInt32 type);
    virtual bool     start(IOService * provider);
    virtual void     stop(IOService * provider);
    virtual IOReturn clientClose(void);
    virtual IOReturn clientDied(void);
    virtual IOExternalMethod * getTargetAndMethodForIndex(IOService ** target,
                                                          UInt32 index);
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options,
                                         IOMemoryDescriptor ** memory);

    /* méthodes exposées (sélecteurs QGPU_UC_* de qgpu_proto.h) */
    IOReturn ucGetInfo(UInt32 * version, UInt32 * caps, UInt32 * shmem,
                       UInt32 * fence, void *, void *);
    IOReturn ucSubmit(UInt32 off, UInt32 len, UInt32 * fence,
                      UInt32 * status, UInt32 * statusPC, void *);
    IOReturn ucWaitFence(UInt32 target, UInt32 timeoutMs, UInt32 * current,
                         void *, void *, void *);
    IOReturn ucReset(void *, void *, void *, void *, void *, void *);
    IOReturn ucGetSlot(UInt32 * index, UInt32 * base, UInt32 * ctxBase,
                       UInt32 * surfBase, void *, void *);

private:
    POMPPCGPU * fOwner;
    task_t      fTask;
    int         fSlot;
};

#endif /* POMPPCGPU_H */
