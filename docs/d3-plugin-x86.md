# DOOM 3 sur le PC : ce que le plugin coûte au G4 émulé, et trois leviers (03/10/2026)

Étape 3 de l'orientation « DOOM 3 sur le PC Linux x86-64 » : réduire le temps PowerPC émulé
que le plugin (`guest/gldriver/pomppc_accel.c`) coûte à chaque image. DOOM 3 (démo, Tiger
10.4.11) fait **150,5 ms/image** en fenêtre sur le PC (CHANGELOG du 03/10), **56-61** sur le M4 ;
le jeu est limité par le G4 émulé (un fil principal, aucune attente sur le M4).

**État : code écrit, compilé seulement par un gcc PowerPC de l'hôte (vérification
syntaxique avec en-têtes bouchons ; `pomppc_vec.c` compilé et désassemblé). Rien n'a tourné
dans l'invité : ni gain ni exactitude ne sont prouvés.** Les trois leviers sont éteints par
défaut (`*_DEFAULT 0`) ; éteints, le plugin se comporte comme `20261001-tout` (révision
`20261003-d3x`). Aucun ne change le protocole qgpu ni le kext.

## 1. Où vont les ~45 ms

Pas de profil `sample` de l'invité sur le PC à ce jour (celui de l'autre agent est attendu).
Base : profil du M4 (`docs/vitesse-profil-2026-10-01.md`, `docs/re/verdict-lots-4-5.md`,
`docs/protocole-v23-etat.md`) et note GL d'un tour du PC
(`bench/matrice/10411-d3c/d3-fen/mesure/note.txt` du dépôt principal). Le passage M4 → PC est
une **règle de trois** (150,5 / 56,1 ≈ 2,7, moins l'attente de l'hôte propre au PC) : à
remplacer par le profil x86 mesuré.

| poste (fil principal de DOOM 3) | M4, ms/image | PC estimé | ce que c'est |
|---|---|---|---|
| plugin, tout compris | 19,6 (35 %) | **45-52** | |
| └ verdict au dispatch (`gldUpdateDispatch` → `pomppc_geom_dispatch`) | 7-8 | 19-21 | ~1 300 dispatches par image ; 54 % court-circuités par la liste blanche, ~46 % recalculés (une interaction relie ses textures) |
| └ dessin (`geom_render_array` → `geom_draw_client`) | 8-9 | 21-24 | ~1 244 dessins par image, tous `DRAW_NATIVE` |
| ┊ └ `send_state` (bloc d'état allumé) | ~1,3 | ~3,5 | clés chaudes des unités, copie des fenêtres |
| ┊ └ `geom_send_all` (matrices, lumières…) | ~1,0 | ~2,7 | comparaison de 64 octets par matrice à chaque dessin |
| ┊ └ `geom_draw_native` | ~1,5-2,7 | 4-7 | descripteurs, miroirs, **recopie des indices** (1,8 Mo/image), attente de l'hôte (≈ 1,2 sur le M4) |
| ┊ └ propre de `geom_draw_client` | ~1,8 | ~5 | **balayage min/max des indices** (456 000 indices 32 bits par image), clé du verdict, plan |
| └ appels au kext | 1,3 | ~3,5 | soumissions |
| attente de l'hôte (QRY « attente totale ») | ~0 | **4,6** | propre au PC : le rendu hôte est plus lent que l'invité |

Ce que dit la note du PC :

- `GEOMHOST : 0 DRAW_RAW_SANE, 0 dessins de tableaux clients natifs` **n'est pas un refus**.
  `NATSHM` (A4) ne concerne que les tableaux **clients** ; DOOM 3 n'en a pas : tous ses
  attributs sont dans des VBO (`idDrawVert` du cache de sommets) et passent déjà par
  `DRAW_NATIVE` v18, l'hôte lisant ses miroirs bruts (`SONDE tableaux` : `vbo 0415b100`, pas
  60). `DRAW_RAW_SANE` ne concerne que Begin/End. Le chemin « natif » est donc pris à 100 %.
- Les **indices** de DOOM 3, eux, sont en mémoire cliente (`r_useIndexBuffers 0`, défaut de la
  1.3.1) et en 32 bits : `DRAW_NATIVE … ibuf ffffffff itype 2`. Le plugin les **balaie** (plage
  [vmin, vmax]) puis les **recopie** dans BAR0 à chaque dessin.
- `LAZYAPPLE` : ~1 294 dispatches par image en jeu (646 000 par 500 images), presque un par
  dessin ; Apple n'en reçoit qu'un par image (bit 0x80).

### Pourquoi l'hôte ne lit pas les tableaux de l'invité par DMA

Ce que l'hôte ferait : lire directement, dans la RAM invitée, les copies clientes des VBO et
les tableaux d'indices, au lieu des `BUF_SUBDATA` (0,65 Mo/image) et de la recopie des indices
(1,8 Mo/image). Obstacles :

1. Ce sont des adresses **virtuelles** d'un processus utilisateur, paginables : il faudrait
   que le kext les épingle (`IOMemoryDescriptor::prepare`) et publie la liste des pages.
   Par dessin, un appel au kext coûte plus que le `memcpy` qu'il évite (`__memcpy` ≈ 0,5 % du
   fil sur le M4) ; à la création du VBO seulement, c'est possible pour les copies clientes de
   VBO, pas pour les indices (tas du jeu, `idDynamicBlockAlloc`).
2. Le rendu est **asynchrone** (une image de latence) : quand l'hôte traiterait le dessin, le
   jeu aurait déjà réécrit le cache de sommets de l'image suivante. Lire en place exigerait un
   instantané — c'est-à-dire la recopie qu'on voulait éviter (même raisonnement que
   `docs/protocole-v23-etat.md` §2 pour l'état).
3. Le gain plafonne à la recopie : ~0,5-1 % du fil. Le coût réel est ailleurs (verdict,
   balayage, clés).

Écarté. Variante sans code à essayer : `+set r_useIndexBuffers 1` (indices dans des VBO
d'éléments, miroités comme les sommets : plus de recopie par dessin, plage seulement si sale).

### Le fil de travail sur le second vCPU

Un vCPU est presque libre (`docs/smp-coeurs.md`, tableau L1-L4 : le parallélisme utile passe
par le fil du jeu). Ce qu'un fil du plugin pourrait prendre **sans copie d'état** :

| candidat | lit | verdict |
|---|---|---|
| verdict, clés, état | l'état de GLEngine **au moment de l'appel** ; il change dès le retour | impossible sans instantané (qui coûte ce qu'il évite) |
| balayage et recopie des indices, miroirs des VBO | la mémoire du jeu, réutilisable dès le retour de `glDrawElements` | interdit par le contrat GL ; le cache double d'idTech4 le rendrait sûr en pratique pour DOOM 3, pas pour les autres jeux |
| soumission au kext (1,3 ms/image M4) et attente des barrières | des données du plugin seulement | possible, mais le passage de relais (mutex, condition : appels Mach, chers sous TCG) coûte du même ordre que l'appel qu'il déplace ; une seule soumission est en vol par conception (deux moitiés) |

Conclusion : pas de fil de travail à cette étape. Le gain sûr est de **faire moins** sur le
fil du jeu.

## 2. Levier A — indices : ne balayer que si nécessaire, et par AltiVec

`POMPPC_GL_IDXLAZY` (0/1/2) et `POMPPC_GL_IDXVEC` (0/1/2), éteints par défaut.

**Constat.** Pour un dessin indexé, `geom_draw_client_unsafe` calcule [vmin, vmax] par
`va_scan_idx` (une boucle de 7 à 9 instructions par indice) **avant** de savoir si la plage
servira. Dans `geom_draw_native`, elle ne sert qu'à (1) recopier les plages **sales** des
miroirs qu'elle couvre (`raw_sync`) et (2) refuser un indice hors du VBO. L'hôte calcule déjà
lui-même [lo, hi] des indices (`DRAW_NATIVE` v18, §Sémantique 2) et borne chaque lecture à
son tampon (refus `BAD_ARG` non fatal).

**IDXLAZY=1.** Le balayage est reporté dans `geom_draw_native`. Si tous les attributs lus
viennent de VBO à miroir **propre** (`nat_mirrors_clean` : tranche assez grande, même copie
cliente qu'au dernier ajustement, aucune plage sale — exactement les cas où `raw_ensure` rend
1 sans rien toucher et où `raw_sync` ne recopie rien), il n'a pas lieu : les descripteurs
désignent le sommet 0 (sans tableau client, `vbase` = 0 dans les deux voies), et la commande
`DRAW_NATIVE` est **la même octet pour octet**. Sinon (tableau client, miroir sale, VBO
inconnu) : balayage d'avant, mêmes refus. Si le chemin natif se dérobe ensuite,
l'empaquetage balaie (« balayés après coup »). Après la recopie d'un VBO d'éléments (qui peut
vider le flux, et un vidage invalider des miroirs), les miroirs des sommets sont revérifiés.

Écarts, seulement pour un dessin fautif : un indice hors du VBO n'est plus refusé par
l'invité (repli vers l'empaquetage) mais lu par l'hôte dans la réserve (16 Mio) qui contient
le miroir, ou refusé par lui (`BAD_ARG`, dessin perdu). En GL, c'est un comportement
indéfini. `IDXLAZY=2` balaie quand même et compte ces cas (« contrôle … écarts »).

**Limite connue** : un VBO dont un dessin ne lit pas les derniers octets (passe de profondeur
qui ne lit que la position d'un `idDrawVert` de 60 octets) garde une plage sale résiduelle
jusqu'au premier dessin qui lit tout ; tant qu'elle existe, ce VBO est balayé. Les compteurs
diront la part des dessins sans balayage ; si elle est faible, raffinement possible : marquer
propre un résidu situé au-delà de la dernière fin d'attribut possible.

**IDXVEC=1.** Quand le balayage a lieu, `pomppc_vec.c` (seul fichier compilé avec `-faltivec`)
le fait par `lvx` / `vminuw` / `vmaxuw` (`vminuh` en 16 bits), 16 indices par tour (~19
instructions au lieu de ~140), tête et queue scalaires pour l'alignement. QEMU traduit
`vminuw` en `tcg_gen_gvec_umin` (SIMD de l'hôte, x86 comme arm64). Exact par construction ;
`IDXVEC=2` compare chaque balayage au scalaire, qui fait foi en cas d'écart (lignes `IDXVEC
écart`). Activé seulement si `hw.vectorunit` (ou `hw.optional.altivec`) l'annonce.

**Gain attendu** (estimation, à mesurer) : le balayage pèse ~3,5-4 M instructions invitées par
image ; ~1-1,3 ms/image sur le M4 (part du « propre » de `geom_draw_client`), **~2-3,5 ms sur le
PC**. IDXVEC en retire ~85 % de ce qui reste balayé ; IDXLAZY retire les dessins de géométrie
statique propre.

## 3. Levier B — verdict refait pour les seules unités désignées

`POMPPC_GL_UNITVD` (0/1), éteint par défaut ; exige `VERDICT`, `WHITELIST`, `TEXMEMO` (défauts).

**Constat.** ~46 % des dispatches de DOOM 3 ne sont pas neutres pour la liste blanche du
lot 3 (`docs/re/bloc-changements-r4.md` §4), presque tous pour une seule raison : le bloc
`0 00000032 … 02900000` (une interaction relie bosselage, diffuse et spéculaire, unités 1, 4,
5 ; le reste du bloc est neutre). Le verdict complet refait alors `geom_ok` (rastérisation,
programmes, tableaux, **toutes** les unités : ~7 sous le programme d'interaction),
`texture_ok` (toutes les unités, téléversement compris), `geom_format`, `va_gen_sizes`.

**Ce que fait `uv_take`.** Si le bloc ne porte, hors des bits neutres de la liste blanche
(`wl_mask`, `wl_extra`), que des bits d'unité (+0x04, bits 0..7), que la clé du verdict gardé
(`vd_key_of`) n'a pas bougé, que ce verdict était bon et texturé :

1. relevé des unités (`us_get`, mémoire TEXMEMO) ; texturage toujours allumé, aucune unité
   au-delà du device ;
2. unités désignées : `geom_texture_unit_ok` (le corps de boucle de `geom_texture_ok`, extrait
   tel quel) puis `texture_unit_ok` — mêmes refus, même téléversement ;
3. autres unités : `TexUnit` gardé, si le relevé y voit encore le même objet de GLEngine ;
4. `geom_format` refait (bon marché sous TEXMEMO) ; `va_gen_sizes` si le format ou les
   tableaux ont bougé ; puis `vd_store` et publication, comme le complet.

Tout autre cas (texturage qui change, refus d'une unité, unités d'image GLSL au-delà de 8,
rastérisation hors domaine) : verdict complet, inchangé. Pourquoi c'est exact : ce que le
complet lirait en plus est soit couvert par la clé (programmes, VAO, surface, image, époque
des textures — qui avance à toute création, destruction, modification ou éviction de
texture), soit signalé par un bit non neutre du bloc (liaisons, cibles, environnement,
combinaison), exactement le raisonnement de la liste blanche du lot 3, restreint aux unités.

**Contrôle** : sous `POMPPC_GL_VERDICTCHECK=1`, le verdict par unité est calculé, puis le
complet, et les deux comparés (ok, format, génériques, signature des `TexUnit`) ; lignes
`UNITVD écart` et compteurs de la ligne D3X. (Sous contrôle, avec plusieurs contextes dans
des fils différents, un `flush` qui relâche `G.mu` peut mêler deux comparaisons : la
mémoire du contrôle est globale.)

**Gain attendu** (estimation) : sur ~560 dispatches recalculés par image, refaire 3 unités au
lieu de ~7 dans deux passes et sauter la rastérisation, les programmes et les génériques :
**~3-8 ms/image sur le PC** (1-3 sur le M4). C'est le plus gros des trois, et le plus
incertain : le profil x86 dira la part de `geom_ok` + `texture_ok` dans le verdict.

## 4. Levier C — `gldUpdateDispatch` en une passe

`POMPPC_GL_DISPONE` (0/1), éteint par défaut.

**Constat.** À chaque dispatch (~1 300/image), trois sections sous `G.mu` : transmission
paresseuse, crochetage (`pomppc_hook_procs` : 36 cases, `install_for` et comparaison), verdict.
Quand Apple n'a pas été appelé (dispatch gardé, 99,9 % des cas en jeu), la table n'a pas
bougé et le crochetage ne change rien.

**Ce que fait `pomppc_hook_and_dispatch`.** Crochetage et verdict sous un seul verrou ; le
crochetage est sauté si Apple n'a pas été appelé et que la table est, case pour case, la
photographie prise après le dernier crochetage complet (`hook_snap`). C'est exact : après
`hook_locked`, chaque case vaut la nôtre ou est nulle sans repli, et `install_for` est figée
dès que l'état du device est connu — `hook_locked` est un point fixe. Toute autre écriture
dans la table (rattrapage paresseux, `gldInitDispatch`, Apple sous 0x80) rend la comparaison
fausse et le crochetage complet a lieu. Les deux `pomppc_log` de `gldUpdateDispatch` ne sont
plus appelés quand la trace est coupée (même sortie).

**Gain attendu** : un couple verrou/déverrou, un `find_ctx` et ~400 instructions par
dispatch, ~0,5-0,8 M instructions par image : **~0,5-1 ms/image sur le PC**.

## 5. Compteurs

Ligne `D3X image N : …` dans la note (`POMPPC_GL_NOTE`) toutes les 500 images, cumulée,
seulement si un levier est allumé, et `D3X <raison> N` au bilan d'un contexte détruit
(`gltest`). En tête de note : `idxlazy= idxvec= dispone= unitvd=`.

| champ | sens |
|---|---|
| `dessins balayés (indices, par AltiVec)` | `va_scan_idx` (contrôles compris) |
| `natifs sans balayage (indices)` | IDXLAZY pris |
| `balayés après coup` | IDXLAZY décidé, le natif s'est dérobé : l'empaquetage a balayé |
| `contrôle, écarts` | IDXLAZY=2 : plages que l'ancienne voie aurait refusées |
| `AltiVec contrôlés, écarts` | IDXVEC=2 |
| `crochetages sautés, complets` | DISPONE |
| `verdict par unité : repris (unités refaites), complets ; contrôle identiques, écarts` | UNITVD |

## 6. Plan de preuve (VM de dev, puis VM quotidienne)

1. Compiler dans la VM de dev (`tools/guest/cycle.sh`) : gcc-4.0 doit accepter
   `pomppc_vec.c` avec `-faltivec` (seul point non vérifié par la compilation de l'hôte, avec
   l'édition de liens).
2. `gltest`, 51 scènes, quatre modes : défaut ; `IDXLAZY=2 IDXVEC=2 DISPONE=1 UNITVD=1
   VERDICTCHECK=1` ; `IDXLAZY=1 IDXVEC=1 DISPONE=1 UNITVD=1` ; chaque levier seul. Attendu :
   md5 des images **identiques** au défaut, mêmes verdicts, `D3X` sans écart, `VERDICT`
   / `UNITVD` / `IDXVEC` / `IDXLAZY` à 0 écart.
3. DOOM 3 sous contrôle (`IDXLAZY=2 IDXVEC=2 UNITVD=1 DISPONE=1 VERDICTCHECK=1`), 5 000
   images : 0 écart ; relever la part des dessins sans balayage et des dispatches repris par
   unité.
4. Matrice **avec vidage** (`--env` des quatre variables à 1) sur `d3,prey,ut,mb,zen` (et
   `nx,cmr,wc3` sur le M4) : images justes (rejeu = VM = référence). Avec IDXLAZY, le flux
   d'un dessin sans balayage est identique octet pour octet : rejouer avec `tests/qgpu_replay.c`
   le vidage DOOM 3 des deux voies doit donner les mêmes images.
5. Autres jeux : UT2004 et Prey passent aussi par `DRAW_NATIVE` (type des indices d'UT2004 à
   relever : 16 bits → `vminuh`) ; Marble Blast et Zenerchi (Begin/End) ne voient que DISPONE
   et UNITVD ; Colin McRae (tableaux clients, `NATSHM`) balaie toujours (IDXVEC seul).

## 7. Plan d'A/B

Hôte au repos (aucune autre VM, charge relevée avant et après), même binaire QEMU, VM
quotidienne, DOOM 3 fenêtre, parties entrelacées ABBA, `matrice.py --sans-vidage --sample 10
--env …`, trois paires au moins :

| bras | variables |
|---|---|
| A | aucune (défaut) |
| B | `POMPPC_GL_IDXLAZY=1 POMPPC_GL_IDXVEC=1 POMPPC_GL_DISPONE=1 POMPPC_GL_UNITVD=1` |
| B1, B2, B3 | chaque levier seul (`UNITVD=1` ; `IDXLAZY=1 IDXVEC=1` ; `DISPONE=1`), si B gagne |

Puis Prey et UT2004 (A/B), et le `sample` de l'invité par bras (part de `pomppc_geom_dispatch`,
`geom_draw_client`, `va_scan_idx`, `pomppc_vec_minmax_u32`). Décision : passer les
`*_DEFAULT` à 1 levier par levier, si la matrice reste verte et l'A/B sans perte.

Gain total attendu sur le PC : **~6-12 ms/image sur ~150 (4-8 %)**, estimation à confirmer.

## 8. Pistes suivantes, non construites

- `geom_send_all` (~2,7 ms/image estimées sur le PC) : ne comparer les matrices que si le
  bloc a signalé un bit +0x08 depuis l'envoi (même méthode que les leviers, avec contrôle).
- Clé du verdict calculée deux fois par dessin (dispatch et dessin) et `TexInfo` (320 octets)
  recopié deux fois.
- `+set r_useIndexBuffers 1` dans DOOM 3 (§1) : rien à écrire, à mesurer.
- Attente de l'hôte (4,6 ms/image sur le PC) : côté QEMU (`nat_conv_attr`, TODO « [Cœur] »).
