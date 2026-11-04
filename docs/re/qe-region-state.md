# Quartz Extreme : région de dessin et état du ciseau (05/11/2026)

La voie `STATE_BLOCK` ne transportait pas l'ajustement du ciseau à la région du
WindowServer. La voie scalaire `compute_state`, elle, le faisait depuis Quartz Extreme.
Ce défaut du contrat logiciel est reproduit nativement. **Son attribution aux traces sous
la barre des menus et sa correction visuelle restent à vérifier dans Tiger** ; le plugin
`20261105-ws-scissor` ne doit pas être installé en production avant cette preuve.

## Écart certain et correction

Le GS de GLEngine porte un rectangle de ciseaux relatif au drawable. `pomppc_ws_region`
reçoit séparément la région de surface : activation, origine supérieure gauche, dimensions,
et origine GL `ws_oy = hauteur - (by + bh)`. La région peut changer sans modifier le GS.

`compute_state` décalait et intersectait le ciseau de GLEngine avec cette région ; si le
ciseau GL était désactivé, il le forçait à la région. `send_state_blk` copiait seulement le
GS et le décodeur `qgpu_state_block_compute` ne connaissait pas la région. Exemple : surface
128×96, région supérieure gauche (10,5,80,40), ciseau GL désactivé :

| Voie | clés SCISSOR, X, Y, W, H |
|---|---|
| ancien bloc brut | 0, 0, 0, 0, 0 |
| scalaire et bloc corrigé | 1, 10, 5, 80, 40 |

Le calcul du ciseau de `compute_state` est extrait dans `compute_scissor`, sans changer
ses règles. La copie du paquet reçoit son rectangle canonique : x, y reconverti en origine
GL, largeur, hauteur, activation forcée. Seul l'octet d'activation est changé dans son mot ;
les 24 autres bits sont préservés. Le GS invité reste intact. Le décodeur réel de l'hôte
retrouve ainsi les cinq clés exactes. Aucun nouveau champ de protocole ni opcode.

Le même ajustement est appliqué au snapshot candidat STPATCH et au paquet réellement écrit,
**avant** la capture de sa référence. Quand la région est inchangée, une reprise zéro clé
reste possible. Hors région, l'ajustement ne fait rien : le bloc reprend ses octets bruts.

Tout changement de région (activation, origine, largeur/hauteur, bords normalisés) pose
`st_dirty`, invalide le snapshot STPATCH et avance sa génération. Une variation de hauteur
d'écran qui change seulement l'origine GL est également relevée. Une région identique ne
produit aucune invalidation. La sortie de région force donc aussi un nouvel état, même si
le GS n'a pas changé, et ne réutilise pas la référence de la région précédente.

**`st_valid` est conservé** : il porte aussi les valeurs canoniques que l'état GL désactivé
laisse en place (couleur de brouillard, facteurs de décalage de polygones). Un simple passage
à la voie scalaire après `st_host=1` serait délicat : le cache froid du plugin ne contient
alors pas nécessairement les clés que le bloc a réellement posées. Corriger la copie du
bloc évite ce changement de représentation et laisse la propriété de l'état à l'hôte.

## Preuve native

```sh
python3 tests/pomppc_ws_scissor_test.py
```

Le harnais extrait les corps réels du calcul des clés au début de `compute_state`, de
`compute_scissor`, du snapshot STPATCH, de l'écriture du paquet plein, du prédicat STSKIP et
de `pomppc_ws_region`. Les appels au nub et les mutex sont simulés ; le décodeur utilisé
est **le vrai `qgpu_state_block_compute`**, lié depuis `patches/qgpu/qgpu-core.c`.
Le modèle natif emploie des mots uint32 et simule les lectures big-endian PPC des octets et
u16 : il n'assimile pas les octets bruts du GS à l'ordre des octets de l'hôte M4.

- **8 205 paquets/snapshots** identiques, clés SCISSOR égales au calcul scalaire : région
  sans ciseau, ciseau relatif, intersections sur les bords, vide, coordonnées négatives,
  8 192 variantes bornées et changement de région avec GS identique.
- Activation, déplacement, dimensions, variation de hauteur d'écran, région identique et
  désactivation : prédicat STSKIP, invalidation et génération vérifiés. Retour au bloc brut,
  puis nouveau candidat STPATCH zéro clé.
- Les autres mots du bloc sont identiques ; les autres clés calculées par l'hôte et ses
  valeurs conservées de brouillard/décalage sont inchangées. Les 24 bits voisins du drapeau
  scissor restent identiques.
- Cinq mutants détectés : ancien paquet non ajusté, snapshot non ajusté, oubli de `st_dirty`,
  référence STPATCH conservée et perte de `st_valid`.

Les tests n'émulent ni le noyau, ni les attentes de barrière, ni le GPU. Ils ne remplacent
pas la compilation Tiger, le dommage/retracé du bureau à définition fixe, les menus et les
fenêtres superposées, ni le contrôle des jeux avec le plugin reconstruit. Aucun résultat de
cette validation visuelle n'est revendiqué ici.
