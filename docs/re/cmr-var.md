# Colin McRae : la géométrie éclatée résolue — `GL_APPLE_vertex_array_range` (26/09/2026)

Mystère ouvert depuis le 23/09 (`programmes-arb.md` §3 bis/ter, TODO §6) : en course, la
géométrie du monde sort en éclats (pointes, nappes) ou disparaît, le HUD est juste. L'hypothèse
du 23/09 (« GLEngine déroule des tableaux déjà libérés par IndirectX ») était **fausse** : les
tableaux sont vides **avant** que le jeu appelle `glDrawElements`. La cause est une extension
que le rendu n'annonçait pas. Tout ce qui suit est **[L]** (lu au désassemblage d'IndirectX,
`otool -tV` de `IndirectX.framework/Versions/A/IndirectX`, tranche ppc) ou **[E]** (vu en vrai).

## 1. La preuve : les sommets sont nuls avant l'appel GL [E]

`gltrap` a appris (`POMPPC_GLTRAP_MEM=1`, fenêtre `POMPPC_GLTRAP_WINDOW=big100:60`) à relire le
premier sommet référencé **avant et après** l'appel réel de `glDrawElements`, et à journaliser
tout `free()` d'un bloc qui contient un pointeur d'attribut encore en service. En course
(`.run/cmr/r10/gltrap.txt`) :

```
glDrawElements(5, 160, 1403, 0x368ee6e)
   avant attr0[35] @0x368d748 : 0 0 0
   après attr0[35] @0x368d748 : 0 0 0
```

Aucun `free` de ces blocs, aucun appel de tampon (`glBufferData`, `glMapBuffer`) : le jeu passe
à GL une mémoire **jamais écrite**. La sonde du plugin (`POMPPC_GL_CMRPROBE`, dans `geom_begin`)
lit la même chose au `BeginPrimitiveBuffer` (`[16] brut 0d8df000 … résolu =00000000`), et
`GEOMCHECK` compte 100 % de sommets `0 0 0`. Aucune lecture après libération : ni GLEngine ni le
plugin n'y sont pour rien. Les « pointeurs de liste libre » du 23/09 étaient le contenu d'un
malloc réutilisé, jamais réécrit ; les blocs nuls de 70–90 Ko alignés sur la page, des mallocs
neufs (grande taille → pages fraîches).

Écarté en chemin : le processeur émulé. Avec tous les patches TCG éteints (`FASTFP=0 SRTLB=0
LFSINLINE=0 VFPFAST=0 VPERMFAST=0 FPINLINE=0 RETINLINE=0 JCIDX=0 JITNEAR=0`), mêmes sommets fous.

## 2. La cause : IndirectX garde deux copies de chaque tampon de sommets [L]

`IdxDirect3DVertexBuffer9` (constructeur `0x19588`) alloue **deux** blocs de la taille du tampon :

| Champ | Rôle | Qui écrit | Qui lit |
|---|---|---|---|
| `+0x38` | copie « verrouillée » | le jeu (`Lock` rend `+0x38 + off`, `0x190a4`) | `Unlock`, `MapBufferContents` |
| `+0x3c` | copie **privée** | `Unlock` (chemin VAR), `MapBufferContents` | les dessins (`LockPrivate` rend `+0x3c + off`, `0x19120`) |
| `+0x40` | chemin VAR actif | = `device+0x9c8` au constructeur | |

`device+0x9c8` est posé une fois dans `IdxDirect3DDevice::InitOpenGL` (`0x400c`) :
`COpenGLUtilities::HaveExtension("GL_APPLE_vertex_array_range")` (chaîne en `0x98414`).

- **`Unlock` (`0x19308`)** : si `+0x40 == 0`, **il ne fait rien**. Sinon il allume
  `GL_VERTEX_ARRAY_RANGE_APPLE`, compare la zone verrouillée des deux copies (`memcmp`), recopie
  `+0x38 → +0x3c` (`memmove`), note la plage et appelle `glFlushVertexArrayRangeAPPLE`. La copie
  privée est la plage VAR (`glVertexArrayRangeAPPLE(taille, +0x3c)`, stockage
  `GL_STORAGE_SHARED_APPLE`, au constructeur `0x196a8`).
- **Chemin programme de sommets** (`IdxDirect3DDevice9::ActivateVertexShader`, `0x205c8`…) :
  `LockPrivate` puis `glVertexAttribPointerARB` sur **`+0x3c`**. Aucune recopie.
- Seul le chemin fixe (`DrawElementListFVF`, `0x26ba8`) appelle `MapBufferContents`, qui recopie
  la plage dessinée `+0x38 → +0x3c` — d'où le HUD et les décalcomanies justes.

Sans l'extension, les dessins sous programme (tout le monde en course) lisent donc la copie
privée **jamais écrite**. Aucun Mac à carte accélérée n'annonce pas cette extension : le chemin
sans VAR d'IndirectX n'a jamais servi ailleurs. Notre rendu, lui, héritait de la liste du
`GLDriver` logiciel d'Apple (bit 47 éteint, `version-extensions.md` §4).

## 3. Le correctif (plugin `20260926-var`)

`caps_extensions` pose le bit 47 de `cfg+0x124` (`GL_APPLE_vertex_array_range`) et
`cfg+0x8c = 0xFFFFF` (`GL_MAX_VERTEX_ARRAY_RANGE_ELEMENT_APPLE`, comme les pilotes matériels,
`capacites-glengine.md` §4). `POMPPC_GL_VAR=0` le retire (comparaison).

Ce que GLEngine en fait [L] : `_gleEvalDrawArraysOrElements_Entires_Exec` prend
`_gleDrawArraysOrElements_VAR_Exec` quand le VAO a une plage et le client state
`GL_VERTEX_ARRAY_RANGE_APPLE`, qui appelle `_gleExecuteVertexArrayRange` →
**`RenderVertexArray`** (`gctx+0x4708`, procédure `+0x70`) **sans regarder le retour**. Le plugin
la tient (`geom_render_array` → `geom_draw_client`, génériques compris) et **copie la plage au
moment du dessin** : `glFlushVertexArrayRangeAPPLE` (aucun appel pilote dans
`_glFlushVertexArrayRangeEXT_Exec`) et `glFinishObjectAPPLE(GL_VERTEX_ARRAY)` n'ont rien à
attendre, et un jeu qui réécrit la plage juste après le dessin reste juste.

Aucun changement de protocole pour cela. En revanche, pour **atteindre** la course, il a fallu :

## 4. Préalable : 4 contextes par client ne suffisaient plus (v19)

Depuis la v19 (`protocole-v19-transport.md`), chaque client n'a que `QGPU_MAX_CTX / 4` = **4**
contextes hôte. Colin McRae en crée **11** dès le menu (sonde « contexte créé », `note.txt`),
puis des contextes hors écran 256×256 pour écrire le nom des pilotes sur les voitures
(`CarNameDecals_Private_WriteToTexture`). Le 5e partait sans contexte hôte (`qctx -1`), donc en
logiciel chez Apple, qui plantait au chargement de la spéciale :

```
1 GLRasterARGB8888D32 glrPolyRGBA000 + 2176     ← bctrl sur r29+0x70 nul (échantillonneur d'unité)
2 GLDriver            gldTessellatePolygonRGBA_SmoothTexture
4 GLDriver-POMPPC     a_strip (repli)
… CarNameDecals_Private_WriteToTexture ← StageLoad_MainHandle
```

(`.run/cmr/r1/crash-contextes.txt`). Le 23/09 (v18, 16 contextes partagés) ce chemin passait.
`qgpu_proto.h` : `QGPU_MAX_CTX` 16 → 128, `QGPU_MAX_SURF` 64 → 128 (32 de chaque par client).
QEMU de référence reconstruit le 26/09 au soir (précédent : `~/src/qemu/build/*.avant-cmr`) ;
le kext ne change pas. Le cœur pèse ~9,5 Mio : `qgpu_core_test`, `qgpu_backend_test` et
`qgpu_replay` le gardent en statique (il débordait la pile de 8 Mio).

## 5. Épreuves

| Épreuve | Avant | Après |
|---|---|---|
| `gltest arbvpvar` (logique d'IndirectX à la lettre, §2 ; taille par défaut 64×64) | 7 échecs (plugin de `main`, QEMU `.avant-cmr`) | 0 échec |
| Rejeu natif d'un vidage en course (même plugin, `POMPPC_GL_VAR=0` / défaut) | éclats : `.run/cmr/r15-sansvar/rj-0005-f31966.png` | spéciale, voiture, paroi, arbres, brouillard : `.run/cmr/r14/rj-0005-f45417.png`, `.run/cmr/r13/rj-0010-f51179.png` |
| `GEOMCHECK` (sommets aberrants au `End`) | 100 % des lots de course | 0 |
| Capture 23/09 | `.run/cmr/cycle.png` | — |

`VERDICTCHECK=1 STATECHECK=1` (menu, chargement, spéciale ; images ~43 000–47 500) :
**0 écart** `VERDICT`, `TEXMEMO`, `STATE` (`note.txt` : « 0 depuis le début » sur 384 bilans).
Matrice des autres jeux (`bench/matrice/20260926-2216`, plugin `20260926-var`, QEMU reconstruit) :
9 vertes, Marble Blast fenêtre rouge (connu), vitesses inchangées (DOOM 3 59,4 / 59,6 ms/image).

La scène se lance **à la taille par défaut** : à 256×256 les témoins des scènes `arbvp*`
tombent hors du dessin (la lecture de `gltest` part du haut de l'image). `arbvp0cmr` garde ses
3 échecs du cas (g) (lot rectangle émulé par Apple), identiques sur `main`.

## 6. Ce qui reste : l'image affichée par la VM n'est pas encore celle de l'hôte

Le rejeu natif (le flux envoyé à l'hôte) montre la spéciale juste ; la capture de la VM au même
moment montre une image noire et blanche aux bonnes formes (`.run/cmr/r14/vm.png`). Chaque
image, le jeu rattache trois drawables hors écran (`attach kind 80 -> 0`, 400×300, 400×300,
200×150 ; à confirmer : qui les rend) et échantillonne une texture **rectangle**
(`fallback tex-target c/700d700` : masque 0xc = rectangle + 2D) — rendu vers texture d'IndirectX
(`aglSurfaceTexture`, clé `RenderTargetMethod`). Ces lots et les échanges se replient chez Apple
(~2 800 replis en deux minutes, `frames.csv`), qui ne sait pas les programmes de fragments :
c'est son image, sans textures, que la VM présente. Prochain chantier : textures rectangle et
surface-comme-texture dans le protocole (pas de repli : l'étendre).
