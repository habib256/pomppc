# Makefile — plugin OpenGL POMPPC (GLDriverPOMPPC.bundle), à compiler DANS
# l'invité Tiger (gcc 4.0, SDK 10.4u). gld_tramp.s est généré sur l'hôte par
# tools/gld/gen_tramp.py et livré tel quel.
SDK     = /Developer/SDKs/MacOSX10.4u.sdk
CC      = /usr/bin/gcc-4.0
CFLAGS  = -arch ppc -isysroot $(SDK) -O2 -Wall -Wno-unused-parameter -fno-common
LDFLAGS = -arch ppc -isysroot $(SDK) -bundle
LIBS    = -framework IOKit -framework CoreFoundation

# Le nom est celui que le kext publie dans IOGLBundleName (POMPPC_GL_BUNDLE_NAME,
# kext/POMPPCGPU/POMPPCGPU.h) : GLEngine charge alors
#   /System/Library/Extensions/GLDriver-POMPPC.bundle/Contents/MacOS/GLDriver-POMPPC
# AVANT les GLDriver* d'OpenGL.framework, comme un vrai pilote de carte
# (docs/re/accelerateur-iokit.md). Le tiret, qui faisait trier ce bundle avant
# « GLDriver » quand il vivait dans Resources, ne sert plus.
NAME    = GLDriver-POMPPC
BIN     = $(NAME).bundle/Contents/MacOS/$(NAME)
OBJS    = pomppc_gld.o pomppc_accel.o pomppc_qgpu.o gld_tramp.o

all: $(BIN) $(NAME).bundle/Contents/Info.plist

# qgpu_proto.h : copie du contrat hôte/invité (kext/POMPPCGPU/qgpu_proto.h)
%.o: %.c pomppc_gld.h pomppc_qgpu.h qgpu_proto.h
	$(CC) $(CFLAGS) -c $< -o $@

# Dans l'arborescence du dépôt, on la reprend du kext ; ailleurs (CD, job de
# développement), elle doit déjà être présente à côté des sources.
PROTO_SRC = $(wildcard ../../kext/POMPPCGPU/qgpu_proto.h)
ifneq ($(PROTO_SRC),)
qgpu_proto.h: $(PROTO_SRC)
	cp $< $@
endif

gld_tramp.o: gld_tramp.s
	$(CC) -arch ppc -c $< -o $@

# Disposition standard d'un bundle, celle qu'impose IOGLBundleName (et non plus
# la disposition « plate » des GLDriver* de Resources).
$(BIN): $(OBJS)
	mkdir -p $(NAME).bundle/Contents/MacOS
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(NAME).bundle/Contents/Info.plist: Info.plist
	mkdir -p $(NAME).bundle/Contents
	cp Info.plist $@

clean:
	rm -rf *.o $(NAME).bundle

.PHONY: all clean
