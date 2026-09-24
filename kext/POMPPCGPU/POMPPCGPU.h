/*
 * POMPPCGPU.h — pilote Tiger du GPU paravirtuel « qgpu » (device QEMU qgpu-pci).
 *
 * Cible : Mac OS X 10.4 (Tiger) PowerPC, machine QEMU mac99, toolchain gcc 4.0.
 *
 * Rôle du kext : le TRANSPORT, rien d'autre. Il mappe les registres (BAR1),
 * publie à chaque processus client SA tranche de la fenêtre partagée (BAR0)
 * via un IOUserClient, frappe le doorbell et lit fence/statut. Il ne connaît
 * AUCUN opcode (v19, chantier A1) : le flux est produit en userland
 * (guest/qgpu-test, le plugin OpenGL guest/gldriver) et exécuté côté hôte, et
 * les objets d'un client qui s'en va sont détruits par le device lui-même
 * (QGPU_REG_CLIENT_RESET). Le nombre de tranches et la disposition des
 * identifiants sont LUS DANS LES REGISTRES au démarrage, jamais compilés :
 * ce kext n'inclut que qgpu_abi.h (l'ABI de transport), pas qgpu_proto.h.
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

#include "qgpu_abi.h"

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

/* Les drapeaux POMPPC_SUB_* passés dans `len` de QGPU_UC_SUBMIT sont définis
   dans qgpu_abi.h (une seule copie, partagée avec le userland). */

/* Capacité du kext en tranches : le nombre RÉEL est lu dans QGPU_REG_CLIENTS
   au démarrage et borné par ceci. Ce n'est pas une constante du protocole,
   c'est la taille des tableaux ci-dessous. */
#define POMPPC_KEXT_MAX_CLIENTS 8

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
    UInt32   clients(void)    { return fClientCount; }
    /* v19 : lecture d'un registre de BAR1 pour le userland (QGPU_UC_READ_REG).
       Sans effet de bord par contrat (qgpu_abi.h) ; bornée sur le BAR. */
    IOReturn readReg(UInt32 offset, UInt32 * value);
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
    IOReturn waitDestroy(UInt32 target, int async);
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
    UInt32   fClientCount;        /* v19 : QGPU_REG_CLIENTS, ≤ POMPPC_KEXT_MAX_CLIENTS */
    UInt32   fQueueDepth;         /* profondeur de file annoncée par le device */
    UInt32   fVersion;
    UInt32   fCaps;
    UInt32   fAsync;              /* le device tient QGPU_CAP_ASYNC (v9) */
    /* K4 : posé par stop() avant de retirer les sources d'événements. Tout
       dormeur le relit à chaque réveil et rend la main (kIOReturnNotReady) ;
       sans lui, free() libère la gate sous un dormeur THREAD_UNINT. */
    UInt32   fStopping;
    POMPPCGPUUserClient * fClients[POMPPC_KEXT_MAX_CLIENTS];
    /* K5 : destruction des objets d'une tranche EN COURS (elle dort dans la
       gate entre deux paquets). Interdit une seconde destruction concurrente
       (clientClose ⊥ clientDied) et toute réattribution de la tranche. */
    UInt32   fSlotBusy[POMPPC_KEXT_MAX_CLIENTS];

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
    IOReturn ucReadReg(UInt32 offset, UInt32 * value, void *, void *, void *, void *);

private:
    POMPPCGPU * fOwner;
    task_t      fTask;
    int         fSlot;
};

#endif /* POMPPCGPU_H */
