# Tableaux de sommets, tampons, programmes de pipeline et état T&L de GLEngine

Relevé de rétro-ingénierie — Mac OS X 10.4.6 PPC, `OpenGL.framework` et trois pilotes
(`GLDriver` logiciel d'Apple, `GeForce3GLDriver`, `ATIRage128GLDriver`).
Méthode : désassemblage annoté par `tools/re/ppcanno.py`, lecture ciblée. PowerPC 32 bits
big-endian, ABI Mach-O (arguments entiers `r3..r10`, flottants `f1..f13`).

Objectif : recevoir dans le plugin la géométrie **avant** transformation, pour faire la
transformation et l'éclairage sur le GPU de l'hôte (tâches 1.2 et 2.1 de `docs/todo-gpu-3d.md`).

Convention de ce document :
- **[L]** = établi par lecture du code (adresse et extrait donnés) ;
- **[H]** = hypothèse, cohérente mais non prouvée ;
- les adresses `0x…` sans autre précision sont des adresses de fichier dans le binaire cité.

---

## 0. Le résultat qui conditionne tout le reste : `ctx_pilote+0x0c` = `gctx + 0x360`

Le bloc d'« état GL » que le pilote reçoit (`CTX_GLSTATE`, `pomppc_accel.c`) n'est pas la
structure de contexte de GLEngine : c'est **une sous-structure de celle-ci, à l'offset 0x360**.
Dans toutes les fonctions `_gl<Nom>_Exec` de `GLEngine`, `r3` est le contexte GLEngine (`gctx`) ;
les offsets relevés dans ces fonctions sont donc **des offsets `gctx`**, et il faut leur
retrancher `0x360` pour obtenir l'offset `GS_*` vu par le plugin.

| Preuve | Binaire + adresse | Extrait |
|---|---|---|
| L'initialiseur du bloc pilote reçoit `gctx+0x360` **[L]** | GLEngine `0x3df4` | `addi r3, r29, 0x360` / `bl 0x457c ; _gleInitGLDState` |
| `glScissor` écrit en `gctx+0x34e0`, le plugin lit `GS+0x3180` | GLEngine `0x205bc` | `stw r4, 0x34e0(r3)` … `stw r7, 0x34ec(r3)` |
| `glLineWidth` → `gctx+0x3180`, plugin `GS+0x2e20` | GLEngine `0x3a33c` | `stfs f1, 0x3180(r3)` |
| `glAlphaFunc` → `gctx+0x30c0/0x30c4`, plugin `GS+0x2d60/0x2d64` | GLEngine `0x164e8` | `stfs f1, 0x30c0(r3)` / `sth r4, 0x30c4(r3)` |
| `glDepthFunc` → `gctx+0x3124`, plugin `GS+0x2dc4` | GLEngine `0x31628` | `sth r4, 0x3124(r3)` |
| `glShadeModel` → `gctx+0x34f4`, plugin `GS+0x3194` | GLEngine `0x31514` | `stw r4, 0x34f4(r3)` |

Les cinq correspondances tombent sur les offsets `GS_*` **déjà vérifiés sur pièces** dans
`guest/gldriver/pomppc_accel.c` : la constante `0x360` est établie.

`_gleInitGLDState` (GLEngine `0x457c–0x5180`) est donc **la carte du bloc visible par le
pilote**. Les offsets qu'il écrit s'étendent de `GS+0x1810` à `GS+0x4310` au moins (plus les
boucles par unité de texture et par lumière). Conséquence pratique immédiate :

> Le traceur `POMPPC_GLTRACE_STATE` ne vide aujourd'hui que **0x4000 octets**
> (`guest/gldriver/pomppc_gld.c:192`, `pomppc_dump("clear-glstate", gls, 0x4000)`).
> Le bloc est plus grand : **passer le vidage à 0x5400** avant d'écrire les sondes ci-dessous,
> sinon les plans de découpe (`GS+0x3e28`) sont au bord et la zone `GS+0x42c0` est tronquée.
> — **Fait le 18/09/2026** : le vidage est à `0x5400`, et les objets pointés par `GS+0x4700`
> (tableau de sommets), `GS+0x4a70`/`GS+0x4a74` (matériaux) et `GS+0x50c0` (pipeline program) sont
> vidés eux aussi à chaque `glClear` (préalables 7.0.1 à 7.0.4).

---

## 1. Les entrées « hautes » du pilote : signatures

Rappel du mécanisme : `_glepPluginConnect` remplit une table de pointeurs dans la structure
« plugin » (`+0x114`…`+0x204`, cf. `re/gld_table.txt`). Dans `GLEngine`, le tableau des rendus
a un pas de **`0x2e0`** et se lit ainsi (motif systématique, `addis rX, gctx, 1` = `gctx+0x10000`) :

```
000065ec  addis    r26, r3, 1                    ; r26 = gctx + 0x10000
000065f8  lbz      r0, -0x7a5c(r26)              ; gctx+0x85a4 : NOMBRE de rendus (u8)
00006614  lwz      r2, -0x777c(r30)              ; gctx+0x8884 + i*0x2e0 : structure PLUGIN
0000661c  lwz      r3, -0x7a54(r30)              ; gctx+0x85ac + i*0x2e0 : CONTEXTE du pilote
00006624  lwz      r12, 0x1a4(r2)                ; entrée gldCreateVertexArray
```
**[L]** GLEngine `0x65e0–0x6650` (`_gleCreatePluginVertexArray`).
Toutes les entrées ci-dessous sont appelées **pour chaque rendu chargé**, en boucle.

### 1.1 Tableaux de sommets

| Entrée | Appelant GLEngine | Signature relevée |
|---|---|---|
| `gldCreateVertexArray` `+0x1a4` | `_gleCreatePluginVertexArray` `0x65e0` | `(ctx, void **poignée_sortie, attribs, bloc_plugin)` **[L]** |
| `gldModifyVertexArray` `+0x1a8` | `_gleModifyPluginVertexArray` `0x4eff8` | `(ctx, poignée)` **[L]** |
| `gldFlushVertexArray` `+0x1ac` | `_gleFlushPluginVertexArray` `0x44964` | `(ctx, poignée, taille, pointeur, premier)` → `0`/`1` **[L]** |
| `gldDestroyVertexArray` `+0x1b0` | `_gleDestroyPluginVertexArray` `0x302c4` | `(ctx, poignée)` **[L]** |
| `gldReclaimVertexArray` `+0x1b4` | `_gleReclaimVertexArrayResources` `0x104260` | `(ctx, poignée)` **[L]** |

Détail de `gldCreateVertexArray`, avec `A` = l'objet passé au plugin (voir §2) et `p` = index du rendu :

```
000065e0 _gleCreatePluginVertexArray:   ; r3 = gctx, r4 = A
00006608  addi     r25, r4, 0x20                 ; r5 = A+0x20  : TABLEAU DES 32 ATTRIBUTS
0000660c  addi     r29, r4, 0x3d0                ; r6 = A+0x3d0 : bloc par rendu (pas 0x8c)
00006618  mr       r4, r28                       ; r4 = &A[4*p] : où écrire la poignée
00006640  bctrl
```
Confirmation par l'implémentation d'Apple : `GLDriver` `_gldCreateVertexArray` `0x19bcc`
écrit une poignée bidon dans `*r4` et rend 0 **[L]** :
```
00019bcc  li       r0, 4
00019bd0  li       r3, 0
00019bd4  stw      r0, 0(r4)
00019bd8  blr
```
et par `GeForce3GLDriver` `0x9410`, qui alloue un objet et y range **les deux pointeurs** :
```
9428:  addi 3, 1, 64          ; &obj
9430:  bl .-92                ; alloc
943c:  stw 29, 0(2)           ; obj[0x00] = r5 = tableau des attributs
9444:  stw 28, 4(9)           ; obj[0x04] = r6 = bloc par rendu
944c:  stw 0, 0(27)           ; *r4 = obj
9450:  li 3, 0                ; rend 0
```

`gldFlushVertexArray` : le 5e argument est un drapeau « premier rendu », et la valeur de retour
`1` signifie « j'ai consommé le vidage » (GLEngine met alors le drapeau à 0 pour les rendus
suivants) **[L]**, GLEngine `0x449b8–0x44a14` :
```
000449b8  mr       r5, r21 / mr r6, r22 / mr r7, r23      ; taille, pointeur, premier
000449e8  lwz      r12, 0x1ac(r9)  ; mtctr ; bctrl
00044a08  cmpwi    cr7, r3, 1
00044a0c  bne      cr7, 0x449b0
00044a10  li       r23, 0
```
Côté `GeForce3GLDriver` `0x9548`, la lecture est symétrique **[L]** : `cmpwi 7, 7, 0` (teste `r7`),
`mr 4, 6` (`r4 = pointeur`), et `li 0, 1` / `mr 3, 0` sur la branche prise.

Les couples (taille, pointeur) proviennent de `glFlushVertexArrayRangeAPPLE`
(GLEngine `0x4f728`, `r5 = longueur`, `r6 = début`) et de l'objet interne construit par
`_gleCreateDrawArraysOrElementsVertexObject` (`0x4491c–0x44950`).

### 1.2 Ce que fait un vrai pilote de ces entrées (comparaison demandée)

| Pilote | `CreateVertexArray` | `ModifyVertexArray` | `FlushVertexArray` |
|---|---|---|---|
| `GLDriver` (logiciel d'Apple) `0x19bcc` | écrit `4` dans `*r4`, rend 0 | `li r3,0 ; blr` | `li r3,0 ; blr` |
| `GLRendererFloat` `0x6468` | identique à GLDriver | stub | stub |
| `ATIRage128GLDriver` `0x815c` | `li 3,0 ; blr` (**stub total**) | stub | stub |
| `GeForce3GLDriver` `0x9410` | **alloue un objet réel** et mémorise les deux pointeurs | lit l'état, le compare, réémet le paramétrage du DMA | déclenche un vidage réel |

`_gldRenderVertexArray` existe aussi dans le `GLDriver` d'Apple (`0x19bf4`) et se contente de
`li r3, 0 ; blr` **[L]** : la table de procédures a bien l'entrée `+0x70`, et **`0` y signifie
« non pris en charge »**, ce qui rend GLEngine à son chemin logiciel.

**Conclusion [L]** : la carte à T&L matériel (GeForce3) est la seule à implémenter le chemin
haut ; la Rage 128 (sans T&L) le refuse intégralement et laisse GLEngine transformer. Le plugin
POMPPC doit donc se modeler sur `GeForce3GLDriver`, pas sur le `GLDriver` logiciel.

`GeForce3GLDriver` lit d'ailleurs le bloc d'état pilote aux offsets `GS+0x1810/0x1814/0x1818/0x181c`
(16, 12, 3 et 3 occurrences — échelle de viewport, §5), `GS+0x1840…0x184c` (viewport entier),
`GS+0x3176` (mode de cull) et `GS+0x3e28` (masque des plans de découpe) **[L]** :
c'est la confirmation que ces champs-là sont bien ceux qu'un pilote à T&L consomme.

`gldModifyVertexArray` de GeForce3 lit `attribs + 0x312`, comparé à `34238 = 0x85BE`
(`GL_STORAGE_CACHED_APPLE`) **[L]**, `GeForce3GLDriver 0x94a8` :
```
94a8:  lhz 0, 786(9)          ; 9 = attribs (= A+0x20) ; 786 = 0x312 -> A+0x332
94b0:  ori 2, 2, 34238        ; 0x85BE GL_STORAGE_CACHED_APPLE
94b4:  cmpw 7, 0, 2
```
ce qui prouve à la fois l'offset `A+0x332` (§2.3) et que `r5` est bien `A+0x20`.

---

## 2. L'objet « tableau de sommets » passé au pilote

### 2.1 Allocation et adressage

`_gleCreateVertexArrayObject` (GLEngine `0x6434`) alloue `0x3e0 + n_rendus * 0x8c` octets.
`_gleInitVertexArrayState` (`0x63a0`) appelle ensuite `_gleCreatePluginVertexArray` avec
**`objet + 0x10`** :
```
000063b8  bl 0x6434 ; _gleCreateVertexArrayObject
000063bc  addi r28, r3, 0x10
000063cc  bl 0x65e0 ; _gleCreatePluginVertexArray      (r4 = objet+0x10)
000063dc  stw r30, 0x4a64(r29)     ; gctx+0x4a64 = objet par défaut
000063e4  stw r30, 0x4a60(r29)     ; gctx+0x4a60 = objet COURANT
```
**[L]**. Dans tout ce qui suit, `V` = base de l'objet, **`A = V + 0x10`** = ce que voient les
fonctions `_gle*PluginVertexArray` et le pilote. `gctx+0x4a60` désigne l'objet courant.

| Offset (`V`) | Offset (`A`) | Type | Sens | Preuve |
|---|---|---|---|---|
| `0x00` | — | ptr | chaînage de la table de hachage des noms | `0x104320 lwz r27, 0(r27)` **[L]** |
| `0x04` | — | u32 | nom GL (argument de création) | `0x6490 stw r29, 4(r3)` **[L]** |
| `0x08` | — | i32 | compteur de références | `0x6414–0x641c`, `0x104298 cmpwi r0, 1` **[L]** |
| `0x0c` | — | ptr | destructeur `_gleFreeVertexArrayObject` | `0x6468/0x6488` **[L]** |
| `0x10 + 4p` | `0x00 + 4p` | ptr | **poignée du rendu `p`** (8 emplacements) | `0x6560` (boucle de 8 mises à zéro) ; `0x44ae8` ; `0x302f4` **[L]** |
| `0x30 + 0x18a` | `0x20 + 0x18a` | struct | **tableau des 32 attributs**, pas `0x18` | `0x64a0–0x64c4` (`mtctr 0x20`, `addi r2, r2, 0x18`) **[L]** |
| `0x330/0x334` | `0x320/0x324` | u64 | masque « attribut activé » (mot haut / mot bas) | `_gleSetClientEnableFlag 0x44cf8` **[L]** |
| `0x338/0x33c` | `0x328/0x32c` | u64 | masque « attribut servi par un VBO » | `_glVertexPointer_Exec 0x4cf4c–0x4cf60` **[L]** |
| `0x342` | `0x332` | u16 | `GL_VERTEX_ARRAY_STORAGE_HINT_APPLE` (défaut `0x85B4` CLIENT) | `0x650c` ; `_glVertexArrayParameteriEXT_Exec 0x56f2c/0x56f54` **[L]** |
| `0x344` | `0x334` | ptr | pointeur de `glVertexArrayRangeAPPLE` | `_glVertexArrayRangeEXT_Exec 0x4efb0` **[L]** |
| `0x348` | `0x338` | u32 | longueur de `glVertexArrayRangeAPPLE` | `_glVertexArrayRangeEXT_Exec 0x4efac` **[L]** |
| `0x34c` | `0x33c` | u8 | inconnu (mis à 0 à l'initialisation) | `0x653c` — **[H]** |
| `0x350/0x354` | `0x340/0x344` | u64 | **masque « sale »** transmis aux rendus | `_gleFlushPluginVertexArray 0x44984` **[L]** |
| `0x358` | `0x348` | ptr | objet tampon lié à `GL_ARRAY_BUFFER` | `_glVertexPointer_Exec 0x4cf00` **[L]** |
| `0x35c` | `0x34c` | u32 | inconnu (0 à l'initialisation) | `0x6540` — **[H]** |
| `0x360 + 4a` | `0x350 + 4a` | ptr | objet tampon GLEngine de l'attribut `a` (32 mots) | `0x6544` ; `0x4cff0 stw r29, 0x350(r30)` **[L]** |
| `0x3e0 + p*0x8c` | `0x3d0 + p*0x8c` | struct | **bloc par rendu** (voir 2.4) | `0x6594`, `0x660c` **[L]** |

### 2.2 Numérotation des attributs (32 emplacements)

`_gleSetClientEnableFlag` (GLEngine `0x44b18`) traduit l'énumération GL en numéro de bit ; le bit
de l'attribut `a` est **`16 + a`** dans un masque 64 bits dont `A+0x320` est le mot **haut** et
`A+0x324` le mot **bas** **[L]** (`lis r12, 1` ⇒ `1<<16`, cf. `0x44c14`).
Vérification croisée : `_glArrayElement_Exec 0x6090–0x60b0` part du bit 15 du mot haut
(= bit 47 = attribut 31) et lit l'emplacement `A+0x308` = `A+0x20+0x18*31`.

| `a` | Attribut | Défaut (taille, type) | Base de l'emplacement | Énum `glEnableClientState` | Preuve |
|---|---|---|---|---|---|
| 0 | position | 4, `GL_FLOAT` | `V+0x30` | `0x8074` | `_glVertexPointer_Exec 0x4ced4 addi r23, r11, 0x30` **[L]** |
| 1 | normale | 3, `GL_FLOAT` | `V+0x48` | `0x8075` | `_glNormalPointer_Exec 0xa3964` **[L]** |
| 2 | couleur | 4, `GL_FLOAT` | `V+0x60` | `0x8076` | `_glColorPointer_Exec 0x4cb40` **[L]** |
| 3 | coordonnée de brouillard | 1, `GL_FLOAT` | `V+0x78` | `0x8457` | `_glFogCoordPointer_Exec 0xa3f50` **[L]** |
| 4 | couleur secondaire | 3, `GL_FLOAT` | `V+0x90` | `0x845e` | `_glSecondaryColorPointer_Exec 0xa3c60` **[L]** |
| 5 | poids (`GL_WEIGHT_ARRAY_ARB`) | 1, `GL_FLOAT` | `V+0xa8` | `0x86ad` | `_gleSetClientEnableFlag 0x44bec/0x44c54` **[L]** |
| 6 | drapeau d'arête | 1, `GL_UNSIGNED_BYTE` | `V+0xc0` | `0x8079` | `_glEdgeFlagPointer_Exec 0xa44e8` ; défaut `0x64fc/0x6530` **[L]** |
| 7 | **indices** (`GL_ELEMENT_ARRAY_APPLE`) | 1, `GL_UNSIGNED_INT` | `V+0xd8` | `0x8a0c` | `_gleSetClientEnableFlag 0x44bfc/0x44c9c` ; défaut `0x6508/0x651c` **[L]** |
| 8..15 | coordonnées de texture, unité `u = a-8` | 4, `GL_FLOAT` | `V+0x150+0x18u` | `0x8078` | `_glTexCoordPointer_Exec 0x4d1e0 addi r22, r10, 8` **[L]** |
| 16..31 | attribut générique ARB `i = a-16` | 4, `GL_FLOAT` | `V+0x270+0x18i` | — | `_glVertexAttribPointerARB_Exec 0xa4728 addi r20, r4, 0x10` **[L]** |

Le tableau d'indices de couleur (`glIndexPointer`, `GL_INDEX_ARRAY` `0x8077`) **n'est pas** un
attribut de ce tableau : il ne pose qu'un drapeau `gctx+0x48ea` (`_gleSetClientEnableFlag 0x44c68`) **[L]**.

### 2.3 Disposition d'un emplacement d'attribut (`0x18` octets)

| Offset | Type | Sens | Preuve |
|---|---|---|---|
| `+0x00` | ptr | **pointeur utilisateur brut** — c'est un *décalage* quand un VBO est lié | `_gleResetVACachePointers 0x44af0` (lit `slot+0x00`, ajoute `vbo+0x30`) ; `_gleCreateDrawArraysOrElementsVertexObject 0x448b0 stw r27, 0x20(r2)` **[L]** |
| `+0x04` | u32 | **pas effectif en octets** (= `taille × octets` si le pas GL vaut 0) | `_glVertexPointer_Exec 0x4d0cc/0x4d0d0` ; `_glArrayElement_Exec 0xa60ac mullw r0, r25, r0` **[L]** |
| `+0x08` | u16 | **type GL** (`0x1402` `GL_SHORT`, `0x1404` `GL_INT`, `0x1406` `GL_FLOAT`, `0x140a` `GL_DOUBLE`) | `0x4d0b8 sth r26, 8(r23)` **[L]** |
| `+0x0a` | u16 | **taille** (nombre de composantes) | `0x4d0c4 sth r24, 0xa(r23)` ; `_glArrayElement_Exec 0xa60a4 lhz r10, 0x1aa(r27)` **[L]** |
| `+0x0c` | u8 | **octets par composante** (2, 4 ou 8) | `0x4d0c0 stb r0, 0xc(r23)` **[L]** |
| `+0x0d` | u8 | drapeau « normalisé » (attributs génériques) | mis à 0 à l'init `0x64b8` — **[H]** |
| `+0x10` | u32 | **pas GL tel que passé** (0 = serré) | mis à 0 à l'init `0x64bc` ; lu comme pas utilisateur — **[H]** |
| `+0x14` | u32 | **signature compacte** : `(pas<<7) \| (normalisé<<6) \| ((taille<<4)&0x30) \| (type&0xf)` ; `-1` à l'init | `_glVertexPointer_Exec 0x4ce70–0x4cea8`, `0x4d0d8 stw r22, 0x14(r23)` ; `_glVertexAttribPointerARB_Exec 0xa46bc–0xa4700` **[L]** |

La signature `+0x14` est le champ que GLEngine compare pour savoir si quelque chose a changé
(`0x4d010 lwz r0, 0x14(r23) ; cmpw r0, r22 ; beq` → sortie sans rien faire) **[L]**.

**Le lien avec un VBO ne passe pas par l'emplacement** : il est stocké deux fois, une fois côté
GLEngine (`A+0x350+4a` = l'objet tampon) et une fois côté rendu (`A+0x3d0 + p*0x8c + 4a` =
la poignée du tampon chez le rendu `p`) **[L]**, `_glVertexAttribPointerARB_Exec 0xa4848–0xa4864` :
```
000a4848  slwi     r2, r20, 2          ; 4 * numéro d'attribut
000a484c  addi     r9, r21, 0x10       ; vbo + 0x10 + 4p = poignée du rendu p
000a4854  addi     r11, r2, 0x3d0      ; A + 0x3d0 + 4a
000a4864  stw      r0, 0(r11)          ; (puis r11 += 0x8c à chaque rendu)
```

Le cache d'adresses résolues vit dans `gctx`, hors du tableau : `gctx+0x48f8 + 4a`
(32 mots, `_gleInitBufferState 0x6340`), recalculé par `_gleResetVACachePointers` (`0x44ad0`) **[L]** :
```
00044ae0  addi     r11, r2, 0x30       ; emplacement 0
00044ae4  addi     r2, r2, 0x360       ; objets tampon
00044af0  lwz      r9, 0(r11)          ; pointeur brut
00044b00  lwz      r0, 0x30(r10)       ; + base des données du VBO
00044b08  stw      r9, 0(r3)           ; -> gctx+0x48f8+4a
```

### 2.4 Bloc par rendu (`A + 0x3d0 + p*0x8c`, 140 octets)

| Offset | Type | Sens | Preuve |
|---|---|---|---|
| `+0x00 + 4a` | ptr | poignée, chez ce rendu, du tampon lié à l'attribut `a` (32 mots) | `0xa4864` **[L]** |
| `+0x80` | u32 | inconnu (0 à l'init) | `0x65a8` — **[H]** |
| `+0x84` | u32 | masque « sale » accumulé, mot **haut** (`-1` à l'init) | `_gleFlushPluginVertexArray 0x449b0/0x449cc` **[L]** |
| `+0x88` | u32 | masque « sale » accumulé, mot **bas** (`-1` à l'init) | idem ; lu par `GeForce3GLDriver 0x9498 lwz 0, 136(2)` **[L]** |

Avant chaque appel à `gldModifyVertexArray` / `gldFlushVertexArray`, GLEngine **OU** le masque
global `A+0x340/0x344` dans le masque du rendu, puis remet le masque global à zéro. Le pilote
lit donc `+0x84/+0x88` pour savoir quels attributs relire — c'est exactement ce que fait GeForce3.

### 2.5 Bits du masque « sale » (`A+0x340` haut, `A+0x344` bas)

| Bit | Cause | Preuve |
|---|---|---|
| 0 | `glVertexArrayRangeAPPLE` ; reprise de ressources | `0x4efc0 ori r3, r3, 1` ; `0x1042f0` **[L]** |
| 1 | `glFlushVertexArrayRangeAPPLE` | `0x4f788 ori r10, r10, 2` **[L]** |
| 2 | `glVertexArrayParameteriEXT` (pname `0x897C`) | `0x56f9c ori r3, r3, 4` **[L]** |
| 3 | `glVertexArrayParameteriEXT` (`GL_VERTEX_ARRAY_STORAGE_HINT_APPLE` `0x851F`) | `0x56f64 ori r3, r3, 8` **[L]** |
| 4 | liaison ou destruction d'un objet tampon | `0x4cff4`, `0x58870 ori r3, r3, 0x10` **[L]** |
| `16+a` | l'attribut `a` a changé | tous les `gl*Pointer_Exec` **[L]** |

---

## 3. Objets tampon (VBO, OpenGL 1.5)

### 3.0 Deux tables recopiées dans `gctx` — pourquoi certains sites d'appel sont introuvables

À la sélection d'un rendu, GLEngine **recopie les 61 pointeurs `gld*`** dans son propre contexte **[L]**,
GLEngine `0x3088` :
```
00003088  lwz      r4, -0x777c(r21)     ; plugin[rendu]
0000308c  addi     r3, r26, 0x4754      ; destination gctx+0x4754
00003090  stb      r22, -0x7a58(r21)    ; gctx+0x85a8 = index du rendu courant
00003094  li       r5, 0xf4             ; 61 * 4 octets
00003098  addi     r4, r4, 0x114
0000309c  bl       0x107d98             ; _memcpy
```
D'où **`gctx + 0x4754 + (X − 0x114)` = `plugin + X`** (décalage constant `+0x4640`).
Quelques entrées ne sont appelées **que** par cette copie, pour le rendu courant :

| Copie | Entrée | Utilisée par |
|---|---|---|
| `gctx+0x481c` | `gldFlushBuffer` `+0x1dc` | `_glBufferData_Exec`, `_glBufferSubData_Exec`, `_glUnmapBuffer_Exec` **[L]** |
| `gctx+0x4834` | `gldTestMemoryPluginData` `+0x1f4` | `0xc7700` **[L]** |
| `gctx+0x4784` | `gldInitDispatch` `+0x144` | `0x9f00` **[L]** |

De même, `gldInitDispatch(ctx, gctx+0x4698, gctx+0x471c)` recopie **33 entrées** de procédures dans
`gctx+0x4698` ; l'entrée d'index 32 (`+0x80`, `PROC_BufferSubData`) est donc **`gctx+0x4718`** **[L]**
(preuve croisée : `GLDriver 0x3108 stw r9, 0x80(r4)` avec `r9 = _gldBufferSubData`).

### 3.1 Signatures

| Entrée | Appelant | Signature |
|---|---|---|
| `gldCreateBuffer` `+0x1d4` | `_gleCreatePluginBuffer 0xc70e4` | `(ctx, u32 *poignée_sortie, void **ptr_données_moteur, u32 *drapeaux_moteur)` **[L]** |
| `gldDestroyBuffer` `+0x1d8` | `_gleFreeBufferObject 0xc7160` | `(ctx, poignée)` **[L]** |
| `gldFlushBuffer` `+0x1dc` | `gctx+0x481c` (`0x59520`, `0x5903c`, `0x58bd8`) | `(ctx, poignée, void *ptr, u32 longueur)` **[L]** |
| `gldReclaimBuffer` `+0x1e0` | `_gleReclaimBufferObjectResources 0xc7b80` | `(ctx, poignée)` **[L]** |
| `gldPageoffBuffer` `+0x1e4` | `_gleSynchronizePluginBuffer 0xc7ae0` | `(ctx, poignée)` **[L]** |
| `BufferSubData` `+0x80` (procédure) | `gctx+0x4718`, `_glBufferSubData_Exec` | `(ctx, poignée, offset, taille, source)` → `0` = refus **[L]** |

`gldCreateBuffer` **[L]**, GLEngine `0xc7114–0xc713c` :
```
000c7118  mr       r4, r29              ; r29 = vbo+0x10 + 4i : où écrire la poignée
000c7120  addi     r6, r29, 0x38        ; r6 = vbo+0x48 + 4i : drapeaux par rendu
000c7124  lwz      r12, 0x1d4(r2)
000c7128  mr       r5, r26              ; r5 = vbo+0x30 : &pointeur des données
```
Le pilote conserve **les deux pointeurs** : `GeForce3GLDriver 0x4478–0x4484` alloue 32 octets et y range
`rec[0x00] = r5`, `rec[0x04] = r6` **[L]**.

### 3.2 Disposition de l'objet tampon de GLEngine (`_gleCreateBufferObject 0xc7208`, taille `0x48 + 4·n_rendus`)

| Offset | Type | Sens | Preuve |
|---|---|---|---|
| `+0x00` | ptr | chaînage de la table de hachage | `0xc7c34` **[L]** |
| `+0x04` | u32 | nom GL | `0xc7284` **[L]** |
| `+0x08` | i32 | compteur de références (init 1) | `0xc724c` **[L]** |
| `+0x0c` | ptr | destructeur `_gleFreeBufferObject` | `0xc7254` **[L]** |
| `+0x10 + 4i` | u32 | **poignée du rendu `i`** (8 emplacements) | `0xc7288` **[L]** |
| `+0x30` | ptr | **pointeur des données** (`vm_allocate`) | `0x58cd4` **[L]** |
| `+0x34` | u32 | taille VM arrondie à 4 Kio | `0x58c44/0x58cd0` **[L]** |
| `+0x38` | u32 | **taille logique** (`GL_BUFFER_SIZE`) | `0x58cc4`, `0x596b8` **[L]** |
| `+0x3c` | u16 | usage (défaut `0x88E4` `GL_STATIC_DRAW`) | `0xc7250/0xc7274` **[L]** |
| `+0x3e` | u16 | accès (défaut `0x88BA` `GL_READ_WRITE`) | `0xc7258`, `0x592ec` **[L]** |
| `+0x40` | u32 | jeton « copie résidente chez tel rendu » (0 = non) | `0x58fb0`, `0xc7b24` **[L]** |
| `+0x44` | u8 | tampon mappé | `0x592e4` / `0x59510` **[L]** |
| `+0x48 + 4i` | u32 | **drapeaux par rendu** (init `-1`) | `0xc72a8` **[L]** |

Protocole des drapeaux `+0x48+4i` **[L]** : le moteur pose `|= 3` après écriture CPU, `|= 2` après
`gldFinishObject`, `|= 1` après `gldReclaimBuffer` ; le **pilote efface les bits 0-1 dans `gldFlushBuffer`**
(`GeForce3GLDriver 0x46c0 rlwinm 0, 0, 0, 0, 29`).

Liaisons : `GL_ARRAY_BUFFER 0x8892` → `A+0x348` ; `GL_ELEMENT_ARRAY_BUFFER 0x8893` → `A+0x34c` ;
`GL_PIXEL_PACK_BUFFER 0x88EB` → `gctx+0x4a68` ; `GL_PIXEL_UNPACK_BUFFER 0x88EC` → `gctx+0x4a6c` **[L]**
(`_gleBindBufferObject 0xc72dc`). — Ceci lève les deux « inconnus » de la table §2.1 :
**`A+0x34c` est la liaison `GL_ELEMENT_ARRAY_BUFFER`.**

### 3.3 Chez les pilotes

| Pilote | `CreateBuffer` | `FlushBuffer` | `BufferSubData` |
|---|---|---|---|
| `GLDriver` `0x17e8` | poignée = **0**, rend 0 | `blr` | `0x2f70 : li r3,0 ; blr` (refus → `memcpy` du moteur) |
| `ATIRage128GLDriver` `0x450c` | poignée = 0 | `blr` | — |
| `GeForce3GLDriver` `0x4450` | `malloc(32)`, mémorise `r5`/`r6`, mémoire GPU allouée paresseusement | invalide la copie GPU **et acquitte les drapeaux du moteur** | chemin rapide DMA |

Pour POMPPC : `gldCreateBuffer` doit rendre une poignée non nulle et implémenter `gldFlushBuffer`
(téléversement vers un tampon hôte) ; `BufferSubData` peut rendre 0 au début (le moteur retombe sur
`memcpy` + `gldFlushBuffer`, correct mais lent).

---

## 4. Objets « pipeline program »

### 4.1 Signatures et sites d'appel

| Entrée | Site d'appel | Signature |
|---|---|---|
| `gldCreatePipelineProgram` `+0x190` | `_gleCreatePluginPipelineProgram 0x62b8` (site `0x62f8`) | `(ctx, u32 *poignée_sortie, void *descripteur)` — **3 arguments** **[L]** |
| `gldModifyPipelineProgram` `+0x194` | `_gleModifyPluginPipelineProgram 0x26254` (site `0x26294`) | `(ctx, poignée, u32 masque)` **[L]** |
| `gldDestroyPipelineProgram` `+0x1a0` | `_gleDestroyPluginPipelineProgram 0x3071c` (site `0x3075c`) | `(ctx, poignée)` **[L]** |
| `gldRelatePipelineProgram` `+0x198` | **aucun** dans GLEngine 10.4.6 | — **[L]** |
| `gldGetPipelineProgramInfo` `+0x19c` | **aucun** dans GLEngine 10.4.6 | — **[L]** |

Masque de `gldModifyPipelineProgram` : **1** = le texte du programme a changé
(`_glProgramStringARB_Exec 0x22be4`), **2** = paramètres locaux modifiés
(`_glProgramLocalParameter4*ARB_Exec 0x26224 li r5, 2`) **[L]**.

### 4.2 Lecture de la trace `gldCreatePipelineProgram(ctx 0186faa8 0186fac8 00000007 0186faf0 00000100)`

```
000062dc  addi     r29, r4, 0x498       ; r4 = ppobj+0x10  =>  r29 = ppobj+0x4a8
000062e0  addi     r26, r4, 0x4b8       ;                      r26 = ppobj+0x4c8
000062f4  mr       r5, r26
000062f8  lwz      r12, 0x190(r2)
0000630c  bctrl                         ; r6, r7, r8 JAMAIS écrits
```
**[L]**, GLEngine `0x62b8–0x6330`.

| Valeur tracée | Interprétation | Statut |
|---|---|---|
| `r4 = 0186faa8` | `ppobj+0x4a8` = où écrire la poignée | **[L]** |
| `r5 = 0186fac8` | `ppobj+0x4c8` = descripteur public (écart de `0x20` exactement) | **[L]** |
| `r6 = 7` puis `2` | **résidu de registre** (rien ne l'écrit ; aucun des trois pilotes ne le lit) | **[L]** pour « non-argument », **[H]** pour « indice de seau de `malloc` » |
| `r7 = 0186faf0` | résidu : `ppobj+0x4f0` = tableau des paramètres locaux (`0x186faa8−0x4a8+0x4f0`) | **[L]** |
| `r8 = 0x100` puis `0x80` | résidu : compteur de la boucle d'initialisation = **nombre de paramètres locaux** (256 pour le programme de sommets, 128 pour celui de fragments) | **[L]** |

Autrement dit **`7` et `2` ne sont pas une cible** ; la cible est dans l'objet, à `+0x4c8` :
`_glePipelineProgramTargetExtactor 0xe1d34` lit `ppobj+0x4ca` et rend `0x8620 GL_VERTEX_PROGRAM_ARB`
pour le type 0, `0x8804 GL_FRAGMENT_PROGRAM_ARB` pour le type 1 **[L]**.

### 4.3 Oui, GLEngine crée des pipeline programs **sans aucun programme ARB**

`_gleInitPipelineProgramState 0x5e5c`, appelée à la création du contexte, boucle sur le type 0 puis 1 **[L]** :
```
00005ea0  mr       r4, r27              ; 0 puis 1
00005ea4  li       r5, 0                ; nom GL = 0
00005eb0  bl       0x60cc               ; _gleCreatePipelineProgramObject
00005ec0  bl       0x62b8               ; -> gldCreatePipelineProgram
00005ec8  stw      r30, 0x5428(r26)     ; objet PAR DÉFAUT
00005ed0  stw      r30, 0x5420(r26)     ; objet COURANT
00005ef0  stw      r0, 0(r11)           ; poignée publiée en gctx+0x886c + i*0x2e0 (sommet)
```
`_gleBindPipelineProgram 0x22820` relie l'objet par défaut quand le nom vaut 0 **[L]**.
⇒ **Le pilote voit toujours deux poignées valides, même en pipeline fixe**, avec une cible (`+0x4c8`) **nulle**.

### 4.4 Disposition de l'objet (`_gleCreatePipelineProgramObject 0x60cc`)

| Offset | Type | Sens | Preuve |
|---|---|---|---|
| `+0x04` / `+0x08` / `+0x0c` | u32 / i32 / ptr | nom GL / références / destructeur | `0x616c`, `0x615c`, `0x6164` **[L]** |
| `+0x10` | ptr | **flux d'instructions décodées** (`PPStream`) | `_gleClearPipelineProgram 0x30614` → `_PPStreamFree` **[L]** |
| `+0x14` | ptr | **texte ASCII du programme ARB** (copie `malloc`) | `_glProgramStringARB_Exec 0x22b5c/0x22c0c` **[L]** |
| `+0x18` / `+0x1c` | u32 | longueur du texte / format (`0x8875` ASCII) | `0x22c08` / `0x22c04` **[L]** |
| `+0x24` | u32 | génération d'état pour laquelle il a été compilé | `0x229a4/0x229d8` **[L]** |
| `+0x28..+0x4b` | u32 ×9 | compteurs de ressources | `0x61b8–0x61d0` **[L]** |
| `+0x4c` | u8 | programme à (re)compiler | `0x6158`, `0x22bfc` **[L]** |
| `+0x4e..+0x56` | u16 ×5 | nombres d'instructions / ressources | `0x6194–0x61a4` **[L]** |
| `+0x58 … +0x4a7` | u8 × `0x450` | **table « paramètre local → numéro de bit d'état du moteur »** (init `0x100`/`0x80` = non lié) | `0x61d4–0x61e8` ; `_glePPUpdateProgram 0x25ae8` ; lu en `0x7a0f0` **[L]** |
| `+0x4a8 + 4i` | u32 | **poignée du rendu `i`** (8 emplacements) | `0x6178–0x61b4`, `0x62dc` **[L]** |
| **`+0x4c8`** | u16 | **cible GL** : `0` (pipeline fixe), `0x8620`, `0x8804`, `0x8200`, `0x8B30/0x8B31` (GLSL) | `0x6260`, `0x22c10` **[L]** |
| `+0x4ca` | u16 | type interne (0 = sommet, 1 = fragment) | `0x624c` **[L]** |
| `+0x4cc` | ptr | vecteur de journal des paramètres | `0x6240` **[L]** |
| `+0x4d0` / `+0x4d4` | i32 | `-1` à l'init | `0x6250/0x6254` **[L]** |
| `+0x4e0` | ptr | **tableau des paramètres locaux** (16 octets par paramètre, init `(0,0,0,1)`) | `0x6170/0x6258`, `0x26218` **[L]** |
| `+0x4e4` | ptr | **forme compilée** : `PPEmulatorProgram` (sommet) ou `LFSStream` (fragment) | `_gleClearPipelineProgram 0x30634–0x30660` **[L]** |

**Le descripteur reçu par le plugin (`r5`) commence à `+0x4c8`** ; son premier `u16` est l'étiquette
de type. Confirmation par `GeForce3GLDriver _gldRelatePipelineProgram 0x773c` : `lwz 2, 0(5) ; lhz 2, 0(2)`
comparé à `0x8B30/0x8B31` **[L]**.

Réponse à la question posée : l'objet porte **à la fois** le texte ARB, une forme intermédiaire et une
forme compilée, plus une table de liaison paramètre ↔ état du moteur. Pour le pipeline **fixe**, `+0x10`
et `+0x14` restent nuls ; **aucun code ne fabrique un programme ARB synthétique à partir de l'état
fixe** — l'état fixe continue de passer par les champs d'état classiques. **[H]** pour ce dernier point
(absence de preuve, pas preuve d'absence : à vérifier par une sonde, cf. §7).

### 4.5 Chez les pilotes

| Pilote | `CreatePipelineProgram` | `ModifyPipelineProgram` |
|---|---|---|
| `GLDriver` `0x11d80` | `*r4 = 4` (jeton factice), rend 0 | `li r3,0 ; blr` |
| `ATIRage128GLDriver` `0x6a88` | `li r3,0 ; blr` — **n'écrit même pas `*r4`** | stub |
| `GeForce3GLDriver` `0x76b8` | alloue un enregistrement, `rec[0] = descripteur`, `rec[0x384] = 3` (masque « tout à refaire ») | `rec->masque \|= r5` ; compilation en microcode différée au dessin |

Pour POMPPC : il suffit d'écrire une poignée non nulle et de rendre 0 ; rien du chemin de dessin ne
dépend de ces entrées tant qu'aucun programme ARB n'est utilisé.

---

## 5. L'état de transformation et d'éclairage dans le bloc vu par le pilote

**Rappel (§0)** : `GS = gctx − 0x360`. Les offsets `gctx` ci-dessous sont ceux lus dans les
fonctions `_gl*_Exec` ; la colonne `GS` est celle à écrire dans `pomppc_accel.c`.

Contrôle indépendant : on vérifie qu'un pilote à T&L **lit** effectivement ces offsets, en
cherchant l'offset décimal dans `GeForce3GLDriver.s` (non annoté, offsets en décimal) **[L]** :

| `GS` | décimal | occurrences GeForce3 | occurrences ATIRage128 |
|---|---|---|---|
| `0x1810` échelle de viewport | 6160 | 16 | 0 |
| `0x1840` viewport entier | 6208 | 1 | 0 |
| `0x1860` matrice MVP | 6240 | 2 | 0 |
| `0x1960` matrice modèle-vue | 6496 | 1 | 0 |
| `0x1f60` inverse de la modèle-vue | 8032 | 1 | 0 |
| `0x2d40` masque des lumières allumées | 11584 | 2 | 0 |
| `0x2d4a` `GL_LIGHTING` | 11594 | 3 | 1 |
| `0x3174` `glFrontFace` | 12660 | 1 | 2 |
| `0x317a` `GL_CULL_FACE` | 12666 | 1 | 4 |
| `0x3176` `glCullFace` | 12662 | 1 | 1 |
| `0x3988` TexGen unité 0 | 14728 | 3 | 0 |

### 5.1 Matrices

GLEngine ne range pas « une matrice par type » mais **deux tableaux parallèles de 24 matrices
`float[16]`**, indexés par un *index de mode* interne **[L]** (`_gleUpdateMatrixMode 0x7e04`) :
```
00007e04  lwz      r11, 0x4ff4(r3)      ; index de mode courant (0..23)
00007e14  slwi     r2, r11, 6           ; mode * 0x40
00007e20  addi     r9, r2, 0x21c0       ; -> matrice INVERSE
00007e28  addi     r2, r2, 0x1bc0       ; -> matrice COURANTE
```

| `gctx` | `GS` | Type | Sens |
|---|---|---|---|
| `0x1bc0 + m·0x40` | **`0x1860 + m·0x40`** | float[16] | tableau des 24 matrices courantes |
| `0x21c0 + m·0x40` | **`0x1e60 + m·0x40`** | float[16] | tableau des 24 matrices inverses |
| `0x27c0` | **`0x2460`** | float[16] | MVP × transformation de viewport |
| `0x4ff4` | `0x4c94` | i32 | index de mode courant |
| `0x4fec` / `0x4ff0` | `0x4c8c` / `0x4c90` | ptr | matrice courante / inverse courante |
| `0x50ac` / `0x50b0` | `0x4d4c` / `0x4d50` | u32 | masques « matrice modifiée » / « inverse à recalculer » |
| `0x4ffc + m·4` | `0x4c9c + m·4` | i32 | profondeur de pile du mode `m` (24 entrées) |
| `0x4678` | `0x4318` | ptr | tampon des piles (`0x32c0` octets ; inverses de la modèle-vue à `+0x2b00`) |

Correspondance énumération → index de mode **[L]** (`_glMatrixMode_Exec 0x16080–0x160d8`) :

| Index | `GS` de la matrice | Contenu |
|---|---|---|
| 0 | `0x1860` | **modèle-vue × projection (MVP)**, cache dérivé recalculé par `_gleApplyViewScissorTransform 0x8674` |
| 1 | `0x18a0` | inutilisé **[H]** |
| 2 | `0x18e0` | `GL_COLOR` |
| 3 | `0x1920` | **`GL_PROJECTION`** |
| 4 | `0x1960` | **`GL_MODELVIEW`** |
| 5..7 | `0x19a0`..`0x1a20` | `GL_MODELVIEW1..3_ARB` **[H]** (calcul) |
| 8..15 | `0x1a60`..`0x1c20` | `GL_MATRIX0..7_ARB` **[H]** (calcul) |
| 16+u | `0x1c60 + u·0x40` | **`GL_TEXTURE` de l'unité `u`** (`_gleTextureMatTrans 0x51374`) |

L'inverse de la modèle-vue est donc en **`GS+0x1f60`** (= `0x1e60 + 4·0x40`) : c'est la matrice de
normales (après mise à l'échelle par le facteur de `GL_RESCALE_NORMAL`).
Format : **float 32 bits** ; `glLoadMatrixd` convertit (`_gleLoadMatrixd 0x522a4` : `lfd`/`frsp`/`stfs`) **[L]**.

### 5.2 Viewport, plage de profondeur

| `gctx` | `GS` | Type | Sens | Preuve |
|---|---|---|---|---|
| `0x1b70 … 0x1b7c` | **`0x1810 … 0x181c`** | float[4] | **échelle** du viewport (x, y, z, w) | `_gleUpdateViewScissorData 0x9d2c` ; `_gleUpdateDepthRangeData 0x8cec` ; consommé en `0xb54e0` : `fmadds f0, échelle, ndc, biais` **[L]** |
| `0x1b80 … 0x1b8c` | **`0x1820 … 0x182c`** | float[4] | **biais** du viewport | idem **[L]** |
| `0x1b90` / `0x1b98` | **`0x1830` / `0x1838`** | double | `glDepthRange` near / far | `_glDepthRange_Exec 0x626d4/0x6270c` **[L]** |
| `0x1ba0/0x1ba4/0x1ba8/0x1bac` | **`0x1840/0x1844/0x1848/0x184c`** | i32 | viewport x, y, largeur, hauteur | `_glViewport_Exec 0x15fc4–0x15fe4` **[L]** |
| `0x1bb0/0x1bb4/0x1bb8` | `0x1850/0x1854/0x1858` | float | near, far, far−near | `_glDepthRange_Exec 0x62714–0x62720` **[L]** |
| `0x4a2c/0x4a30/0x4a34/0x4a38` | `0x46cc/…` | float | échelle x, biais x, échelle y, biais y (fenêtre) | `_gleUpdateViewScissorData 0x9718–0x9728` **[L]** |
| `0x4a3c/0x4a40` | `0x46dc/0x46e0` | float | échelle z, biais z en `[0,1]` | `_gleUpdateDepthRangeData 0x8cf0/0x8cf8` **[L]** |

Valeurs par défaut posées par `_gleInitGLDState` : échelle `(1, 1, 0.5, 1)`, biais `(0, 0, 0.5, 0)`,
`near = 0.0` (double), `far = 1.0` (double) — cohérent avec `glDepthRange(0,1)` **[L]**
(`0x4ef4–0x4f18`, `r25 = 0x3f800000`, `r23 = 0x3f000000`).

### 5.3 Élimination des faces

| `gctx` | `GS` | Type | Sens | Preuve |
|---|---|---|---|---|
| `0x34d0` / `0x34d2` | **`0x3170` / `0x3172`** | u16 | `glPolygonMode` avant / arrière (déjà connu : `GS_POLY_MODE`) | `_gleUpdatePolyMode 0x9434` **[L]** |
| `0x34d4` | **`0x3174`** | u16 | `glFrontFace` (`GL_CW 0x900` / `GL_CCW 0x901`) | `_glFrontFace_Exec 0x558ec` **[L]** |
| `0x34d6` | **`0x3176`** | u16 | `glCullFace` (`0x404`/`0x405`/`0x408`) | `_glCullFace_Exec 0x39478` **[L]** |
| `0x34da` | **`0x317a`** | u8 | `GL_CULL_FACE` activé | `_gleSetEnable_CULL_FACE 0x165fc/0x16608` **[L]** |

Ces quatre champs complètent la ligne `GS_POLY_MODE 0x3170` déjà présente dans `pomppc_accel.c`
(`0x3178` stipple, `0x3179` lissage sont déjà relevés — `0x3174/0x3176/0x317a` manquaient).

### 5.4 Plans de découpe

| `gctx` | `GS` | Type | Sens | Preuve |
|---|---|---|---|---|
| `0x4188` | **`0x3e28`** | u32 | masque d'activation des 6 plans (bit `i` = `GL_CLIP_PLANE0+i`) | `_gleSetEnable_CLIP_PLANE 0xcb29c/0xcb2c0` **[L]** |
| `0x418c + i·0x10` | **`0x3e2c + i·0x10`** | float[4] | plan `i`, **en coordonnées ŒIL** (déjà multiplié par l'inverse de la modèle-vue) | `_glClipPlane_Exec 0x5a108–0x5a1ec` **[L]** |

Six plans au maximum (`0x5a0e4 cmplwi cr7, r27, 5`). `_gleInitGLDState` écrit bien `GS+0x3e28`.

### 5.5 TexGen — bloc séparé, pas `0x94` par unité

Le TexGen **n'est pas** dans le bloc d'unité de texture `GS+0x31c4 + u·0x7c` : il occupe son propre
bloc, de pas `0x94`, base `gctx+0x3ce8` = **`GS+0x3988`** **[L]** (`_glTexGen_Exec 0x9b8d4`,
unité active lue en `gctx+0x53d4`).

| `gctx` (unité `u`) | `GS` | Type | Sens |
|---|---|---|---|
| `0x3ce8 + u·0x94` | **`0x3988 + u·0x94`** | u16 | mode TexGen **S** (`0x2400` EYE_LINEAR, `0x2401` OBJECT_LINEAR, `0x2402` SPHERE_MAP, `0x8511` NORMAL_MAP, `0x8512` REFLECTION_MAP) |
| `0x3cec + u·0x94` | `0x398c + u·0x94` | float[4] | plan **œil** S (déjà transformé) |
| `0x3cfc + u·0x94` | `0x399c + u·0x94` | float[4] | plan **objet** S |
| `0x3d0c / 0x3d10 / 0x3d20` | `0x39ac / 0x39b0 / 0x39c0` | u16 / float[4] / float[4] | mode T, plan œil T, plan objet T |
| `0x3d30 / 0x3d34 / 0x3d44` | `0x39d0 / 0x39d4 / 0x39e4` | u16 / float[4] / float[4] | mode R, plan œil R, plan objet R |
| `0x3d54 / 0x3d58 / 0x3d68` | `0x39f4 / 0x39f8 / 0x3a08` | u16 / float[4] / float[4] | mode Q, plan œil Q, plan objet Q |
| `0x3d78/79/7a/7b + u·0x94` | `0x3a18/19/1a/1b + u·0x94` | u8 | activation `GL_TEXTURE_GEN_S/T/R/Q` |

Preuve des bits d'activation **[L]**, `_gleSetEnable_TEXTURE_GEN_S 0xcb304–0xcb320` :
`lwz r29, 0x53d4(r3)` / `mulli r2, r29, 0x94` / `addi r2, r2, 0x3d70` / `lbz r0, 8(r2)` / `stb r5, 8(r2)`.

### 5.6 Normalisation

| `gctx` | `GS` | Type | Sens | Preuve |
|---|---|---|---|---|
| `0x2804` | **`0x24a4`** | float | facteur de mise à l'échelle des normales (1.0 par défaut) | `_gleModelMatInvert 0x3354c/0x33558` ; init `_gleInitGLDState 0x4d40` **[L]** |
| `0x280d` | **`0x24ad`** | u8 | `GL_NORMALIZE` (`0x0ba1`) | `_gleSetEnable_NORMALIZE 0x46168/0x46180` ; init `0x4d38` **[L]** |
| `0x280e` | **`0x24ae`** | u8 | `GL_RESCALE_NORMAL` (`0x803a`) | `_gleSetEnable_RESCALE_NORMAL_EXT 0x31da0/0x31db8` ; init `0x4d3c` **[L]** |

### 5.7 Éclairage

| `gctx` | `GS` | Type | Sens | Preuve |
|---|---|---|---|---|
| `0x30a0` | **`0x2d40`** | u32 | **masque des lumières allumées** (bit `i` = `GL_LIGHT0+i`) | `_gleUpdateLightFast 0x7790` ; `_glLightfv…0x31c4c` **[L]** |
| `0x30aa` | **`0x2d4a`** | u8 | `GL_LIGHTING` | `0x31cf0 stb r5, 0x30aa(r3)` **[L]** |
| `0x30ad` | `0x2d4d` | u8 | matériau de couleur / deux faces **[H]** | `_gleUpdateLightFast 0x77bc` |
| `0x2820 + i·0x80` | **`0x24c0 + i·0x80`** | struct | **bloc de la lumière `i`** (8 lumières) | `_glLightfv_Exec 0x316b0`, `_glLightf_Exec 0x6b50c`, `_gleUpdateLightPosition 0x74b8`, `_gleUpdateLightAttenuation 0x7b8c` **[L]** |
| `0x24b0..0x24bc` (GS) | `0x24b0` | float[4] | couleur du modèle d'éclairage / ambiante de scène **[H]** | init `_gleInitGLDState 0x4dec–0x4df8`, défaut `(0,0,0,1)` |

Disposition d'un bloc de lumière (`0x80` octets) :

| Offset | Type | Sens | Statut |
|---|---|---|---|
| `+0x00` | float[4] | ambiante | **[H]** (par élimination ; défaut non écrit par `_gleInitGLDState`) |
| `+0x10` | float[4] | diffuse (défaut `(1,1,1,1)` pour la lumière 0) | **[L]** init `0x4e14–0x4e20` |
| `+0x20` | float[4] | spéculaire (défaut `(1,1,1,1)` pour la lumière 0) | **[L]** init `0x4e24–0x4e30` |
| `+0x30` | float[4] | **position** (x, y, z, w) en coordonnées œil | **[L]** `_gleUpdateLightPosition 0x74c4–0x74fc` |
| `+0x40` | float[3] | direction du spot | **[H]** |
| `+0x4c` | float | **cosinus du seuil du spot** (`< 0` ⟺ pas un spot, donc cutoff 180°) | **[L]** comparaison `0x7a10–0x7a30` ; **[H]** pour « cosinus » |
| `+0x50` | float | atténuation constante | **[L]** `_gleUpdateLightAttenuation 0x7b94` |
| `+0x54` | float | atténuation linéaire | **[L]** `0x7bc8` |
| `+0x58` | float | atténuation quadratique | **[L]** `0x7bd8` |
| `+0x5c` | float | exposant du spot | **[L]** `0x7a40`, passé à `log`/`exp` |
| `+0x60..0x7f` | — | dérivés | **[H]** |

L'état **dérivé** par lumière vit hors du bloc pilote, en `gctx+0x4a70 + i·0x6c`
(`+0x00..0x0b` direction normalisée, `+0x64` `1/atténuation constante`, `+0x68/0x69/0x6a/0x6b` drapeaux) **[L]**.

### 5.8 Matériau

Le matériau **n'est pas un champ de structure** : c'est un objet séparé, désigné par deux pointeurs
`gctx+0x4dd0` (face avant) et `gctx+0x4dd4` (face arrière) — `GS+0x4a70` / `GS+0x4a74`, donc
**probablement hors du bloc transmis** **[H]**. `_glMaterialfv_Exec 0x32a5c` délègue à une famille
`_gleLightMaterialRGBAChange_{F,B,FB}_{A,D,S,E,AD}` qui appelle `_glePushMaterial(gctx, face)` et
écrit dans l'objet rendu **[L]** :

| Offset dans l'objet matériau | Type | Sens | Bits posés en `+0x234` | Preuve |
|---|---|---|---|---|
| `+0x00` | float[4] | ambiante | `0x404` | `_gleLightMaterialRGBAChange_F_A 0xdc0c0–0xdc0d8` **[L]** |
| `+0x10` | float[4] | diffuse | `0x808` | `_gleLightMaterialRGBAChange_F_D 0xdc178–0xdc190` **[L]** |
| `+0x20` | float[4] | spéculaire | `0x1010` | `_gleLightMaterialRGBAChange_F_S 0x4a9c8–0x4a9d8` **[L]** |
| `+0x30` | float[4] | émission | `0x202` | `_gleLightMaterialRGBAChange_F_E 0x4a910–0x4a920` **[L]** |
| `+0x40` | float | brillance | **[H]** | pname `GL_SHININESS 0x1601` |
| `+0x234` | u16 | masque des composantes modifiées | — | `0xdc0c4/0xdc0d4` **[L]** |

### 5.9 Attributs courants (hors tableau) — **hors du bloc pilote**

| `gctx` | Type | Sens | Preuve |
|---|---|---|---|
| `0x2a0..0x2ac` | float[4] | couleur courante | `_glColor4f_NoColorMat_Exec 0x398ec` **[L]** |
| `0x2b0..0x2b8` | float[3] | normale courante | `_glNormal3f_Exec 0x3a940` **[L]** |
| `0x120 + u·0x10` | float[4] | coordonnée de texture courante de l'unité `u` | `_glTexCoord4f_Exec 0x93e54`, `_glMultiTexCoord4f_Exec 0x96374` **[L]** |

Ces offsets sont **inférieurs à `0x360`** : ils ne sont pas dans le bloc passé au pilote — GLEngine
les consomme lui-même en construisant les sommets.

### 5.10 Comment trouver les drapeaux qui manquent

`glEnable`/`glDisable` n'utilisent **pas** un `switch` mais une table de hachage globale
(`_gle_enable_hash_table` `0x113eb8`, recopiée en `gctx+0x4850`) de 1024 seaux de 16 octets
`{u32 énum ; ptr fn(gctx, énum, valeur) ; ptr suivant ; —}`, indexée par `(énum & 0x3FF)·16` **[L]**
(`_glEnable_Exec 0x163fc–0x16428`). La liste des fonctions `_gleSetEnable_*` de `GLEngine.nm` donne
donc, une par une, l'adresse du drapeau de chaque capacité : c'est la voie à suivre pour le stencil
(tâche 3.1) et pour tout le reste.

---

## 6. Les procédures de dessin « hautes » de la table de rastérisation

### 6.0 Où est la table

`_gliSetCurrentPluginDispatchTable 0x9ed8` **[L]** :
```
00009f00  lwz      r12, 0x4784(r3)      ; = plugin+0x144 = gldInitDispatch
00009f04  addi     r4, r3, 0x4698       ; TABLE DE PROCÉDURES : gctx+0x4698
00009f08  addi     r5, r3, 0x471c       ; seconde structure, 6 mots
00009f0c  lwz      r3, 0x4688(r3)       ; contexte du pilote
00009f14  bctrl
...
00009f54  li       r9, 0x21             ; 33 entrées, remplies de _gliDispatchNoop
00009f60  addi     r2, r3, 0x4698
```
⇒ `gldInitDispatch(ctx, gctx+0x4698, gctx+0x471c)`, **33 entrées** (`+0x00`…`+0x80`), donc
`RenderVertexBuffer` = `gctx+0x46e4`, `Begin/EndPrimitiveBuffer` = `gctx+0x46e8/0x46ec`,
`RenderVertexArray` = `gctx+0x4708`, `BufferSubData` = `gctx+0x4718` **[L]**.

### 6.1 `RenderVertexArray` (`+0x70`) — un seul site d'appel

`_gleExecuteVertexArrayRange 0x44f64` **[L]** :
```
00044fc4  addi     r0, r29, 0x120       ; gctx+0x120 : bloc des ATTRIBUTS COURANTS
00044fc8  lwz      r3, 0x4688(r29)      ; contexte du pilote
00044fcc  mr       r4, r28              ; drapeau « indexé »
00044fd0  lwz      r10, 0x4854(r29)     ; r10 = tampon de sommets de GLEngine
00044fd4  mr       r5, r27              ; mode de primitive
00044fd8  stw      r0, 0x38(r1)         ; 9e argument (56(r1))
00044fdc  mr       r6, r26              ; premier
00044fe0  lwz      r12, 0x4708(r29)     ; procédure +0x70
00044fe4  mr       r7, r25              ; nombre
00044fe8  mr       r8, r23              ; type d'indices
00044fec  mr       r9, r24              ; pointeur d'indices
00044ff4  bctrl
```

```c
GLint RenderVertexArray(void *ctx,
                        GLint  indexé,          /* 0 = glDrawArrays, 1 = glDrawElements */
                        GLenum mode,            /* GL_POINTS … GL_POLYGON */
                        GLint  premier,
                        GLsizei nombre,
                        GLenum typeIndices,     /* 0x1401/0x1403/0x1405, 0x14FF = aucun */
                        const void *indices,    /* 0 si glDrawArrays */
                        void *tamponSommets,    /* gctx+0x4854 */
                        void *attributsCourants /* gctx+0x120, 9e mot à 56(r1) */);
/* 0 = non traité → GLEngine reprend le chemin logiciel */
```
**[L]** pour les 9 arguments (trois chaînes d'appel indépendantes : `_glDrawArrays_Exec 0x4d538`
pose `r7 = 0` et `gctx+0x48dc = 0x14FF` ; `_glDrawElements_Exec 0xa8c44` pose `r7 = 1`,
`gctx+0x48dc = type`, `gctx+0x48d8 = indices + base du VBO` ; les trois variantes `*_ListExec`
passent littéralement `r4 = 0, r6 = 0, r8 = 0x14FF, r9 = 0`).
**[H]** sur le rôle exact de `tamponSommets` et de `attributsCourants` (le pilote y lit
vraisemblablement les attributs non fournis par un tableau activé — le bloc `gctx+0x120` est
celui des coordonnées de texture courantes, §5.9).

**Garde d'appel [L]** : chaque site est précédé de `lbz r0, 0x7580(r30) ; cmpwi ; beq`.
`gctx+0x7580` vient de `rendererInfo[0x79]` (`0x5498 lbz r0, 0x79(r10)` / `0x54a0 stb r0, 0x7580(r30)`).
**C'est le verrou de la tâche 1.1** : sans ce bit, GLEngine n'appelle jamais `RenderVertexArray`.

> **Correction du 18/09/2026** (`docs/re/verification-tcl.md`) : `r10` en `0x5498` vient de
> `lwz r10, 0x468c(r30)` — c'est le **bloc de configuration** (5ᵉ argument de `gldCreateContext`),
> **pas** `rendererInfo`. Et cette lecture ne donne que la valeur initiale : `gctx+0x7580` est
> refait à chaque changement d'état par `_gleUpdateDispatchCodeChange` (0xc8d20) à partir du
> **bit 0 de la valeur rendue par `gldInitDispatch`/`gldUpdateDispatch`**. C'est **là** qu'est le
> verrou utilisable, et il est réévaluable par lot d'état.

### 6.2 `RenderVertexBuffer` (`+0x4c`) et le trio `AllocVertexBuffer`

```c
void *gldAllocVertexBuffer(void *ctx, GLuint idFormatSommet, GLuint *nSommets /* E/S */);
void  gldCompleteVertexBuffer(void *ctx, void *tampon, GLuint nUtilisés /* [H] */);
void  gldFreeVertexBuffer(void *ctx, void *tampon);
void  RenderVertexBuffer(void *ctx, void *tampon, GLenum mode,
                         GLint biaisIndice, GLsizei nombre,
                         GLenum typeIndices, const void *indices);
```
**[L]** (`_gleCompileTCLVertexArray 0xda6b0`, `_gleExecuteTCLVertexArray 0xdada8/0xdae38`).

- `idFormatSommet` vient de `rendererInfo[0x7c..0x86]`, recopié dans `gctx+0x498c` ; le **pas**
  correspondant (`0x10`, `0x14`, `0x18`, `0x20`, `0x24` ou `0x2c` octets) est mis dans `gctx+0x4990`
  **[L]** (`0xda5c0–0xda65c`). Confirmation croisée : `ATIRage128GLDriver 0xdf6c–0xdf74` masque
  `r4 & 0x7FFF` et refuse au-delà de 6 → **sept formats de sommet possibles**.
- `nSommets` est un paramètre **entrée/sortie** ; les deux pilotes matériels plafonnent à **2048**
  (`GeForce3 0x5eb14`, `ATI 0xdf60`) et rendent un pointeur situé **128 octets après un en-tête**
  (`_gldFreeVertexBuffer : addi 4, 4, -128` dans les deux) **[L]**.
- `biaisIndice` = `gctx+0x4984 − gctx+0x4994` : le sommet d'indice `i` occupe le slot `i − biais` **[L]**.
- **`RenderVertexBuffer` reçoit toujours un tableau d'indices** : pour `glDrawArrays`, GLEngine
  *fabrique* un tableau `GL_UNSIGNED_INT` `first…first+count-1` par `malloc`, appelle, puis `free`
  **[L]** (`0xdadd8–0xdae6c`).

### 6.3 `BeginPrimitiveBuffer` (`+0x50`) / `EndPrimitiveBuffer` (`+0x54`)

```c
void *BeginPrimitiveBuffer(void *ctx, GLshort mode, GLuint *nSommets /* E/S */);
void  EndPrimitiveBuffer(void *ctx, GLint drapeau /* 0,1,2 */, GLshort mode, GLint nSommets);
```
**[L]** (`_gleBeginPrimitiveTCLFunc 0x1d160`, `_gleRenderPrimitiveTCLFunc 0x1ee28`,
`_gleForceToSoftwareTCL 0x103ba4`). Ce chemin est celui de `glBegin`/`glEnd` : GLEngine transforme
sur le PowerPC mais écrit les sommets **directement dans le tampon DMA du pilote** (le pointeur
rendu par `Begin` devient `gctx+0x4858`, la limite `gctx+0x485c`, le pas `gctx+0x487c`).
Garde : `gctx+0x48d0 != 0` (`_gleUpdatePrimitiveData 0x6780`) **[L]**.
La sémantique de `drapeau` (2 = cas nominal, 0/1 sur les chemins `GL_LINE_LOOP`) est **[H]**.

> **Vérifié et corrigé le 18/09/2026** (`docs/re/verification-tcl.md` §6) :
> - GLEngine **ne transforme pas** : les sommets écrits dans le tampon sont en **coordonnées
>   d'objet**, telles que l'application les a données (mesuré sur `glVertex2f`, sur `glDrawArrays`
>   et sur `glDrawElements` avec projection en perspective et modèle-vue non triviale).
> - Le tirage **indexé est déroulé** par GLEngine : `glDrawElements(GL_TRIANGLES, 6, …)` donne
>   `n = 6` sommets à plat, et `RenderVertexBuffer`/`RenderVertexArray` ne sont pas appelées.
> - `drapeau = 2` est bien le cas nominal (confirmé sur toutes les scènes essayées).
> - Le **pas** est en `gctx+0x4880` (u16, en octets) ; `gctx+0x487c` est aussi un **u16**, pas un
>   mot de 32 bits.
> - `gctx+0x48d0` vient de **`config+0x11c`** (`_gleUpdateDispatchCodeChange 0xc8d48`), que ni le
>   `GLDriver` d'Apple ni le `GeForce3GLDriver` ne posent (`GeForce3 0x3ab34/0x3ab98` : 0). Sa
>   disposition : `u8 n ; u8 ? ; u8 pas_en_mots ; u8 ? ; u16 entrée[n]` (`0x1d360`, copie de
>   `((n+1)/2)+1` mots vers `_gleVPSetFuncOutputDesc`). Sans lui, poser le verrou fait **perdre la
>   géométrie en silence**.

### 6.4 `BufferSubData` (`+0x80`)

```c
GLint BufferSubData(void *ctx, void *poignéeTampon, GLintptr offset, GLsizeiptr taille, const void *données);
```
**[L]** `_glBufferSubData_Exec 0x58f60–0x58f8c` ; `r4 = vbo + 0x10 + 4·index_de_contexte`.
Retour `0` ⇒ GLEngine fait le `memcpy` lui-même puis `gldFlushBuffer`.

### 6.5 Qui implémente quoi (la réponse à la question 6)

| Entrée | `GLDriver` (logiciel) | `GeForce3GLDriver` | `ATIRage128GLDriver` |
|---|---|---|---|
| `+0x4c` RenderVertexBuffer | `blr` (`0x4d34`) | **réel** (Init + Update) | **réel** (Update, conditionnel) |
| `+0x50` BeginPrimitiveBuffer | `li r3,0` (`0x4d10`) | **jamais armé** sauf no-op | **réel** (Init) |
| `+0x54` EndPrimitiveBuffer | `blr` (`0x4d18`) | **jamais armé** sauf no-op | **réel** (Init) |
| `+0x70` RenderVertexArray | `li r3,0` (`0x19bf4`) | **réel** (Init + Update) | armé depuis une variable `__data` |
| `+0x80` BufferSubData | `li r3,0` (`0x2f70`) | **réel** | **jamais armé** |
| `AllocVertexBuffer` | `*r5 = 0` puis `0` (`0x4d1c`) | **réel**, ≤ 2048 sommets | **réel**, ≤ 2048, 7 formats |
| `CreateVertexArray` / `Flush` | poignée factice / stub | **réel** | **stub total** |

Lecture de ce tableau, et c'est le point à retenir pour POMPPC **[L]** :

- **GeForce3** (T&L matériel) prend `RenderVertexArray` : GLEngine ne transforme rien, il passe le
  mode, la plage et les indices, et le pilote va lire lui-même les tableaux d'attributs.
- **Rage 128** (sans T&L) refuse `RenderVertexArray`/`CreateVertexArray` mais prend
  `Begin/EndPrimitiveBuffer` : GLEngine transforme sur le processeur et écrit **directement dans la
  mémoire DMA de la carte** — un gain de recopie, pas de calcul.
- Le rendu **logiciel d'Apple** ne prend rien : c'est le comportement actuel de notre plugin.

Pour l'axe 1 du TODO, la cible est donc le comportement GeForce3 : annoncer le bit
`rendererInfo[0x79]`, implémenter `CreateVertexArray`/`ModifyVertexArray`/`FlushVertexArray`
(§1, §2) et `RenderVertexArray` (§6.1). Le chemin `Begin/EndPrimitiveBuffer` (Rage 128) est un lot
séparé, plus simple, qui supprimerait déjà la recopie des sommets de `glBegin`/`glEnd`.

---

## 7. Sondes à écrire

Modèle : la scène `combprobe` de `guest/gltest/gltest.c` — **un réglage GL, un `glClear`** ;
avec `POMPPC_GLTRACE_STATE=1` le plugin traceur vide l'état à chaque effacement et l'on diffe
les vidages successifs. Chaque sonde ci-dessous donne la **liste exacte des appels GL**.

### 7.0 Préalables dans le traceur (`guest/gldriver/pomppc_gld.c`, cas `PROC_Clear`)

1. **Agrandir le vidage de l'état** : `pomppc_dump("clear-glstate", gls, 0x4000)` → **`0x5400`**.
   Le bloc va au moins jusqu'à `GS+0x4310` (§0) et les champs de la §5.1 (matrices, `GS+0x1860`…)
   comme ceux de la §5.4 (plans de découpe, `GS+0x3e28`) doivent tenir dans le vidage ; `0x5400`
   couvre aussi les pointeurs `GS+0x4700`, `GS+0x4a70` et `GS+0x50c0` utilisés ci-dessous.
2. **Vider l'objet « tableau de sommets » courant** : il est désigné par `GS+0x4700`
   (= `gctx+0x4a60`). Ajouter, dans le même cas :
   `V = GLD_U32(gls, 0x4700)` puis `pomppc_dump("clear-vao", (void*)(V), 0x600)`
   (couvre l'en-tête, les 32 emplacements, les masques et le premier bloc par rendu).
3. **Vider les deux objets matériau** : pointeurs en `GS+0x4a70` et `GS+0x4a74`
   (= `gctx+0x4dd0`/`0x4dd4`), `0x240` octets chacun.
4. **Vider l'objet « pipeline program » courant** : pointeur en `GS+0x50c0` (= `gctx+0x5420`),
   `0x520` octets — pour la sonde 7.6.

Ces quatre ajouts sont **la condition** de toutes les sondes qui suivent ; ils ne changent rien au
rendu (le traceur ne fait que lire).

### 7.1 `xformprobe` — viewport, profondeur, faces, normalisation, plans de découpe

Vérifie : `GS+0x1810..0x182c` (échelle/biais, dont les composantes `w`), `GS+0x1830/0x1838`
(doubles), `GS+0x1840..0x184c`, `GS+0x1850/0x1854/0x1858`, `GS+0x3174/0x3176/0x317a`,
`GS+0x24a4/0x24ad/0x24ae`, `GS+0x3e28` et `GS+0x3e2c + i·0x10`.

```c
glClearColor(0, 0, 0, 1);
glClear(GL_COLOR_BUFFER_BIT);                                          /*  1 référence   */
glViewport(3, 5, 17, 19);            glClear(GL_COLOR_BUFFER_BIT);     /*  2 viewport    */
glDepthRange(0.25, 0.75);            glClear(GL_COLOR_BUFFER_BIT);     /*  3 profondeur  */
glDepthRange(0.0, 1.0);              glClear(GL_COLOR_BUFFER_BIT);     /*  4 retour      */
glEnable(GL_CULL_FACE);              glClear(GL_COLOR_BUFFER_BIT);     /*  5 0x317a      */
glCullFace(GL_FRONT);                glClear(GL_COLOR_BUFFER_BIT);     /*  6 0x3176      */
glCullFace(GL_FRONT_AND_BACK);       glClear(GL_COLOR_BUFFER_BIT);     /*  7 0x3176      */
glFrontFace(GL_CW);                  glClear(GL_COLOR_BUFFER_BIT);     /*  8 0x3174      */
glDisable(GL_CULL_FACE);             glClear(GL_COLOR_BUFFER_BIT);     /*  9             */
glEnable(GL_NORMALIZE);              glClear(GL_COLOR_BUFFER_BIT);     /* 10 0x24ad      */
glEnable(GL_RESCALE_NORMAL);         glClear(GL_COLOR_BUFFER_BIT);     /* 11 0x24ae      */
{ static const double p0[4] = { 0.125, 0.25, 0.375, 0.5 };
  glClipPlane(GL_CLIP_PLANE0, p0); } glClear(GL_COLOR_BUFFER_BIT);     /* 12 0x3e2c      */
glEnable(GL_CLIP_PLANE0);            glClear(GL_COLOR_BUFFER_BIT);     /* 13 0x3e28 bit0 */
{ static const double p3[4] = { 0.0625, 0.125, 0.1875, 0.25 };
  glClipPlane(GL_CLIP_PLANE3, p3); } glClear(GL_COLOR_BUFFER_BIT);     /* 14 0x3e5c      */
glEnable(GL_CLIP_PLANE3);            glClear(GL_COLOR_BUFFER_BIT);     /* 15 0x3e28 bit3 */
glDisable(GL_CLIP_PLANE0); glDisable(GL_CLIP_PLANE3);
                                     glClear(GL_COLOR_BUFFER_BIT);     /* 16             */
```
Points de vérification : l'étape 2 doit faire apparaître `17 19` en entiers à `GS+0x1848/0x184c`
**et** `8.5 / 9.5` en flottants dans l'échelle/biais ; l'étape 12 doit montrer le plan **transformé**
(matrice de vue identité ⇒ valeurs inchangées), l'étape 14 le montre à `GS+0x3e2c+3·0x10`.

### 7.2 `matprobe` — matrices et piles

Vérifie : `GS+0x1860 + m·0x40` (index de mode 0..23), `GS+0x1e60 + m·0x40` (inverses),
`GS+0x2460`, `GS+0x4c94` (mode courant), `GS+0x4c9c + m·4` (profondeurs de pile).
Les valeurs sont choisies pour être **reconnaissables telles quelles** dans un vidage binaire.

```c
static const float M[16] = { 1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16 };
static const float N[16] = { 2,0,0,0, 0,2,0,0, 0,0,2,0, 0.125f,0.25f,0.375f,1 };
glClearColor(0, 0, 0, 1);
glClear(GL_COLOR_BUFFER_BIT);                                          /*  1 référence */
glMatrixMode(GL_MODELVIEW);  glLoadMatrixf(M);  glClear(GL_COLOR_BUFFER_BIT);   /*  2 */
glMatrixMode(GL_PROJECTION); glLoadMatrixf(N);  glClear(GL_COLOR_BUFFER_BIT);   /*  3 */
glMatrixMode(GL_TEXTURE);    glLoadMatrixf(M);  glClear(GL_COLOR_BUFFER_BIT);   /*  4 unité 0 */
glActiveTextureARB(GL_TEXTURE1_ARB);
glMatrixMode(GL_TEXTURE);    glLoadMatrixf(N);  glClear(GL_COLOR_BUFFER_BIT);   /*  5 unité 1 */
glActiveTextureARB(GL_TEXTURE0_ARB);
glMatrixMode(GL_COLOR);      glLoadMatrixf(M);  glClear(GL_COLOR_BUFFER_BIT);   /*  6 mode 2  */
glMatrixMode(GL_MODELVIEW);  glPushMatrix();    glClear(GL_COLOR_BUFFER_BIT);   /*  7 pile    */
glLoadIdentity();                               glClear(GL_COLOR_BUFFER_BIT);   /*  8 */
glPushMatrix(); glTranslatef(0.5f, 0.25f, 0.125f);
                                                glClear(GL_COLOR_BUFFER_BIT);   /*  9 */
glPopMatrix();                                  glClear(GL_COLOR_BUFFER_BIT);   /* 10 */
glPopMatrix();                                  glClear(GL_COLOR_BUFFER_BIT);   /* 11 */
glMatrixMode(GL_PROJECTION); glLoadIdentity();
glMatrixMode(GL_MODELVIEW);  glLoadIdentity();  glClear(GL_COLOR_BUFFER_BIT);   /* 12 */
```
Attendu : l'étape 2 fait apparaître `1.0 … 16.0` à `GS+0x1960` **et** dans la MVP `GS+0x1860` ;
l'étape 3 met `N` en `GS+0x1920` et recalcule `GS+0x1860` ; l'étape 4 écrit en `GS+0x1c60` et
l'étape 5 en `GS+0x1ca0` (ce qui **prouve** le pas `0x40` par unité de texture) ; l'étape 6 lève
l'ambiguïté sur l'index de mode 2. Les étapes 7 à 11 font varier `GS+0x4c9c + 4·4`.

### 7.3 `lightprobe` — lumières, matériau, modèle d'éclairage

Vérifie : `GS+0x24c0 + i·0x80` champ par champ, `GS+0x24b0` (ambiante de scène), `GS+0x2d40`,
`GS+0x2d4a`, `GS+0x2d4d`, et les deux objets matériau (préalable 7.0.3).
Chaque composante utilise une valeur **unique** pour être identifiable dans le vidage.

```c
static const float amb[4]  = { 0.0625f, 0.125f,  0.1875f, 0.25f   };
static const float dif[4]  = { 0.3125f, 0.375f,  0.4375f, 0.5f    };
static const float spc[4]  = { 0.5625f, 0.625f,  0.6875f, 0.75f   };
static const float pos[4]  = { 1.5f,    2.5f,    3.5f,    1.0f    };
static const float dir[3]  = { 0.0f,    0.0f,   -1.0f             };
static const float scn[4]  = { 0.8125f, 0.875f,  0.9375f, 1.0f    };
static const float mam[4]  = { 0.03125f,0.0625f, 0.09375f,0.125f  };
static const float mdi[4]  = { 0.15625f,0.1875f, 0.21875f,0.25f   };
static const float msp[4]  = { 0.28125f,0.3125f, 0.34375f,0.375f  };
static const float mem[4]  = { 0.40625f,0.4375f, 0.46875f,0.5f    };
glClearColor(0, 0, 0, 1);
glClear(GL_COLOR_BUFFER_BIT);                                                   /*  1 réf   */
glEnable(GL_LIGHTING);                                glClear(GL_COLOR_BUFFER_BIT); /*  2 0x2d4a */
glEnable(GL_LIGHT0);                                  glClear(GL_COLOR_BUFFER_BIT); /*  3 0x2d40 bit0 */
glLightfv(GL_LIGHT0, GL_AMBIENT,  amb);               glClear(GL_COLOR_BUFFER_BIT); /*  4 +0x00 */
glLightfv(GL_LIGHT0, GL_DIFFUSE,  dif);               glClear(GL_COLOR_BUFFER_BIT); /*  5 +0x10 */
glLightfv(GL_LIGHT0, GL_SPECULAR, spc);               glClear(GL_COLOR_BUFFER_BIT); /*  6 +0x20 */
glLightfv(GL_LIGHT0, GL_POSITION, pos);               glClear(GL_COLOR_BUFFER_BIT); /*  7 +0x30 */
glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, dir);         glClear(GL_COLOR_BUFFER_BIT); /*  8 +0x40 */
glLightf (GL_LIGHT0, GL_SPOT_CUTOFF, 60.0f);          glClear(GL_COLOR_BUFFER_BIT); /*  9 +0x4c : 60 ou cos60=0.5 ? */
glLightf (GL_LIGHT0, GL_SPOT_EXPONENT, 12.0f);        glClear(GL_COLOR_BUFFER_BIT); /* 10 +0x5c */
glLightf (GL_LIGHT0, GL_CONSTANT_ATTENUATION,  3.0f); glClear(GL_COLOR_BUFFER_BIT); /* 11 +0x50 */
glLightf (GL_LIGHT0, GL_LINEAR_ATTENUATION,    5.0f); glClear(GL_COLOR_BUFFER_BIT); /* 12 +0x54 */
glLightf (GL_LIGHT0, GL_QUADRATIC_ATTENUATION, 7.0f); glClear(GL_COLOR_BUFFER_BIT); /* 13 +0x58 */
glLightf (GL_LIGHT0, GL_SPOT_CUTOFF, 180.0f);         glClear(GL_COLOR_BUFFER_BIT); /* 14 retour */
glEnable(GL_LIGHT5);                                  glClear(GL_COLOR_BUFFER_BIT); /* 15 0x2d40 bit5 */
glLightfv(GL_LIGHT5, GL_DIFFUSE, dif);                glClear(GL_COLOR_BUFFER_BIT); /* 16 prouve le pas 0x80 */
glLightModelfv(GL_LIGHT_MODEL_AMBIENT, scn);          glClear(GL_COLOR_BUFFER_BIT); /* 17 0x24b0 ? */
glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);      glClear(GL_COLOR_BUFFER_BIT); /* 18 0x2d4d ? */
glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, GL_TRUE);  glClear(GL_COLOR_BUFFER_BIT); /* 19 */
glMaterialfv(GL_FRONT, GL_AMBIENT,  mam);             glClear(GL_COLOR_BUFFER_BIT); /* 20 mat+0x00 */
glMaterialfv(GL_FRONT, GL_DIFFUSE,  mdi);             glClear(GL_COLOR_BUFFER_BIT); /* 21 mat+0x10 */
glMaterialfv(GL_FRONT, GL_SPECULAR, msp);             glClear(GL_COLOR_BUFFER_BIT); /* 22 mat+0x20 */
glMaterialfv(GL_FRONT, GL_EMISSION, mem);             glClear(GL_COLOR_BUFFER_BIT); /* 23 mat+0x30 */
glMaterialf (GL_FRONT, GL_SHININESS, 37.0f);          glClear(GL_COLOR_BUFFER_BIT); /* 24 mat+0x40 ? */
glMaterialfv(GL_BACK,  GL_DIFFUSE,  msp);             glClear(GL_COLOR_BUFFER_BIT); /* 25 2e objet */
glEnable(GL_COLOR_MATERIAL);                          glClear(GL_COLOR_BUFFER_BIT); /* 26 */
glColorMaterial(GL_FRONT, GL_SPECULAR);               glClear(GL_COLOR_BUFFER_BIT); /* 27 */
glDisable(GL_COLOR_MATERIAL); glDisable(GL_LIGHTING);
glDisable(GL_LIGHT0); glDisable(GL_LIGHT5);           glClear(GL_COLOR_BUFFER_BIT); /* 28 */
```
La question tranchée par l'étape 9 est **la forme stockée du seuil de spot** : `60.0` (degrés) ou
`0.5` (cosinus). Le code compare `[+0x4c]` à `0.0` et considère « pas un spot » quand c'est négatif,
ce qui suggère le cosinus — la sonde le dit en une lecture.
L'étape 16 tranche le pas de `0x80` entre deux lumières (hypothèse calculée en §5.7).
Les étapes 20 à 25 ont besoin du vidage des objets matériau (préalable 7.0.3) ; à défaut, elles
montreront seulement les pointeurs `GS+0x4a70/0x4a74` et leur `+0x234`.

### 7.4 `texgenprobe` — TexGen par unité et par coordonnée

Vérifie le pas `0x94` et les offsets `GS+0x3988 + u·0x94` (§5.5), qui sont calculés et non lus
pour les unités `u > 0`.

```c
static const float sp[4] = { 0.0625f, 0.125f, 0.1875f, 0.25f };
static const float tp[4] = { 0.3125f, 0.375f, 0.4375f, 0.5f  };
static const float rp[4] = { 0.5625f, 0.625f, 0.6875f, 0.75f };
static const float qp[4] = { 0.8125f, 0.875f, 0.9375f, 1.0f  };
glClearColor(0, 0, 0, 1);
glClear(GL_COLOR_BUFFER_BIT);                                                   /*  1 réf */
glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR); glClear(GL_COLOR_BUFFER_BIT); /*  2 +0x00 */
glTexGenfv(GL_S, GL_OBJECT_PLANE, sp);                  glClear(GL_COLOR_BUFFER_BIT); /*  3 +0x14 */
glTexGenfv(GL_S, GL_EYE_PLANE,    sp);                  glClear(GL_COLOR_BUFFER_BIT); /*  4 +0x04 */
glEnable(GL_TEXTURE_GEN_S);                             glClear(GL_COLOR_BUFFER_BIT); /*  5 +0x90 */
glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);    glClear(GL_COLOR_BUFFER_BIT); /*  6 +0x24 */
glTexGenfv(GL_T, GL_OBJECT_PLANE, tp);                  glClear(GL_COLOR_BUFFER_BIT); /*  7 +0x38 */
glEnable(GL_TEXTURE_GEN_T);                             glClear(GL_COLOR_BUFFER_BIT); /*  8 +0x91 */
glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);    glClear(GL_COLOR_BUFFER_BIT); /*  9 +0x48 */
glTexGenfv(GL_R, GL_OBJECT_PLANE, rp);                  glClear(GL_COLOR_BUFFER_BIT); /* 10 +0x5c */
glTexGeni(GL_Q, GL_TEXTURE_GEN_MODE, GL_REFLECTION_MAP);glClear(GL_COLOR_BUFFER_BIT); /* 11 +0x6c */
glTexGenfv(GL_Q, GL_OBJECT_PLANE, qp);                  glClear(GL_COLOR_BUFFER_BIT); /* 12 +0x80 */
glEnable(GL_TEXTURE_GEN_R); glEnable(GL_TEXTURE_GEN_Q); glClear(GL_COLOR_BUFFER_BIT); /* 13 +0x92/93 */
glActiveTextureARB(GL_TEXTURE2_ARB);
glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_NORMAL_MAP);    glClear(GL_COLOR_BUFFER_BIT); /* 14 unité 2 : prouve le pas 0x94 */
glTexGenfv(GL_S, GL_OBJECT_PLANE, qp);                  glClear(GL_COLOR_BUFFER_BIT); /* 15 */
glActiveTextureARB(GL_TEXTURE0_ARB);
glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T);
glDisable(GL_TEXTURE_GEN_R); glDisable(GL_TEXTURE_GEN_Q);
                                                        glClear(GL_COLOR_BUFFER_BIT); /* 16 */
```

### 7.5 `vaprobe` — tableaux de sommets et objets tampon

Nécessite le préalable 7.0.2 (vidage de l'objet `V`). Vérifie les champs **[H]** de la §2.3
(`+0x0d` normalisé, `+0x10` pas utilisateur), la numérotation des attributs, les masques
`A+0x320/0x324`, `A+0x328/0x32c`, `A+0x340/0x344` et le lien VBO (`A+0x350+4a`, bloc par rendu).

```c
static float  vbuf[64];
static float  nbuf[64];
static GLubyte cbuf[64];
static GLushort ibuf[8] = { 0,1,2, 0,2,3, 0,1 };
GLuint vbo = 0, ebo = 0;
glClearColor(0, 0, 0, 1);
glClear(GL_COLOR_BUFFER_BIT);                                                   /*  1 réf, tout à 0 */
glVertexPointer(3, GL_FLOAT, 0, vbuf);        glClear(GL_COLOR_BUFFER_BIT);     /*  2 attribut 0 */
glEnableClientState(GL_VERTEX_ARRAY);         glClear(GL_COLOR_BUFFER_BIT);     /*  3 bit 16 */
glVertexPointer(2, GL_SHORT, 28, vbuf);       glClear(GL_COLOR_BUFFER_BIT);     /*  4 type/taille/pas : +0x04 = 28, +0x0c = 2 */
glVertexPointer(4, GL_FLOAT, 0, vbuf);        glClear(GL_COLOR_BUFFER_BIT);     /*  5 +0x04 = 16 */
glNormalPointer(GL_FLOAT, 0, nbuf);           glClear(GL_COLOR_BUFFER_BIT);     /*  6 attribut 1 */
glEnableClientState(GL_NORMAL_ARRAY);         glClear(GL_COLOR_BUFFER_BIT);     /*  7 bit 17 */
glColorPointer(4, GL_UNSIGNED_BYTE, 0, cbuf); glClear(GL_COLOR_BUFFER_BIT);     /*  8 attribut 2 */
glEnableClientState(GL_COLOR_ARRAY);          glClear(GL_COLOR_BUFFER_BIT);     /*  9 bit 18 */
glTexCoordPointer(2, GL_FLOAT, 0, vbuf);      glClear(GL_COLOR_BUFFER_BIT);     /* 10 attribut 8 */
glEnableClientState(GL_TEXTURE_COORD_ARRAY);  glClear(GL_COLOR_BUFFER_BIT);     /* 11 bit 24 */
glClientActiveTextureARB(GL_TEXTURE1_ARB);
glTexCoordPointer(4, GL_FLOAT, 0, nbuf);      glClear(GL_COLOR_BUFFER_BIT);     /* 12 attribut 9 */
glEnableClientState(GL_TEXTURE_COORD_ARRAY);  glClear(GL_COLOR_BUFFER_BIT);     /* 13 bit 25 */
glClientActiveTextureARB(GL_TEXTURE0_ARB);
glEdgeFlagPointer(0, vbuf);                   glClear(GL_COLOR_BUFFER_BIT);     /* 14 attribut 6 */
glEnableClientState(GL_EDGE_FLAG_ARRAY);      glClear(GL_COLOR_BUFFER_BIT);     /* 15 bit 22 */
glDrawArrays(GL_TRIANGLES, 0, 3);             glClear(GL_COLOR_BUFFER_BIT);     /* 16 après dessin */
glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, ibuf);
                                              glClear(GL_COLOR_BUFFER_BIT);     /* 17 */
glGenBuffersARB(1, &vbo);
glBindBufferARB(GL_ARRAY_BUFFER_ARB, vbo);    glClear(GL_COLOR_BUFFER_BIT);     /* 18 A+0x348 */
glBufferDataARB(GL_ARRAY_BUFFER_ARB, sizeof vbuf, vbuf, GL_STATIC_DRAW_ARB);
                                              glClear(GL_COLOR_BUFFER_BIT);     /* 19 */
glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)0);
                                              glClear(GL_COLOR_BUFFER_BIT);     /* 20 A+0x350, bloc par rendu, masque 0x328 */
glVertexPointer(3, GL_FLOAT, 0, (const GLvoid *)48);
                                              glClear(GL_COLOR_BUFFER_BIT);     /* 21 slot+0x00 = 48 (décalage brut) */
glGenBuffersARB(1, &ebo);
glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, ebo);
                                              glClear(GL_COLOR_BUFFER_BIT);     /* 22 A+0x34c */
glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
                                              glClear(GL_COLOR_BUFFER_BIT);     /* 23 */
glVertexArrayParameteriEXT(GL_VERTEX_ARRAY_STORAGE_HINT_APPLE, GL_STORAGE_CACHED_APPLE);
                                              glClear(GL_COLOR_BUFFER_BIT);     /* 24 A+0x332 = 0x85BE */
glDisableClientState(GL_VERTEX_ARRAY); glDisableClientState(GL_NORMAL_ARRAY);
glDisableClientState(GL_COLOR_ARRAY);  glDisableClientState(GL_EDGE_FLAG_ARRAY);
glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                                              glClear(GL_COLOR_BUFFER_BIT);     /* 25 */
```
Ce que la sonde tranche : **l'étape 4** donne d'un coup `+0x04` (pas effectif = 28), `+0x08` (`0x1402`),
`+0x0a` (2), `+0x0c` (2) et `+0x14` (signature `(28<<7)|(2<<4)|2 = 0x0E22`) — elle **valide ou
infirme** la formule de la signature. **L'étape 21** prouve que `slot+0x00` contient le décalage
brut et non l'adresse résolue. **L'étape 24** confirme `A+0x332`.
Les emplacements `+0x0d` et `+0x10` (marqués **[H]**) ne bougeront que si l'on ajoute
`glVertexAttribPointerARB(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 20, vbuf)` — à mettre en étape 26,
avec `glEnableVertexAttribArrayARB(1)` en 27 (attribut 17, bit 33).

### 7.6 `ppprobe` — le pipeline program du pipeline fixe

Question ouverte de la §4.4 : l'état fixe est-il *encodé* dans le programme par défaut ?
Nécessite le préalable 7.0.4 (vidage de l'objet pointé par `GS+0x50c0`).

```c
glClearColor(0, 0, 0, 1);
glClear(GL_COLOR_BUFFER_BIT);                                          /* 1 état fixe nu */
glEnable(GL_TEXTURE_2D);        glClear(GL_COLOR_BUFFER_BIT);          /* 2 */
glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
                                glClear(GL_COLOR_BUFFER_BIT);          /* 3 */
glEnable(GL_FOG);               glClear(GL_COLOR_BUFFER_BIT);          /* 4 */
glEnable(GL_LIGHTING); glEnable(GL_LIGHT0);
                                glClear(GL_COLOR_BUFFER_BIT);          /* 5 */
glDisable(GL_LIGHTING); glDisable(GL_FOG); glDisable(GL_TEXTURE_2D);
                                glClear(GL_COLOR_BUFFER_BIT);          /* 6 retour */
```
Si les octets de `ppobj+0x58 … +0x4a7` (table paramètre → bit d'état) ou `ppobj+0x4e4` (forme
compilée) changent entre les étapes, **l'état fixe est bien recompilé en programme** et il faudra
en tenir compte ; s'ils ne bougent pas, la conclusion **[H]** de la §4.4 est confirmée et le plugin
peut ignorer complètement les entrées `*PipelineProgram`.

### 7.7 Sonde sans invité : le verrou de la tâche 1.1

`gctx+0x7580` est mis depuis `rendererInfo[0x79]` (`GLEngine 0x5498`). La vérification ne demande
pas de sonde GL mais **une trace du plugin** : journaliser, dans `gldGetRendererInfo`, l'octet
`+0x79` de la structure rendue par le `GLDriver` d'Apple, puis le forcer à 1 dans notre copie et
observer si `RenderVertexArray` (`PROC_RenderVertexArray`) commence à être appelé. C'est le
préalable de tout l'axe 1 et cela se teste avec la scène `gltest spin` inchangée.

> **Faite le 18/09/2026, et l'énoncé était faux sur deux points** : l'octet est celui du **bloc de
> configuration**, pas de `rendererInfo` ; et il faut en plus rendre le bit 0 depuis
> `gldInitDispatch`/`gldUpdateDispatch` et publier `config+0x11c`. Résultat complet, traces et
> conséquences pour l'implémentation : **`docs/re/verification-tcl.md`**. Les sondes 7.1 à 7.6
> restent à écrire ; les préalables 7.0.1 à 7.0.4 (vidage de `0x5400` octets, de l'objet tableau de
> sommets, des deux matériaux et du pipeline program) sont **faits** dans `pomppc_gld.c`.

---

## 8. Récapitulatif des constantes à ajouter à `pomppc_accel.c`

```c
/* transformation (bloc vu par le pilote = gctx + 0x360) */
#define GS_VP_SCALE      0x1810   /* float ×4 : x, y, z, w                       */
#define GS_VP_BIAS       0x1820   /* float ×4                                     */
#define GS_DEPTH_NEAR    0x1830   /* double                                       */
#define GS_DEPTH_FAR     0x1838   /* double                                       */
#define GS_VIEWPORT      0x1840   /* i32 ×4 : x, y, largeur, hauteur              */
#define GS_MATRIX(m)     (0x1860 + (m) * 0x40)   /* float[16], m = index de mode  */
#define GS_MAT_MVP       GS_MATRIX(0)
#define GS_MAT_PROJ      GS_MATRIX(3)
#define GS_MAT_MODELVIEW GS_MATRIX(4)
#define GS_MAT_TEXTURE(u) GS_MATRIX(16 + (u))
#define GS_MATINV(m)     (0x1e60 + (m) * 0x40)
#define GS_MAT_NORMAL    GS_MATINV(4)            /* inverse de la modèle-vue      */
#define GS_NORMAL_SCALE  0x24a4   /* float                                        */
#define GS_NORMALIZE     0x24ad   /* u8                                           */
#define GS_RESCALE_NRM   0x24ae   /* u8                                           */
#define GS_LIGHT(i)      (0x24c0 + (i) * 0x80)   /* 8 lumières                    */
#define GS_LIGHT_MASK    0x2d40   /* u32 : bit i = GL_LIGHT0+i                     */
#define GS_LIGHTING      0x2d4a   /* u8                                           */
#define GS_FRONT_FACE    0x3174   /* u16 : GL_CW / GL_CCW                          */
#define GS_CULL_MODE     0x3176   /* u16 : GL_FRONT / GL_BACK / GL_FRONT_AND_BACK */
#define GS_CULL_FACE     0x317a   /* u8                                           */
#define GS_TEXGEN(u)     (0x3988 + (u) * 0x94)   /* S/T/R/Q, cf. §5.5             */
#define GS_CLIP_MASK     0x3e28   /* u32 : bit i = GL_CLIP_PLANE0+i                */
#define GS_CLIP_PLANE(i) (0x3e2c + (i) * 0x10)   /* float ×4, coordonnées œil     */
#define GS_VAO           0x4700   /* → objet « tableau de sommets » courant        */
#define GS_MATERIAL_F    0x4a70   /* → objet matériau avant                        */
#define GS_MATERIAL_B    0x4a74   /* → objet matériau arrière                      */
#define GS_PIPEPROG      0x50c0   /* → pipeline program courant                    */
```
Champs d'un bloc de lumière : `+0x00` ambiante, `+0x10` diffuse, `+0x20` spéculaire,
`+0x30` position (œil), `+0x40` direction du spot, `+0x4c` seuil, `+0x50/54/58` atténuations,
`+0x5c` exposant. Champs d'un bloc TexGen (par coordonnée `c = S,T,R,Q` : `+0x24·c`) :
`+0x00` mode (u16), `+0x04` plan œil, `+0x14` plan objet ; activations à `+0x90..0x93`.
