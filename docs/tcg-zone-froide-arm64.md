# Zone froide arm64 : revue, portage 0066 et preuves du 02/11

La revue de fin de nuit ci-dessous précède le portage. À la demande de
l'utilisateur, **`tcg/0066` est implanté dans la séance du matin**, validé dans
l’invité (§6). `TCGCOLD=1` agit désormais sur arm64 en émulation système ;
les preuves sont closes (§6) et la matrice des cinq jeux est terminée (§7).
Le défaut du M4 reste éteint : aucun gain générique établi.

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

## 5. Décision de fin de revue, avant le portage 0066

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

## 6. Portage 0066 du matin : relais chauds et pools séparés

[D — code] QEMU v11.1.2, série POMPPC et `tcg/0066-tcg-cold-arm64.patch` :

- Chaque branche conditionnelle d'entrée garde son sens et sa largeur. Un
  relais `B` est ajouté en fin du corps chaud ; la branche d'origine le vise
  en imm19, puis le relais vise le froid en imm26. Le chemin rapide ne traverse
  aucune instruction supplémentaire. Les cinq callbacks utilisent `reloc_ldst`,
  qui distingue le relais long des branches courtes créées dans le froid par tlbb.
- `raddr` reste dans le corps chaud. Les adresses transmises aux helpers mémoire
  et FP restent celles d'avant ; un relais ne prend aucun registre temporaire
  et n'écrit pas LR.
- Les régions arm64 froides ont une taille nominale au plus égale à 64 Mio,
  y compris en SMP=1. Leurs bornes réelles (reliquat d'alignement inclus) sont
  vérifiées strictement sous 128 Mio. Le mode utilisateur et TCI arm64 restent
  exclus ; le garde x86-64 de 0056 reste valide.
- Les pools froids sont finalisés avant le retour au chaud, avec leur propre
  liste et leur propre `data_gen_ptr`. Le curseur, la liste et la borne chauds
  sont rétablis sur tous les retours du finaliseur froid. Un épuisement du froid
  ou des relais marque la région pleine ; un échec de relocation réduit le TB
  suivant la procédure de QEMU. Le vidage du cache inclut code et pool froids.

[A — hôte] `tools/tcg/coldzoneproof.py --mutants` extrait les fonctions du
backend, le finaliseur et la règle de région de l'arbre construit. **53 cas
réussis** : 48 exécutions natives des portes (chemins rapides et lents, arguments 64 bits) et accès aux pools à
±48/96 Mio (`B.ne`, `B.hi`, `CBNZ` 64 et 32 bits ; zéro, un, bit 40), et cinq
retours d'erreur. Les pools chaud/froid restent distincts ; les pointeurs
chauds sont restaurés et aucun froid en échec n'est validé. Cinq mutants sont
détectés : BL au lieu de B, portée courte, pools mélangés, curseur chaud perdu,
région SMP=1 trop grande. Journal `.run/coldzoneproof-0211.log`.

Le harnais teste aussi l'arithmétique des alias RX/RW et le découpage SMP=1.
L'exécution native utilise MAP_JIT à adresse RW/RX identique ; elle ne prétend
pas exécuter deux mappings distincts. Les callbacks mémoire/FP y sont remplacés
par un émetteur de contrôle : **les preuves invitées restent nécessaires**.
Le patch appliqué à la copie avant donne quatre fichiers identiques à ceux de
l'arbre construit.

**Validation invitée terminée** sur une copie autonome privée de la base
d'endurance (`bench/tcg/cold-arm64-0211/invite/`), témoins froid éteint puis
allumé, sorties identiques :

- SMP=1, JIT placé à `0x7000000000`, loin du texte, tampon 1 Gio ;
- SMP=2, tampon 64 Mio, **191 renouvellements du tampon dans le témoin et 137
  avec le froid**. Le premier démarrage témoin à 8 Mio a manqué le délai SSH ;
  son dossier interrompu est conservé, sans être compté comme une réussite.

Tests flottants simples/doubles, programmes aléatoires et comparaisons,
vecteurs, fautes mémoire au PC/DAR exact (**500 984 cas tlbb, 309 224 fautes**),
mémoire protégée, lmw, dcbz hors réservation et code réécrit. Les vérificateurs
FP/vectoriels, LMW et DCBZ restent actifs. Le témoin SMP=2 relève **102
signalements TLB sans PTE, zéro autre page** ; le froid n'en relève aucun dans
ce passage. [F] La cause de ces signalements du témoin reste ouverte, comme
les observations antérieures de `tcg-g4.md` §35/44/45 : ni régression attribuée
au froid, ni preuve qu'ils seraient faux. Trancher par une trace ordonnée des
écritures de PTE et des invalidations sur les deux vCPU ; si le contrat reste
ambigu, rejouer le cas minimal sur un G4 réel. Les sorties et compteurs bruts sont
conservés. Aucun écart aux tests C. Les trois images qcow2 contrôlées après les
essais sont sans erreur. Aucune vitesse n'est déduite de ces essais instrumentés.

Le premier témoin SMP=1 avec `RETVERIFY=1`, **sans zone froide**, a donné
44 signalements du cache de retours (`complet 0x0`, traduction absente), point
« sans traduction » déjà ouvert dans le TODO. Aucun autre vérificateur n'y
signale de divergence. Relevé conservé dans `invite-temoin-retverify/` et
`temoin-retverify.json`. [F] Cause à instruire ; ces signalements ne sont pas
attribués au portage ni déclarés faux. Trancher par un cas réduit qui remappe
une page de code, avec journal du cache de retours (mmu_idx, époque, TB),
traduction complète et témoin `RETINLINE=0`. La comparaison est rejouée avec ce
vérificateur éteint dans les deux bras ; les tests invités continuent de
contrôler les fautes au PC exact et les autres vérificateurs restent actifs.


### Exceptions flottantes au PC invité exact

Le contrôle `tools/guest/jobs/fptest/fpexctest.c` est compilé avec GCC dans
Tiger, puis rejoué sans et avec le froid : SMP=1/JIT loin du texte, puis
SMP=2/tampon 64 Mio. **640 exceptions par configuration, 2 560 au total,
zéro erreur, sorties identiques** (`exceptions-fp-2/` dans le dossier de preuve).
Cinq opérations : fadds/fadd sur NaN signalant, fdivs par zéro, fcmpo sur NaN
calme et frsp sur NaN signalant. Chaque SIGFPE contrôle le PC contre
l'étiquette de l'instruction ; les FPSCR et codes du signal sont aussi conservés.
Ce contrôle de non-régression dans l'émulateur n'est pas une mesure sur G4 réel.

[B — logiciel] Apple **xnu-792.24.17** :
[`sigreturn`](https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/bsd/dev/ppc/unix_signal.c)
réactive les exceptions précises pour un fil avec gestionnaire SIGFPE ;
[`thread_enable_fpe`, dans status.c](https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/ppc/status.c)
pose FE0/FE1. Un retour de SIGUSR1 arme le test. Le même `status.c` exporte
**MSR_EXPORT_MASK_SET** dans `PPC_THREAD_STATE`, et masque donc FE0/FE1 :
le `msr_fe=000` du contexte du signal ne décrit pas le MSR réel. La première
assertion du harnais sur ces bits a été retirée sur cette preuve primaire ;
le contrôle des signaux et du PC est conservé. La campagne finale utilise une
copie figée du source pour les quatre configurations. Les premières tentatives
d'assemblage et d'assertion du masque sont conservées, sans être comptées
comme des réussites ni attribuées au portage.

## 7. Matrice finale et décision

[A — hôte] Trois passages par bras avec témoin identique, quatre jeux en
fenêtre et CMR en course 1920×1080 : **60 cellules sans anomalie d'exécution
ni repli**, charge valide. Protocole, tableau complet et données dans
[`re/ut-attente-m4.md`, §7](re/ut-attente-m4.md#7-matrice-finale-close-à-13-h-28--résultat-et-décision).
Six preuves d'image séparées avec 0066 + attente progressive sont vertes.

La comparaison propre au froid est `POLLNS=3 TCGCOLD=1` contre
`POLLNS=3 TCGCOLD=0` : médianes mb −1,03 %, zen −1,08 %, d3 −0,34 %,
ut −0,78 %, cmr +1,41 %. Petits déplacements, recouvrement des échantillons ;
**aucun gain générique établi**, et aucune conclusion sur le froid seul sans
attente progressive. L'ensemble contre la référence fait zen −10,06 %,
ut +0,23 %, d3 +1,09 %, mb −0,76 %, cmr +0,65 %.

**Portage terminé, option conservée éteinte par défaut sur le M4** ; activation
explicite `TCGCOLD=1`. Le gain x86-64 ne suffit pas à l'allumer sur arm64.
Le finaliseur générique de pools change aussi sur x86-64 : reconstruire
`build/` et `build-fast/`, puis valider sur le PC à son retour. Aucun test
Linux de cette séance n'est revendiqué.

## Le finaliseur de pools sur x86-64 (PC, 04/11/2026)

[A — hôte] `tcg/0066` modifie le finaliseur générique de pools, commun aux deux hôtes : les preuves natives du M4 ne
valaient pas pour le PC. `tools/tcg/froid-x86-proof.sh` joue les suites invitées sans et avec `x-tcg-cold`, binaire rapide
de la série `f9490f65c5ffe1c7`, en SMP=1 puis en SMP=2 sur tampon de 64 Mio, `x-vfp-native-verify`, `x-vmx-verify` et
`x-tb-verify` allumés (`bench/tcg/froid-x86-0411/`) :

- **Sorties identiques entre témoin et froid dans les deux configurations**, hors temps imprimés par `jctest` et `tbtest` :
  vfp, vmx, lmw, tb, smc, jc, tlbb, fp simple, double, comparaisons et programmes aléatoires, exceptions FP.
- tlbb : 519 984 essais, 309 224 fautes, 0 erreur, même empreinte `028069d6e50d8d0b` dans les quatre bras.
- **Exceptions FP : 640 cas par bras, 2 560 au total, 0 erreur** au PC exact.
- 0 divergence aux vérificateurs. `x-ret-verify` et `TLBPVERIFY` n'étaient pas allumés.

**Limites.** Les renouvellements de tampon n'ont pas été comptés, à la différence du M4 : `tb-size=64` ne prouve pas qu'il y
en a eu sur ce parcours. Aucune vitesse n'est à tirer de ces essais instrumentés.
