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

## 9. Levier D — une soumission par image : `POMPPC_GL_ZONES` (20/10/2026)

**Constat (profil du 20/10, `bench/tcg/ab/x86-depart-2010/fast-{1,2}/d3-fen`).** Le fil
principal de DOOM 3 attend l'hôte **5,5 ms/image** (`wait_half_ex` → `qgpu_wait` →
`mach_msg_trap`) alors que le fil de rendu de l'hôte n'est occupé qu'à 19 % : 3,55 ms dans
`present_direct` (attente de la moitié précédente à l'échange) et 2,0 ms dans les `flush`
de `geom_draw_native`. La note dit **8 doorbells asynchrones par image**, exactement
(4 000 par 500 images), et `frames.csv` ~5,4 ms/image d'attente.

**Cause (vidages des 7/10 et du 4/10, `tools/re/dumpvidages.py`).** 7 des 8 soumissions
d'une image partent parce que la **zone des indices** (256 Kio par moitié) est pleine :
DOOM 3 recopie ~1,8 Mio d'indices 32 bits clients par image (un seul dessin en prend
jusqu'à 190 Kio), alors que le flux n'en fait que ~410 Kio, les sommets ~80 Kio et l'arène
~0,7 Mio sur les 6 Mio de la moitié. La 8e est l'échange. Or la v9 n'avait prévu **qu'une**
soumission par image : l'attente de l'échange devait porter sur l'image n−1, déjà finie.
Avec 8, `present_direct` attend la soumission qui vient de partir — la plus lourde de
l'image (≈ 500 dessins d'interaction) —, et chaque `switch_half` attend celle d'avant :
c'est de la latence (doorbell → fil hôte → barrière → kext), pas de la charge.

**Ce que fait le levier** (plugin seul, ni protocole ni kext) :

- `ZONES=1` : dans `geom_draw_native`, des indices clients qui ne tiennent plus dans leur
  zone vont dans **l'arène** de la même moitié si elle a la place (`QGPU_BUF_SHMEM`, offset
  absolu dans la tranche, comme la zone des indices). La place se constate verrou tenu, au
  même endroit que pour la zone des indices : aucun vidage ne peut s'intercaler entre les
  descripteurs, les indices et la commande. Un dessin dont les indices dépassent 256 Kio,
  qui retombait sur l'empaquetage, part en natif.
- `ZONES=2` : en plus, **flux de 512 Kio** au lieu de 256 (DOOM 3 en écrit ~410 Kio par
  image) ; sommets et indices glissent de 256 Kio, tailles inchangées, l'arène passe de 6 à
  5,75 Mio (encore 1280×1024×4 en un transfert ; la disposition n'est prise que si la
  moitié garde 5 Mio d'arène). La disposition est fixée une fois, à l'ouverture
  (`G.zl_cmd_bytes` ; `CMD_WORDS`, `VTX_OFF`… deviennent des lectures de `G`).
- `ZONES_FORCE=1` (épreuve) : TOUS les indices clients natifs passent par l'arène.

Attendu pour DOOM 3 : **1 soumission par image** (+ les sondes et relectures rares), plus
d'attente à l'échange ni au changement de moitié (l'image n−1 a eu toute la construction de
l'image n pour finir), ~6 appels au kext de moins par image. Gain attendu ~5-6 ms/image de
plugin, ~7 ms/image d'image (règle du §3.1 de `vitesse-doom3-x86.md`), **à mesurer**.

**Compteurs.** Ligne `VIDAGE image N` de la note (toutes les 500 images) : vidages par
image et, par ligne de l'appelant (`L<ligne>` pour `flush()`, `W<ligne>` pour un
`wait_half()` direct), le remplissage moyen du flux, de l'arène et des sommets au vidage,
les barrières réellement attendues, leur temps, et l'âge de la soumission attendue au début
de l'attente (latence vue = âge + attente) ; puis les dessins aux indices dans l'arène.
Ligne `ZONES` au bilan d'un contexte (gltest). `zones=` en tête de note.

**Preuves (VM de dev, RTX, 20/10).** Job `plugmd5` (nouveau : les scènes gltest dans
plusieurs modes, md5 comparés à l'octet) : **65 scènes × 4 modes (`ZONES=0`, 1, 2, 2 +
`ZONES_FORCE=1`), 0 écart** ; job `gpu` avec `ZONES_DEFAULT 2` : **56 OK, 0 échec**
(`qgpu_test` 0). Scène `d3idx`
(nouvelle) : le régime de DOOM 3 — sommets en VBO, indices 32 bits clients, 2 000 dessins
avec une matrice chacun, 12 Mio d'indices, mélange alpha — **53 soumissions sous ZONES=0, 7
sous ZONES=1, 8 sous ZONES=2**, image identique à l'octet dans les quatre modes.

**Reste à faire (VM quotidienne, en série, hôte au repos)** : DOOM 3 sous `ZONES=2` avec
vidage (rejeu = VM, `dumpvidages.py` doit dire ~1 soumission par image), ligne `VIDAGE`, et
A/B entrelacé contre `ZONES=0` ; UT2004 et Marble Blast (aucune régression). Défaut à passer
à 2 (`ZONES_DEFAULT`) si l'A/B gagne.

## 10. Le fil de DOOM 3 dans le plugin, compté à l'instruction (21/10/2026)

Révision `20261021-d3p`. Point de départ : 101,3 ms/image (d3-fen, binaire rapide, `ZONES=1`).
Profil `sample` de l'invité du 21/10 (VM quotidienne, 1 742 échantillons du fil principal,
`bench/tcg/ab/x86-prof-2110/apres-1/d3-fen/mesure/sample.txt` du dépôt principal, à
101,1 ms/image) : `gldUpdateDispatch` 13,3 ms/image inclusif (dont `geom_dispatch_locked`
1,7 propre, `us_get` 1,3, `geom_ok` 1,0, `vd_key_of` 0,9, `texture_unit_ok` 0,8,
`geom_format` 0,75, `geom_dispatch_publish` 0,7), `geom_draw_client` 17,8 (3,0 propre ;
`geom_draw_native` 4,8, `geom_send_all` 2,3, `send_state` 2,3, `prog_sync` 1,5), `memcmp` 1,5
propre (appelants : `changed()` de `geom_send_all`, `prog_sync`, la clé du verdict,
`send_state`), `pthread_mutex_*` + `__spin_*` ~3,4, `prog_state` 1,3. Rien n'atteint 5 % :
le plugin est un coût réparti.

### 10.1 L'outil : profil d'instructions exact (greffon TCG `hotblocks`)

Un `sample` à 1 ms ne sépare pas des postes de 0,3 ms. Ici : le greffon TCG
`tools/tcg/hotblocks.c` (compteur par bloc traduit) gagne l'option `snap=FICHIER` — un
`touch` côté hôte écrit un instantané cumulé `<out>.<n>` — et une case par (adresse, taille) :
le démarrage de Tiger épuisait les 262 144 cases et les blocs du plugin n'étaient plus comptés.
Le job `plugprof` (`BUILD=1` : kext, plugin, `gltest` en `/tmp/gltest-prof`, `nm` du plugin ;
sinon la scène seule) est encadré par deux instantanés ; `tools/re/hotplug.py AVANT APRÈS
NM NOTE [N] [--par K]` nomme les blocs (le plugin écrit en tête de note l'adresse de
`pomppc_geom_dispatch`, d'où le décalage de chargement) et donne les instructions invitées
exécutées par fonction, sans échantillonnage. À part : `libSystem` (`memcmp`…, adresses
fixes de Tiger), la page commune (`memcpy`), les appels par talon (`__picsymbolstub1`,
`llvm-objdump --macho --indirect-symbols`) ; `llvm-objdump-21 --macho -d` désassemble le
plugin pour annoter un bloc chaud instruction par instruction.

```
QEMU_EXTRA="-plugin $PWD/tools/tcg/libhotblocks.so,out=$H/hb.csv,snap=$H/go" \
  DEVDISK=…/tiger-dev-plugin.raw python3 tools/guest/devloop.py start
touch $H/go ; devloop.py run <job plugprof> ; touch $H/go
python3 tools/re/hotplug.py $H/hb.csv.1 $H/hb.csv.2 nm.txt note-1.txt 40 --par 2120
```

(`QEMU_SRC=~/src/qemu bash tools/tcg/build.sh hotblocks` construit le greffon.)

Scène **`d3inter`** (nouvelle, `gltest`) : la boucle ARB2 d'idTech 4 réduite — par lumière,
volumes d'ombre (programme de sommets seul, faces alternées, couleur masquée) puis
interactions (programmes de sommets et de fragments, unités 1, 4, 5 reliées à chaque
surface, unités 0, 2, 3, 6 par lumière, env de sommets et de fragments, pointeurs génériques
dans un VBO, indices 32 bits clients, une matrice par entité), précédées d'un remplissage
au pipeline fixe qui relie l'unité 0 à chaque surface ; mélange additif (image comparée à
l'octet). 2 120 dessins, 2 124 dispatches : 1 626 refaits par unité, 31 complets, le reste
court-circuité (DOOM 3 : 410 / 148 / 736 par image — la scène charge plus le verdict par
unité ; c'est un banc, pas un DOOM 3).

Profil de `d3inter` avant ce lot : **15 174 instructions du plugin par dessin**, plus
~1 400 dans `memcmp` (21 appels par dessin) et ~300 dans `pthread_mutex_*` (3 couples par
dessin). Premiers postes : `send_state` + `state_units` 2 000, `geom_draw_client` 1 000,
`geom_dispatch_locked` 920, `geom_draw_native` 835, `geom_format` 640, `vd_key_of` 530,
`prog_state` + `prog_parse` + `pprog_find` ~790, `geom_send_all` 475, `us_get` + `us_fill`
810, le crochetage sauté de DISPONE 320 (36 cases comparées) ; et **~10 % en appels de petits
utilitaires** (`wl_extra` 280, `to_u8` 220, `put_f` 215, `unit_mask` 195, `gctx_of` 180,
`us_fp`, `tex_cp`, `gls`, `va_enabled`, `unit_slot`…) : gcc 4.0 à `-O2` n'inline pas une
fonction statique qui n'est pas déclarée `inline`.

### 10.2 Les leviers

Tous exacts par construction, chacun derrière une variable (allumée par défaut depuis le
21/10, `*_DEFAULT 1`, après l'A/B du §10.5 ; `=0` la coupe), sauf HOTINLINE (compilation, allumé). Aucun ne touche `qgpu_proto.h` ni
`qgpu_abi.h` : ni QEMU ni kext à reconstruire. Gains : instructions par dessin de `d3inter`,
chaque levier seul.

| levier | variable | ce qu'il fait | gain |
|---|---|---|---|
| HOTINLINE | défaut **1** ; `-DHOTINLINE_DEFAULT=0` l'éteint | `static inline` (macro `HOT`) sur 16 utilitaires chauds — `gls`, `gctx_of`, `sw_color`, `unit_slot`, `unit_mask`, `us_fp`, `va_enabled`, `va_bpc`, `wl_extra`, `array_switch`, `geom_switch`, `self_thr`, `changed`, `put_f`, `clamp01`, `to_u8` — et le cas courant de `tex_cp` (déjà relevé cette image ; le relevé passe dans `tex_cp_fill`). Aucune arithmétique flottante déplacée (`clamp01`, `put_f` comparent ; `to_u8` garde son expression) | plugin −1 200 (−7,9 %) |
| 1. dispatch | `POMPPC_GL_DISPFAST=1` (2 = contrôle) | bloc classé en **une** passe (`df_classify` : `st_neutral`, `wl_neutral` et la condition de bloc d'`uv_take`, `wl_extra` sorti de la boucle) ; dans le verdict par unité, la clé calculée par `uv_take` est reprise par `vd_store_k` si ses 7 mots de tête (image, époque des textures, état du device, contexte perdu, `qctx`, `cfg`, état GL) n'ont pas bougé — le reste est lu dans GLEngine et dans les programmes, que le verdict ne touche pas (fil de l'application, verrou tenu) ; `geom_format` prend le relevé des unités une fois au lieu d'un `us_get` par unité | plugin −1 020 |
| 2. programmes | `POMPPC_GL_PROGMEMO=1` | `prog_state` (4 à 5 fois par dessin : clé au dispatch et au dessin, `prog_sync`, `texture_ok`) cherchait chaque fois l'objet programme dans les 256 entrées de `pprog` ; la réponse est gardée par étage pour (génération de la table, objet GLEngine) — `pprog_gen` avance à `pp_create`, `pp_destroy` et à la mort d'un contexte, seuls lieux où un couple (ctx, obj) change. `prog_parse` est toujours appelé | plugin −150 (3 programmes dans la scène ; DOOM 3 en a davantage, plus loin dans la table) |
| 3. matrices | `POMPPC_GL_MTXDIRTY=1` (2 = contrôle) | `geom_send_all` ne compare modèle-vue, projection, matrices de texture et fenêtre/plage de profondeur (64 + 64 + 64·u + 16 + 8 octets, à chaque dessin) que si un dispatch a signalé leur bit du mot +0x08 depuis la dernière comparaison (`md_bits` ; textures : `md_tex`, un bit par unité, posé pour toutes à chaque bit 0x00ff0000, effacé à la comparaison de l'unité — une unité qui s'allume plus tard compare encore) ; tout à comparer après `invalidate_mirrors_from` et avant le premier envoi. Une comparaison faite laisse le cache égal à l'état : sans bit, elle redirait « inchangé » | `memcmp` −300 |
| 4. état | `POMPPC_GL_STUNIT=1` (2 = contrôle) | dans `send_state_blk`, quand le verdict diffère de la dernière série : unités changées relevées mot à mot (`su_diff`, remplace le `memcmp` de 320 octets), clés refaites pour ces seules unités (`state_unit_one`), comparées dans l'ordre de la liste chaude (masque de positions parcouru par `cntlzw`) : même flux que la voie complète. Une unité au `TexUnit` inchangé (même époque) redonnerait les clés déjà dans `p->st` — l'hypothèse de la reprise « même verdict » d'avant, restreinte à l'unité | plugin −560, page commune −115 |
| 5. verrou | `POMPPC_GL_DISPLOCK=1` | `gldUpdateDispatch` courant (transmission paresseuse prise, pas de 0x80, DISPONE allumé) : `pomppc_lazy_update` puis crochetage et verdict sous **un** verrou (`pomppc_update_dispatch_fast`, `hook_and_dispatch_locked`) — un couple `pthread_mutex_lock/unlock` (atomiques `lwarx`/`stwcx.`, chers sous TCG) et un `find_ctx` de moins par dispatch | −160 (davantage en temps : `pthread_mutex_*` + `__spin_*` ~3,4 ms/image dans le `sample` de DOOM 3, pour 3 couples par dessin) |

**Tout allumé, `d3inter`** : plugin **15 174 → 12 205** instructions par dessin (−19,6 %,
dont HOTINLINE −1 200 et les variables −1 820), `memcmp` 21 → 17 appels par dessin ;
plugin + `libSystem` + page commune 26 650 → 23 045 (−3 600 par dessin ; dans ce qui reste,
~7 000 sont l'écriture de l'image et des journaux par `gltest`, hors plugin). Pour DOOM 3,
ordre de grandeur seulement : le plugin y pèse ~37 ms/image, dont ~25 dans ce que ces leviers
touchent ; −15 à −20 % de ce poste ferait **~4 à 7 ms/image** (dont ~1 ms de verrous), ×1,3
sur l'image — à mesurer, la répartition des dispatches de DOOM 3 n'est pas celle de la scène.

### 10.3 Preuves (VM de dev `tiger-dev-plugin.raw`, RTX, 21/10)

- `plugmd5`, **66 scènes** (les 65 d'avant + `d3inter`), images comparées à l'octet :
  modes `ref`, `all` (`DISPFAST PROGMEMO MTXDIRTY STUNIT`), `chk` (mêmes leviers en
  contrôle + `VERDICTCHECK=1`), `dfpm`, `mdsu` : **0 écart** ; puis `ref`, `dl`
  (`DISPLOCK`), `all` (les cinq), `chk2` (`DISPFAST=2 MTXDIRTY=2 STUNIT=2 PROGMEMO=1
  DISPLOCK=1`, sans `VERDICTCHECK` qui détourne le verdict par unité) : **0 écart**.
  Compteurs de contrôle (ligne `D3P` de la note) sur l'ensemble des scènes : 2 639 clés
  reprises recalculées, **0 écart** ; 19 260 comparaisons de matrices sautées, **0
  changement non signalé** par le bloc ; 3 263 séries d'unités partielles comparées à la
  complète, **0 écart** ; 6 222 dispatches sous un verrou. (Les scènes `arbvp`…`glsldp`
  rendent 1 dans tous les modes, défaut compris : témoins pensés pour 64×64, pas un écart.)
- HOTINLINE : `plugmd5` (66 scènes) avec un plugin compilé `-DHOTINLINE_DEFAULT=0`
  (`EXTRA_CFLAGS`, nouveau dans le `Makefile`) : md5 **identiques** au plugin inliné.
- Job `gpu` : défaut **56 OK, 0 échec** (`qgpu_test` 0) ; les cinq leviers allumés
  (variables exportées au job) **56 OK, 0 échec**.
- Rejeu : sans objet — aucun levier ne change le flux (`STUNIT` garde l'ordre des
  `SET_STATE`, les autres ne touchent pas aux commandes émises).

### 10.4 A/B sur la VM quotidienne (à jouer)

Installer le plugin `20261021-d3p` (bundle construit dans la VM de dev :
`bench/d3plug-2110/GLDriver-POMPPC-20261021.bundle.tar` du worktree), d'abord une partie
sous contrôle :

```
tools/tcg/matab.sh d3p-chk 1 "chk:QEMU_FAST=1 POMPPC_GL_DISPFAST=2 POMPPC_GL_PROGMEMO=1 POMPPC_GL_MTXDIRTY=2 POMPPC_GL_STUNIT=2 POMPPC_GL_DISPLOCK=1" d3 fen
```

(ligne `D3P image N` de la note toutes les 500 images : écarts à 0), puis l'A/B :

```
tools/tcg/matab.sh d3p 3 "ref:QEMU_FAST=1" "x:QEMU_FAST=1 POMPPC_GL_DISPFAST=1 POMPPC_GL_PROGMEMO=1 POMPPC_GL_MTXDIRTY=1 POMPPC_GL_STUNIT=1 POMPPC_GL_DISPLOCK=1" d3 fen
```

Les deux bras ont HOTINLINE (compilé) ; son effet seul se lit en échangeant le bundle
(`GLDriver-POMPPC-20261021-sansinline.bundle.tar`, même source, `-DHOTINLINE_DEFAULT=0`).
Puis UT2004 et Marble Blast (MTXDIRTY et DISPLOCK les touchent aussi). Défauts à passer à 1
levier par levier.

### 10.5 Ce qui reste

- **Verdicts complets de DOOM 3** (~148 par image, `geom_ok` + `texture_ok` ~1,6 ms) : les
  changements de programmes (ombres ↔ interactions), dont le bit du bloc n'est pas neutre et
  qui changent la clé. Piste : deux verdicts gardés (le courant et le précédent, avec le OU
  des blocs reçus depuis) — l'interaction suivante repartirait du verdict d'interaction
  précédent, par unités. Gain estimé ~0,5 ms/image (toutes les unités restent à refaire), non
  construit.
- **`geom_draw_client`** (3 ms propre dans DOOM 3) : boucles sur les 13 + 16 + 8 + 16
  attributs possibles à chaque dessin (plan et sources) ; un parcours des seuls bits du format
  (`cntlzw`) les réduirait.
- **Recopie des indices** (`__memcpy` 2,9 ms/image, ~1,8 Mio) et **levier 5** (indices en VBO
  d'éléments, `r_useIndexBuffers 1`) : regardé, pas construit. Servir par `DRAW_NATIVE` un VBO
  d'éléments réécrit à chaque image supprimerait le balayage, pas la recopie (les
  `BUF_SUBDATA` du jeu la feraient à la place de la zone des indices) ; le repli `NATSHM` de
  `vitesse-doom3-x86.md` §5.6 (miroir d'éléments sale à chaque image) est à comprendre
  d'abord.
- Le crochetage sauté de DISPONE compare encore 36 cases par dispatch (~320 instructions).

### 10.5 Résultats (VM quotidienne, PC, 21/10, hôte au repos, binaire rapide PGO)

Contrôle (`d3p-chk`, 2 500 images de DOOM 3) : `dispfast 2` 84 197 clés reprises, `mtxdirty 2`
1 201 504 comparaisons sautées, `stunit 2` 129 133 séries contrôlées, **0 écart** partout.
A/B en fenêtre, 3 parties par bras, plugin `20261021-d3p` (HOTINLINE dans les deux bras),
médianes en ms/image :

| jeu | sans les 5 variables | avec | écart |
|---|---|---|---|
| DOOM 3 (`d3p-d3`) | 99,3 | 95,5 | **−3,8 %** |
| UT2004 (`d3p-ut`) | 46,2 | 46,7 | +1,1 % (dispersion 6-7 %) : neutre |
| Marble Blast (`d3p-mb`) | 14,4 | 13,6 | **−5,6 %** |

Les cinq variables passent à 1 par défaut (révision suivante du plugin).
