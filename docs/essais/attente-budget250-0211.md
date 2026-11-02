# Attente à budget fixe de 250 ms : essai écarté, 02/11/2026

Instantané de travail avant le repli progressif. Binaire aussi construit avec
0066, zone froide éteinte dans les comparaisons ci-dessous. Pas un état publié.

[A — hôte M4] Trois passages par bras, référence et témoin identiques,
au repos, même binaire, pas de trace. Médianes ms/image :

| jeu | référence | témoin | budget 250 ms | écart/réf |
|---|---:|---:|---:|---:|
| mb-fen | 6,165033 | 6,093067 | 6,120600 | −0,72 % |
| zen-fen | 2,616791 | 2,650840 | 2,368637 | −9,48 % |
| d3-fen | 28,788278 | 28,980070 | 29,015391 | +0,79 % |
| ut-fen | 19,462917 | 19,367250 | 19,966600 | +2,59 % |

Sur UT, chacun des trois budgets est plus lent que les six contrôles.
L'ancienne perte de 10 % est réduite, pas éliminée. Candidat écarté ;
campagne arrêtée volontairement pendant froid-3, données partielles conservées.
**CMR non joué**, aucune conclusion sur ce jeu ni de gain générique.

Les six preuves d'image de 0066+0067 sont vertes ; 587 contrôles de contrat
et cinq mutants réussis. Données locales : bench/tcg/ab/m4-budget-froid-0211-fen,
provenance dans bench/tcg/ab/m4-budget-froid-0211/provenance.json.
La suite garde le seuil de densité et allonge le repli quand une rafale
reprend immédiatement après son expiration. Le nouveau fpexctest.c est
préparé dans cet instantané, pas encore compilé ni validé.
