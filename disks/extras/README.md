# disks/extras/ — pilotes tiers pour l'invité Mac OS 9

Le reste de `disks/` est gitignoré (images disque et CD copyrightés). Ce dossier fait
exception : il contient de **petits binaires tiers, libres, nécessaires aux fonctions
d'OS 9**, sans lesquels `run_os9.sh` perd des morceaux. Ils sont versionnés parce qu'ils sont
introuvables autrement à l'identique et qu'ils font quelques centaines de Ko.

| Fichier | Origine | Utilisé par |
| --- | --- | --- |
| `classicvirtio/ndrv/ndrvloader` | [classicvirtio](https://github.com/elliotnunn/classicvirtio) (Elliot Nunn) — pilotes virtio pour le Mac classique | **oui** : `run_os9.sh` le charge en RAM (`-device loader,addr=0x4000000`) et enchaîne sur `boot-command=init-program go`. C'est lui qui injecte les pilotes virtio **sans rien installer dans l'invité** et monte le volume `Shared` (virtio-9p) sur le bureau. |
| `classicvirtio/classic/declrom` | Idem, variante « ROM de déclaration » (slot NuBus/68k) | **non** : conservé pour une éventuelle voie déclaration-ROM. Aucun script ne le référence. |
| `USBTabletINIT.sit` | Extension `USBTabletINIT` de [kanjitalk755](https://github.com/kanjitalk755) — souris absolue via `usb-tablet` | **non, indirectement** : c'est la charge utile du mode `TABLET=1` (voir ci-dessous). Aucun script ne la monte automatiquement. |

## Souris absolue : les deux voies, et laquelle marche sans rien installer

**Voie virtio (par défaut, rien à installer).** Le `ndrvloader` apporte aussi
`virtio-tablet-pci` : la souris est absolue dès le boot disque, en `via=pmu`, sans toucher au
Dossier Système. C'est ce que fait `./run_os9.sh` sans option.

**Voie extension (`TABLET=1`), seulement si tu veux `usb-tablet`.** Elle exige `via=cuda`
(l'extension plante en `via=pmu`) et l'installation manuelle de `USBTabletINIT` dans le
Dossier Système. Le dépôt fournit l'archive `.sit`, **pas** un CD prêt à monter : les
en-têtes de `run_os9.sh` parlent de `USB_Tablet.iso`, à fabriquer d'abord.

```sh
# 1. graver l'archive sur un CD que OS 9 saura monter
mkdir -p disks/cdr
xorriso -as mkisofs -quiet -R -J -V USBTABLET \
        -o disks/cdr/USB_Tablet.iso disks/extras/USBTabletINIT.sit

# 2. le monter dans OS 9, puis, dans l'invité :
#    StuffIt Expander sur le .sit -> glisser l'extension dans le Dossier Système -> redémarrer
CDR=USB_Tablet.iso ./run_os9.sh

# 3. ensuite seulement, lancer en mode tablette
TABLET=1 ./run_os9.sh
```

## Licences

Ces fichiers appartiennent à leurs auteurs respectifs et sont redistribués sous leurs
licences d'origine (voir les dépôts liés ci-dessus) — pas sous celle de POMPPC.
