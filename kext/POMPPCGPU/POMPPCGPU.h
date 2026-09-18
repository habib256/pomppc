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
#include <IOKit/pci/IOPCIDevice.h>

#include "qgpu_proto.h"

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
#define POMPPC_SUB_FLAGS        (POMPPC_SUB_ASYNC | POMPPC_SUB_PEEK | POMPPC_SUB_QUEUE)

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
    UInt32   caps(void)       { return fCaps; }
    UInt32   fence(void)      { return regRead(QGPU_REG_FENCE); }
    int      allocSlot(POMPPCGPUUserClient * client);        /* -1 si complet */
    void     freeSlot(int slot);                             /* détruit ses objets */
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
    void     destroyClientObjects(int slot);
    void     drainQueue(void);
    static bool     irqFilter(OSObject * owner, IOFilterInterruptEventSource * src);
    static void     irqAction(OSObject * owner, IOInterruptEventSource * src, int count);
    static void     timerAction(OSObject * owner, IOTimerEventSource * src);

    IOPCIDevice *     fPCI;
    IODeviceMemory *  fShmemRange;
    IODeviceMemory *  fRegsRange;
    IOMemoryMap *     fRegsMap;
    volatile UInt32 * fRegs;

    UInt32   fShmemSize;
    UInt32   fSlotSize;
    UInt32   fVersion;
    UInt32   fCaps;
    UInt32   fAsync;              /* le device tient QGPU_CAP_ASYNC (v9) */
    POMPPCGPUUserClient * fClients[QGPU_MAX_CLIENTS];

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
