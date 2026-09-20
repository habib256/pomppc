# Le descripteur de sortie de sommet de GLEngine (18/09/2026)

Suite directe de `docs/re/verification-tcl.md` §6.4 (« codes d'entrée du
descripteur — non élucidés ») et §8.2. Objet : **la table complète des codes**
du descripteur publié en `cfg+0x11c`, et la sémantique exacte de ce que
GLEngine écrit dans le tampon de `BeginPrimitiveBuffer` quand le verrou T&L est
posé.

Méthode : lecture des désassemblages de 10.4.6 PPC (GLEngine, `GLDriver`,
`ATIRage128GLDriver`, `GeForce3GLDriver`, `ATIRadeonGLDriver`), puis vérification
dans l'invité avec la scène `gltest tclprobe` (un triangle, tous les attributs
distincts) et la variable `POMPPC_GL_TCL_DESC`. Chaque ligne est marquée
**[L]** établi par lecture, **[E]** confirmé par l'expérience, **[H]** hypothèse.

**Résumé en trois phrases.** Une entrée du descripteur est un `u16`
`(code << 10) | ((composantes − 1) << 8) | décalageEnMots` ; le **code est
l'indice d'attribut d'entrée** (0 position, 1 normale, 2 couleur, 3 brouillard,
4 couleur secondaire, 8..15 coordonnées de texture), le même indice qui repère
l'emplacement dans l'objet « tableau de sommets ». Ce que GLEngine dépose est
**l'attribut brut** : aucune transformation, aucun éclairage, aucun texgen,
aucune matrice de texture, aucun découpage, aucune élimination de face — et il
n'existe **aucun code donnant une sortie déjà transformée**. Le chemin
« intermédiaire » (option B) n'existe donc pas : c'est tout ou rien.

---

## 1. Où le descripteur est lu, copié et consommé [L]

| adresse | symbole | rôle |
|---|---|---|
| `0xc8d40` | `_gleUpdateDispatchCodeChange` | `cfg+0x11c → gctx+0x48d0`, `cfg+0x120 → gctx+0x48d4`, à **chaque** changement d'état |
| `0x9dc8` | `_gleSelectVertexSubmitFunc` | si `gctx+0x48d0 ≠ 0` **et** `gctx+0x7580 ≠ 0` **et** `gctx+0x4e1c == 0x1c00` : `gctx+0x4880 = octet 2 × 4` (pas d'un sommet, en octets). Sinon `gctx+0x487c = 0x100` et le tampon est le sommet **interne** de GLEngine (`gctx+0x4860`, 256 octets) |
| `0x1d330` | `_gleBuildVertexFuncNO` | recopie `((n+1)/2)+1` **mots** depuis `gctx+0x48d0` dans un tampon local, puis construit la fonction de sortie de sommet |
| `0x1d414` | → `_gleVPSetFuncOutputDesc` | variante « programme de sommets » : **fabrique** un descripteur au même format |
| `0x1d428` | → `_gleSetFuncOutputDesc` | variante « pipeline fixe logiciel » : **fabrique** un descripteur au même format |
| `0x4dd18` | `_gleSetFunctionIDFromArray` | relit chaque entrée et va chercher, **avec le code comme indice**, l'emplacement d'attribut du tableau de sommets |
| `0xb4d14` | `_gleSetFunctionIDFromList` | idem pour la liste d'attributs d'un programme de sommets |

`_gleBuildVertexFuncNO 0x1d318-0x1d33c` montre les trois conditions pour que
**notre** descripteur soit pris : `gctx+0x7580 ≠ 0` (le verrou T&L),
`gctx+0x4e1c == 0x1c00` (pipeline fixe, pas de programme ARB) et
`gctx+0x48d0 ≠ 0`.

## 2. En-tête du descripteur [L][E]

```
 octet 0 : n, nombre d'entrées            (_gleBuildVertexFuncNO 0x1d360 lbz r27,0(r11))
 octet 1 : 0x20 chez GLEngine             (_gleVPSetFuncOutputDesc 0xb4a10 oris r2,r0,0x20)
 octet 2 : pas d'un sommet, en MOTS de 4  (_gleSelectVertexSubmitFunc 0x9e2c lbz r0,2(r2))
 octet 3 : 0
 puis    : n entrées de 16 bits, deux par mot, à partir du mot 1
```

`_gleVPSetFuncOutputDesc 0xb4a08-0xb4a30` écrit le mot 0 en une fois :
`(n << 24) | 0x00200000 | ((n << 10) & 0xFC00)` — l'octet 2 y vaut donc `4·n`,
ce qui dit que ce constructeur-là empile des `vec4` jointifs.
**[E]** L'octet 1 n'est pas nécessaire : notre descripteur le laisse à 0 et tout
fonctionne. L'octet 2 est bien le pas et il est **libre** : on a fait tourner
des pas de 4, 8, 12, 16, 20 et 28 mots.

## 3. Format d'une entrée [L][E]

```
 bits 15..10 : CODE de l'attribut          (_gleSetFunctionIDFromList 0xb4da0
 bits  9.. 8 : nombre de composantes − 1    rlwinm. r9,r0,0x16,0xa,0x1f = entrée >> 10)
 bits  7.. 0 : décalage dans le sommet, en MOTS de 4 octets
```

Construction par GLEngine, `_gleVPSetFuncOutputDesc 0xb4c00-0xb4c24` :

```
 b4c00  slwi 9, 10, 4      ; indice × 16 octets
 b4c0c  slwi 0, 11, 10     ; CODE << 10
 b4c10  srawi 9, 9, 2      ; → décalage en mots
 b4c18  or   0, 0, 9
 b4c20  ori  0, 0, 768     ; | 0x300 : 4 composantes
 b4c24  sth  0, 4(2)
```

**[E]** Vérification directe : descripteur `n=2`, pas 8 mots, entrées
`0x0300` (code 0, 4 comp., mot 0) et `0x0B04` (code 2, 4 comp., mot 4) :

```
    s0 : 1 2 3 1 0.11 0.22 0.33 0.44
    s1 : 4 5 6 1 0.51 0.52 0.53 0.54
    s2 : 7 8 9 1 0.61 0.62 0.63 0.64
```

C'est exactement `glVertex3f(1,2,3)` et `glColor4f(0.11,0.22,0.33,0.44)`.
Le champ « composantes » est respecté : code 1 à 3 composantes n'écrit que
trois mots, le quatrième garde le motif témoin.

> Le relevé précédent (§6.4 de `verification-tcl.md`) essayait les entrées
> brutes `0`, `1`, `2`, `4` : ce sont des **décalages** de code 0, pas des
> codes. D'où la « duplication de la position » observée — elle est ici
> entièrement expliquée : `{0,1}` écrit la position au mot 0 puis au mot 1.

## 4. La table des codes

Source de lecture : `_gleSetFunctionIDFromArray 0x4de6c`
(`mulli r2,r30,0x18 ; add r2,r2,r22` avec `r22 = V+0x30`, `V` = objet tableau de
sommets `*(u32*)(gctx+0x4a60)`) — **l'emplacement d'attribut est `V+0x30+0x18·code`**,
et le bit d'activation est le bit `code+16` du masque 64 bits en `V+0x330:V+0x334`
(`0x4dde0 ashldi3(1, code+16)`). Il suffit donc de regarder où chaque
`gl*Pointer` écrit son emplacement :

| code | attribut | emplacement [L] | comp. | valeur observée [E] |
|---|---|---|---|---|
| 0 | **position d'objet** | `V+0x30` (`_glVertexPointer_Exec 0x4cefc`) | 4 | `1 2 3 1` — `w` complété à 1 |
| 1 | **normale** | `V+0x48` (`_glNormalPointer_Exec 0xa398c`) | 3 | `0.1 0.2 0.3` |
| 2 | **couleur primaire** | `V+0x60` (`_glColorPointer_Exec 0x4cb7c`) | 4 | `0.11 0.22 0.33 0.44` |
| 3 | **coordonnée de brouillard** | `V+0x78` (`_glFogCoordPointer_Exec 0xa3f78`) | 1 | `0.9` |
| 4 | **couleur secondaire** | `V+0x90` (`_glSecondaryColorPointer_Exec 0xa3c88`) | 3 | `0.71 0.72 0.73` |
| 5 | **poids** (`GL_ARB_vertex_blend`) | `V+0xa8` (`_glWeightPointerARB_Exec 0xa420c`) | 1..4 | `1` (valeur courante par défaut) |
| 6 | **drapeau d'arête** | `V+0xc0` (`_glEdgeFlagPointer_Exec 0xa4510`) | 1 | ⚠ **plante GLEngine** (voir §5) |
| 7 | non identifié | `V+0xd8` | 4 | `0 0 64 64` (= le viewport de l'essai) |
| 8..15 | **coordonnées de texture, unité 0..7** | `V+0xf0 + 0x18·u` (`_glTexCoordPointer_Exec`) | 4 | u0 `0.5 0.25 0 1`, u1 `0.75 0.125 0 1`, u≥2 `0 0 0 1` |
| 16..31 | **attributs génériques 0..15** (`glVertexAttribPointerARB`) | `V+0x1b0 + 0x18·(code−16)` | 4 | code 16 = **alias de la position** (`1 2 3 1`), 17..31 = `0 0 0 1` |
| 32,33 | matériau **ambiant** avant / arrière | constante d'état | 4 | `0.31 0.32 0.33 0.34` (deux fois) |
| 34,35 | matériau **diffus** avant / arrière | constante d'état | 4 | `0.41 0.42 0.43 0.44` |
| 36,37 | matériau **spéculaire** avant / arrière | constante d'état | 4 | `0.81 0.82 0.83 0.84` |
| 38,39 | matériau **émission** avant / arrière | constante d'état | 4 | `0.91 0.92 0.93 0.94` |
| 42, 43 | internes du chemin **logiciel** (`_gleSetFuncOutputDesc 0x39fec`) | — | 1 | non essayés |
| 44 | interne du chemin **logiciel** (émis quand `gctx+0x7580 == 0`) | — | 1 | `0x01000000` |

Corroborations de lecture :

- `_gleSetFunctionIDFromArray 0x4de10-0x4de4c` : le code 0 dont le tableau n'est
  pas actif **bascule sur l'emplacement 16** (`r21 = r22+0x180`) en posant
  `entrée |= 0x4000` (c'est-à-dire `code |= 16`) — d'où l'aliasing observé.
- `0x4de90-0x4dea4` : les codes `0x20..0x27` (32..39) ne viennent d'aucun
  tableau (`li r5,0`) et valent **4 composantes** ; au-delà, 1 composante.
- `_gleSetFuncOutputDesc 0x39cc0-0x3a03c` construit le descripteur du **chemin
  logiciel** avec les mêmes codes et des décalages fixes dans le sommet interne
  de 64 mots : code 0 au mot 8, code 2 au mot 12, code 1 au mot 16, code 4 au
  mot 20, code 3 au mot 23, code 5 au mot 24, code 8+u au mot 32+4u. Les
  conditions d'émission confirment la sémantique : le code 3 n'est émis que si
  `gctx+0x3166 == 0x8451` (`GL_FOG_COORDINATE`) → **brouillard** ; le code 6
  que si le mode de polygone n'est pas `GL_FILL|GL_FILL` (`gctx+0x34d0 ≠
  0x1b021b02`) → **drapeau d'arête** ; le code 1 quand l'éclairage est allumé
  (`gctx+0x30aa`) → **normale** ; le code 2 quand il est éteint → **couleur**.

## 5. Codes à ne pas demander [E]

- **Code 6 (drapeau d'arête)** : `BeginPrimitiveBuffer` est appelé, puis le
  processus meurt avant `EndPrimitiveBuffer`, avec 1 comme avec 4 composantes.
  À éviter ; le mode de polygone non rempli devra rester hors domaine, ou passer
  par un autre canal.
- **Codes 42..44** : internes au chemin logiciel, sans intérêt (le 44 écrit la
  constante `0x01000000`).

## 6. Ce que GLEngine écrit vraiment — réponses expérimentales

Toutes ces réponses viennent de la scène `tclprobe` (modèle-vue =
`glTranslatef(10,20,30)`, attributs distincts), descripteur
`16,0:0:4,1:4:3,2:8:4,8:12:4`.

| question | réponse [E] |
|---|---|
| position transformée ? | **non** : `1 2 3 1` avec une modèle-vue de translation (10,20,30) et, en `TCLP_PERSP`, une projection en perspective. Coordonnées d'**objet**, `w` complété à 1. |
| éclairage fait par GLEngine ? | **non** : avec `GL_LIGHTING` + `GL_LIGHT0` directionnelle, on reçoit la **couleur brute** `0.11 0.22 0.33 0.44` et la **normale brute** `0.1 0.2 0.3`. Idem avec `GL_COLOR_MATERIAL`. **Il n'existe aucun code « couleur éclairée »** : l'éclairage est entièrement à notre charge. |
| coordonnées de texture avant ou après texgen ? | **avant** : `GL_OBJECT_LINEAR` sur S et T ne change rien (`0.5 0.25 0 1`). |
| avant ou après la matrice de texture ? | **avant** : `glTranslatef(100,200,0)` sur `GL_TEXTURE` ne change rien. |
| découpage fait par GLEngine ? | **non** : un triangle `(-5,-1,0) (5,-1,0) (0,5,0)` en projection identité (donc largement hors du volume de vue) arrive **tel quel, 3 sommets**. Même le triangle entièrement hors champ des autres essais est transmis. |
| élimination de face ? | **non** : `glEnable(GL_CULL_FACE)` avec un triangle orienté à l'envers donne les **3 sommets**, dans l'ordre de soumission. |
| `glDrawArrays` / `glDrawElements` ? | **avec le descripteur** : toujours `Begin`/`EndPrimitiveBuffer`. `RenderVertexBuffer` (+0x4c) et `RenderVertexArray` (+0x70) ne sont **jamais** appelées. `glDrawElements` est **déroulé** par GLEngine (3 sommets à plat). **Sans descripteur** (`POMPPC_GL_ARRAY=1`, 20/09/2026) : le plugin prend le canal GeForce3 — `RenderVertexArray` / `RenderVertexBuffer`, indices conservés (`docs/gpu-3d-tiger.md` §4.7). |
| listes d'affichage ? | **toujours** `Begin`/`EndPrimitiveBuffer`, une paire par `glCallList`, contenu identique à l'immédiat. |
| modes de primitive ? | transmis **tels quels** : `mode = 4` `GL_TRIANGLES`, `5` `GL_TRIANGLE_STRIP`, `6` `GL_TRIANGLE_FAN`, `7` `GL_QUADS`. GLEngine ne décompose pas les rubans ni les éventails. |
| groupement ? | un seul `Begin`/`End` par primitive : scène `game`, `mode=7`, **192 sommets** d'un coup. |

Extrait (scène `game`, descripteur position + normale + couleur + texcoord0) :

```
TCL BeginPrimitiveBuffer(ctx=0x2808e00 mode=7 *n=0) pas=64
TCL EndPrimitiveBuffer(ctx=0x2808e00 drapeau=2 mode=7 n=192) pas=64
    s0 : -2 -1.5 0 1 | 0 0 1 | 1 1 1 1 | 0 0 0 1
    s1 :  2 -1.5 0 1 | 0 0 1 | 1 1 1 1 | 2 0 0 1
```

## 7. Ce que font les pilotes de 10.4.6

**Aucun des trois pilotes disponibles ne publie `cfg+0x11c`** — tous les trois y
écrivent explicitement 0. [L]

| pilote | `cfg+0x78..0x7b` | `cfg+0x11c` / `+0x120` | table de procédures | retour de `gldUpdateDispatch` |
|---|---|---|---|---|
| `GLDriver` (Apple, logiciel) | `00 00 00 00` | 0 | complète | **4** (bit 0 nul) |
| `ATIRage128GLDriver` | `01 00 00 00` (`0xa5a4`, `0xa5b0`) | jamais écrits | `0x50` `0x54` `0x64` `0x70` `0x74` `0x78` `0x7c` | — |
| `GeForce3GLDriver` | `01 01 01 01` (`0x3a9b8`, `r28=1` en `0x3a938`) | `0` (`0x3ab98 stw r11,0x11c(r30)`) | `0x4c` `0x64` `0x68` `0x6c` `0x70` `0x74` `0x78` `0x7c` `0x80` — **pas** `0x50`/`0x54` | **7**, ou **6** si `ctx+0x20 ≠ 0` (`0x3cdcc-0x3cdd8`) |
| `ATIRadeonGLDriver` | `01 01 01 01` (`0x13ef8`, `r28=1` en `0x13e84`) | `0` (`0x1404c`, `0x14050`) | — | — |

Trois conséquences :

1. **Le relevé précédent se trompait sur le Rage 128** : il pose `cfg+0x78 = 1`,
   pas `cfg+0x79`. Il n'annonce **pas** de T&L matérielle — ce qui est cohérent,
   la carte n'en a pas.
2. **Le Rage 128 reçoit donc des sommets déjà transformés et éclairés par
   GLEngine**, dans le sommet **interne** de 64 mots (`gctx+0x4860`,
   `gctx+0x487c = 0x100`), à la disposition fixe de `_gleSetFuncOutputDesc`
   (§4). Il n'a pas besoin de descripteur : GLEngine en fabrique un lui-même.
   C'est le chemin « tampon de primitive **logiciel** ». [L]
3. **Le GeForce3 et le Radeon**, eux, annoncent la T&L matérielle
   (`cfg+0x79 = 1`, retour de dispatch = 7, bit 0 posé) mais laissent
   `cfg+0x11c = 0` et n'installent **pas** `Begin`/`EndPrimitiveBuffer` : ils
   passent par `RenderVertexArray` (+0x70) et `RenderVertexBuffer` (+0x4c), dont
   le format de sommet vient des six identifiants de `cfg+0x7c..0x87` — c'est de
   là que vient l'`idFormat` de `gldAllocVertexBuffer`. Le Rage 128 publie
   `{0x8003, 0x8002, 0x8001, 0x8006, 0x8005, 0x8004}` (`0xa6b0-0xa6e4`), le
   Radeon `{…, 2, …, 6, 5, 4}` (`0x13ff0-0x14030`), avec les pas **imposés par
   GLEngine** (`0x10 0x18 0x20 0x14 0x20|0x24 0x2c|0x34`, `verification-tcl.md` §7).
   **Le descripteur `cfg+0x11c` est donc le canal des cartes à programmes de
   sommets** (Radeon 9x00, GeForce FX), absentes de cette installation. [H]

## 8. Le chemin « intermédiaire » (option B) n'existe pas

Deux constats indépendants :

- **[L]** `_gleSelectVertexSubmitFunc 0x9e10-0x9e24` ne lit `gctx+0x48d0` que si
  `gctx+0x7580 ≠ 0`. Descripteur et verrou T&L vont ensemble ; il n'y a pas de
  réglage « transforme mais donne-moi le résultat au format que je demande ».
  Et la table des codes (§4) ne contient **aucun** code de position en
  coordonnées d'œil, de clip ou de fenêtre, ni de couleur éclairée : ce sont des
  codes d'**entrée**, indexant l'objet « tableau de sommets ».
- **[E]** Avec le verrou à 0 (`POMPPC_GL_TCL_BITS=0`), y compris en posant
  `cfg+0x78 = 1` comme le Rage 128, **`BeginPrimitiveBuffer` n'est jamais
  appelée** (0 appel sur la scène `tclprobe`). Le chemin « tampon de primitive
  logiciel » du Rage 128 n'est pas atteignable tant que le plugin enchaîne sur
  le `GLDriver` d'Apple, qui fournit ses propres entrées de rastérisation. [H]

Le choix est donc binaire : **attributs bruts, tout le pipeline de sommets à
notre charge**, ou le chemin actuel.

## 9. Ce que cela coûte, ce que cela rapporte

Mesure faite le 18/09/2026, scène `gltest spin` (60 images, 400 triangles =
1200 sommets par image, éclairage éteint, couleur par sommet) :

| taille | plugin actuel (T&L par GLEngine + tracé sur l'hôte) | verrou T&L posé, géométrie brute **jetée** | rendu logiciel d'Apple |
|---|---|---|---|
| 16×16 (rastérisation négligeable) | **415 img/s** | **788 img/s** | 481 img/s |
| 64×64 | 443 img/s | 771 img/s | 400 img/s |
| 256×256 | 423 img/s | 706 img/s | 225 img/s |

À 16×16, où la rastérisation ne compte pas, on passe de 2,41 ms à 1,27 ms par
image : **1,14 ms pour 1 200 sommets, soit ≈ 0,95 µs par sommet** de T&L
GLEngine + soumission à l'hôte, sur un total de 2,41 ms. La seconde colonne est
une **borne supérieure** (rien n'est dessiné du tout) : le gain réel sur une
scène géométrique est au plus **×1,9**.

## 10. Décision d'architecture

**Option A — attributs bruts (`DRAW_RAW` du protocole v7) — retenue.**

Ce que le plugin demande dans `cfg+0x11c` : codes 0 (position), 1 (normale),
2 (couleur), 4 (couleur secondaire), 3 (brouillard), 8..15 (coordonnées de
texture des unités actives), avec un pas ajusté à l'état courant.

Ce qui reste **sur le PowerPC émulé** avec A :
- la lecture des tableaux de sommets et le déroulement des indices (GLEngine le
  fait, on ne peut pas l'éviter sur ce chemin) ;
- la recopie des attributs bruts dans notre tampon (≈ 1,27 ms pour 1 200 sommets
  à 16×16, soit la moitié du coût actuel) ;
- le suivi d'état : matrices, lumières, matériaux, texgen, plans de découpe à
  relever dans le bloc GLEngine et à envoyer à l'hôte **quand ils changent**.

Ce qui part **sur le GPU de l'hôte** : modèle-vue, projection, division
perspective, viewport, éclairage complet, texgen, matrice de texture,
découpage et élimination de face — GLEngine n'en fait **aucun** sur ce chemin
(§6), donc l'hôte doit tout refaire, ce qui tombe bien : c'est exactement ce
qu'un pipeline fixe OpenGL de l'hôte sait faire nativement.

Ce que l'option A impose :
- **le repli reste obligatoire** et se fait par lot d'état, en rendant le bit 0
  du retour de `gldUpdateDispatch` (`verification-tcl.md` §3) : dès qu'un état
  sort du domaine (mode de polygone non rempli — le code 6 plante,
  `GL_ARB_vertex_program`, brouillard par fragment, plus de plans de découpe que
  l'hôte n'en a…), on rend la main à GLEngine, image exacte garantie ;
- **l'exactitude de l'éclairage** devient notre responsabilité : la couleur
  arrive brute, la normale non normalisée et non transformée.

**Option B écartée** : le descripteur ne donne aucune sortie transformée (§8),
et le seul chemin « sommets déjà transformés » de GLEngine (celui du Rage 128)
laisse **100 % de la T&L sur le PowerPC** — il ne ferait gagner que le
groupement des appels, pas le calcul.

## 11. Outillage (derrière `POMPPC_GL_TCL` uniquement)

| variable | effet |
|---|---|
| `POMPPC_GL_TCL_DESC=pas,e0,e1,…` | publie un descripteur en `cfg+0x11c` ; `pas` en mots de 4 octets ; chaque entrée est soit brute (`0x0300`), soit **`code:décalage[:composantes]`** (composantes = 4 par défaut) |
| `POMPPC_GL_TCL_78=v` | pose `cfg+0x78 = v` (défaut : inchangé) |

Scène `gltest tclprobe` : un triangle, positions `(1,2,3) (4,5,6) (7,8,9)`,
normales `(0.1,0.2,0.3)…`, couleurs `(0.11,0.22,0.33,0.44)…`, couleur secondaire
`(0.71,0.72,0.73)…`, brouillard `0.9 1.9 2.9`, texture unité 0 `(0.5,0.25)…` et
unité 1 `(0.75,0.125)…`, modèle-vue `glTranslatef(10,20,30)`. Réglages par
variables d'environnement : `TCLP_MODE=imm|arrays|elements|list`, `TCLP_PERSP`,
`TCLP_LIGHT`, `TCLP_COLORMAT`, `TCLP_TWOSIDE`, `TCLP_MATERIAL`, `TCLP_TEX`,
`TCLP_TEXGEN`, `TCLP_TEXMAT`, `TCLP_CULL`, `TCLP_CULLFRONT`, `TCLP_BACKFACE`,
`TCLP_CLIP`, `TCLP_FOG`, `TCLP_FOGCOORD`, `TCLP_SECCOL`, `TCLP_UNFILLED`,
`TCLP_EDGE`. La scène ne vérifie aucun pixel : c'est la trace de
`EndPrimitiveBuffer` qui est le résultat.

## 12. Non-régression (plugin modifié, sans `POMPPC_GL_TCL`)

```
tri        : OK (0 échec(s))
tex        : OK (0 échec(s))
comb       : OK (0 échec(s))
game       : OK (0 échec(s))
varray     : OK (0 échec(s))
stencil    : OK (0 échec(s))
varray : IMAGES IDENTIQUES   (plugin vs POMPPC_GL_DISABLE=1)
```
