#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# retvboot.sh JOURNAL SECONDES — démarre la VM de dev SANS devloop (disque en
# -snapshot, rien n'est écrit), options de production du PC + x-ret-verify,
# et l'arrête (SIGTERM : les bilans atexit sont imprimés) au bout de SECONDES.
# Sert au cas « sans traduction » de x-ret-verify (docs/tcg-g4.md §35) : les
# quatre cas tombent dans OpenBIOS/BootX entre 10 et 25 s après le lancement.
#   RAM=768 SMP=2 TLBP=on WATCH=0x5605d24 QBIN=~/src/qemu-retv/build \
#     DEVDISK=disks/tiger-dev-retv.raw tools/tcg/retvboot.sh /tmp/w.log 90
# Variables : SMP (1|2), RAM (Mo, défaut 768 : les cas en dépendent, 0 à 1024),
# TLBP (on|off : x-tlb-precise), EXTRA_CPU (propriétés en plus), WATCH (pc
# surveillé, binaire avec patches/tcg/essais/0031-retv-diag.patch),
# QBIN (dossier build), DEVDISK (copie du disque de dev, jamais l'original).
set -u
LOG=$1; SECS=$2
R="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QB=${QBIN:-$HOME/src/qemu-retv/build}
DISK=${DEVDISK:?DEVDISK=copie du disque de dev requise}
SMP=${SMP:-2}
BIN=$QB/qemu-system-ppc; [ "$SMP" -gt 1 ] && BIN=${BIN}64
ACC="tcg,x-jit-near=on,x-jit-rel32=on,x-jc-bits=14,x-jc-word=on"
[ "$SMP" -gt 1 ] && ACC="$ACC,thread=multi"
CPU="g4,x-fast-fp=on,x-sr-tlb=on,x-lfs-inline=on,x-vfp-fast=on,x-vperm-fast=on,x-vmx-inline=on,x-vfp-native=on,x-lmw-vector=on,x-fp-inline=on,x-fp-native=on,x-fp-native64=on,x-fp-native-cmp=on,x-vfp-native-cmp=on,x-tb-fast=on,x-ret-inline=on,x-jc-idx=on,x-ret-verify=on,x-icbi-sync=on,x-msr-nobql=on,x-tlb-precise=${TLBP:-on},x-lmw-inline=on,x-dcbz-inline=on${EXTRA_CPU:+,$EXTRA_CPU}"
ARGS=(-k fr -M mac99,via=pmu -cpu "$CPU" -m "${RAM:-768}" -smp "$SMP" -accel "$ACC"
  -display none -bios "$R/patches/smp-mac99/openbios-smp-screamer.elf" -g 1024x768x32
  -drive "file=$DISK,format=raw,media=disk" -snapshot
  -device usb-tablet,x-abs-margin=15 -nic none -device qgpu-pci,id=gpu0,backend=auto
  -prom-env "auto-boot?=true" -prom-env "boot-device=hd:10,\\System\\Library\\CoreServices\\BootX"
  -prom-env "boot-args=-v -s")
echo "# $(date -Iseconds) SMP=$SMP RAM=${RAM:-768} TLBP=${TLBP:-on} WATCH=${WATCH:-} EXTRA=${EXTRA_CPU:-} $BIN" > "$LOG"
POMPPC_RV_WATCH=${WATCH:-} timeout -s TERM -k 15 "$SECS" "$BIN" "${ARGS[@]}" >> "$LOG" 2>&1
echo "# fin rc=$?" >> "$LOG"
