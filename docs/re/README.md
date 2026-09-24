# docs/re — relevés de rétro-ingénierie d'OpenGL.framework (Tiger 10.4.6)

Un fichier par sujet. Chaque relevé donne : le binaire, l'adresse, ce qui a été lu, et **comment
le vérifier** (sonde `guest/gltest`, trace `POMPPC_GLTRACE`, diff de vidages). Rien de deviné.

Les désassemblages annotés sont produits par `tools/re/ppcanno.py` à partir des binaires du
système installé ; ils ne sont pas dans le dépôt (code d'Apple). Une copie de travail est gardée
hors dépôt (disque USB : `pomppc/re/`).

| Fichier | Sujet |
|---|---|
| `capacites-glengine.md` | d'où viennent `GL_VERSION` et `GL_EXTENSIONS`, le bloc de configuration de `gldCreateContext`, le verrou de la T&L. ⚠ sa table bit → extension (§3.2) est corrigée par `version-extensions.md` §4 |
| `verification-tcl.md` | le même, vérifié dans l'invité : le vrai verrou est le bit 0 du retour de `gldInitDispatch` |
| `tableaux-de-sommets.md` | relevé par lecture des tableaux de sommets et de l'état T&L |
| `descripteur-de-sommet.md` | le descripteur de sortie de sommet publié en `cfg+0x11c` |
| `programmes-arb.md` | programmes ARB (v16) : le texte reste dans l'objet, `program.env` en `gctx+0x4668/+0x4670`, activation `gctx+0x5434/+0x5438`, limites `cfg+0xec..` (un bloc de 16 octets par cible), ce que Colin McRae demande |
| `etat-tcl.md` | l'état T&L de GLEngine, offset par offset, établi par sondes |
| `stencil.md` | l'état de stencil, et où le rendu d'Apple range le tampon |
| `etat-v8.md` | mélange constant, opération logique, pointillés, modes de polygone ; et comment les **requêtes d'occlusion** atteignent le pilote |
| `accelerateur-iokit.md` | comment CGL et GLEngine trouvent le pilote d'une carte (`IOAccelTypes` sur le framebuffer, `IOGLBundleName` sur l'accélérateur), et quand le WindowServer tente Quartz Extreme — tâche 4.2 |
| `ut2004-demo.md` | premier passage UT2004 Demo (20/09/2026) : portes GPU levées, lenteur restante, **contrôle de la VM quotidienne** (AZERTY, QMP, CD) |
| `version-extensions.md` | ce que la chaîne **tient** vraiment, fonction par fonction — d'où la version et les extensions annoncées ; table bit → extension corrigée par l'expérience |
