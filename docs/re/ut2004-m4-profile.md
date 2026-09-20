# UT2004 : profil M4 du 20 septembre 2026

Suite de [la première installation](ut2004-demo.md). Mesures sur le disque de
**développement** `disks/tiger-dev.raw`, Tiger, G4, un CPU, QGPU. La quotidienne
n'a pas été modifiée. QEMU a été reconstruit avec `x-fast-fp`.

## Correctif vérifié

Apple refuse RenderVertexArray par un stub PPC `li r3,0; blr`. Le plugin traitait
ce refus comme un accès au framebuffer : lecture couleur/profondeur, puis
réupload avant le rendu suivant. `pomppc_proc_pre` reconnaît désormais ces deux
instructions et évite cette synchronisation inutile. Les autres fonctions
conservent leur synchronisation. Définir `POMPPC_GL_ARRAY_STUB_SYNC=1` rétablit
l'ancien comportement pour comparaison.

Ce changement ne constitue pas encore une implémentation directe des tableaux
de sommets ni des buffers persistants hôte.

Validation dans la session graphique Tiger, en 256×256 : `tri`, `varray`,
`varrayvbo`, `mixte`, `lit`, `texgen`, `game`, chacun avec correctif, ancien
comportement et Apple : **21/21 contrôles de pixels réussis**. Les sept images
corrigées sont identiques aux anciennes. Les trois premières sont aussi
identiques à Apple. Pour `varrayvbo`, lectures du framebuffer : 3 → 1 ; uploads :
2 → 0. Preuves locales : `bench/ut2004-m4/regression-256/`.

## Configuration et observations

UT Demo, DM-Rankin, fenêtre 800×600, `MaxTextureUnits=4`. Le défaut à 8 unités
provoque des replis logiciels lorsque le matériau dépasse les quatre unités
hôte. Le profil après correction montre encore une part importante du temps
principal dans GLEngine et son code généré de traitement des sommets.

| Capture | Conditions | Débit | p95 | Maximum |
|---|---|---:|---:|---:|
| `four.csv`, fenêtre à partir de 10 s, durée 20 s | FASTFP on, partie commencée, sans bots | 11,44 i/s | 122,80 ms | 358,78 ms |
| `exact-initial/`, fenêtre mesurée ~40 s | FASTFP off, caméra avant partie, sans bots | 20,67 i/s | 67,54 ms | 230,19 ms |

**Les caméras diffèrent : ces lignes ne mesurent pas le gain FASTFP.** Le premier
cas a 41 intervalles au-delà de 100 ms et deux au-delà de 250 ms. Le seuil de
30 i/s soutenues dans une partie représentative n'est pas atteint/démontré.
Les mesures sont des intervalles de Swap, pas des temps de fin GPU.

## Reproduction et suite

`POMPPC_GL_FRAMES=/tmp/frames.csv` active la trace de swaps. Sur l'hôte :

```
python3 tools/guest/frame_report.py frames.csv --start 10 --duration 20
```

Le job `tools/guest/jobs/utprofile/job.sh` lance une caméra stationnaire sans
bots, chauffe dix secondes, mesure, puis échantillonne le processus et ferme
le jeu. Il fixe `srand` par interposition Mach-O temporaire ; comparer les
captures et les nombres de triangles avant de considérer deux runs équivalents.
`-benchmark` a planté cette démo dans `FStats::UpdateString` et n'est pas utilisé.
La première mesure `seeded-exact` peut être perturbée par un dialogue de crash
antérieur ; ne pas en déduire un gain.

Pour voir la VM, `--gui` seul choisit le bureau Tiger mais reste sans fenêtre.
Après arrêt propre, lancer :

```
DEVDISK=disks/tiger-dev.raw CPU_OPTS=x-fast-fp=on POMPPC_DISPLAY=cocoa \
  python3 tools/guest/devloop.py start --gui
```

Prochaines étapes : A/B exact/FASTFP sur une scène réellement identique, puis
prise en charge directe des tableaux de sommets et buffers persistants hôte.
La validation finale reste une partie reproductible de DM-Rankin en 800×600,
avec suivi des images lentes et pauses, puis augmentation des réglages.
