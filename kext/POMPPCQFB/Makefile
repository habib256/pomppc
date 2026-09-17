# Makefile — construction de POMPPCQFB.kext DANS l'invité Tiger (PowerPC).
#
#   Prérequis : Xcode 2.5 (gcc 4.0.1 + SDK MacOSX10.4u) installé dans l'invité.
#   Usage     : make          → POMPPCQFB.kext
#               sudo make install
#               sudo make load
#
# -mlong-branch est OBLIGATOIRE : sans lui, kld refuse le kext (« relocation
# overflow … displacement too large ») car les appels au noyau dépassent les
# 24 bits d'un `bl`. C'est le flag que Xcode pose sur tout kext PPC. Vu en vrai.
#
# Le compilateur DOIT être gcc 4.0 : l'ABI C++ du noyau 10.4 est celle de
# gcc 4.0. Un kext C++ compilé par un autre compilateur charge puis échoue de
# façon obscure (start() jamais appelé, ou panic au premier appel virtuel).

SDK        = /Developer/SDKs/MacOSX10.4u.sdk
KFRAMEWORK = $(SDK)/System/Library/Frameworks/Kernel.framework
CXX        = /usr/bin/g++-4.0
CC         = /usr/bin/gcc-4.0
LD         = /usr/bin/ld

KEXT       = POMPPCQFB.kext
BIN        = POMPPCQFB
OBJS       = POMPPCQFB.o POMPPCQFB_info.o

CXXFLAGS = -arch ppc -static -O2 -mlong-branch \
           -fno-builtin -fno-exceptions -fno-rtti -fno-common -fapple-kext \
           -msoft-float -fno-strict-aliasing -Wall -Wno-unused-parameter \
           -nostdinc -I$(KFRAMEWORK)/Headers -I$(KFRAMEWORK)/PrivateHeaders \
           -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
           -isysroot $(SDK)

CFLAGS   = -arch ppc -static -O2 -mlong-branch -fno-builtin -fno-common -Wall -nostdinc \
           -I$(KFRAMEWORK)/Headers -DKERNEL -DKERNEL_PRIVATE -isysroot $(SDK)

LDFLAGS  = -arch ppc -static -r -syslibroot $(SDK)
# les bibliothèques doivent venir APRÈS les objets. Pas de -nostdlib : c est un
# flag du driver gcc, que le ld de Tiger rejette (« unknown flag »), vu en vrai.
# libcc_kext.a vit dans le répertoire privé de gcc (/usr/lib/gcc/…/4.0.1), hors
# du SDK : avec -syslibroot, `-lcc_kext` donne « can't locate file » (vu en vrai).
# On demande son chemin au compilateur.
CC_KEXT  := $(shell $(CXX) -arch ppc -print-file-name=libcc_kext.a)
LDLIBS   = -lkmodc++ -lkmod $(CC_KEXT)

all: $(KEXT)

%.o: %.cpp POMPPCQFB.h qfb_regs.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

%_info.o: %_info.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(KEXT): $(BIN) Info.plist
	rm -rf $(KEXT)
	mkdir -p $(KEXT)/Contents/MacOS
	cp Info.plist $(KEXT)/Contents/
	cp $(BIN) $(KEXT)/Contents/MacOS/
	touch $(KEXT)

install: $(KEXT)
	rm -rf /System/Library/Extensions/$(KEXT)
	cp -R $(KEXT) /System/Library/Extensions/
	chown -R root:wheel /System/Library/Extensions/$(KEXT)
	chmod -R 755 /System/Library/Extensions/$(KEXT)
	touch /System/Library/Extensions
	kextcache -e -v 1

load: $(KEXT)
	chown -R root:wheel $(KEXT)
	chmod -R 755 $(KEXT)
	kextload -t $(KEXT)

unload:
	kextunload -b net.pomppc.POMPPCQFB

clean:
	rm -rf $(OBJS) $(BIN) $(KEXT)

.PHONY: all install load unload clean
