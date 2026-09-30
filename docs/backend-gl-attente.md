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
  ms/image (~5,5 %), ARB 1,7 → 0,4 (§7). Pas de changement de protocole. Non construit : le
  plugin est en travaux (A4).

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
