/*
 * POMPPCQFB.h — pilote IOFramebuffer pour le framebuffer paravirtualisé
 * « qfb1 » exposé en PCI par QEMU (hw/display/qfb-pci.c).
 *
 * Cible : Mac OS X 10.4 (Tiger) PowerPC, machine QEMU mac99.
 * Équivalent Tiger du pilote NuBus 68k de Solra Bizna (mac_qfb_driver).
 *
 * Note ABI : les signatures suivent les en-têtes IOGraphics de 10.4
 * (attributs en UInt32 ; à partir de 10.5 ce sont des uintptr_t).
 */

#ifndef POMPPCQFB_H
#define POMPPCQFB_H

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "qfb_regs.h"

/* PAGE_SIZE est fourni par les en-têtes noyau ; filet de sécurité. */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define kQFBMaxModes   16
#define kQFBDepthCount 3          /* 8 bpp indexé, 16 bpp 555, 32 bpp xRGB */

typedef struct {
    UInt32 width;
    UInt32 height;
    UInt32 refresh;               /* Hz entiers */
} QFBMode;

class POMPPCQFB : public IOFramebuffer
{
    OSDeclareDefaultStructors(POMPPCQFB)

public:
    /* cycle de vie */
    virtual bool     start(IOService * provider);
    virtual void     stop(IOService * provider);
    virtual void     free(void);
    virtual IOReturn enableController(void);
    virtual bool     isConsoleDevice(void);

    /* mémoire */
    virtual IODeviceMemory * getApertureRange(IOPixelAperture aperture);
    virtual IODeviceMemory * getVRAMRange(void);

    /* modes et formats */
    virtual const char * getPixelFormats(void);
    virtual IOItemCount  getDisplayModeCount(void);
    virtual IOReturn     getDisplayModes(IODisplayModeID * allDisplayModes);
    virtual IOReturn     getInformationForDisplayMode(IODisplayModeID displayMode,
                                                      IODisplayModeInformation * info);
    virtual UInt64       getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                                       IOIndex depth);
    virtual IOReturn     getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                             IOPixelAperture aperture,
                                             IOPixelInformation * pixelInfo);
    virtual IOReturn     getCurrentDisplayMode(IODisplayModeID * displayMode, IOIndex * depth);
    virtual IOReturn     setDisplayMode(IODisplayModeID displayMode, IOIndex depth);

    /* connexion */
    virtual IOItemCount  getConnectionCount(void);
    virtual IOReturn     connectFlags(IOIndex connectIndex, IODisplayModeID displayMode,
                                      IOOptionBits * flags);
    virtual bool         hasDDCConnect(IOIndex connectIndex);

    /* attributs */
    virtual IOReturn getAttribute(IOSelect attribute, UInt32 * value);
    virtual IOReturn setAttribute(IOSelect attribute, UInt32 value);
    virtual IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               UInt32 * value);
    virtual IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                               UInt32 value);

    /* palette et gamma */
    virtual IOReturn setCLUTWithEntries(IOColorEntry * colors, UInt32 index,
                                        UInt32 numEntries, IOOptionBits options);
    virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                   UInt32 dataWidth, void * data);

    /* interruption VBL */
    virtual IOReturn registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
                                              OSObject * target, void * ref,
                                              void ** interruptRef);
    virtual IOReturn unregisterInterrupt(void * interruptRef);
    virtual IOReturn setInterruptState(void * interruptRef, UInt32 state);

private:
    /* accès registres : la région est déclarée big endian côté QEMU et le
       processeur invité est big endian → aucun échange d'octets ici. */
    UInt32 regRead(UInt32 offset)  { return fRegs[offset >> 2]; }
    void   regWrite(UInt32 offset, UInt32 value)
    {
        fRegs[offset >> 2] = value;
        OSSynchronizeIO();
    }

    void     buildModeList(void);
    void     programMode(void);
    void     enableVBL(bool enable);
    UInt32   strideForMode(UInt32 modeIndex, IOIndex depth);

    static UInt32 bitsForDepth(IOIndex depth);   /* 8 / 16 / 32 */
    static UInt32 qfbDepthForDepth(IOIndex depth); /* 8 / 16 / 24 */

    static void   vblAction(OSObject * owner, IOInterruptEventSource * src, int count);
    static bool   vblFilter(OSObject * owner, IOFilterInterruptEventSource * src);

    IOPCIDevice *    fPCI;
    IODeviceMemory * fVRAMRange;
    IODeviceMemory * fRegsRange;
    IOMemoryMap *    fRegsMap;
    volatile UInt32 * fRegs;

    QFBMode  fModes[kQFBMaxModes];
    UInt32   fModeCount;
    UInt32   fDefaultMode;      /* index dans fModes */
    UInt32   fCurrentMode;      /* index dans fModes */
    IOIndex  fCurrentDepth;     /* 0..kQFBDepthCount-1 */

    IOFilterInterruptEventSource * fVBLSource;
    IOFBInterruptProc              fVBLProc;
    OSObject *                     fVBLTarget;
    void *                         fVBLRef;
    bool                           fVBLEnabled;
};

#endif /* POMPPCQFB_H */
