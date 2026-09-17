# Makefile — plugin OpenGL POMPPC (GLDriverPOMPPC.bundle), à compiler DANS
# l'invité Tiger (gcc 4.0, SDK 10.4u). gld_tramp.s est généré sur l'hôte par
# tools/gld/gen_tramp.py et livré tel quel.
SDK     = /Developer/SDKs/MacOSX10.4u.sdk
CC      = /usr/bin/gcc-4.0
CFLAGS  = -arch ppc -isysroot $(SDK) -O2 -Wall -Wno-unused-parameter -fno-common
LDFLAGS = -arch ppc -isysroot $(SDK) -bundle
LIBS    = -framework IOKit -framework CoreFoundation

# Le tiret est voulu : GLEngine charge les « GLDriver* » dans l'ordre du
# dossier, et CGL retient par défaut le premier renderer qui convient.
# « GLDriver-POMPPC » trie avant « GLDriver » (« - » < « . ») : sans demande
# explicite, les applications reçoivent le renderer accéléré (vu en vrai).
NAME    = GLDriver-POMPPC
OBJS    = pomppc_gld.o pomppc_accel.o pomppc_qgpu.o gld_tramp.o

all: $(NAME).bundle/$(NAME)

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

# Disposition « plate » attendue par GLEngine dans GL_RESOURCES :
#   <dossier>/GLDriver-POMPPC.bundle/GLDriver-POMPPC
$(NAME).bundle/$(NAME): $(OBJS)
	mkdir -p $(NAME).bundle
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

# Dossier GL_RESOURCES de test : notre plugin + le rendu flottant d'Apple.
glres: all
	rm -rf glres && mkdir glres
	cp -R $(NAME).bundle glres/
	ln -s /System/Library/Frameworks/OpenGL.framework/Versions/A/Resources/GLRendererFloat.bundle glres/

clean:
	rm -rf *.o $(NAME).bundle glres

.PHONY: all glres clean
