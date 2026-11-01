# GLDriver-POMPPC — plugin OpenGL de Tiger pour le GPU paravirtuel qgpu

Le plugin que GLEngine (`OpenGL.framework`) charge comme un rendu « GLDriver* ».
Il fait rendre la géométrie des applications OpenGL de Tiger par le GPU de
l'hôte, à travers le kext `POMPPCGPU` et le device QEMU `qgpu-pci`.
Conception, rétro-ingénierie et mesures : `docs/gpu-3d-tiger.md`.

**Au 01/11/2026** : révision `20261101-ombre` (`POMPPC_PLUGIN_REV` de `pomppc_accel.c`),
protocole qgpu v22 (`docs/protocole.md`). Plusieurs paragraphes ci-dessous datent de septembre
(v7 à v9) et sont marqués là où ils ne valent plus. Les défauts font foi dans le code : macros
`*_DEFAULT` et lectures `getenv("POMPPC_GL_…")` de `pomppc_accel.c`.

## Principe

- **Mandataire du rendu logiciel d'Apple.** Le plugin charge en module privé
  `GLDriver.bundle` (le rendu « Generic » de Tiger) et lui transmet les 63
  points d'entrée `gld*` : 42 par des trampolines assembleur (`gld_tramp.s`,
  généré par `tools/gld/gen_tramp.py`) qui ne touchent à aucun argument, les 21
  autres (liste `C_OVERRIDES` du générateur) écrits en C dans `pomppc_gld.c`,
  qui appellent à leur tour ceux d'Apple. Tout ce que le plugin n'accélère pas
  reste donc exact.
- **Accélération au niveau de la rastérisation.** GLEngine transforme, éclaire,
  découpe et élimine les faces, puis appelle les procédures installées par
  `gldInitDispatch` avec des sommets en coordonnées fenêtre. Le plugin remplace
  l'effacement et les procédures de triangles (et bandes, éventails, quads,
  polygones), de lignes et de points : elles deviennent des commandes qgpu,
  groupées et soumises au kext. Les textures sont suivies (création, niveaux,
  modifications) et recopiées sur l'hôte à la première utilisation après
  changement.
- **Cache de textures.** Les 128 identifiants hôte (01/11 : 1 024 par client depuis la v17,
  `QGPU_CLIENT_TEX_IDS`) sont réutilisés par éviction
  de la texture la moins récemment utilisée. Les textures actives du dessin
  courant sont protégées ; les commandes précédentes sont terminées avant
  réutilisation. Les niveaux et paramètres restent chez GLEngine et sont
  rechargés à la prochaine utilisation. La scène `gltest texcache` vérifie
  257 textures, les modifications après éviction et deux contextes partagés.
- **Géométrie sur l'hôte (chemin « brut », protocole v7).** Quand l'état courant
  est dans le domaine, `gldInitDispatch`/`gldUpdateDispatch` rendent le bit 0 :
  GLEngine cesse de transformer, d'éclairer, de découper et d'éliminer les faces,
  et dépose les attributs **bruts** dans le tampon que `BeginPrimitiveBuffer`
  lui donne — un pointeur **dans la fenêtre partagée**, à la disposition exacte
  de `DRAW_RAW` : aucune recopie. Matrices, viewport, lumières, matériaux,
  texgen, plans de découpe et brouillard partent à l'hôte, et seulement quand ils
  changent. Hors domaine, on rend le retour d'Apple tel quel et GLEngine reprend
  tout le travail : le repli se fait **par lot d'état** et l'image reste exacte.
  `POMPPC_GL_GEOM=0` coupe ce chemin.
- **Tableaux de sommets (canal mixte).** Quand `GL_VERTEX_ARRAY` est actif,
  le plugin pose `cfg+0x78` : `_gleDrawArraysOrElements_Exec` quitte Begin/End
  et appelle `RenderVertexArray` (+0x70) ou `AllocVertexBuffer` /
  `RenderVertexBuffer` (+0x4c). Le descripteur `cfg+0x11c` **reste** : `glBegin`
  continue d'écrire les attributs bruts (Marble Blast, Zenerchi, HUD). Le
  plugin lit l'objet tableau (`GS_VAO`), packe au format `DRAW_RAW` et envoie
  les **indices tels quels**. `POMPPC_GL_ARRAY=1` (opt-in) est ce mixte ; `=2` retire le descripteur
  (canal GeForce3 strict, `glBegin` jeté jusqu'au repli) ; **`=0` (défaut)**
  coupe et GLEngine déroule tout via Begin/End.
- **Fusion des dessins.** GLEngine remet la géométrie par `glBegin`/`glEnd`, et
  les jeux en font des rubans et des éventails **courts** : Marble Blast en
  envoyait ~1 065 par image pour 8 200 sommets, soit 7,7 sommets par dessin, et
  chaque dessin coûte à l'hôte un `gl_target` complet (mesuré : **2 µs**). Le
  plugin recolle les lots **consécutifs** — même contexte, même format, sommets
  contigus — en **un seul `DRAW_RAW` en `GL_TRIANGLES` indexés** : seuls des
  indices u16 sont écrits, les sommets ne bougent pas. Le sommet provoquant de
  l'ombrage plat est respecté mode par mode, l'alternance d'orientation des
  rubans aussi, et l'ordre de dessin n'est jamais changé. Tout changement d'état
  ferme la série. `POMPPC_GL_MERGE=0` coupe la fusion.
- **Fin du pipeline fixe (protocole v8).** Mélange à couleur constante et équations `GL_MIN` /
  `GL_MAX`, les seize **opérations logiques**, **pointillé de polygone**, **pointillé de ligne**,
  **modes de polygone** `GL_POINT` / `GL_LINE`, décalage de polygone en ligne et en point, et les
  **requêtes d'occlusion** d'OpenGL 1.5. Contrairement aux clés de la v7, celles-ci valent pour les
  **deux** chemins de dessin : elles agissent au fragment ou à l'assemblage des triangles, après
  l'endroit où les deux chemins se rejoignent. Deux exceptions, et elles sont dites par le test :
  les **modes de polygone** sont réservés au chemin brut (le chemin hérité reçoit des triangles
  déjà décomposés, le contour du quadrilatère est perdu avant l'hôte), et le **pointillé de ligne**
  aussi (l'hôte tient le compteur primitive par primitive, ce que le découpage en segments du
  chemin hérité ne permet pas). Offsets relevés : `docs/re/etat-v8.md`.
- **Requêtes d'occlusion, tenues entièrement par le plugin.** GLEngine les remet au pilote par
  `gldCreateQuery`, `gldDestroyQuery`, `gldGetQueryInfo` et les procédures `+0x68` / `+0x6c` ; le
  rendu d'Apple n'en tient **aucune** (bouchons, rien d'installé), au point que
  `glGetQueryObjectuiv` n'écrit même pas dans la variable de sortie. Si un dessin retombe sur le
  logiciel pendant qu'une requête court, le plugin **majore** le compte de l'aire de la surface :
  sur-estimer est la seule direction sans danger, un objet déclaré visible étant simplement
  dessiné.
- **Deux copies, un drapeau de fraîcheur.** La surface hôte double le tampon de
  dessin du rendu logiciel (couleur et profondeur). Avant un dessin hôte, ce que
  le logiciel a dessiné est téléversé ; avant un échange, un vidage ou tout
  chemin logiciel, ce que l'hôte a dessiné est relu. Un effacement complet ne
  téléverse rien, et la profondeur n'est relue que si un chemin logiciel s'en
  sert.
- **État accéléré.** Profondeur, stencil, masques, mélange (y compris couleur constante et
  min/max), opérations logiques, test alpha, ciseaux, ombrage lisse ou plat, brouillard, décalage
  de polygone, pointillés, largeur de ligne et taille de point, et **quatre** unités de texture
  2D/1D (six formats de base, texels en octets ou compacts 8888/1555/565/4444, filtres avec
  mipmaps, modes de répétition, environnements MODULATE/REPLACE/DECAL/BLEND/ADD et `GL_COMBINE`).
  (01/11 : depuis, textures 1D/3D/cube/rectangle, profondeur et S3TC en v10, 8 unités en v17
  — 4 au pipeline fixe sur un hôte NVIDIA, `QGPU_CAP_FIXED4` —, programmes ARB en v16, GLSL en
  v21, combineurs ATI en v22.) Hors de ce domaine (liste complète : `docs/gpu-3d-tiger.md` §4.5),
  le plugin
  synchronise puis laisse faire le code d'Apple.
- **Présentation directe.** Une application au premier plan reçoit son image
  directement dans la mémoire vidéo, sans passer par le WindowServer : en plein
  écran, et en fenêtre tant que rien ne recouvre la surface et que le curseur
  n'y bouge pas (`docs/gpu-3d-tiger.md` §4.4). (01/11 : sous Quartz Extreme, plein
  écran seulement ; une fenêtre passe par la voie QE, `docs/quartz-extreme.md`.)
- **Soumission asynchrone (protocole v9).** Le plugin ne frappe plus le doorbell
  synchrone : il dépose et repart, l'hôte dessine pendant que l'invité prépare
  l'image suivante. La tranche du client est coupée en **deux moitiés**
  alternées à chaque soumission (flux, sommets, indices, arène) : tant que
  `FENCE` n'a pas dépassé une soumission, l'hôte peut lire ou écrire tout ce
  qu'elle désigne, et écrire l'image *n+1* dans la même moitié donnerait des
  triangles qui clignotent. Une seule de nos soumissions est en vol à la fois.
  L'attente de barrière ne tombe qu'aux trois endroits où l'invité a **besoin**
  du résultat : avant un chemin logiciel (`sync_to_sw`), pour un compte
  d'occlusion, et — au plus tard possible — au **début de l'échange suivant**,
  qui présente l'image *n−1* pendant qu'on prépare la *n+1*. Une image de
  latence, jamais plus. Mesuré sur Marble Blast : temps de soumission
  **1,6 → 0,06 ms par image**, attente restante 0,1 ms, **+11 %** d'images par
  seconde. `POMPPC_GL_ASYNC=0` revient au doorbell synchrone, au bit près.
- **Identité.** Identifiant de plugin `0x7700` (renderer `0x00027700`), annoncé
  accéléré ; `GL_VENDOR = POMPPC`, `GL_RENDERER = POMPPC qgpu (OpenGL host GPU)`.
  Le kext publie un accélérateur IOKit dont `IOGLBundleName` est
  `GLDriver-POMPPC` (tâche 4.2) : GLEngine charge le plugin depuis
  `/System/Library/Extensions/GLDriver-POMPPC.bundle`, **avant** le GLDriver
  d'Apple, comme le pilote d'une carte. CGL retient le premier renderer qui
  convient, et le nôtre est aussi le seul à répondre à `kCGLPFAAccelerated`,
  `kCGLPFAFullScreen` et `kCGLPFANoRecovery` (retirés de la copie envoyée au
  GLDriver d'Apple ; drapeaux `0x100` / `0x2` / `0x2000` posés sur nos formats).
  `kCGLRPVideoMemory` / `kCGLRPTextureMemory` annoncent **64 Mio** (le logiciel
  d'Apple y laisse 0, ce qui fait refuser OpenGL à Warcraft III).
  Un second exemplaire du plugin dans le même processus (une copie restée dans
  `Resources`) est rejeté par GLEngine après son `gldInitializeLibrary` : il
  reste inactif (marque `POMPPC_GLD_OWNER`), et `gldTerminateLibrary` rend la
  tranche du kext (`docs/re/accelerateur-iokit.md` §4).
- **Ce qui est annoncé est tenu.** (01/11 : paragraphe d'avant la v10. Sur un device v12 ou
  plus qui tient textures 3D, cubes, 1.3 et 1.4, et v8, le plugin annonce désormais
  **« 1.5 POMPPC-1.0 »** ; voir `docs/protocole.md` et `docs/re/version-extensions.md`.)
  `GL_VERSION = "1.1 POMPPC-1.0"` — la plus haute version dont
  *toutes* les fonctions sont tenues par la chaîne (plugin + hôte + repli exact sur le rendu
  d'Apple). Pas 1.2, parce que les **textures 3D** manquent partout dans la chaîne. Le plugin
  ajoute au tableau de bits d'extensions d'Apple exactement trois noms, chacun vérifié au rendu :
  `GL_ARB_occlusion_query` (et seulement sur un device v8), `GL_ARB_vertex_buffer_object` et
  `GL_EXT_blend_func_separate` — 42 extensions au lieu de 39. Les limites d'Apple (8 unités de
  texture, 4096 de côté) sont **laissées telles quelles** : au-delà du chemin accéléré, le repli
  tient, c'est mesuré. Le relevé fonction par fonction est dans `docs/re/version-extensions.md`.

## Construire et installer (dans l'invité)

```sh
sudo sh install.sh            # kext + plugin, voir l'en-tête du script
sudo sh install.sh --remove
```

Kext dans `/System/Library/Extensions/POMPPCGPU.kext`, plugin dans
`/System/Library/Extensions/GLDriver-POMPPC.bundle` (disposition standard,
`Contents/MacOS/`). Une installation d'avant la tâche 4.2 avait mis le plugin
dans `Resources` d'OpenGL.framework : `install.sh` l'en retire.

`GL_RESOURCES` ne marche pas sur Tiger 10.4.6 (`kCGLBadCodeModule`,
`docs/gpu-3d-tiger.md` §5.1) : il n'y a pas d'essai « sans installation ».

## Variables d'environnement

| Variable | Effet |
|---|---|
| `POMPPC_GL_DISABLE=1` | aucune accélération : le plugin n'est qu'un mandataire |
| `POMPPC_GL_STATS=1` | bilan sur stderr en fin de processus (triangles, soumissions, relectures…) |
| `POMPPC_GL_ARRAY_STUB_SYNC=1` | témoin A/B : rétablit les synchronisations couleur/profondeur avant le refus vide de `RenderVertexArray` d'Apple. Par défaut, le plugin évite ces transferts seulement si les instructions de la fonction sont exactement `li r3,0; blr`. Toute valeur définie active ce témoin ; retirer la variable pour le chemin corrigé. |
| `POMPPC_GL_FRAMES=/chemin.csv` | trace optionnelle par échange : contexte, temps monotone en ms depuis le premier échange, compteurs cumulés de géométrie/replis/relectures et temps cumulés de soumission/attente/copie. Fichier remplacé au lancement, écrit avec tampon, vidé au moins toutes les 5 s pendant le rendu ; un arrêt brutal peut perdre la fin. Mesure les appels d'échange, pas la fin GPU. Analyse : `python3 tools/guest/frame_report.py trace.csv --start 30 --duration 60` (fenêtre à choisir après vérification du chargement et de la scène). |
| `POMPPC_GL_STATS=/chemin` | bilan ajouté au fichier toutes les 5 s : images/s, relectures, replis logiciels, temps de soumission, sommets bruts / `DRAW_RAW` / commandes d'état / **dessins fusionnés** / **sommets par dessin** par image, **barrières attendues et temps d'attente par image, profondeur de file, `QUEUE_FULL`, replis synchrones**, et motifs de refus de l'accélération avec le premier cas |
| `POMPPC_GL_DIRECT=0` | pas de présentation directe (voir ci-dessous) ; `=f` : plein écran seulement ; `=c` : même avec un curseur en mouvement dans la surface. Sous Quartz Extreme, plein écran seulement quel que soit le réglage ; non lue dans le WindowServer |
| `POMPPC_GL_PRESENT=0` | pas de `SURF_PRESENT` (relecture + copie G4, pour A/B) ; défaut : hôte → VRAM si device v13 |
| `POMPPC_GL_PIXEL=0` | pas de `COPY_TEX` / `ReadPixels` hôte (relecture complète + Apple, pour A/B) ; défaut : device v13 |
| `POMPPC_GL_VBO=0` | pas de tampons hôte v14 (`DRAW_RAW` retraverse BAR0 à chaque dessin, pour A/B) ; défaut : device v14 |
| `POMPPC_GL_XFER16=0` | pas de transfert 16 bits hôte (v15) ; défaut : device v15 copie RGB1555 / Z16 tels quels, conversion par l'hôte |
| `POMPPC_GL_GEOM=0` | coupe le **chemin brut** : GLEngine transforme et éclaire de nouveau lui-même, comportement d'avant le lot 2. `=1` (défaut) l'active ; `=2` l'active avec un format de sommet fixe et large, pour mesurer |
| `POMPPC_GL_ARRAY=1` | **canal mixte** : `cfg+0x78` si `GL_VERTEX_ARRAY` actif, descripteur `cfg+0x11c` **gardé** pour `glBegin`. `=2` retire le descripteur (GeForce3 strict). **`=0` (défaut)** coupe : Begin/End seulement |
| `POMPPC_GL_MERGE=0` | coupe la **fusion** des `DRAW_RAW` consécutifs en triangles indexés (repli et comparaison). `=1` (défaut) l'active. Sans elle, seuls les lots `TRIANGLES`, `QUADS`, `LINES` et `POINTS` de même mode se recollent bout à bout, comme avant |
| `POMPPC_GL_GENSIZES=0` | envoie les **attributs génériques** du chemin tableaux à 4 flottants, comme avant. `=1` (défaut) les envoie à la taille de leur tableau (clé `QGPU_SK_GEN_SIZES` : DOOM 3 / Prey, 11 mots de génériques par sommet au lieu de 16), si le device annonce `QGPU_CAP_GEN_SIZES` et que les programmes ARB sont actifs (`docs/protocole-v17-generiques-tailles.md`) |
| `POMPPC_GL_ASYNC=0` | coupe le **doorbell asynchrone** : chaque soumission attend la fin du rendu hôte, comme avant la v9 (repli et comparaison). `=1` (défaut) l'active, à condition que le device soit v9, qu'il annonce `QGPU_CAP_ASYNC`, que le kext installé connaisse le drapeau et que la tranche tienne deux moitiés — sinon le mode synchrone est gardé et la raison est dite dans le journal |
| `POMPPC_GL_IDXLAZY=1` | ~~éteint par défaut~~ **allumé par défaut depuis le 04/10** (`=0` l'éteint ; 03/10, `docs/d3-plugin-x86.md` §2) : un dessin `DRAW_NATIVE` indexé dont tous les VBO lus ont un miroir propre part sans balayage de ses indices (commande identique) ; `=2` balaie quand même et compte ce que l'ancienne voie aurait refusé |
| `POMPPC_GL_IDXVEC=1` | ~~éteint par défaut~~ **allumé par défaut depuis le 04/10** : balayage min/max des indices 16 et 32 bits par AltiVec (`pomppc_vec.c`) ; `=2` le compare au scalaire |
| `POMPPC_GL_UNITVD=1` | ~~éteint par défaut~~ **allumé par défaut depuis le 04/10** (§3) : un dispatch qui ne relie que des textures refait le verdict des seules unités désignées ; contrôlé par `POMPPC_GL_VERDICTCHECK=1` |
| `POMPPC_GL_DISPONE=1` | ~~éteint par défaut~~ **allumé par défaut depuis le 04/10** (§4) : fin de `gldUpdateDispatch` sous un seul verrou, crochetage sauté quand la table n'a pas bougé |
| `POMPPC_GL_ZONES=0`, `POMPPC_GL_STATEBLK=0`, `POMPPC_GL_RAWSANE=0`, `POMPPC_GL_NATSHM=0`, `POMPPC_GL_QFLUSH=0` | coupent des leviers **allumés par défaut** : indices clients dans l'arène (`docs/d3-plugin-x86.md` §9), bloc d'état, tri des sommets et tableaux clients par l'hôte (`docs/protocole-v23-etat.md`, `-geometrie.md`), coupe au premier `QUERY_BEGIN` (`docs/backend-gl-attente.md` §9) |
| `POMPPC_GL_DISPFAST`, `PROGMEMO`, `MTXDIRTY`, `STUNIT`, `DISPLOCK`, `BEGINVD`, `LIGHTDIRTY`, `SWAPLITE`, `DISPSEED`, `WINSEED`, `UVWIDE`, `PACKBITS` (`=0`) | leviers d'allègement du plugin, **tous allumés par défaut** (`docs/d3-plugin-x86.md` §10, `docs/plugin-allegement.md` §6 à §8) ; `=2` contrôle pour ceux qui l'ont. `POMPPC_GL_PRESENTQ` reste éteint |
| `POMPPC_GL_QE_DIRECT=0`, `POMPPC_GL_QE_ASYNC=0`, `POMPPC_GL_QE_NONSIMPLE=0`, `POMPPC_GL_QE_HZ=n`, `POMPPC_GL_QE_NONSIMPLE_HZ=n` | Quartz Extreme (`docs/quartz-extreme.md`) : présentation directe par le noyau, Flush asynchrone, avis des surfaces non simples (défaut 20 Hz ; 0 = la cadence de `QE_HZ`), cadence des avis au WindowServer (défaut 60 Hz) |
| `POMPPC_GL_GEOM_SLOTS=n` | plafonne le nombre de sommets offerts à un `BeginPrimitiveBuffer` (mesure : c'est ainsi qu'on a établi comment GLEngine coupe une longue primitive) |
| `POMPPC_GL_NOTE=/chemin` | journal d'appoint (chargement du plugin, attachements de drawable). **Sans cette variable, rien n'est écrit** : le plugin tourne aussi dans le WindowServer et `loginwindow`, donc en root, et le fichier d'avant (`/tmp/pomppc-gl.log`, sans interrupteur) était une cible de lien symbolique. Le fichier est ouvert une seule fois, en `O_EXCL | O_NOFOLLOW` : un chemin **déjà pris** est un refus, jamais une écriture ailleurs — choisir un nom neuf à chaque essai |
| `POMPPC_GLTRACE=dossier` | trace de chaque appel `gld*` et de chaque procédure, avec vidages binaires |
| `POMPPC_GLTRACE_STATE=1` | en trace, vide l'état GL complet à chaque effacement (sans elle, au premier seulement) |
| `POMPPC_GLTRACE_VERTS=1` | en trace, vide aussi en binaire les sommets des procédures de rastérisation |
| `POMPPC_GL_ANNOUNCE=0` | n'ajoute rien à l'annonce d'Apple : `GL_VERSION` et la liste d'extensions redeviennent les siennes (comparaison) |
| `POMPPC_GL_ALLEXT=1` | **sonde** : allume les 79 bits du tableau d'extensions, pour lire la table bit → nom (expérience V3 de `docs/re/capacites-glengine.md`) |
| `POMPPC_GL_TRY3D=n` | **sonde** : déclare une taille maximale de texture 3D, pour vérifier par l'expérience que le repli logiciel ne sait pas les échantillonner |

## Limites connues

- Textures 3D/cube/rectangle, plus de 4 unités, modes de répétition ou
  filtres hors du domaine du protocole (01/11 : liste de septembre ; textures 3D, cube et
  rectangle en v10, 8 unités en v17). `glCopyTexSubImage2D`,
  `glReadPixels` (RGBA/RGB octet, profondeur FLOAT, stencil octet),
  `glDrawPixels` / `glCopyPixels` COLOR/DEPTH/STENCIL et `glBitmap`
  passent par l'hôte dès la v13 (`POMPPC_GL_PIXEL=0` les rend à Apple).
  DEPTH/STENCIL : seulement si le test est coupé ou `ALWAYS` et le masque
  d'écriture est plein. Les polices `GL_ALPHA` (Warcraft III) partent en RGBA blanc+A
  pour que le `MODULATE` hôte garde la couleur du sommet. Drawables 16 bits
  (RGB1555) et Z 16 : memcpy G4 + conversion hôte dès la v15
  (`POMPPC_GL_XFER16=0` les laisse sans aller-retour).
- **Hors du domaine du chemin brut** (le rendu d'Apple reprend tout le pipeline
  de sommets, image exacte) : lissage (`GL_*_SMOOTH`), atténuation de la taille des points par la
  distance, programmes ARB de sommets ou de fragments, et tout ce qui sort déjà du domaine
  de la rastérisation. (01/11 : les programmes ARB sont tenus par l'hôte depuis la v16, par le
  chemin des tableaux natifs.)
- **Canal tableaux** : un `glBegin` pendant que `GL_VERTEX_ARRAY` est actif est jeté (sans
  quitter le domaine). Le chemin `AllocVertexBuffer` ignore le tampon packé par GLEngine et
  relit les tableaux clients — les indices sont conservés, la recopie vers le tampon de 2048
  sommets est perdue. `POMPPC_GL_ARRAY=0` rétablit l'ancien déroulement par Begin/End.
  En v14, un VBO dont tous les attributs dessinés viennent d'objets tampon est
  emballé une fois puis dessiné par `DRAW_RAW_BUF` (`POMPPC_GL_VBO=0` retraverse BAR0).
- **Modes de polygone et pointillé de ligne : chemin brut seulement** (voir plus haut) ; par le
  chemin hérité, le rendu d'Apple les fait, exactement.
- Le chemin brut ne porte pas la **couleur secondaire par sommet** ; elle passe en valeur courante
  (`SET_CURRENT`). Mesuré depuis : **ni GLEngine ni le rendu d'Apple ne tiennent `GL_COLOR_SUM`**,
  qui n'ajoute rien à la couleur primaire — ce n'est donc pas une perte du chemin brut, et
  `GL_EXT_secondary_color` n'est pas annoncée.
- Une **arête posée exactement sur une frontière de pixels** (coordonnée entière) peut tomber sur
  l'une ou l'autre des deux lignes voisines : GLEngine et l'hôte ne choisissent pas la même. C'est
  dans la latitude d'OpenGL, et cela ne se voit que sur les tracés en fil de fer.
- 4 processus GL accélérés à la fois au plus (tranches du kext) ; le 5e est rendu
  en logiciel. Sous Quartz Extreme, le WindowServer en prend une.
- Chaque échange (`glFinish`, `CGLFlushDrawable`) relit l'image hôte dans la
  mémoire invitée : c'est le coût dominant sur les petites scènes, **sauf**
  avec un device v13 (`SURF_PRESENT`) : l'hôte écrit alors lui-même dans la
  VRAM QFB, 1555 compris, et le G4 ne touche plus un pixel à la présentation.
  En fenêtre, le WindowServer recopie ensuite l'image et l'échange l'attend.
- **En asynchrone, la présentation directe a une image de retard** : l'écran
  montre l'image *n−1* pendant qu'on prépare la *n+1*. Une application qui
  **cesse** de dessiner laisse donc sa dernière image en vol jusqu'au prochain
  point de synchronisation (un `glReadPixels`, un chemin logiciel, la fermeture
  du contexte), qui la présente. Et si la fenêtre bouge entre la demande de
  relecture et la copie, une image part à l'ancienne position.
- **`QGPU_REG_ERRORS` est global au device**, et le balayage de fermeture du
  kext (148 identifiants détruits par client qui s'en va, dont la plupart
  n'existent pas) le fait monter sans que personne n'ait de bogue : 8 877
  erreurs comptées sur une VM qui rendait des images justes depuis des heures.
  Le plugin ne peut donc pas s'en servir pour **nommer** une soumission
  fautive ; quand le compteur bouge, il repasse en synchrone pendant 120 images
  — le temps que l'erreur, si elle est la nôtre, revienne avec son statut et son
  `pc` exacts — puis reprend l'asynchrone. Vu en vrai : un repli par application
  GL qui se ferme à côté, suivi d'une reprise. (Le balayage est fait par le
  device depuis la v19, sans erreur ; restaient la sonde attendue ou le flux
  fautif d'un autre processus.) **Depuis le 07/10/2026**, sur un device qui
  annonce `QGPU_CAP_CLIENT_ERRORS`, le global n'est plus qu'un déclencheur :
  quand il bouge, le plugin relit `QGPU_REG_CLIENT_ERRORS` de **sa** tranche
  (`QGPU_UC_READ_REG`, un appel au kext) et, s'il n'a pas bougé, se recale et
  reste en asynchrone, miroirs intacts (`POMPPC_GL_STATS` : « foreign error
  move(s) » ; trace : « aucune de notre tranche »). Le repli de 120 images ne
  vise plus que ses propres erreurs. Sans le bit, ou si sa tranche n'est pas
  celle que le device calcule (autre kext), règle globale d'avant.
- Tampons de dessin 32 bits. Une demande 16 bits (Warcraft III / Colin McRae
  « milliers de couleurs ») est élevée en 32 côté hôte (01/11 : depuis la v15, un drawable
  RGB1555 + Z16 est dans le domaine, voir `POMPPC_GL_XFER16`) ; `aglSetFullScreen`
  peut commuter l'écran QFB en 1555, et le swap convertit xRGB → 1555.
- Les pixels exactement sur une arête peuvent différer du rendu d'Apple (règle
  de remplissage du GPU hôte).

### Plein écran CGL (Tiger)

Les demandes `CGLSetFullScreen` sont raccordées à un tampon arrière mémoire
à la taille de l’écran principal 32 bits. Le swap présente aussi les replis
logiciels Apple. Validation UT2004 en 800×600 et 1024×768 :
[notes et reproduction](../../docs/re/ut2004-fullscreen.md).
