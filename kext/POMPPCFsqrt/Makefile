# Makefile — POMPPCFsqrt.kext, construit DANS l'invité Tiger (voir fsqrt.c).
#
#   make                       → POMPPCFsqrt.kext
#   sudo make load / unload    → essai à chaud (kextload / kextunload)
#
# Même recette que kext/POMPPCGPU (gcc 4.0, -mlong-branch), en C seul.

SDK        = /Developer/SDKs/MacOSX10.4u.sdk
KFRAMEWORK = $(SDK)/System/Library/Frameworks/Kernel.framework
CC         = /usr/bin/gcc-4.0
LD         = /usr/bin/ld

KEXT       = POMPPCFsqrt.kext
BIN        = POMPPCFsqrt

CFLAGS   = -arch ppc -static -O2 -mlong-branch -fno-builtin -fno-common -Wall -nostdinc \
           -msoft-float -I$(KFRAMEWORK)/Headers -DKERNEL -DKERNEL_PRIVATE -isysroot $(SDK)
LDFLAGS  = -arch ppc -static -r -syslibroot $(SDK)
CC_KEXT  := $(shell $(CC) -arch ppc -print-file-name=libcc_kext.a)
LDLIBS   = -lkmod $(CC_KEXT)

all: $(KEXT)

fsqrt.o: fsqrt.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): fsqrt.o
	$(LD) $(LDFLAGS) -o $@ fsqrt.o $(LDLIBS)

$(KEXT): $(BIN) Info.plist
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp Info.plist $(KEXT)/Contents/
	cp $(BIN) $(KEXT)/Contents/MacOS/

load: $(KEXT)
	rm -rf /tmp/$(KEXT) && cp -R $(KEXT) /tmp/ && chown -R root:wheel /tmp/$(KEXT)
	kextload /tmp/$(KEXT)

unload:
	kextunload -b net.pomppc.POMPPCFsqrt

clean:
	rm -rf $(KEXT) $(BIN) fsqrt.o
