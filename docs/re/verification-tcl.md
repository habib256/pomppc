# Vérification dans l'invité du verrou T&L de GLEngine (18/09/2026)

Objet : vérifier **par l'expérience**, dans Tiger 10.4.6 sous QEMU, ce que
`docs/re/capacites-glengine.md` (§5, expériences V1–V7) et
`docs/re/tableaux-de-sommets.md` (§6, §7.7) avaient établi **par lecture** :
l'octet `+0x79` du bloc de configuration (5ᵉ argument de `gldCreateContext`)
décide-t-il que GLEngine remet la géométrie **non transformée** au pilote ?

Méthode : plugin `guest/gldriver` instrumenté (tout derrière `POMPPC_GL_TCL`,
comportement par défaut inchangé), scènes de `guest/gltest`, boucle
`tools/guest/devloop.py` sur `disks/tiger-dev.raw`, protocole v6.
Désassemblage complémentaire par `otool -tV` **dans l'invité** (GLEngine,
`GeForce3GLDriver`, `ATIRage128GLDriver`), les adresses citées sont celles des
binaires de 10.4.6 PPC.

**Résumé en une phrase** : le relevé est **confirmé sur la structure** (le bloc,
ses offsets, l'octet `+0x79`, la destination `gctx+0x7580`) mais **infirmé sur
le mécanisme** : `+0x79` ne donne que la valeur *initiale*, le verrou réel est
le **bit 0 de la valeur rendue par `gldInitDispatch`/`gldUpdateDispatch`**, relu
à **chaque changement d'état** ; et il ne suffit pas, il faut en plus publier un
**descripteur de sortie de sommet en `cfg+0x11c`**.

---

## 1. Sonde V1 — `CGLGetParameter(310)` et `(311)`

Ajouté dans `guest/gltest/gltest.c`, juste après `CGLSetCurrentContext` :
`CGLGetParameter(ctx, 310 /* kCGLCPGPUVertexProcessing */, &v)` et `311`.

| Exécution | 310 | 311 |
|---|---|---|
| plugin POMPPC (`GL_RESOURCES`), scène `tri` | **0** | **1** |
| rendu d'Apple (`POMPPC_GL_DISABLE=1`), scène `tri` | **0** | **1** |

**310 = 0 : conforme au relevé.** Le désassemblage le confirme mot pour mot
(`_gliGetInteger`, GLEngine `0x2914`) :

```
00002914  lbz    r0,0x7580(r30)
00002918  xori   r0,r0,0x1
0000291c  subfic r2,r0,0
00002920  adde   r0,r2,r0          ; rend (gctx+0x7580 == 1)
```

**311 = 1 : le relevé est infirmé.** `capacites-glengine.md` §5.4 et §9 (V6.1)
annonçaient 0, en supposant `gctx+0x7581` copié de `gctx+0x7580`. Le code rend
en fait `(gctx+0x7581 != 0)` :

```
00002960  lbz    r0,0x7581(r30)
00002964  neg    r0,r0
00002968  rlwinm r0,r0,1,31,31     ; rend (gctx+0x7581 != 0)
```

et `gctx+0x7581` n'est **pas** l'image de `+0x79` : il est écrit par
`_gliSetCurrentPluginDispatchTable` (`0x9f24`) et par
`_gleUpdateDispatchCodeChange` (`0xc8bd0`) **à partir du bit 2 de la valeur
rendue par `gldInitDispatch`** :

```
00009f24  andi.  r0,r29,0x4        ; r29 = retour de gldInitDispatch
00009f28  bne+   0x9f34
00009f2c  li     r0,0x4
00009f30  stb    r0,0x7581(r30)
```

Le `GLDriver` d'Apple rend **4** (relevé dans notre trace :
`gldInitDispatch(...) -> 4`), donc `311 = 1` même en rendu purement logiciel.
**`CGLGetParameter(311)` ne dit rien de la T&L ; seul 310 est une sonde valide.**

---

## 2. Le bloc de configuration (V1, §4 du relevé) — **entièrement confirmé**

Vidage ajouté dans `gldCreateContext`, **après** l'appel au `GLDriver` d'Apple
(`pomppc_dump("cfg", e, 0x200)`), avec journal des champs clés :

```
  cfg +78..7b = 00 00 00 00 ; +b4 unités=8 +b6=8 +ba=8 ; +bc taille max=4096
  cfg ext +124=fc0002b1 +128=00004001 +12c=00001000 ;
          +00=00000004 +08=00000003 +0c=00000800 +10=00000800
```

À comparer au tableau §4.2 de `capacites-glengine.md` : `+0x00 = 4`,
`+0x08 = 3`, `+0x0c = +0x10 = 0x800`, `+0xb4 = +0xb6 = +0xba = 8`,
`+0xbc = 4096`, `+0x124 = 0xFC0002B1`, `+0x128 = 0x00004001`,
`+0x12c = 0x00001000`, `+0x78..0x7b = 00 00 00 00`. **Tout concorde, à l'octet
près.** Le 5ᵉ argument est bien le bloc de configuration, il est bien rempli par
le pilote pendant `gldCreateContext`, et la table du §4.2 est vérifiée.

Deux champs à ajouter au tableau, relevés ici :

| offset | valeur (GLDriver) | rôle |
|---|---|---|
| `+0x94` | `0x00000003` | lu en `0xda5cc`/`0xda60c` : le bit 3 (`& 8`) allonge le pas du sommet |
| `+0x11c` | `0` | **descripteur de sortie de sommet** → `gctx+0x48d0` (§5) |
| `+0x120` | `0` | second descripteur → `gctx+0x48d4` |

---

## 3. `+0x79 → gctx+0x7580` : confirmé, puis écrasé

Le désassemblage confirme le §5.1 mot pour mot (`_gleInitGLIState`) :

```
0000544c  lwz  r10,0x468c(r30)     ; bloc de configuration
00005498  lbz  r0,0x79(r10)
000054a0  stb  r0,0x7580(r30)
000054a8  lbz  r2,0x7580(r30)
000054b0  stb  r2,0x7581(r30)
```

Expérience : `POMPPC_GL_TCL=1` pose `cfg+0x79 = 1` et `cfg+0x7a = 1` après
l'appel au `GLDriver` d'Apple, et installe les procédures traceuses. Le plugin
journalise ensuite l'état du verrou à chaque `gldInitDispatch` /
`gldUpdateDispatch` :

```
[verrou initdispatch]   cfg+78..7b=00 01 01 00  7580=1  7581=1
[verrou updatedispatch] cfg+78..7b=00 01 01 00  7580=0  7581=4
```

**L'écriture prend** (`7580 = 1` à l'initialisation) **puis est défaite** au
premier changement d'état. `CGLGetParameter(310)` rend 0, aucune procédure T&L
n'est appelée, et l'image reste **correcte** (scènes `tri` et `varray` :
`OK (0 échec(s))`).

### Le vrai verrou : le retour de `gldInitDispatch`/`gldUpdateDispatch`

`_gliSetCurrentPluginDispatchTable` (`0x9f14`) passe le retour du pilote à
`_gleUpdateDispatchCodeChange`, qui refait `gctx+0x7580` à partir de son **bit 0** :

```
000c8bac  rlwinm r2,r4,0,29,29     ; r4 & 4  -> gctx+0x7581 (fragment)
000c8bd0  stb    r2,0x7581(r30)
...
000c8d20  lbz    r0,0x7580(r30)
000c8d24  rlwinm r2,r29,0,30,31    ; r4 & 3
000c8d28  cmpw   cr7,r2,r0
000c8d2c  beq    cr7,0xc8d7c       ; inchangé -> on saute TOUT le bloc
000c8d30  rlwinm r0,r29,0,31,31    ; r4 & 1
000c8d38  stb    r0,0x7580(r30)    ; <-- LE VERROU
000c8d3c  bl     _gleSetColorMaterialEnable
000c8d40  lwz    r9,0x468c(r30)
000c8d48  lwz    r0,0x11c(r9)
000c8d4c  stw    r0,0x48d0(r30)    ; descripteur de sortie de sommet
000c8d50  lwz    r2,0x120(r9)
000c8d54  stw    r2,0x48d4(r30)
000c8d58  bl     _gleUpdatePrimitiveData
000c8d60  bl     _gleUpdateDrawArraysFuncs
000c8d68  bl     _gleSelectVertexSubmitFunc
...
000c8d7c  xori   r0,r29,0x8        ; bit 3 -> gctx+0x759d
000c8d90  stb    r0,0x759d(r30)    ; _gleUpdateFramebufferOperationFuncs
```

Décodage de la valeur rendue par `gldInitDispatch` / `gldUpdateDispatch` :

| bit | effet |
|---|---|
| 0 (`1`) | **`gctx+0x7580` : le pilote fait la transformation et l'éclairage** |
| 1 (`2`) | force la reconstruction du chemin (sans lui, le test `(retour & 3) == gctx+0x7580` fait sauter le bloc, dont la relecture de `cfg+0x11c`) |
| 2 (`4`) | `gctx+0x7581` — ce que rend `CGLGetParameter(311)` |
| 3 (`8`) | `gctx+0x759d` : le pilote prend les opérations de tampon d'image |

Le `GLDriver` d'Apple rend **4** : bit 0 à zéro. **C'est pour cela que poser
`cfg+0x79` seul ne sert à rien** : le premier `gldUpdateDispatch` remet 0.

**Conséquence majeure, contraire au relevé** : `capacites-glengine.md` §10(b).3
affirmait qu'« il n'existe pas de moyen de refuser primitive par primitive :
`+0x79` est fixé une fois pour toutes à la création du contexte ». C'est faux.
Le verrou est **réévalué à chaque changement d'état GL**, c'est-à-dire à chaque
`gldUpdateDispatch`. Le repli est donc **possible par lot d'état**, ce qui est
exactement la granularité dont le plugin a besoin.

Expérience de contrôle (`POMPPC_GL_TCL=1` avec le retour laissé à 4) : le verrou
retombe à 0 au premier changement d'état et **les scènes `tri`, `varray`,
`gouraud` rendent l'image exacte**. C'est la preuve que le repli fonctionne.

---

## 4. Verrou posé : GLEngine prend le chemin T&L… et perd la géométrie

Avec le retour forcé à `4 | 1 = 5` :

```
[verrou initdispatch]   7580=1
[verrou updatedispatch] 7580=1   (×3)
CGLGetParameter 310 = 1
```

- `CGLGetParameter(310)` rend **1** : conforme à V6.2 du relevé.
- Scène `tri` : le triangle **disparaît**, fond bleu partout.
- Scène `varray` : les deux quadrilatères **disparaissent**.
- **Aucune** des procédures `+0x4c`, `+0x50`, `+0x54`, `+0x70` n'est appelée.
- **Aucune** procédure `Render*` logicielle n'est appelée non plus.
- `gldAllocVertexBuffer` n'est pas appelée (surchargée pour l'occasion).

GLEngine **jette la géométrie en silence**. La cause est `gctx+0x48d0 == 0` :
c'est la garde de `_gleUpdatePrimitiveData` (`0x6780`) et de
`_gleSelectVertexSubmitFunc` (`0x9e1c`), et elle vient de `cfg+0x11c`, que le
`GLDriver` d'Apple laisse à 0.

> **Réponse à la question posée** : **non**, rendre 0 depuis les quatre
> procédures ne donne pas un repli primitive par primitive. Pour
> `RenderVertexBuffer` (+0x4c) et `RenderVertexArray` (+0x70), `0` signifie bien
> « non pris en charge » (`tableaux-de-sommets.md` §6) — mais pour
> `BeginPrimitiveBuffer` (+0x50) il n'y a **pas** de valeur de refus : rendre 0
> ferait écrire GLEngine à l'adresse nulle, et rendre un tampon sans dessiner
> donne une image vide. **Le repli utilisable est ailleurs** : c'est le bit 0 du
> retour de `gldUpdateDispatch` (§3).

---

## 5. Le descripteur de sortie de sommet (`cfg+0x11c`) — pièce manquante

`_gleUpdateDispatchCodeChange` recopie `cfg+0x11c` dans `gctx+0x48d0` et
`cfg+0x120` dans `gctx+0x48d4`. `_gleSelectVertexSubmitFunc` en tire le **pas
d'un sommet** :

```
00009e1c  lwz  r2,0x48d0(r3)
00009e20  cmpwi cr7,r2,0
00009e24  beq  cr7,0x9e58          ; pas de descripteur -> chemin logiciel
00009e2c  lbz  r0,0x2(r2)          ; octet 2 = pas, en MOTS de 4 octets
00009e34  rlwinm r0,r0,2,0,29      ; × 4
00009e3c  sth  r0,0x4880(r3)       ; pas d'un sommet, en octets
00009e48  lbz  r0,0x2(r9)          ; idem pour le second descripteur
00009e50  sth  r0,0x4882(r3)
```

et `_gleBeginPrimitiveTCLFunc` (`0x1d360`) le recopie entrée par entrée :

```
0001d360  lbz    r27,0(r11)        ; octet 0 = NOMBRE d'entrées n
0001d368  addi   r2,r27,0x1
0001d36c  srawi  r2,r2,1
0001d370  addi   r27,r2,0x1        ; ((n+1)/2)+1 MOTS à copier
0001d38c  lwzx   r2,r11,r0         ; ... vers un tampon local
0001d414  bl     _gleVPSetFuncOutputDesc
```

Disposition établie : `u8 n ; u8 ? ; u8 pasEnMots ; u8 ? ; u16 entrée[n]`
(les entrées commencent au mot 1, deux par mot).

**Ni `GeForce3GLDriver` ni le `GLDriver` d'Apple ne posent ce champ** :
`GeForce3GLDriver 0x3ab34 li r11,0 ; 0x3ab98 stw r11,0x11c(r30)` — c'est **0**.
C'est cohérent avec `tableaux-de-sommets.md` §6.5 : GeForce3 n'arme jamais
`Begin/EndPrimitiveBuffer` et passe par `RenderVertexArray`/`RenderVertexBuffer`.
Le descripteur est donc **le commutateur du chemin « à la Rage 128 »**
(tampon de primitive) ; le chemin « à la GeForce3 » (tableaux de sommets) se
prend autrement et n'a **pas** été atteint dans ces essais.

---

## 6. La géométrie observée — **en coordonnées d'objet**

Avec `cfg+0x11c` pointant sur un descripteur synthétique
(`POMPPC_GL_TCL_DESC=4,0` : une entrée de code 0, pas de 4 mots) et le retour de
dispatch à `4|3 = 7`, `BeginPrimitiveBuffer` et `EndPrimitiveBuffer` sont
appelées et GLEngine écrit dans **notre** tampon.

### 6.1 Mode immédiat, scène `tri`

Le programme fait `glOrtho(0,64,64,0,-1,1)`, modèle-vue identité, puis
`glVertex2f(4,4) / (60,4) / (4,60)`.

```
TCL BeginPrimitiveBuffer(ctx=0x2808e00 mode=4 *n=0) pas=16
        4854=000f8220 4858=000f8220 485c=000fa620 487c=0100 4880=0010 4882=0010
TCL EndPrimitiveBuffer(ctx=0x2808e00 drapeau=2 mode=4 n=3) pas=16
    s0 : 4 4 0 1
    s1 : 60 4 0 1
    s2 : 4 60 0 1
```

**Les sommets sont les coordonnées d'objet telles quelles** : `(4,4,0,1)`,
`(60,4,0,1)`, `(4,60,0,1)`. Aucune transformation, aucune mise à l'échelle de
viewport, aucune division perspective. `mode = 4` = `GL_TRIANGLES`,
`drapeau = 2` (le cas nominal supposé en `tableaux-de-sommets.md` §6.3 — confirmé).

### 6.2 Tableaux de sommets, scène `varray` (nouvelle)

Scène ajoutée à `gltest` : projection en perspective
(`glFrustum(-1,1,-1,1,1,10)`), modèle-vue non triviale
(`glTranslatef(0.25, 0.125, -2)`), `glVertexPointer`/`glColorPointer` puis
`glDrawArrays(GL_TRIANGLES,0,6)` et `glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,…)`.

```
TCL BeginPrimitiveBuffer(... mode=4 *n=6)      /* glDrawArrays  */
TCL EndPrimitiveBuffer(... drapeau=2 mode=4 n=6)
    -2.0625 -1.125 0 … | -0.4375 -1.125 0 … | -0.4375 0.875 0 …
TCL BeginPrimitiveBuffer(... mode=4 *n=6)      /* glDrawElements */
TCL EndPrimitiveBuffer(... drapeau=2 mode=4 n=6)
    -0.0625 -1.125 0 … | 1.5625 -1.125 0 … | 1.5625 0.875 0 …
```

Ce sont **exactement** les coordonnées d'objet du programme, y compris pour le
tirage indexé — GLEngine **déroule les indices** et remet 6 sommets à plat.
Avec ce descripteur, `RenderVertexBuffer` (+0x4c) et `RenderVertexArray` (+0x70)
ne sont **pas** utilisées : tout passe par le tampon de primitive.

### 6.3 Beaucoup de géométrie, scène `game`

```
TCL BeginPrimitiveBuffer(... mode=7 *n=0) pas=32
TCL EndPrimitiveBuffer(... drapeau=2 mode=7 n=192) pas=32
    s0 : -2 -1.5 … 0 1 | s1 : 2 -1.5 … 0 1 | s2 : 2 -1.5 … -3 1 …
```

`mode = 7` = `GL_QUADS`, **192 sommets d'un coup** : GLEngine groupe toute la
primitive dans un seul tampon. 60 appels `Begin`/`End` pour 60 images.

### 6.4 Codes d'entrée du descripteur — **non élucidés**

Le pas et le nombre d'entrées sont établis ; la signification des `u16` ne l'est
pas. Observations sur la scène `gouraud` (sommets `(0,0)`, `(64,0)`, `(0,64)`) :

| descripteur | pas | contenu d'un sommet |
|---|---|---|
| `n=1`, {0} | 16 | `x y z w` — **la position, propre** |
| `n=2`, {0,1} | 32 | `x  x y z w  …` (5 flottants utiles) |
| `n=2`, {0,2} | 32 | `x y  x y  z w  …` |
| `n=2`, {0,4} | 32 | `x y z w  x y z w` |

Les couleurs, normales et coordonnées de texture **n'apparaissent jamais** avec
ces codes : c'est le prochain relevé à faire (une sonde par code, de 0 à ~32,
sur une scène colorée et texturée). **[H]** : les codes sont des identifiants de
sortie du « vertex program » interne (`_gleVPSetFuncOutputDesc`), pas des
numéros d'attribut GL.

### 6.5 `cfg+0x7a` à 0 puis à 1

Aucune différence observée sur `tri`, `gouraud` ni `varray` : traces identiques,
mêmes sommets, même `drapeau = 2`. C'est cohérent avec le §5.3 du relevé — la
différence ne se manifeste qu'à un **changement d'état entre `glBegin` et
`glEnd`**, que ces scènes ne font pas. Le test reste à écrire (scène à
`glDisable(GL_TEXTURE_2D)` en cours de primitive).
`_gleUpdatePrimitiveData 0x6704 lbz r0,0x7a(r2)` confirme que l'octet est bien
lu, et directement dans le bloc de configuration.

---

## 7. Où sont les matrices au moment du dessin

Les procédures traceuses journalisent les matrices en remontant du contexte
pilote au contexte GLEngine (`gctx = *(u32 *)(drvctx+0x0c) − 0x360`, §0 de
`tableaux-de-sommets.md`), à `gctx+0x1bc0 + m·0x40` (= `GS+0x1860 + m·0x40`) :
`m = 0` MVP, `m = 3` projection, `m = 4` modèle-vue. Les trois sont lisibles et
à jour au moment de `BeginPrimitiveBuffer` — **le §5.1 de
`tableaux-de-sommets.md` est confirmé** : c'est bien là que le plugin ira
chercher de quoi transformer sur l'hôte. Les identifiants de format de sommet
sont bien en `cfg+0x7c..0x87` avec un pas **imposé par GLEngine**
(`0xda5a8-0xda65c`) :

| champ | pas imposé |
|---|---|
| `cfg+0x7c` | `0x10` |
| `cfg+0x7e` | `0x18` |
| `cfg+0x80` | `0x20` |
| `cfg+0x82` | `0x14` |
| `cfg+0x84` | `0x20`, ou `0x24` si `cfg+0x94 & 8` |
| `cfg+0x86` | `0x2c`, ou `0x34` si `cfg+0x94 & 8` |

---

## 8. Ce qu'il faut pour implémenter

Pour recevoir la géométrie brute et l'envoyer à l'hôte, le plugin doit :

1. **Publier le verrou par la table de procédures, pas par le bloc de
   configuration.** `gldInitDispatch` et `gldUpdateDispatch` doivent rendre
   `(retour d'Apple) | 1` quand le lot d'état courant est dans notre domaine, et
   le retour d'Apple tel quel sinon. Ajouter le bit 1 (`| 3`) lorsqu'on veut
   forcer la reconstruction du chemin (changement de descripteur, reprise après
   repli). Poser aussi `cfg+0x79 = 1` dans `gldCreateContext` reste utile : cela
   fixe la valeur initiale, avant le premier `gldInitDispatch`.
   **C'est le point de repli** : il est relu à chaque changement d'état, donc le
   plugin peut rendre la main à GLEngine dès qu'une fonctionnalité sort de son
   domaine, sans rien casser (vérifié : l'image redevient exacte).
2. **Publier un descripteur de sortie de sommet en `cfg+0x11c`**
   (`u8 n ; u8 ? ; u8 pasEnMots ; u8 ? ; u16 entrée[n]`, durée de vie ≥ celle du
   contexte). Sans lui, GLEngine prend le chemin T&L et **jette la géométrie**.
   Reste à relever : la table des codes d'entrée (§6.4) pour obtenir couleur,
   normale et coordonnées de texture à côté de la position.
3. **Installer les quatre procédures** `+0x50` `BeginPrimitiveBuffer`,
   `+0x54` `EndPrimitiveBuffer`, `+0x4c` `RenderVertexBuffer`,
   `+0x70` `RenderVertexArray` **à chaque `gldInitDispatch` et chaque
   `gldUpdateDispatch`** (le `GLDriver` d'Apple réécrit la table entière) —
   c'est déjà ce que fait `pomppc_hook_procs`/`install_for`.
   - `BeginPrimitiveBuffer(ctx, mode, &n)` : rendre un tampon **à nous** et
     écrire dans `*n` le nombre d'emplacements disponibles. **Rendre 0 est
     interdit** (écriture à l'adresse nulle).
   - `EndPrimitiveBuffer(ctx, drapeau, mode, n)` : `drapeau = 2` est le cas
     nominal ; `mode` est l'énumération GL (`4` = `GL_TRIANGLES`, `7` = `GL_QUADS`) ;
     `n` est le nombre de sommets écrits. C'est **ici** que le plugin émet.
   - `RenderVertexBuffer` / `RenderVertexArray` : rendre `0` (« non pris en
     charge ») tant que le chemin « tableaux de sommets » n'est pas implémenté.
4. **Lire le pas d'un sommet dans `gctx+0x4880`** (u16, en octets ; `gctx+0x487c`
   et `gctx+0x4882` portent la même valeur), **pas** dans `gctx+0x487c` traité
   comme un mot de 32 bits.
5. **Lire les matrices et l'état** dans le bloc déjà reçu :
   `GS+0x1860` MVP, `GS+0x1920` projection, `GS+0x1960` modèle-vue,
   `GS+0x1f60` inverse de la modèle-vue, `GS+0x1810/0x1820` échelle et biais de
   viewport, `GS+0x2d40/0x2d4a` éclairage, `GS+0x3174/0x3176/0x317a` faces,
   `GS+0x3e28` plans de découpe (`tableaux-de-sommets.md` §5, confirmé ici pour
   les matrices).
6. **Ne pas compter sur un refus primitive par primitive.** La granularité
   disponible est le **lot d'état** (point 1). Concrètement : à chaque
   `gldUpdateDispatch`, décider si l'état courant est dans le domaine du plugin
   (formats de sommet publiés, éclairage géré, pas de texgen exotique…) et
   rendre le bit 0 en conséquence.
7. **Ne pas se fier à `CGLGetParameter(311)`** ni aux appels à
   `gldCreateVertexArray`/`gldCreatePipelineProgram` : ni l'un ni les autres ne
   disent quoi que ce soit de la T&L.

Restes à établir avant 1.3/1.4 : les codes du descripteur (§6.4) ; le chemin
`RenderVertexArray`/`RenderVertexBuffer` « à la GeForce3 », jamais atteint ici ;
le comportement de `cfg+0x7a = 0` sur un changement d'état en cours de primitive.

---

## 9. Non-régression (plugin modifié, sans `POMPPC_GL_TCL`)

```
--- scene tri (plugin, sans TCL) ---      OK (0 échec(s))
--- scene tex (plugin, sans TCL) ---      OK (0 échec(s))
--- scene comb (plugin, sans TCL) ---     OK (0 échec(s))
--- scene game (plugin, sans TCL) ---     OK (0 échec(s))
--- scene varray (plugin, sans TCL) ---   OK (0 échec(s))
--- scene stencil (GLTEST_STENCIL=1) ---  OK (0 échec(s))
```

La scène `varray` rend de plus une image **identique à l'octet près** à celle du
rendu d'Apple (`cmp` des PPM).

---

## 10. Variables d'environnement de la sonde

Toutes sont sans effet si `POMPPC_GL_TCL` n'est pas défini.

| variable | effet |
|---|---|
| `POMPPC_GL_TCL=1` | pose `cfg+0x79 = 1`, publie six identifiants de format, installe les quatre procédures traceuses, force le bit 0 (et 1) du retour de dispatch |
| `POMPPC_GL_TCL=2` | en plus, force `gctx+0x7580 = 1` à chaque dispatch |
| `POMPPC_GL_TCL_7A=0` | pose `cfg+0x7a = 0` (défaut 1) |
| `POMPPC_GL_TCL_BITS=n` | bits ajoutés au retour de `gldInitDispatch`/`gldUpdateDispatch` (défaut 3 ; `0` = retour d'Apple inchangé) |
| `POMPPC_GL_TCL_DESC=pas,e0,e1,…` | publie un descripteur en `cfg+0x11c` (`pas` en mots de 4 octets) |
