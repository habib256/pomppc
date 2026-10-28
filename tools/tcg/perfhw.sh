#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 VERHILLE Arnaud
# perfhw.sh — profil aux compteurs matériels du QEMU en jeu (Intel, perf_event_paranoid ≤ 1).
# Lancé par la matrice pendant la fenêtre :
#   MATRICE_OPTS="--sample-hote 45" POMPPC_PERF_CMD=tools/tcg/perfhw.sh \
#       tools/tcg/matab.sh x86-hw-2810 1 "fast:QEMU_FAST=1 EXTRA_ARGS=-perfmap" d3 fen
# PID, OUT, S viennent de l'environnement. Un tiers de S : perf stat (TMA niveaux 1-3,
# ratés de cache) ; deux tiers : perf record d'événements précis (l'ip est celle de
# l'instruction fautive, PEBS), nommés par /tmp/perf-$PID.map.
set -u
s1=$(( S / 3 )); s2=$(( S - s1 ))
perf stat -p "$PID" -M TopdownL1,TopdownL2,TopdownL3 -o "$OUT/perfhw-tma.txt" -- sleep "$s1" 2>/dev/null
perf stat -p "$PID" -o "$OUT/perfhw-stat.txt" -e cycles,instructions,\
mem_inst_retired.all_loads,mem_inst_retired.all_stores,mem_load_retired.l1_miss,\
mem_load_retired.l2_miss,mem_load_retired.l3_miss,mem_load_retired.fb_hit,\
cycle_activity.stalls_l1d_miss,cycle_activity.stalls_l2_miss,cycle_activity.stalls_l3_miss,\
cycle_activity.stalls_mem_any,cycle_activity.stalls_total,dtlb_load_misses.walk_completed,\
br_misp_retired.all_branches,br_inst_retired.all_branches,frontend_retired.l1i_miss,\
frontend_retired.itlb_miss -- sleep "$s1" 2>/dev/null
perf record -p "$PID" -o "$OUT/perfhw.data" -e cpu/event=cycles,period=200003/pp \
    -e cpu/mem_load_retired.l1_miss,period=20011/pp -e cpu/mem_load_retired.l2_miss,period=5003/pp \
    -e cpu/br_misp_retired.all_branches,period=5003/pp -e cpu/mem_inst_retired.all_loads,period=200003/pp \
    -- sleep "$(( s2 - s1 ))" >/dev/null 2>&1
perf report -i "$OUT/perfhw.data" --stdio --no-children --sort dso,sym \
    --percent-limit 0.3 > "$OUT/perfhw-report.txt" 2>/dev/null
true
