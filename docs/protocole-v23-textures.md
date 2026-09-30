# A4, volet textures : textures par DMA sur plages sales — mesuré, rien à construire (30/09/2026)

Chantier A4 (`TODO.md`, « Plus tard > Vitesse ») : « déplacer le travail vers l'hôte […]
textures par DMA sur plages sales ». Volet voisin du backend : G7 (`glTexSubImage*` par
rectangle sale) et G8 (PBO en rotation pour `SURF_PRESENT`), bug hunt du 22/09. Règle du
projet : mesurer avant d'optimiser. **Verdict : le plafond est nul en jeu et négligeable au
chargement ; le protocole n'est pas construit** (ni bit de capacité `0x00010000`, ni opcode
`0x0088..0x008F` consommés : ils restent libres). G7 : rien à gagner non plus. G8 : voir §4.

## 1. Ce que l'invité fait aujourd'hui (v22)

- Depuis la v10 (19/09), **le plugin ne convertit plus** : `upload_texture` recopie chaque
  niveau tel quel (format, type et pas de ligne de GLEngine) dans l'arène de BAR0 par un seul
  `memcpy`, et le cœur de l'hôte convertit (`tex_store`). L'idée de départ (« le G4 convertit
  et recopie ») ne vaut plus que pour la recopie.
- Les octets lus ne sont **pas ceux de l'application** : c'est le stockage des niveaux de
  GLEngine (`LV_DATA` de l'objet texture), que GLEngine a déjà rempli en copiant (et parfois
  convertissant) les texels de l'application pendant `glTexImage2D`/`glTexSubImage2D`, sur le
  G4. Cette première copie, qui est la plus chère, n'est pas à la portée d'un DMA du device.
- Une texture n'est renvoyée que si elle est sale (crochet `pomppc_texture_changed`, ou
  empreinte `tex_lv0_sig` changée) ; elle part alors **en entier** (tous ses niveaux) —
  `TEX_SUBIMAGE` existe dans le protocole mais le plugin ne l'émet pas.
- Hors du chemin des textures : `COPY_TEX` et `SURF_TEX` restent sur le GPU de l'hôte
  (`glCopyTexSubImage*`, 27/09) ; rien ne traverse l'invité.

## 2. Mesure (sans VM : vidages et profils existants)

Sources : vidages des tours `bench/matrice/20260930-wlunif` et `20260930-0128` (16 cellules
chacun), profils `bench/plugin/ab2-B{1,2}/{d3,prey}-fen/mesure/sample.txt`, `frames.csv` des
mêmes tours, `bench/reg/camp2-smp2.txt` (banc `regbench`, noyau `copy`). Scripts : voir §6.

**Méthode.** Un vidage de matrice est *autonome* : sa première image réémet tout l'état et
retéléverse toutes les textures liées ; les autres textures, marquées sales, repartent à leur
premier usage dans les images suivantes. On distingue donc, par clé (texture, face, niveau) :
un **renvoi** (niveau déjà envoyé dans le vidage et renvoyé : vrai trafic de régime) et un
**premier envoi** (première apparition après la réémission : surtout de la contamination du
vidage, donc une borne haute du régime).

### 2.1 En jeu (régime), octets de texels recopiés par l'invité par image

| Cellule | renvois | premiers envois (borne haute) | ms/image | part, borne haute |
|---|---:|---:|---:|---:|
| DOOM 3 fenêtre / plein écran | **0** | 19 Kio | 60 | < 0,02 % |
| Prey fenêtre / plein écran | **0** | 10–69 Kio | 50–71 | < 0,07 % |
| UT2004 fenêtre / plein écran | **0** | 73–179 Kio | 26 | < 0,35 % |
| Colin McRae plein écran | **0** | 51–169 Kio | 69 | < 0,13 % |
| Warcraft III plein écran | **0** | 53–55 Kio | 18 | < 0,15 % |
| Nexuiz ARB fenêtre / plein écran | **0** | 0,82–0,92 Mio | 98–110 | < 0,5 % |
| Nexuiz GLSL fenêtre / plein écran | **0** | 0,89–1,04 Mio | 40 | < 1,3 % |
| Marble Blast fenêtre / plein écran | **0,9 Kio** (une 64×64 L) | 34–35 Kio | 9–10 | < 0,2 % |
| Zenerchi fenêtre | **0** | 9 Kio | 4–5 | < 0,1 % |

Parts calculées à **2 Gio/s** de `memcpy` invité, hypothèse prudente : `regbench` mesure
1 500 × 1 Mio en 189–198 ms (≈ 8 Gio/s, cache chaud) et BAR0 est une région RAM de QEMU
(`memory_region_init_ram`), donc le chemin rapide du TLB, pas du MMIO. **En régime, le trafic
réel est de 0 octet par image dans 14 cellules sur 16, et de 0,9 Kio dans Marble Blast.**
Aucun jeu de la matrice ne met à jour une texture à chaque image par l'invité : le rendu vers
texture (DOOM 3 `_currentRender`, Prey, Colin McRae) passe par `COPY_TEX`/`SURF_TEX` sur
l'hôte, et les vidéos ne sont pas dans les scènes mesurées.

Profils `sample` (DOOM 3, Prey, fenêtre, 26/09), fil principal : `upload_texture` 0,5–1,1 %
inclusif, `texture_uploadable` + `tex_lv0_sig` ≤ 0,5 %, **sans aucun `memcpy` sous eux** : ce
temps est la vérification des textures propres (`tex_is_3d`, `tex_prm_sig`, empreinte), pas le
transport des texels. Il relève du volet « état »/verdict, pas d'un DMA.

### 2.2 Au chargement

Jeu résident réémis par le vidage (toutes les textures liées à cette image) : DOOM 3 14,5 Mio
(2 099 niveaux), plein écran 18 Mio ; Nexuiz 35–42 Mio ; Zenerchi 14 Mio ; Colin McRae
12,5 Mio ; Marble Blast 6 Mio ; UT2004 5 Mio ; Prey 4,3–6,7 Mio ; Warcraft III 3 Mio. Même en
comptant large (Nexuiz : 3 873 niveaux envoyés sur toute la session d'après le bilan
`POMPPC_GL_STATS`, soit ~5× le jeu résident), la recopie par l'invité coûte **7 à 100 ms par
chargement** à 2 Gio/s, pour des chargements de 10 à 60 s (pauses de plus de 250 ms entre
échanges : DOOM 3 30 s, Prey 63 s, Nexuiz 10–19 s, UT2004 2–9 s) : **≤ 0,3 %**. La recopie
a lieu au premier dessin de chaque texture, pas pendant l'écran de chargement.

### 2.3 Côté hôte : ce que G7 et G8 peuvent gagner

Le device exécute les soumissions sur son fil de rendu (`qgpu-render`), en parallèle du vCPU :
un gain côté hôte n'apparaît dans le temps d'image que par l'**attente** de l'invité sur les
barrières (colonne `wait_ms` de `frames.csv`, fenêtre de 600 images avant le vidage) :

| Cellule | attente ms/image | ms/image | plafond d'un gain hôte |
|---|---:|---:|---:|
| DOOM 3 | 0,76–0,77 | 60 | 1,3 % |
| Prey | 0,47–0,49 | 50–60 | 0,9 % |
| UT2004 | 0,06–0,10 | 26 | 0,4 % |
| Warcraft III | 0,06 | 19 | 0,3 % |
| Marble Blast | 0,09–0,11 | 10 | 1 % |
| Zenerchi | 0,03 | 5,5 | 0,6 % |
| Nexuiz ARB | 3,6–3,7 | 98 | 3,7 % |
| Nexuiz GLSL | 2,7–2,8 | 40–41 | 6,8 % |

- **G7** (re-spécification par `glTexImage*` au lieu de `glTexSubImage*` du rectangle sale) :
  `gl_tex_sync` ne renvoie déjà que les **niveaux** sales ; en régime, aucun niveau n'est sali
  (0 renvoi, 0 `TEX_SUBIMAGE`, `COPY_TEX`/`SURF_TEX` sur le GPU sans passer par `px`). Plafond
  nul en jeu ; au chargement, le coût est la création même des textures. **Rien à faire.**
- **G8** : voir §4.

## 3. Pourquoi le protocole n'est pas construit — et la durée de vie des octets

Un DMA « lire les texels là où ils sont » ne supprimerait que le `memcpy` du plugin, soit au
plus 0,02 ms/image en régime (0 en pratique) et quelques dizaines de ms par chargement. Il
coûterait :

- **La durée de vie des octets.** Les texels lus sont le stockage de GLEngine. Avec la
  soumission asynchrone (`docs/architecture.md` §3), le device les lirait *après* le retour de
  l'appel GL : entre-temps l'application peut appeler `glTexSubImage2D` (GLEngine écrit en
  place : texels plus récents que la commande — ordre des commandes violé), `glTexImage2D`
  (GLEngine peut libérer et réallouer le niveau : lecture d'une page rendue au système ou
  réutilisée) ou `glDeleteTextures`. Avec `GL_APPLE_client_storage`, `LV_DATA` pointe dans la
  mémoire de l'application, qui peut la réécrire ou la libérer quand elle veut (le remplissage
  en place de l'atlas de Warcraft III, `tex_lv0_sig`, en est un exemple). Il faudrait donc,
  pour chaque niveau envoyé : câbler les pages (kext, `qgpu_abi.h` ⇒ redémarrage), et soit
  attendre la barrière de la soumission avant de rendre la main à GLEngine pour toute
  modification de la texture (doorbell synchrone : exactement ce que L4 a éliminé), soit
  protéger les pages en écriture et copier à la faute (copie sur écriture dans le noyau de
  Tiger). Les deux coûtent plus que le `memcpy` qu'ils remplacent — qui, lui, **fige** les
  octets dans l'arène au moment de l'appel et règle la question par construction.
- **La sûreté.** Le device lirait des adresses physiques fournies par l'invité hors de BAR0 :
  une table de pages câblées à valider par le kext et le device, alors qu'aujourd'hui tout ce
  que le cœur lit est borné dans `shmem` (`in_shmem`).

Ce que coûte vraiment une texture sur le G4 est la copie de GLEngine (`glTexImage2D` de
l'application vers le stockage de GLEngine) et le décodage par le jeu ; ni l'un ni l'autre
n'est atteignable par le device sans court-circuiter GLEngine
(`docs/re/etude-court-circuit-glengine.md`).

## 4. G8 : relecture de présentation

`SURF_PRESENT` fait, sur le fil de rendu, un `glReadPixels` synchrone de la surface puis
l'empaquetage vers le scanout (`qgpu-core.c`). Rejeu natif des vidages du tour
`20260930-wlunif` (backend gl, hôte au repos, compteur `present_ns` ajouté au cœur, §5) :

| Cellule | `SURF_PRESENT` ms (moyenne) | relecture seule, GPU déjà synchronisé | soumissions ms/image (régime) | attente de l'invité ms/image (VM) | plafond G8 |
|---|---:|---:|---:|---:|---:|
| DOOM 3 fenêtre / plein écran | 2,6 / 2,9 | 0,29 / 0,52 | 9,3 / 9,4 | 0,77 / 0,76 | ≤ 1,3 % |
| Prey fenêtre | 1,3 | 0,29 | 2,4 | 0,49 | ≤ 0,8 % |
| UT2004 plein écran | 1,7 | 0,49 | 3,2 | 0,06 | ≤ 0,2 % |
| Nexuiz ARB fenêtre | 1,1 | 0,43 | 7,2 | 3,6 | ≤ 1,2 % |
| Nexuiz GLSL fenêtre / plein écran | 1,1 / 1,2 | 0,43 / 0,50 | 5,8 / 6,0 | 2,7 / 2,8 | ≤ 2,9 % |
| Colin McRae plein écran | 1,8–1,9 | 0,50–0,60 | 2,1 par échange (3 par image) | ~7 par image affichée | ≤ 2,7 % |
| Warcraft III plein écran | 1,7 | 0,51 | 2,7 | 0,06 | ≤ 0,3 % |
| Marble Blast plein écran | 1,25 | 0,51 | 1,5 | 0,11 | ≤ 1 % |

Lecture : l'essentiel d'un `SURF_PRESENT` (1 à 2,4 ms) est **l'attente de la fin du rendu de
l'image par le GPU** ; la relecture elle-même et l'empaquetage coûtent 0,3–0,6 ms. Un PBO en
rotation n'enlève cette attente que du fil de rendu. Il ne peut raccourcir le temps d'image que
de l'attente de l'invité, et au plus de la durée du `SURF_PRESENT` : plafond = min(attente,
présentation) / ms par image, **borne haute**, car une partie de l'attente vient d'autres
soumissions (Colin McRae attend ~7 ms par image affichée pour 1,9 ms de présentation).
Plafonds : ≤ 1,3 % pour DOOM 3, ≤ 2,9 % pour Nexuiz GLSL, ≤ 2,7 % pour Colin McRae, < 1 %
ailleurs.

**Décision : G8 n'est pas construit dans ce lot.** Le plafond ne dépasse 2 % que pour deux
cellules, comme borne haute, et le PBO change une sémantique : l'image n'est plus dans le
scanout quand la barrière de sa soumission passe. Un échange rendu par Apple (repli d'un
échange sur 90 en fenêtre) ou une capture de la matrice arriverait alors avant la copie
différée, qui écraserait l'image plus récente d'Apple. Il faudrait solder la copie en attente
au début de la soumission suivante et avant toute relecture, et prouver l'absence
d'image périmée sur la matrice. **Épreuve qui trancherait sans construire** : une propriété de
débogage du device qui saute la relecture de présentation (image fausse, attente mesurée),
A/B de `wait_ms` sur Nexuiz GLSL et Colin McRae ; si l'attente ne baisse pas d'au moins 1 ms
par image, G8 est clos « mesuré, rien à gagner ».

Colin McRae (10 % du temps à attendre l'hôte, ~7 ms par image affichée, pour ~6,5 ms de
travail hôte par image) est le seul jeu dont l'invité attend l'hôte pour une part notable ; ce
n'est ni la présentation seule ni `SURF_TEX` (0,12 ms la copie sur le GPU). Piste hors de ce
volet : ce que le plugin attend de façon synchrone dans les échanges des fenêtres cachées.

## 5. Compteurs ajoutés

- `QgpuCopyStats.present`, `present_ns` (cœur) : nombre et temps hôte des `SURF_PRESENT`
  (relecture et empaquetage vers le scanout). Sans effet sur le comportement ; la copie
  `hw/display/` de QEMU est à reprendre à l'intégration (rien d'autre n'en dépend).
- `tests/qgpu_replay.c` : ligne « régime » (temps des soumissions par image hors première image
  du vidage, temps de la relecture de présentation) et ligne `SURF_PRESENT`.

## 6. Reproduire

Depuis la racine du dépôt : `tools/re/a4tex.py` (réémission, renvois et premiers envois par
image d'un vidage), `tools/re/a4attente.py` (attente de l'hôte par image et pauses de
chargement d'un `frames.csv`), et le rejeu natif (construit comme dans `tools/matrice/matrice.py`).

```
python3 tools/re/a4tex.py bench/matrice/20260930-wlunif/*/invite/dump
python3 tools/re/a4attente.py bench/matrice/20260930-wlunif/*/invite/frames.csv
cc -std=gnu11 -O1 -pthread -w -I patches/qgpu tests/qgpu_replay.c patches/qgpu/qgpu-core.c \
   patches/qgpu/qgpu-soft.c patches/qgpu/qgpu-gl.c -framework OpenGL -lm -o /tmp/qgpu_replay
/tmp/qgpu_replay bench/matrice/20260930-wlunif/d3-fen/invite/dump /tmp/r gl 2>&1 | grep -E 'régime|SURF_PRESENT :'
```

Recalage sur les profils frais de l'agent « état » (`bench/a4/depart/tours/t1-*`, commit
`eb121a2`, 9 profils `sample`) : **0 échantillon de `memcpy` sous `upload_texture`** dans 8
profils, 2 sur 865 dans Nexuiz GLSL fenêtre, pris pendant l'écriture du vidage (réémission).
`upload_texture` y pèse 0,4–1,6 % inclusif, sans transport de texels : c'est la vérification
des textures propres.

## 7. Ce qui reste libre

Bit de capacité `0x00010000` et opcodes `0x0088..0x008F`, réservés pour ce volet : non
consommés. Aucun changement de `qgpu_proto.h`, de `qgpu_abi.h`, du plugin ni du kext.
