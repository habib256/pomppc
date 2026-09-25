# patches/ — ce qui est appliqué, ce qui est là pour référence

Deux natures de fichiers cohabitent ici, et la distinction compte : **ce que
`scripts/build_qemu_qfb.sh` applique réellement**, et **le matériau d'origine** (mails de
liste, essais revertés, binaires supplantés) gardé pour pouvoir refaire le raisonnement.

## Appliqué par `scripts/build_qemu_qfb.sh`

| Fichier | Rôle |
| --- | --- |
| `smp-mac99/qemu-mac99-cpus-v2.patch` | SMP mac99. Remplace le garde-fou `« Only UP supported today »` d'`hw/intc/openpic.c` par la vraie limite du modèle (`nb_cpus > KEYLARGO_MAX_CPU`), ajoute les GPIO 3 **et** 4 de KeyLargo (lignes de reset du CPU 1) dans `hw/misc/macio/gpio.c`, et dans `hw/ppc/mac_newworld.c` le `PIR = cpu_index` plus le `cpu_kick()` qui relâche le cœur secondaire par `async_run_on_cpu()`. S'applique **sans fuzz ni décalage** sur QEMU 9.2.0 pristine (le build l'applique quand même avec `--fuzz=3`). |
| `qfb/qfb-pci.c` | Le device paravirtuel `qfb-pci`, copié dans `hw/display/`. Protocole « qfb1 » de Solra Bizna porté du NuBus vers PCI. |
| `qfb/0002-wire-qfb-pci-build.patch` | Câblage meson/Kconfig du device ci-dessus. |
| `qgpu/qgpu-pci.c`, `qgpu-core.[ch]`, `qgpu-soft.c`, `qgpu-gl.c`, `qgpu_proto.h` | Le GPU paravirtuel `qgpu-pci` (protocole v16 : géométrie brute, tampons hôte, programmes ARB) : device (transport), cœur d'exécution du flux de commandes (contextes, surfaces, textures, programmes, état GL), backend logiciel de référence, **backend OpenGL** (CGL/EGL, rendu sur le GPU hôte), et le contrat hôte/invité. Copiés dans `hw/display/`. |
| `qgpu/0003-wire-qgpu-pci-build.patch` | Câblage meson/Kconfig de `qgpu-pci` ; lie `OpenGL.framework` (macOS) ou EGL+GL (Linux) si présents, sinon le backend GL est un stub. |
| `screamer/screamer.c` + `screamer/screamer.h` | Le device audio **Screamer** (AWACS PowerMac), copiés dans `hw/audio/` et `include/hw/audio/`. |
| `screamer/0001-wire-screamer-build.patch` | Câblage du Screamer : `hw/audio/Kconfig`, `hw/audio/meson.build`, `hw/ppc/Kconfig`, et surtout l'instanciation + les IRQ/DBDMA dans `hw/misc/macio/macio.c`. |
| `fastfp/0001-ppc-fast-fp.patch` | **Flottant rapide** : propriété de CPU `x-fast-fp` (défaut *off*) qui laisse softfloat confier les opérations flottantes au FPU de l'hôte. Touche `fpu/softfloat.c`, `include/fpu/softfloat-types.h`, `target/ppc/{cpu.h,cpu_init.c,fpu_helper.c}`. Voir plus bas et `docs/flottant-rapide.md`. |
| `fastfp/0002-ppc-fewer-fp-helpers.patch` | 4 appels de helper par instruction flottante → 2 (`reset_fpstatus` émis en ligne, `compute_fprf` + `float_check_status` fusionnés). **Aucun effet observable**, dans aucun des deux modes. S'applique par-dessus le 0001 ; `NO_FASTFP2=1` sur un arbre propre applique le 0001 seul. |
| `tcg/0001-ppc-sr-tlb.patch` | **TLB gardé d'un jeu de segments à l'autre** : propriété de CPU `x-sr-tlb` (défaut *off*, `SRTLB=1 ./run_tiger.sh`). Un changement de registre de segment ne vide plus tout le TLB de QEMU (~23 000 fois par seconde sous Tiger) : chaque `mmu_idx` traduit retient le jeu de segments de ses entrées et n'est vidé que s'il sert sous un autre ; `tlbie` devient global en SMP. Mode preuve `x-sr-tlb-verify=N`. Touche `target/ppc/{cpu.h,cpu_init.c,helper_regs.[ch],mmu_helper.c}`. Marble Blast SMP=2 : **+9,8 %** img/s. Voir `docs/tcg-g4.md`. `NO_TCG=1` le saute. |

Les constantes `OUT_DATA` / `IN_DATA` / `OUT_ENABLE` sont absentes de `gpio.c` en 9.2.0 :
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
- **GPIO 3 en plus du GPIO 4** (S-M3) : le firmware livré ne publie pas de propriété
  `soft-reset` et Tiger retombe sur son offset codé en dur, 0x5B ou 0x5C selon les versions,
  soit GPIO 3 soit GPIO 4. Le GPIO 3 tombait dans `LOG_UNIMP`, en silence.
- Mineurs : garde-fou openpic réel au lieu d'`#if 0` ; plus de boucle qui écrase le lien d'IRQ
  au-delà de deux CPU ; `-smp 2` sans `via=pmu` sort par un `error_report` au lieu d'un
  `sysbus_connect_irq(NULL, …)`.

Pour trancher en VM, les traces amont suffisent :
`-d trace:macio_gpio_write,trace:macio_set_gpio,trace:macio_gpio_irq_assert,trace:macio_gpio_irq_deassert`.

Reste **côté firmware**, hors de portée de ce patch : `openbios-smp-screamer.elf` n'a pas de
source dans le dépôt, ne publie pas de propriété `soft-reset`, et c'est lui qui décide de ce
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

Ces `.elf` sont des binaires : ils ne se régénèrent pas depuis ce dépôt (il faut un arbre
OpenBIOS et la chaîne croisée PowerPC).

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
| `tcg/essais/0002-ppc-lmw-inline.patch` | **Exact mais sans gain** (25/09/2026) : `lmw`/`stmw` en accès en ligne quand l'accès tient dans une page (helper sinon), propriété `x-lmw-inline`. Test invité `tools/guest/jobs/lmwtest` : sortie identique octet pour octet (608 cas, 14 fautes à cheval sur deux pages) ; banc de 20 M paires de 19 registres : 1 564/1 582 → 1 544/1 548 ms (−1,3 %) — le chemin rapide du helper (`probe_contiguous` + copie) coûte autant que 19 accès TCG. **Non appliqué.** S'applique par-dessus `tcg/0001`. |
| `01-timebase-and-vclock.patch` | **Neutre** (23,32 s = stock). Reverté : zéro gain, et l'approximation par réciproque touche le timing. |
| `02-jmpcache-generation.patch` | **Régression de ~23 %** (28,63 s). Reverté. |

Détail du protocole de mesure et des conclusions : `docs/metrologie-boot.md` (01, 02) et
`docs/tcg-g4.md` (essais/0002). Aucun de ces patches n'est appliqué par le build. Les deux
patches de performance appliqués (`fastfp/`, `tcg/0001`) ajoutent chacun une propriété de
CPU éteinte par défaut : sans elle, le binaire se comporte comme avant.

## Licences

`qgpu/*.c` et `qgpu/*.h` sont sous GPL-2.0-or-later (code POMPPC).
`qfb/qfb-pci.c` est sous GPL-2.0-or-later : il dérive de `hw/display/mac_qfb.c` (Solra Bizna),
lui-même dérivé du code de Laurent Vivier et Hervé Poussineau.
`screamer/screamer.c` et `screamer/screamer.h` sont sous licence MIT, © 2016 Mark
Cave-Ayland — en-tête de licence conservé tel quel. Les patches QEMU et les mails
de liste relèvent de la licence de QEMU (GPL-2.0). Voir aussi `kext/POMPPCQFB/README.md`
§ « Licence et crédits ».
