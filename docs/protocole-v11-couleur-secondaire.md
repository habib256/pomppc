# Protocole qgpu v11 — la couleur secondaire sur le chemin hérité

Un seul ajout, pour une seule raison : tenir la **couleur spéculaire séparée** d'OpenGL 1.2 par
les deux chemins de dessin, dernière fonction qui manquait pour annoncer 1.2.

## Le problème

Sous `GL_LIGHT_MODEL_COLOR_CONTROL = GL_SEPARATE_SPECULAR_COLOR`, OpenGL ajoute la spéculaire
**après** l'environnement de texture (la « somme des couleurs »), au lieu de la mêler à la
couleur primaire avant. Relevé dans l'invité (`docs/re/textures-3d.md` §5, scène `sepspec`) :

* sur le **chemin brut** (v7), l'hôte éclaire lui-même : c'était déjà exact ;
* sur le **chemin hérité**, GLEngine éclaire, garde la primaire sans spéculaire en `+0x30` de son
  sommet et range la spéculaire en **`+0x50`**. Les opcodes hérités (v1–v10) n'avaient pas de
  place pour elle : elle était perdue ;
* le **rendu d'Apple** ignore `+0x50` : il ne tient pas la spéculaire séparée non plus.

Le chemin hérité reste indispensable : c'est par lui que passent les textures 3D (GLEngine jette
la géométrie brute quand une texture 3D est active) et tout état que le chemin brut refuse.

## L'ajout

`QGPU_OP_DRAW_TRIANGLES_SEC` (0x36) `[nverts, off, nunits]` : les sommets de
`DRAW_TRIANGLES_TEXN`, pour **0** à 4 unités, suivis de la couleur secondaire r, g, b
(`QGPU_VERTEX_SEC_WORDS(n)` mots). La somme est toujours faite pour ce dessin, après
l'environnement de texture et avant le brouillard ; la couleur est bornée à [0,1] par le cœur.
Aucune clé d'état nouvelle, rien ne change pour les opcodes v1–v10.

Côté hôte, le cœur donne au backend la position de la couleur (`QgpuCore.cur_sec`, comme le motif
de pointillé et la requête ouverte) : le backend de référence la passe à `soft_tri`, qui savait
déjà l'ajouter pour `DRAW_RAW` ; le backend OpenGL pose un tableau de couleurs secondaires et
`GL_COLOR_SUM` le temps du dessin.

## Preuve

* `run_v11` de `tests/qgpu_core_test.c`, deux backends : texture noire en MODULATE + secondaire
  (0,4 ; 0,6 ; 1) → `6699ff` ; le même dessin par `TEXN` → noir ; sans texture 0,2 + 0,4 →
  `999999` ; saturation ; brouillard de facteur 0 → couleur du brouillard (il vient après la
  somme) ; 5 unités et longueur fausse refusées. **0 échec.**
* Dans l'invité, scène `sepspec` : spéculaire séparée **blanche** par le chemin brut **et** par le
  chemin hérité (`POMPPC_GL_GEOM=0`) ; noire sous le rendu d'Apple seul.

## Ce que le plugin en fait

Avec un device v11, un lot de triangles hérités sous éclairage en `GL_SEPARATE_SPECULAR_COLOR`
part en `DRAW_TRIANGLES_SEC` (genres `RK_TRI_SEC0..4`), la spéculaire lue en `V_SEC` (`+0x50`)
du sommet, celle du sommet provoquant en ombrage plat. Les lignes et points hérités n'en ont
pas (comme sous Apple) : cas rare, noté.
