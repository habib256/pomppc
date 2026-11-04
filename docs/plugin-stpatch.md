# STPATCH : deltas cull et stencil sans changer le protocole

Prototype du 04/11/2026, **éteint par défaut**, dans le plugin `20261104-stpatch-b1`.
Ni QEMU ni kext à reconstruire. Aucun gain de jeu ni équivalence d'image en VM encore revendiqué.
La révision b1 rétablit la frontière de flux des candidats sans clé, omise dans le premier
prototype. Un défaut visuel a été signalé au titre 3D de Colin McRae sous ce prototype ;
**b1 n'est pas encore une correction visuelle CMR prouvée**. La comparaison de cette scène
aux modes 0/1/2 reste à faire, indépendamment de la preuve native de frontière rétablie.

Le niveau de DOOM 3 avec ombres envoie environ 700 `STATE_BLOCK` de 109 mots par image.
L'audit du flux du témoin montre qu'environ trois quarts de ces blocs ne changent que
les faces et le stencil. STPATCH remplace ces seuls blocs par les `SET_STATE` déjà existants,
pour les **clés canoniques effectivement différentes**, dans l'ordre du bloc. Un bloc identique
ou un changement des seuls champs arrière (que le décodeur actuel ne lit pas) n'envoie aucune clé.
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
La réservation reste obligatoire pour zéro clé : elle ferme les séries géométriques RUN/RAW,
attend le flux disponible et lie le contexte, comme le faisait le bloc complet. L'équivalence
des clés ne prouve pas que supprimer cette frontière de flux serait exact.

## Épreuves natives

`python3 tests/pomppc_stpatch_integration_test.py` (également dans `tests/run-all.sh`) :

- **134 539 transitions**, dont 131 468 acceptées, comparées sur toutes les clés au **vrai**
  `qgpu_state_block_compute` : 0 divergence. Chaque bit de chaque mot, stencil off/on,
  capacités variées, références/masques de 32 bits, cull non canonique, arrière ignoré,
  champs étrangers et enums invalides. Snapshot après delta identique au nouveau bloc.
- Corps réels de `stpatch_try` et `reserve` extraits du plugin : une réservation, delta de deux clés,
  trois pertes de référence réentrantes, rollback conservant `CTX_BIND`, override froid,
  fermeture RUN/RAW et liaison de contexte même sans clé, trois invalidations pendant la
  réservation zéro, modes de contrôle avec et sans clé : tous passent.
- **Six mutants détectés** : référence stencil off fausse, mauvais masque d'octet cull,
  champ arrière employé comme avant, contrôle de génération supprimé, rollback supprimé,
  réservation sautée sans clé. L'ancien code échoue aussi à cette nouvelle épreuve de flux.

Ces tests ne remplacent pas la compilation Tiger, `plugmd5` à l'octet (0/1/2), les scènes de
points/bitmap/mixte, les contrôles en jeu et l'A/B entrelacé de **mb, zen, ut, d3 et cmr**.
Ne l'allumer qu'après preuves d'image et absence de perte sur toute la matrice, puis endurance.

## Contrôles VM du premier prototype (04/11, M4)

Révision `20261104-stpatch` (`f0416c02`), uniquement dans une copie du disque Tiger ;
le disque quotidien et `disks/prebuilt` gardent le plugin `20261102-modes`.
Les épreuves ci-dessous précèdent b1 et ne valent pas validation de b1.

- `bench/plugin/ombres-0411/plugmd5-tailles` : **69 images identiques à l’octet** aux modes
  0/1/2, contexte local accéléré POMPPC, tailles natives de chaque scène. 68 scènes passent
  toutes leurs assertions ; le témoin λ2 de `tex14` échoue identiquement aux trois modes et
  avec le plugin d’avant STPATCH (même MD5 d’image `906383cc967bb3c18ba0755b0bca7ec4`).
  Le journal strict conserve ses trois erreurs d’assertion ; aucune différence d’image.
  Les essais précédents sans image (pbuffer distant) ou forcés en 256² sont écartés.
- `bench/tcg/ab/m4-stpatch-preuve-0411-fen/opt-1` : mb, zen, ut et d3 en fenêtre verts,
  zéro repli et zéro écart rejeu/VM. D3 conserve `r_shadows=1`.
- `bench/tcg/ab/m4-stpatch-controle-0411-d3/verif-1` : D3 vert en mode 2,
  **1 225 512 candidats contrôlés, zéro divergence**.
- La référence D3 du M4 avec ombres est celle du témoin
  `m4-ombres-base-0411-fen/ref-1`, regardée et validée. L’ancienne sans ombres est conservée
  sous `bench/matrice/ref/d3-fen-sans-ombres-0111` ; ne pas comparer les deux qualités.
- **CMR reste ouvert** : l’utilisateur a signalé des aplats blancs au titre 3D en 1920×1080.
  Reproduits aussi en mode 0, captures `bench/plugin/ombres-0411/cmr-ref0-pilotage` ;
  le premier vidage concernait le menu DÉFIS et le suivant un film, aucun ne localise le
  défaut 3D. Essai de résolution en cours : les notes montrent des refus `COPY_TEX` à
  1920×1080 et `fallback tex-size 780/438`, à distinguer de STPATCH.

**Aucun A/B de vitesse du prototype n’a encore été lancé**, aucun gain en ms revendiqué.
Les temps des épreuves ne sont pas des A/B au repos. Contrôle général avant b1 :
522 OK, 0 échec, 5 ignorés depuis Terminal (`.run/tests-stpatch-terminal-0411.log`).
Après b1, l’épreuve native ciblée passe à nouveau avec ses six mutants.

## Bundle `20261104-texblank` après b1

Compilation Tiger sur la même copie de disque, MD5 `b3dd60c9465b73bc45fba929b7b6071e`.
69 comparaisons plugmd5 0/1/2 identiques, contrôle sans divergence ; même témoin préexistant
`tex14` (68 scènes entièrement vertes). Le correctif distinct des grandes textures du titre
CMR est décrit dans [`cmr-rendu-vers-texture.md` §8](re/cmr-rendu-vers-texture.md#8-niveaux-vierges-plus-grands-que-larène-04112026).
Les titres 800×600 et 1920×1080 se rejouent sans erreur et à 0,00 / 0,00 % de leur capture.
La matrice complète de ce bundle reste à terminer avant A/B. STPATCH reste éteint par défaut.
