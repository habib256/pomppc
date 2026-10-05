#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -e
gcc -O2 -Wall -o jctest jctest.c > out/build.txt 2>&1
./jctest > out/jctest.txt 2>&1
cat out/jctest.txt
