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
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "qgpu_proto.h"

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
    static IOReturn slotGated(OSObject * owner, void * a0, void * a1,
                              void * a2, void * a3);
    void     destroyClientObjects(int slot);
    static bool     irqFilter(OSObject * owner, IOFilterInterruptEventSource * src);
    static void     irqAction(OSObject * owner, IOInterruptEventSource * src, int count);

    IOPCIDevice *     fPCI;
    IODeviceMemory *  fShmemRange;
    IODeviceMemory *  fRegsRange;
    IOMemoryMap *     fRegsMap;
    volatile UInt32 * fRegs;

    UInt32   fShmemSize;
    UInt32   fSlotSize;
    UInt32   fVersion;
    UInt32   fCaps;
    POMPPCGPUUserClient * fClients[QGPU_MAX_CLIENTS];

    IOWorkLoop *                   fWorkLoop;
    IOCommandGate *                fGate;
    IOFilterInterruptEventSource * fIRQSource;
    UInt32                         fIRQCount;
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
