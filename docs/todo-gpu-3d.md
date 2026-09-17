# TODO — GPU 3D paravirtuel (device qgpu, kext POMPPCGPU, plugin GLDriver-POMPPC)

État et liste courte au 17/09/2026. La conception, les offsets relevés et les mesures sont dans
`docs/gpu-3d-tiger.md` ; **le plan à long terme (OpenGL 1.5, Quartz Extreme, Core Image) est dans
`docs/roadmap-opengl15.md`**. Ce fichier ne garde que ce qui reste à faire à court terme.

## 1. Où passe le temps aujourd'hui

Le protocole en est à la v5 (4 unités de texture, GL_COMBINE). Zenerchi tourne à ~50 img/s et
Marble Blast Gold (moteur Torque, vraie 3D) entre 20 et 80 img/s : dans les deux cas, **c'est le
processeur émulé qui limite, plus le rendu**.

Trois postes restent à la charge de l'invité, en plus de la géométrie :

- la conversion des texels au format du protocole ;
- l'assemblage des sommets dans la fenêtre partagée ;
- la recopie de l'image relue à chaque échange.

## 2. Pourquoi l'invité fait encore toute la géométrie

Le plugin se branche **tout en bas** de la chaîne OpenGL de Tiger. GLEngine, la partie commune du
framework, transforme les sommets, applique l'éclairage, découpe, élimine les faces arrière,
génère les coordonnées de texture, déroule les listes d'affichage et les tableaux de sommets.
Elle appelle ensuite les procédures de rastérisation du pilote avec des sommets **déjà en
coordonnées fenêtre** : ce sont elles que le plugin remplace. Tout ce qui est au-dessus reste du
code PowerPC émulé.

Ce choix était le seul praticable au départ : l'interface de rastérisation est petite, stable, et
c'est elle que la rétro-ingénierie a établie en premier. Les vrais pilotes matériels d'Apple
prennent la main plus haut, par les interfaces de programme de pipeline
(`gldCreatePipelineProgram`) et de tableau de sommets (`gldCreateVertexArray`). On les voit
passer dans la trace des jeux ; le plugin se contente aujourd'hui de les transmettre au moteur
logiciel d'Apple.

## 3. Le grand chantier : monter d'un étage

(Étape B de `docs/roadmap-opengl15.md`.)

**Se brancher au niveau du tableau de sommets et du programme de pipeline**, c'est-à-dire envoyer
à l'hôte les sommets **non transformés**, avec les matrices et l'état d'éclairage. Le GPU de
l'hôte ferait alors la transformation et l'éclairage, et l'invité ne ferait plus que préparer des
commandes.

Ce que cela demande :

1. relever dans GLEngine la disposition des descripteurs de tableaux de sommets, des programmes
   de pipeline, des matrices (modèle-vue, projection, texture) et des lumières — même méthode que
   la sonde `combprobe` de `guest/gltest` pour GL_COMBINE : un réglage, un vidage d'état, un diff ;
2. étendre le protocole : matrices, état d'éclairage et de matériau, tableaux de sommets ;
3. côté hôte, poser ces états en OpenGL classique et dessiner sans retransformer.

C'est le plus gros gain restant, et aussi le plus gros travail. À faire par étapes, en gardant à
chaque étape le repli vers le chemin actuel.

## 4. OpenGL 1.3 : ce qui manque, par ordre de coût

1. **Stencil** : n'existe nulle part dans la chaîne — ni tampon hôte, ni clé de protocole, ni
   rendu logiciel de référence. C'est le plus gros morceau.
2. **Modes de polygone non pleins**, pointillés et lissage des lignes, points et polygones.
3. **Opérations logiques**, textures 3D, cube et compressées, bordures de texture, lignes et
   points texturés, multiéchantillonnage, facteurs de mélange à couleur constante, équations
   minimum et maximum.
4. **Opérations de pixels** (`glDrawPixels`, `glBitmap`, `glCopyPixels`, accumulation) : déjà
   correctes, mais rendues par le code d'Apple.

## 5. Écarts de fidélité connus

- Brouillard par fragment en `GL_NICEST` (GLEngine ne fournit alors pas le facteur par sommet).
- Niveau de détail des mipmaps calculé **par triangle** dans le backend logiciel de référence.
- Pixels exactement sur une arête : règle de remplissage du GPU hôte.

## 6. Autres pistes, indépendantes

1. **Zero-copy** : laisser le device écrire lui-même dans la VRAM (plage déclarée par le kext,
   jamais par le plugin) supprimerait la recopie de l'image relue ; en fenêtre, composer côté
   hôte dans le frontend supprimerait aussi le WindowServer.
2. **Exécution asynchrone** : le device exécute dans l'écriture MMIO du doorbell ; un thread de
   rendu hôte libérerait le vCPU. `FENCE` et l'IRQ `DONE` sont déjà en place.
3. **Accélérateur IOKit** : publier `IOGLBundleName` sur un nœud `IOAccelerator` rattaché à
   l'écran remplacerait l'astuce du nom de bundle et ouvrirait la voie à Quartz Extreme.
4. **Autres backends hôte** : Vulkan (natif ou Zink), Metal (ANGLE) ; la suite de tests hôte
   s'applique telle quelle.

## 7. En cours / à vérifier

- **Zenerchi, fin de partie** : ralentissement quand les cristaux brillent — pas encore
  diagnostiqué. Le bilan périodique (`POMPPC_GL_STATS=<fichier>`) donne maintenant les motifs de
  refus et les replis par procédure : relancer une partie jusqu'à la fin et lire ces lignes.
- **Présentation directe en fenêtre** : Marble Blast tourne en fenêtre 800x600, donc sans
  présentation directe ; il attend le WindowServer à chaque image.
