# Makefile — fpcheck / fpbench, compilés DANS l'invité Tiger 10.4.6 (PowerPC).
#
#   make            les deux outils
#   make check      fpcheck seul
#   make bench      fpbench seul
#
# gcc 4.0 des Xcode Tools + SDK 10.4u (voir tools/guest/jobs/xcode).
CC      ?= /usr/bin/gcc-4.0
SDK     ?= /Developer/SDKs/MacOSX10.4u.sdk
ARCH    ?= -arch ppc -mcpu=7400
# -fno-strict-aliasing : on transtype les bits des flottants par des unions.
CFLAGS  ?= $(ARCH) -O2 -isysroot $(SDK) -fno-strict-aliasing -Wall
ALTIVEC ?= -faltivec

all: fpcheck fpbench

check: fpcheck
bench: fpbench

fpcheck: fpcheck.c
	$(CC) $(CFLAGS) $(ALTIVEC) -o $@ fpcheck.c

fpbench: fpbench.c
	$(CC) $(CFLAGS) $(ALTIVEC) -o $@ fpbench.c -lm

# repli sans AltiVec, si -faltivec manque à la chaîne de compilation
noav:
	$(CC) $(CFLAGS) -o fpcheck fpcheck.c
	$(CC) $(CFLAGS) -o fpbench fpbench.c -lm

clean:
	rm -f fpcheck fpbench

.PHONY: all check bench noav clean
