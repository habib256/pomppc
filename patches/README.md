# patches/ — ce qui est appliqué, ce qui est là pour référence

Deux natures de fichiers cohabitent ici, et la distinction compte : **ce que
`scripts/build_qemu_qfb.sh` applique réellement**, et **le matériau d'origine** (mails de
liste, essais revertés, binaires supplantés) gardé pour pouvoir refaire le raisonnement.

## Base : QEMU 11.1.2 (portage du 01/10/2026)

Toute la série vise **QEMU v11.1.2** (elle visait 9.2.0 jusqu'au 01/10/2026 ; l'historique git
garde l'ancienne). Chaque patch a été reposé à la main sur 11.1.2 dans un arbre git, un commit
par patch, puis régénéré ici par `git diff` : il s'applique sans fuzz. Ce que l'amont a changé
entre-temps, et que les patches suivent :

- **TCG** : recherche de TB par `TCGTBCPUState` et `tcg_ops->get_tb_cpu_state` (0008) ;
  `accel/tcg/translator.c` est du code commun (plus d'`ArchCPU`, `TARGET_PAGE_BITS` est une
  valeur d'exécution : les champs du CPU s'atteignent par `offsetof(CPUState, x) -
  sizeof(CPUState)` depuis `tcg_env`) ; `NB_MMU_MODES` vaut 22 et les masques sont des
  `MMUIdxMap` 32 bits (cache de sauts de 0008/0011 élargi en conséquence) ; ops sans type et
  backends par tables `outop_*` : `ppc_fp32` (0014) est un op `TCG_TYPE_I64` sans `outop`,
  contraint par `tcg_target_op_def` et émis par un `case` de `tcg_reg_alloc_op`, et la cible
  demande `tcg_ppc_fp32_supported()` au lieu de lire `TCG_TARGET_HAS_*` ; dans
  `tcg_optimize`, la boucle n'appelle plus `finish_folding` pour un `case` resté `!done`
  (seul le `default` le fait) : le `case` de `ppc_fp32` l'appelle lui-même, sans quoi le
  temporaire de sortie gardait ses listes de copies, qui se corrompaient, et l'optimiseur
  bouclait sans fin (démarrage de Tiger gelé après « BSD root », moniteur muet, 01/10/2026) ; `exec-all.h`
  supprimé (déclarations dans `accel/tcg/internal-common.h` et
  `include/exec/translation-block.h`) ; `tb_invalidate_phys_range` prend le CPU ;
  `cpu_physical_memory_*` → `physical_memory_*`, `xlat_section` → `xlat_offset`.
- **ppc** : instructions flottantes et `fcmpu` en decodetree (`trans_FCMPU`, `helper_FCMPU`) ;
  `helper_store_msr`/`do_rfi` dans `tcg-excp_helper.c` ; propriétés du CPU dans
  `powerpc_cpu_properties` (`const`).
- **softfloat** commun à toutes les cibles : plus de veto `TARGET_PPC` sur le hardfloat ;
  `float_status` en champs de bits, d'où `PPC_FP_FLAGS_OFS` (les 16 bits de
  `float_exception_flags` en tête de structure, vérifiés au `realize` du CPU) ;
  `float_muladd_halve_result` remplacé par `scalbn`.
- **Devices** : en-têtes sous `hw/core/` et `system/`, audio par `AudioBackend`
  (`audio_be_*`, `qemu/audio.h`), console par `qemu_console_*` et
  `qemu_graphic_console_create` (`gfx_update` rend un `bool`), listes de propriétés `const` sans
  `DEFINE_PROP_END_OF_LIST`, `class_init(ObjectClass *, const void *)`. GPIO de macio : l'amont a
  ses noms de bits (`enum MacioGPIORegisterBits`) ; le patch SMP n'ajoute que
  `GPIO_RESET_CPU1`, `macio_gpio_set_extirq()` et les fronts de la ligne de reset du CPU 1.

## Appliqué par `scripts/build_qemu_qfb.sh`

| Fichier | Rôle |
| --- | --- |
| `smp-mac99/qemu-mac99-cpus-v2.patch` | SMP mac99. Remplace le garde-fou `« Only UP supported today »` d'`hw/intc/openpic.c` par la vraie limite du modèle (`nb_cpus > KEYLARGO_MAX_CPU`), ajoute le GPIO 4 de KeyLargo (0x5C, `KL_GPIO_RESET_CPU1`, active basse) dans `hw/misc/macio/gpio.c`, et dans `hw/ppc/mac_newworld.c` le `PIR = cpu_index` plus `cpu_reset_line()` : ligne tenue basse = CPU 1 remis à zéro et arrêté, relâchement = démarrage, par `async_run_on_cpu()`. S'applique **sans fuzz ni décalage** sur QEMU 11.1.2 pristine (le build l'exige : `--fuzz=0`). |
| `openpic/0001-openpic-reset-niveau.patch` | **Reset de l'OpenPIC** (29/09/2026) : `openpic_reset` remettait les sources en front sans oublier `pending` ; une ligne de niveau haute au reset (OHCI pendant une panique de l'invité) restait en attente pour toujours, et le démarrage suivant se bloquait dans une interruption perpétuelle (« using 1966 buffer headers »). Chaque source retient le niveau brut de sa ligne (`input`), le reset remet `pending` à 0, l'écriture de l'IVPR d'une source de niveau reprend l'état réel de la ligne. Touche `hw/intc/openpic.c`, `include/hw/ppc/openpic.h`. Posé après la série SMP (même fichier). `docs/gel-doom3-baddisplay.md` §3. |
| `qfb/qfb-pci.c` | Le device paravirtuel `qfb-pci`, copié dans `hw/display/`. Protocole « qfb1 » de Solra Bizna porté du NuBus vers PCI. |
| `qfb/0002-wire-qfb-pci-build.patch` | Câblage meson/Kconfig du device ci-dessus. |
| `qgpu/qgpu-pci.c`, `qgpu-core.[ch]`, `qgpu-soft.c`, `qgpu-gl.c`, `qgpu_proto.h` | Le GPU paravirtuel `qgpu-pci` (protocole v16 : géométrie brute, tampons hôte, programmes ARB) : device (transport), cœur d'exécution du flux de commandes (contextes, surfaces, textures, programmes, état GL), backend logiciel de référence, **backend OpenGL** (CGL/EGL, rendu sur le GPU hôte), et le contrat hôte/invité. Copiés dans `hw/display/`. |
| `qgpu/0003-wire-qgpu-pci-build.patch` | Câblage meson/Kconfig de `qgpu-pci` ; lie `OpenGL.framework` (macOS) ou EGL+GL (Linux) si présents, sinon le backend GL est un stub. |
| `screamer/screamer.c` + `screamer/screamer.h` | Le device audio **Screamer** (AWACS PowerMac), copiés dans `hw/audio/` et `include/hw/audio/`. |
| `screamer/0001-wire-screamer-build.patch` | Câblage du Screamer : `hw/audio/Kconfig`, `hw/audio/meson.build`, `hw/ppc/Kconfig`, et surtout l'instanciation + les IRQ/DBDMA dans `hw/misc/macio/macio.c`. |
| `fastfp/0001-ppc-fast-fp.patch` | **Flottant rapide** : propriété de CPU `x-fast-fp` (défaut *off*) qui laisse softfloat confier les opérations flottantes au FPU de l'hôte. Touche `fpu/softfloat.c`, `include/fpu/softfloat-types.h`, `target/ppc/{cpu.h,cpu_init.c,fpu_helper.c}`. Voir plus bas et `docs/flottant-rapide.md`. |
| `fastfp/0002-ppc-fewer-fp-helpers.patch` | 4 appels de helper par instruction flottante → 2 (`reset_fpstatus` émis en ligne, `compute_fprf` + `float_check_status` fusionnés). **Aucun effet observable**, dans aucun des deux modes. S'applique par-dessus le 0001 ; `NO_FASTFP2=1` sur un arbre propre applique le 0001 seul. |
| `tcg/0001-ppc-sr-tlb.patch` | **TLB gardé d'un jeu de segments à l'autre** : propriété de CPU `x-sr-tlb` (défaut *off*, `SRTLB=1 ./run_tiger.sh`). Un changement de registre de segment ne vide plus tout le TLB de QEMU (~23 000 fois par seconde sous Tiger) : chaque `mmu_idx` traduit retient le jeu de segments de ses entrées et n'est vidé que s'il sert sous un autre ; `tlbie` devient global en SMP. Mode preuve `x-sr-tlb-verify=N`. Touche `target/ppc/{cpu.h,cpu_init.c,helper_regs.[ch],mmu_helper.c}`. Marble Blast SMP=2 : **+9,8 %** img/s. Voir `docs/tcg-g4.md`. `NO_TCG=1` le saute. |
| `tcg/0002-ppc-lfs-inline.patch` | **`lfs`/`stfs` sans helper** : propriété de CPU `x-lfs-inline` (défaut *off*, `LFSINLINE=1 ./run_tiger.sh`). Les conversions simple ↔ double de `lfs`/`stfs` (et formes `x`/`u`/`ux`) en 13 ops TCG entières sans branchement au lieu de `helper_todouble`/`helper_tosingle` (5,1 % du temps vCPU sur DOOM 3). Preuve hôte exhaustive (`tools/tcg/lfsproof.sh` : 2^32 float32, 2^35 float64, 0 divergence) et invitée (`tools/guest/jobs/lfstest`, 2^32 + 2^32 cas, sortie identique à l'octet). Banc invité −45 %. Touche `target/ppc/{cpu.h,cpu_init.c,translate.c,translate/fp-impl.c.inc}`. S'applique par-dessus `tcg/0001` ; `NO_TCG=1` le saute aussi. Voir `docs/tcg-g4.md` §8. |
| `tcg/0003-ppc-vfp-fast.patch` | **Flottant AltiVec à 4 voies** : propriété `x-vfp-fast` (défaut *off*, `VFPFAST=1`). `vaddfp`/`vsubfp`/`vmaddfp`/`vnmsubfp` font leurs 4 voies d'un coup sur le FPU hôte quand le hardfloat de softfloat les aurait toutes prises (même décision par voie, sinon la boucle d'origine) : résultats et drapeaux identiques par construction. Preuve contre le vrai `fpu_softfloat.c.o` (`tools/tcg/vfpproof.sh` : 648 M vecteurs, 8 états, 0 divergence ; 5 mutations détectées) ; `vfptest` invité identique à l'octet ; banc −17 %. Touche `target/ppc/{cpu.h,cpu_init.c,int_helper.c}`. §9. |
| `tcg/0004-ppc-vperm-fast.patch` | **`vperm` par table** : propriété `x-vperm-fast` (défaut *off*, `VPERMFAST=1`), traduite vers `helper_VPERM_FAST` (un `tbl` NEON sur arm64, C portable ailleurs). Preuve `tools/tcg/vpermproof.sh` (50,7 M cas, recouvrements compris, 0 divergence) ; banc −33 %. Touche aussi `helper.h`, `translate.c`, `vmx-impl.c.inc`. §10. |
| `tcg/0006-tcg-jit-near.patch` | **Tampon du JIT près du texte de QEMU** : propriétés de l'**accélérateur** `x-jit-near` (défaut *off*, `JITNEAR=1 ./run_tiger.sh`) et `x-jit-addr` (essais). macOS pose le tampon de 1 Gio hors de la fenêtre de 4 Gio du texte un lancement sur deux environ ; sur Apple M4 chaque appel de helper est alors plus lent et tout le processus perd ~6 % (Marble Blast) : c'étaient « les deux régimes ». `x-jit-near` garde le tampon dans la fenêtre (en le réduisant au besoin, 512 Mio au moins). Imprime toujours `tcg: tampon JIT … même/AUTRE fenêtre`. Avec `split-wx` (éteint par défaut), le code exécuté est l'alias RX de `mach_vm_remap` : la vue RW est laissée au noyau et c'est l'alias RX qui est cherché dans la fenêtre du texte, avec la même réduction (bug hunt 4) ; sa place est imprimée à part (`tcg: split-wx, alias RX exécuté … (x-jit-near sur l'alias)`). Touche `tcg/region.c`, `accel/tcg/tcg-all.c`, `include/tcg/startup.h`. `docs/tcg-g4.md` §14. |
| `tcg/0007-ppc-fp-inline.patch` | **Flottant scalaire simple sans ses helpers** : propriété de CPU `x-fp-inline` (défaut *off*, `FPINLINE=1 ./run_tiger.sh` ; n'agit qu'avec `x-fast-fp`), mode preuve `x-fp-verify` (`FPVERIFY=1`). `fadds fsubs fmuls fmadds fmsubs fnmadds fnmsubs` : quand le FPSCR est amorcé sans trappe (XX, pas XE/OE/UE), RN = 00, et que les opérandes sont des float32 nuls ou normaux, un seul appel **pur** (`helper_fp32_fast`, l'op float32 sur le FPU hôte, `fmaf` pour les FMA) et FPRF/FI en ligne, au lieu de deux helpers sans drapeau ; `fcmpu` entièrement en ligne (comparaison entière). Sinon la séquence d'origine, inchangée (un branchement). Preuve hôte contre les vrais objets de l'arbre (`tools/tcg/fpproof.sh` : 174 M vecteurs, 0 divergence ; 10 mutations sur 10 détectées, `fpproof-mut.sh`), invitée (`tools/guest/jobs/fptest`, 30 M instructions dans 11 états du FPSCR, empreinte identique) et vérificateur en jeu (Marble Blast : 3,04 milliards de passages vérifiés, 0 divergence, 99 % par le chemin court). Banc invité −21 à −46 %. Touche `target/ppc/{cpu.h,cpu_init.c,fpu_helper.c,helper.h,internal.h,translate.c,translate/fp-impl.c.inc}`. Par-dessus `tcg/0001-0004` ; `NO_TCG=1` le saute. `docs/tcg-g4.md` §15. |
| `tcg/0008-tcg-ret-inline.patch` | **Sorties indirectes des blocs** : propriétés de CPU `x-ret-inline` et `x-jc-idx` (défaut *off*, `RETINLINE=1 JCIDX=1 ./run_tiger.sh`), mode preuve `x-ret-verify` (`RETVERIFY=1`). `x-ret-inline` : à chaque `blr`, `bctr`, `bclr`/`bcctr`, branchement vers une autre page, la sonde du cache de sauts de `helper_lookup_tb_ptr` est émise dans le code généré (mêmes comparaisons : pc, hflags relus, cflags, points d'arrêt), le helper n'est appelé que sur un raté. `x-jc-idx` : un vidage du TLB ne jette que les entrées du cache de sauts des `mmu_idx` vidés (rien si aucun n'était sale), en ne visitant que les emplacements posés depuis le dernier vidage de ce `mmu_idx`. Preuve : `x-ret-verify` compare chaque bloc pris dans le cache à une recherche physique complète (plus de 25 milliards de blocs, 0 divergence, SMP=2 et SMP=1) ; code modifié/remappé dans l'invité (`tools/guest/jobs/smctest`, empreintes identiques). Touche `accel/tcg/{cpu-exec.c,cputlb.c,tb-jmp-cache.h,tcg-runtime.h,translate-all.c,translator.c}`, `include/exec/{exec-all.h,tb-flush.h,translator.h}`, `include/hw/core/cpu.h`, `include/tcg/tcg-op-common.h`, `tcg/tcg-op.c`, `target/ppc/{cpu.h,cpu_init.c,translate.c}`. Par-dessus `tcg/0007` ; `NO_TCG=1` le saute. `docs/tcg-g4.md` §16. |
| `tcg/0010-tcg-smc-mttcg.patch` | **Code réécrit par l'autre vCPU (MTTCG)** : trois courses de QEMU 9.2 corrigées **sans condition** — la page de code est protégée quand le traducteur en prend le verrou, avant de lire le code (et non plus au lien du bloc) ; `tlb_set_dirty` et `tlb_set_page_full` testent la propreté de la page sous le verrou du TLB. Plus la propriété de CPU `x-icbi-sync` (défaut *off* dans QEMU, **allumée par `run_tiger.sh`**, `ICBISYNC=0` l'éteint) : `icbi` invalide les blocs qui recouvrent sa ligne (une écriture invalide avant d'être faite ; l'autre vCPU pouvait retraduire l'ancien code entre les deux). Preuve : `smctest` E, référence 5/5 exécutions en erreur (86 M appels périmés) → 0 erreur sur 1 105 exécutions (SMP=2) ; A-D, F identiques ; +9 ns par `icbi`. Touche `accel/tcg/{cputlb.c,tb-maint.c}`, `include/exec/translation-block.h`, `target/ppc/{cpu.h,cpu_init.c,mem_helper.c}`. Par-dessus `tcg/0008` ; `NO_TCG=1` le saute. `docs/tcg-g4.md` §17. Outils du diagnostic dans `tcg/essais/0010-smcdbg-diag.patch` (anneau d'événements par page, relevé du bloc périmé) et `0010-smcstat.patch` (compteurs des courses). |
| `tcg/0017-tcg-fp-native-x86.patch`, `0018-ppc-tb-fast-x86.patch`, `0019-ppc-vperm-fast-x86.patch` | **Les accélérations arm64 sur hôte x86-64** (02/10/2026) : émetteur x86_64 de l'op `ppc_fp32` de `x-fp-native`/`x-fp-native64` (VEX et FMA3 sondés à l'exécution, `TCG_TARGET_PPC_FP32_IMPL` pour les `#if` de `tcg.c` ; vérificateur sans `float_flag_input_denormal_used`), `x-tb-fast` par le TSC invariant sous Linux, `vperm` par `pshufb` (AVX). Posés après 0016. Preuves : `fptest` identique à l'octet, `x-fp-verify`/`x-tb-verify` sans divergence, `vpermproof.sh` 50,7 M cas. `docs/tcg-g4.md` §25. |
| `tcg/0020-ppc-vfp-fast-x86.patch` | **`x-vfp-fast` complet sur hôte x86-64** (03/10/2026) : `vmaddfp`/`vnmsubfp` à 4 voies par `vfmadd231ps` (FMA3, sondé à l'exécution ; avant : toujours au logiciel sur x86), `vaddfp`/`vsubfp` par AVX ; mêmes décisions par voie que la version scalaire, sur les motifs binaires. Posé après 0019. Preuves : `vfpproof.sh` 648 M vecteurs sans divergence, `vfpproof-x86-mut.sh` 9/9, `vfptest` identique. `docs/tcg-g4.md` §26. |
| `tcg/0021-ppc-vmx-inline.patch` | **AltiVec en ligne** (03/10/2026, pour DOOM 3) : propriétés de CPU `x-vmx-inline` et `x-vmx-verify` (défaut *off*, `VMXINLINE=1`, `VMXVERIFY=1`). `vsldoi` (deux `extract2`), `vmrghw`/`vmrglw` (quatre ops `i64`) et, en mode grand-boutiste, `stvebx`/`stvehx`/`stvewx` (l'élément choisi par `movcond`/décalage, un `qemu_st` de même taille, même `mmu_idx`, même adresse) traduits en ops TCG au lieu de leurs helpers (`stve*x` : helper sans drapeau). Code commun aux deux hôtes. Preuve hôte `tools/tcg/vmxproof.sh` (helpers extraits tels quels contre le modèle op par op, mutants), invitée `tools/guest/jobs/vmxtest`, vérificateur `x-vmx-verify`. Touche `target/ppc/{cpu.h,cpu_init.c,helper.h,mem_helper.c,translate.c,translate/vmx-impl.c.inc}`. `docs/tcg-g4.md` §28. |
| `tcg/0022-tcg-vfp-native.patch` | **Flottant AltiVec dans le code généré** (03/10/2026) : op TCG nouvelle `INDEX_op_ppc_vfp` (sans opérande TCG : les AVR et `vec_status` vivent dans `env` ; allouée comme un appel qui ne touche aucune globale), propriétés `x-vfp-native` et `x-vfp-native-verify` (défaut *off*, `VFPNATIVE=1`, `VFPNVERIFY=1` ; n'agit qu'avec `x-vfp-fast`). `vaddfp`/`vsubfp`/`vmaddfp`/`vnmsubfp` : le chemin court de `x-vfp-fast` en VEX.128 + FMA3 (x86_64, sondé à l'exécution) ou NEON (aarch64, **compilé et prouvé sur M4 le 05/10**, voir `docs/jit-m4-2026-10-05.md`), mêmes décisions par voie, porte `vfp_can_use_fpu()` en deux fenêtres de 64 bits du `float_status` calculées au `realize` ; une voie infinie ou un test raté : le helper d'origine hors ligne. Touche `include/tcg/{tcg-opc.h,tcg-op-common.h}`, `tcg/{tcg.c,tcg-op.c,optimize.c,tcg-has.h}`, `tcg/x86_64/*`, `tcg/aarch64/*`, `target/ppc/{cpu.h,cpu_init.c,helper.h,int_helper.c,translate.c,translate/vmx-impl.c.inc}`. Preuve : modèle de l'émetteur x86 dans `vfpproof.sh` (`VFPPROOF_NATIVE=1`, 8 mutants), `vfptest` invité, vérificateur. `docs/tcg-g4.md` §28. |
| `tcg/0025-ppc-lmw-vector.patch` | Copie NEON après la sonde RAM de `lmw/stmw`, `LMWVEC=1`, défaut QEMU éteint, lanceur macOS arm64 allumé sur demande. Preuves GPR 32/64 bits et fautes invitées ; **aucun gain mesuré sur M4**, clang vectorise déjà la boucle d’origine. |
| `tcg/0026-tcg-jc-word.patch` | Hachage par mots dans le même groupe de page du cache de sauts, `JCWORD=1`, défaut QEMU éteint, lanceur macOS arm64 allumé sur demande. 573 440 comparaisons C/TCG et vérificateur invité sans écart ; **aucun gain mesuré sur M4**. |
| `tcg/0027-tcg-splitwx-rw-away.patch` | Vue RW placée par indication hors de la fenêtre du texte, pour laisser la place à RX sous `x-jit-near` + `split-wx`. 20/20 lancements M4 dans la bonne fenêtre après correction. |
| `tcg/0028-tcg-vfp-neon-slow.patch` | Corrige les arguments du helper lent de 0022 sur aarch64 : `tcg_out_addi_ptr` est un bouchon en assertion dans QEMU 11. Emprunt lent réellement exécuté et vérifié en VM. |
| `tcg/fixes/{0003-nmsub-zero,0022-neon-nmsub-zero}.patch` | Migration des arbres déjà patchés : `vnmsubfp` minuscule ou nul passe au softfloat pour préserver le zéro signé. Arbres neufs corrigés directement dans 0003/0022. |

| `tcg/0023-ppc-tb-div.patch` | **`mftb` sans `div` 64 bits** (03/10/2026) : sous `x-tb-fast`, la base de temps à 25 MHz divise les ns par la constante 40 (multiplication et décalage) au lieu d'une variable ; même quotient pour tout `uint64_t`. ~3,3 ns de moins par `mftb` sur l'i7-10700F. Touche `hw/ppc/ppc.c`. `docs/tcg-g4.md` §28. |
| `tcg/0024-tcg-jit-rel32.patch` | **Tampon du JIT à moins de 2 Gio du texte, Linux x86-64** (03/10/2026) : propriété de l'**accélérateur** `x-jit-rel32` (défaut *off*, `JITREL32=1`). Sous Linux le noyau pose le tampon à ~35 Tio du texte PIE (`x-jit-near` n'y trouve jamais sa fenêtre : « pas de place, noyau » dans tous les journaux du PC) ; `tcg_out_branch()` ne peut alors émettre que `call *[rip+pool]` vers les helpers. `x-jit-rel32` demande la place juste sous le texte (indication de `mmap`, vérifiée, réduction jusqu'à 512 Mio) et imprime si les appels sont directs. Sans effet ailleurs. Touche `tcg/region.c`, `accel/tcg/tcg-all.c`, `include/tcg/startup.h`. `docs/tcg-g4.md` §28. |
| `tcg/0031-ppc-tlb-precise.patch` | **TLB invalidé avec précision** (06/10/2026, PC x86-64) : propriétés de CPU `x-tlb-precise`, `x-tlb-precise-verify=N` et `x-mem-stats` (défaut *off*, `TLBPRECISE=1`, `TLBPVERIFY=N`, `MEMSTATS=1`). MMU 32 bits à table de hachage : `tlbie` retire sa classe de congruence élargie (EA[16:19], balayage au pas de 16) sur tous les CPU au même moment que le vidage d'origine, et les pages de ses opérandes du cache de sauts ; un changement de registre de segment (avec `x-sr-tlb`) retire les pages journalisées du segment ; une BAT, sa plage ; le TLB grandit quand il déborde. Corrige aussi le vérificateur de `x-sr-tlb` (table rapide indexée à l'envers dans QEMU 11). Touche `accel/tcg/cputlb.c`, `include/exec/cputlb.h`, `target/ppc/{cpu.h,cpu_init.c,helper.h,helper_regs.[ch],mmu_helper.c,translate.c,translate/storage-ctrl-impl.c.inc}`. Preuves : `tools/tcg/tlbpproof.py` (extraits, mutants), `tools/guest/jobs/tlbtest`, vérificateur. `docs/tcg-g4.md` §32. |
| `tcg/0032-ppc-lmw-inline.patch` | **`lmw`/`stmw` en ligne** (06/10/2026) : `x-lmw-inline`, `x-lmw-inline-verify` (défaut *off*, `LMWINLINE=1`, `LMWVERIFY=1`). Plage dans une page (test à l'exécution) : accès mot de TCG, mêmes accès que le chemin d'E/S du helper, le premier fait la faute ; sinon le helper. Autre conception que l'essai `essais/0002` (même nom de propriété). Touche `target/ppc/{cpu.h,cpu_init.c,helper.h,mem_helper.c,translate.c}`. Preuves : `lmwtest`, `dcbztest`, vérificateur, mutants. `docs/tcg-g4.md` §32. |
| `tcg/0033-ppc-dcbz-inline.patch` | **`dcbz` en ligne** (06/10/2026) : `x-dcbz-inline`, `x-dcbz-inline-verify` (défaut *off*, `DCBZINLINE=1`, `DCBZVERIFY=1`). Ligne de 32 octets : réservation retirée puis quatre rangements de zéros (ceux du chemin d'E/S du helper). Retire toujours la réservation, ce que le helper d'origine (`TCG_CALL_NO_WG` mais écrivant `reserve_addr`) ne fait pas toujours. Touche `target/ppc/{cpu.h,cpu_init.c,helper.h,mem_helper.c,translate.c}`. `docs/tcg-g4.md` §32. |
| `usbhid/0001-usb-tablet-abs-margin.patch` | **Tablette USB juste sous Tiger 10.4.11** (02/10/2026) : propriété `x-abs-margin` (en %, défaut 0) de `usb-tablet`. L'IOHIDEventDriver de 10.4.11 (IOHIDFamily 1.4.13, portage de Leopard) retire `pct/2` % de chaque bout des axes absolus, 15 en dur ; la tablette rend alors ses coordonnées dans `[m, 0x7fff − m]`, m calculé comme l'invité. `run_tiger.sh` et `devloop.py` passent 15 (`TABLET_MARGIN=0` pour ≤ 10.4.10). Touche `hw/input/hid.c`, `hw/usb/dev-hid.c`, `include/hw/input/hid.h`. `docs/tablette-tiger-10.4.11.md`. |

Les constantes `OUT_DATA` / `IN_DATA` / `OUT_ENABLE` étaient absentes de `gpio.c` en 9.2.0 ;
11.1 les a dans `enum MacioGPIORegisterBits`, et le garde de l'étape 2 reconnaît cette forme
(une macro du même nom casserait l'enum). Historique :
le patch les **porte désormais lui-même** (mêmes valeurs que l'enum de `balaton2`, voir plus
bas), de sorte qu'il compile seul sur un arbre pristine. L'étape 2 de
`scripts/build_qemu_qfb.sh`, qui les réinjectait par un script Python, devient de ce fait un
no-op : son garde (`grep -q "define OUT_ENABLE"`) les trouve déjà. Elle est laissée en place,
elle ne coûte rien.

### Ce que le patch corrige par rapport à la version d'août 2025

Findings S-C1, S-C2, S-C3, S-M1, S-M2, S-M3 et mineurs de
`docs/bug-hunt-2026-09-22.md` §8.4 :

- **`cpu_kick()` passe par `async_run_on_cpu()`** (S-C2) : le travail tourne sur le thread du
  vCPU visé, plus sur celui du cœur qui écrit le GPIO. C'est aussi ce qui **réveille** le cœur
  secondaire (S-C3) : `async_run_on_cpu()` appelle `qemu_cpu_kick()`, seul moyen de sortir le
  thread de son `qemu_cond_wait(halt_cond)`. Motif de `hw/ppc/ppce500_spin.c` et
  `target/arm/arm-powerctl.c`.
- **`excp_prefix = 0` après `cpu_reset()`** (S-M1) : `cpu_reset()` repose `MSR[EP]` et donc
  `excp_prefix = 0xFFF00000`. Posé avant, il ne marchait qu'au premier kick.
- **`spr_cb[SPR_PIR].default_value = cpu_index`** (S-C1) : sans cela `register_74xx_sprs()`
  laisse PIR à 0 sur les deux cœurs, et `MacRISC2CPU` prend les deux nœuds `/cpus` pour le
  cœur d'amorçage — un seul processeur, sans message. Même endroit que
  `hw/ppc/spapr_cpu_core.c` : après `realize`, avant le reset.
- **`gpio_regs[addr]` toujours stocké** (S-M2) : `OUT_ENABLE`/`OUT_DATA` restent relisibles ;
  seul le niveau est dérivé pour la ligne de reset.
- **GPIO 4 seul** (S-M3, révisé le 29/09/2026) : le firmware livré ne publie pas de propriété
  `soft-reset` et Tiger retombe sur son offset codé en dur. `AppleMacRISC2PE` prend 0x5B pour
  le **CPU 0** et 0x5C pour les autres : le GPIO 3 est la ligne du CPU 0, et le relier à
  `cpus[1]` réinitialisait le CPU 1 en pleine exécution. Retiré (docs/bug-hunt-2026-09-29.md, M1).
- **Démarrage au relâchement** (29/09/2026) : le CPU 1 démarrait à l'assertion (écriture 0x4)
  et une ligne tenue basse ne le retenait pas. Le reset machine remet la ligne haute sans
  front, pour ne pas lancer le CPU 1 avant que l'invité le demande.
- Mineurs : garde-fou openpic réel au lieu d'`#if 0` ; plus de boucle qui écrase le lien d'IRQ
  au-delà de deux CPU ; `-smp 2` sans `via=pmu` sort par un `error_report` au lieu d'un
  `sysbus_connect_irq(NULL, …)`.

Pour trancher en VM, les traces amont suffisent :
`-d trace:macio_gpio_write,trace:macio_set_gpio,trace:macio_gpio_irq_assert,trace:macio_gpio_irq_deassert`.

Reste **côté firmware**, hors de portée de ce patch : `openbios-smp-screamer.elf` (source :
`smp-mac99/openbios-smp-screamer-source.patch`) ne publie pas de propriété `soft-reset`, et c'est lui qui décide de ce
que vaut `reg` dans les nœuds `/cpus` (il doit y mettre le PIR pour que le correctif S-C1
serve).

## Le device audio Screamer

`screamer/screamer.c` et `screamer/screamer.h` sont **vendus dans ce dépôt**, repris de la
branche `screamer-v9.1.0` du fork de Mark Cave-Ayland
([github.com/mcayland/qemu](https://github.com/mcayland/qemu)) — série de 11 commits sur
`v9.1.0`, dont le delta hors firmware fait 590 lignes. Les copier ici plutôt que de les
récupérer au build est délibéré : **le dépôt doit pouvoir reconstruire son binaire de
référence sans dépendre d'un fork tiers**, qui peut disparaître ou se réécrire.

Une seule modification fonctionnelle par rapport à l'amont, signalée en tête de
`screamer.c` : `dc->reset` a disparu entre QEMU 9.1 et 9.2, remplacé par
`device_class_set_legacy_reset()` (même sémantique — `hw/display/qfb-pci.c` utilise déjà la
forme 9.2).

La garde de débordement commentée de `pmac_screamer_tx()` a été **remplacée par l'explication
de pourquoi elle ne doit pas être restaurée** (commentaire, aucun changement de code) : le
débordement est déjà empêché par le `MIN()` de `pmac_screamer_tx_transfer()`, et restaurer la
garde bloquerait le son sur toute requête DBDMA plus grosse que `mixbuf` — 0 octet transféré,
`wpos - rpos` nul, `screamerspk_callback()` qui ressort aussitôt, transfert reporté
indéfiniment.

Correctifs DBDMA des bug hunts du 29/09/2026 (`docs/bug-hunt-2026-09-29.md`) : arrêt, pause et
FLUSH du canal de sortie (`pmac_screamer_tx_flush`), reliquat de moins d'une trame, index de
relecture du codec ; puis (3e passe) le fragment différé tiré par le callback audio écrit
`RUN|ACTIVE` dans le `xfer_status` du descripteur comme le DBDMA réel (et non le statut sans
RUN), il est abandonné si l'invité réécrit CMDPTR pendant une pause, et `screamer_reset` remet à
faux `io.processing` des deux canaux (que `mac_dbdma_reset` ne touche pas).

Le son a besoin des **deux moitiés** : ce device côté QEMU, et le nœud audio publié côté
firmware par l'OpenBIOS unifié (`openbios-smp-screamer.elf`, plus bas).

⚠ **Le Screamer n'apparaît pas dans `qemu-system-ppc -device help`** : ce n'est pas un device
instanciable en ligne de commande, c'est un enfant interne du macio. Pour vérifier sa
présence il faut interroger QOM — c'est ce que fait `scripts/caps.sh`, utilisé à la fois par
les lanceurs et par la vérification de capacités de `scripts/build_qemu_qfb.sh`. Un premier
jet de ce sondage s'est fait piéger par `-device help` et a conclu à tort à un binaire
incomplet.

## Le flottant rapide (`fastfp/`)

Ces deux patches-là sont les seuls du dépôt qui touchent à la **performance**,
et pas à une fonctionnalité — d'où la précaution : ils ajoutent un mode, ils ne
changent rien par défaut. `-cpu g4` sans option donne exactement le binaire
d'avant (vérifié octet pour octet, § `docs/flottant-rapide.md` 5.3) ; il faut
`-cpu g4,x-fast-fp=on` (c'est-à-dire `FASTFP=1 ./run_tiger.sh`) pour l'allumer.

Les deux « optimisations TCG tentées puis revertées » du tableau plus bas ont
laissé une règle dans ce dépôt : *le binaire utilisé n'est patché que pour des
fonctionnalités, jamais pour la performance du JIT.* Le flottant rapide ne la
viole pas — il n'accélère rien tant qu'on ne le demande pas — mais il mérite la
même exigence de preuve, d'où le volume de vérification documenté. Même règle
pour `tcg/0001` (`x-sr-tlb`, `docs/tcg-g4.md`) : éteint par défaut, prouvé par un
mode vérificateur intégré.

Ce qu'il fait, en deux lignes : QEMU sait utiliser le FPU de la machine hôte
(mécanisme *hardfloat* de `fpu/softfloat.c`), mais l'interdit à PowerPC, parce
que hardfloat exige que le drapeau « inexact » soit déjà posé et que PowerPC
remet les drapeaux à zéro avant chaque instruction (FPSCR[FI] n'est pas
collant). Le patch ajoute un amorçage conditionnel qui ne s'arme qu'une fois
FPSCR[XX] (collant, lui) réellement posé, plus des chemins rapides **exacts**
pour les opérations simple précision (`float64r32_*`), qui n'en avaient aucun.

Résultats, FPRF et tous les bits d'exception de FPSCR restent identiques au bit
près ; seuls FPSCR[FI] (constant à 1 une fois amorcé) et la règle de transition
de FX dévient. Conception complète, invariants et **preuves mesurées** :
`docs/flottant-rapide.md`. Test différentiel hôte : `tests/fastfp-diff.c`,
exécuté par `tests/run-all.sh`.

⚠ Le garde d'idempotence de `scripts/build_qemu_qfb.sh` teste
`ppc_fp_primed` dans `target/ppc/fpu_helper.c` — c'est le **dernier** fichier
du patch, et surtout le seul dont l'absence serait complètement silencieuse :
la propriété existerait, `-cpu g4,x-fast-fp=on` serait accepté, et le mode
rapide ne ferait rien. Un A/B aurait conclu « aucun gain » au lieu de « patch à
moitié appliqué ». Les cinq marqueurs sont vérifiés un par un après coup.

## Firmware pré-buildé

| Fichier | État |
| --- | --- |
| `smp-mac99/openbios-smp-screamer.elf` | **Celui qu'on utilise.** OpenBIOS unifié : bring-up SMP + nœud audio screamer. Passé en `-bios` par `run_tiger.sh` et `run_os9.sh`. Compilé en `-O1` (gcc-13 miscompile ce code OpenBIOS à `-Os`). |
| `smp-mac99/openbios-qemu-smp.elf` | **Supplanté.** Build antérieur, SMP seul, sans le nœud audio. Gardé pour bissecter si l'unifié régresse. Aucun script ne le référence. |

Ces `.elf` sont des binaires (OpenBIOS, GPL-2.0). La source de `openbios-smp-screamer.elf` est
`smp-mac99/openbios-smp-screamer-source.patch` : le commit `e1e703a` de
`github.com/mcayland/openbios` (branche screamer de Mark Cave-Ayland) plus un diff de deux
fichiers ; l'arbre ainsi patché redonne l'ELF à l'octet près (sha1 `f11083a8…`, vérifié le
01/10/2026). Il faut la chaîne croisée PowerPC pour le reconstruire.

## Matériau d'origine — non appliqué

| Fichier | Ce que c'est |
| --- | --- |
| `smp-mac99/balaton` | Archive brute (mbox) du mail de BALATON Zoltan sur `qemu-ppc`, février 2025 : *« hw/misc/macio/gpio.c: Add defines for register bits »*. Contient le corps du patch. C'est la **source** des constantes GPIO. |
| `smp-mac99/balaton1` | Le **même mail, tronqué** (en-têtes et discussion, corps du patch absent). Doublon sans valeur propre. |
| `smp-mac99/balaton2` | La **v2** du même : *« Add constants for register bits »*, sous forme d'`enum MacioGPIORegisterBits { OUT_DATA=1, IN_DATA=2, OUT_ENABLE=4 }`. C'est la forme amont retenue ; le build n'en reprend que `IN_DATA` et `OUT_ENABLE`, les deux dont le patch SMP a besoin. |
| `smp-mac99/openbios.patch` | Patch OpenBIOS tiers ayant servi à produire `openbios-qemu-smp.elf`. **Ne s'applique pas tel quel** : ses chemins sont enracinés dans l'arbre de son auteur (`a/home/hsp/src/openbios-…`), et il laisse un `printk` de debug plus un `cpu_add_pir_property()` commenté. Valeur documentaire. |

## Optimisations TCG tentées puis revertées

| Fichier | Verdict mesuré |
| --- | --- |
| `smp-mac99/essais/qemu-mac99-4cpus.patch` | **Essai 3-4 CPU** (26/09/2026), par-dessus `qemu-mac99-cpus-v2.patch` : `mc->max_cpus = 4`, GPIO 15 et 16 de KeyLargo (offsets 0x67/0x68, `KL_GPIO_RESET_CPU2/3` de Linux, ceux que Tiger 10.4.6 écrit) câblés sur le reset des CPU 2 et 3. Firmware livré inchangé. Tiger démarre sur 3 et 4 processeurs (`hw.ncpu 4`) ; aucun gain sur les jeux. **Non appliqué.** `docs/smp-coeurs.md`. |
| `tcg/essais/0009-ppc-isync-chain.patch` | **Exact mais sans gain mesuré** (26/09/2026) : `isync` cherche le bloc suivant (`DISAS_CHAIN_UPDATE`) au lieu de revenir à `cpu_exec`, propriété `x-isync-chain`. Les recherches de la boucle principale tombent de ~110 M à ~30 M par tour de preuve, 0 divergence (`x-ret-verify`, SMP=2 et SMP=1), `smctest` identique ; Marble Blast pas meilleur que `tcg/0008` seul (A/B bruité, −4 % contre `x-ret-inline`+`x-jc-idx`). **Non appliqué.** S'applique par-dessus `tcg/0008`. `docs/tcg-g4.md` §16. |
| `tcg/essais/0005-ppc-vfp-nrwg.patch` | **Exact mais sans gain** (25/09/2026) : les quatre helpers flottants AltiVec appelés en `TCG_CALL_NO_RWG` (jumeaux `*_nrwg`, propriété `x-vfp-nrwg`) ; licite (ni globale TCG ni exception), même empreinte `vfptest` ; banc 1 313 → 1 346 ms. **Non appliqué.** S'applique par-dessus `tcg/0004`. |
| `tcg/essais/0002-ppc-lmw-inline.patch` | **Exact mais sans gain** (25/09/2026) : `lmw`/`stmw` en accès en ligne quand l'accès tient dans une page (helper sinon), propriété `x-lmw-inline`. Test invité `tools/guest/jobs/lmwtest` : sortie identique octet pour octet (608 cas, 14 fautes à cheval sur deux pages) ; banc de 20 M paires de 19 registres : 1 564/1 582 → 1 544/1 548 ms (−1,3 %) — le chemin rapide du helper (`probe_contiguous` + copie) coûte autant que 19 accès TCG. **Non appliqué.** S'applique par-dessus `tcg/0001`. |
| `01-timebase-and-vclock.patch` | **Neutre** (23,32 s = stock). Reverté : zéro gain, et l'approximation par réciproque touche le timing. |
| `02-jmpcache-generation.patch` | **Régression de ~23 %** (28,63 s). Reverté. |

Détail du protocole de mesure et des conclusions : `docs/metrologie-boot.md` (01, 02) et
`docs/tcg-g4.md` (essais/0002, essais/0005). Aucun de ces patches n'est appliqué par le
build. Les patches de performance appliqués (`fastfp/`, `tcg/0001` à `0004`, `0007`, `0008`) ajoutent chacun
une propriété de CPU éteinte par défaut (`x-sr-tlb` est depuis allumée par `run_tiger.sh`) :
sans elle, le binaire se comporte comme avant.

## Licences

`qgpu/*.c` et `qgpu/*.h` sont sous GPL-2.0-or-later (code POMPPC).
`qfb/qfb-pci.c` est sous GPL-2.0-or-later : il dérive de `hw/display/mac_qfb.c` (Solra Bizna),
lui-même dérivé du code de Laurent Vivier et Hervé Poussineau.
`screamer/screamer.c` et `screamer/screamer.h` sont sous licence MIT, © 2016 Mark
Cave-Ayland — en-tête de licence conservé tel quel. Les patches QEMU et les mails
de liste relèvent de la licence de QEMU (GPL-2.0). Voir aussi `kext/POMPPCQFB/README.md`
§ « Licence et crédits ».
