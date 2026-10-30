/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (c) 2026 VERHILLE Arnaud */
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
#include <IOKit/graphics/IOGraphicsTypes.h>          /* IOGBounds */
#include <IOKit/graphics/IOGraphicsInterfaceTypes.h>   /* kIOAccelTypesKey… */
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IOKitKeys.h>                         /* kIOCFPlugInTypesKey */
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
 *   AccelCaps n'est publié que si la personnalité porte POMPPCQuartzExtreme à
 *   vrai (install.sh QE=1), ou à « auto » (le défaut depuis le 29/10) quand
 *   l'hôte le recommande (QGPU_CAP_QE_DEFAULT ; docs/quartz-extreme.md) :
 *   le WindowServer tente Quartz Extreme dès que ce masque est non nul
 *   (CGXGLDisplayContextInitialize, CoreGraphics) — il ne passe alors même
 *   plus par la liste d'autorisation de _isAccelUsable. */
#define POMPPC_GL_BUNDLE_NAME   "GLDriver-POMPPC"

/* ── Quartz Extreme : le plugin GA (POMPPCGA.plugin, guest/ga/) ──────────────
 *
 *   _CGXMappedDisplayStart (CoreGraphics) ne marque l'écran « accélérable »
 *   que si IOPSAllocateBlitEngine (IOKit) charge un plugin GA pour le
 *   framebuffer : IOCFPlugInTypes du framebuffer, type
 *   ACCF0000-0000-0000-0000-000a2789904e (docs/quartz-extreme.md §1 bis). En
 *   mode QE, le kext pose cette propriété ; le plugin ouvre alors le nub avec
 *   le type ci-dessous pour mapper la VRAM du framebuffer (mémoire 0) et en
 *   lire la géométrie (méthode 0 : largeur, hauteur, octets par ligne, bits
 *   par pixel). Les copies et remplissages sont faits par le plugin. */
#define POMPPC_GA_CLIENT_TYPE   0x504F4741UL    /* 'POGA' */
#define POMPPC_GA_MEM_VRAM      0
#define POMPPC_GA_GET_INFO      0
/* Méthode 1 (29/10) : rectangle de la forme de la surface `id` [x, y, l, h],
   tel que posé par IOAccelSetSurfaceShape. Le compositeur du WindowServer
   dessine chaque zone sale dans un viewport (0, 0, l, h) : c'est la forme de
   la surface d'écran qui la place, et le plugin GL l'applique. */
#define POMPPC_GA_SURF_BOUNDS   1
#define POMPPC_MAX_SURF_IDS     16
/* Méthodes 2 et 3 (29/10, P2) : export d'une fenêtre GL d'application. Le jeu
   publie [sid, texture hôte, largeur, hauteur] (texture 0xffffffff : retrait) ;
   le WindowServer cherche [sid] → [texture, largeur, hauteur, numéro]. Les
   identifiants hôte sont communs à toutes les tranches (un seul cœur). Une
   entrée meurt avec le client 2D qui l'a publiée. */
#define POMPPC_GA_EXPORT        2
#define POMPPC_GA_LOOKUP        3
/* Méthode 4 : [sid] → [x, y] de la surface au dernier Flush (preuve de la
   matrice : le jeu ne connaît pas la position de sa fenêtre à l'écran). */
#define POMPPC_GA_SURF_POS      4
/* Méthode 5 (29/10, P3) : [sid] → présentation par le noyau dans la région
   visible gardée au dernier SetShape, sans passer par le WindowServer.
   Curseur visible sur la fenêtre : retiré, présentation synchrone, remis ;
   30/10, curseur immobile : région moins le rectangle du curseur, asynchrone.
   kIOReturnBusy : région inconnue (pas encore de SetShape) — le jeu avise
   alors le WindowServer, qui fera le Flush comme avant. */
#define POMPPC_GA_PRESENT       5
#define POMPPC_MAX_EXPORTS      32
#define POMPPC_MAX_RECTS        16
/* Zone de commandes du client (fin de sa tranche), écrite par le kext au
   Flush d'une surface exportée : un SURF_PRESENT par rectangle visible. */
#define POMPPC_QE_CMD_BYTES     4096
/* 30/10 : curseur immobile sur une fenêtre de jeu — la présentation contourne
   son rectangle, asynchrone ; une sur POMPPC_QE_CURSOR_REFRESH + 1 reste
   synchrone (retrait, image, remise) pour rafraîchir le fond sous la flèche. */
#define POMPPC_QE_CURSOR_REFRESH 8

struct POMPPCExport {
    UInt32 sid, tex, w, h, seq;
    UInt32 surf, cmdoff;            /* surface hôte du jeu, zone de commandes (absolue) */
    SInt32 x, y;                    /* position à l'écran au dernier Flush */
    bool   pos;
    UInt32 half;                    /* P3 : moitié de la zone à écrire au prochain Flush */
    UInt32 qfence[2];               /* barrière de la dernière soumission de chaque moitié */
    IOGBounds cur;                  /* 30/10 : rectangle du curseur à la dernière remise */
    UInt32 carved;                  /* présentations qui l'ont contourné depuis */
    void * owner;                   /* client 2D qui a publié */
};
#define POMPPC_GA_PLUGIN        "POMPPCGA.plugin"
#define POMPPC_GA_TYPE_UUID     "ACCF0000-0000-0000-0000-000a2789904e"

#include <IOKit/graphics/IOAccelSurfaceConnect.h>

class POMPPCAccelerator : public IOAccelerator
{
    OSDeclareDefaultStructors(POMPPCAccelerator)

public:
    virtual IOReturn newUserClient(task_t owningTask, void * securityID,
                                   UInt32 type, IOUserClient ** handler);
    void             setSurfaceBounds(UInt32 id, const IOAccelBounds * b);
    bool             surfaceBounds(UInt32 id, IOAccelBounds * b);
    IOReturn         exportSurface(void * owner, UInt32 sid, UInt32 tex, UInt32 w, UInt32 h,
                                   UInt32 surf, UInt32 cmdoff);
    IOReturn         flushSurface(UInt32 sid, IOFramebuffer * fb, const IOAccelBounds * bounds,
                                  const IOAccelBounds * rects, UInt32 nrects);
    IOReturn         lookupSurface(UInt32 sid, UInt32 * tex, UInt32 * w, UInt32 * h,
                                   UInt32 * seq);
    void             dropExports(void * owner);
    IOReturn         surfacePos(UInt32 sid, UInt32 * x, UInt32 * y);
    void             setSurfaceShape(void * client, UInt32 sid, IOFramebuffer * fb,
                                     const IOAccelBounds * bounds,
                                     const IOAccelBounds * rects, UInt32 nrects);
    void             dropSurfaceShape(void * client);
    IOReturn         presentSurface(UInt32 sid);
    virtual void     free(void);

private:
    IOReturn         flushSurfaceLocked(UInt32 sid, IOFramebuffer * fb,
                                        const IOAccelBounds * bounds,
                                        const IOAccelBounds * rects, UInt32 nrects,
                                        bool direct);
    IOLock *         lock(void);
    struct Shape {
        void *          client;         /* client de surface qui l'a posée (0 : libre) */
        UInt32          sid, nrects;
        IOFramebuffer * fb;             /* retenu par ce client tant qu'il vit */
        IOAccelBounds   bounds, rects[POMPPC_MAX_RECTS];
    };
    Shape            fShapes[POMPPC_MAX_EXPORTS];
    IOLock *         fLock;
    POMPPCExport     fExports[POMPPC_MAX_EXPORTS];
    UInt32           fExportSeq;
    UInt32           fFlushErrs;
    IOAccelBounds    fSurfBounds[POMPPC_MAX_SURF_IDS];
    bool             fSurfKnown[POMPPC_MAX_SURF_IDS];
};

/* Quartz Extreme, P2 (exploration) : client IOAccelSurface du nub
   (kIOAccelSurfaceClientType = 0, méthodes de IOAccelSurfaceConnect.h). La
   surface est la VRAM du framebuffer elle-même, au rectangle de sa forme :
   les verrous rendent cette adresse dans la tâche cliente. */

class POMPPCSurfaceClient : public IOUserClient
{
    OSDeclareDefaultStructors(POMPPCSurfaceClient)

public:
    bool             initWithFramebuffer(task_t owningTask, IOFramebuffer * fb);
    virtual void     free(void);
    virtual IOReturn clientClose(void);
    virtual IOExternalMethod * getTargetAndMethodForIndex(IOService ** target,
                                                          UInt32 index);
    IOReturn sLock(UInt32 options, IOAccelSurfaceInformation * info, IOByteCount * size);
    IOReturn sLock0(IOAccelSurfaceInformation * info, IOByteCount * size);
    IOReturn sUnlock(UInt32 options);
    IOReturn sNop(void);
    IOReturn sGetState(UInt32 * state);
    IOReturn sRead(IOAccelSurfaceReadData * in, void * out, IOByteCount inSize,
                   IOByteCount * outSize);
    IOReturn sSetIDMode(UInt32 id, UInt32 mode);
    IOReturn sSetShape(UInt32 options, UInt32 fbIndex, IOAccelDeviceRegion * rgn,
                       IOByteCount size);
    IOReturn sSetShapeBacking(UInt32 options, UInt32 fbIndex, UInt32 addr, UInt32 rowBytes,
                              IOAccelDeviceRegion * rgn, IOByteCount size);
    IOReturn sSetShapeBackingLen(UInt32 options, UInt32 fbIndex, UInt32 addr,
                                 UInt32 rowBytes, UInt32 len, IOAccelDeviceRegion * rgn,
                                 IOByteCount size);
    IOReturn sSetScale(UInt32 options, void * scaling, IOByteCount size);
    IOReturn sFlush(UInt32 fbMask, UInt32 options);
    IOReturn sControl(UInt32 selector, UInt32 arg, UInt32 * result);

private:
    void     note(UInt32 index);
    IOReturn shape(IOAccelDeviceRegion * rgn, IOByteCount size);
    IOFramebuffer * fFB;            /* retenu */
    IOMemoryMap *   fMap;           /* VRAM dans la tâche cliente */
    task_t          fTask;
    UInt32          fID, fMode;
    IOAccelBounds   fBounds;
    IOAccelBounds   fRects[POMPPC_MAX_RECTS];   /* région visible (forme), P2 */
    UInt32          fNRects;
    UInt32          fCalls[kIOAccelNumSurfaceMethods];
};

class POMPPCGAClient : public IOUserClient
{
    OSDeclareDefaultStructors(POMPPCGAClient)

public:
    bool             initWithFramebuffer(task_t owningTask, IOFramebuffer * fb);
    virtual void     free(void);
    virtual IOReturn clientClose(void);
    virtual IOExternalMethod * getTargetAndMethodForIndex(IOService ** target,
                                                          UInt32 index);
    virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options,
                                         IOMemoryDescriptor ** memory);
    IOReturn gaGetInfo(UInt32 * width, UInt32 * height, UInt32 * rowBytes,
                       UInt32 * bitsPerPixel, void *, void *);
    IOReturn gaSurfBounds(UInt32 id, UInt32 * x, UInt32 * y, UInt32 * w, UInt32 * h, void *);
    IOReturn gaExport(UInt32 sid, UInt32 tex, UInt32 w, UInt32 h, UInt32 surf, UInt32 cmdoff);
    IOReturn gaLookup(UInt32 sid, UInt32 * tex, UInt32 * w, UInt32 * h, UInt32 * seq, void *);
    IOReturn gaSurfPos(UInt32 sid, UInt32 * x, UInt32 * y, void *, void *, void *);
    IOReturn gaPresent(UInt32 sid, void *, void *, void *, void *, void *);

private:
    IOFramebuffer * fFB;            /* retenu */
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
    UInt32   fence(void);
    /* Bug hunt 3 — APPELS EN VOL. Tout service rendu au user client entre par
       enterCall() (compte l'appel, PUIS relit fStopping) et sort par
       leaveCall() ; stop() pose fStopping, PUIS attend que le compte tombe à
       zéro avant de retirer la gate. Sans lui, un appel qui avait passé le
       test de fStopping (ou attendait la gate derrière un doorbell synchrone)
       entrait dans une gate déjà retirée : workLoop NULL, panic. */
    bool     enterCall(void);
    void     leaveCall(void);
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
    IOFramebuffer *  linkedFramebuffer(void);                /* QE : le premier lié, retenu */
    void             removeGAPlugin(IOService * fb);
    bool             quartzExtreme(void)      { return fQE; }
    /* `len` porte les drapeaux POMPPC_SUB_* dans ses bits hauts (voir plus haut). */
    /* 29/10 (QE, P2) : soumission du noyau, `nwords` mots écrits à l'offset
       absolu `off` de la fenêtre partagée (zone réservée par le client, fin
       de sa tranche). Synchrone : rend le statut de l'hôte. P3 : `*fence`
       non nul en entrée = asynchrone si l'hôte a fini la soumission de cette
       barrière (sinon la zone est encore lue : synchrone) ; en sortie, la
       barrière de CETTE soumission (0 si elle a été synchrone). */
    IOReturn kernelSubmit(UInt32 off, const UInt32 * cmds, UInt32 nwords, UInt32 * status,
                          UInt32 * fence = 0);
    IOReturn submit(int slot, POMPPCGPUUserClient * client, UInt32 off, UInt32 len,
                    UInt32 * fence, UInt32 * status, UInt32 * statusPC);
    IOReturn waitFence(UInt32 target, UInt32 timeoutMs, UInt32 * current);

private:
    IOReturn waitFenceCounted(UInt32 target, UInt32 timeoutMs, UInt32 * current);
    /* registres big-endian côté QEMU, CPU big-endian : pas d'échange d'octets */
    UInt32 regRead(UInt32 offset) { return fRegs[offset >> 2]; }
    void   regWrite(UInt32 offset, UInt32 value)
    {
        fRegs[offset >> 2] = value;
        OSSynchronizeIO();
    }

    static IOReturn kernelSubmitGated(OSObject * owner, void * a0, void * a1,
                                      void * a2, void * a3);
    static IOReturn submitGated(OSObject * owner, void * a0, void * a1,
                                void * a2, void * a3);
    static IOReturn waitGated(OSObject * owner, void * a0, void * a1,
                              void * a2, void * a3);
    static IOReturn slotGated(OSObject * owner, void * a0, void * a1,
                              void * a2, void * a3);
    /* Contexte gated : dort jusqu'à ce que FENCE atteigne `target`. */
    IOReturn sleepForFence(UInt32 target, UInt32 timeoutMs, UInt32 * current);
    /* A6 (L2, 14/10/2026) : rend le verdict du nettoyage. kIOReturnSuccess
       = objets détruits ET tranche plus lue (barrière atteinte). Sinon
       `*fence` vaut la barrière du CLIENT_RESET accepté mais pas encore
       atteint, ou 0 s'il n'a jamais été mis en file. */
    IOReturn destroyClientObjects(int slot, UInt32 * fence);
    /* A6 : contexte gated. Une tranche « sale » (nettoyage échoué) peut-elle
       être redonnée ? Relance d'abord un CLIENT_RESET jamais parti, puis
       regarde (ou attend, `mayWait`) sa barrière. */
    bool     slotCleaned(int slot, bool mayWait);
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
    IOMemoryMap *     fShmemMap;          /* QE P2 : écriture noyau des présentations */
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
    /* Bug hunt 3 : un FREE arrivé pendant une destruction en cours (RESET du
       même client) est noté ici ; le fil occupé rend la tranche en sortant.
       Sans cela, la tranche restait prise jusqu'au déchargement du kext. */
    POMPPCGPUUserClient * fFreePending[POMPPC_KEXT_MAX_CLIENTS];
    /* A6 (L2) : tranche dont le nettoyage a échoué. allocSlot ne la redonne
       qu'une fois propre : SLOT_DIRTY_UNSENT = CLIENT_RESET jamais mis en file
       (refusé, file non drainée) — à relancer avant tout ; SLOT_DIRTY_FENCE =
       accepté, barrière fSlotFence pas encore atteinte (FIFO : quand elle
       l'est, objets détruits ET tranche plus lue). Sous la gate. */
    UInt32   fSlotDirty[POMPPC_KEXT_MAX_CLIENTS];
    UInt32   fSlotFence[POMPPC_KEXT_MAX_CLIENTS];
    /* Bug hunt 3 : appels en vol (enterCall/leaveCall), atomique. */
    volatile SInt32 fCallers;

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
    bool                           fQE;           /* Quartz Extreme demandé (AccelCaps) */
};

class POMPPCGPUUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(POMPPCGPUUserClient)

public:
    virtual bool     initWithTask(task_t owningTask, void * securityID, UInt32 type);
    virtual bool     start(IOService * provider);
    virtual void     stop(IOService * provider);
    virtual void     free(void);
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
    /* Bug hunt 3 : RETENU de start() à free(). fOwner est effacé par stop(),
       mais un appel déjà parti l'a lu avant : l'objet POMPPCGPU (gate,
       registres) doit survivre jusqu'à ce que ce client disparaisse. */
    POMPPCGPU * fOwnerRef;
    task_t      fTask;
    int         fSlot;
};

#endif /* POMPPCGPU_H */
