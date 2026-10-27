# patches/diag — patches de MESURE, jamais posés par `build_qemu_qfb.sh`

Ces patches servent à mesurer dans un arbre à part (`~/src/qemu-ceil`). Ils ne vont jamais dans le binaire de référence, et certains sont faux par construction.

    rsync -a --exclude='build*' --exclude='pgo-*' ~/src/qemu/ ~/src/qemu-ceil/
    (cd ~/src/qemu-ceil && patch -p1 < ~/src/pomppc/patches/diag/0001-mesure-selfprof-tlb-fpkceil.patch)
    QEMU_SRC=$HOME/src/qemu-ceil ./scripts/build_qemu_qfb.sh

`0001-mesure-selfprof-tlb-fpkceil.patch` (27/10, `docs/vitesse-doom3-x86.md` §18) apporte trois choses :

- **Auto-profileur, `POMPPC_SELFPROF=PRÉFIXE`.**
  - Chaque fil vCPU (MTTCG) prend 1 000 échantillons par seconde de son temps CPU (`SIGPROF`) et note le `rip` interrompu.
  - Il n'enregistre que tant que `PRÉFIXE.on` existe. Les échantillons sont écrits dans `PRÉFIXE.<tid>.bin` quand ce fichier disparaît.
  - Il n'a pas besoin de `perf`, ce qui compte quand `perf_event_paranoid` vaut 4.
  - `matrice.py --sample-hote S` l'utilise de lui-même quand la variable est définie. L'analyse se fait avec `tools/tcg/selfprof.py`.
- **`POMPPC_FPK_CEIL=1|2` : mesure de plafond, FAUSSE par construction.** Tous les opérandes de `x-fp-known` sont tenus pour connus ; avec `2`, la porte FPSCR aussi.
- **`POMPPC_TLB_LOG=1` et `POMPPC_TLB_HUGE=1`.**
  - `POMPPC_TLB_LOG=1` journalise les tailles de la table du TLB rapide.
  - `POMPPC_TLB_HUGE=1` aligne sur 2 Mio et marque `MADV_HUGEPAGE` les tables de 2 Mio et plus.
