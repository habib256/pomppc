# Temps de chargement des jeux

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Piste non lancée.

## Piste proposée le 01/11 (à arbitrer par l'utilisateur)

5. **Temps de chargement des jeux.** cmr met ~70 s à charger sa course, DOOM 3 autant pour un
   niveau, le binaire PGO instrumenté n'atteint pas la course en 900 s : jamais mesuré ni
   optimisé, et c'est ce que l'utilisateur subit à chaque partie. Mesurer d'abord (secondes du
   lancement à la scène de mesure, par jeu ; la matrice a déjà l'horloge), puis les pistes :
   mémoire de l'invité 768 Mo → 1,5 Go (le mac99 va à 2 Go ; moins de pagination, les
   `copypv` des `read()` à 1 000-2 800/s), cache disque des shaders traduits sur l'hôte
   ([`plus-tard.md`](plus-tard.md)), disque IDE (DMA, taille des requêtes). **Fermeture :** temps de chargement dans
   `frames.csv` ou le tableau de la matrice, et un levier prouvé. Coût de la mesure : une
   demi-journée.
