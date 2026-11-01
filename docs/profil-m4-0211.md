# M4, nuit du 01 au 02/11 : rattrapage et profils

## 1. A/B de PACKBITS et du chemin des surfaces non simples

[A] Mesures sur cet hôte Mac M4, Darwin arm64, du 01/11 à 23 h 36 au 02/11 à
1 h 36 (heure Réunion). QEMU 11.1.2, binaire de référence reconstruit à 23 h 08,
SHA-256 préfixe `b8921ddf205e6b22` ; plugin installé `20261101-ombre`, MD5
`860690d582e1ac383f8c4dc0c12e0fee`, kext MD5 `06a464b59a084a377443aef18039d822`.
POLLNS et VCPUQOS restent éteints. SMP=2, 768 Mo, Quartz Extreme allumé, frontal
Cocoa natif. Ce protocole ne mesure pas le coût du frontal ImGuiDock.

Campagnes locales `bench/tcg/ab/m4-bundle-0111n-{fen,pe}` : trois parties par
bras, ordre A B C C B A A B C, redémarrage de QEMU avant chaque partie :

| Bras | `POMPPC_GL_PACKBITS` | `POMPPC_GL_QE_NONSIMPLE` |
|---|---:|---:|
| avant | 0 | 0 |
| bits | 1 | 0 |
| actuel | 1 | 1 |

Le même binaire du plugin sert aux trois bras : « avant » désigne ces deux
chemins désactivés, pas une ancienne compilation du bundle. Les variables sont
passées au jeu ; aucun changement de l'environnement du WindowServer n'est
revendiqué. Les six cellules sont mesurées sans vidage ni profilage. Elles sont
rouges par construction dans le rapport d'image ; aucune erreur d'exécution,
aucun repli. Les preuves séparées `preuve-nuit-0111-{fen,pe}` sont vertes (6/6).

Barrière avant chaque partie : agents externes, compilateurs, autres bancs
d'endurance et Time Machine terminés, puis `chargehote.py attendre`. Relevé
continu toutes les cinq secondes : les 18 parties sont valides, charge étrangère
moyenne de 0,33 à 0,38 cœur, jamais de série au-dessus de 1,5 cœur ; aucun autre
QEMU. Voir les `charge-bilan.txt` et `contexte-m4-0111n.jsonl`.

Médianes en ms/image ; pourcentages calculés **avant arrondi**, depuis les deux
bornes de `frames.csv`. Colin fait trois échanges par image : son facteur est
conservé, comme dans `Jeu.echanges_par_image`. Les valeurs détaillées et fenêtres
sont dans `bilan-precis.json` de chaque campagne.

| Cellule | avant | bits | actuel | PACKBITS, bits/avant | actuel/bits |
|---|---:|---:|---:|---:|---:|
| mb-fen | 6,1299 | 6,1477 | 6,1814 | +0,29 % | +0,55 % |
| zen-fen | 2,6534 | 2,6062 | 2,5943 | −1,78 % | −0,46 % |
| ut-fen | 19,9076 | 19,2728 | 19,3836 | **−3,19 %** | +0,57 % |
| d3-fen | 29,0766 | 28,9212 | 28,8526 | −0,53 % | −0,24 % |
| mb-pe | 6,3001 | 6,2490 | 6,2304 | −0,81 % | −0,30 % |
| cmr-pe | 44,4007 | 41,2568 | 41,0288 | **−7,08 %** | −0,55 % |

PACKBITS gagne sur UT et Colin : les trois parties `bits` sont plus rapides que
les trois `avant` dans les deux cellules (UT : 19,196–19,605 contre
19,833–19,966 ; Colin : 41,143–41,858 contre 44,330–44,638). Aucune cellule ne
perd 3 %. Les autres écarts restent **inconclusifs sur trois parties**, sans
preuve d'un petit gain ni d'une petite perte. Aucun réglage par défaut ne change.

Le bras `actuel/bits` ne démontre pas le coût d'un avis à 20 Hz sous une fenêtre
posée sur le jeu : il change aussi la méthode 5 en méthode 6 du kext, et ces
relevés ne comptent pas les avis effectivement envoyés. Tous ses écarts sont
sous 1 % et inconclusifs. Ne pas les assimiler au coût de l'ombre mesuré sur le
PC. L'utilisateur a confirmé l'ombre de DOOM 3 correcte avec la correction
active à 22 h 39 ; les mesures de vitesse ne rouvrent pas ce défaut.

L'arrondi historique à 0,1 ms suffit à afficher zen 2,7 → 2,6, soit −3,7 %,
alors que les données donnent −1,78 %. Il ne faut pas prendre une décision au
seuil de 3 % depuis ces valeurs arrondies.

## 2. Profils croisés

En cours : `m4-profils-0111n-fen` (mb, zen, ut, d3), puis
`m4-profils-0111n-pe` (cmr), avec les défauts. Après la fenêtre de vitesse :
`sample` hôte 30 secondes, puis `sample` du jeu invité 10 secondes. Ces relevés
instrumentés restent séparés de l'A/B ci-dessus.
