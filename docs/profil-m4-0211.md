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

Terminés à 1 h 55 : `m4-profils-0111n-fen` (mb, zen, ut, d3), puis
`m4-profils-0111n-pe` (cmr), avec les défauts. Après la fenêtre de vitesse :
`sample` hôte 30 secondes, puis `sample` du jeu invité 10 secondes. Ces relevés
instrumentés restent séparés de l'A/B ci-dessus.

[A] Dépouillement par `threadbusy.py` : occupation estimée, attentes noyau
exclues ; coûts propres des fonctions, rapportés à la somme des vCPU actifs.
Les `???` des fils TCG sont l'heuristique du code généré de `samplesum.py`, sans
carte des adresses invitées. « Recherche » additionne les coûts propres de
`tb_lookup`, `qht_lookup_custom`, `tb_htable_lookup`, `helper_lookup_tb_ptr`,
`tb_jmp_cache_fill`, `tb_lookup_cmp`, `ppc_get_tb_cpu_state` ; les colonnes sont
indépendantes, pas des coûts inclusifs à additionner entre eux.

| Cellule | vCPU, cœurs | qgpu-render, cœur | code généré estimé | recherche | base de temps | protection JIT |
|---|---:|---:|---:|---:|---:|---:|
| mb-fen | 1,025 | 0,255 | 69,7 % | 8,05 % | 0,75 % | 3,25 % |
| zen-fen | 0,588 | 0,477 | 67,8 % | 5,08 % | 1,61 % | 3,76 % |
| ut-fen | 1,055 | 0,248 | 81,1 % | 4,84 % | 0,74 % | 1,99 % |
| d3-fen | 1,506 | 0,178 | 78,2 % | 6,87 % | 0,13 % | 3,10 % |
| cmr-pe | 1,059 | 0,186 | 74,3 % | 6,52 % | 0,31 % | 3,68 % |

Base de temps : `tbf_load` et `helper_load_tbu/tbl`. Protection JIT :
`pthread_jit_write_protect_np`. Les helpers nommés `helper_*` font 1,4–2,1 %
des vCPU actifs, dont une partie déjà dans la recherche ; les flottants nommés
font 10,2 % sur zen, au plus 1,2 % ailleurs. La génération de code nommée
(`tb_gen_code`, `translator_*`, `tcg_gen_*`, `tcg_out_*`) fait moins de 0,1 %
dans ces scènes déjà chaudes. Données détaillées locales :
`bench/tcg/ab/profils-m4-0211-somme.json`.

Sur d3, `qgpu-render` attend dans `__psynch_cvwait` pendant **82,2 %** des
échantillons ; son travail estimé vaut environ 5,2 ms de CPU par image à
29 ms/image, loin des 20 ms supposées d'après le PC. Le relevé ne mesure pas le
temps GPU, ni le chemin critique complet ; il ne justifie pas de faire passer
`nat_conv_attr` en tête sur le M4. La recherche de blocs reste visible, mais son
optimisation doit garder cmr sain sur les deux hôtes. Le profil d'UT ne retrouve
pas les 14 % de base de temps du PC : cette attribution n'est pas transférable.

Le `sample` invité montre `qn_main` entièrement en attente de condition sur
mb (860/860), zen (876/876) et d3 (837/837). Cela conforte la réserve du §1 :
ces scènes ne caractérisent pas un avis d'ombre actif à 20 Hz.

Colin a atteint la course après **410 s**, contre environ 237 s pendant l'A/B.
Capture `diagnostic-attente.png` et 2 131 sommets par échange dans sa fenêtre
confirment la course. Lectures SSH et capture de diagnostic ont accompagné la
fin du chargement ; ce premier profil sera recoupé avec les témoins de la
campagne 0058. Aucun chiffre de cette partie n'est substitué à l'A/B de vitesse.

**Suite retenue selon le TODO :** tester 0058 seulement sous la milliseconde,
contre le défaut et le mode complet corrigé, sur les cinq jeux. La zone froide
arm64 reste après cette étude ; aucune nouvelle piste proposée n'est lancée.
