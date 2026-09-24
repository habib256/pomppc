# Bilan du 23/09/2026 — POMPPCGPU face aux jeux Tiger : chemin parcouru, chemin restant

Écrit dans la nuit du 23 au 24/09/2026, à la demande de l'utilisateur (« réfléchis en détail
sur le chemin déjà parcouru et le chemin encore à faire pour avoir POMPPCGPU fonctionnel pour
tous les jeux Tiger »). Le tableau de bord courant reste `docs/todo-gpu-3d.md` ; la feuille de
route de fond `docs/roadmap-opengl15.md`. Ce document est une photographie et un raisonnement,
pas une liste de cases à cocher.

## 1. Où l'on en est

### 1.1 L'architecture tient

Trois couches, toutes à nous, rien d'Apple modifié :

- **device QEMU `qgpu-pci`** (`patches/qgpu/`) : un protocole de commandes dans une fenêtre
  partagée (BAR0), un cœur qui valide tout (`qgpu-core.c`), deux backends (rastériseur logiciel de
  référence, OpenGL hôte). Tests natifs : 808 épreuves vertes sur les deux backends ;
- **kext `POMPPCGPU`** : publie l'accélérateur IOKit, découpe la fenêtre en 4 clients, doorbell
  asynchrone, nettoyage des objets d'un client mort ;
- **plugin `GLDriver-POMPPC`** : le « pilote de carte » que GLEngine charge. Il traduit l'état
  GL de GLEngine (relevé par rétro-ingénierie, offsets dans `docs/gpu-3d-tiger.md` §4 et
  `docs/re/`) en commandes qgpu, et sait rendre la main au rendu d'Apple hors domaine.

Le protocole est passé de v1 à **v17** en une semaine (17/09 → 23/09) : textures, brouillard,
4 puis 8 unités, GL_COMBINE, stencil, géométrie brute (matrices, éclairage, texgen, DRAW_RAW),
pipeline fixe complet, doorbell asynchrone, OpenGL 1.2–1.4 (3D, cube, S3TC, profondeur), tampons
hôte (VBO), présentation directe, programmes ARB, 16 attributs génériques, 4096 textures.

### 1.2 Ce que les jeux disent (état au 24/09, 00h)

| Jeu | Chemin GL | État | Ce qu'il a coûté |
|---|---|---|---|
| Zenerchi | Carbon/AGL, pipeline fixe | ~50 img/s, juste | formats de texels, présentation directe |
| Marble Blast Gold | pipeline fixe, 4 unités | ~88 img/s | GL_COMBINE, géométrie hôte, v9 async |
| UT2004 Demo | tableaux + VBO, S3TC | jouable ~20 img/s ; arme noire (lot 11) | VTX_LIMIT, cube, textures |
| Warcraft III | tableaux | jouable ; texte mal rendu | (chemin tableaux, non résolu) |
| Colin McRae | programmes ARB via IndirectX | compile tout ; géométrie éclatée en course | v16, relevé ARB, garde-fous NaN |
| DOOM 3 Demo | ARB2 (vp+fp, génériques 8..11, 7 unités, DXT) | **tourne**, 3-5 img/s, « quelques bugs graphiques » | 16 génériques, **v17 : 8 unités**, 4096 textures |
| Prey Demo | ARB2 (8 unités, DXT5 nm) | **tourne, image juste** (salle de bain, miroir), ~5 img/s | v17, **S3TC annoncé**, vidage complet |
| RTCW | idTech3, pipeline fixe + multitexture | pas encore essayé | — |

Ce que la nuit a appris, jeu par jeu, et qui vaut pour tous :

1. **Chaque jeu est une « notion » de plus dans le protocole.** DOOM 3 : les tangentes sont
   dans les génériques 8..11, puis `interaction.vfp` lit `texture[0..6]`. Le protocole n'avait
   ni l'un ni l'autre. Le repli vers Apple n'était pas une option : sous programme ARB, tout
   repli finit dans `gleBuildInterpolateFunc` → `exit(1)` → segfault dans les destructeurs du
   jeu (`docs/re/glengine-exit-interpolateur.md`). La règle de l'utilisateur (« pas de repli :
   étendre le protocole ») est donc aussi une règle de survie.
2. **Annoncer honnêtement, dans les deux sens.** Prey n'a pas trouvé
   `GL_EXT_texture_compression_s3tc` (volontairement non annoncée : le rendu d'Apple plante sur
   un repli avec une texture DXT à mipmaps), n'a donc pas pu ouvrir ses `.dds` et a rendu tout
   le niveau avec ses images par défaut 16×16 RGB565 — normales fausses, écran noir. L'annonce
   coûte un risque de repli ; ne pas l'annoncer coûtait le jeu entier. Annoncée depuis ce soir
   (`POMPPC_GL_S3TC=0` pour revenir en arrière).
3. **Un budget « raisonnable » ne l'est plus avec idTech4.** 128 textures par client (512 / 4)
   suffisaient à Marble Blast ; Prey en lie plusieurs centaines par image : 2 000 évictions et
   860 retéléversements **par image**, 2 Mio de texels par image. Passé à 4096 (1024 par
   client). Corollaire découvert dans la douleur : **le kext compile aussi ces constantes** ; un
   kext ancien ne détruit que 128 textures d'un client mort, le client suivant recrée des
   identifiants encore vivants (BAD_ARG → `broken_all` → repli → Bus error), puis les créneaux
   de clients fuient (« no OpenGL-supported video card »). Règle : **toute modification de
   `qgpu_proto.h` ⇒ QEMU + kext (`install.sh`, redémarrage) + plugin**, jamais le plugin seul.
4. **Le rejeu natif doit être autonome ou il ment.** Le vidage déclenché ne portait ni les
   textes des programmes (envoyés dans une soumission sonde de l'image en cours, que l'on
   sautait) ni `program.env[10..16]` (miroir déclaré valide par un programme à 5 paramètres),
   ni les tampons hôte (pas de `BUF_CREATE` dans le prologue du rejoueur). Trois correctifs ce
   soir : `tests/qgpu_replay.c` recrée les tampons, le plugin renvoie les textes et tient le
   miroir d'env par compte d'entrées, et le vidage saute seulement la soumission en cours. Le
   rejeu de Prey reproduit alors l'image de la VM à l'identique, et les expériences par
   réécriture du texte du programme (`MOV result.color, light;` etc.) ont désigné la normale
   fausse en trois passes.

### 1.3 Où passe le temps (mesure DOOM 3, 24/09 00h, 640×480 fenêtré, cinématique)

Par image : ~200 ms, dont **6 ms** de rendu hôte (`wait`), **20 ms** de relectures (`copy`,
6 par image : 5 dessins repliés vers Apple + 1 présentation), et **~175 ms non mesurés côté
plugin = processeur émulé** : le jeu lui-même (idTech4 veut un G4 à 1,5 GHz ; TCG en donne une
fraction) et **le déroulage des sommets par GLEngine** (les `glDrawElements` passent par le
chemin T&L immédiat où GLEngine recopie chaque sommet dans son format de 256 octets avant que
le plugin ne le repacke). Le GPU hôte est oisif. Le goulot est le CPU invité, et la moitié de ce
CPU est du travail GL que l'on pourrait supprimer en prenant les tableaux tels quels.

## 2. Ce qui reste, par ordre d'effet sur « tous les jeux Tiger »

### P1 — Le chemin tableaux sous programme (le plus gros gain restant)

`RenderVertexArray` (canal ARRAY) prend les tableaux de l'application sans passer par le
déroulage de GLEngine ; il est refusé sous programme (`NO_G_GENERIC` : il ne packe pas les
génériques 16..31 du VAO). L'ouvrir avec les génériques (`docs/todo-gpu-3d.md`) supprime le
travail par sommet de GLEngine pour DOOM 3, Prey, UT2004 et WC3, et devrait ramener les ~175 ms
à ce que le jeu coûte seul. À prouver par `gltest` (scène génériques + tableaux) puis mesure
`frames.csv` avant/après. Dans le même mouvement : réutiliser les VBO hôte (`DRAW_RAW_BUF`)
sans réécriture quand l'application ne les a pas touchés (aujourd'hui 159 `BUF_SUBDATA` par
image dans Prey).

### P2 — Zéro repli par image

Chaque repli vers Apple coûte une relecture et un retéléversement de l'écran (≈ 3 ms chacun ici,
et le risque Bus error / `exit(1)`). Les motifs vivants : `raw:arrays 30/48` (5 par image dans
DOOM 3 et Prey), `raw:late-state`, `apple:nan-vertex`. Ils se lisent dans `note.txt` ; chacun se
traite comme les précédents : comprendre la notion manquante, l'apprendre au protocole, prouver.
Objectif chiffré : `fb=0 rb=1` par image dans `frames.csv`.

### P3 — Justesse des deux idTech4

- DOOM 3 : « quelques bugs graphiques » (dixit l'utilisateur) à caractériser avec le vidage
  maintenant autonome : `~/doom3.command` + `POMPPC_GL_DUMP_TRIGGER`, rejeu, réécriture du
  programme pour isoler le terme fautif (méthode de ce soir, §1.2.4).
- Prey : miroirs et sous-vues (`_currentRender`, COPY_TEX), `screenprocess.vfp`, distorsions ;
  vérifier `fragment.position` sous programme de fragments (noté « non retourné »).
- `POMPPC_GL_S3TC` : rendre le repli impossible quand une texture DXT à mipmaps est liée
  (aujourd'hui c'est le Bus error d'Apple qui tranche).

### P4 — Les chantiers ouverts d'avant ce soir

Colin McRae (tableaux déjà libérés par IndirectX au `Begin` : qui recycle le bloc ?), le texte
de Warcraft III (chemin tableaux), l'arme noire d'UT2004 (lot 11). Tous trois sont des affaires
de chemin tableaux ou de cycle de vie des tableaux : P1 les rapproche.

### P5 — Hygiène de protocole et de déploiement (petit, à faire vite)

- Le plugin doit **vérifier au démarrage** que le kext a les mêmes constantes de tranches
  (exposer `tex_ids`, `buf_ids`… dans la réponse du kext, refuser sinon) : ce soir, le désaccord a
  coûté une heure et deux plantages.
- `QGPU_REG_ERRORS` par client, pas global (un autre processus fait passer le plugin en
  synchrone).
- `cycle.sh` : une variante qui reconstruit le kext et redémarre quand `qgpu_proto.h` change.
- **Commit.** Tout ceci (v16, v17, S3TC, 4096 textures, rejeu) est non commité : 37 fichiers.
  Un lot par notion, avec ses épreuves.

### P6 — Le système, après les jeux

Quartz Extreme (étape E de la feuille de route) et Core Image demandent des surfaces hôte pour
le WindowServer — à faire sur `tiger-dev.raw`, jamais sur le disque quotidien. Plus de 4 clients
avant que la composition ne devienne elle-même un client. Zéro-copie de la présentation (F).

## 3. Ce que « fonctionnel pour tous les jeux Tiger » devrait vouloir dire

Une matrice de jeux avec, pour chacun, trois preuves : (1) **image juste** — rejeu natif du
vidage identique à la VM, et scène `gltest` de chaque notion nouvelle comparée au rendu
d'Apple ; (2) **zéro repli par image** (`fb=0`) ; (3) **mesure** (`frames.csv`) avec un plancher
par jeu, sachant que le CPU émulé borne tout (UT2004 20 img/s, DOOM 3 sans doute ≤ 15 même
parfait). Les familles à couvrir : pipeline fixe (Marble Blast, Zenerchi, RTCW à essayer),
tableaux et VBO (UT2004, WC3), programmes ARB (Colin McRae, DOOM 3, Prey). Quand une famille
est verte sur son représentant, les autres jeux de la même famille tombent pour peu de frais ;
c'est pour cela que P1 et P2 passent avant tout ajout de fonction.

## 4. Ordre proposé pour la prochaine session

1. P5 (vérification kext ↔ plugin, commit des lots de ce soir) — une heure, évite de reperdre
   la soirée.
2. P1 (tableaux + génériques) avec mesure DOOM 3 et Prey avant/après.
3. P2 (replis à zéro) sur les mêmes jeux.
4. Essayer Return to Castle Wolfenstein (pipeline fixe : devrait tomber vite) et reprendre les
   bugs graphiques de DOOM 3 avec le rejeu.
