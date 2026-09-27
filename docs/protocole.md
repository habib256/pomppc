# Protocole qgpu — contrat courant v22

Référence consolidée au 27/09/2026 pour le plugin invité, le device QEMU et les
tests natifs. Les notes `protocole-v7…v21-*.md` conservent l'historique et les
preuves des changements ; ce document décrit le contrat courant.

## Sources et compatibilité

| Source | Propriétaire | Ce qu'elle définit |
|---|---|---|
| `patches/qgpu/qgpu_abi.h` | Device + kext + plugin | Transport : PCI, BAR, registres, barrières, clients, user client |
| `patches/qgpu/qgpu_proto.h` | Device + plugin | Commandes GL, clés, formats, objets et limites |
| `patches/qgpu/qgpu-core.c` | Cœur commun aux backends | Validation des flux et sémantique exécutée |
| `tests/qgpu_core_test.c` | Validation native | Cas acceptés/refusés, équivalence des chemins et limites |

Le kext ne compile **aucun opcode GL**. Modifier le protocole sémantique demande
de reconstruire QEMU et le plugin ; modifier l'ABI demande aussi le kext et un
redémarrage de Tiger. La copie de `qgpu_abi.h` dans `kext/POMPPCGPU/` doit
rester identique. Le numéro publié par le device est **22** ; « kext v19 »
désigne sa génération d'ABI, pas un second registre de version.

Le plugin vérifie la version minimale, la lecture des registres par le kext,
`QGPU_CAP_CLIENTS`, puis les plages d'identifiants publiées par le device.
Il exige des plages compatibles avec ses constantes. Une version seule ne
suffit jamais à autoriser une fonction optionnelle : tester aussi sa capacité.
Le plancher historique `QGPU_PROTO_MIN=12` ne dispense pas du transport v19.

| Fonction | Version minimale / capacité |
|---|---|
| Requêtes d'occlusion | v8, `QGPU_CAP_OCCLUSION` |
| Soumissions asynchrones | v9, `QGPU_CAP_ASYNC` |
| Textures et état GL 1.4 | v10, `QGPU_CAP_GL14` |
| Présentation directe | v13, `QGPU_CAP_SCANOUT` |
| Tampons hôte / transferts 16 bits | v14 / v15 |
| Programmes ARB | v16, `QGPU_CAP_PROGRAMS` |
| 8 unités / génériques courts | v17 / `QGPU_CAP_GEN_SIZES` |
| Attributs natifs de VBO | v18, `QGPU_CAP_NATIVE` |
| Tranches de clients | v19, `QGPU_CAP_CLIENTS` |
| Surface vers texture / lecture de texture | v20, `QGPU_CAP_SURF_TEX` / `QGPU_CAP_TEX_READBACK` |
| Programmes GLSL et 16 unités d'image | v21, `QGPU_CAP_GLSL` |
| Combineurs ATI et sources ZERO/ONE | v22, `QGPU_CAP_COMBINE3` |

Le backend logiciel ne promet ni programmes ARB ni GLSL. L'annonce GLSL du backend
GL dépend des points d'entrée 2.0, d'au moins 16 unités d'image et d'un programme
d'essai lié à l'initialisation. Le plugin conserve l'annonce OpenGL **1.5** :
le protocole v22 ne signifie pas une implémentation complète d'OpenGL 2.0.

### Combineurs ATI (v22)

`ATI_texture_env_combine3` ajoute trois fonctions RGB et alpha : `MODULATE_ADD`
(Arg0 × Arg2 + Arg1), `MODULATE_SIGNED_ADD` (idem − 0,5), `MODULATE_SUBTRACT`
(Arg0 × Arg2 − Arg1). L'échelle 1/2/4 s'applique avant le bornage à [0,1].
Voir la [spécification ATI](https://registry.khronos.org/OpenGL/extensions/ATI/ATI_texture_env_combine3.txt).

Les codes 8/9/10 occupent les champs de quatre bits déjà réservés dans
`QGPU_SK_COMBINE(u)`. Deux bits par argument ajoutent les sources littérales :
RGB aux bits 12..17, alpha aux bits 18..23, dans l'ordre des arguments 0/1/2.
Valeurs : 0 = source habituelle de `COMBINE_SRC`, 1 = ZERO, 2 = ONE, 3 = invalide.
Une source littérale ignore les trois bits source de `COMBINE_SRC` mais conserve
l'opérande (couleur, alpha, inverse). Les bits 24..31 restent nuls. Les anciens
flux ne changent pas ; les sources croisées 0..3 conservent leur encodage.

Le cœur refuse les fonctions inconnues, DOT3 en alpha, les échelles invalides et
les champs réservés (`BAD_ARG`, sans mutation). Sans `QGPU_CAP_COMBINE3`, toute
opération ATI ou source littérale est refusée (`BACKEND`, sans mutation).
Le backend logiciel les calcule ; le backend GL n'annonce la capacité que si
l'hôte annonce l'extension ATI. Le plugin exige version ≥22 **et** capacité,
puis expose le bit 70 des extensions de GLEngine. `POMPPC_GL_COMBINE3=0`
permet une comparaison sans annonce. Les huit unités fixes restent inchangées.

## Transport, mémoire et durée de vie

Le device est un coprocesseur de commandes, pas un écran. BAR0 contient la mémoire
partagée (64 Mio par défaut, 16–256 Mio configurables) ; BAR1 expose les registres
32 bits dans une BAR de 4 Kio. Les registres définis sont sous `0x100`.
Les registres, commandes, flottants IEEE simple précision et indices sont
**big-endian**. Un offset de commande est en octets dans BAR0 ; le user client
traduit l'offset de soumission relatif à sa tranche. Les adresses de données
du flux sont des offsets BAR0, jamais des pointeurs du processus hôte.

Le device publie quatre tranches par défaut. Les plages globales sont :
32 contextes, 32 surfaces, 1024 textures, 16 requêtes et 64 tampons **par client**.
Les 256 programmes sont **par contexte**, dans un espace commun ARB/GLSL.
Le kext lit `CLIENTS` et `LAYOUT` et ne doit pas redéfinir ces nombres.
La table comporte 16 entrées possibles, dont 5 classes actuellement définies.

Le user client fournit `GET_INFO`, `SUBMIT`, `WAIT_FENCE`, `RESET`,
`GET_SLOT` et `READ_REG`, ainsi que le mapping de la tranche.
À la fermeture d'un client, `CLIENT_RESET` fait détruire ses objets par le
device, en FIFO après les soumissions en vol. Un reset global draine l'exécution
courante, abandonne les soumissions en attente et remet les compteurs à zéro.
Les objets de rendu hôte ne sont pas persistés comme un état GL migrable.

## Soumission et barrières

Une commande commence par `opcode << 16 | longueur`, longueur en **mots de
32 bits, en-tête compris**. Les `QGPU_LEN_*` et les opcodes sont recensés
dans l'annexe générée. Le flux est aligné sur 4 octets ; au plus 1 048 576 mots
par soumission et 32 arguments par commande. Chaque soumission sélectionne
son contexte par `CTX_BIND` : ne pas dépendre du contexte d'un autre client.

L'invité écrit le flux et ses données, applique les barrières mémoire du transport,
renseigne `SUBMIT_OFF/LEN`, puis écrit le doorbell :

- **1 (GO)** : synchrone, exécuté après les soumissions antérieures ; attend une
  place si la file est pleine.
- **3 (GO | ASYNC)** : accepté en FIFO ou refusé `QUEUE_FULL` sans exécution.
  Lire `SUBMIT_ST` puis conserver la barrière `FENCE_SUBMITTED`.

`FENCE_SUBMITTED` compte les soumissions acceptées, `FENCE` les terminées.
La file de référence a 16 places ; lire `QUEUE_DEPTH` plutôt que le supposer.
L'hôte publie les résultats et le statut **avant** la barrière. Attendre
`(int32_t)(fence - cible) >= 0` pour gérer le bouclage des compteurs.
Jusque-là, ne modifier ni commandes ni données sources, et ne lire aucune
destination de relecture. Les registres OFF/LEN peuvent être réutilisés dès
l'acceptation ; les octets de BAR0 qu'ils désignent restent occupés.

`STATUS/STATUS_PC` décrivent la dernière soumission terminée, pas nécessairement
celle attendue. `ERRORS` compte les soumissions en erreur et reste **global** :
une erreur d'un autre client peut le faire avancer. `IRQ_DONE` est de niveau,
peut regrouper plusieurs fins ; acquitter puis relire FENCE.

## Objets, commandes et erreurs

| Famille | Opérations et effets |
|---|---|
| Contextes | `CTX_CREATE/DESTROY/BIND` ; état GL propre à chaque contexte |
| Surfaces | `SURF_CREATE/DESTROY/BIND`, couleur et profondeur/stencil optionnels |
| Transferts | `SURF/DEPTH/STENCIL_UPLOAD/READBACK`, rectangles, pas en octets |
| État | `SET_STATE`, matrices, lumières, matériaux, texgen, plans de coupe, valeurs courantes |
| Géométrie | `DRAW_*` hérités, `DRAW_RAW`, `DRAW_RAW_BUF`, `DRAW_NATIVE` |
| Textures | `TEX_CREATE/CREATE3/DESTROY/IMAGE/IMAGE3/SUBIMAGE/PARAM` |
| Tampons | `BUF_CREATE/DESTROY/SUBDATA` ; données conservées côté hôte |
| Requêtes | `QUERY_BEGIN/END/RESULT`, résultat relu dans BAR0 |
| Programmes | `PROG_*` ARB, puis `GLSL_*` sur le même espace d'identifiants |
| Présentation / copie | `SURF_PRESENT`, `COPY_TEX`, `SURF_TEX`, `TEX_READBACK` |

Les signatures ordonnées sont les commentaires `QGPU_OP_*` de l'en-tête ; les
longueurs exactes et valeurs de toutes les clés figurent ci-dessous.
Les dimensions, tailles, débordements et plages mémoire sont validés avant accès.
Un mauvais en-tête ou un opcode inconnu interrompt la soumission. Les commandes
déjà exécutées ne sont pas annulées. Certains refus sont délibérément non fatals :
dessin natif invalide, programme refusé par le compilateur, dessin GLSL cassé.
Il faut donc consulter les contrôles de chaque commande plutôt que considérer
tout `BAD_ARG` comme une annulation du flux.

Les statuts distinguent soumission, en-tête, opcode, argument, accès hors BAR0,
contexte/surface manquants, limite, backend et file pleine. Une soumission refusée
ou échouée ne bloque pas les suivantes.

## Surfaces et textures

Surface : dimensions au plus 4096×4096 ; couleur XRGB8888 avec options profondeur
et stencil. Les rectangles de transfert ont leur origine **en haut à gauche**.
Couleur 32 bits : octets x,R,G,B ; format 16 bits : RGB1555 big-endian.
Profondeur : FLOAT32 ou UNORM16 big-endian. Les longueurs 8/9 des transferts
distinguent le format historique du format explicite ajouté en v15.
`CLEAR` respecte ciseaux et masques d'écriture.

Texture : dimensions 2D au plus 2048, 3D au plus 256, 12 niveaux ; cibles 1D,
2D, 3D, cube et rectangle. Les formats source, types, cibles d'image,
paramètres et combineurs sont les constantes `QGPU_T*`/`QGPU_C*`.
Conversions et décompression S3TC relèvent du cœur, pas d'une promesse du backend.
Une texture rectangle n'a que le niveau 0. L'état du pipeline fixe comprend
8 unités complètes, avec coordonnées, matrice et environnement ; GLSL ajoute
8 unités d'image, sans nouvelles coordonnées conventionnelles.

`SURF_PRESENT [surf, off, stride, x, y, w, h, format]` copie le rectangle vers
la fenêtre visible du scanout QFB. L'offset est relatif à **la base du mode
courant**, pas à BAR0 qgpu ni nécessairement à l'origine de la VRAM.
Pas et profondeur doivent correspondre à l'écran ; tout débordement est refusé.
La conversion RGB1555 est faite par l'hôte.

`COPY_TEX [tex, cible, niveau, x, y, z, sx, sy, w, h]` copie depuis la surface
liée vers un niveau déjà défini ; un rectangle vide est sans effet.
`SURF_TEX [tex, cible, niveau, surf]` redéfinit un niveau 2D/rectangle à la taille
et au format RGBA de la surface, éventuellement d'un autre contexte. C'est une
copie au moment de la commande. Sa ligne 0 reste celle du haut de la surface
(convention `aglSurfaceTexture`), à distinguer de l'orientation de COPY_TEX.
Les copies GPU et le chemin de relecture doivent produire les mêmes texels.

`TEX_READBACK [tex, cible, niveau, off, max]` écrit quatre mots w,h,d,format,
puis les texels ARGB big-endian. Niveau non défini : dimensions nulles.
Ce transfert sert notamment à rendre les vidages autonomes.

## État et géométrie

Les clés `QGPU_SK_*` pilotent profondeur, mélange, alpha, brouillard, stencil,
rasterisation, textures, programmes et génériques. L'annexe donne chaque clé
et les macros d'adressage par unité ; `QGPU_SK_COUNT=162` borne la table.
Les énumérations reprennent OpenGL quand l'en-tête le dit. L'état initial est
celui défini par le cœur (pipeline fixe OpenGL).

`DRAW_RAW` lit position, normale, couleur, couleur secondaire, brouillard,
coordonnées 0…7, puis attributs génériques suivant `QGPU_VF_*`. Les champs
absents prennent les valeurs courantes. Matrices de 16 flottants en ordre
colonne ; 8 lumières et 6 plans de coupe. Indices NONE/U16/U32 big-endian ;
au plus 65536 sommets. Le pas de RAW est en **mots**.
`DRAW_RAW_BUF` ajoute des identifiants de tampons hôte ; `QGPU_BUF_SHMEM`
désigne BAR0 pour les emplacements où cette sentinelle est admise.

`DRAW_NATIVE [mode,n,ibuf,ioff,itype,premier,nattr,aoff]` lit dans BAR0 une
table de descripteurs de six mots :
`[code,buf,offset,pas,type,taille|drapeaux]`. Le pas est cette fois en **octets**,
0 signifie serré. Attributs dans des tampons hôte, jamais dans BAR0 ; indices
dans un tampon hôte ou BAR0. Types entiers ou flottants big-endian, normalisation
explicite des entiers, position obligatoire (ou générique 0), codes uniques.
Le cœur vérifie toute la plage des sommets cités et convertit vers RAW.
Un descripteur ou un dessin natif invalide jette ce dessin sans interrompre
le reste de la soumission.

Les programmes ARB ont des paramètres locaux et d'environnement par cible ;
leur texte est borné à 65536 octets. Leur utilisation exige PROGRAMS, y compris
les attributs génériques. GEN_SIZES réduit le nombre de composantes transférées
sur RAW ; NATIVE prend la taille de ses propres descripteurs.

## GLSL v21

Un programme se crée avec `PROG_CREATE [id,QGPU_PT_GLSL]`. GLSL_SOURCE reçoit
jusqu'à 8 textes ASCII de 256 Kio chacun, par étage vertex/fragment.
GLSL_ATTRIB associe un nom à un emplacement 0…15 avant liaison.
GLSL_UNIFORM déclare nom, type, taille du tableau et emplacement invité :
256 déclarations au plus, noms de 128 octets, table de 1024 emplacements vec4.
Un élément scalaire/vecteur occupe un emplacement ; une matrice 2/3/4 colonnes
en occupe 2/3/4. Recouvrements et noms `gl_*` sont refusés.

GLSL_LINK compile et lie sur l'hôte. L'échec marque le programme cassé, avec un
journal récupérable par GLSL_INFO_LOG, sans arrêter la soumission.
GLSL_UNIFORMS transfère des mots bruts selon les types déclarés ; validation
tout ou rien, flottants finis et bornés, sampler entre 0 et 15.
Les valeurs peuvent être posées avant liaison. Une nouvelle SOURCE après liaison
commence une nouvelle définition et efface sources, déclarations et valeurs.

PROG_BIND GLSL prime sur ARB pour RAW, RAW_BUF et NATIVE, jamais pour les anciens
dessins hérités. Un étage absent utilise le pipeline fixe ; programme cassé ou
jamais lié : dessin refusé. Les matrices, lumières et autres `gl_*` sont l'état
du contexte, y compris quand leur fonction fixe est désactivée.
Un sampler sans texture valide lit du noir. Le backend adapte l'axe y de
`gl_Position` et `gl_FragCoord` ; cette adaptation est invisible sur le fil.

GLEngine garde la compilation et les emplacements exposés au jeu. Le plugin
retransmet les mêmes sources et valeurs à l'hôte ; son préprocesseur contourne
le défaut des conditions imbriquées de Tiger. Voir `re/glsl-glengine.md` pour
les offsets privés de GLEngine, qui ne font pas partie du protocole.

## Vérification et évolution

`python3 scripts/qgpu_contract.py --check` compare l'annexe aux deux en-têtes.
`tests/run-all.sh` contrôle aussi l'identité de l'ABI hôte/kext, l'absence de
sémantique GL dans le kext, le cœur et les backends, les refus et les cas limites.
Les scènes `guest/gltest` et la matrice VM vérifient l'intégration :
les tests natifs seuls ne prouvent pas le comportement de GLEngine.

Toute extension documente ses mots, limites, refus, capacité et version minimale,
puis ajoute une épreuve au cœur avant activation dans le plugin. Les mesures et
versions des binaires installés sont dans TODO/CHANGELOG, pas dans le contrat.

<!-- qgpu-constants -->

## Référence exhaustive des constantes

Générée par `python3 scripts/qgpu_contract.py` depuis les deux en-têtes.
Les expressions C sont conservées ; les longueurs `QGPU_LEN_*` incluent
l'en-tête. Les commentaires et contrats détaillés restent dans les sources.

### qgpu_abi.h

| Symbole | Valeur / expression |
|---|---|
| `QGPU_PCI_VENDOR_ID` | `0x1234` |
| `QGPU_PCI_DEVICE_ID` | `0x0fb2` |
| `QGPU_IOPCI_PRIMARY_MATCH` | `0x0fb21234` |
| `QGPU_MAGIC` | `0x71677031` |
| `QGPU_SHMEM_DEFAULT_MB` | `64` |
| `QGPU_SHMEM_MIN_MB` | `16` |
| `QGPU_SHMEM_MAX_MB` | `256` |
| `QGPU_CTRL_BAR_SIZE` | `4096` |
| `QGPU_CTRL_TOPADDR` | `0x100` |
| `QGPU_REG_MAGIC` | `0x00` |
| `QGPU_REG_VERSION` | `0x04` |
| `QGPU_REG_CAPS` | `0x08` |
| `QGPU_REG_SHMEM_SIZE` | `0x0C` |
| `QGPU_REG_SUBMIT_OFF` | `0x10` |
| `QGPU_REG_SUBMIT_LEN` | `0x14` |
| `QGPU_REG_DOORBELL` | `0x18` |
| `QGPU_REG_FENCE` | `0x1C` |
| `QGPU_REG_STATUS` | `0x20` |
| `QGPU_REG_STATUS_PC` | `0x24` |
| `QGPU_REG_IRQ_MASK` | `0x28` |
| `QGPU_REG_IRQ` | `0x2C` |
| `QGPU_REG_DEBUG` | `0x30` |
| `QGPU_REG_BACKEND_NAME` | `0x34` |
| `QGPU_REG_QUEUE_FREE` | `0x38` |
| `QGPU_REG_FENCE_SUBMITTED` | `0x3C` |
| `QGPU_REG_SUBMIT_ST` | `0x40` |
| `QGPU_REG_ERRORS` | `0x44` |
| `QGPU_REG_QUEUE_DEPTH` | `0x48` |
| `QGPU_REG_CLIENTS` | `0x80` |
| `QGPU_REG_CLIENT_RESET` | `0x84` |
| `QGPU_REG_LAYOUT` | `0x88` |
| `QGPU_REG_LAYOUT_CLASSES` | `16` |
| `QGPU_REG_LAYOUT_CLASS(k)` | `(QGPU_REG_LAYOUT + 4 * (k))` |
| `QGPU_DOORBELL_GO` | `0x00000001` |
| `QGPU_DOORBELL_ASYNC` | `0x00000002` |
| `QGPU_QUEUE_DEPTH` | `16` |
| `QGPU_CAP_ASYNC` | `0x00000008` |
| `QGPU_CAP_CLIENTS` | `0x00000200` |
| `QGPU_IRQ_DONE` | `0x00000001` |
| `QGPU_ST_OK` | `0` |
| `QGPU_ST_BAD_SUBMIT` | `1` |
| `QGPU_ST_BAD_HEADER` | `2` |
| `QGPU_ST_BAD_OPCODE` | `3` |
| `QGPU_ST_BAD_ARG` | `4` |
| `QGPU_ST_OOB` | `5` |
| `QGPU_ST_NO_CTX` | `6` |
| `QGPU_ST_NO_SURF` | `7` |
| `QGPU_ST_LIMIT` | `8` |
| `QGPU_ST_BACKEND` | `9` |
| `QGPU_ST_QUEUE_FULL` | `10` |
| `QGPU_UC_GET_INFO` | `0` |
| `QGPU_UC_SUBMIT` | `1` |
| `QGPU_UC_WAIT_FENCE` | `2` |
| `QGPU_UC_RESET` | `3` |
| `QGPU_UC_GET_SLOT` | `4` |
| `QGPU_UC_READ_REG` | `5` |
| `QGPU_UC_METHOD_COUNT` | `6` |
| `QGPU_UC_MEM_SHMEM` | `0` |

### qgpu_proto.h

| Symbole | Valeur / expression |
|---|---|
| `QGPU_PROTO_MIN` | `12` |
| `QGPU_PROTO_VERSION` | `22` |
| `QGPU_CAP_SOFT` | `0x00000001` |
| `QGPU_CAP_GL` | `0x00000002` |
| `QGPU_CAP_OCCLUSION` | `0x00000004` |
| `QGPU_CAP_GL14` | `0x00000010` |
| `QGPU_CAP_SCANOUT` | `0x00000020` |
| `QGPU_CAP_PROGRAMS` | `0x00000040` |
| `QGPU_CAP_GEN_SIZES` | `0x00000080` |
| `QGPU_CAP_NATIVE` | `0x00000100` |
| `QGPU_CAP_SURF_TEX` | `0x00000400` |
| `QGPU_CAP_TEX_READBACK` | `0x00000800` |
| `QGPU_CAP_GLSL` | `0x00001000` |
| `QGPU_CAP_COMBINE3` | `0x00002000` |
| `QGPU_MAX_CTX` | `128` |
| `QGPU_MAX_SURF` | `128` |
| `QGPU_MAX_SURF_DIM` | `4096` |
| `QGPU_MAX_CMD_WORDS` | `(1 << 20)` |
| `QGPU_MAX_VERTS` | `(1 << 16)` |
| `QGPU_MAX_TEX` | `4096` |
| `QGPU_MAX_TEX_DIM` | `2048` |
| `QGPU_MAX_TEX_LEVELS` | `12` |
| `QGPU_MAX_TEX_3D_DIM` | `256` |
| `QGPU_MAX_LOD_BIAS` | `16` |
| `QGPU_MAX_UNITS` | `8` |
| `QGPU_MAX_IMAGE_UNITS` | `16` |
| `QGPU_MAX_LIGHTS` | `8` |
| `QGPU_MAX_CLIP_PLANES` | `6` |
| `QGPU_MAX_QUERIES` | `64` |
| `QGPU_MAX_BUF` | `256` |
| `QGPU_MAX_BUF_SIZE` | `(16u * 1024u * 1024u)` |
| `QGPU_BUF_SHMEM` | `0xFFFFFFFFu` |
| `QGPU_MAX_PROG` | `256` |
| `QGPU_PROG_NONE` | `0xFFFFFFFFu` |
| `QGPU_MAX_PROG_LEN` | `65536` |
| `QGPU_MAX_PROG_PARAMS` | `256` |
| `QGPU_MAX_GLSL_SRC` | `8` |
| `QGPU_MAX_GLSL_LEN` | `(256u * 1024u)` |
| `QGPU_MAX_GLSL_UNIFORMS` | `256` |
| `QGPU_MAX_GLSL_SLOTS` | `1024` |
| `QGPU_MAX_GLSL_NAME` | `128` |
| `QGPU_MAX_GLSL_ATTRIBS` | `16` |
| `QGPU_MAX_GLSL_LOG` | `(64u * 1024u)` |
| `QGPU_MAX_CMD_ARGS` | `32` |
| `QGPU_CMD_HDR(op, len)` | `(((unsigned long)(op) << 16) \| ((unsigned long)(len) & 0xFFFF))` |
| `QGPU_CMD_OP(hdr)` | `(((unsigned long)(hdr) >> 16) & 0xFFFF)` |
| `QGPU_CMD_LEN(hdr)` | `((unsigned long)(hdr) & 0xFFFF)` |
| `QGPU_OP_NOP` | `0x0000` |
| `QGPU_OP_CTX_CREATE` | `0x0001` |
| `QGPU_OP_CTX_DESTROY` | `0x0002` |
| `QGPU_OP_CTX_BIND` | `0x0003` |
| `QGPU_OP_SURF_CREATE` | `0x0010` |
| `QGPU_OP_SURF_DESTROY` | `0x0011` |
| `QGPU_OP_SURF_BIND` | `0x0012` |
| `QGPU_OP_SURF_READBACK` | `0x0013` |
| `QGPU_OP_SURF_UPLOAD` | `0x0014` |
| `QGPU_OP_DEPTH_READBACK` | `0x0015` |
| `QGPU_OP_DEPTH_UPLOAD` | `0x0016` |
| `QGPU_OP_STENCIL_READBACK` | `0x0017` |
| `QGPU_OP_STENCIL_UPLOAD` | `0x0018` |
| `QGPU_OP_SURF_PRESENT` | `0x0019` |
| `QGPU_OP_COPY_TEX` | `0x001A` |
| `QGPU_OP_BUF_CREATE` | `0x001B` |
| `QGPU_OP_SURF_TEX` | `0x001E` |
| `QGPU_OP_TEX_READBACK` | `0x001F` |
| `QGPU_OP_BUF_DESTROY` | `0x001C` |
| `QGPU_OP_BUF_SUBDATA` | `0x001D` |
| `QGPU_OP_CLEAR` | `0x0020` |
| `QGPU_OP_VIEWPORT` | `0x0021` |
| `QGPU_OP_SET_STATE` | `0x0022` |
| `QGPU_OP_DRAW_TRIANGLES` | `0x0030` |
| `QGPU_OP_DRAW_TRIANGLES_TEX` | `0x0031` |
| `QGPU_OP_DRAW_TRIANGLES_TEX2` | `0x0032` |
| `QGPU_OP_DRAW_LINES` | `0x0033` |
| `QGPU_OP_DRAW_POINTS` | `0x0034` |
| `QGPU_OP_DRAW_TRIANGLES_TEXN` | `0x0035` |
| `QGPU_OP_DRAW_TRIANGLES_SEC` | `0x0036` |
| `QGPU_OP_TEX_CREATE` | `0x0040` |
| `QGPU_OP_TEX_DESTROY` | `0x0041` |
| `QGPU_OP_TEX_IMAGE` | `0x0042` |
| `QGPU_OP_TEX_PARAM` | `0x0043` |
| `QGPU_OP_TEX_CREATE3` | `0x0044` |
| `QGPU_OP_TEX_IMAGE3` | `0x0045` |
| `QGPU_OP_TEX_SUBIMAGE` | `0x0046` |
| `QGPU_OP_SET_MATRIX` | `0x0050` |
| `QGPU_OP_DEPTH_RANGE` | `0x0051` |
| `QGPU_OP_SET_LIGHT` | `0x0052` |
| `QGPU_OP_SET_MATERIAL` | `0x0053` |
| `QGPU_OP_SET_LIGHT_MODEL` | `0x0054` |
| `QGPU_OP_SET_TEXGEN` | `0x0055` |
| `QGPU_OP_SET_CLIP_PLANE` | `0x0056` |
| `QGPU_OP_SET_CURRENT` | `0x0057` |
| `QGPU_OP_DRAW_RAW` | `0x0058` |
| `QGPU_OP_DRAW_RAW_BUF` | `0x0059` |
| `QGPU_OP_DRAW_NATIVE` | `0x005A` |
| `QGPU_OP_SET_POLYGON_STIPPLE` | `0x0060` |
| `QGPU_OP_QUERY_BEGIN` | `0x0061` |
| `QGPU_OP_QUERY_END` | `0x0062` |
| `QGPU_OP_QUERY_RESULT` | `0x0063` |
| `QGPU_OP_PROG_CREATE` | `0x0070` |
| `QGPU_OP_PROG_STRING` | `0x0071` |
| `QGPU_OP_PROG_DESTROY` | `0x0072` |
| `QGPU_OP_PROG_BIND` | `0x0073` |
| `QGPU_OP_PROG_ENV` | `0x0074` |
| `QGPU_OP_PROG_LOCAL` | `0x0075` |
| `QGPU_OP_GLSL_SOURCE` | `0x0076` |
| `QGPU_OP_GLSL_ATTRIB` | `0x0077` |
| `QGPU_OP_GLSL_UNIFORM` | `0x0078` |
| `QGPU_OP_GLSL_LINK` | `0x0079` |
| `QGPU_OP_GLSL_UNIFORMS` | `0x007A` |
| `QGPU_OP_GLSL_INFO_LOG` | `0x007B` |
| `QGPU_LEN_NOP` | `1` |
| `QGPU_LEN_CTX` | `2` |
| `QGPU_LEN_SURF_CREATE` | `5` |
| `QGPU_LEN_SURF` | `2` |
| `QGPU_LEN_SURF_XFER` | `8` |
| `QGPU_LEN_SURF_XFER_PF` | `9` |
| `QGPU_LEN_SURF_PRESENT` | `9` |
| `QGPU_LEN_COPY_TEX` | `11` |
| `QGPU_LEN_SURF_TEX` | `5` |
| `QGPU_LEN_TEX_READBACK` | `6` |
| `QGPU_LEN_BUF_CREATE` | `3` |
| `QGPU_LEN_BUF` | `2` |
| `QGPU_LEN_BUF_SUBDATA` | `5` |
| `QGPU_LEN_CLEAR` | `4` |
| `QGPU_LEN_VIEWPORT` | `5` |
| `QGPU_LEN_DRAW` | `3` |
| `QGPU_LEN_DRAW_N` | `4` |
| `QGPU_LEN_SET_STATE` | `3` |
| `QGPU_LEN_TEX` | `2` |
| `QGPU_LEN_TEX_IMAGE` | `7` |
| `QGPU_LEN_TEX_PARAM` | `4` |
| `QGPU_LEN_TEX_CREATE3` | `3` |
| `QGPU_LEN_TEX_IMAGE3` | `13` |
| `QGPU_LEN_TEX_SUBIMAGE` | `15` |
| `QGPU_LEN_SET_MATRIX` | `18` |
| `QGPU_LEN_DEPTH_RANGE` | `3` |
| `QGPU_LEN_SET_LIGHT` | `27` |
| `QGPU_LEN_SET_MATERIAL` | `19` |
| `QGPU_LEN_SET_LIGHT_MODEL` | `5` |
| `QGPU_LEN_SET_TEXGEN` | `13` |
| `QGPU_LEN_SET_CLIP_PLANE` | `7` |
| `QGPU_LEN_SET_CURRENT` | `6` |
| `QGPU_LEN_DRAW_RAW` | `10` |
| `QGPU_LEN_DRAW_RAW_BUF` | `12` |
| `QGPU_LEN_DRAW_NATIVE` | `9` |
| `QGPU_LEN_SET_POLYGON_STIPPLE` | `33` |
| `QGPU_LEN_QUERY` | `2` |
| `QGPU_LEN_QUERY_RESULT` | `3` |
| `QGPU_LEN_PROG_CREATE` | `3` |
| `QGPU_LEN_PROG_STRING` | `4` |
| `QGPU_LEN_PROG` | `2` |
| `QGPU_LEN_PROG_BIND` | `3` |
| `QGPU_LEN_PROG_PARAMS` | `5` |
| `QGPU_LEN_GLSL_SOURCE` | `5` |
| `QGPU_LEN_GLSL_ATTRIB` | `5` |
| `QGPU_LEN_GLSL_UNIFORM` | `7` |
| `QGPU_LEN_GLSL_LINK` | `2` |
| `QGPU_LEN_GLSL_UNIFORMS` | `5` |
| `QGPU_LEN_GLSL_INFO_LOG` | `4` |
| `QGPU_FMT_XRGB8888` | `1` |
| `QGPU_FMT_MASK` | `0xFF` |
| `QGPU_FMT_FLAG_DEPTH` | `0x100` |
| `QGPU_FMT_FLAG_STENCIL` | `0x200` |
| `QGPU_PF_XRGB8888` | `0` |
| `QGPU_PF_RGB1555` | `1` |
| `QGPU_DF_FLOAT32` | `0` |
| `QGPU_DF_UNORM16` | `1` |
| `QGPU_CLEAR_COLOR` | `0x1` |
| `QGPU_CLEAR_DEPTH` | `0x2` |
| `QGPU_CLEAR_STENCIL` | `0x4` |
| `QGPU_SK_DEPTH_TEST` | `1` |
| `QGPU_SK_DEPTH_FUNC` | `2` |
| `QGPU_SK_DEPTH_WRITE` | `3` |
| `QGPU_SK_COLOR_MASK` | `4` |
| `QGPU_SK_BLEND` | `5` |
| `QGPU_SK_BLEND_SRC_RGB` | `6` |
| `QGPU_SK_BLEND_DST_RGB` | `7` |
| `QGPU_SK_BLEND_SRC_A` | `8` |
| `QGPU_SK_BLEND_DST_A` | `9` |
| `QGPU_SK_BLEND_EQ_RGB` | `10` |
| `QGPU_SK_BLEND_EQ_A` | `11` |
| `QGPU_SK_ALPHA_TEST` | `12` |
| `QGPU_SK_ALPHA_FUNC` | `13` |
| `QGPU_SK_ALPHA_REF` | `14` |
| `QGPU_SK_SCISSOR` | `15` |
| `QGPU_SK_SCISSOR_X` | `16` |
| `QGPU_SK_SCISSOR_Y` | `17` |
| `QGPU_SK_SCISSOR_W` | `18` |
| `QGPU_SK_SCISSOR_H` | `19` |
| `QGPU_SK_TEXTURE` | `20` |
| `QGPU_SK_TEX_BIND` | `21` |
| `QGPU_SK_TEX_ENV_MODE` | `22` |
| `QGPU_SK_TEX_ENV_COLOR` | `23` |
| `QGPU_SK_FOG` | `24` |
| `QGPU_SK_FOG_COLOR` | `25` |
| `QGPU_SK_LINE_WIDTH` | `26` |
| `QGPU_SK_POINT_SIZE` | `27` |
| `QGPU_SK_TEXTURE1` | `28` |
| `QGPU_SK_TEX1_BIND` | `29` |
| `QGPU_SK_TEX1_ENV_MODE` | `30` |
| `QGPU_SK_TEX1_ENV_COLOR` | `31` |
| `QGPU_SK_POLY_OFFSET` | `32` |
| `QGPU_SK_POLY_FACTOR` | `33` |
| `QGPU_SK_POLY_UNITS` | `34` |
| `QGPU_SK_TEXTURE2` | `35` |
| `QGPU_SK_TEX2_BIND` | `36` |
| `QGPU_SK_TEX2_ENV_MODE` | `37` |
| `QGPU_SK_TEX2_ENV_COLOR` | `38` |
| `QGPU_SK_TEXTURE3` | `39` |
| `QGPU_SK_TEX3_BIND` | `40` |
| `QGPU_SK_TEX3_ENV_MODE` | `41` |
| `QGPU_SK_TEX3_ENV_COLOR` | `42` |
| `QGPU_SK_COMBINE0` | `43` |
| `QGPU_SK_COMBINE_SRC0` | `47` |
| `QGPU_SK_STENCIL_TEST` | `51` |
| `QGPU_SK_STENCIL_FUNC` | `52` |
| `QGPU_SK_STENCIL_REF` | `53` |
| `QGPU_SK_STENCIL_VALUE_MASK` | `54` |
| `QGPU_SK_STENCIL_WRITE_MASK` | `55` |
| `QGPU_SK_STENCIL_OP_FAIL` | `56` |
| `QGPU_SK_STENCIL_OP_ZFAIL` | `57` |
| `QGPU_SK_STENCIL_OP_ZPASS` | `58` |
| `QGPU_SK_STENCIL_CLEAR` | `59` |
| `QGPU_SK_LIGHTING` | `60` |
| `QGPU_SK_NORMALIZE` | `61` |
| `QGPU_SK_RESCALE_NORMAL` | `62` |
| `QGPU_SK_SHADE_MODEL` | `63` |
| `QGPU_SK_CULL_FACE` | `64` |
| `QGPU_SK_CULL_MODE` | `65` |
| `QGPU_SK_FRONT_FACE` | `66` |
| `QGPU_SK_COLOR_MATERIAL` | `67` |
| `QGPU_SK_COLOR_MAT_FACE` | `68` |
| `QGPU_SK_COLOR_MAT_MODE` | `69` |
| `QGPU_SK_LOCAL_VIEWER` | `70` |
| `QGPU_SK_TWO_SIDE` | `71` |
| `QGPU_SK_COLOR_CONTROL` | `72` |
| `QGPU_SK_FOG_MODE` | `73` |
| `QGPU_SK_FOG_DENSITY` | `74` |
| `QGPU_SK_FOG_START` | `75` |
| `QGPU_SK_FOG_END` | `76` |
| `QGPU_SK_BLEND_COLOR` | `77` |
| `QGPU_SK_LOGIC_OP` | `78` |
| `QGPU_SK_LOGIC_OP_MODE` | `79` |
| `QGPU_SK_POLYGON_MODE_FRONT` | `80` |
| `QGPU_SK_POLYGON_MODE_BACK` | `81` |
| `QGPU_SK_POLY_OFFSET_LINE` | `82` |
| `QGPU_SK_POLY_OFFSET_POINT` | `83` |
| `QGPU_SK_LINE_STIPPLE` | `84` |
| `QGPU_SK_LINE_STIPPLE_FACTOR` | `85` |
| `QGPU_SK_LINE_STIPPLE_PATTERN` | `86` |
| `QGPU_SK_POLYGON_STIPPLE` | `87` |
| `QGPU_SK_TEX_LOD_BIAS0` | `88` |
| `QGPU_SK_COLOR_SUM` | `92` |
| `QGPU_SK_POINT_SIZE_MIN` | `93` |
| `QGPU_SK_POINT_SIZE_MAX` | `94` |
| `QGPU_SK_POINT_FADE` | `95` |
| `QGPU_SK_POINT_ATT_CONST` | `96` |
| `QGPU_SK_POINT_ATT_LINEAR` | `97` |
| `QGPU_SK_POINT_ATT_QUAD` | `98` |
| `QGPU_SK_VERTEX_PROGRAM` | `99` |
| `QGPU_SK_FRAGMENT_PROGRAM` | `100` |
| `QGPU_SK_TEXTURE4` | `101` |
| `QGPU_SK_COMBINE4` | `117` |
| `QGPU_SK_COMBINE_SRC4` | `121` |
| `QGPU_SK_TEX_LOD_BIAS4` | `125` |
| `QGPU_SK_GEN_SIZES` | `129` |
| `QGPU_SK_TEXTURE8` | `130` |
| `QGPU_SK_COUNT` | `162` |
| `QGPU_FOG_VERTEX` | `0` |
| `QGPU_FOG_EXP` | `0x0800` |
| `QGPU_FOG_EXP2` | `0x0801` |
| `QGPU_FOG_LINEAR` | `0x2601` |
| `QGPU_SOP_ZERO` | `0x0000` |
| `QGPU_SOP_INVERT` | `0x150A` |
| `QGPU_SOP_KEEP` | `0x1E00` |
| `QGPU_SOP_REPLACE` | `0x1E01` |
| `QGPU_SOP_INCR` | `0x1E02` |
| `QGPU_SOP_DECR` | `0x1E03` |
| `QGPU_SOP_INCR_WRAP` | `0x8507` |
| `QGPU_SOP_DECR_WRAP` | `0x8508` |
| `QGPU_SK_UNIT(u)` | `((u) == 0 ? QGPU_SK_TEXTURE : (u) == 1 ? QGPU_SK_TEXTURE1 : (u) < 4 ? QGPU_SK_TEXTURE2 + 4 * ((u) - 2) : (u) < 8 ? QGPU_SK_TEXTURE4 + 4 * ((u) - 4) : QGPU_SK_TEXTURE8 + 4 * ((u) - 8))` |
| `QGPU_SK_COMBINE(u)` | `((u) < 4 ? QGPU_SK_COMBINE0 + (u) : QGPU_SK_COMBINE4 + ((u) - 4))` |
| `QGPU_SK_COMBINE_SRC(u)` | `((u) < 4 ? QGPU_SK_COMBINE_SRC0 + (u) : QGPU_SK_COMBINE_SRC4 + ((u) - 4))` |
| `QGPU_SK_TEX_LOD_BIAS(u)` | `((u) < 4 ? QGPU_SK_TEX_LOD_BIAS0 + (u) : QGPU_SK_TEX_LOD_BIAS4 + ((u) - 4))` |
| `QGPU_SK_U_ENABLE` | `0` |
| `QGPU_SK_U_BIND` | `1` |
| `QGPU_SK_U_ENV_MODE` | `2` |
| `QGPU_SK_U_ENV_COLOR` | `3` |
| `QGPU_CB_REPLACE` | `0` |
| `QGPU_CB_MODULATE` | `1` |
| `QGPU_CB_ADD` | `2` |
| `QGPU_CB_ADD_SIGNED` | `3` |
| `QGPU_CB_INTERPOLATE` | `4` |
| `QGPU_CB_SUBTRACT` | `5` |
| `QGPU_CB_DOT3_RGB` | `6` |
| `QGPU_CB_DOT3_RGBA` | `7` |
| `QGPU_CB_MODULATE_ADD` | `8` |
| `QGPU_CB_MODULATE_SIGNED_ADD` | `9` |
| `QGPU_CB_MODULATE_SUBTRACT` | `10` |
| `QGPU_COMBINE_LITERAL_RGB(i, value)` | `((unsigned long)(value) << (12 + 2 * (i)))` |
| `QGPU_COMBINE_LITERAL_A(i, value)` | `((unsigned long)(value) << (18 + 2 * (i)))` |
| `QGPU_CL_ZERO` | `1` |
| `QGPU_CL_ONE` | `2` |
| `QGPU_CS_TEXTURE` | `0` |
| `QGPU_CS_CONSTANT` | `1` |
| `QGPU_CS_PRIMARY` | `2` |
| `QGPU_CS_PREVIOUS` | `3` |
| `QGPU_CS_TEXTURE0` | `4` |
| `QGPU_CO_COLOR` | `0` |
| `QGPU_CO_ONE_MINUS_COLOR` | `1` |
| `QGPU_CO_ALPHA` | `2` |
| `QGPU_CO_ONE_MINUS_ALPHA` | `3` |
| `QGPU_CA_ALPHA` | `0` |
| `QGPU_CA_ONE_MINUS_ALPHA` | `1` |
| `QGPU_COMBINE(rgb, a, rgb_shift, a_shift)` | `((unsigned long)(rgb) \| ((unsigned long)(a) << 4) \| ((unsigned long)(rgb_shift) << 8) \| ((unsigned long)(a_shift) << 10))` |
| `QGPU_COMBINE_SRC_RGB(i, src, op)` | `(((unsigned long)(src) \| ((unsigned long)(op) << 3)) << (5 * (i)))` |
| `QGPU_COMBINE_SRC_A(i, src, op)` | `(((unsigned long)(src) \| ((unsigned long)(op) << 3)) << (15 + 4 * (i)))` |
| `QGPU_COMBINE_DEFAULT` | `QGPU_COMBINE(QGPU_CB_MODULATE, QGPU_CB_MODULATE, 0, 0)` |
| `QGPU_COMBINE_SRC_DEFAULT` | `(QGPU_COMBINE_SRC_RGB(0, QGPU_CS_TEXTURE, QGPU_CO_COLOR) \| QGPU_COMBINE_SRC_RGB(1, QGPU_CS_PREVIOUS, QGPU_CO_COLOR) \| QGPU_COMBINE_SRC_RGB(2, QGPU_CS_CONSTANT, QGPU_CO_ALPHA) \| QGPU_COMBINE_SRC_A(0, QGPU_CS_TEXTURE, QGPU_CA_ALPHA) \| QGPU_COMBINE_SRC_A(1, QGPU_CS_PREVIOUS, QGPU_CA_ALPHA) \| QGPU_COMBINE_SRC_A(2, QGPU_CS_CONSTANT, QGPU_CA_ALPHA))` |
| `QGPU_TP_MIN_FILTER` | `1` |
| `QGPU_TP_MAG_FILTER` | `2` |
| `QGPU_TP_WRAP_S` | `3` |
| `QGPU_TP_WRAP_T` | `4` |
| `QGPU_TP_WRAP_R` | `5` |
| `QGPU_TP_BORDER_COLOR` | `6` |
| `QGPU_TP_MIN_LOD` | `7` |
| `QGPU_TP_MAX_LOD` | `8` |
| `QGPU_TP_BASE_LEVEL` | `9` |
| `QGPU_TP_MAX_LEVEL` | `10` |
| `QGPU_TP_LOD_BIAS` | `11` |
| `QGPU_TP_COMPARE_MODE` | `12` |
| `QGPU_TP_COMPARE_FUNC` | `13` |
| `QGPU_TP_DEPTH_MODE` | `14` |
| `QGPU_TP_GENERATE_MIPMAP` | `15` |
| `QGPU_VERTEX_WORDS` | `8` |
| `QGPU_VERTEX_BYTES` | `(QGPU_VERTEX_WORDS * 4)` |
| `QGPU_VERTEX_TEX_WORDS` | `12` |
| `QGPU_VERTEX_TEX_BYTES` | `(QGPU_VERTEX_TEX_WORDS * 4)` |
| `QGPU_VERTEX_TEX2_WORDS` | `16` |
| `QGPU_VERTEX_TEX2_BYTES` | `(QGPU_VERTEX_TEX2_WORDS * 4)` |
| `QGPU_VERTEX_TEXN_WORDS(n)` | `(8 + 4 * (n))` |
| `QGPU_VERTEX_MAX_WORDS` | `QGPU_VERTEX_TEXN_WORDS(QGPU_MAX_UNITS)` |
| `QGPU_VERTEX_SEC_WORDS(n)` | `(QGPU_VERTEX_TEXN_WORDS(n) + 3)` |
| `QGPU_MTX_MODELVIEW` | `0` |
| `QGPU_MTX_PROJECTION` | `1` |
| `QGPU_MTX_TEXTURE0` | `2` |
| `QGPU_MTX_COUNT` | `(QGPU_MTX_TEXTURE0 + QGPU_MAX_UNITS)` |
| `QGPU_TG_S` | `0` |
| `QGPU_TG_T` | `1` |
| `QGPU_TG_R` | `2` |
| `QGPU_TG_Q` | `3` |
| `QGPU_TG_OBJECT_LINEAR` | `0x2401` |
| `QGPU_TG_EYE_LINEAR` | `0x2400` |
| `QGPU_TG_SPHERE_MAP` | `0x2402` |
| `QGPU_TG_NORMAL_MAP` | `0x8511` |
| `QGPU_TG_REFLECTION_MAP` | `0x8512` |
| `QGPU_CUR_NORMAL` | `0` |
| `QGPU_CUR_COLOR` | `1` |
| `QGPU_CUR_SEC_COLOR` | `2` |
| `QGPU_CUR_FOG` | `3` |
| `QGPU_CUR_TEXCOORD0` | `4` |
| `QGPU_CUR_COUNT` | `(QGPU_CUR_TEXCOORD0 + QGPU_MAX_UNITS)` |
| `QGPU_PRIM_MODE_POINTS` | `0x0000` |
| `QGPU_PRIM_MODE_LINES` | `0x0001` |
| `QGPU_PRIM_MODE_LINE_LOOP` | `0x0002` |
| `QGPU_PRIM_MODE_LINE_STRIP` | `0x0003` |
| `QGPU_PRIM_MODE_TRIANGLES` | `0x0004` |
| `QGPU_PRIM_MODE_TRIANGLE_STRIP` | `0x0005` |
| `QGPU_PRIM_MODE_TRIANGLE_FAN` | `0x0006` |
| `QGPU_PRIM_MODE_QUADS` | `0x0007` |
| `QGPU_PRIM_MODE_QUAD_STRIP` | `0x0008` |
| `QGPU_PRIM_MODE_POLYGON` | `0x0009` |
| `QGPU_IDX_NONE` | `0` |
| `QGPU_IDX_U16` | `1` |
| `QGPU_IDX_U32` | `2` |
| `QGPU_VF_POS(n)` | `((unsigned long)((n) - 2))` |
| `QGPU_VF_POS_MASK` | `0x0003` |
| `QGPU_VF_POS_COUNT(m)` | `((int)((m) & QGPU_VF_POS_MASK) + 2)` |
| `QGPU_VF_NORMAL` | `0x0004` |
| `QGPU_VF_COLOR` | `0x0008` |
| `QGPU_VF_SEC_COLOR` | `0x0010` |
| `QGPU_VF_FOG` | `0x0020` |
| `QGPU_VF_TEX0` | `0x0040` |
| `QGPU_VF_TEX4` | `0x4000000` |
| `QGPU_VF_TEX(u)` | `((u) < 4 ? (QGPU_VF_TEX0 << (u)) : (QGPU_VF_TEX4 << ((u) - 4)))` |
| `QGPU_VF_TEX_MASK` | `0x3C0003C0` |
| `QGPU_VF_GEN0` | `0x0400` |
| `QGPU_VF_GEN(k)` | `(QGPU_VF_GEN0 << (k))` |
| `QGPU_VF_GEN_MAX` | `16` |
| `QGPU_VF_GEN_MASK` | `0x3FFFC00` |
| `QGPU_VF_ALL` | `0x3FFFFFFF` |
| `QGPU_VF_GEN_WORDS(m)` | `((((m) & QGPU_VF_GEN(0)) ? 4 : 0) + (((m) & QGPU_VF_GEN(1)) ? 4 : 0) + (((m) & QGPU_VF_GEN(2)) ? 4 : 0) + (((m) & QGPU_VF_GEN(3)) ? 4 : 0) + (((m) & QGPU_VF_GEN(4)) ? 4 : 0) + (((m) & QGPU_VF_GEN(5)) ? 4 : 0) + (((m) & QGPU_VF_GEN(6)) ? 4 : 0) + (((m) & QGPU_VF_GEN(7)) ? 4 : 0) + (((m) & QGPU_VF_GEN(8)) ? 4 : 0) + (((m) & QGPU_VF_GEN(9)) ? 4 : 0) + (((m) & QGPU_VF_GEN(10)) ? 4 : 0) + (((m) & QGPU_VF_GEN(11)) ? 4 : 0) + (((m) & QGPU_VF_GEN(12)) ? 4 : 0) + (((m) & QGPU_VF_GEN(13)) ? 4 : 0) + (((m) & QGPU_VF_GEN(14)) ? 4 : 0) + (((m) & QGPU_VF_GEN(15)) ? 4 : 0))` |
| `QGPU_VF_WORDS(m)` | `(QGPU_VF_POS_COUNT(m) + (((m) & QGPU_VF_NORMAL) ? 3 : 0) + (((m) & QGPU_VF_COLOR) ? 4 : 0) + (((m) & QGPU_VF_SEC_COLOR) ? 3 : 0) + (((m) & QGPU_VF_FOG) ? 1 : 0) + (((m) & QGPU_VF_TEX(0)) ? 4 : 0) + (((m) & QGPU_VF_TEX(1)) ? 4 : 0) + (((m) & QGPU_VF_TEX(2)) ? 4 : 0) + (((m) & QGPU_VF_TEX(3)) ? 4 : 0) + (((m) & QGPU_VF_TEX(4)) ? 4 : 0) + (((m) & QGPU_VF_TEX(5)) ? 4 : 0) + (((m) & QGPU_VF_TEX(6)) ? 4 : 0) + (((m) & QGPU_VF_TEX(7)) ? 4 : 0) + QGPU_VF_GEN_WORDS(m))` |
| `QGPU_GS_CODE(gs, k)` | `((int)(((gs) >> (2 * (k))) & 3))` |
| `QGPU_GS_COUNT(gs, k)` | `(QGPU_GS_CODE(gs, k) ? QGPU_GS_CODE(gs, k) : 4)` |
| `QGPU_GS(k, n)` | `((unsigned long)((n) & 3) << (2 * (k)))` |
| `QGPU_GS_FIELD(k)` | `((unsigned long)3 << (2 * (k)))` |
| `QGPU_GS_SAVED_K(m, gs, k)` | `(((m) & QGPU_VF_GEN(k)) ? ((4 - QGPU_GS_CODE(gs, k)) & 3) : 0)` |
| `QGPU_VF_GEN_SAVED(m, gs)` | `(QGPU_GS_SAVED_K(m, gs, 0) + QGPU_GS_SAVED_K(m, gs, 1) + QGPU_GS_SAVED_K(m, gs, 2) + QGPU_GS_SAVED_K(m, gs, 3) + QGPU_GS_SAVED_K(m, gs, 4) + QGPU_GS_SAVED_K(m, gs, 5) + QGPU_GS_SAVED_K(m, gs, 6) + QGPU_GS_SAVED_K(m, gs, 7) + QGPU_GS_SAVED_K(m, gs, 8) + QGPU_GS_SAVED_K(m, gs, 9) + QGPU_GS_SAVED_K(m, gs, 10) + QGPU_GS_SAVED_K(m, gs, 11) + QGPU_GS_SAVED_K(m, gs, 12) + QGPU_GS_SAVED_K(m, gs, 13) + QGPU_GS_SAVED_K(m, gs, 14) + QGPU_GS_SAVED_K(m, gs, 15))` |
| `QGPU_VF_WORDS_GS(m, gs)` | `(QGPU_VF_WORDS(m) - QGPU_VF_GEN_SAVED(m, gs))` |
| `QGPU_VF_MAX_WORDS` | `111` |
| `QGPU_BF_CONSTANT_COLOR` | `0x8001` |
| `QGPU_BF_ONE_MINUS_CONSTANT_COLOR` | `0x8002` |
| `QGPU_BF_CONSTANT_ALPHA` | `0x8003` |
| `QGPU_BF_ONE_MINUS_CONSTANT_ALPHA` | `0x8004` |
| `QGPU_BEQ_ADD` | `0x8006` |
| `QGPU_BEQ_MIN` | `0x8007` |
| `QGPU_BEQ_MAX` | `0x8008` |
| `QGPU_BEQ_SUBTRACT` | `0x800A` |
| `QGPU_BEQ_REVERSE_SUBTRACT` | `0x800B` |
| `QGPU_LO_CLEAR` | `0x1500` |
| `QGPU_LO_AND` | `0x1501` |
| `QGPU_LO_AND_REVERSE` | `0x1502` |
| `QGPU_LO_COPY` | `0x1503` |
| `QGPU_LO_AND_INVERTED` | `0x1504` |
| `QGPU_LO_NOOP` | `0x1505` |
| `QGPU_LO_XOR` | `0x1506` |
| `QGPU_LO_OR` | `0x1507` |
| `QGPU_LO_NOR` | `0x1508` |
| `QGPU_LO_EQUIV` | `0x1509` |
| `QGPU_LO_INVERT` | `0x150A` |
| `QGPU_LO_OR_REVERSE` | `0x150B` |
| `QGPU_LO_COPY_INVERTED` | `0x150C` |
| `QGPU_LO_OR_INVERTED` | `0x150D` |
| `QGPU_LO_NAND` | `0x150E` |
| `QGPU_LO_SET` | `0x150F` |
| `QGPU_POLY_POINT` | `0x1B00` |
| `QGPU_POLY_LINE` | `0x1B01` |
| `QGPU_POLY_FILL` | `0x1B02` |
| `QGPU_TT_1D` | `0x0DE0` |
| `QGPU_TT_2D` | `0x0DE1` |
| `QGPU_TT_3D` | `0x806F` |
| `QGPU_TT_CUBE_MAP` | `0x8513` |
| `QGPU_TT_RECTANGLE` | `0x84F5` |
| `QGPU_TT_CUBE_FACE(f)` | `(0x8515 + (f))` |
| `QGPU_TEX_NO_DATA` | `0xFFFFFFFFUL` |
| `QGPU_TF_DXT1_RGB` | `0x83F0` |
| `QGPU_TF_DXT1_RGBA` | `0x83F1` |
| `QGPU_TF_DXT3` | `0x83F2` |
| `QGPU_TF_DXT5` | `0x83F3` |
| `QGPU_TW_MIRRORED_REPEAT` | `0x8370` |
| `QGPU_TW_CLAMP_TO_BORDER` | `0x812D` |
| `QGPU_TC_NONE` | `0x0000` |
| `QGPU_TC_COMPARE_R` | `0x884E` |
| `QGPU_CSUM_OFF` | `0` |
| `QGPU_CSUM_ON` | `1` |
| `QGPU_CSUM_FORMAT` | `2` |
| `QGPU_PT_VERTEX` | `0x8620` |
| `QGPU_PT_FRAGMENT` | `0x8804` |
| `QGPU_PT_GLSL` | `0x8B40` |
| `QGPU_GLSL_FRAGMENT` | `0x8B30` |
| `QGPU_GLSL_VERTEX` | `0x8B31` |
| `QGPU_GT_FLOAT` | `0x1406` |
| `QGPU_GT_FLOAT_VEC2` | `0x8B50` |
| `QGPU_GT_FLOAT_VEC3` | `0x8B51` |
| `QGPU_GT_FLOAT_VEC4` | `0x8B52` |
| `QGPU_GT_INT` | `0x1404` |
| `QGPU_GT_INT_VEC2` | `0x8B53` |
| `QGPU_GT_INT_VEC3` | `0x8B54` |
| `QGPU_GT_INT_VEC4` | `0x8B55` |
| `QGPU_GT_BOOL` | `0x8B56` |
| `QGPU_GT_BOOL_VEC2` | `0x8B57` |
| `QGPU_GT_BOOL_VEC3` | `0x8B58` |
| `QGPU_GT_BOOL_VEC4` | `0x8B59` |
| `QGPU_GT_FLOAT_MAT2` | `0x8B5A` |
| `QGPU_GT_FLOAT_MAT3` | `0x8B5B` |
| `QGPU_GT_FLOAT_MAT4` | `0x8B5C` |
| `QGPU_GT_SAMPLER_1D` | `0x8B5D` |
| `QGPU_GT_SAMPLER_2D` | `0x8B5E` |
| `QGPU_GT_SAMPLER_3D` | `0x8B5F` |
| `QGPU_GT_SAMPLER_CUBE` | `0x8B60` |
| `QGPU_GT_SAMPLER_1D_SHADOW` | `0x8B61` |
| `QGPU_GT_SAMPLER_2D_SHADOW` | `0x8B62` |
| `QGPU_GT_SAMPLER_2D_RECT` | `0x8B63` |
| `QGPU_GT_SAMPLER_2D_RECT_SHADOW` | `0x8B64` |
| `QGPU_GT_IS_SAMPLER(t)` | `((t) >= QGPU_GT_SAMPLER_1D && (t) <= QGPU_GT_SAMPLER_2D_RECT_SHADOW)` |
| `QGPU_GT_IS_MAT(t)` | `((t) >= QGPU_GT_FLOAT_MAT2 && (t) <= QGPU_GT_FLOAT_MAT4)` |
| `QGPU_GT_SLOTS(t)` | `((t) == QGPU_GT_FLOAT_MAT2 ? 2 : (t) == QGPU_GT_FLOAT_MAT3 ? 3 : (t) == QGPU_GT_FLOAT_MAT4 ? 4 : 1)` |
| `QGPU_NATIVE_MAX_ATTRS` | `24` |
| `QGPU_NATIVE_DESC_WORDS` | `6` |
| `QGPU_NA_POSITION` | `0` |
| `QGPU_NA_NORMAL` | `1` |
| `QGPU_NA_COLOR` | `2` |
| `QGPU_NA_SEC_COLOR` | `3` |
| `QGPU_NA_FOG` | `4` |
| `QGPU_NA_TEX(u)` | `(8 + (u))` |
| `QGPU_NA_GEN(k)` | `(16 + (k))` |
| `QGPU_NA_CODE_MAX` | `31` |
| `QGPU_NA_SIZE_MASK` | `0xFF` |
| `QGPU_NA_NORMALIZED` | `0x100` |
| `QGPU_NT_BYTE` | `0x1400` |
| `QGPU_NT_UBYTE` | `0x1401` |
| `QGPU_NT_SHORT` | `0x1402` |
| `QGPU_NT_USHORT` | `0x1403` |
| `QGPU_NT_INT` | `0x1404` |
| `QGPU_NT_UINT` | `0x1405` |
| `QGPU_NT_FLOAT` | `0x1406` |
| `QGPU_NT_DOUBLE` | `0x140A` |
| `QGPU_CLASS_CTX` | `0` |
| `QGPU_CLASS_SURF` | `1` |
| `QGPU_CLASS_TEX` | `2` |
| `QGPU_CLASS_QUERY` | `3` |
| `QGPU_CLASS_BUF` | `4` |
| `QGPU_CLASS_COUNT` | `5` |
| `QGPU_MAX_CLIENTS` | `4` |
| `QGPU_CLIENT_CTX_IDS` | `(QGPU_MAX_CTX / QGPU_MAX_CLIENTS)` |
| `QGPU_CLIENT_SURF_IDS` | `(QGPU_MAX_SURF / QGPU_MAX_CLIENTS)` |
| `QGPU_CLIENT_TEX_IDS` | `(QGPU_MAX_TEX / QGPU_MAX_CLIENTS)` |
| `QGPU_CLIENT_QUERY_IDS` | `(QGPU_MAX_QUERIES / QGPU_MAX_CLIENTS)` |
| `QGPU_CLIENT_BUF_IDS` | `(QGPU_MAX_BUF / QGPU_MAX_CLIENTS)` |
