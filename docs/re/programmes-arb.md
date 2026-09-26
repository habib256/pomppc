# Programmes ARB dans GLEngine 10.4.6 — relevé pour le protocole v16 (23/09/2026)

Ce que le plugin doit lire dans GLEngine pour porter `GL_ARB_vertex_program` /
`GL_ARB_fragment_program` sur l'hôte (docs/protocole-v16-programmes.md). Listing :
`bench/devloop/jobs/1789676199209/out/gle-dis.txt` (GLEngine) et
`…/1789676612114/out/GeForce3GLDriver.txt`. Tout ce qui suit est **[L]** (lu au
listing) sauf mention ; les objets « pipeline program » eux-mêmes sont relevés
dans `tableaux-de-sommets.md` §4 (texte `ppobj+0x14`, longueur `+0x18`, cible
`+0x4c8`, paramètres locaux `*(ppobj+0x4e0)`, 16 octets chacun, 256 sommets /
128 fragments).

## 1. Le texte ne va JAMAIS au pilote

`_glProgramStringARB_Exec 0x22aa4` : copie le texte (`malloc`, `0x22b64`), le
range dans l'objet (`0x22c0c`), pose `+0x4c = 1` (à recompiler), puis pour la
cible sommets appelle `_glePPParse(ctx, ppobj+0x10, 0)`, `_glePPUpdateProgram`,
`_gleUpdateInverseNeeds`, `_gleSelectVertexSubmitFunc` et met le bit
`0x00400000` dans `gctx+0x31c` ; pour la cible fragments, `_glePPParse(…, 1)`
puis `_gleSelectVertexSubmitFunc`, bit `0x01000000`.

Le seul appel à `_gleModifyPluginPipelineProgram(ctx, ppobj+0x10, 1)`
(`0x22be4`) précède la libération de **l'ancien** texte : le masque 1 veut dire
« le texte que tu connaissais est mort », pas « voici le nouveau ». Le masque 2
vient de `_glProgramLocalParameter4*ARB_Exec` (`0x26224`). Un pilote qui veut
le texte le **lit dans l'objet**, au moment où il en a besoin (le dessin).

`_glePPParse` (`0x22dc0`) : le troisième argument (`r18`) dit s'il faut
construire le programme de l'émulateur logiciel (`_PPEmulatorProgramCreate`,
`0x2402c`, quand `r18 == 0`) — donc **toujours** pour un programme de sommets,
jamais pour un programme de fragments (la forme compilée des fragments est un
`LFSStream`, `_gleLFSParse`). Aucune limite du bloc de configuration n'entre
dans cette décision : l'émulateur existe, et c'est la fonction de soumission
(`_gleSelectVertexSubmitFunc`) qui choisit, à partir du verrou T&L, qui
transforme. Sous notre verrou (bit 0 de `gldUpdateDispatch`), GLEngine déroule
les tableaux dans notre tampon selon notre descripteur — d'où la « géométrie
éclatée » de Colin McRae tant que le descripteur ne demandait pas les
emplacements génériques (`descripteur-de-sommet.md` §4, codes 16..31).

## 2. Activation, objets courants, program.env

| Où | Quoi | Preuve |
|---|---|---|
| `gctx+0x5420 + 4·t` | objet **courant** de la cible `t` (0 sommets, 1 fragments), `glBindProgramARB` | `_glGetProgramivARB_Exec 0x2119c`, `_glProgramStringARB_Exec 0x22bc8` |
| `gctx+0x5428 + 4·t` | objet par défaut (nom 0) | `tableaux-de-sommets.md` §4.3 |
| `gctx+0x5430` | objet **shader GLSL** actif, 0 sinon | `_glProgramEnvParameter4fvARB_Exec 0x4f350` |
| `gctx+0x5434` / `+0x5438` | u32 : sommets / fragments **GLSL** actifs — posés par `_updateShaderState 0x865a8`, appelée seulement par `glUseProgramObjectARB`, `glLinkProgramARB`, `_setImageUnit`. **Pas** l'activation ARB : nuls sous `gltest arbvp` (vu en vrai, 23/09/2026, première version du plugin) | `0x8660c`, `0x86638` |
| **`gctx+0x4664`** | u8 : **`glEnable(GL_VERTEX_PROGRAM_ARB)`** | `_gleGetEnabled 0x22310` (cas `0x8620`) |
| **`gctx+0x466c`** | u8 : **`glEnable(GL_FRAGMENT_PROGRAM_ARB)`** | `_gleGetEnabled 0x22328` (cas `0x8804`) |
| `gctx+0x4665` / `+0x4666` | u8 : `GL_VERTEX_PROGRAM_POINT_SIZE_ARB` / `_TWO_SIDE_ARB` | `0x22318`, `0x22320` |
| **`*(gctx+0x4668)`** | table des **`program.env` des sommets**, 16 octets par entrée, indices 0..255 | `_glProgramEnvParameter4fvARB_Exec 0x4f3c4` (`lwz r2, 0x4668(ctx + type·8)` ; borne `0x4f2e4` : `cmplwi r5, 0xff`) |
| **`*(gctx+0x4670)`** | idem **fragments**, indices 0..127 | `0x4f2fc` : `cmplwi r5, 0x7f` |
| `gctx+0x31c` | bits « à revalider » : `0x00800000` env sommets, `0x02000000` env fragments, `0x00400000` / `0x01000000` texte | `0x4f2f4`, `0x4f320`, `0x22cbc`, `0x22d78` |

`_gleVPEnable 0x25fc0` : `_gleVPChanged`, puis relit `gctx+0x30aa` (éclairage)
pour poser `gctx+0x316b` (color material) et rejoue `_gleSetColorMaterialEnable`,
`_gleUpdateInverseNeeds`, `_gleUpdatePrimitiveData`, `_gleUpdateTextureTransform`,
`_gleUpdatePolyMode`, `_gleSelectVertexSubmitFunc`. `_gleFPEnable 0xd22e0` :
`_gleFPChanged` puis `_gleSelectVertexSubmitFunc`.

Le plugin lit donc, au dispatch et au lot : `gctx+0x5434` / `+0x5438` (actif),
`gctx+0x5420` / `+0x5424` (quel objet), le texte et les locaux dans l'objet, les
env dans les deux tables — et refuse le domaine si `gctx+0x5430` (GLSL) est
non nul. `gctx = GS − 0x360` (constante `GS_LOW` du plugin).

## 3. Les limites de programmes du bloc de configuration (`cfg+0xec..+0x11b`)

Seul lecteur : `_glGetProgramivARB_Exec 0x2110c` (`r2 = cfg + 16·t`, `t` = 0
sommets / 1 fragments : **un bloc de 16 octets par cible**, sommets à `+0xec`,
fragments à `+0xfc`). Champs (u16) et requête qui les rend :

| Offset dans le bloc | Requête | Vérifié contre la réponse du pilote (`gldGetPipelineProgramInfo`, pname) |
|---|---|---|
| `+0` | `GL_MAX_PROGRAM_INSTRUCTIONS_ARB` | `0x88a2` (`0x214f4`) |
| `+2` | `GL_MAX_PROGRAM_ATTRIBS_ARB` | `0x88ae` (`0x215c0`) |
| `+4` | `GL_MAX_PROGRAM_PARAMETERS_ARB` | `0x88aa` (`0x21538`) |
| `+6` | `GL_MAX_PROGRAM_TEMPORARIES_ARB` | `0x88a6` (`0x2157c`) |
| `+8` (u32), `+0xc` (u16) | requêtes `0x88f4` / `0x88f5` (NV, profondeur d'appel / de branchement) | `0x21890`, `0x218a4` |

Puis, hors bloc : `cfg+0x10c` / `+0x10e` / `+0x110` (fragments : instructions
ALU, TEX, indirections, comparées à `0x8808..` en `0x21658..0x216d8`),
`cfg+0x112` / `+0x114` / `+0x116` (requêtes `0x88f6..0x88f8`), `cfg+0x118`
(sommets : `GL_MAX_PROGRAM_ADDRESS_REGISTERS_ARB`, `0x88b2`, `0x21608`).
`GL_MAX_PROGRAM_ENV_PARAMETERS_ARB` / `_LOCAL_` (`0x88b5`, `0x88b6`) ne
viennent pas du bloc : constantes 256 (sommets) / 128 (fragments), `0x2148c`.

Chez le rendu d'Apple ces champs sont **nuls** (`capacites-glengine.md` §4) :
`glGetProgramivARB(GL_MAX_…)` répond 0 et `GL_PROGRAM_UNDER_NATIVE_LIMITS_ARB`
0. Ils n'entrent dans aucune décision de GLEngine ; le plugin les remplit
(`caps_extensions`) pour qu'une application qui les interroge ne s'en détourne
pas. `GL_ARB_fragment_program` est le bit 15 de `cfg+0x124`
(`version-extensions.md` §4) ; le rendu d'Apple ne l'annonce pas et **ignore
en silence** `glEnable(GL_FRAGMENT_PROGRAM_ARB)` (scène `gltest arbfp` avec
`POMPPC_GL_DISABLE=1` : couleur du pipeline fixe, aucune erreur GL). Colin
McRae en fait pourtant huit : sous Apple ils ne jouaient pas.

## 3 bis. Les pointeurs résolus périmés (`gctx+0x48f8`, la géométrie éclatée)

Sonde `VA_PTRS` du plugin (nuit du 23/09/2026, `close_raw`), même lot dans
`gltest arbvp0cmr` (juste) et dans Colin McRae en course (sommets aux valeurs
courantes) :

```
gltest : VA_PTRS [0]=00000000 [2]=00000000 [16]=0002fc1c [17]=0002fc28 [18]=0002fc34 [19]=0002fc38
CMR    : VA_PTRS [0]=0b64b000 [2]=0b64b010 [16]=0e1a7c00 [17]=0e1a7c0c [18]=0e1a7c18 [19]=0e1a7c1c
         (slot 0 « en 0 ptr 0b64b000 », slot 2 « en 0 ptr 0b64b010 » : les tableaux du HUD, DÉSACTIVÉS)
```

`gctx+0x48f8 + 4·code` est la table des pointeurs **résolus** de GLEngine. Elle
n'est mise à jour que pour les tableaux actifs : un tableau conventionnel
désactivé y garde son dernier pointeur. Le déroulage T&L d'un descripteur qui
demande le code 0 (position) ou 2 (couleur) lit **à cette adresse périmée**
au lieu de basculer sur l'emplacement 16 ou de prendre la valeur courante —
la bascule « code 0 inactif → 16 » de `_gleSetFunctionIDFromArray` ne vaut
que quand le pointeur résolu est nul (jamais posé), ce qui est le cas de
`gltest` et jamais celui d'un jeu qui a dessiné son HUD en tableaux
conventionnels. D'où, en v16, un descripteur qui sous programme demande
**le code 16 pour la position** quand le générique 0 est actif, et **aucun
attribut conventionnel dont le tableau n'est pas actif** (`geom_format`,
`geom_publish`). Le journal `gltrap` fenêtré (`POMPPC_GLTRAP_WINDOW=big500:40`)
ne montre rien d'autre autour d'un lot de course : `glDisable(FP)`,
`glDisable(VP)`, locaux, `glVertexAttribPointerARB` 0/1/2 (nouvelles adresses à
chaque lot), `glEnable(VP)`, `glDrawElements(TRIANGLE_STRIP, n, UNSIGNED_SHORT)`.

## 3 ter. Colin McRae en course : les tableaux sont libérés avant le déroulage

> **Résolu le 26/09/2026 — l'hypothèse ci-dessous était fausse** (`cmr-var.md`). Les tableaux
> sont déjà vides **avant** le `glDrawElements` du jeu (gltrap `POMPPC_GLTRAP_MEM`) : IndirectX
> ne remplit la copie privée de ses tampons de sommets, seule lue sous programme de sommets, que
> si le rendu annonce `GL_APPLE_vertex_array_range`. Le plugin `20260926-var` l'annonce (bit 47).
> Le texte d'origine est gardé pour l'historique.

Sondes `BEGIN gen0` (dans `geom_begin`, au début du déroulage de GLEngine) et
`précédent relu` (le même tampon, au lot suivant), nuit du 23/09 : la mémoire
que désigne l'emplacement générique 0 est **déjà nulle** (`0xc601000`,
`0xc617000`… blocs de 70–90 Ko, alignés sur la page) ou contient des pointeurs
de liste libre (`0x0341db7b 00000000 0x0341ee00`) quand notre
`BeginPrimitiveBuffer` est appelé — et le reste ensuite. IndirectX construit
un tableau temporaire par `DrawIndexedPrimitiveVB`, le libère, et GLEngine
sur notre chemin T&L lit après cette libération ; sur le chemin d'Apple
(émulation logicielle du programme) la lecture précède. Tout ce que la trace
`gltrap` montre autour d'un lot (pointeurs neufs à chaque dessin, programme
coupé/rallumé, deux VAO, lot hors domaine intercalé, tampon `malloc` libéré
et réutilisé après le dessin, trois dessins consécutifs, indices en VBO, vrai
texte `PreLit_fog`) a été rejoué dans `gltest arbvp0cmr` sans reproduire le
défaut : là, GLEngine lit avant `free`. Reste à établir **qui** libère (ou
réutilise) entre l'appel du jeu et notre `Begin` — piste : le plugin lui-même
alloue (textures, notes) pendant le dispatch et recycle le bloc que le jeu
vient de rendre.

## 4. Ce que Colin McRae demande (journal `gltrap`, `.run/cmr/gltrap.txt`)

19 `glProgramStringARB` (11 `!!ARBvp1.0`, 8 `!!ARBfp1.0`), `glBindProgramARB`
à chaque dessin (sommets et fragments), `glEnable(GL_VERTEX_PROGRAM_ARB)` /
`glDisable` des deux cibles, `glVertexAttribPointerARB` 0/1/2 avec
`glEnableVertexAttribArrayARB`, `glDrawElements(GL_TRIANGLE_STRIP, 4,
GL_UNSIGNED_SHORT, …)` par petits lots, `glProgramEnvParameter4fvARB`
(sommets, indices 0..95 — dont des entrées **NaN / inf** : `env[6] = 0 nan inf
255`, `env[56] = … nan 1` ; le cœur les refuse, le plugin les assainit comme
`put_f`), `glProgramLocalParameter4fvARB` (sommets 0, fragments 34/35). Jamais
`glEnable(GL_TEXTURE_2D)` : sous programme de fragments, la texture de l'unité
est celle que le **texte** échantillonne (`texture[u], 2D`), d'où `unit_mask`.
