# Backend GL sur un hôte NVIDIA : 4 unités au pipeline fixe (QGPU_CAP_FIXED4)

02/10/2026, PC Linux (RTX 4060 Ti, pilote 595.91.07, EGL).

## Symptôme

Sur ce PC, chaque démarrage disait « qgpu: backend gl refusé par l'auto-test : unités de
texture du pipeline fixe » : tout le GPU 3D retombait sur le backend logiciel (couloir de jeu de
`gltest` : 16 images/s au lieu de 626). C'était le cas depuis le protocole v17 (24/09, 8 unités
de texture, mis au point sur le M4).

## Cause

`gl_selftest` exigeait `GL_MAX_TEXTURE_UNITS ≥ QGPU_MAX_UNITS` (8). Le GL d'Apple en donne 8 ;
NVIDIA, en profil de compatibilité, **4** au pipeline fixe, mais 8 jeux de coordonnées et 32 unités
d'image (`/tmp/glunits` : `MAX_TEXTURE_UNITS=4 COORDS=8 IMAGE_UNITS=32`). Et il **ignore en
silence** les unités fixes 4..7 : `glEnable(GL_TEXTURE_2D)` et `glTexEnv` y passent sans erreur
GL, mais le texel ne compte pas (unité 0 blanche en REPLACE, unité N verte en MODULATE : vert pour
N = 1..3, blanc pour N = 4..7). Le refus était donc juste : accepter tel quel aurait faussé
l'image sans rien dire. Le refuser tout entier, en revanche, ne l'était pas.

Bug voisin : `gl_init` pose `COMBINE3`, `OCCLUSION`, `GL14`, `PROGRAMS` et `GLSL` avant l'auto-test
et ne les retirait pas en cas de refus ; le backend logiciel qui prenait la suite en héritait.

## Correction

- **Hôte** (`qgpu-gl.c`) : 4 unités fixes suffisent si les programmes ont 8 coordonnées et 8 unités
  d'image ; le backend annonce alors `QGPU_CAP_FIXED4` (`qgpu_proto.h`, sans changer
  `QGPU_PROTO_VERSION`) et le dit au démarrage. Un dessin du pipeline fixe qui allume une unité au
  delà (plugin plus ancien) est signalé une fois sur stderr. Le chemin d'échec de `gl_init` retire
  ses bits.
- **Invité** (plugin) : avec `QGPU_CAP_FIXED4`, `GL_MAX_TEXTURE_UNITS` annoncé = 4, comme un
  GeForce FX ; `GL_MAX_TEXTURE_COORDS` et `_IMAGE_UNITS` gardent 8 (DOOM 3 lit `texture[0..6]` par
  ses programmes). Un dessin du pipeline fixe qui allumerait une 5ᵉ unité va au rendu d'Apple
  (`UNIT_LIM` : `G.fixed_units` sans programme de fragments, `G.units` avec). Sur le M4 (8 unités
  fixes), rien ne change.

## Preuve (VM de dev 10.4.11, backend GL de la RTX 4060 Ti)

| épreuve | résultat |
|---|---|
| job `gpu` (51 scènes `gltest`, chacune comparée au rendu d'Apple) | **51 OK, 0 échec** ; couloir de jeu 625,96 img/s (16 par Apple) |
| scène `units` (nouvelle : toutes les unités annoncées en MODULATE) | plugin : `GL_MAX_TEXTURE_UNITS = 4`, vert attendu ; Apple seul : 8, vert |
| `prebuilt` régénéré sous 10.4.11 (plugin, kexts POMPPCGPU et POMPPCFsqrt), CD gravé par `xorriso` | monté par Tiger (HFS+), `install.sh` réussit, les deux kexts se chargent au redémarrage |
| Marble Blast sur le backend GL | rendu juste, aucun avertissement d'unité ignorée |

## Test natif (07/10)

`tests/qgpu_core_test.c` (`run_v17`) dessinait les unités 5 et 6 **au pipeline fixe** sur les deux
backends ; sur le GL de ce PC, (a) et (b) rendaient le fond blanc (`ffffff`, texel ignoré) : le
test demandait ce que `QGPU_CAP_FIXED4` ne promet plus, et que le plugin n'envoie jamais (dessin
rendu par Apple, `UNIT_LIM`). Sous `QGPU_CAP_FIXED4`, (a) et (b) sont refaits sous **programme
de fragments** (`TEX texture[5]`, `MUL texture[0] × texture[6]`), ce que le device promet
toujours : mêmes textures, mêmes pixels attendus (`4080c0`, `208000`), obtenus sur la RTX 4060
Ti. Le backend logiciel garde le pipeline fixe. Le test vérifie aussi que `FIXED4` ne vient
qu'avec `QGPU_CAP_PROGRAMS` et jamais du backend logiciel.

Les VM existantes doivent recevoir le nouveau plugin (CD POMPPCSRC : `install.sh` du dépôt, ou
CD invité) : avec l'ancien, le backend GL marche mais annonce 8 unités fixes à l'application.

## Scènes gltest sur le PC (07/10)

**Constat.** Une passe de nuit sur la VM quotidienne (10.4.11, session bureau, plugin
`20261004-d3x`, `gltest` précompilé du 04/10, binaire de référence) a joué les 82 scènes par
`./gltest <scène>` par ssh, **sans taille ni variable** : 70 réussites, 12 échecs (`bigstrip`,
`clip`, `dlist`, `fusion`, `lit`, `matbegin`, `mixte`, `stencil`, `tcprobe`, `texgen`, `v15`,
`combine3`), codes de sortie seulement. Le même jour, le job `gpu` (53 scènes, 256×256,
`GLTEST_NOWS=1`, `GLTEST_STENCIL=1` pour `stencil`) était vert sur une copie de la VM de dev. Dix
des onze (hors `combine3`, attendu sur NVIDIA) sont dans le job `gpu` ; `matbegin` n'est que dans
`gtgeo.sh`. Le `gltest` précompilé n'est pas en cause : `gltest.c` n'a changé depuis le 04/10
que par l'ajout de `vpimm`.

**Rejeu** (copie de `tiger-dev.raw`, binaire de référence `~/src/qemu/build` du 06/10, plugin et
`gltest` compilés dans l'invité ; pour chaque cas, plugin exigé puis rendu d'Apple seul,
`POMPPC_GL_DISABLE=1`, puis `gltest diff`) :

| scène | sans taille (64×64), comme la nuit | 256×256 (job `gpu`) | verdict |
|---|---|---|---|
| `bigstrip` | plugin ET Apple : 3 témoins faux, « polygone : centre (32,160) hors du tampon 64x64 » | OK, écart à Apple 1 hors arêtes | taille |
| `clip` | plugin ET Apple : « coupé à droite » lit (70,10), hors du tampon | OK, 1 | taille |
| `dlist` | plugin ET Apple : « liste d'affichage (2) : bleu » hors du tampon | OK, 1 | taille |
| `fusion` | plugin ET Apple : 8 témoins et plus « hors du tampon 64x64 » | OK, 0 | taille |
| `lit` | plugin ET Apple : 4 témoins hors du tampon (six cases de 64×64) | OK, 1 | taille |
| `mixte` | plugin ET Apple : « triangle après glDrawPixels » (60,130) hors du tampon | OK, 1 | taille (+ plantage, plus bas) |
| `tcprobe` | plugin ET Apple : 7 témoins x = 68..188 hors du tampon | OK, 0 | taille |
| `texgen` | plugin ET Apple : 3 « motif présent » hors du tampon | OK, 1 | taille |
| `v15` | plugin ET Apple : 8 relevés « NON TENU » (pixel hors du tampon) | plugin 17/17 tenu ; Apple 5 non tenus (référence, comme toujours) | taille (+ plantage) |
| `stencil` | sans `GLTEST_STENCIL` : plugin ET Apple, 3 témoins faux ; avec, en 64×64 : OK, 0 | OK, 0 | variable manquante |
| `matbegin` | plugin : (8,48) noir et (28,48) vert au lieu de rouge ; Apple OK | idem | **défaut réel**, connu |

Mêmes verdicts en single-user (`GLTEST_NOWS=1`) et en session bureau, par l'agent racine comme
par le relais `POMPPCGuiRunner` (sans `GLTEST_NOWS`, qui y rend 10006 à toute scène) : la
session n'est pour rien dans les dix faux rouges, ni NVIDIA (Apple seul échoue aux mêmes
témoins, dans la même VM). `matbegin` est le défaut documenté dans `docs/re/opengl-1.4.md` §3.3
(`glMaterial` entre `glBegin`/`glEnd` : `_gleForceToSoftwareTCL` sans consulter `cfg+0x7a`) ; il
est côté invité (GLEngine, chemin brut), pas propre au PC ni à NVIDIA, et vu aussi par `gtgeo.sh`
sur la VM de contrôle d'A4.

**Corrigé dans `gltest`** : sans taille donnée, les neuf scènes ci-dessus se jouent en 256×256
(liste `grandes`, comme `tex13`/`tex14`/`gl15`/`texlod` depuis le 24/09) et `stencil` demande
toujours son tampon de stencil. Rejeu de la passe de nuit (sans taille ni `GLTEST_STENCIL`) après
correction : les dix vertes, écart à Apple ≤ 1 hors arêtes, `matbegin` seule rouge. Le `gltest`
précompilé de `disks/prebuilt` est à régénérer (job `prebuilt`) pour que la passe de nuit en
profite.

### Plantage de `try_draw_pixels` dans la session de l'utilisateur

En session bureau, par le relais (le contexte des jeux), `mixte` et `v15` en 256×256 et `v15`
sans taille ont fini **trois fois de suite en `Segmentation fault`** (rc 139) avec le plugin de
`main`, alors que les mêmes scènes passaient par l'agent racine dans la même session. Journal
de CrashReporter (`~tiger/Library/Logs/CrashReporter/gltest.crash.log`) :
`EXC_BAD_ACCESS … __memcpy ← try_draw_pixels (pomppc_accel.c:16725) ← pomppc_proc_pre ←
glDrawPixels_Exec`. Pour `v15` (`glDrawPixels` 8×8 GL_RGB, `pixels` = 0x40de0), l'adresse
source fautive est 0x5d450de0 = `pixels + 0x5d410000` : le décalage vient de
`CTX_UNPACK_ROW_LENGTH/SKIP_ROWS/SKIP_PIXELS/ALIGNMENT` (`gctx+0x31cc..0x31e0`), qui ont donc
valu autre chose que 0.

Ces offsets ont été relevés sur 10.4.6 et déjà pris en défaut le 22/09 (ALIGNMENT à 4 quand
l'application pose 1, `docs/bug-hunt-2026-09-22.md`). Une note par appel (64 appels, plugin de
`main` puis corrigé, racine et relais, 8 × `mixte` et 8 × `v15` chacun) les montre **à 0 tous
les quatre**, `ALIGNMENT` compris — le défaut GL est 4 : ce ne sont pas les mots de
`glPixelStorei` sur 10.4.11. Le plantage ne s'est pas reproduit sur ces 64 appels : ce qui
rend ces mots non nuls est inconnu (mémoire non initialisée, ou un autre état de GLEngine).

**Correction (plugin)** : `unpack_trusted()` ; `try_draw_pixels`, `try_draw_ds` (profondeur,
stencil) et `try_bitmap` ne prennent le chemin hôte que si `ROW_LENGTH` vaut 0 ou `w`, les deux
`SKIP` 0 et `ALIGNMENT` 0, 1, 2 ou 4 (même pas pour une ligne serrée multiple de 4, la seule que
ces chemins acceptent) ; sinon rendu logiciel d'Apple, exact puisque GLEngine lit, lui, le vrai
état ; une note `unpack non sûr …` (8 au plus par processus, `POMPPC_GL_NOTE`) le dit. Le cas
relevé (tout à 0) ne change pas. Ce n'est pas la cause, c'est la garde : les vrais offsets
restent à relever (TODO).

**Preuves après correction** : job `gpu` en single-user **52 OK, 1 échec** (`vpimm`, plus bas),
TEX3 et lot 16 bits verts, `qgpu_test` vert ; les 53 scènes du job en 256×256 dans la session
bureau, par l'agent racine et par le relais, image comparée à Apple : même 52/53, aucun
plantage du plugin (rc des 106 exécutions plugin relevés ; le journal ne gagne qu'une entrée par
passe, celle de `tex13` sous Apple seul — Bus error connu de la référence, lue pour la première
passe : pile dans `GLDriver`/`gldTessellatePolygonRGBA_SmoothTexture`).

**`vpimm` rouge sur le binaire de référence** : 7 témoins « (b) imm texcoord[1] » faux ; la
scène (07/10, 99e36a4) exige le correctif hôte de `qgpu-gl.c` du même commit, et
`~/src/qemu/build/qemu-system-ppc` date du 06/10 15:58. Le « 53 OK » du même jour venait d'un
binaire à jour. Rien à corriger dans le plugin ; QEMU de référence à reconstruire.

**Piège de la boucle de dev, revu ce jour** : l'agent d'une copie ancienne de `tiger-dev.raw`
écrit l'en-tête à trois champs (`agent.sh` du 19/09) — `devloop.py run` rend alors 0 même quand
le job échoue (`VERDICT : 1 échec(s)` mais `rc=0`) : lire le verdict, pas le code. Et sur un
hôte chargé, `devloop.py start` peut juger « stable » l'écran gris de BootX et frapper dans le
vide, ou lancer l'agent APRÈS le dépôt du premier job, qu'il ignore alors comme périmé : vérifier
« agent: prêt » sur une capture avant le premier `run`.
