# Makefile — programme de test invité qgpu_test, à compiler DANS l'invité Tiger
# (gcc 4.0 + SDK 10.4u, Xcode Tools du DVD Tiger).
CC      = /usr/bin/gcc-4.0
CFLAGS  = -arch ppc -O1 -Wall
LDLIBS  = -framework IOKit -framework CoreFoundation

# Le contrat (qgpu_proto.h, qui inclut l'ABI qgpu_abi.h) vient de patches/qgpu/.
all: qgpu_test errpeer unloadpeer

qgpu_test: qgpu_test.c ../../patches/qgpu/qgpu_proto.h ../../patches/qgpu/qgpu_abi.h
	$(CC) $(CFLAGS) -I../../patches/qgpu -o $@ qgpu_test.c $(LDLIBS)

# 07/10/2026 : client fautif de l'épreuve des erreurs par client (jobs/regerr)
errpeer: errpeer.c ../../patches/qgpu/qgpu_proto.h ../../patches/qgpu/qgpu_abi.h
	$(CC) $(CFLAGS) -I../../patches/qgpu -o $@ errpeer.c $(LDLIBS)

# 07/10/2026 : client tenu ouvert pendant un kextunload (K4, KG4, KT1)
unloadpeer: unloadpeer.c ../../patches/qgpu/qgpu_proto.h ../../patches/qgpu/qgpu_abi.h
	$(CC) $(CFLAGS) -I../../patches/qgpu -o $@ unloadpeer.c $(LDLIBS) -lpthread

clean:
	rm -f qgpu_test errpeer unloadpeer out.ppm

.PHONY: all clean
