# docs/re — relevés de rétro-ingénierie d'OpenGL.framework (Tiger 10.4.6)

Un fichier par sujet. Chaque relevé donne : le binaire, l'adresse, ce qui a été lu, et **comment
le vérifier** (sonde `guest/gltest`, trace `POMPPC_GLTRACE`, diff de vidages). Rien de deviné.

Les désassemblages annotés sont produits par `tools/re/ppcanno.py` à partir des binaires du
système installé ; ils ne sont pas dans le dépôt (code d'Apple). Une copie de travail est gardée
hors dépôt : l'ancien `~/src/pomppc-re` (binaires et pilotes GL de 10.4.6, en-têtes du SDK,
désassemblages `asm/*.s`), archivé le 22/09 sur le disque USB TEST dans `pomppc-re.tar.zst` ;
le restaurer depuis `~/src` par `zstd -dc /media/gistarcade/TEST/pomppc-re.tar.zst | tar -xf -`.

| Fichier | Sujet |
|---|---|
| `capacites-glengine.md` | d'où viennent `GL_VERSION` et `GL_EXTENSIONS`, le bloc de configuration de `gldCreateContext`, le verrou de la T&L. ⚠ sa table bit → extension (§3.2) est corrigée par `version-extensions.md` §4 |
| `verification-tcl.md` | le même, vérifié dans l'invité : le vrai verrou est le bit 0 du retour de `gldInitDispatch` |
| `tableaux-de-sommets.md` | relevé par lecture des tableaux de sommets et de l'état T&L |
| `descripteur-de-sommet.md` | le descripteur de sortie de sommet publié en `cfg+0x11c` |
| `programmes-arb.md` | programmes ARB (v16) : le texte reste dans l'objet, `program.env` en `gctx+0x4668/+0x4670`, activation `gctx+0x5434/+0x5438`, limites `cfg+0xec..` (un bloc de 16 octets par cible), ce que Colin McRae demande |
| `cmr-var.md` | Colin McRae : la géométrie éclatée venait de `GL_APPLE_vertex_array_range` absent (les deux copies des tampons de sommets d'IndirectX), et des 4 contextes par client de la v19 ; ce qui reste (rendu vers texture rectangle) |
| `etat-tcl.md` | l'état T&L de GLEngine, offset par offset, établi par sondes |
| `stencil.md` | l'état de stencil, et où le rendu d'Apple range le tampon |
| `etat-v8.md` | mélange constant, opération logique, pointillés, modes de polygone ; et comment les **requêtes d'occlusion** atteignent le pilote |
| `accelerateur-iokit.md` | comment CGL et GLEngine trouvent le pilote d'une carte (`IOAccelTypes` sur le framebuffer, `IOGLBundleName` sur l'accélérateur), et quand le WindowServer tente Quartz Extreme — tâche 4.2 |
| `ut2004-demo.md` | premier passage UT2004 Demo (20/09/2026) : portes GPU levées, lenteur restante, **contrôle de la VM quotidienne** (AZERTY, QMP, CD) |
| `moteur-multifil.md` | `kCGLCEMPEngine` (313) absent de Tiger 10.4.6 : GLEngine ne crée aucun fil, `CGLEnable(313)` → `kCGLBadEnumeration` (01/10) |
| `version-extensions.md` | ce que la chaîne **tient** vraiment, fonction par fonction — d'où la version et les extensions annoncées ; table bit → extension corrigée par l'expérience |
| `pixelstore.md` | l'état de `glPixelStorei` (`GC_PACK_*`, `GC_UNPACK_*`) dans le contexte de GLEngine, et le défaut de base (contexte du GLDriver) qui faisait planter `try_draw_pixels` (07/10) ; GLEngine identique en 10.4.6 et 10.4.11 |
| `glengine-exit-interpolateur.md` | `exit(1)` dans `_gleBuildInterpolateFunc` : pourquoi un repli vers le rendu d'Apple tue un jeu sous programme ARB (22/09) |
| `glengine-tampon-coupe.md` | GLEngine coupe ses lots `GL_TRIANGLES` à cheval sur deux `BeginPrimitiveBuffer` (22-23/09) |
| `bloc-changements-r4.md` | le bloc de changements de GLEngine, bit par bit (relevé R4, 24/09) |
| `etude-court-circuit-glengine.md` | court-circuiter GLEngine pour les dessins VBO ? étude du 24/09 |
| `relecture-2026-09-24.md` | relecture adversariale des commits du 24/09 |
| `verdict-lots-4-5.md` | verdict unique : mémoire des unités et des textures, lots 4 et 5 (26/09) |
| `dispatch-paresseux.md` | transmission paresseuse des `gldUpdateDispatch` au GLDriver d'Apple |
| `textures-3d.md`, `cartes-de-cube.md`, `bordure-et-compression.md`, `opengl-1.4.md` | ce que GLEngine donne au pilote : textures 3D et LOD, cartes de cube, bordure/miroir/S3TC, OpenGL 1.4 |
| `glsl-glengine.md` | GLSL dans GLEngine 10.4.6, relevé pour le protocole v21 (27/09) |
| `cmr-rendu-vers-texture.md` | Colin McRae : le rendu vers texture d'IndirectX tenu par l'hôte (27/09) ; §7 : les reflets de la carrosserie (31/10) |
| `ut2004-flyby.md`, `ut2004-fullscreen.md`, `ut2004-m4-profile.md`, `ut2004-texture-cache.md`, `ut2004-arme-noire.md` | UT2004 : banc caméra AS-Convoy, plein écran, profil M4 du 20/09, réutilisation des identifiants de texture, arme noire (texture diffuse perdue) |
| `apple-headers/` | en-têtes publics d'Apple `gliContext.h` et `gliDispatch.h` (interface GLI du pilote) |
