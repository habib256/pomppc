# patches/ — ce qui est appliqué, ce qui est là pour référence

Deux natures de fichiers cohabitent ici, et la distinction compte : **ce que
`scripts/build_qemu_qfb.sh` applique réellement**, et **le matériau d'origine** (mails de
liste, essais revertés, binaires supplantés) gardé pour pouvoir refaire le raisonnement.

## Appliqué par `scripts/build_qemu_qfb.sh`

| Fichier | Rôle |
| --- | --- |
| `smp-mac99/qemu-mac99-cpus-v2.patch` | SMP mac99. Neutralise le garde-fou `« Only UP supported today »` d'`hw/intc/openpic.c`, ajoute le GPIO 4 de KeyLargo (ligne de reset du CPU1) dans `hw/misc/macio/gpio.c`, et le `cpu_kick()` qui relâche le cœur secondaire dans `hw/ppc/mac_newworld.c`. Appliqué avec `--fuzz=3` sur QEMU 9.2.0. |
| `qfb/qfb-pci.c` | Le device paravirtuel `qfb-pci`, copié dans `hw/display/`. Protocole « qfb1 » de Solra Bizna porté du NuBus vers PCI. |
| `qfb/0002-wire-qfb-pci-build.patch` | Câblage meson/Kconfig du device ci-dessus. |

Le patch SMP suppose les constantes `IN_DATA` / `OUT_ENABLE`, absentes de `gpio.c` en 9.2.0 :
le script les réinjecte lui-même (mêmes valeurs que l'enum de `balaton2`, voir plus bas)
plutôt que d'appliquer un second patch amont.

## Ce qui manque ici — le device audio Screamer

Le binaire de référence (celui qui fait tourner `run_tiger.sh` et `run_os9.sh` avec le son)
contient un **port maison du device `screamer`** depuis le fork de Mark Cave-Ayland. Ce port
**n'est pas dans ce dépôt** : ni source, ni patch. `scripts/build_qemu_qfb.sh` ne peut donc
pas le reconstruire, et un binaire issu de ce script refusera
`-global screamer.audiodev=snd0` — d'où le `NOSOUND=1` obligatoire avec ce build.

L'OpenBIOS unifié, lui, *est* fourni pré-buildé (`openbios-smp-screamer.elf`) : il publie le
nœud audio côté firmware, mais ne remplace pas le device côté QEMU. Les deux moitiés sont
nécessaires pour avoir du son.

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
| `01-timebase-and-vclock.patch` | **Neutre** (23,32 s = stock). Reverté : zéro gain, et l'approximation par réciproque touche le timing. |
| `02-jmpcache-generation.patch` | **Régression de ~23 %** (28,63 s). Reverté. |

Détail du protocole de mesure et des conclusions : section « Optimisations » du `README.md`
racine. Aucun de ces deux patches n'est appliqué par le build — le binaire utilisé n'est
patché que pour des **fonctionnalités**, jamais pour la performance du JIT.

## Licences

`qfb/qfb-pci.c` est sous GPL-2.0-or-later : il dérive de `hw/display/mac_qfb.c` (Solra Bizna),
lui-même dérivé du code de Laurent Vivier et Hervé Poussineau. Les patches QEMU et les mails
de liste relèvent de la licence de QEMU (GPL-2.0). Voir aussi `kext/POMPPCQFB/README.md`
§ « Licence et crédits ».
