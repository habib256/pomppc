# STPATCH : deltas cull et stencil sans changer le protocole

Prototype du 04/11/2026, **éteint par défaut**, dans le plugin `20261104-stpatch`.
Ni QEMU ni kext à reconstruire. Aucun gain de jeu ni équivalence d'image en VM encore revendiqué.

Le niveau de DOOM 3 avec ombres envoie environ 700 `STATE_BLOCK` de 109 mots par image.
L'audit du flux du témoin montre qu'environ trois quarts de ces blocs ne changent que
les faces et le stencil. STPATCH remplace ces seuls blocs par les `SET_STATE` déjà existants,
pour les **clés canoniques effectivement différentes**, dans l'ordre du bloc. Un bloc identique
ou un changement des seuls champs arrière (que le décodeur actuel ne lit pas) n'envoie rien.
Le mécanisme ne connaît ni le jeu, ni le nom d'un programme ARB, ni un type d'ombre.

## Variables et contrôle

- `POMPPC_GL_STPATCH=0` ou absent : chemin existant, aucune photographie ni comparaison supplémentaire.
- `=1` : un snapshot froid par contexte ; les différences cull/stencil permises sont envoyées en clés.
- `=2` : simulation des deltas contre `compute_state`, **voie pleine envoyée** avec le vecteur oracle
  de `STATE_BLOCK_CHECK`. `STATECHECK=1` force aussi cette voie de contrôle si STPATCH est demandé.

La ligne `STPATCH` de la note (500 images et total) donne blocs remplacés, clés émises,
blocs complets, candidats contrôlés, écarts, pertes de référence pendant `reserve` et motifs
de refus (cache, en-tête, autre champ, enum). En `=2`, attendre des candidats >0 et **0 écart**.

## Exactitude

La source du calcul est le contrat logiciel `qgpu_state_block_compute` de `patches/qgpu/qgpu-core.c`.
En-tête identique : drapeaux (VALID et V7 requis), dimensions, bits de stencil. Cinq comparaisons
de plages et le masque précis de l'octet cull à `0x317a` garantissent que tout autre champ est
inchangé. Le helper ne parcourt pas les 108 mots dans une boucle masquée sur le G4.

Le groupe stencil est recalculé en entier : quand son test est coupé, `ALWAYS`, référence 0,
masque ff et `KEEP`, même si les champs dormants portent d'autres valeurs ; write-mask et clear
restent lus. Les masques/références sont bornés aux huit bits bas. Faces non canoniques : mêmes
normalisations que le décodeur. Une enum de stencil refusée par `SET_STATE` impose le bloc complet.
Les mots arrière `0x31a8..0x31bc` sont ignorés comme par ce décodeur ; une évolution de ce contrat
doit faire échouer le test différentiel et conduire à mettre le helper à jour.

Le snapshot est invalidé avec les miroirs et l'état des textures, mais aussi après les overrides
froids de `pt_size` et `pix_punch_zero`, même si GLEngine n'a pas changé. Toutes les clés d'un
delta sont réservées **en une fois**. Après `reserve`, validité et génération de la référence sont
revérifiées : une perte ou une réentrée rend les seuls mots réservés, puis impose le bloc entier.
Un `CTX_BIND` posé par la réservation peut rester. Le snapshot d'un bloc complet vient des octets
effectivement écrits après la réservation ; après un delta, seuls les 15 mots permis sont copiés.

## Épreuves natives

`python3 tests/pomppc_stpatch_integration_test.py` (également dans `tests/run-all.sh`) :

- **134 539 transitions**, dont 131 468 acceptées, comparées sur toutes les clés au **vrai**
  `qgpu_state_block_compute` : 0 divergence. Chaque bit de chaque mot, stencil off/on,
  capacités variées, références/masques de 32 bits, cull non canonique, arrière ignoré,
  champs étrangers et enums invalides. Snapshot après delta identique au nouveau bloc.
- Corps réel de `stpatch_try` extrait du plugin : une réservation, delta de deux clés,
  trois pertes de référence réentrantes, rollback conservant `CTX_BIND`, override froid,
  candidat zéro clé et modes de contrôle avec et sans clé : tous passent.
- **Cinq mutants détectés** : référence stencil off fausse, mauvais masque d'octet cull,
  champ arrière employé comme avant, contrôle de génération supprimé, rollback supprimé.

Ces tests ne remplacent pas la compilation Tiger, `plugmd5` à l'octet (0/1/2), les scènes de
points/bitmap/mixte, les contrôles en jeu et l'A/B entrelacé de **mb, zen, ut, d3 et cmr**.
Ne l'allumer qu'après preuves d'image et absence de perte sur toute la matrice, puis endurance.
