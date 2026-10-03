#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# vmxtest - equivalence vsldoi/vmrghw/vmrglw/stve[bhw]x : helpers d'origine <->
# traduction en ligne (patches/tcg/0021-ppc-vmx-inline, docs/tcg-g4.md section 28).
#   mkdir /tmp/j && cp tools/guest/jobs/vmxtest/* /tmp/j && devloop.py run /tmp/j
# env.sh a cote (facultatif) : NVEC=n couples aleatoires (defaut 2^20),
# BANC=N (banc de N x 4 instructions dependantes).
# Sortie : out/vmxtest.txt ; comparer les deux modes par `diff` (hors ligne « banc »).
[ -f ./env.sh ] && . ./env.sh
export NVEC
OUT=$PWD/out
gcc -O2 -faltivec -Wall -o vmxtest vmxtest.c > $OUT/build.txt 2>&1 || { cat $OUT/build.txt; exit 1; }
./vmxtest $BANC > $OUT/vmxtest.txt 2> $OUT/vmxtest.err
rc=$?
cat $OUT/vmxtest.err $OUT/vmxtest.txt
exit $rc
