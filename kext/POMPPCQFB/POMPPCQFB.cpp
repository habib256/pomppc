/*
 * POMPPCQFB.cpp — IOFramebuffer pour le device PCI « qfb1 » de QEMU.
 * Voir POMPPCQFB.h. Licence : identique au projet POMPPC.
 */

#include "POMPPCQFB.h"

#define super IOFramebuffer
OSDefineMetaClassAndStructors(POMPPCQFB, IOFramebuffer)

#define QFBLog(fmt, args...) IOLog("POMPPCQFB: " fmt, ## args)

/* Catalogue proposé à Tiger. Les modes qui ne tiennent pas dans la VRAM en
   32 bpp sont écartés à l'exécution. */
static const QFBMode kQFBBuiltinModes[] = {
    {  640,  480, 60 },
    {  800,  600, 60 },
    { 1024,  768, 60 },
    { 1152,  870, 75 },
    { 1280,  800, 60 },
    { 1280, 1024, 60 },
    { 1440,  900, 60 },
    { 1600, 1200, 60 },
    { 1680, 1050, 60 },
    { 1920, 1080, 60 },
    { 1920, 1200, 60 },
};
#define kQFBBuiltinModeCount (sizeof(kQFBBuiltinModes) / sizeof(kQFBBuiltinModes[0]))

static const char kQFBPixelFormats[] =
    IO32BitDirectPixels "\0"
    IO16BitDirectPixels "\0"
    IO8BitIndexedPixels "\0\0";

UInt32 POMPPCQFB::bitsForDepth(IOIndex depth)
{
    switch (depth) {
    case 0:  return 8;
    case 1:  return 16;
    default: return 32;
    }
}

UInt32 POMPPCQFB::qfbDepthForDepth(IOIndex depth)
{
    switch (depth) {
    case 0:  return 8;
    case 1:  return 16;
    default: return 24;   /* 24 = 32 bits par pixel, xRGB (contrat qfb1) */
    }
}

UInt32 POMPPCQFB::strideForMode(UInt32 modeIndex, IOIndex depth)
{
    return QFB_STRIDE(fModes[modeIndex].width, qfbDepthForDepth(depth));
}

#pragma mark - cycle de vie

bool POMPPCQFB::start(IOService * provider)
{
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) {
        return false;
    }

    fVRAMRange = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    fRegsRange = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1);
    if (!fVRAMRange || !fRegsRange) {
        QFBLog("BAR0 (VRAM) ou BAR1 (registres) absent — Open Firmware n'a pas "
               "assigné les ressources PCI ?\n");
        return false;
    }
    fVRAMRange->retain();
    fRegsRange->retain();

    fRegsMap = fRegsRange->map();
    if (!fRegsMap) {
        QFBLog("impossible de mapper les registres\n");
        return false;
    }
    fRegs = (volatile UInt32 *) fRegsMap->getVirtualAddress();

    fPCI->setMemoryEnable(true);

    if (regRead(QFB_VERSION) != QFB_MAGIC) {
        QFBLog("signature invalide (0x%08lx au lieu de 'qfb1')\n",
               (unsigned long) regRead(QFB_VERSION));
        return false;
    }

    fCurrentDepth = kQFBDepthCount - 1;   /* 32 bpp par défaut */
    buildModeList();
    fCurrentMode  = fDefaultMode;
    fVBLProc      = 0;
    fVBLTarget    = 0;
    fVBLRef       = 0;
    fVBLEnabled   = false;

    if (!super::start(provider)) {
        QFBLog("IOFramebuffer::start a échoué\n");
        return false;
    }

    /* Source d'interruption VBL — après super::start(), sinon getWorkLoop()
       peut encore renvoyer NULL. Elle reste masquée tant que personne ne
       s'enregistre : une IRQ à 60 Hz coûte cher sous TCG. */
    IOWorkLoop * wl = getWorkLoop();
    if (wl) {
        fVBLSource = IOFilterInterruptEventSource::filterInterruptEventSource(
                         this,
                         (IOInterruptEventSource::Action) &POMPPCQFB::vblAction,
                         (IOFilterInterruptEventSource::Filter) &POMPPCQFB::vblFilter,
                         provider, 0);
        if (fVBLSource && (kIOReturnSuccess != wl->addEventSource(fVBLSource))) {
            fVBLSource->release();
            fVBLSource = 0;
        }
        if (fVBLSource) {
            fVBLSource->disable();
        }
    }

    QFBLog("démarré (VRAM %lu Mio, mode demandé %lux%lux%lu)\n",
           (unsigned long) (QFB_VRAM_SIZE >> 20),
           (unsigned long) regRead(QFB_CUSTOM_WIDTH),
           (unsigned long) regRead(QFB_CUSTOM_HEIGHT),
           (unsigned long) regRead(QFB_CUSTOM_DEPTH));
    return true;
}

void POMPPCQFB::stop(IOService * provider)
{
    enableVBL(false);

    if (fVBLSource) {
        IOWorkLoop * wl = getWorkLoop();
        if (wl) {
            wl->removeEventSource(fVBLSource);
        }
        fVBLSource->release();
        fVBLSource = 0;
    }

    super::stop(provider);
}

void POMPPCQFB::free(void)
{
    if (fRegsMap)    { fRegsMap->release();    fRegsMap = 0; }
    if (fRegsRange)  { fRegsRange->release();  fRegsRange = 0; }
    if (fVRAMRange)  { fVRAMRange->release();  fVRAMRange = 0; }
    super::free();
}

bool POMPPCQFB::isConsoleDevice(void)
{
    /* La console Open Firmware reste sur le device VGA : on ne revendique pas
       le rôle d'écran de boot. */
    return false;
}

IOReturn POMPPCQFB::enableController(void)
{
    if (regRead(QFB_VERSION) != QFB_MAGIC) {
        return kIOReturnUnsupported;
    }

    if (fModeCount == 0) {
        buildModeList();
        fCurrentMode = fDefaultMode;
    }
    programMode();

    QFBLog("%lu modes publiés, défaut %lux%lu\n",
           (unsigned long) fModeCount,
           (unsigned long) fModes[fDefaultMode].width,
           (unsigned long) fModes[fDefaultMode].height);
    return kIOReturnSuccess;
}

#pragma mark - modes

void POMPPCQFB::buildModeList(void)
{
    UInt32 i;

    fModeCount   = 0;
    fDefaultMode = 0;

    for (i = 0; i < kQFBBuiltinModeCount && fModeCount < kQFBMaxModes; i++) {
        UInt32 stride = QFB_STRIDE(kQFBBuiltinModes[i].width, 24);
        if ((stride * kQFBBuiltinModes[i].height) > QFB_VRAM_SIZE) {
            continue;
        }
        fModes[fModeCount++] = kQFBBuiltinModes[i];
        if (kQFBBuiltinModes[i].width == 1024 && kQFBBuiltinModes[i].height == 768) {
            fDefaultMode = fModeCount - 1;
        }
    }

    /* Mode demandé sur la ligne de commande QEMU (-device qfb-pci,width=,height=) :
       il devient le mode par défaut, et il est ajouté s'il manque. */
    UInt32 cw = regRead(QFB_CUSTOM_WIDTH);
    UInt32 ch = regRead(QFB_CUSTOM_HEIGHT);
    if (cw >= 32 && ch >= 32 && cw <= QFB_MAX_WIDTH && ch <= QFB_MAX_HEIGHT
        && (QFB_STRIDE(cw, 24) * ch) <= QFB_VRAM_SIZE) {
        bool found = false;
        for (i = 0; i < fModeCount; i++) {
            if (fModes[i].width == cw && fModes[i].height == ch) {
                fDefaultMode = i;
                found = true;
                break;
            }
        }
        if (!found && fModeCount < kQFBMaxModes) {
            fModes[fModeCount].width   = cw;
            fModes[fModeCount].height  = ch;
            fModes[fModeCount].refresh = 60;
            fDefaultMode = fModeCount;
            fModeCount++;
        }
    }

    if (fModeCount == 0) {   /* ne devrait pas arriver */
        fModes[0].width = 640; fModes[0].height = 480; fModes[0].refresh = 60;
        fModeCount = 1; fDefaultMode = 0;
    }
}

void POMPPCQFB::programMode(void)
{
    regWrite(QFB_MODE_BASE,   0);
    regWrite(QFB_MODE_WIDTH,  fModes[fCurrentMode].width);
    regWrite(QFB_MODE_HEIGHT, fModes[fCurrentMode].height);
    regWrite(QFB_MODE_DEPTH,  qfbDepthForDepth(fCurrentDepth));
}

const char * POMPPCQFB::getPixelFormats(void)
{
    return kQFBPixelFormats;
}

IOItemCount POMPPCQFB::getDisplayModeCount(void)
{
    return fModeCount;
}

IOReturn POMPPCQFB::getDisplayModes(IODisplayModeID * allDisplayModes)
{
    if (!allDisplayModes) {
        return kIOReturnBadArgument;
    }
    for (UInt32 i = 0; i < fModeCount; i++) {
        allDisplayModes[i] = (IODisplayModeID) (i + 1);
    }
    return kIOReturnSuccess;
}

IOReturn POMPPCQFB::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                 IODisplayModeInformation * info)
{
    UInt32 idx = (UInt32) displayMode - 1;

    if (!info || idx >= fModeCount) {
        return kIOReturnBadArgument;
    }

    bzero(info, sizeof(*info));
    info->nominalWidth  = fModes[idx].width;
    info->nominalHeight = fModes[idx].height;
    info->refreshRate   = fModes[idx].refresh << 16;   /* 16.16 */
    info->maxDepthIndex = kQFBDepthCount - 1;
    info->flags         = kDisplayModeValidFlag | kDisplayModeSafeFlag;
    if (idx == fDefaultMode) {
        info->flags |= kDisplayModeDefaultFlag;
    }
    return kIOReturnSuccess;
}

UInt64 POMPPCQFB::getPixelFormatsForDisplayMode(IODisplayModeID /*displayMode*/,
                                                IOIndex /*depth*/)
{
    return 0;   /* obsolète : IOGraphics attend 0 */
}

IOReturn POMPPCQFB::getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                        IOPixelAperture aperture,
                                        IOPixelInformation * pixelInfo)
{
    UInt32 idx = (UInt32) displayMode - 1;

    if (!pixelInfo || idx >= fModeCount || depth < 0 || depth >= kQFBDepthCount
        || aperture != kIOFBSystemAperture) {
        return kIOReturnBadArgument;
    }

    bzero(pixelInfo, sizeof(*pixelInfo));
    pixelInfo->activeWidth  = fModes[idx].width;
    pixelInfo->activeHeight = fModes[idx].height;
    pixelInfo->bytesPerRow  = strideForMode(idx, depth);
    pixelInfo->bytesPerPlane = 0;
    pixelInfo->bitsPerPixel = bitsForDepth(depth);

    switch (depth) {
    case 0:
        bcopy(IO8BitIndexedPixels, pixelInfo->pixelFormat,
              sizeof(IO8BitIndexedPixels));
        pixelInfo->pixelType         = kIOCLUTPixels;
        pixelInfo->componentCount    = 1;
        pixelInfo->bitsPerComponent  = 8;
        break;
    case 1:
        bcopy(IO16BitDirectPixels, pixelInfo->pixelFormat,
              sizeof(IO16BitDirectPixels));
        pixelInfo->pixelType         = kIORGBDirectPixels;
        pixelInfo->componentCount    = 3;
        pixelInfo->bitsPerComponent  = 5;
        pixelInfo->componentMasks[0] = 0x7C00;
        pixelInfo->componentMasks[1] = 0x03E0;
        pixelInfo->componentMasks[2] = 0x001F;
        break;
    default:
        bcopy(IO32BitDirectPixels, pixelInfo->pixelFormat,
              sizeof(IO32BitDirectPixels));
        pixelInfo->pixelType         = kIORGBDirectPixels;
        pixelInfo->componentCount    = 3;
        pixelInfo->bitsPerComponent  = 8;
        pixelInfo->componentMasks[0] = 0x00FF0000;
        pixelInfo->componentMasks[1] = 0x0000FF00;
        pixelInfo->componentMasks[2] = 0x000000FF;
        break;
    }

    return kIOReturnSuccess;
}

IOReturn POMPPCQFB::getCurrentDisplayMode(IODisplayModeID * displayMode, IOIndex * depth)
{
    if (displayMode) {
        *displayMode = (IODisplayModeID) (fCurrentMode + 1);
    }
    if (depth) {
        *depth = fCurrentDepth;
    }
    return kIOReturnSuccess;
}

IOReturn POMPPCQFB::setDisplayMode(IODisplayModeID displayMode, IOIndex depth)
{
    UInt32 idx = (UInt32) displayMode - 1;

    if (idx >= fModeCount || depth < 0 || depth >= kQFBDepthCount) {
        return kIOReturnBadArgument;
    }

    fCurrentMode  = idx;
    fCurrentDepth = depth;
    programMode();

    return kIOReturnSuccess;
}

#pragma mark - mémoire

IODeviceMemory * POMPPCQFB::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture || !fVRAMRange) {
        return 0;
    }

    IOByteCount length = strideForMode(fCurrentMode, fCurrentDepth)
                       * fModes[fCurrentMode].height;
    length = (length + (PAGE_SIZE - 1)) & ~((IOByteCount) PAGE_SIZE - 1);
    if (length > QFB_VRAM_SIZE) {
        length = QFB_VRAM_SIZE;
    }

    return IODeviceMemory::withSubRange(fVRAMRange, 0, length);
}

IODeviceMemory * POMPPCQFB::getVRAMRange(void)
{
    if (!fVRAMRange) {
        return 0;
    }
    return IODeviceMemory::withSubRange(fVRAMRange, 0, QFB_VRAM_SIZE);
}

#pragma mark - connexion et attributs

IOItemCount POMPPCQFB::getConnectionCount(void)
{
    return 1;
}

IOReturn POMPPCQFB::connectFlags(IOIndex /*connectIndex*/, IODisplayModeID displayMode,
                                 IOOptionBits * flags)
{
    UInt32 idx = (UInt32) displayMode - 1;

    if (!flags || idx >= fModeCount) {
        return kIOReturnBadArgument;
    }

    *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
    if (idx == fDefaultMode) {
        *flags |= kDisplayModeDefaultFlag;
    }
    return kIOReturnSuccess;
}

bool POMPPCQFB::hasDDCConnect(IOIndex /*connectIndex*/)
{
    return false;
}

IOReturn POMPPCQFB::getAttribute(IOSelect attribute, UInt32 * value)
{
    switch (attribute) {
    case kIOHardwareCursorAttribute:
        if (value) {
            *value = 0;          /* pas de curseur matériel dans qfb1 */
        }
        return kIOReturnSuccess;
    default:
        return super::getAttribute(attribute, value);
    }
}

IOReturn POMPPCQFB::setAttribute(IOSelect attribute, UInt32 value)
{
    switch (attribute) {
    case kIOPowerAttribute:
        return kIOReturnSuccess;
    default:
        return super::setAttribute(attribute, value);
    }
}

IOReturn POMPPCQFB::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                              UInt32 * value)
{
    return super::getAttributeForConnection(connectIndex, attribute, value);
}

IOReturn POMPPCQFB::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute,
                                              UInt32 value)
{
    switch (attribute) {
    case kConnectionPower:
        return kIOReturnSuccess;
    default:
        return super::setAttributeForConnection(connectIndex, attribute, value);
    }
}

#pragma mark - palette et gamma

IOReturn POMPPCQFB::setCLUTWithEntries(IOColorEntry * colors, UInt32 index,
                                       UInt32 numEntries, IOOptionBits options)
{
    if (!colors) {
        return kIOReturnBadArgument;
    }

    for (UInt32 i = 0; i < numEntries; i++) {
        UInt32 slot = (options & kSetCLUTByValue) ? colors[i].index : (index + i);
        if (slot > 255) {
            continue;
        }
        regWrite(QFB_PAL_INDEX, slot);
        regWrite(QFB_PAL_COLOR,
                 (((UInt32) (colors[i].red   >> 8)) << 16) |
                 (((UInt32) (colors[i].green >> 8)) <<  8) |
                  ((UInt32) (colors[i].blue  >> 8)));
    }
    return kIOReturnSuccess;
}

IOReturn POMPPCQFB::setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                  UInt32 dataWidth, void * data)
{
    if (!data || dataCount == 0 || channelCount == 0
        || (dataWidth != 8 && dataWidth != 16)) {
        return kIOReturnBadArgument;
    }

    for (UInt32 i = 0; i < 256; i++) {
        UInt32 pos = (dataCount == 1) ? 0 : ((i * (dataCount - 1)) / 255);
        UInt32 r, g, b;

        if (dataWidth == 8) {
            const UInt8 * d = (const UInt8 *) data;
            r = d[pos];
            g = (channelCount > 1) ? d[dataCount + pos] : r;
            b = (channelCount > 2) ? d[2 * dataCount + pos] : r;
        } else {
            const UInt16 * d = (const UInt16 *) data;
            r = d[pos] >> 8;
            g = (channelCount > 1) ? (d[dataCount + pos] >> 8) : r;
            b = (channelCount > 2) ? (d[2 * dataCount + pos] >> 8) : r;
        }

        regWrite(QFB_LUT_INDEX, i);
        regWrite(QFB_LUT_COLOR, (r << 16) | (g << 8) | b);
    }
    return kIOReturnSuccess;
}

#pragma mark - interruption VBL

bool POMPPCQFB::vblFilter(OSObject * owner, IOFilterInterruptEventSource * /*src*/)
{
    POMPPCQFB * self = OSDynamicCast(POMPPCQFB, owner);

    if (!self || !self->fRegs) {
        return false;
    }
    if (!(self->regRead(QFB_IRQ) & QFB_IRQ_VBL)) {
        return false;                      /* pas pour nous */
    }
    self->regWrite(QFB_IRQ, QFB_IRQ_VBL);  /* acquittement */
    return true;
}

void POMPPCQFB::vblAction(OSObject * owner, IOInterruptEventSource * /*src*/, int /*count*/)
{
    POMPPCQFB * self = OSDynamicCast(POMPPCQFB, owner);

    if (self && self->fVBLProc) {
        self->fVBLProc(self->fVBLTarget, self->fVBLRef);
    }
}

void POMPPCQFB::enableVBL(bool enable)
{
    if (!fVBLSource || (enable == fVBLEnabled)) {
        return;
    }

    if (enable) {
        fVBLSource->enable();
        regWrite(QFB_IRQ_MASK, QFB_IRQ_VBL);
    } else {
        regWrite(QFB_IRQ_MASK, 0);
        fVBLSource->disable();
    }
    fVBLEnabled = enable;
}

IOReturn POMPPCQFB::registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc,
                                             OSObject * target, void * ref,
                                             void ** interruptRef)
{
    if (interruptType != kIOFBVBLInterruptType) {
        return kIOReturnUnsupported;
    }
    if (!fVBLSource) {
        return kIOReturnUnsupported;
    }
    if (fVBLProc) {
        return kIOReturnBusy;
    }

    fVBLProc   = proc;
    fVBLTarget = target;
    fVBLRef    = ref;
    if (interruptRef) {
        *interruptRef = this;
    }
    enableVBL(true);
    return kIOReturnSuccess;
}

IOReturn POMPPCQFB::unregisterInterrupt(void * interruptRef)
{
    if (interruptRef != this) {
        return kIOReturnBadArgument;
    }
    enableVBL(false);
    fVBLProc   = 0;
    fVBLTarget = 0;
    fVBLRef    = 0;
    return kIOReturnSuccess;
}

IOReturn POMPPCQFB::setInterruptState(void * interruptRef, UInt32 state)
{
    if (interruptRef != this) {
        return kIOReturnBadArgument;
    }
    enableVBL(state != 0);
    return kIOReturnSuccess;
}
