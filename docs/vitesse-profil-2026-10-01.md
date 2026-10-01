# Profil de vitesse au 01/10/2026 (configuration par défaut)

Les quatre jeux au-dessus de 40 ms/image (DOOM 3, Prey, Colin McRae, Nexuiz ARB), sur la
configuration allumée par défaut le 01/10 (A4, `x-fp-native`, attente). But : décider de la
suite de l'orientation vitesse (étapes 0 et 1 proposées le 01/10).

## Méthode

`tools/matrice/matrice.py -j d3,prey,nx,cmr -m fen,pe --sans-vidage --sample 10 --sample-hote 10`,
deux tours (`bench/vitesse/profil-20261001/t1`, `t2`, hors git), VM quotidienne, QEMU de
référence, hôte au repos (0 autre QEMU, charge 1,1 à 2,8). Après la fenêtre de mesure :
`sample` du processus QEMU sur l'hôte (nouvelle option `--sample-hote`, tour 2 seulement : au
tour 1, le rapatriement de la cellule l'écrasait, corrigé), puis `sample` du jeu dans l'invité.

Le fil principal du jeu est partagé par `tools/re/partfil.py` (nouveau) entre quatre
propriétaires, d'après les symboles de l'invité (`nm` de GLEngine, de libGL/OpenGL, et les
fonctions des sources du plugin) : **plugin** (toute pile passant par le plugin, appels au kext
compris), **GLEngine** (sinon, pile passant par GLEngine/OpenGL), **attente**, **jeu** (le reste :
code du jeu, libSystem, noyau hors GL).

## Le fil principal dans l'invité (deux tours agrégés)

| jeu | ms/image | jeu | GLEngine | plugin | dont appels au kext | attente |
|---|---|---|---|---|---|---|
| DOOM 3 fenêtre | 56,1 | 57,5 % · 32,2 | 7,5 % · 4,2 | 35,0 % · 19,6 | 1,3 | 0,0 |
| DOOM 3 plein écran | 56,6 | 60,3 % · 34,1 | 6,4 % · 3,6 | 33,2 % · 18,8 | 1,1 | 0,1 |
| Prey fenêtre | 57,1 | 65,5 % · 37,4 | 7,4 % · 4,2 | 26,3 % · 15,0 | 0,3 | 0,5 |
| Prey plein écran | 56,4 | 69,2 % · 39,0 | 6,8 % · 3,8 | 23,1 % · 13,0 | 0,1 | 0,5 |
| Colin McRae plein écran | 46,4 | 42,3 % · 19,6 | 16,3 % · 7,6 | 39,6 % · 18,4 | 0,7 | 0,8 |
| Nexuiz ARB fenêtre | 80,0 | 32,7 % · 26,2 | 31,5 % · 25,2 | 35,8 % · 28,6 | 4,7 | 0,0 |
| Nexuiz ARB plein écran | 80,2 | 34,6 % · 27,8 | 30,8 % · 24,7 | 34,6 % · 27,8 | 5,5 | 0,0 |

(part du fil · ms/image)

- **Aucun jeu n'attend** : tout est temps du G4 émulé. Le plugin pèse 13 à 29 ms/image.
- **Plugin, DOOM 3** (sampleplug, tour 1) : `gldUpdateDispatch`/`pomppc_geom_dispatch` (verdict)
  ~7-8 ms, `geom_render_array`/`geom_draw_client` (empaquetage) ~8-9 ms, état 1,4 ms.
  Colin McRae : même partage, plus `prog_sync` 2,9 ms. Nexuiz : verdict 7 ms, empaquetage 7 ms,
  `flush` 3,5 ms (les appels au kext : 4,7-5,5 ms/image).
- **GLEngine, Nexuiz : 25 ms/image**, dont ~17 % du fil dans du code **sans symbole** de GLEngine
  (`0x3093844`, `0x3169444`…) : les fonctions de sommets que GLEngine génère
  (`gleBuildVertexFunc*`) pour dérouler les tableaux clients en Begin/End.
- **`sqrt` : 8,5-9 % de Nexuiz, 4-5 % de Prey** (voir plus bas).
- Jeu, DOOM 3 : `idSIMD_AltiVec::*` (Dot, DeriveTangents, NormalizeTangents) ~12 %, culling ;
  Prey : `idSIMD_Generic::*` (squelettes : TransformVertsAndTangents, TransformJoints) ~13 %.

## Le processus QEMU (tour 2)

Les deux vCPU se partagent le fil du jeu, que le noyau de Tiger déplace de l'un à l'autre
(52 % + 55 % pour DOOM 3, 38 + 67 % Prey, 46 + 61 % Colin McRae, 33 + 67 % Nexuiz : total ≈ un
cœur) ; **presque un vCPU entier reste libre** pour un second fil.

Du temps occupé des vCPU, **environ deux tiers sont le code généré lui-même**, un quart les
helpers. Fonctions appelées directement depuis le code généré (part du temps vCPU occupé) :

| poste | DOOM 3 | Prey | Colin McRae | Nexuiz ARB |
|---|---|---|---|---|
| `helper_lookup_tb_ptr` (sauts indirects) | 4,1 % | 4,7 % | **8,3 %** | 4,4 % |
| `helper_lmw` + `helper_stmw` | 5,4 % | 4,4 % | **8,7 %** | 6,5 % |
| `helper_ldul_mmu` (chargement lent) | 2,2 % | 2,4 % | 2,2 % | 1,7 % |
| `mftb` (`helper_load_tbu`, `cpu_ppc_load_tbl`) | 2,0 % | 1,7 % | 2,2 % | **3,3 %** |
| AltiVec (`vmaddfp`, `vaddfp`, `vsldoi`…) | **6,6 %** | — | — | — |
| flottant **double** (`FMADD`, `FNMSUB`, `FSUB`, `FADD`, `frsp`, FPRF) | ~1 % | ~4 % | ~2 % | **~7 %** |
| `helper_store_dbatu` | — | — | 2,3 % | — |

`x-fp-native` (0014) ne couvre que le flottant **simple** ; le double reste en helpers.

## Étape 1 : le moteur multifil d'Apple

Absent de Tiger 10.4.6 (`docs/re/moteur-multifil.md`) : `CGLEnable(313)` →
`kCGLBadEnumeration`, GLEngine ne crée aucun fil.

## Trouvé en route : `sqrt` par `fsqrt`

`_sqrt` de libSystem lit `_cpu_capabilities` (0xFFFF8020) : `kHasFsqrt` (0x20000000, G5
seulement) levé → l'instruction `fsqrt`, sinon `___sqrt`, racine logicielle. QEMU exécute `fsqrt`
sur son G4. `tools/guest/jobs/sqrttest` (dans l'invité) : **0 écart sur 2 300 011 valeurs**
(spéciales, carrés exacts et voisins, aléatoires), **228 ns par appel contre 22,8 ns**. Seuls les
drapeaux FPSCR des cas invalides diffèrent (négatif : `fsqrt` lève FX, VXSQRT et FPRF), ceux
d'un vrai G5. `sqrtf` appelle `sqrt` puis `frsp` : couvert aussi.

`kext/POMPPCFsqrt` lève le bit dans `__cpu_capabilities` et dans la commpage
(`_commPagePtr32 + 0x20`), refuse si les deux diffèrent, le rebaisse au déchargement
(`securelevel = 1` interdit `/dev/kmem` ; `com.apple.kernel` exige sa version exacte, 8.6.0).
Chargé, `sqrt()` de la libm passe de 218 à 25,5 ns.

**Mesuré** (`tools/matrice/ab-fsqrt.sh 3 nx,prey fen`, entrelacé, hôte au repos,
`bench/vitesse/ab-fsqrt`) :

| jeu (fenêtre) | sans kext | avec | écart |
|---|---|---|---|
| Prey | 56,7 / 57,3 / 58,1 | 46,7 / 47,7 / 47,3 | **−17,5 %** (médianes) |
| Nexuiz ARB | 80,0 / 80,8 / 80,5 | 71,4 / 71,7 / 71,4 | **−11,3 %** |

Plus que la part de `sqrt` dans le `sample` de l'invité (4-5 % pour Prey) : la racine logicielle
n'est pas toute sous le symbole `sqrt`. Matrice complète kext chargé
(`bench/matrice/20261001-fsqrt`, Nexuiz rejoué en `nx-bis`/`nxg-bis` après un redémarrage qui
avait perdu le kext) : 15 vertes sur 15, images justes ; DOOM 3, UT2004, Warcraft III, Marble
Blast inchangés, Nexuiz GLSL 35,3 → 33,4.

## Lecture pour la suite

1. **`fsqrt`** : le plus petit lot, gain sûr sur Nexuiz et Prey ; A/B puis installation du kext.
2. **Plugin** : 13-29 ms/image, deux postes par jeu (verdict, empaquetage). Un fil de travail
   dans le plugin trouverait un vCPU libre (≈ un cœur inoccupé) ; ce qu'il pourrait prendre
   sans copie d'état reste à délimiter : le verdict lit GLEngine au moment de l'appel,
   l'empaquetage lit les tableaux du jeu, qui peuvent changer aussitôt après.
3. **Nexuiz : les fonctions de sommets générées par GLEngine** (~13 ms/image) : suite A4
   `gldCreateVertexArray`.
4. **TCG** : flottant double natif (Nexuiz ~7 %, Prey ~4 %), sur le modèle de 0014 ; AltiVec
   natif (DOOM 3 ~6,6 %) ; `mftb` en ligne (2-3 %) ; sauts indirects de Colin McRae (8,3 %).
