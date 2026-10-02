# La tablette USB sous Tiger 10.4.11

02/10/2026, PC Linux, VM de dev passée de 10.4.6 à 10.4.11 (combo, `disks/tiger-dev-10.4.6.raw`
gardé).

## Symptôme

Sous 10.4.11 le pointeur ne tombe plus où l'hôte clique : `devloop.py click 945 240` (l'icône de
Marble Blast) rate, le pointeur se colle au bord droit. Deux clics au même point tombent au même
endroit (absolu), mais la correspondance est **affine** : clic (200, 600) → pointeur (145, 643),
clic (945, 240) → (1020, 220). Pente 1,175 sur les deux axes, centrée : l'écran de 1024×768 est
vu comme un rectangle d'environ 1203×902.

## Cause

Pile HID changée par la mise à jour : IOHIDFamily 1.4.4 → 1.4.13, IOUSBHIDDriver 2.2.5 → 2.5.5.
Sous 10.4.11, la tablette (« QEMU USB Tablet ») est prise par **IOHIDEventDriver**, portage de
celui de Leopard. Ce pilote retire d'office 15 % de la plage logique des axes absolus, 7,5 % à
chaque bout (`IOHIDEventDriver.cpp` d'IOHIDFamily-258.1 : `absoluteAxisRemovalPercentage = 15`,
`boundsDiff = (max − min) · 15 / 200`) : 1 / 0,85 = 1,176. Le binaire de 10.4.11 ne connaît pas la
propriété `AbsoluteAxisBoundsRemovalPercentage` des versions suivantes (absente de `strings`) :
le 15 y est en dur. Un kext sans code qui écarte IOHIDEventDriver de la tablette (pilote
`IOService` plus prioritaire) la rend muette : c'est le seul chemin des événements sous 10.4.11.

## Correction

`patches/usbhid/0001` : propriété `x-abs-margin` (en %) de `usb-tablet`. Les coordonnées 0..0x7fff
de QEMU sont rendues dans `[m, 0x7fff − m]`, `m = 0x7fff · pct / 200` (le même calcul entier que
l'invité), le descripteur HID ne change pas : l'invité retranche m et retombe sur l'écran entier.
`run_tiger.sh` (`TABLET_MARGIN`, défaut 15) et `devloop.py` la passent si le binaire l'a ;
`TABLET_MARGIN=0` pour un invité 10.4.10 ou plus ancien, qui n'a pas ce retrait.

Vérifié sous 10.4.11 : clics en (200, 600), (800, 150), (8, 760) et (1016, 8) sous la pointe du
pointeur, double-clic sur l'icône de Marble Blast qui lance le jeu.
