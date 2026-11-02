# Zone froide arm64 : revue et preuve préparatoire du 02/11

Le portage de `tcg/0056` n'est pas implanté. Son garde x86-64 reste en place et
`TCGCOLD` reste sans effet sur le M4. Cette étude ferme la question du mécanisme
de branchement, pas celle du gain en jeu ni celle du portage complet.

## 1. Les entrées courtes concernent cinq familles

[D] QEMU **v11.1.2**, série POMPPC jusqu'à `0059` et `0064` (sources examinées
au dépôt `2a3d987`), `tcg/aarch64/tcg-target.c.inc` et `tcg/tcg.c` :

- lectures et écritures mémoire : `tcg_out_qemu_{ld,st}_slow_path` ;
- second niveau de `qemu_ld_tlbb` (`tlbb_kind == 2`) ;
- flottant scalaire : `tcg_out_ppc_fp32_slow_path` ;
- flottant vectoriel : `tcg_out_ppc_vfp_slow_path`.

Ces cinq callbacks corrigent leur entrée avec **`reloc_pc19`**. Les portes sont
des `B.cond`, mais aussi des **`CBNZ`** pour le scalaire : un portage limité au
test TLB manquerait les flottants. Le second niveau tlbb peut ajouter une
nouvelle étiquette mémoire pendant la finalisation ; la liste n'est pas figée.

[D] Dans cet encodeur, l'écart est en mots de quatre octets : le champ signé
19 bits couvre −1 Mio à +1 Mio−4 ; le champ 26 bits couvre −128 Mio à +128 Mio−4.
Le retour des callbacks utilise `tcg_out_goto`, qui attend aussi un saut direct
26 bits à portée. Les adresses de retour invitées doivent garder `lb->raddr`
dans le corps chaud, notamment pour les exceptions mémoire et FP différées.

## 2. Une région n'est pas toujours de 64 Mio

[D] `tcg/region.c`, `tcg_n_regions` : avec un seul fil, une seule région ; sinon
le nombre dépend de la taille du tampon et de `max_threads`. `0056` réserve son
dernier quart au froid. Les journaux M4 du 02/11 montrent des tampons de
**768, 832, 896, 960 et 1024 Mio**, `x-jit-near` réduisant l'allocation par pas
de 64 Mio selon la place disponible. Ce ne sont pas les tailles d'une région.

Pour `max_threads=2`, la formule donne 16 régions, donc environ 48–64 Mio par
région, moins la garde. Mais avec `max_threads=1`, une région peut approcher
le Gio : **un `B` 26 bits ne suffit alors pas**. Enlever le garde arm64 après
avoir seulement remplacé `reloc_pc19` serait incorrect. Le portage doit borner
la taille d'une région froide ou fournir un autre saut et prouver son ABI.

## 3. Les pools de constantes sont un second obstacle

[D] `tcg_out_movi` peut émettre un `LDR` littéral avec une relocation 19 bits
si une constante demande plus de deux instructions. `0056` restaure le pointeur
chaud avant `tcg_out_pool_finalize` : une référence émise dans le froid viserait
alors un pool chaud trop éloigné. `JITNEAR=1` rend certaines adresses faciles à
matérialiser avec `ADRP`, mais ne constitue pas une preuve pour toutes les
configurations et constantes.

[F — conception] Séparer les listes de pools chaud/froid et finaliser chacune
près de ses utilisateurs paraît approprié. Il faut sauvegarder/restaurer aussi
`data_gen_ptr`, gérer tous les retours d'erreur et inclure le pool froid dans le
vidage du cache d'instructions. À trancher par une implantation et ses preuves,
avec pointeurs hors de la fenêtre de 4 Gio, épuisement de région et exceptions.

## 4. Le mécanisme de porte longue fonctionne sur le M4

[A — hôte réel] `tools/tcg/coldbranchproof.py` extrait les fonctions **réelles**
`reloc_pc19` et `reloc_pc26` des sources de QEMU construites, les compile avec
une petite preuve C puis exécute les portes sur le processeur du Mac. Source
du backend SHA-256
`8fd5b4d71a5857569e315b7f1b979984e22141934b4c240fb41be90daaaced72`.

**36 cas exécutés sans écart**, vers des pages `MAP_JIT` à **±48 et ±96 Mio** :
portes originales `B.ne`, `B.hi`, `CBNZ` transformées en condition inverse
sautant un `B` long. Conditions zéro, un et bit 40 seul ; argument transmis
et retour au C conservés. L'encodeur 19 bits refuse ces destinations éloignées,
le 26 bits les atteint. Deux mutants sont détectés à l'exécution : mauvaise
inversion et test 32 bits au lieu de 64. Journal et sources mutées :
`bench/tcg/coldbranch-m4-0211/`.

```sh
python3 tools/tcg/coldbranchproof.py [QEMU_SRC]
```

La preuve utilise la même adresse virtuelle RW/RX avec la protection JIT par
fil de macOS. Elle ne teste pas un mapping séparé RW/RX, le gestionnaire de
régions, les pools ni le rétablissement d'un état invité en exception.

## 5. Implantation et fermeture restantes

[F — conception] Une porte inversée suivie de `B` conserve les opérandes sans
prendre de registre temporaire, mais change le sens de la branche rapide et
ajoute quatre octets par porte. Un talon proche garde le sens de la branche
mais demande une allocation chaude supplémentaire : comparer les coûts avant
de choisir. Toutes les familles du §1 et les retours froid→chaud sont à couvrir.

Preuves nécessaires : limites de portée et configurations SMP=1/2, chemins
mémoire/FP/tlbb, constantes éloignées, défauts mémoire et exceptions FP avec le
bon PC invité, saturation puis changement de région. Puis A/B entrelacé sur
**mb, zen, ut, d3, cmr**, hôte au repos, six parties ou témoin pour les écarts
sous 3 %. Le gain x86 de 1–2 % ne présume pas du gain M4 ; le profil du 02/11
n'établit pas un goulot de cache d'instructions comparable à celui du PC.
