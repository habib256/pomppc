/*
 * POMPPCGPU.cpp — pilote Tiger du GPU paravirtuel « qgpu » : voir POMPPCGPU.h.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSByteOrder.h>

#include "POMPPCGPU.h"

#define GPULog(fmt, args...) IOLog("POMPPCGPU: " fmt, ## args)

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
        GPULog("BAR0 (fenêtre partagée) ou BAR1 (registres) absent — Open "
               "Firmware n'a pas assigné les ressources PCI ?\n");
        return false;
    }
    fShmemRange->retain();
    fRegsRange->retain();

    fRegsMap = fRegsRange->map();
    if (!fRegsMap) {
        GPULog("impossible de mapper les registres\n");
        return false;
    }
    fRegs = (volatile UInt32 *) fRegsMap->getVirtualAddress();

    fPCI->setMemoryEnable(true);

    if (regRead(QGPU_REG_MAGIC) != QGPU_MAGIC) {
        GPULog("signature invalide (0x%08lx au lieu de 'qgp1')\n",
               (unsigned long) regRead(QGPU_REG_MAGIC));
        fPCI->setMemoryEnable(false);
        return false;
    }
    fVersion   = regRead(QGPU_REG_VERSION);
    fCaps      = regRead(QGPU_REG_CAPS);
    fShmemSize = regRead(QGPU_REG_SHMEM_SIZE);
    /* Un device plus récent comprend les flux des versions précédentes
       (chaque version ne fait qu'ajouter des opcodes et des clés). */
    if (fVersion < QGPU_PROTO_VERSION) {
        GPULog("protocole v%lu, ce kext exige au moins v%d : refus\n",
               (unsigned long) fVersion, QGPU_PROTO_VERSION);
        fPCI->setMemoryEnable(false);
        return false;
    }
    if (fShmemSize > fShmemRange->getLength()) {
        /* le device et le BAR doivent dire la même chose */
        fShmemSize = fShmemRange->getLength();
    }
    fSlotSize = (fShmemSize / QGPU_MAX_CLIENTS) & ~(UInt32) 0xFFF;

    /* Work loop privé : sérialise doorbell/lecture des fences entre clients
       et avec l'interruption. */
    fWorkLoop = IOWorkLoop::workLoop();
    fGate = fWorkLoop ? IOCommandGate::commandGate(this) : 0;
    if (!fWorkLoop || !fGate ||
        fWorkLoop->addEventSource(fGate) != kIOReturnSuccess) {
        GPULog("work loop / command gate : échec\n");
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
        GPULog("pas de source d'interruption : fonctionnement en scrutation\n");
    }

    /* Propriétés visibles dans ioreg, pour le diagnostic et pour que le
       userland sache ce qu'il a en face avant même d'ouvrir le service. */
    setProperty("QGPUVersion",   (unsigned long long) fVersion, 32);
    setProperty("QGPUCaps",      (unsigned long long) fCaps, 32);
    setProperty("QGPUShmemSize", (unsigned long long) fShmemSize, 32);
    {
        UInt32 tag = regRead(QGPU_REG_BACKEND_NAME);
        char   name[5];
        name[0] = tag >> 24; name[1] = tag >> 16; name[2] = tag >> 8;
        name[3] = tag; name[4] = 0;
        setProperty("QGPUBackend", name);
        GPULog("démarré : protocole v%lu, backend hôte '%s', caps 0x%lx, "
               "fenêtre %lu Mio, %d clients × %lu Mio\n", (unsigned long) fVersion, name,
               (unsigned long) fCaps, (unsigned long) (fShmemSize >> 20),
               QGPU_MAX_CLIENTS, (unsigned long) (fSlotSize >> 20));
    }

    registerService();
    return true;
}

void POMPPCGPU::stop(IOService * provider)
{
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
    if (fGate)       { fGate->release();       fGate = 0; }
    if (fWorkLoop)   { fWorkLoop->release();   fWorkLoop = 0; }
    if (fRegsMap)    { fRegsMap->release();    fRegsMap = 0; }
    if (fRegsRange)  { fRegsRange->release();  fRegsRange = 0; }
    if (fShmemRange) { fShmemRange->release(); fShmemRange = 0; }
    super::free();
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
    /* Réveille d'éventuels dormeurs de waitFence (contexte gated). */
    if (self->fGate) {
        self->fGate->commandWakeup(&self->fIRQCount, false);
    }
}

/* ─────────────────────────────── soumission ─────────────────────────────── */

struct SubmitArgs {
    UInt32 off, len;
    UInt32 fence, status, statusPC;
    IOReturn result;
};

IOReturn POMPPCGPU::submitGated(OSObject * owner, void * a0, void *, void *, void *)
{
    POMPPCGPU * self = (POMPPCGPU *) owner;
    SubmitArgs * a = (SubmitArgs *) a0;

    self->regWrite(QGPU_REG_SUBMIT_OFF, a->off);
    self->regWrite(QGPU_REG_SUBMIT_LEN, a->len);
    self->regWrite(QGPU_REG_DOORBELL, 1);
    /* Exécution synchrone côté QEMU : les résultats sont déjà là. */
    a->fence    = self->regRead(QGPU_REG_FENCE);
    a->status   = self->regRead(QGPU_REG_STATUS);
    a->statusPC = self->regRead(QGPU_REG_STATUS_PC);
    a->result   = kIOReturnSuccess;
    return kIOReturnSuccess;
}

IOReturn POMPPCGPU::submit(int slot, UInt32 off, UInt32 len,
                           UInt32 * fence, UInt32 * status, UInt32 * statusPC)
{
    SubmitArgs a;

    /* Le device revérifie, mais un flux hors de la tranche du client ne doit
       même pas partir : c'est un bug du client, pas un état du matériel. */
    if (slot < 0 || slot >= QGPU_MAX_CLIENTS ||
        (off & 3) || (len & 3) || len == 0 || off > fSlotSize ||
        len > fSlotSize - off) {
        return kIOReturnBadArgument;
    }
    a.off = off + (UInt32) slot * fSlotSize; a.len = len;
    fGate->runAction(&POMPPCGPU::submitGated, &a);
    *fence = a.fence; *status = a.status; *statusPC = a.statusPC;
    return a.result;
}

IOReturn POMPPCGPU::waitFence(UInt32 target, UInt32 timeoutMs, UInt32 * current)
{
    UInt32 waited = 0;

    /* Avec un device synchrone la fence est déjà atteinte ; la boucle ne sert
       qu'au futur device asynchrone. Scrutation à 1 ms, bornée : jamais de
       sommeil sans réveil garanti dans un kext de bring-up. */
    for (;;) {
        UInt32 f = regRead(QGPU_REG_FENCE);
        *current = f;
        if ((SInt32) (f - target) >= 0) {
            return kIOReturnSuccess;
        }
        if (waited >= timeoutMs) {
            return kIOReturnTimeout;
        }
        IOSleep(1);
        waited++;
    }
}

/* ─────────────────────────── tranches des clients ─────────────────────────── */

struct SlotArgs {
    int op;                       /* 0 = allouer, 1 = libérer */
    POMPPCGPUUserClient * client;
    int slot;
};

IOReturn POMPPCGPU::slotGated(OSObject * owner, void * a0, void *, void *, void *)
{
    POMPPCGPU * self = (POMPPCGPU *) owner;
    SlotArgs * a = (SlotArgs *) a0;
    int i;

    if (a->op == 0) {
        a->slot = -1;
        for (i = 0; i < QGPU_MAX_CLIENTS; i++) {
            if (!self->fClients[i]) {
                self->fClients[i] = a->client;
                a->slot = i;
                break;
            }
        }
    } else if (a->slot >= 0 && a->slot < QGPU_MAX_CLIENTS) {
        self->destroyClientObjects(a->slot);
        self->fClients[a->slot] = 0;
    }
    return kIOReturnSuccess;
}

int POMPPCGPU::allocSlot(POMPPCGPUUserClient * client)
{
    SlotArgs a;
    a.op = 0; a.client = client; a.slot = -1;
    fGate->runAction(&POMPPCGPU::slotGated, &a);
    return a.slot;
}

void POMPPCGPU::freeSlot(int slot)
{
    SlotArgs a;
    a.op = 1; a.client = 0; a.slot = slot;
    fGate->runAction(&POMPPCGPU::slotGated, &a);
}

IODeviceMemory * POMPPCGPU::slotRange(int slot)
{
    if (slot < 0 || slot >= QGPU_MAX_CLIENTS) {
        return 0;
    }
    return IODeviceMemory::withSubRange(fShmemRange, (IOPhysicalAddress) slot * fSlotSize,
                                        fSlotSize);
}

/* Contexte gated. Détruit les objets de la plage du client, une soumission par
   objet : un identifiant inutilisé fait échouer SA commande (et arrêterait un
   flux groupé), ce qui est ici attendu et sans conséquence. Le flux est écrit
   dans la première page de la tranche, que le client n'utilise plus. */
void POMPPCGPU::destroyClientObjects(int slot)
{
    IODeviceMemory * page = IODeviceMemory::withSubRange(fShmemRange,
                                (IOPhysicalAddress) slot * fSlotSize, PAGE_SIZE);
    IOMemoryMap * map = page ? page->map() : 0;
    UInt32 * w = map ? (UInt32 *) map->getVirtualAddress() : 0;
    UInt32 base = (UInt32) slot * fSlotSize;
    int i;

    if (w) {
        static const struct { UInt32 op, per_client; } kinds[3] = {
            { QGPU_OP_SURF_DESTROY, QGPU_CLIENT_SURF_IDS },
            { QGPU_OP_TEX_DESTROY,  QGPU_CLIENT_TEX_IDS },
            { QGPU_OP_CTX_DESTROY,  QGPU_CLIENT_CTX_IDS },
        };
        int k;
        for (k = 0; k < 3; k++) {
            for (i = 0; i < (int) kinds[k].per_client; i++) {
                w[0] = QGPU_CMD_HDR(kinds[k].op, 2);
                w[1] = (UInt32) slot * kinds[k].per_client + i;
                regWrite(QGPU_REG_SUBMIT_OFF, base);
                regWrite(QGPU_REG_SUBMIT_LEN, 8);
                regWrite(QGPU_REG_DOORBELL, 1);
            }
        }
    }
    if (map) {
        map->release();
    }
    if (page) {
        page->release();
    }
}

/* ─────────────────────────────── user client ────────────────────────────── */

IOReturn POMPPCGPU::newUserClient(task_t owningTask, void * securityID,
                                  UInt32 type, IOUserClient ** handler)
{
    POMPPCGPUUserClient * client = new POMPPCGPUUserClient;

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
    if (!fOwner || !super::start(provider)) {
        return false;
    }
    /* Une tranche de fenêtre et une plage d'identifiants par client. */
    fSlot = fOwner->allocSlot(this);
    if (fSlot < 0) {
        GPULog("déjà %d clients : ouverture refusée\n", QGPU_MAX_CLIENTS);
        return false;
    }
    return true;
}

void POMPPCGPUUserClient::stop(IOService * provider)
{
    super::stop(provider);
}

IOReturn POMPPCGPUUserClient::clientClose(void)
{
    if (fOwner && fSlot >= 0) {
        fOwner->freeSlot(fSlot);
        fSlot = -1;
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

    if (type != QGPU_UC_MEM_SHMEM || !fOwner || fSlot < 0) {
        return kIOReturnBadArgument;
    }
    /* withSubRange rend un objet déjà retenu : IOUserClient::mapClientMemory
       le relâchera après le mapping. */
    range = fOwner->slotRange(fSlot);
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
    if (!fOwner) {
        return kIOReturnNotAttached;
    }
    *version = fOwner->version();
    *caps    = fOwner->caps();
    *shmem   = fOwner->slotSize();
    *fence   = fOwner->fence();
    return kIOReturnSuccess;
}

IOReturn POMPPCGPUUserClient::ucSubmit(UInt32 off, UInt32 len, UInt32 * fence,
                                       UInt32 * status, UInt32 * statusPC, void *)
{
    if (!fOwner) {
        return kIOReturnNotAttached;
    }
    return fOwner->submit(fSlot, off, len, fence, status, statusPC);
}

IOReturn POMPPCGPUUserClient::ucWaitFence(UInt32 target, UInt32 timeoutMs, UInt32 * current,
                                          void *, void *, void *)
{
    if (!fOwner) {
        return kIOReturnNotAttached;
    }
    if (timeoutMs > 60000) {
        timeoutMs = 60000;
    }
    return fOwner->waitFence(target, timeoutMs, current);
}

/* Détruit les objets du client (et seulement les siens) ; il garde sa tranche. */
IOReturn POMPPCGPUUserClient::ucReset(void *, void *, void *, void *, void *, void *)
{
    int slot;
    if (!fOwner || fSlot < 0) {
        return kIOReturnNotAttached;
    }
    slot = fSlot;
    fOwner->freeSlot(slot);
    fSlot = fOwner->allocSlot(this);
    return fSlot >= 0 ? kIOReturnSuccess : kIOReturnNoResources;
}

IOReturn POMPPCGPUUserClient::ucGetSlot(UInt32 * index, UInt32 * base, UInt32 * ctxBase,
                                        UInt32 * surfBase, void *, void *)
{
    if (!fOwner || fSlot < 0) {
        return kIOReturnNotAttached;
    }
    *index    = fSlot;
    *base     = (UInt32) fSlot * fOwner->slotSize();
    *ctxBase  = (UInt32) fSlot * QGPU_CLIENT_CTX_IDS;
    *surfBase = (UInt32) fSlot * QGPU_CLIENT_SURF_IDS;
    return kIOReturnSuccess;
}
