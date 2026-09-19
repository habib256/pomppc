# Protocole qgpu v12 — les sources croisées de GL_COMBINE

Un seul ajout, pour la dernière fonction d'OpenGL 1.4 que ni l'hôte ni le rendu d'Apple ne
tenaient : **`ARB_texture_env_crossbar`**, qui permet à l'environnement d'une unité de prendre
comme source la texture d'une **autre** unité (`GL_TEXTUREn` dans `GL_SOURCEi_RGB/_ALPHA`).

## Le problème

Le champ de source de `QGPU_SK_COMBINE_SRC<u>` (v5) a 3 bits et n'employait que 0..3
(`TEXTURE`, `CONSTANT`, `PRIMARY`, `PREVIOUS`). Le plugin refusait donc toute source croisée,
et le rendu d'Apple reprenait la main — mais il se trompe dès que la source croisée entre dans
une opération : `MODULATE(TEXTURE0, TEXTURE1)` y rend la seconde texture seule (scène `tex14`,
`docs/re/opengl-1.4.md` §2).

## L'ajout

`QGPU_CS_TEXTURE0 + n` (valeurs **4 à 7**) : la texture de l'unité `n` (0..3), vue comme la voit
`GL_TEXTURE` dans sa propre unité (format de base, mode de profondeur). Dans l'unité `u`,
`QGPU_CS_TEXTURE0 + u` vaut `QGPU_CS_TEXTURE`. Une unité désignée sans texture, ou dont la
texture est incomplète : résultat **indéfini**, comme en OpenGL. Le backend doit tenir
OpenGL 1.4 (`QGPU_CAP_GL14`). Aucun opcode, aucune clé nouvelle ; avant la v12, ces valeurs se
lisaient modulo 4.

Côté hôte : le backend OpenGL traduit en `GL_TEXTURE0 + n` ; le backend de référence
échantillonne désormais **toutes** les unités d'un fragment avant d'appliquer les
environnements, pour qu'une unité puisse lire la texture d'une unité qui vient après elle.

## Preuve

`run_v12` de `tests/qgpu_core_test.c`, les deux backends : unité 0 ← texture de l'unité 1
(`99ffff`), `MODULATE(TEXTURE0, TEXTURE1)` (`990000`), unité 1 ← texture de l'unité 0 et non
`PREVIOUS` (`ff0000`). **0 échec.** Dans l'invité, les deux cas crossbar de la scène `tex14`
passent par les deux chemins.

## Ce que le plugin en fait

`combine_src_code` : `GL_TEXTUREn` de sa propre unité → `QGPU_CS_TEXTURE` (comme avant) ; d'une
autre unité jusqu'à la 4e → `QGPU_CS_TEXTURE0 + n` avec un device v12 (`G.xbar`) ; au-delà,
refus. `GL_ARB_texture_env_crossbar` (bit 2) est annoncée avec `G.xbar`, et c'est la dernière
condition de `GL_VERSION = « 1.4 POMPPC-1.0 »`. `POMPPC_GL_XBAR=0` coupe.
