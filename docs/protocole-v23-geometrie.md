# A4, volet géométrie : le travail par sommet passe à l'hôte (30/09/2026)

Chantier A4 (TODO « Plus tard > Vitesse ») : « empaquetage minimal ». Principe :
chaque mot que le plugin (G4 émulé par TCG) relit, convertit ou recopie par
sommet coûte ~10 fois ce qu'il coûte au device natif ; l'invité envoie les
octets tels qu'il les a, l'hôte fait le reste.

Rien ici ne change `QGPU_PROTO_VERSION` (l'intégrateur passera à v23 avec les
autres volets) : une capacité, un opcode, deux extensions de `DRAW_NATIVE`.

## 1. Chiffrer d'abord : où l'invité travaille par sommet

Sources : profils `sample` de 10 s du fil principal (fil le plus actif pour
Warcraft III), tour `bench/a4/depart/tours/t1-*` du 30/09 (QEMU et plugin de
`eb121a2`), résumés par `tools/re/sampleplug.py` ; octets par image : vidages
et `frames.csv` du tour `bench/matrice/20260930-wlunif` (décodés comme
`tools/re/dumpdec.py`).

### 1.1 Octets de géométrie par image (vidages, images « en scène »)

| jeu | chemin | lots/image | sommets/image | octets écrits dans BAR0 | revus à l'image précédente |
|---|---|---|---|---|---|
| Nexuiz ARB | Begin/End (déroulage de GLEngine) | 354 | 114 000 (150 000 à 400 000 hors vidage) | 6,5 Mo | 4 % |
| Nexuiz ARB | `DRAW_NATIVE` (VBO) | 599 | 69 000 indices | 0,13 Mo d'indices | — |
| Nexuiz GLSL | Begin/End | 314 | 26 000 | 1,8 Mo | 6 % |
| Warcraft III | Begin/End | 104 | 43 000 | 2,4 Mo | 44 % |
| Colin McRae | tableaux empaquetés (VAR) | 240 | 20 700 | 1,1 Mo | 5 % |
| Marble Blast | Begin/End | 35 | 2 100 | 0,14 Mo | 98 % |
| Zenerchi | Begin/End | 15 | 1 400 | 0,05 Mo | 99 % |
| DOOM 3 | `DRAW_NATIVE` | 1 244 | 456 000 indices | 1,8 Mo d'indices + 0,65 Mo de VBO | — |
| Prey | `DRAW_NATIVE` | 96 | 58 000 indices | 0,23 + 0,16 Mo | — |
| UT2004 | `DRAW_NATIVE` | 237 | 66 000 indices | 0,05 Mo de VBO | — |

(« revus » : lots `DRAW_RAW` dont les octets sont identiques, à l'octet, à un
lot de l'image précédente.)

### 1.2 Ce que cela pèse (part du fil principal)

| jeu | `raw_scan_nan` (tri des sommets fous, plugin) | déroulage par le code généré de GLEngine | vidages et attentes | autres postes de géométrie |
|---|---|---|---|---|
| Nexuiz ARB (110 ms) | **12,8 %** | **15,6 %** (+ 1,6 % en mode immédiat) | `flush` 5,1 % (dont 3,8 % d'attente de l'hôte) | — |
| Warcraft III (18 ms) | **3,6 %** | **4,5 %** | 0,3 % | — |
| Zenerchi (4,4 ms) | 0,1 % | 3,3 % (mode immédiat) | 0,5 % | — |
| Marble Blast (9,5 ms) | 0,8 % | ~0 | 1,3 % | `__memcpy` 1,3 % |
| Nexuiz GLSL (40 ms) | 0,5 % | 0,3 % | `flush` 48,6 % (pas la géométrie) | — |
| DOOM 3 (60 ms) | 0 | 0 | `geom_draw_native` 4,5 % dont 2 % d'attente de l'hôte, `__memcpy` 0,5 % | propre de `geom_draw_client` 3,0 % (balayage des indices en ligne, clés) |
| Prey (70 ms) | 0 | 0 | 0,5 % | `geom_draw_native` 2,0 %, propre de `geom_draw_client` 2,6 % |
| UT2004 (26 ms) | 0 | 0 | 0,6 % | `geom_draw_native` 1,4 %, propre de `geom_draw_client` 2,2 % |
| Colin McRae (70 ms) | pas de profil dans le tour (cellule sans `sample`) | | | 20 700 sommets empaquetés par image |

`send_state` pèse 14,1 % sur Nexuiz ARB, mais c'est **`close_raw` → `raw_fix_nan`**
appelé par `reserve()` (13,3 %), pas l'état : il sort avec le tri.

Conclusions :

- **Gisement 1 — le tri des sommets fous** (`raw_fix_nan` → `raw_scan_nan`) :
  chaque mot de chaque sommet que GLEngine a écrit dans BAR0 est relu sur le G4.
  12,8 % sur Nexuiz ARB, 3,6 % sur Warcraft III : au-dessus du seuil de 2 %.
  Le travail passe entier à l'hôte, à la lettre (§2).
- **Gisement 2 — le déroulage des tableaux clients par GLEngine** :
  `glDrawArrays` / `glDrawElements` sur des tableaux clients du pipeline fixe
  passent par `BeginPrimitiveBuffer` ; le code généré de GLEngine recopie
  chaque sommet **cité** (les indices sont déroulés) en flottants. 15,6 % sur
  Nexuiz ARB (14,3 % au second tour), 4,5 % sur Warcraft III. **Pas
  atteignable sans rétro-ingénierie de GLEngine** (§3.2) : `cfg+0x78` ne
  détourne pas ces dessins.
- **Gisement 3 — l'empaquetage des tableaux clients qui ARRIVENT au plugin**
  (`RenderVertexArray` : plage VAR de Colin McRae, `RenderVertexBuffer` de
  Warcraft III) : `va_pack_planned`, sommet par sommet, attribut par attribut.
  Colin McRae : 20 700 sommets empaquetés par image ; le propre de
  `geom_draw_client` (où l'empaqueteur est en ligne) pèse 12,3 % du fil
  (profil de la VM de contrôle, §5). §3 : ils partent aux octets de
  l'application en `DRAW_NATIVE` depuis BAR0.
- **DOOM 3, Prey, UT2004** passent déjà par `DRAW_NATIVE` : aucun poste de
  géométrie n'y dépasse ~3 %, et ce qui reste (balayage des indices pour la
  plage [vmin, vmax], recopie des indices clients, attentes de l'hôte) n'est
  pas séparable dans le profil (fonctions en ligne). Rien de construit.
- **Tampons hôte réutilisés pour la géométrie statique** (« mêmes octets
  renvoyés ») : Marble Blast et Zenerchi renvoient 98-99 % des mêmes octets,
  mais pour 0,05-0,14 Mo par image ; Nexuiz et Colin McRae 4-6 % ; Warcraft III
  44 % de 2,4 Mo. Voir §4 : pas construit.

## 2. `DRAW_RAW_SANE` : le tri des sommets fous fait par l'hôte

`QGPU_CAP_GEOM_HOST` = `0x00020000`, annoncé par le cœur avec
`QGPU_CAP_NATIVE`. `QGPU_OP_DRAW_RAW_SANE` = `0x0090`, longueur 11 :

```
DRAW_RAW_SANE [mode, n, voff, pas, format, ioff, itype, premier, nverts, drapeaux]
```

Les neuf premiers mots sont `DRAW_RAW` (mêmes refus). `drapeaux` :
`QGPU_RAWS_KEEP_W0` (sous programme de sommets, w = 0 n'est pas fou : volumes
d'ombre). Le cœur marque fou un sommet dont un mot a des bits
`|u| ≥ 0x4e6e6b28` (NaN, infini, |v| ≥ 1e9), ou `|w| < 1e-6` (position à 4
composantes, sans le drapeau) ; puis, s'il y en a : `GL_TRIANGLES` indexé →
triangles retirés (indice ≥ nverts ou sommet fou) ; non indexé → triangles
gardés recopiés bout à bout ; autre mode → dessin jeté (statut OK). Les mots
fous d'un sommet fou converti mais non cité valent 0. C'est **à la lettre**
`raw_scan_nan` + `raw_fix_nan` du plugin.

Plugin : `POMPPC_GL_RAWSANE=1` (éteint par défaut) — `close_raw` n'appelle plus
`raw_fix_nan` et émet `DRAW_RAW_SANE`. Le chemin tableaux (qui jette ou filtre
autrement) n'est pas touché.

## 3. `DRAW_NATIVE` depuis BAR0 : les tableaux clients aux octets de l'application

Sous `QGPU_CAP_GEOM_HOST` :

- un descripteur peut porter `buf = QGPU_BUF_SHMEM` : `offset` est un offset
  **absolu** dans BAR0, bornes comptées sur la fenêtre entière (refus = BAD_ARG
  non fatal, comme tout `DRAW_NATIVE`) ;
- indexé, `premier` n'est plus forcément 0 : c'est la **base** soustraite de
  chaque indice (un indice < base = BAD_ARG). L'invité recopie un tableau à
  partir de son plus petit sommet cité sans réécrire les indices.

### 3.1 Plugin : `POMPPC_GL_NATSHM=1` (éteint par défaut)

Dans `geom_draw_native`, un attribut sans VBO est un tableau client : ses octets
`[src + vmin·pas, src + vmax·pas + taille)` sont recopiés tels quels (`memcpy`,
sous la garde de faute du dessin) dans la zone des sommets, les tableaux
entrelacés (plages qui se chevauchent ou distantes de moins de 64 octets)
partagent un bloc, et le descripteur désigne le sommet `vmin` ; les VBO du même
dessin sont désignés au même sommet ; `premier` = vmin (indexé). Un dessin qui
ne s'y prête pas (type inconnu, attribut constant de position, de brouillard,
de secondaire ou générique, bloc plus grand que la zone) retombe sur
l'empaquetage. Les huit premiers replis sont notés (`NATSHM repli ligne …`).

Écarts assumés, les mêmes que `DRAW_NATIVE` v18 : l'hôte assainit (NaN → 0,
saturation) au lieu de retirer les triangles d'un sommet fou ; pas de « couleur
morte » (rustine de Warcraft III du chemin empaqueté ; le chemin de
`RenderVertexArray` l'ignorait déjà, `var_draw`).

### 3.2 Ce qui n'a pas marché : sortir le pipeline fixe du déroulage de GLEngine

Essayé en VM (gltest `varray`, 30/09) :

| réglage | ce que fait GLEngine sur `glDrawArrays` / `glDrawElements` à tableaux clients |
|---|---|
| défaut, `POMPPC_GL_NATSHM=1` avec `cfg+0x78` posé à chaque dispatch | déroulage (`BeginPrimitiveBuffer`), image juste |
| `POMPPC_GL_ARRAY=1` (mixte, existant) | déroulage, image juste |
| `POMPPC_GL_ARRAY=2` (descripteur `cfg+0x11c` retiré) | **aucun appel au pilote** : les dessins sont perdus (2 échecs de `varray`, trace `POMPPC_GLTRACE` sans `Render*`) |

Seuls les VBO (`gleDrawArraysOrElements_VBO_Exec` → `gleExecuteVertexArrayRange`)
et la plage VAR arrivent à `RenderVertexArray`. Le canal « GeForce3 » de
`docs/re/tableaux-de-sommets.md` §6.5 demande sans doute
`gldCreateVertexArray` / `ModifyVertexArray` / `FlushVertexArray` tenus (§7 de
ce document de RE : sondes jamais écrites) ; c'est un chantier de
rétro-ingénierie de GLEngine à part, pas un lot de ce volet. Le routage n'a donc
pas été changé (`geom_va_on` intact) : le gisement 2 (Nexuiz ARB 14-16 %,
Warcraft III 4,5 %) reste ouvert.

## 4. Pourquoi pas de tampons hôte réutilisés pour les tableaux clients

La difficulté est de savoir, sans notification d'écriture, que les octets d'un
tableau client n'ont pas changé depuis l'envoi précédent :

- une empreinte ou un `memcmp` sur le G4 relit les mêmes octets que la recopie
  qu'elle éviterait : c'est le coût de `raw_scan_nan`, précisément ce qu'on
  retire ;
- une protection des pages (`mprotect` en lecture seule, faute à la première
  écriture) donnerait la notification, mais les tableaux clients de Nexuiz et
  de Warcraft III partagent leurs pages avec les autres données du jeu (tas) :
  une faute par écriture du jeu, et un gestionnaire de signal dans le chemin
  du jeu ;
- les seuls tableaux à notification sont déjà servis : VBO (`gldFlushBuffer`,
  miroirs bruts v18) et plage VAR de Colin McRae (`glFlushVertexArrayRangeAPPLE`
  ne descend pas au pilote : copie au dessin, `docs/re/cmr-var.md` §3).

Et le gain serait petit : ce qui se répète d'une image à l'autre pèse 0,05 à
0,14 Mo (Marble Blast, Zenerchi), soit un `memcpy` de ~0,1 ms ; les gros
volumes (Nexuiz, Colin McRae) ne se répètent pas (4-6 %). Warcraft III (44 %
de 2,4 Mo) est le seul cas où la question se reposerait, une fois le §3 en
place et mesuré.

## 5. Épreuves

**Tests natifs** (`tests/qgpu_core_test.c`, soft et gl) :

- `run_raw_sane` : 400 lots tirés au hasard (positions à 3 et 4 composantes,
  NaN, infinis, ±1e9, w ≈ 0, indices hors bornes, `TRIANGLES`, rubans,
  éventails, indexés ou non, `KEEP_W0`) : la référence est `raw_scan_nan` +
  `raw_fix_nan` du plugin recopiés dans le test, appliqués à une copie des mots
  puis `DRAW_RAW` ; `DRAW_RAW_SANE` sur les mots d'origine remet au backend
  **les mêmes sommets et les mêmes indices à l'octet**, donne la même image et
  le même statut, 400 sur 400 (soft : 247 dessinés dont 165 filtrés, 111
  jetés, 42 refusés par les deux voies ; gl : 234 / 139 / 116 / 50). Drapeau
  réservé : BAD_ARG non fatal ; sans la capacité : BAD_OPCODE.
- `run_native_shmem` : `idDrawVert` entrelacés dans BAR0, trois tableaux
  séparés (couleur à offset impair, pas 0 et 12), position en tampon hôte et le
  reste dans BAR0, non indexé à `premier` 1, base des indices 1000, génériques
  8..11 sous programme ARB (gl) : sommets et indices remis au backend
  **identiques à l'octet** à `DRAW_RAW` (la forme empaquetée) et à la voie v18
  (tampon hôte) ; bornes sur la fenêtre entière (dernier octet accepté, un de
  plus refusé, débordement 32 bits), capacité absente : BAD_ARG non fatal.
- `tests/run-all.sh` : 172 OK, 0 échec, 6 ignorés.

**Rejeu** : les 9 vidages de la matrice `20260930-wlunif` (357 images) rejoués
par le cœur de `main` et par celui de cette branche : **identiques à l'octet**.

**VM de contrôle** (recouvrement de `tiger-endurance.qcow2`, QEMU
`~/src/qemu-a4geo`, SMP=2, sans fenêtre ni son ; plugin de cette branche
compilé par gcc-4.0 dans l'invité) :

- `gltest` (`tools/guest/jobs/gtgeo.sh`), 27 scènes de géométrie en 256×256 et
  les 8 scènes à programmes à la taille par défaut : image **identique à
  l'octet** (md5) entre les voies d'avant, `RAWSANE=1`, `NATSHM=1` et les deux ;
  mêmes verdicts ; les échecs vus (`matbegin`, scènes à programmes en 256×256,
  `arbvp0cmr` cas (g)) sont les mêmes dans tous les modes, donc d'avant.
- matrice, les deux leviers allumés, avec vidage : **image juste**
  (rejeu = VM et référence validée, 0,00/0,00 %) pour Nexuiz ARB fenêtre et
  plein écran, Nexuiz GLSL, Marble Blast, Zenerchi, Warcraft III, Colin McRae
  (rejeu fait par le `qgpu_replay` de cette branche : `bench/matrice/bin` est
  partagé entre agents, voir §6), et **DOOM 3, Prey, UT2004 en plein écran**
  (0 repli). En fenêtre, ces trois-là finissent en « capture non
  synchronisée » (présentation hôte refusée, replis) dans les DEUX modes —
  témoin sans levier compris — sur cette VM sans affichage : défaut de
  l'environnement de contrôle, pas de ce volet ; à revoir par le kit sur la VM
  quotidienne.
- compteurs : Nexuiz ARB 437 000 `DRAW_RAW_SANE` sur 1 000 images, 0 repli ;
  Colin McRae **249 dessins natifs par image depuis BAR0, 0 `DRAW_RAW`
  empaqueté** (avant : 240 `DRAW_RAW` et 1,1 Mo empaquetés par image),
  0 retombé sur l'empaquetage, 0 refus de l'hôte ; Warcraft III 25 000 dessins
  natifs clients (22 Mo recopiés) sur 2 000 images.

**Ce que l'invité ne fait plus** (profils `sample` de la VM de contrôle, parts
du fil principal, même scène, voies d'avant → leviers ; l'hôte était partagé
avec 2-3 autres VM : les ms/image de ces tours ne valent rien, seules les parts
comptent) :

| jeu | poste | avant | après |
|---|---|---|---|
| Nexuiz ARB | `raw_scan_nan` | 8,4 % (12,8 % au tour de départ) | 0 |
| Nexuiz ARB | `qgpu_wait` (attente de l'hôte) | 2,7 % | 4,8 % |
| Warcraft III | `raw_scan_nan` | 4,8 % | 0 |
| Colin McRae | `geom_draw_client` inclusif | 22,4 % | 14,2 % |
| Colin McRae | propre de `geom_draw_client` (empaqueteur en ligne) | 12,3 % | 2,1 % |
| Colin McRae | `geom_draw_native` + `__memcpy` | 1,0 % | 4,6 % |

Sur Nexuiz ARB, une partie du gain part en attente de l'hôte : le G4 va plus
vite que le device ne dessine ses 400 000 sommets par image. La mesure A/B
(kit §7) dira ce qui reste en ms/image.

## 6. Piège rencontré : `bench/matrice/bin` est partagé

`tools/matrice/matrice.py` reconstruit `bench/matrice/bin/qgpu_replay` depuis
les sources du worktree qui le lance, dès qu'elles sont plus récentes. Plusieurs
agents avec des cœurs différents se l'écrasent : un vidage qui porte
`DRAW_RAW_SANE` rejoué par le binaire d'un autre worktree rend `BAD_OPCODE`
(« rejeu : 41 soumission(s) en erreur »), et inversement. Après fusion, un seul
cœur : le piège disparaît. En attendant, les tours de ce volet ont été
réanalysés avec le `qgpu_replay` de la branche.

## 7. Kit d'A/B (pour l'intégrateur)

`tools/matrice/ab-geometrie.sh CAMPAGNE N [JEUX] [MODES] [VARIABLES_B]`, sur la
VM quotidienne, avec un QEMU qui porte ce `qgpu_proto.h` / `qgpu-core.c` et le
plugin de la branche installé : A/B entrelacé ref / geo (`matrice.py --env`),
QEMU relancé à chaque partie, puis un tour de justesse avec vidage par mode,
bilan `tools/matrice/ab-geometrie-bilan.py` (médianes, rapport, et alerte si le
plugin n'a pas vu le levier). Exemple :

```
QEMU_BIN=~/src/qemu-a4geo/build/qemu-system-ppc \
  tools/matrice/ab-geometrie.sh geo1 3 nx,wc3,cmr,mb,zen fen,pe
tools/matrice/ab-geometrie.sh geo2 3 nx fen "POMPPC_GL_RAWSANE=1"   # un levier seul
```

Décision attendue : allumer `POMPPC_GL_RAWSANE` et `POMPPC_GL_NATSHM` par défaut
si la matrice reste 16/16 et si l'A/B ne montre pas de perte (Nexuiz ARB,
Warcraft III, Colin McRae sont les cellules qui bougent ; DOOM 3, Prey, UT2004
ne devraient pas bouger).
