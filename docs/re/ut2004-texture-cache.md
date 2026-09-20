# UT2004 : réutilisation des identifiants de texture

## Modification

Le pilote pouvait saturer ses 128 identifiants hôte alors que GLEngine
conservait des centaines de textures vivantes. Une nouvelle texture sortait
alors du domaine accéléré (`id-texture`), avec rendu logiciel et relectures.

Les identifiants deviennent un cache LRU. À saturation, `alloc_tex_id()` :

1. protège toutes les textures effectives du dessin courant, y compris les
   unités pas encore téléversées ;
2. choisit la texture résidente la moins récemment utilisée ;
3. ferme les séries de dessins et termine les soumissions en vol ;
4. émet DESTROY puis laisse CREATE réutiliser le même identifiant ;
5. invalide les états des contextes et marque les niveaux/paramètres évincés
   pour rechargement depuis GLEngine lors de leur prochaine utilisation.

Pas de changement de protocole, de limite mémoire hôte ou de kext.
La terminaison des soumissions à chaque éviction privilégie la correction ;
son coût est compris dans les mesures UT2004.

## Validation dans Tiger

`gltest texcache` garde 257 textures vivantes, parcourt les textures dans
plusieurs ordres sans attendre entre dessins, modifie une texture évincée,
protège une deuxième unité et alterne deux contextes partageant les objets.
768 pixels témoins sont vérifiés par mode. Les images complètes sont
identiques au rendu Apple dans les trois modes accélérés : brut asynchrone,
brut synchrone et chemin hérité.

- Ancien pilote, première version du test : 774 refus `id-texture`,
  390 appels logiciels et 75 relectures.
- Cache : 483 évictions, aucun refus `id-texture`, 3 appels logiciels et
  3 relectures. Tous les pixels témoins passent.
- Scènes `tex`, `comb`, `mix`, `game`, `tex3d`, `cube`, `varrayvbo` : passent.
- `tex14` : défaut préexistant au témoin « λ=2 sans biais » ; confirmé avec
  l'ancien pilote et le nouveau, images complètes identiques. Ce changement
  ne prétend pas corriger ce défaut de mipmaps.
- Compilation GCC 4.0 : seul avertissement `tex_base_lv` inutilisé, préexistant.

Preuves locales : `bench/ut-texcache/initial-tests/` et
`bench/ut-texcache/shared-tests/`.

## Mesure UT2004

Même cinématique AS-Convoy, même pas de simulation et mêmes swaps 13 à 73
que la [référence](ut2004-flyby.md). Un passage par résolution, sans
Zenerchi, capture au swap 74 et fermeture contrôlée du jeu. Les comparaisons
portent sur les intervalles entre swaps de l'application.

Le cache supprime les refus `id-texture` et les appels logiciels dans la
fenêtre mesurée. La géométrie auparavant traitée par Apple passe maintenant
au chemin brut, ce qui augmente son compteur de sommets. Les captures
montrent le même point du parcours ; elles ne sont pas pixel-identiques à
l'ancien rendu, et certains défauts visuels restent à corriger.

| Résolution | Avant (img/s) | Cache (img/s) | Gain | Médiane cache | P95 cache |
| --- | ---: | ---: | ---: | ---: | ---: |
| 800x600 | 1.08 | 10.89 | ×10.1 | 101 ms | 147 ms |
| 1024x768 | 0.69 | 10.77 | ×15.6 | 102 ms | 149 ms |

Dans les deux résolutions : environ 2 242 → 0 appels logiciels par image,
29,43 → 1 relecture par image. Aucun refus `id-texture`, aucune primitive
perdue et aucun repli de soumission synchrone dans les journaux des passages.
Les deux processus quittent avec le code 0. Réglages originaux restaurés.

Preuves : `bench/ut-texcache/800x600/` et `bench/ut-texcache/1024x768/`,
avec rapports JSON, traces CSV, captures au même point et sources archivées
sous `bench/ut-texcache/sources/`.
