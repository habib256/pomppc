# Makefile — programme de test invité qgpu_test, à compiler DANS l'invité Tiger
# (gcc 4.0 + SDK 10.4u, Xcode Tools du DVD Tiger).
CC      = /usr/bin/gcc-4.0
CFLAGS  = -arch ppc -O1 -Wall
LDLIBS  = -framework IOKit -framework CoreFoundation

# pomppc_qgpu.h : uniquement pour les macros POMPPC_SUB_* (drapeaux v9 posés
# dans `len` de QGPU_UC_SUBMIT), copie identique de celles de POMPPCGPU.h.
qgpu_test: qgpu_test.c ../../kext/POMPPCGPU/qgpu_proto.h ../gldriver/pomppc_qgpu.h
	$(CC) $(CFLAGS) -o $@ qgpu_test.c $(LDLIBS)

clean:
	rm -f qgpu_test out.ppm

.PHONY: clean
