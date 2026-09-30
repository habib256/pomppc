# Backend GL : le temps que l'invité passe à attendre l'hôte — G8, G9, Colin McRae (30/09/2026)

Suite de `docs/protocole-v23-textures.md` (A4, volet textures). Le rendu hôte tourne sur son
propre fil (`qgpu-render`), en parallèle du vCPU : un gain côté hôte ne compte que s'il réduit
**l'attente de l'invité** (colonne `wait_ms` de `frames.csv`). Plafond par jeu = attente par image
/ ms par image. Règle du projet : mesurer avant d'optimiser, ne construire que ce qui vaut plus
de ~2 % du temps d'image d'au moins un jeu.

**Verdict.**
- **Colin McRae n'attend pas l'hôte** : 0,12 ms par image affichée (0,2 %). Les « 10 %, ~7 ms
  par image » venaient d'une fenêtre de mesure tombée dans un menu (§1).
- **Seul Nexuiz dépasse 2 %** : GLSL 2,5 ms/image (6,2 %), ARB 3,6 ms/image (3,3 %). L'invité y
  attend ses **requêtes d'occlusion** : `q_info` soumet sa moitié et attend la barrière. Le temps
  attendu, c'est le rendu, par l'hôte puis par le GPU, des ~430 lots soumis avec la première
  requête (§2).
- **G9 (cache d'état) : classé**. Poser l'état coûte 15 à 21 % du temps hôte. Sur l'attente, cela
  fait au plus 0,5 ms/image (Nexuiz GLSL, 1,25 %), et moins de 1 % partout ailleurs (§4).
- **G8 (PBO pour `SURF_PRESENT`) : classé**. La présentation n'est pas sur le chemin d'attente de
  Nexuiz. Sautée, son attente du GPU passe à la requête suivante (§4).
- **Construit** (hôte seulement, sans changement de protocole) :
  - `QGPU_GL_FLUSH=1`, un `glFlush` en fin de soumission, éteint par défaut (§5) ;
  - `QGPU_PRESENT_SKIP=N`, l'épreuve de G8 ;
  - le kit d'A/B en VM `tools/matrice/ab-attente.sh` (§6).
- **Le gisement est côté plugin** : soumettre la moitié courante, sans l'attendre, au premier
  `QUERY_BEGIN`. Modèle du rejeu, avec le flush de l'hôte : attente de Nexuiz GLSL 2,7 → 0,45
  ms/image (~5,5 %), ARB 1,7 → 0,4 (§7). Pas de changement de protocole.
- **Construit, mesuré en VM, gain faible** (§9) : `POMPPC_GL_QFLUSH=1`, éteint par défaut. Les
  comptes d'occlusion sont exacts et les images justes. Avec `QGPU_GL_FLUSH=1`, l'attente des
  lectures de Nexuiz GLSL passe de 2,5 à ~2,0 ms/image, soit **−0,5 ms/image (~1 %)**, et non
  −2,3. Le modèle supposait que l'invité mettait 2 ms à émettre ses requêtes. En VM, il met
  **0,4 ms** entre la coupe et la première lecture : l'hôte n'a que ce temps d'avance.

## 1. L'attente de l'invité, par jeu (VM)

La mesure porte sur la fenêtre de la matrice (`resultats.csv`), par image **affichée**.
Tours `20260930-wlunif`, `20260930-0128`, `bench/a4/depart/tours`, avec
`python3 tools/re/attente.py <tour>` :

| Cellule | ms/image | attente ms/image | part | lots/image |
|---|---:|---:|---:|---:|
| Nexuiz GLSL fen / pe | 40–42 | 2,49–2,60 | **6,0–6,4 %** | 320 |
| Nexuiz ARB fen / pe | 109–111 | 3,63–3,71 | **3,3–3,4 %** | 545 |
| DOOM 3 fen / pe | 60–62 | 0,75–0,77 | 1,2–1,3 % | (natif) |
| Prey fen / pe | 70–71 | 0,75–0,81 | 1,1 % | 4 |
| Marble Blast | 9–10 | 0,08–0,12 | 0,8–1,2 % | 35 |
| Zenerchi | 4–4,5 | 0,03 | 0,6–0,7 % | 15 |
| Warcraft III | 18 | 0,06–0,07 | 0,3–0,4 % | 109 |
| UT2004 | 26–27 | 0,05–0,07 | 0,2–0,3 % | — |
| **Colin McRae** pe | 69–74 | **0,12–0,13** | **0,2 %** | 750 (3 échanges) |

**Colin McRae recalé.** `tools/re/a4attente.py` prend le contexte qui échange le plus, puis les
600 échanges qui précèdent le premier trou d'une seconde. Chez Colin McRae, ces 600 échanges
tombent dans un **menu** : images 41694..42294 du tour `wlunif`, contexte `0x302f800` seul, 2 ms
par échange, 10,2 % d'attente. Le « ~7 ms par image » de la note v23 était 10 % × 69 ms.
Sur la fenêtre de la course (57476..58076, trois échanges par image), l'attente vaut 0,117
ms/image. Les tours A4 de départ disent de même : 0,123 ms/image, contre les « 11 à 21 % » du
même script. `a4attente.py` porte désormais cet avertissement. `tools/re/attente.py` prend la
fenêtre de la matrice et le contexte affiché : parmi les contextes qui échangent au moins 20 %
autant que le plus bavard, celui qui échange le moins.

## 2. Où l'invité attend

Le plugin n'attend l'hôte qu'à trois endroits (`guest/gldriver/pomppc_accel.c`) :

1. **`switch_half`** : la moitié reprise est encore en vol, et l'on attend la soumission
   d'avant.
2. **`present_direct`** : à l'échange, attente de la moitié précédente (une image de latence,
   v9).
3. **`q_info`** (requête d'occlusion, `glGetQueryObject*`) : `flush()` puis `wait_half` de la
   moitié **qu'il vient de soumettre**. C'est une attente synchrone de tout ce qui a été soumis
   jusque-là, GPU compris : `glGetQueryObjectuiv(GL_QUERY_RESULT)` bloque côté hôte.

**Suite des soumissions** (`qseq.py` du scratchpad, image de régime ; `dN` = N lots) :

```
Nexuiz GLSL : [PRES] [CLR d285] [d431 (QB d1 QE)×14 QR] [QR]×13 [d35]
DOOM 3      : [d70 CPTEX×3 d20 PRES] [CLR CLR d151 CLR d9] … [d16 CLR d471]      (8 soumissions)
Prey        : [d77 … d20 PRES] [CLR CLR d38 … CPTEX …] [d29 … d93]
Warcraft III: [d55 PRES] [CLR d33]
Colin McRae : [PRES] [CLR CLR] [] [] [] [d111] [] [] [] [d34 CLR CLR] … [d4 CLR SURFTEX] …
```

DarkPlaces pose ses requêtes de halos (deux par lumière) juste avant d'en lire les résultats.
Le premier `QUERY_RESULT` part donc avec une moitié de ~430 lots, que l'hôte doit exécuter,
puis le GPU rendre, avant de répondre. Les 13 suivants font chacun un aller-retour.

**Le modèle du rejeu** (ligne « sortes » de `qgpu_replay`, §8) : le temps hôte des soumissions
qui portent un `QUERY_RESULT` estime ce que l'invité y attend quand l'hôte était à jour à leur
arrivée.

| Cellule | soumissions « requête » (rejeu) | attente mesurée (VM) |
|---|---:|---:|
| Nexuiz GLSL fen | 2,3–2,8 ms/image | 2,5 ms/image |
| Nexuiz ARB fen | 1,6–1,8 ms/image | 3,6 ms/image |

GLSL est expliqué en entier. ARB l'est pour moitié : le reste vient de `switch_half` et de
`present_direct` sur ses 4,7 autres soumissions par image, que le rejeu ne peut pas situer.
DOOM 3 (0,76 ms/image) et Prey (0,8) n'ont pas de requête : leur attente est celle de
`switch_half` et `present_direct`, 1,1–1,3 % au plus.

**Ce que le rejeu prouve et ne prouve pas.** Il exécute exactement les soumissions du vidage,
sur le même cœur, le même backend et le même GPU que le device. Le temps hôte par soumission,
par opcode et par appel GL y est donc exact, et l'on sait ce que contient chaque soumission
attendue. Il ne reproduit pas la cadence de l'invité : il enchaîne les soumissions sans les
pauses du vCPU (`QGPU_REPLAY_GAP_US` en met une, fixe). Il ne peut donc pas dire si l'hôte
était en retard quand l'invité a attendu, ni combien d'attente vient de `switch_half` et de
`present_direct`. Seule la VM tranche (§6).

## 3. Temps hôte par image et sa répartition (rejeu natif)

Rejeu de régime (hors première image du vidage), `-O2`. Hôte chargé : deux VM d'autres agents,
charge 6–7 ; les écarts se lisent en entrelacé (§5).

| Cellule | fil de rendu ms/image | lots/image | µs par lot | `SURF_PRESENT` ms | requêtes ms/image |
|---|---:|---:|---:|---:|---:|
| Nexuiz GLSL fen | 6,2–6,4 | 581 (307 bruts + 274 natifs) | 6,4 / 8,5 | 0,86–0,91 | 0,67 (6,7 × 100 µs) |
| Nexuiz ARB fen | 7,3–7,5 | 955 | 8,2 / 4,1 | 0,92–0,95 | 0,78 (6,2 × 125 µs) |
| DOOM 3 fen | 10,9–11,1 | 1 246 natifs | 6,8 | 1,8 | — |
| Prey fen | 2,4 | — | — | 1,0 | — |
| UT2004 pe | 3,4 | — | — | 1,4 | — |
| Warcraft III pe | 2,5 | — | — | 1,3 | — |
| Colin McRae pe | 2,4–2,6 par échange | 247 bruts | 17 | 1,4–1,6 | — |
| Marble Blast pe | 1,5 | — | — | 1,1 | — |
| Zenerchi fen | 1,0 | — | — | 0,85 | — |

**Par opcode** (`QGPU_REPLAY_OPTIME=1`) : les dessins font 67 à 85 % du temps. Viennent ensuite
`SURF_PRESENT` (8 à 17 %), `QUERY_RESULT` pour Nexuiz (10 %), `COPY_TEX` pour DOOM 3 (3 %),
`TEX_IMAGE3` (2 à 5 %, premiers envois du vidage) et `GLSL_LINK` (3 %, programmes réémis par
le vidage). `SET_STATE`, `SET_MATRIX` et `PROG_*` coûtent moins de 1 %.

**Par appel** (`sample` sur `QGPU_REPLAY_REPEAT`, `-fno-inline`, part du temps de
`qgpu_core_execute`) :

| Poste | Nexuiz GLSL | DOOM 3 | Colin McRae |
|---|---:|---:|---:|
| dessin dans le pilote (`glDraw*` : validation, copie des sommets clients) | 23 % | 27 % | 26 % |
| **poser l'état** (`gl_target`, `gl_reset_raw`, lumières, texgen, unités, appels directs de `gl_draw_raw`) | **16 %** | **16 %** | **15 %** |
| relecture de présentation (`glReadPixels`, surtout l'attente du GPU) | 10 % | 12 % | 16 % |
| requêtes (`glGetQueryObject`, attente du GPU) | 7 % | — | — |
| conversion des attributs natifs par le cœur (`nat_conv_attr`) | — | **27 %** | — |
| textures (premiers envois du vidage), compilation | 8 % | < 1 % | 5 % |
| entrée de commande (`CGLSetCurrentContext` + `glGetError`) | 0,4 % | 0,3 % | 0,2 % |

À `-O2`, poser l'état monte à 20–21 % : les aides sont inlinées dans `gl_draw_raw`. Chaque
dessin brut émet de l'ordre de 300 appels GL. `gl_target` en fait ~90 (8 unités × 5 cibles
coupées, tests, matrices). `gl_reset_raw` en fait ~150 : 8 lumières, 6 plans, 8 unités × texgen,
cibles et matrice de texture. `gl_draw_raw` en fait ~60–100. La plupart reposent la même valeur
d'un dessin à l'autre.

## 4. Ce qui est classé, avec les chiffres

- **G9, cache d'état dans `gl_target`** : on supprimerait au mieux les 15–21 % du temps hôte
  passés à poser l'état. Ce n'est qu'une borne haute : les matrices, les pointeurs et les
  unités changent vraiment. La validation du pilote dans `glDraw*` baisserait peut-être aussi,
  mais le rejeu ne peut pas la séparer. Sur l'attente, au mieux 20 % d'elle :

  | Jeu | gain au plus |
  |---|---:|
  | Nexuiz GLSL | 0,5 ms/image (1,25 %) |
  | Nexuiz ARB | 0,7 ms/image (0,7 %) |
  | DOOM 3 | 0,15 ms/image (0,25 %) |
  | autres | < 0,03 ms/image |

  Moins de 2 % partout. Si le plugin soumet ses requêtes plus tôt (§7), les dessins de Nexuiz
  sortent même du chemin d'attente, et le gain tombe à ~0. Il y a aussi un risque : un état
  modifié hors du cache (contexte, surface, `SURF_TEX`/`COPY_TEX`, programmes, reset de client)
  donnerait une image fausse. **Mesuré, rien à construire.**
- **G8, PBO en rotation pour `SURF_PRESENT`** : à Nexuiz, la soumission `PRES` part seule, et
  l'invité ne la reprend qu'après avoir rempli la moitié suivante (285 lots) : elle n'est pas
  sur son chemin d'attente. Chez Colin McRae, l'attente totale vaut 0,12 ms/image, sous le seuil
  de 1 ms fixé pour G8. En rejeu, `QGPU_PRESENT_SKIP=8` fait tomber le coût de `SURF_PRESENT` de
  20,3 à 2,4 ms sur 24 images de Nexuiz GLSL. L'attente du GPU passe alors à la requête suivante
  (soumissions « requête » 2,48 → 2,74 ms/image) : un PBO ne l'enlèverait pas, il la
  déplacerait. **Classé**, sous réserve de l'épreuve en VM (`ab-attente.sh … g8`).
- **G7** : clos par la note v23.
- **DOOM 3 : `nat_conv_attr`**, 27 % du temps hôte (le cœur reconvertit les attributs des
  tampons hôtes à chaque dessin natif), est hors de ce chantier (cœur) et hors du chemin
  d'attente : 0,76 ms/image d'attente, 1,3 % au plus. À noter pour un lot du cœur :
  convertir une fois, au `BUF_SUBDATA`.

## 5. Construit : `glFlush` en fin de soumission (`QGPU_GL_FLUSH=1`)

Le pilote d'Apple (OpenGL sur Metal) garde le travail encodé jusqu'à la synchronisation
suivante : relecture de présentation, résultat de requête, copie. Celle-ci paie alors tout le
rendu de l'image au GPU, en série. D'où les 1 à 2,4 ms d'un `SURF_PRESENT` relevés par A4 : c'est
l'attente du GPU, pas la relecture.

- `QgpuBackend.submit_end` et `qgpu_core_submit_end` (cœur, 7 lignes) ; `gl_submit_end` fait
  `glFlush`.
- Le device l'appelle à la fin de `qgpu_run_job`, **après** la barrière, le marquage de BAR0 et
  l'IRQ : l'invité n'attend pas ce `glFlush`. Le rejeu l'appelle après chaque soumission.
- **Éteint par défaut** (`QGPU_GL_FLUSH=1` pour l'allumer). Chaque `glFlush` coupe la passe de
  rendu de Metal, et l'arrondi d'un pixel mélangé de part et d'autre de la coupure peut varier
  d'une unité (voir la preuve d'images plus bas).

**Rejeu entrelacé** : 5 passes par mode, `QGPU_REPLAY_REPEAT=3`, pause de 1 ms entre les
soumissions. Hôte chargé (2 autres VM, charge 6–7) ; fichier `mes_gap1000.txt`. Médianes :

| Cellule | fil de rendu ms/image | soumissions « requête » | soumission « présentation » | `SURF_PRESENT` ms |
|---|---:|---:|---:|---:|
| Nexuiz GLSL fen | 6,39 → 6,23 | **2,49 → 2,32** | 0,91 → 0,76 | 0,91 → 0,76 |
| Nexuiz ARB fen | 7,42 → 7,27 | **1,64 → 1,34** | 0,92 → 0,71 | 0,92 → 0,71 |
| DOOM 3 fen | 11,11 → 10,84 | — | **2,63 → 1,61** | **1,82 → 0,87** |
| Colin McRae pe | 2,45 → 2,37 | — | 0,86 → 0,74 | 1,56 → 1,21 |
| Prey fen | 2,42 → 2,39 | — | 1,66 → 1,57 | 1,02 → 0,91 |
| Warcraft III pe | 2,47 → 2,42 | — | 1,84 → 1,76 | 1,30 → 1,22 |
| UT2004 pe | 3,36 → 3,36 | — | 3,36 → 3,35 | 1,39 → 1,38 |
| Zenerchi fen | 1,01 → 0,98 | — | 1,01 → 0,98 | 0,85 → 0,84 |
| Marble Blast pe | 1,53 → 1,57 | — | 1,50 → 1,54 | 1,12 → 1,13 |

Le temps du fil de rendu comprend les `glFlush` (DOOM 3 : 0,64 ms/image, qui s'échangent
contre 0,95 ms de `SURF_PRESENT` en moins). **Sur l'attente, on peut espérer au mieux :**

| Jeu | gain | part |
|---|---:|---:|
| Nexuiz GLSL | 0,17 ms/image | 0,4 % |
| Nexuiz ARB | 0,3 ms/image | 0,3 % |
| DOOM 3 | ≤ 0,76 ms/image (si sa soumission de présentation est sur le chemin) | ≤ 1,3 % |

Seul, c'est sous les 2 %. Ce qui fait construire ce lot, c'est son rôle de prérequis, bon
marché, de la soumission anticipée des requêtes (§7) ; c'est aussi la forme sans risque de ce
que G8 visait (`SURF_PRESENT` divisé par deux, sans différer la présentation).

**Images.** Rejeux des 45 vidages : tours `wlunif` et `0128`, références `bench/matrice/ref`.
L'ancien et le nouveau rejeu sont construits à `-O1` et comparés octet à octet ; script
`images.sh` du scratchpad.

- **Éteint (défaut)** : 1 277 images, **0 différente** après relance. Une première passe avait
  montré 1 écart sur la référence de Colin McRae ; l'ancien rejeu contre lui-même en fait autant
  dans **6 rejeux sur 40** (10 pixels, ±1–2, image `f62507`). C'est un non-déterminisme du
  pilote d'Apple, qui existait avant ce lot.
- **Allumé** : 8 images sur 1 277 diffèrent, toutes de ±1 : Prey `0128` (1 image en fenêtre,
  4 en plein écran), références de Prey (2) et de Colin McRae (1). Prey plein écran `f1036` a
  45 pixels écartés ; les autres images en ont 1. Les écarts sont déterministes d'un rejeu à
  l'autre. Contre les références (`ppmcmp -r`), tout reste à 0,000.
- **Épreuves de tolérance** : la matrice admet 0,5 d'écart moyen et 0,1 % de pixels contre la
  référence, et 0,3 / 0,2 % contre la capture de la VM. Les écarts ci-dessus en sont loin.
  L'allumage par défaut attend quand même la matrice en VM (§6).

**Tests.** `qgpu_backend_test` F1 : la même scène (texture, requête d'occlusion, relecture) est
jouée d'un bloc, puis commande par commande avec une fin de soumission après chacune. Résultat :
0 pixel d'écart et le même compte d'occlusion, sur les deux backends. `tests/run-all.sh` : 176 OK.

## 6. Kit d'A/B en VM (non joué : la preuve finale n'est pas pour cet agent)

**QEMU** : clone APFS `~/src/qemu-backend`, où les 7 fichiers de `patches/qgpu/` de la branche
sont copiés dans `hw/display/`, puis `ninja -C build qemu-system-ppc qemu-system-ppc64`
(construit le 30/09, sans avertissement du device). Pour le refaire :

```
cp -c -R ~/src/qemu ~/src/qemu-backend
cp patches/qgpu/{qgpu-core.c,qgpu-core.h,qgpu-gl.c,qgpu-soft.c,qgpu-pci.c,qgpu_proto.h,qgpu_abi.h} \
   ~/src/qemu-backend/hw/display/
ninja -C ~/src/qemu-backend/build qemu-system-ppc qemu-system-ppc64
```

**Jouer** (VM quotidienne libre ; aucun changement de plugin ni de kext) :

```
tools/matrice/ab-attente.sh 6 "g8 flush"     # A/B entrelacés, ~6 × 2 × 6 parties
tools/matrice/ab-attente.sh 1 matrice        # tour complet sous QGPU_GL_FLUSH=1
```

Bilan : `bench/tcg/ab/attente-<horodatage>-bilan.txt`, qui donne les médianes de l'attente et
des ms/image par mode et par cellule.

**Critères :**

| Épreuve | Si… | Alors |
|---|---|---|
| g8 | l'attente baisse de moins de 1 ms/image sous `QGPU_PRESENT_SKIP=8` (prévu : < 0,2) | **G8 clos** |
| flush | l'attente baisse et les ms/image ne montent pas | `QGPU_GL_FLUSH=1` peut être allumé par défaut… |
| matrice | … et la matrice sous flush est à 16/16 | … effectivement allumé |

À retirer du tableau : les cellules rouges de l'épreuve g8, rouges par construction (image
figée 7 fois sur 8).

## 7. Ce qui reste : le gisement est dans le plugin

**Soumission anticipée des requêtes d'occlusion** (plugin seul, protocole inchangé) : au
**premier `QUERY_BEGIN`** après des lots, faire `flush()` sans attendre la moitié.

- L'hôte exécute alors les ~430 lots de Nexuiz pendant que l'invité émet ses 14 requêtes, et le
  `glFlush` de fin de soumission (§5) les envoie au GPU aussitôt.
- Au premier `QUERY_RESULT`, il ne reste que les 14 petits dessins de requête.

Modèle du rejeu (`QGPU_REPLAY_SPLITQ=1`, pause de 2 ms, 2 passes), soumissions « requête »,
en ms/image :

| Cellule | tel quel | coupe seule | flush seul | coupe + flush |
|---|---:|---:|---:|---:|
| Nexuiz GLSL | 2,70–2,84 | 0,73–0,78 | 2,56–2,61 | **0,45** |
| Nexuiz ARB | 1,67–1,76 | 0,85–0,88 | 1,38–1,39 | **0,40–0,43** |

Gain espéré : ~2,3 ms/image pour Nexuiz GLSL (≈ 5,5 % de 40 ms), ~1,3 ms/image pour ARB
(≈ 1,2 %). Les 13 allers-retours restants (un `flush` + `wait_half` par `glGetQueryObject`)
pèsent peu. Les regrouper voudrait un changement de protocole, par exemple une requête dont le
résultat est écrit par l'hôte à sa disponibilité, que l'invité lirait sans attendre la barrière.
Ce n'est pas nécessaire au gain ci-dessus.

Construit et mesuré en VM : §9. Le gain réel est quatre fois plus petit que ce modèle.

## 8. Reproduire

Scripts de la session dans le scratchpad (`rj.sh`, `mesure.sh`, `images.sh`, `qseq.py`,
`cat.py`, `g9.py`) ; l'essentiel :

```
python3 tools/re/attente.py bench/matrice/20260930-wlunif      # attente par image affichée
cc -std=gnu11 -O2 -pthread -w -I patches/qgpu tests/qgpu_replay.c patches/qgpu/qgpu-core.c \
   patches/qgpu/qgpu-soft.c patches/qgpu/qgpu-gl.c -framework OpenGL -lm -o /tmp/rj
D=bench/matrice/20260930-wlunif/nxg-fen/invite/dump
QGPU_REPLAY_OPTIME=1 /tmp/rj $D /tmp/r gl 2>&1 | grep -E 'régime|sortes|OPTIME'
QGPU_REPLAY_OPTIME=1 QGPU_REPLAY_GAP_US=2000 QGPU_REPLAY_SPLITQ=1 QGPU_GL_FLUSH=1 /tmp/rj $D /tmp/r gl 2>&1 | grep sortes
QGPU_REPLAY_REPEAT=60 /tmp/rj $D /tmp/r gl &  sample $! 8 1 -file /tmp/nxg.txt
```

Options du rejeu :

| Option | Effet |
|---|---|
| `QGPU_REPLAY_OPTIME=1` | temps par opcode (commandes exécutées une à une) |
| `QGPU_REPLAY_REPEAT=n` | rejoue n fois les images de régime, sans PPM |
| `QGPU_REPLAY_GAP_US=n` | pause entre deux soumissions |
| `QGPU_REPLAY_SPLITQ=1` | avec OPTIME : coupe au premier `QUERY_BEGIN` |

Ligne `sortes` : soumissions « requête », « présentation » et autres, en ms/image. Ligne
`fin de soumission` : temps total des `glFlush`.

## 9. Construit : la coupe au premier `QUERY_BEGIN` (`POMPPC_GL_QFLUSH`, plugin)

**Où l'invité attendait.** Le code confirme le §2. `q_info` fait `flush()`, puis `wait_half(i)`
sur la moitié qu'il vient de soumettre. `flush()` passe par `switch_half`, qui attend d'abord
l'autre moitié, celle de la soumission précédente. La première lecture d'une image attend donc
toute la moitié qui porte les ~320–430 lots et les requêtes : l'hôte doit les exécuter, puis le
GPU les rendre (`glGetQueryObjectuiv` bloque). Les lectures suivantes font chacune un
aller-retour. Les compteurs le confirment en VM : Nexuiz GLSL fait **~4,6 requêtes par image**
(démo 1, pas 14 ; 2 310 `QUERY_BEGIN` sur 500 images), en **un seul groupe** (461 premières
lectures sur 500 images). La première lecture pèse **90 %** de l'attente des lectures
(2,25 ms sur 2,5).

**Ce que fait la coupe** (`q_begin`, section des requêtes d'occlusion) :

- elle s'active au **premier** `QUERY_BEGIN` d'une soumission, si la moitié courante porte au
  moins `QCUT_MIN` = 256 mots de flux (`POMPPC_GL_QFLUSH=<n>`, n > 1, change le seuil) ;
- elle fait alors `flush()` **avant** d'émettre la requête, sans attendre la barrière de la
  moitié soumise ;
- l'attente de `switch_half` porte sur la soumission d'avant, que l'hôte exécute de toute façon
  en premier (la file est dans l'ordre) : elle était déjà comprise dans l'attente de `q_info` ;
- la variable est lue une fois (`qcut_init`), sans `getenv` dans le chemin chaud ; protocole,
  device et kext sont inchangés.

**Cas traités** (commentaire en tête de la section) :

- **`QUERY_BEGIN` répétés.** `qcut_seq` retient la soumission (`G.sub_seq`) qui porte déjà un
  `QUERY_BEGIN`. Il est relevé **après** le `reserve` de la requête, donc sur la moitié qui la
  porte, même si `reserve` a vidé une moitié pleine. On ne coupe jamais entre les requêtes d'un
  groupe : 2 310 `QUERY_BEGIN`, 447 coupes, 461 premières lectures.
- **Requête ouverte à cheval.** La coupe précède le `QUERY_END` implicite de `q_begin`. Une
  requête qui court d'une soumission à l'autre existait déjà (moitié pleine), et le cœur la tient
  par contexte.
- **Octets de BAR0.** `flush()` est le geste ordinaire : arène, sommets et indices restent dans
  la moitié soumise, en vol jusqu'à sa barrière (architecture §3).
- **`BeginPrimitiveBuffer` ouvert (`G.npend`), mode synchrone, une seule moitié, hôte mort.**
  Pas de coupe : `flush()` y soumettrait en synchrone. Le cas est compté « non coupé »
  (4 à 9 sur 500 images).
- **`G.mu` relâché** par l'attente de `switch_half` : le contexte est recherché de nouveau.
- **Vidage.** La coupe ajoute une soumission par image, vidée comme les autres. Le rejeu joue
  des soumissions, il ne suppose rien de leur découpage. `QGPU_REPLAY_SPLITQ` ne coupe pas en
  tête de soumission (`q && …`). Images justes ci-dessous.
- **Autres lecteurs synchrones** (`sync_to_sw_locked`, `present_direct`) : inchangés.

**Compteurs.** Ligne `QRY` de la note, toutes les 500 images, plus une dernière à la sortie
avec `POMPPC_GL_STATS` pour les processus courts comme `gltest`. Elle donne :

- les `QUERY_BEGIN`, les coupes et leur attente, les requêtes non coupées ;
- les lectures et leur attente, dont les **premières** (celles qui soumettent des
  `QUERY_BEGIN`) ;
- l'**avance de la coupe** : le temps de l'invité entre la coupe et la première lecture ;
- l'attente totale (`G.t_wait`).

La ligne sort dès qu'une requête a été vue ou que la coupe est allumée.

### Épreuves (instance sur recouvrement de `tiger-endurance.qcow2`, QEMU `~/src/qemu-backend`)

L'hôte était chargé pendant toute la mesure : charge 4,5 à 8,9, deux ou trois autres QEMU.
Nexuiz GLSL tourne à 55–60 ms/image au lieu de 41 : les cellules sont rouges **par le
plancher de vitesse seul**. Les ms/image et l'attente de `attente.py` (fenêtre 120..600) sont
dans le bruit (±0,5 ms d'une partie à l'autre). La preuve s'appuie sur les lignes `QRY`,
images 500..1000.

- **`tests/run-all.sh`** : 176 OK.
- **`gltest occl`** : comptes **exacts** (1024 / 512 / 0) sous `POMPPC_GL_QFLUSH` = 0, 1 et 2
  (seuil de 2 mots). Ils le restent avec et sans `QGPU_GL_FLUSH=1`. Il y a une coupe par
  lancement, et une soumission de plus (11 contre 10). Même chose pour `qprobe` (2016
  échantillons, disponible 1) et pour `game`, qui n'a pas de requête : 0 `QUERY_BEGIN`,
  0 coupe, 65 soumissions dans les deux cas.
- **Images** : matrice avec vidage sur l'instance (`MATRICE_MON`, `TSSH_PORT`, `MATRICE_BIN`).
  Nexuiz GLSL et ARB en fenêtre, coupe éteinte et allumée, avec et sans `QGPU_GL_FLUSH=1` :
  **juste**, rejeu contre capture 0,00/0,00 %, rejeu de la référence 0,00/0,00 % (validée).
- **Jeu sans requêtes** (Marble Blast, fenêtre, coupe allumée) : ligne `QRY` à 0 `QUERY_BEGIN`,
  0 coupe, 0 lecture. Les replis varient d'une partie à l'autre (40, 60, 526) : c'est la mise
  au premier plan de l'instance sans fenêtre, que la coupe ne touche pas (aucun `QUERY_BEGIN`).
  17,0 ms/image avec la coupe, 16,6 sans.

**Attente des lectures, Nexuiz GLSL fenêtre**, images 500..1000, ms par image (lignes `QRY`) :

| QEMU | coupe | parties | lectures | dont premières | avance de la coupe |
|---|---|---|---:|---:|---:|
| `QGPU_GL_FLUSH` éteint | éteinte | 3 | 2,92 / 2,62 / 2,69 | 2,40 / 2,52 | — |
| `QGPU_GL_FLUSH` éteint | allumée | 3 | 2,59 / 2,10 / 2,77 | 1,96 / 2,46 | — |
| `QGPU_GL_FLUSH=1` | éteinte | 2 | **2,50 / 2,54** | 2,25 / 2,27 | — |
| `QGPU_GL_FLUSH=1` | allumée | 4 | **1,93 / 2,14 / 1,97 / 1,98** | 1,72 / 1,87 / 1,71 / 1,71 | **0,39** |

Nexuiz ARB, `QGPU_GL_FLUSH=1`, lectures : images 0..500, 1,16 → 0,87 ms/image ; au-delà,
2,39 → 1,49, mais les fenêtres diffèrent (274 contre 500 images). Sans le flush, images 0..500 :
1,62 → 1,31.

**Lecture.**

- **Avec `QGPU_GL_FLUSH=1`**, la coupe enlève ~0,55 ms/image de l'attente de Nexuiz GLSL (les
  quatre parties coupées sont toutes sous les deux témoins), soit **~1 % du temps d'image**,
  sous le seuil de 2 %.
- **Sans le flush**, l'écart (~0,25) est dans le bruit : le pilote d'Apple garde le travail
  encodé jusqu'à la lecture, et le GPU ne démarre de toute façon qu'à ce moment-là (§5).
- **Pourquoi pas les 2,3 ms du modèle.** L'avance mesurée est de **0,39 ms/image**, soit
  ~0,4 ms par coupe : DarkPlaces émet ses ~4,6 requêtes et lit la première en 0,4 ms de temps
  invité. L'hôte ne gagne donc que ces 0,4 ms sur les ~2,3 ms que lui coûtent la moitié
  (encodage de ~320 lots, puis GPU). Le rejeu supposait 2 ms de pause (`QGPU_REPLAY_GAP_US=2000`)
  ; la VM en donne cinq fois moins. Le gain observé (~0,55) est de l'ordre de cette avance,
  plus le flush qui fait démarrer le GPU plus tôt.

**Verdict.** La coupe est **construite, exacte et sans risque mesuré**, avec un gain **faible**
(≈ 1 % sur Nexuiz GLSL avec `QGPU_GL_FLUSH=1`, dans le bruit sans lui). Elle reste **éteinte
par défaut**. L'A/B entrelacé sur la VM quotidienne (`ab-attente.sh … qflush`, §6 et
ci-dessous) doit trancher : elle ne mérite d'être allumée que s'il confirme ≥ 1 ms/image, ce que
ces chiffres ne laissent pas prévoir.

**Ce qui reste, si l'on veut les ~2 ms.** Il faut que l'hôte ait travaillé **avant** les
requêtes, donc pendant les ~320 dessins eux-mêmes. Deux pistes :

- **soumettre par tranches** pendant l'image (flush tous les N lots) : c'est le chemin de la
  géométrie (`reserve`, `geom_*`), aujourd'hui en travaux par un autre agent ;
- **des résultats de requête écrits par l'hôte à leur disponibilité**, lus sans barrière :
  c'est un changement de protocole (§7).

Les deux sortent de ce lot.

### Kit d'A/B (VM quotidienne, non joué ici)

Deux épreuves ajoutées à `tools/matrice/ab-attente.sh`. Prérequis : le plugin de la branche,
installé dans la VM quotidienne par `NORUN=1 tools/guest/cycle.sh`, depuis son worktree.
Éteint, il se comporte comme celui de main. Le kit vérifie par `strings` que le plugin
installé connaît `POMPPC_GL_QFLUSH`, et saute l'épreuve sinon.

```
tools/matrice/ab-attente.sh 6 qflush       # 5 campagnes A/B entrelacées (~5 × 12 parties)
tools/matrice/ab-attente.sh 1 matriceq     # tour complet sous POMPPC_GL_QFLUSH=1 (+ QGPU_GL_FLUSH=1 ; QMAT_FLUSH=0 sans)
```

`qflush` compare à la référence :

- `q` (`POMPPC_GL_QFLUSH=1`) sur Nexuiz GLSL et ARB fenêtre ;
- `qf` (`POMPPC_GL_QFLUSH=1 QGPU_GL_FLUSH=1`) sur les mêmes ;
- `q` sur Marble Blast, témoin sans requêtes.

Il relève ensuite la dernière ligne `QRY` de chaque partie dans le bilan. `tools/tcg/matab.sh`
passe désormais les variables `POMPPC_GL_*` d'un mode au jeu (`matrice.py --env`) ;
auparavant, elles n'atteignaient que QEMU.

| Épreuve | Si… | Alors |
|---|---|---|
| qflush | l'attente médiane de nxg baisse de ≥ 1 ms/image (sous `q` ou `qf`), ses ms/image ne montent pas, nx et mb ne se dégradent pas, `QRY` de mb à 0 coupe | la coupe peut être allumée par défaut… |
| matriceq | … et le tour est à 16/16 | … effectivement allumée (et `QGPU_GL_FLUSH=1` avec elle si c'est `qf` qui gagne) |
| qflush | la baisse est < 1 ms/image (prévu ici : ~0,5 avec `qf`, ~0 avec `q`) | **coupe close « mesuré, gain faible »** : on la laisse éteinte, le code reste pour l'étude |

### Reproduire sur une instance à soi

Recouvrement :

```
qemu-img create -f qcow2 -b disks/tiger-endurance.qcow2 -F qcow2 .run/a4req/disque.qcow2
```

Lancement, depuis le worktree :

```
HEADLESS=1 TABLET=1 WEBPROXY=0 GLISO=0 POMPPC_AUDIO_PROFILE=muet SSH_FWD=2290 \
  POMPPC_SCRATCH=$PWD/../../.run/a4req DISK=…/disque.qcow2 \
  QEMU_BIN=~/src/qemu-backend/build/qemu-system-ppc [QGPU_GL_FLUSH=1] ./run_tiger.sh
```

Le port réel est dans `.run/a4req/tiger.sshport` (2291 ici). Ensuite :

```
TSSH_PORT=2291 NORUN=1 tools/guest/cycle.sh
MATRICE_MON=.run/a4req/mon.sock TSSH_PORT=2291 MATRICE_BIN=bench/a4req/bin \
  python3 tools/matrice/matrice.py -j nxg -m fen --sans-vidage --env POMPPC_GL_QFLUSH=1 --sortie …
grep '^QRY' <cellule>/mesure/note.txt
```

`MATRICE_BIN` (nouveau) garde les outils de la matrice hors de `bench/matrice/bin`, qui est
partagé. Relevés : `bench/a4req/p1..p5/` (hors dépôt).
