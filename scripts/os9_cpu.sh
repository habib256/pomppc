#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# Profils expérimentaux cumulatifs OS 9. Sondage strict : aucun A/B partiel.
OS9_PROFILE="${OS9_PROFILE:-reference}"
CPU_SPEC=g4
TCG_ACCEL=tcg
_os9_mem="x-sr-tlb x-tlb-precise x-lmw-inline x-dcbz-inline x-dirty-skip x-pteg-map x-tlb-base"
_os9_fp="x-fast-fp x-lfs-inline x-vfp-fast x-vperm-fast x-vmx-inline x-vfp-native x-lmw-vector x-fp-inline x-fp-native x-fp-native64 x-fp-native-cmp x-fp-known x-fprf-lazy x-stfs-known x-lfs-known x-vfp-native-cmp"
_os9_ctl="x-ret-inline x-jc-idx x-icbi-sync x-msr-nobql x-tb-fast"
_os9_verify="x-lmw-inline-verify x-dcbz-inline-verify x-dirty-skip-verify x-pteg-map-verify x-tlb-base-verify x-fp-verify x-fp-native-cmp-verify x-vmx-verify x-vfp-native-verify x-vfp-native-cmp-verify x-ret-verify x-msr-nobql-verify x-tb-verify"
case "$OS9_PROFILE" in
  reference) _os9_props="" ;;
  memoire) _os9_props="$_os9_mem" ;;
  flottant) _os9_props="$_os9_mem $_os9_fp" ;;
  complet) _os9_props="$_os9_mem $_os9_fp $_os9_ctl" ;;
  verifie) _os9_props="$_os9_mem $_os9_fp $_os9_ctl $_os9_verify" ;;
  *) echo "OS9_PROFILE attendu : reference, memoire, flottant, complet ou verifie" >&2; exit 2 ;;
esac
for _os9_prop in $_os9_props; do
  if ! qemu_cpu_has_prop "$QEMU_BIN" "$MACHINE" g4 "$_os9_prop=on"; then
    echo "OS9_PROFILE=$OS9_PROFILE : propriété absente $_os9_prop ($QEMU_BIN)" >&2
    exit 2
  fi
  CPU_SPEC="$CPU_SPEC,$_os9_prop=on"
done
if [ "$OS9_PROFILE" = verifie ]; then
  for _os9_prop in x-sr-tlb-verify=64 x-tlb-precise-verify=64; do
    qemu_cpu_has_prop "$QEMU_BIN" "$MACHINE" g4 "$_os9_prop" || exit 2
    CPU_SPEC="$CPU_SPEC,$_os9_prop"
  done
fi
case "$OS9_PROFILE" in
  complet|verifie)
    for _os9_prop in x-jit-near=on x-jc-bits=14 x-jc-word=on x-jc-2way=on; do
      if ! qemu_tcg_has_prop "$QEMU_BIN" "$MACHINE" "$_os9_prop"; then
        echo "OS9_PROFILE=$OS9_PROFILE : propriété TCG absente $_os9_prop" >&2
        exit 2
      fi
      TCG_ACCEL="$TCG_ACCEL,$_os9_prop"
    done ;;
esac
