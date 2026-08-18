# Makefile — construction de POMPPCQFB.kext DANS l'invité Tiger (PowerPC).
#
#   Prérequis : Xcode 2.5 (gcc 4.0.1 + SDK MacOSX10.4u) installé dans l'invité.
#   Usage     : make          → POMPPCQFB.kext
#               sudo make install
#               sudo make load
#
# Le compilateur DOIT être gcc 4.0 : l'ABI C++ du noyau 10.4 est celle de
# gcc 4.0. Un kext C++ compilé par un autre compilateur charge puis échoue de
# façon obscure (start() jamais appelé, ou panic au premier appel virtuel).

SDK        = /Developer/SDKs/MacOSX10.4u.sdk
KFRAMEWORK = $(SDK)/System/Library/Frameworks/Kernel.framework
CXX        = /usr/bin/g++-4.0
LD         = /usr/bin/ld

KEXT       = POMPPCQFB.kext
BIN        = POMPPCQFB
OBJS       = POMPPCQFB.o

CXXFLAGS = -arch ppc -static -O2 \
           -fno-builtin -fno-exceptions -fno-rtti -fno-common -fapple-kext \
           -msoft-float -fno-strict-aliasing -Wall -Wno-unused-parameter \
           -nostdinc -I$(KFRAMEWORK)/Headers -I$(KFRAMEWORK)/PrivateHeaders \
           -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
           -isysroot $(SDK)

LDFLAGS  = -arch ppc -static -r -nostdlib -syslibroot $(SDK)
# les bibliothèques doivent venir APRÈS les objets
LDLIBS   = -lkmodc++ -lkmod -lcc_kext

all: $(KEXT)

%.o: %.cpp POMPPCQFB.h qfb_regs.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

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
