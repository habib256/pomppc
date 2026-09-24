# Makefile — programme de test invité qgpu_test, à compiler DANS l'invité Tiger
# (gcc 4.0 + SDK 10.4u, Xcode Tools du DVD Tiger).
CC      = /usr/bin/gcc-4.0
CFLAGS  = -arch ppc -O1 -Wall
LDLIBS  = -framework IOKit -framework CoreFoundation

# Le contrat (qgpu_proto.h, qui inclut l'ABI qgpu_abi.h) vient de patches/qgpu/.
qgpu_test: qgpu_test.c ../../patches/qgpu/qgpu_proto.h ../../patches/qgpu/qgpu_abi.h
	$(CC) $(CFLAGS) -I../../patches/qgpu -o $@ qgpu_test.c $(LDLIBS)

clean:
	rm -f qgpu_test out.ppm

.PHONY: clean
