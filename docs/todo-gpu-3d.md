# TODO — Tiger en OpenGL 1.5, le plus vite possible, avec le maximum de travail sur le GPU hôte

**Objectif unique** : que Mac OS X Tiger sous QEMU annonce et tienne OpenGL 1.5, et que tout ce
qui peut s'exécuter sur le GPU de l'hôte s'y exécute. Chaque tâche ci-dessous est jugée à cette
aune : *combien de travail quitte le PowerPC émulé ?*

Ce fichier est le tableau de bord ; il est tenu à jour à chaque lot. Le contexte long est dans
`docs/roadmap-opengl15.md`, la conception et les offsets relevés dans `docs/gpu-3d-tiger.md`,
les relevés de rétro-ingénierie nouveaux dans `docs/re/`.

## État (17/09/2026)

- Protocole **v5** : rastérisation sur l'hôte, 4 unités de texture, GL_COMBINE, brouillard,
  lignes, points. Sous-ensemble d'OpenGL 1.3.
- Zenerchi ~50 img/s, Marble Blast Gold 20-80 img/s. **Dans les deux cas c'est le PowerPC émulé
  qui limite** : GLEngine transforme, éclaire et découpe chaque sommet sur l'invité ; le plugin
  ne reçoit que des sommets déjà en coordonnées fenêtre.
- Le renderer annonce « 1.1 APPLE-1.1 » (chaîne du rendu logiciel d'Apple, transmise telle quelle).

## Règles de travail (plusieurs agents)

- **Une seule VM de développement** (`disks/tiger-dev.raw`, `tools/guest/devloop.py`) : un seul
  agent à la fois y lance des jobs. Le travail hôte (protocole, backends, tests natifs) et la
  lecture des désassemblages se font en parallèle, sans VM.
- Un lot = une fonction, avec sa **preuve** : cas dans `tests/qgpu_core_test.c` (mêmes pixels sur
  les backends logiciel et OpenGL), scène `guest/gltest` comparée au rendu d'Apple
  (`POMPPC_GL_DISABLE=1`), et mesure avant/après sur un jeu quand c'est une tâche de vitesse.
- Hors domaine, le rendu d'Apple reprend la main. **Rien n'est annoncé qui ne soit tenu.**
- Tout relevé dans `OpenGL.framework` s'écrit dans `docs/re/`, avec l'adresse et la méthode.

---

## Axe 1 — Sortir la géométrie de l'invité (le plus gros gain de vitesse)

| # | Tâche | Statut |
|---|---|---|
| 1.1 | **Relever la négociation des capacités** : ce qui décide GLEngine à appeler les entrées « hautes » du pilote (`CreateVertexArray`, `RenderVertexArray` +0x70, `AllocVertexBuffer`, `CreatePipelineProgram`) plutôt que de transformer lui-même. Lire comment les pilotes ATI/NVIDIA de 10.4.6 répondent à `GetRendererInfo`, `GetInteger`, `GetString`. **Verrou de tout l'axe.** | à faire |
| 1.2 | Relever la disposition des tableaux de sommets, des programmes de pipeline, des matrices, des lumières et matériaux, du texgen et des plans de découpe dans l'état GLEngine (sondes `gltest` : un réglage, un vidage, un diff). | à faire |
| 1.3 | Protocole : matrices, éclairage/matériau, texgen, plans de découpe, dessin indexé de sommets bruts. Backends logiciel (référence) et OpenGL. | à faire |
| 1.4 | Plugin : envoyer les sommets non transformés ; repli sur le chemin actuel hors domaine. Mesurer sur Marble Blast. | à faire |

## Axe 2 — Ne plus recopier, ne plus attendre

| # | Tâche | Statut |
|---|---|---|
| 2.1 | **Objets tampon** (`CreateBuffer`, `FlushBuffer`, `BufferSubData`) sur tampons hôte : les maillages statiques ne retraversent plus la fenêtre partagée. (OpenGL 1.5.) | à faire |
| 2.2 | **Doorbell asynchrone** : thread de rendu hôte, l'invité continue pendant que le GPU dessine. `FENCE` et IRQ `DONE` existent ; les barrières `CreateFence`/`TestObject`/`FinishObject` donnent la sémantique invité. | à faire |
| 2.3 | **Zero-copy à la présentation** : le device écrit lui-même dans la VRAM (plage déclarée par le kext) ; plus de relecture ni de recopie par l'invité. | à faire |
| 2.4 | **Présentation en fenêtre sans attendre le WindowServer** (Marble Blast tourne en fenêtre) — dépend de 4.2 ou d'une composition côté hôte. | à faire |
| 2.5 | Téléversement de textures sans conversion invité quand le format est connu de l'hôte (BGRA, 565, 1555…) : la conversion passe sur l'hôte. | à faire |
| 2.6 | Opérations de pixels sur l'hôte (`DrawPixels`, `CopyPixels`, `Bitmap`, `ReadPixels`, `CopyTexSubImage`) : chacune force aujourd'hui une relecture complète. | à faire |

## Axe 3 — Compléter le pipeline fixe jusqu'à 1.5 (condition pour annoncer 1.5)

| # | Tâche | Statut |
|---|---|---|
| 3.1 | **Stencil** : tampon hôte, clés de protocole, backend de référence, offsets GLEngine. Absent de toute la chaîne. | à faire |
| 3.2 | Modes de polygone (ligne, point), pointillés de ligne et de polygone, lissage. | à faire |
| 3.3 | Opérations logiques ; mélange à couleur constante, équations minimum et maximum. | à faire |
| 3.4 | Textures 3D, cube, rectangle ; bordures ; compressées (S3TC passé tel quel à l'hôte). | à faire |
| 3.5 | Lignes et points texturés ; sprites de points ; couleur secondaire. | à faire |
| 3.6 | Textures de profondeur et comparaison d'ombre ; génération automatique de mipmaps (`GenerateTexMipmaps` repérée). | à faire |
| 3.7 | **Requêtes d'occlusion** (`CreateQuery`, `GetQueryInfo`). (OpenGL 1.5.) | à faire |
| 3.8 | Multiéchantillonnage. | à faire |
| 3.9 | Brouillard par fragment (`GL_NICEST`) ; niveau de détail des mipmaps par fragment dans le backend de référence. | à faire |

## Axe 4 — Annoncer 1.5, et brancher le système

| # | Tâche | Statut |
|---|---|---|
| 4.1 | **Annoncer version et extensions** telles que tenues (dépend de 1.1) : `GL_VERSION`, liste d'extensions, limites (`GetInteger`). | à faire |
| 4.2 | **Accélérateur IOKit** : nœud `IOAccelerator` + `IOGLBundleName`, chargement comme un vrai pilote de carte. Préalable de Quartz Extreme. | à faire |
| 4.3 | Programmes ARB de sommets et de fragments (`CreatePipelineProgram`) — au-delà de 1.5 strict, mais condition de Core Image. | à faire |
| 4.4 | Quartz Extreme (surfaces de fenêtre sur l'hôte), puis Core Image, puis Quartz 2D Extreme. Objectif visible : « QE/CI géré » dans Informations Système. | à faire |

## Points ouverts

- **Zenerchi, fin de partie** : ralentissement quand les cristaux brillent, non diagnostiqué. Le
  bilan `POMPPC_GL_STATS=<fichier>` donne les motifs de refus et les replis par procédure.
- Plus de 4 clients GL accélérés (tranches du kext) — à lever avant 4.4.

## Ordre d'attaque

1. **1.1** d'abord : sans elle, ni l'axe 1 ni l'annonce 4.1 ne peuvent démarrer.
2. En parallèle, sans VM : **3.1** (stencil) côté hôte, puis **2.2** (asynchrone).
3. Puis 1.2 → 1.3 → 1.4 (géométrie), 2.1 (tampons), 3.x par lots, 4.1 dès que la liste tient.
