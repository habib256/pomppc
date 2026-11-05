# Quartz Extreme : région de dessin et état du ciseau (05/11/2026)

La voie `STATE_BLOCK` ne transportait pas l'ajustement du ciseau à la région du
WindowServer. La voie scalaire `compute_state`, elle, le faisait depuis Quartz Extreme.
Ce défaut du contrat logiciel est reproduit nativement, **et dans Tiger depuis le 05/11** :
c'est bien lui qui retirait l'ombre sous la barre des menus (§ « Preuve dans Tiger »).
Plugin `20261105-ws-scissor`, MD5 `0054a39d43c15d4b17a2227ed745f433`.
**[A] Confirmé à l'œil par l'utilisateur le 05/11 au soir, sur la quotidienne du M4 : ombre juste.**

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

Les tests n'émulent ni le noyau, ni les attentes de barrière, ni le GPU : la preuve visuelle
est au paragraphe suivant.

## Preuve dans Tiger (05/11, M4)

**Reproduction [A].** Le test du 05/11 à 00 h 15 ne voyait rien parce qu'il comparait le bureau
avant et après dans le même état faux (zéro pixel de différence). La bonne mesure est le
**profil vertical sous la barre**, ici le vert à x=300, y=22..30, juste après le démarrage
(1024×768×16, `bench/qe-menu-0511/*/bureau.ppm`) :

| Plugin et chemin | y=22..30 | ombre |
|---|---|---|
| `texblank`, bloc d'état (quotidienne `modes` comprise) | 152 128 136 136 136 128 136 136 136 | **absente** |
| `texblank`, `STATECHECK=1` (vecteur de l'invité appliqué) | 96 88 96 104 112 112 120 120 128 | présente |
| `ws-scissor`, bloc d'état (défaut) | 96 88 96 104 112 112 120 120 128 | présente |
| `ws-scissor`, `STATEBLK=0 STSKIP=0` (oracle scalaire) | 96 88 96 104 112 112 120 120 128 | présente |

Le journal de QEMU sous `STATECHECK=1` confirme la cause : `bloc d'état : écart, 4 clé(s),
première 15 : cœur 0, invité 1`, 15 étant `QGPU_SK_SCISSOR`. Avec l'ancien plugin, un changement
de mode rend l'ombre, ce qui expliquait l'aspect instable du défaut. Le défaut ne date pas de
`texblank` : la quotidienne en `20261102-modes` montre le même profil.

**Correction [A].**
- `ws-scissor` par défaut contre oracle scalaire, redémarrage entre les deux, 26 images (repos,
  TextEdit déplacé sous la barre, huit menus Finder) : à part l'horloge et le curseur de texte,
  seuls 10 pixels des triangles du Dock diffèrent. Ces pixels varient aussi entre deux tours de
  la même configuration (`fix-normal-fen` contre `fix-normal2-fen`) : minutage du Dock, pas
  chemin d'état.
- Changements de mode 1920×1080×32, 1024×768×32, 1920×1080×16, 1024×768×32 : images égales au
  témoin à l'horloge près (`fix-normal-modes`).
- plugmd5 aux modes 0/1/2 : 69 images égales entre elles et égales à `texblank`, avec le seul
  témoin déjà connu `tex14` (`bench/plugin/ws-scissor-0511/plugmd5-tailles`).
- Jeux, défaut (`STPATCH=0`) : mb, zen, ut, d3 en fenêtre, cmr en course 1920×1080, tous verts,
  zéro repli, rejeux et références à 0,00 % sauf le curseur de mb
  (`bench/tcg/ab/m4-wsscissor-preuve-0511-*`) ; d3 en `STPATCH=2` vert
  (`m4-wsscissor-controle-0511-d3`).

**Limites.** [F] Aucune comparaison avec un vrai G4 : l'oracle est le chemin scalaire, et la
bonne image celle que Tiger montre après un changement de mode. La capture de l'utilisateur
n'a pas pu être lue (dossier temporaire de macOS) : « traces anormales » est rapproché de cette
ombre par son texte, sa confirmation à l'œil reste à obtenir. Le rapport ne vaut pas pour le PC
tant que le plugin n'y est pas installé (code de l'invité, rien à porter).
