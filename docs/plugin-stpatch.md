# STPATCH : deltas cull et stencil sans changer le protocole

Prototype du 04/11/2026, **éteint par défaut**, présent dans le plugin courant `20261105-ws-scissor`.
Ni QEMU ni kext à reconstruire. **État au 05/11 :** équivalence d'image prouvée en VM, A/B à la fin de cette page.
La révision b1 rétablit la frontière de flux des candidats sans clé, omise dans le premier
prototype. Le défaut visuel signalé au titre 3D de Colin McRae n'en venait pas : il existait aussi
en mode 0, et il est corrigé par TEXBLANK ([détail](re/cmr-rendu-vers-texture.md#8-niveaux-vierges-plus-grands-que-larène-04112026)).

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
Sous région WindowServer, la copie du bloc et le snapshot candidat portent le même ciseau
ajusté, issu du calcul scalaire ; le GS n'est pas modifié. Un changement de région invalide
le snapshot et l'état froid, y compris au retour hors région, sans perdre `st_valid`
([preuve native](re/qe-region-state.md), image encore à valider).
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

Pas d’A/B de ce premier prototype ; celui du bundle du 05/11 est à la fin de cette page.
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

## A/B au repos du bundle `20261105-ws-scissor` (05/11, M4)

[A] Terminal, hôte au repos, frontal natif, `r_shadows=1`, ms/image, médianes, modes 0 contre 1.
Six parties par bras en deux tours d'ordre inversé pour cmr, ut, d3 ; trois pour mb et zen.
`bench/tcg/ab/m4-stpatch-ab-0511-{fen,pe}`, `m4-stpatch-ab2-0511-{fen,pe}`.

| jeu | parties | mode 0 | mode 1 | écart | étendue 0 | étendue 1 |
|---|---:|---:|---:|---:|---:|---:|
| d3 fenêtre | 6 | 36,862 | 36,067 | **−2,16 %** | 36,458-37,158 | 35,598-36,584 |
| ut fenêtre | 6 | 19,502 | 19,700 | +1,01 % | 19,239-19,809 | 19,410-20,238 |
| cmr en course | 6 | 39,603 | 39,867 | +0,67 % | 38,945-40,647 | 39,456-41,069 |
| mb fenêtre | 3 | 6,362 | 6,398 | +0,57 % | 6,233-6,423 | 6,193-6,473 |
| zen fenêtre | 3 | 3,451 | 3,469 | +0,52 % | 3,435-3,454 | 3,447-3,473 |

- **d3 gagne environ 0,8 ms/image** : bras disjoints dans chacun des deux tours. Sur les ~8 ms que
  coûtent les ombres (`chantiers/d3-macos272.md`), STPATCH en rend donc un dixième : réduire la
  taille du flux ne suffit pas à récupérer ce coût.
- ut, cmr, mb, zen : bras recouverts, ni perte ni gain prouvés.
- **Toujours éteint.** Reste avant tout allumage : mb et zen à six parties, l'endurance, la matrice.
  **Décision de l'utilisateur, 05/11 au soir : on poursuit, un levier qui ne sert qu'à d3 l'intéresse.**
  **Compromis accepté par l'utilisateur le même soir :** une petite perte sur mb et zen, déjà parfaitement
  jouables sur le M4, est acceptable si d3 et Prey gagnent. La règle reste entière pour ut et cmr. Prey reste à mesurer.
- Première série polluée par un frontal orphelin (0,17 cœur, mêmes conditions dans les deux bras) ;
  arrêté avant la seconde. Ce biais est écrit ici plutôt que corrigé dans les chiffres.

## Soirée du 05/11 : mb, zen et Prey (M4)

[A] Terminal, hôte au repos, frontal natif, ms/image, médianes, modes 0 contre 1.
mb et zen avec `r_shadows=1` passé par le banc, sans effet sur eux : `bench/tcg/ab/m4-stpatch-ab{,3,4}-0511-fen`. Prey : `m4-stpatch-ab6-0511-fen`.

| jeu | parties | mode 0 | mode 1 | écart | étendue 0 | étendue 1 |
|---|---:|---:|---:|---:|---:|---:|
| mb fenêtre | 12 | 6,236 | 6,313 | +1,2 % | série par série : +0,6, +3,2, +0,0 % | |
| zen fenêtre | 12 | 3,164 | 3,219 | +1,7 % | série par série : +0,5, +0,8, +2,3 % | |
| **Prey fenêtre** | 6 | 30,535 | 32,445 | **+6,3 %** | 30,29-30,70 | 32,20-32,67 |
| Prey, 90 s à horloge égale | 6 | 30,255 | 30,795 | **+1,8 %** | 30,01-30,37 | 30,66-30,98 |

- **Prey perd, bras disjoints sur les deux mesures.** La scène suit l'horloge, d'où la seconde mesure : images rendues de 10 à 100 s
  après le chargement. Par tranche : 10-40 s +4,4 %, 40-70 s +0,6 %, 70-100 s +1,7 %.
- **La condition du compromis n'est pas remplie** (d3 **et** Prey gagnent) : **STPATCH reste éteint.** Allumage, endurance et matrice
  ne sont pas lancés ; la décision revient à l'utilisateur.
- [F] Cause inconnue. Compteurs à l'image 6500 de Prey : 1 416 170 blocs remplacés par 440 342 clés, donc un flux plus petit,
  et pourtant plus lent. Tranché par : un profil de l'invité et de l'hôte sur 10-40 s après le chargement, modes 0 et 1.
- L'A/B `m4-stpatch-ab5-0511-fen` est écarté : 9 parties sur 12 mesuraient une rafale d'images vides (`tools/matrice/jeux/prey.py`).

**Décision de l'utilisateur, 05/11 au soir : STPATCH abandonné.** Pas de profil de Prey. Le code reste dans le plugin, éteint par défaut ; aucun allumage prévu.
