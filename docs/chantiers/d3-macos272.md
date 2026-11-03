# DOOM 3 plus lent sur le M4 depuis le 03/11

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

DOOM 3 en fenêtre mesure **36,6 à 37,8 ms/image** le 03/11, contre **28,9** le 02/11 au matin
(`m4-pollns-0211`, `m4-progressif-0211`), soit environ +27 %. UT2004 et Zenerchi n'ont pas bougé ; Marble Blast
non plus écran allumé (6,1 à 6,3 ms). macOS 27.2 a été installé le 03/11 à 12 h 48, avant toutes ces mesures.

**Ce que montre le profil** (`bench/tcg/ab/m4-os272-0311/actuel`, comparé à `m4-profils-0111n-fen/ref-1`) :
`CPU 0/TCG` occupé 34,5 % contre 67,2 %, `CPU 1/TCG` 69,6 % contre 83,4 %, `qgpu-render` 23,2 % contre 17,8 %
(`do_draw_native` 4,3 % contre 2,2 %). L'invité dort donc davantage pendant une image plus longue ; le cumul
`wait_ms` de `frames.csv` (0,5 s sur 145 s) n'explique pas cette attente.

## Écarté, mesuré le 03/11 (`bench/tcg/ab/m4-os272-0311/`, une partie sauf mention)

| Essai | DOOM 3, ms/image |
|---|---|
| Témoin | 37,26 |
| Kext et plugin d'avant le 02/11 (3 parties, écran éteint) | médiane 36,88, contre 37,05 |
| `NOSOUND=1` | 37,80 |
| `POLLNS=3` | 37,20 |
| `VCPUQOS=1` | 37,17 |
| Binaire du 26/10 (`~/src/qemu-arm64`), `QE=0` | 37,21 |
| Réglages de jeu d'avant (`D3_SET`, depuis `DoomConfig.cfg.avant-nettoyage`) | 36,72, contre 37,31 |

## Ce qui reste

[F] Inconnu : macOS 27.2 est le suspect principal, par élimination seulement ; un autre état du disque de
l'invité n'est pas exclu (`DoomConfig.cfg` et sauvegardes réécrits le 02/11 à 18 h 48). **Tranché par :** la même
mesure sur une copie du disque d'avant le 02/11 18 h 48, ou sur une autre machine restée sous macOS 27.0.1 ; puis,
si le système est en cause, un profil des attentes de l'invité (`sample` dans l'invité sur la fenêtre de mesure).

**Fermeture :** cause établie, et DOOM 3 revenu à ~29 ms ou nouvelle référence de vitesse assumée par écrit.
