# GLDriver-POMPPC — plugin OpenGL de Tiger pour le GPU paravirtuel qgpu

Le plugin que GLEngine (`OpenGL.framework`) charge comme un rendu « GLDriver* ».
Il fait rendre la géométrie des applications OpenGL de Tiger par le GPU de
l'hôte, à travers le kext `POMPPCGPU` et le device QEMU `qgpu-pci`.
Conception, rétro-ingénierie et mesures : `docs/gpu-3d-tiger.md`.

## Principe

- **Mandataire du rendu logiciel d'Apple.** Le plugin charge en module privé
  `GLDriver.bundle` (le rendu « Generic » de Tiger) et lui transmet les 63
  points d'entrée `gld*`, par des trampolines assembleur (`gld_tramp.s`,
  généré par `tools/gld/gen_tramp.py`) qui ne touchent à aucun argument. Tout
  ce que le plugin n'accélère pas reste donc exact.
- **Accélération au niveau de la rastérisation.** GLEngine transforme, éclaire,
  découpe et élimine les faces, puis appelle les procédures installées par
  `gldInitDispatch` avec des sommets en coordonnées fenêtre. Le plugin remplace
  l'effacement et les procédures de triangles (et bandes, éventails, quads,
  polygones), de lignes et de points : elles deviennent des commandes qgpu,
  groupées et soumises au kext. Les textures sont suivies (création, niveaux,
  modifications) et recopiées sur l'hôte à la première utilisation après
  changement.
- **Deux copies, un drapeau de fraîcheur.** La surface hôte double le tampon de
  dessin du rendu logiciel (couleur et profondeur). Avant un dessin hôte, ce que
  le logiciel a dessiné est téléversé ; avant un échange, un vidage ou tout
  chemin logiciel, ce que l'hôte a dessiné est relu. Un effacement complet ne
  téléverse rien, et la profondeur n'est relue que si un chemin logiciel s'en
  sert.
- **État accéléré.** Profondeur, masques, mélange, test alpha, ciseaux, ombrage
  lisse ou plat, brouillard, décalage de polygone, largeur de ligne et taille de
  point, et deux unités de texture 2D/1D (six formats de base, texels en octets
  ou compacts 8888/1555/565/4444, filtres avec
  mipmaps, modes de répétition, environnements MODULATE/REPLACE/DECAL/BLEND/ADD).
  Hors de ce domaine (liste complète : `docs/gpu-3d-tiger.md` §4.5), le plugin
  synchronise puis laisse faire le code d'Apple.
- **Présentation directe.** Une application au premier plan reçoit son image
  directement dans la mémoire vidéo, sans passer par le WindowServer : en plein
  écran, et en fenêtre tant que rien ne recouvre la surface et que le curseur
  n'y bouge pas (`docs/gpu-3d-tiger.md` §4.4).
- **Identité.** Identifiant de plugin `0x7700` (renderer `0x00027700`), annoncé
  accéléré ; `GL_VENDOR = POMPPC`, `GL_RENDERER = POMPPC qgpu (OpenGL host GPU)`.
  Le bundle s'appelle `GLDriver-POMPPC` pour être chargé avant le GLDriver
  d'Apple : CGL retient le premier renderer qui convient, et le nôtre est aussi
  le seul à répondre à `kCGLPFAAccelerated`.

## Construire et installer (dans l'invité)

```sh
sudo sh install.sh            # kext + plugin, voir l'en-tête du script
sudo sh install.sh --remove
```

Sans installation, pour un seul processus :

```sh
make glres                                   # glres/ = ce plugin + le rendu flottant d'Apple
GL_RESOURCES=$PWD/glres/ ./mon_application   # GLEngine lit ce dossier au lieu de Resources/
```

## Variables d'environnement

| Variable | Effet |
|---|---|
| `POMPPC_GL_DISABLE=1` | aucune accélération : le plugin n'est qu'un mandataire |
| `POMPPC_GL_STATS=1` | bilan sur stderr en fin de processus (triangles, soumissions, relectures…) |
| `POMPPC_GL_STATS=/chemin` | bilan ajouté au fichier toutes les 5 s : images/s, relectures, replis logiciels, temps de soumission, et motifs de refus de l'accélération avec le premier cas |
| `POMPPC_GL_DIRECT=0` | pas de présentation directe (voir ci-dessous) ; `=f` : plein écran seulement ; `=c` : même avec un curseur en mouvement dans la surface |
| `POMPPC_GLTRACE=dossier` | trace de chaque appel `gld*` et de chaque procédure, avec vidages binaires |
| `POMPPC_GLTRACE_STATE=1` | en trace, vide l'état GL complet à chaque effacement |

## Limites connues

- Trois unités de texture ou plus, GL_COMBINE, textures 3D/cube/rectangle,
  stencil, opérations de pixels (`glBitmap`, `glDrawPixels`) : rendus par le
  logiciel, correctement, avec une relecture à chaque changement de chemin.
- 4 processus GL accélérés à la fois au plus (tranches du kext) ; le 5e est rendu
  en logiciel.
- Chaque échange (`glFinish`, `CGLFlushDrawable`) relit l'image hôte dans la
  mémoire invitée : c'est le coût dominant sur les petites scènes. En fenêtre,
  le WindowServer recopie ensuite l'image et l'échange l'attend.
- Tampons 32 bits seulement (couleur « Millions », profondeur 32 bits du rendu
  d'Apple) ; sinon, logiciel.
- Les pixels exactement sur une arête peuvent différer du rendu d'Apple (règle
  de remplissage du GPU hôte).
