# POMPPCQFB — pilote Tiger pour le framebuffer paravirtualisé « qfb1 »

Portage sur **PCI / PowerPC / Mac OS X 10.4** du framebuffer paravirtualisé
[`mac_qfb_driver`](https://github.com/SolraBizna/mac_qfb_driver) de Solra Bizna
(à l'origine : NuBus, Quadra 800 émulée, pilote 68k dans une ROM de déclaration).

| Côté | D'origine (NuBus / 68k) | Ici (PCI / PowerPC) |
|---|---|---|
| Device QEMU | `nubus-qfb` (`hw/display/mac_qfb.c`) | **`qfb-pci`** (`patches/qfb/qfb-pci.c`) |
| Pilote invité | ROM de déclaration + driver Slot Manager | **`POMPPCQFB.kext`** (sous-classe `IOFramebuffer`) |
| OS invité | Mac OS 7–9 / A/UX | Mac OS X 10.4 Tiger |
| Bus | NuBus (slot space) | PCI : BAR0 = VRAM 32 Mio, BAR1 = registres |

Le **protocole registre est identique** : même signature `'qfb1'`, mêmes offsets,
même calcul de `rowbytes`, même sémantique palette/gamma/IRQ. Un futur NDRV
Mac OS 9 pourra donc réutiliser le même device.

## État

| Élément | État | Preuve |
|---|---|---|
| Device `qfb-pci` (QEMU 9.2.0) | **fait, testé** | `tests/qfb_smoke.py` : mode 640×480×32 programmé depuis Open Firmware, `rowbytes` relu = 0xa00, bandes rouge/verte/fond gris vérifiées pixel à pixel sur un `screendump` |
| Ressources PCI assignées par OpenBIOS | **oui** | nœud `/pci@f2000000/pci1234,fb1@f`, `assigned-addresses` : VRAM `0x82000000` (32 Mio), registres `0x84000000` (4 Kio), `interrupts 1` |
| Tiger démarre avec le device attaché | **oui** (écran VGA inchangé) | boot headless + `screendump` des deux écrans |
| `POMPPCQFB.kext` | **écrit, pas encore compilé** | l'hôte Linux n'a pas de toolchain `ppc-apple-darwin8` : la compilation se fait **dans** l'invité (§ Compilation) |

## Registres (BAR1, 32 bits, big endian)

| Offset | Nom | Sens |
|---|---|---|
| `0x00` | `VERSION` | lecture : `'qfb1'` (0x71666231) ; écriture : reset du device |
| `0x04` / `0x08` | `WIDTH` / `HEIGHT` | mode courant, en pixels |
| `0x0C` | `DEPTH` | 1, 2, 4, 8, 16, 24 (**24 = 32 bpp xRGB**) |
| `0x10` | `BASE` | offset du scanout dans la VRAM (aligné 4) |
| `0x14` | `STRIDE` | `rowbytes` recalculé par le device (lecture seule) |
| `0x1C` / `0x20` | `PAL_INDEX` / `PAL_COLOR` | palette, `xxRRGGBB` |
| `0x24` / `0x28` | `LUT_INDEX` / `LUT_COLOR` | rampes gamma, `xxRRGGBB` |
| `0x2C` / `0x30` | `IRQ_MASK` / `IRQ` | VBL 60 Hz (bit 0) ; écrire dans `IRQ` acquitte |
| `0x34`…`0x3C` | `CUSTOM_*` | mode demandé sur la ligne de commande QEMU ; écrire dans `CUSTOM_DEPTH` envoie un octet sur `stderr` (traçage) |

La région est déclarée `DEVICE_BIG_ENDIAN` côté QEMU et le processeur invité est
big endian : **aucun échange d'octets dans le kext**, un `volatile UInt32 *`
suffit.

## Ce que le kext fournit

* modes : catalogue fixe (640×480 → 1920×1200) filtré par la VRAM, plus le mode
  passé à QEMU (`-device qfb-pci,width=…,height=…`) qui devient le mode par défaut ;
* profondeurs : 8 bpp indexé (CLUT), 16 bpp 555, 32 bpp xRGB ;
* changement de mode à chaud, palette, rampes gamma ;
* interruption VBL (masquée tant que personne ne s'y abonne — une IRQ 60 Hz coûte
  cher sous TCG).

Non fourni, par construction : **pas de curseur matériel** (absent du protocole
qfb1), **pas d'accélération 2D** (voir `docs/gpu-tiger-4060ti.md` §4.6), **pas de
console de boot** (Open Firmware ne sait pas piloter ce device — l'écran VGA
reste la console, `qfb-pci` arrive en second moniteur).

## Compilation (dans l'invité Tiger)

L'ABI C++ du noyau 10.4 est celle de **gcc 4.0** : compiler avec Xcode 2.5 dans
l'invité. Un kext bâti avec un autre compilateur se charge puis échoue de façon
obscure (`start()` jamais appelé, ou panic au premier appel virtuel).

```sh
# 1. côté hôte : graver les sources sur un CD et démarrer avec l'écran QFB
./scripts/make_kext_iso.sh
QFB=1 SNAPSHOT=1 \
EXTRA_ARGS="-drive file=disks/pomppcqfb-src.iso,if=ide,media=cdrom" ./run_tiger.sh

# 2. dans l'invité (Terminal)
cp -R /Volumes/POMPPCQFB ~/POMPPCQFB && cd ~/POMPPCQFB
make                      # → POMPPCQFB.kext
sudo make load            # kextload -t : vérifie dépendances et ABI
```

**Toujours travailler avec `SNAPSHOT=1`** pendant la mise au point : un kext
graphique fautif provoque une boucle de panic. Pour récupérer un disque
persistant abîmé : démarrer avec `-prom-env 'boot-args=-s'` puis sortir le kext
de `/System/Library/Extensions`.

À vérifier au premier chargement (les versions ci-dessous sont celles déclarées
dans `Info.plist`, à ajuster si `kextload` proteste) :

```sh
defaults read /System/Library/Extensions/IOGraphicsFamily.kext/Contents/Info CFBundleVersion
defaults read /System/Library/Extensions/IOPCIFamily.kext/Contents/Info CFBundleVersion
```

## Diagnostic

```sh
kextstat | grep -i pomppcqfb                     # chargé ?
ioreg -l -w0 | grep -iE "POMPPCQFB|pci1234"      # nub trouvé, matché ?
ioreg -c IOFramebuffer -l | grep -i -A5 pomppc   # modes publiés
tail -f /var/log/system.log                      # traces IOLog "POMPPCQFB: ..."
```

Traçage côté hôte, sans invité : écrire un octet dans `CUSTOM_DEPTH` (0x3C) le
renvoie sur le `stderr` de QEMU — c'est le canal de debug prévu par le protocole
d'origine.

Une fois le kext chargé, l'écran QFB apparaît comme **second moniteur** dans
*Préférences Système → Moniteurs* : y déplacer la barre des menus pour en faire
l'écran principal.

## Fichiers

| Fichier | Rôle |
|---|---|
| `qfb_regs.h` | définitions des registres, miroir exact du device |
| `POMPPCQFB.h` / `.cpp` | la sous-classe `IOFramebuffer` |
| `Info.plist` | appariement PCI `0x1234:0x0fb1`, dépendances IOKit |
| `Makefile` | build/install/load dans l'invité (gcc 4.0) |

## Licence et crédits

Protocole et implémentation d'origine : Solra Bizna (`mac_qfb_driver`,
Apache-2.0 / MIT). Le device QEMU `qfb-pci` dérive de `hw/display/mac_qfb.c`
(GPL-2.0-or-later), lui-même dérivé du code de Laurent Vivier et Hervé
Poussineau. Le kext est du code neuf écrit pour POMPPC.
