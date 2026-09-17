# Feuille de route — d'un sous-ensemble d'OpenGL 1.3 à OpenGL 1.5 accéléré, et à QE/CI

Objectif : que Tiger sous QEMU utilise le GPU de l'hôte **partout où le système et les
applications s'en serviraient sur un vrai Power Mac** — jeux OpenGL, mais aussi composition des
fenêtres (Quartz Extreme) et Core Image.

Ce document donne l'ordre de travail, ce qu'il faut relever dans `OpenGL.framework` avant chaque
étape, et comment chaque étape se prouve. L'état courant et la liste courte sont dans
`docs/todo-gpu-3d.md` ; la conception et les offsets déjà établis dans `docs/gpu-3d-tiger.md`.

## 0. Le terrain

Sous Tiger, toute l'accélération 3D passe par OpenGL : ni Direct3D, ni RAVE ou QuickDraw 3D
(qui ne vivent que dans Classic). `OpenGL.framework` de 10.4 expose jusqu'à OpenGL 1.5 **quand le
pilote suit** ; le renderer logiciel générique d'Apple, celui que notre plugin remplace, annonce
1.1. Les surfaces passent par CGL (bas niveau, hors écran), AGL (Carbon — c'est ce qu'utilisent
Zenerchi et Marble Blast) et NSOpenGL (Cocoa).

L'architecture est celle que le projet exploite : **GLEngine** (partie commune du framework) fait
la transformation, l'éclairage, le découpage, l'élimination des faces et la génération des
coordonnées de texture, puis délègue à un bundle `GLDriver*` ; en dessous, un kext IOKit publie
l'accélérateur et la mémoire vidéo. Sous QEMU, aucune carte accélérée n'existe : le système ne
charge aucun pilote matériel, Informations Système affiche « QE/CI non géré », et tout tombe sur
le rasteriseur logiciel exécuté par le PowerPC émulé.

**Ce que le projet remplit déjà** : `GLDriver-POMPPC` se fait choisir par CGL, le kext
`POMPPCGPU` transporte les commandes, le device `qgpu-pci` les exécute sur le GPU de l'hôte.
Domaine couvert : un sous-ensemble d'OpenGL 1.3 au niveau **rastérisation** (protocole v5), la
géométrie restant calculée par GLEngine sur le processeur émulé.

## 1. Le point d'appui : les 61 points d'entrée du pilote

La table `gld*` résolue par `_glepPluginConnect` nomme déjà tout ce que demande OpenGL 1.5. C'est
la carte du chemin qui reste :

| Groupe d'entrées `gld*` | Ce que ça porte | Étape |
|---|---|---|
| `InitDispatch`, `UpdateDispatch` (procédures de rastérisation) | ce que le plugin utilise aujourd'hui | fait (v5) |
| `CreateVertexArray`, `ModifyVertexArray`, `FlushVertexArray`, procédure `RenderVertexArray` (+0x70) | tableaux de sommets **non transformés** | B |
| `AllocVertexBuffer`, `CompleteVertexBuffer`, `FreeVertexBuffer`, procédure `RenderVertexBuffer` (+0x4c) | flux de sommets immédiat | B |
| `CreatePipelineProgram`, `Modify`, `Relate`, `GetInfo`, `Destroy` | état de pipeline et programmes ARB | B puis D |
| `CreateBuffer`, `FlushBuffer`, `ReclaimBuffer`, `PageoffBuffer`, procédure `BufferSubData` (+0x80) | objets tampon (VBO, 1.5) | C |
| `CreateQuery`, `GetQueryInfo`, `DestroyQuery` | requêtes d'occlusion (1.5) | C |
| `CreateFence`, `TestObject`, `FinishObject`, `DestroyFence` | barrières, synchronisation asynchrone | C / F |
| `CreateFramebuffer`, `ReclaimFramebuffer`, `DestroyFramebuffer`, et le `cb_surface` de `gldInitializeLibrary` | surfaces de fenêtre : la porte de Quartz Extreme | E |
| `GetMemoryPluginData` et la famille | mémoire partagée avec le WindowServer | E |

Méthode, éprouvée sur GL_COMBINE : une **sonde** (`guest/gltest`, scène `combprobe`) qui change un
réglage à la fois, un vidage de l'état GLEngine à chaque effacement, un diff pour trouver l'offset.
Rien n'est deviné : tout se vérifie contre le rendu d'Apple.

## 2. Étapes, dans l'ordre recommandé

### A. Publier l'accélérateur IOKit (petit, à faire en premier)

Le kext publie un nœud `IOAccelerator` portant `IOGLBundleName = GLDriver-POMPPC`, rattaché à
l'écran. GLEngine charge alors notre bundle **comme un vrai pilote de carte**, au lieu de l'astuce
du nom de bundle qui trie avant celui d'Apple. C'est aussi le préalable de tout ce que le système
conditionne à la présence d'un accélérateur (étape E).

*Preuve* : `ioreg -c IOAccelerator`, et une application GL qui choisit notre renderer sans
`GL_RESOURCES` ni renommage.

### B. La géométrie sur l'hôte — le plus gros gain

Aujourd'hui le plugin reçoit des sommets **déjà transformés** : tout le travail géométrique reste
sur le PowerPC émulé. Se brancher un étage plus haut (tableaux de sommets, tampons de sommets,
programme de pipeline) permet d'envoyer les sommets bruts avec les matrices et l'état d'éclairage,
et de laisser le GPU hôte transformer et éclairer.

À relever : descripteurs de tableaux de sommets (format, pas, pointeurs, indices), objets
« programme de pipeline » (ce que GLEngine y range de l'état fixe), matrices modèle-vue,
projection et texture, lumières et matériaux, génération de coordonnées, plans de découpe.

À ajouter au protocole : matrices, éclairage/matériau, texgen, plans de découpe, et un dessin
indexé prenant des sommets bruts (positions, normales, couleurs, coordonnées, indices).

Règle inchangée : tout état hors domaine repasse par le chemin actuel, qui reste exact.

*Preuve* : scènes `gltest` comparées au rendu d'Apple ; Marble Blast, dont le temps est
aujourd'hui dans la transformation de Torque **et** celle de GLEngine.

### C. OpenGL 1.4 / 1.5

- **Objets tampon** (`CreateBuffer`, `BufferSubData`, `FlushBuffer`) → tampons hôte : une texture
  ou un maillage cesse d'être recopié à chaque image.
- **Requêtes d'occlusion** (`CreateQuery`, `GetQueryInfo`) → requêtes hôte.
- **Barrières** (`CreateFence`, `TestObject`, `FinishObject`) → synchronisation hôte ; c'est aussi
  ce qui rend le doorbell asynchrone sûr (étape F).
- Reste de 1.4/1.5 : sprites de points, couleur secondaire, génération automatique de mipmaps
  (la procédure `GenerateTexMipmaps` est déjà repérée), textures de profondeur et comparaison
  d'ombre, mélange à couleur constante, équations minimum et maximum.
- **Annoncer la version** : établir comment GLEngine compose `GL_VERSION` et la liste d'extensions
  à partir des capacités du pilote (structure `RendererInfo`, `gldGetInteger`, `gldGetString`) —
  aujourd'hui le plugin transmet la chaîne d'Apple (« 1.1 APPLE-1.1 »). Une application qui teste
  les extensions doit voir ce que l'on sait vraiment faire, ni plus, ni moins.

### D. Programmes ARB de sommets et de fragments

`CreatePipelineProgram` / `ModifyPipelineProgram` portent les programmes ARB. Il faut établir ce
que GLEngine y dépose (texte assembleur ARB ou forme déjà analysée), puis les exécuter sur l'hôte :
soit directement (les profils hérités d'OpenGL sur macOS gardent `ARB_vertex_program` et
`ARB_fragment_program`), soit par traduction en GLSL.

C'est la condition de Core Image, et de beaucoup de jeux de 2004-2006.

### E. Les fonctions Apple qui exploitent le GPU

- **Quartz Extreme** : le WindowServer compose les fenêtres sur le GPU dès qu'un accélérateur
  publie des surfaces. Il faut relever le couple de rappels passé à `gldInitializeLibrary`
  (`cb_flush`, `cb_surface`), la famille `CreateFramebuffer` et `GetMemoryPluginData`, puis donner
  aux fenêtres un support hôte. Gain système : la composition et le défilement cessent de coûter
  au processeur émulé — c'est aussi ce qui supprime l'attente du WindowServer mesurée dans les
  jeux en fenêtre.
- **Core Image** : exige les programmes de fragments (étape D). Une fois D et A en place,
  Informations Système doit afficher QE/CI **géré**.
- **Quartz 2D Extreme** : présent dans 10.4 mais désactivé par défaut ; à essayer une fois
  Quartz Extreme en place, en mesurant avant/après.

### F. Performance système, indépendante des étapes précédentes

- **Zero-copy** : le device écrit lui-même dans la VRAM (plage déclarée par le kext, jamais par le
  plugin) ; plus de recopie de l'image relue.
- **Doorbell asynchrone** : un thread de rendu hôte libère le vCPU ; `FENCE` et l'IRQ `DONE` sont
  déjà en place, les barrières de l'étape C donnent la sémantique invité.
- **Téléversement de textures en DMA**, surfaces partagées entre processus, et plus de 4 clients
  (aujourd'hui le kext découpe la fenêtre en 4 tranches).

## 3. Ce que chaque étape doit prouver

| Niveau | Outil | Règle |
|---|---|---|
| protocole et backends | `tests/qgpu_core_test.c` | le backend GL doit donner les mêmes pixels que le rasteriseur logiciel de référence |
| device | `tests/qgpu_smoke.py` | flux complet à travers QEMU |
| plugin, hors écran et en fenêtre | `guest/gltest`, `glwin` | chaque fonction nouvelle a sa scène, comparée au rendu d'Apple (`POMPPC_GL_DISABLE=1`) |
| applications réelles | Zenerchi, Marble Blast Gold | images correctes **et** mesure avant/après (`POMPPC_GL_STATS=<fichier>`) |
| système | Informations Système, Quartz Debug | QE/CI, et le coût du WindowServer mesuré à l'`ioreg`/`top` |

Toute fonction nouvelle garde son repli : hors domaine, le rendu d'Apple reprend la main, et le
bilan périodique dit pourquoi (motifs de refus, replis par procédure).

## 4. Dépendances et jalons

```
A (IOAccelerator) ─┬─> E (Quartz Extreme, Core Image)
                   │
B (géométrie hôte) ─┴─> C (1.4/1.5 : tampons, requêtes, barrières) ──> D (programmes ARB) ──> E
                                   │
                                   └─> F (zero-copy, asynchrone) — indépendant
```

Jalons, dans l'ordre : accélérateur publié ; géométrie transformée sur l'hôte ; tampons et
barrières ; version et extensions annoncées honnêtement ; programmes ARB ; QE/CI géré.

## 5. Risques connus

- **Les capacités décident du chemin** : GLEngine n'appellera les entrées « hautes » (tableaux de
  sommets, programmes de pipeline) que si le pilote se déclare capable. Tant que cette négociation
  n'est pas comprise, l'étape B ne peut pas démarrer : c'est le premier relevé à faire.
- **Le WindowServer est un client, pas un cobaye** : une erreur dans les surfaces (étape E) casse
  l'affichage du système entier, pas une application. À faire sur `disks/tiger-dev.raw`, jamais
  d'abord sur le disque de tous les jours.
- **Quatre clients GL accélérés** au plus (tranches du kext) : à lever avant que la composition du
  système devienne elle-même un client.
- **Fidélité** : chaque étape élargit la surface où notre rendu peut différer de celui d'Apple.
  Les écarts connus et tolérés sont listés dans `docs/todo-gpu-3d.md` §5.
