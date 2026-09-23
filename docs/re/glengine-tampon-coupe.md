# GLEngine coupe ses lots `GL_TRIANGLES` à cheval sur deux `BeginPrimitiveBuffer` (relevé du 22–23/09/2026)

**Symptôme** : dans UT2004 (logo animé, murs et arcades en jeu), des « échardes » — triangles
longs et fins reliant des sommets de triangles voisins — sur une partie d'un maillage, qui
disparaissent et reviennent. Plus nombreuses avec la fusion des lots, présentes sans elle.

**Méthode** : toutes les sondes invité (positions, `w`, pas de sommet, fermetures de tampon,
erreurs hôte) étaient muettes. Le vidage des soumissions (`POMPPC_GL_DUMP`, plugin) rejoué en
natif (`tests/qgpu_replay.c`, cœur + backend logiciel) a **reproduit** le défaut sur l'hôte avec
des données exactes ; l'analyse du `DRAW_RAW` de 14 509 sommets du texte (image 5) montre que
la « phase » des triplets est décalée d'un sommet **dès le sommet 0** : 14 509 = 3 × 4 836 + 1.

**Mécanisme** : le plugin offrait à `BeginPrimitiveBuffer` jusqu'à `GEOM_MAX_SLOTS = 8192`
sommets, ou ce qui restait de place — jamais arrondi à la primitive. Pour `GL_TRIANGLES`
(et `QUADS`, `LINES`), GLEngine **remplit le tampon jusqu'au bout et continue la primitive
coupée dans le tampon suivant** : le second lot commence par la fin du triangle précédent.
Mis bout à bout dans une même série (`raw_count += n`), ou envoyés séparément, les lots
qui suivent la coupure ont tous leurs triangles décalés. Les rubans et éventails, eux, sont
recoupés proprement (GLEngine répète les sommets, `POMPPC_GL_GEOM_SLOTS` l'avait établi).

**Correctif** (`geom_begin`) : le nombre de places offertes est arrondi vers le bas au multiple
de la primitive (3, 4, 2) ; et (`geom_end`) seules les primitives entières d'un lot sont
comptées dans une série (un lot réellement incomplet — `glEnd` prématuré — clôt la série).
Vérifié par l'utilisateur sur le logo : texte net. Le recollage indexé (`tris_emit`) ignorait
déjà les restes ; il ne pouvait rien contre un tampon dont le premier sommet est orphelin.

**Leçon** : ce que l'on offre à GLEngine doit être un nombre entier de primitives ; ce qu'il
rend n'est pas forcément aligné sur la primitive.
