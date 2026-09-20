# Flottant rapide — confier le FPU PowerPC au FPU de l'hôte

`./run_tiger.sh` — **allumé par défaut.** `FASTFP=0` pour le flottant exact.

Les jeux (Marble Blast, Zenerchi) passent l'essentiel de leur temps dans du
flottant : décodage Ogg Vorbis, physique. Or dans QEMU 9.2 la cible PowerPC
n'utilise **jamais** le FPU de la machine hôte : chaque `fadds`, chaque `fmuls`
est recalculé bit à bit en logiciel. Ce document décrit le mode qui lève cette
limite, ce qu'il garantit, ce qu'il sacrifie, et comment ça a été prouvé.

Deux patches, appliqués par `scripts/build_qemu_qfb.sh` et gardés séparés pour
qu'un A/B puisse attribuer les gains :

| patch | ce qu'il fait | change le comportement ? |
| --- | --- | --- |
| `patches/fastfp/0001-ppc-fast-fp.patch` | le mode rapide lui-même (propriété `x-fast-fp`) | oui, quand il est **allumé** : FI et FX (§ 3) |
| `patches/fastfp/0002-ppc-fewer-fp-helpers.patch` | 4 appels de helper par instruction flottante → 2 | **non**, dans aucun des deux modes |

`NO_FASTFP2=1 ./scripts/build_qemu_qfb.sh` sur un arbre QEMU propre applique le
0001 seul.

---

## 1. Pourquoi PowerPC n'avait pas droit au FPU de l'hôte

QEMU sait déjà faire : c'est le mécanisme **hardfloat** de `fpu/softfloat.c`,
ajouté en 2018. L'idée est de calculer sur le FPU hôte *et* de savoir quels
drapeaux IEEE poser sans jamais lire le registre d'état du FPU hôte (trop
lent). L'astuce qui rend ça possible :

> le drapeau **inexact** est levé par la quasi-totalité des opérations
> flottantes, et les invités ne l'effacent presque jamais. Donc **si inexact
> est déjà posé**, l'opération n'a plus rien à signaler : on peut la faire sur
> le FPU hôte sans regarder ses drapeaux.

D'où le test central, `can_use_fpu()` : hardfloat ne s'enclenche que si
`float_flag_inexact` est **déjà** dans `float_status`, et si l'arrondi est au
plus proche pair.

PowerPC casse exactement cette hypothèse. `FPSCR[FI]` (« le résultat de la
*dernière* opération était inexact ») n'est pas collant : il doit refléter la
dernière instruction et rien d'autre. QEMU l'implémente de la façon la plus
directe — remettre les drapeaux softfloat à zéro avant chaque instruction
flottante (`helper_reset_fpstatus`) et lire l'inexact après. Résultat :
`can_use_fpu()` est toujours faux. Le fichier en tirait la conclusion qui
s'impose :

```c
#if defined(TARGET_PPC) || defined(__FAST_MATH__)
# define QEMU_NO_HARDFLOAT 1
```

Deuxième trou, indépendant du premier : les instructions **simple précision**
de PowerPC (`fadds`, `fsubs`, `fmuls`, `fdivs`, `fsqrts`, `fmadds`…) passent
par les fonctions `float64r32_*` de softfloat — opérandes float64, résultat
arrondi à la précision float32 puis re-rangé en float64. Ces fonctions n'ont
**aucun** chemin hardfloat, même sur les cibles où hardfloat est actif. Et
c'est précisément ce que produit le code compilé en `float` : `lfs` charge un
simple, `fmuls`/`fadds` calculent, `stfs` range.

---

## 2. La conception

Quatre morceaux, dans l'ordre où ils se déclenchent.

### 2.1 Le veto devient une donnée, pas une constante de compilation

`QEMU_NO_HARDFLOAT` n'est plus forcé à 1 pour `TARGET_PPC`. À la place,
`float_status` gagne un champ `no_hardfloat`, et `can_use_fpu()` le respecte.
PowerPC pose `no_hardfloat = 1` sur `env->fp_status` **et** sur
`env->vec_status` au reset, sauf si la propriété `x-fast-fp` est allumée.

En mode exact, hardfloat est donc **inaccessible**, pas seulement improbable :
le comportement est celui d'aujourd'hui, par construction et non par accident.

`can_use_fpu()` refuse en plus `rebias_overflow` / `rebias_underflow`. Ces deux
modes (que PowerPC arme quand `FPSCR[OE]` / `FPSCR[UE]` sont mis) font renvoyer
au logiciel un résultat *re-biaisé* au lieu d'un infini ou d'un dénormal — le
FPU hôte ne sait pas faire ça. C'est un garde-fou nécessaire : sans lui, une
instruction comme `frsqrtes` (qui enchaîne `float64_sqrt` puis
`float64r32_div` **sans** remise à zéro entre les deux) pourrait emprunter le
chemin rapide avec le re-biaisage armé.

### 2.2 Chemins rapides exacts pour `float64r32_*`

`float64r32_{add,sub,mul,div,sqrt,muladd}` gagnent un chemin hardfloat, sur le
modèle de `float32_gen2`, sous une condition supplémentaire :

> **chaque opérande doit être exactement un `float32` « zéro ou normal ».**

Test fait sur les bits, sans ambiguïté :

```c
exposant == 0        -> mantisse nulle (±0 ; les dénormaux float64 sont exclus)
sinon                -> 29 bits bas de mantisse nuls,  0x381 <= exposant <= 0x47e
```

`0x381 = 1023-126` et `0x47e = 1023+127` : la plage *normale* de float32. Ça
exclut d'office Inf, NaN, les dénormaux float64 et tout ce qui deviendrait un
dénormal float32.

Sous cette condition, l'opération `float64r32` **est** une opération float32
unique : l'arrondi correct du résultat exact vers 24 bits. Le FPU hôte la fait
en une instruction, et l'élargissement du résultat float32 en float64 est
exact. Le résultat est donc, au bit près, celui du chemin logiciel — ce n'est
pas une approximation, c'est la même fonction mathématique.

Les garde-fous de `float32_gen2` sont conservés : résultat infini → on pose
`float_flag_overflow` ; résultat de module ≤ `FLT_MIN` → retour au logiciel
(c'est lui qui gère dénormaux, underflow et flush-to-zero). `muladd` utilise
`fmaf()` avec les drapeaux `float_muladd_negate_*`, exactement comme le fait
déjà `float32_muladd`, et respecte `force_soft_fma`.

Sur la **contraction** : le danger serait qu'un `a*b` suivi d'un `+c` soit fusionné
par le compilateur en une FMA (une seule arrondi au lieu de deux). Il n'existe
pas ici : chaque chemin rapide ne fait qu'**une** opération arithmétique, et la
multiplication-addition passe par `fmaf()` explicite — jamais par `a*b+c`.
C'est la même protection que celle dont softfloat.c se contente déjà (des
fonctions `hard_f32_add` / `hard_f32_mul` distinctes, jamais composées).
Le build de référence n'emploie de toute façon ni `-march=native` ni
`-ffast-math` (`-ffast-math` réarme `QEMU_NO_HARDFLOAT` avec un `#warning`).

### 2.3 Amorçage : la seule déviation architecturale

C'est le cœur du mode. En mode rapide, `helper_reset_fpstatus()` pose
`float_flag_inexact` **au lieu de 0**, mais seulement quand :

| condition | pourquoi |
| --- | --- |
| `FPSCR[XX] == 1` | XX est **collant**. Tant qu'il vaut 0, tout passe par le logiciel, et XX est donc posé *exactement*, au premier résultat réellement inexact. Une fois à 1, aucune transition 0→1 ne peut plus être manquée. |
| `FPSCR[XE] == 0` | aucune trappe « inexact » ne peut être due. |
| `FPSCR[OE] == 0` et `FPSCR[UE] == 0` | sinon softfloat re-biaise les débordements (§ 2.1). |

Conséquence : un programme qui efface XX (`mtfsf`, `mtfsb0`, `mtfsfi`,
`mcrfs`) **retombe automatiquement** en logiciel exact jusqu'au prochain
résultat inexact. L'amorçage se ré-arme tout seul, et se désarme tout seul.

Côté sortie, `do_float_check_status()` doit savoir que l'inexact qu'il lit ne
veut rien dire : quand l'opération était amorcée, il **ne rappelle pas**
`float_inexact_excp()`. C'est correct pour XX (déjà à 1) et pour FEX (XE = 0),
et c'est **plus** juste que le mode exact pour FX : le matériel réel ne pose FX
que sur une transition 0→1 d'un bit d'exception, alors que
`float_inexact_excp()` de QEMU pose FX à chaque inexact.

`FPSCR[FI]` vaut alors **1 en permanence** (valeur constante documentée, la
moins fausse : la très grande majorité des opérations d'un programme flottant
*sont* inexactes). Les instructions qui doivent explicitement ne pas signaler
l'inexact (`frin`/`friz`/`frip`/`frim`, `xsrqpi`) effacent le drapeau
elles-mêmes avant le contrôle : elles continuent de mettre FI à 0 et de ne pas
toucher XX, dans les deux modes.

`FPSCR[FR]` (« fraction arrondie ») n'est **pas modélisé** par QEMU, ni avant
ni après ce patch : aucun chemin ne le pose, seuls les traitements
d'opération invalide l'effacent. Il vaut 0 dans les deux modes.

### 2.4 AltiVec

`env->vec_status` est mis sous la **même** propriété (`no_hardfloat`).

Techniquement, AltiVec n'aurait pas besoin d'amorçage : `VSCR` n'a aucun
drapeau d'exception flottante, rien dans `target/ppc` ne lit
`vec_status.float_exception_flags`, et rien ne les efface — l'inexact s'y
accumulerait donc tout seul dès la première opération vectorielle inexacte, et
hardfloat s'enclencherait ensuite en permanence. Le mode NJ (flush-to-zero) est
géré par hardfloat lui-même : `float32_input_flush2()` avant le test d'entrée,
et tout résultat de module ≤ `FLT_MIN` repart au logiciel.

Ça a été mis sous la propriété quand même, pour deux raisons : garder
« mode exact = exactement le binaire d'aujourd'hui » vrai *par construction* et
vérifiable d'un coup d'œil, et rendre l'A/B propre (tout le gain d'un côté,
rien de l'autre).

### 2.5 Patch 0002 : moins d'appels de helper (orthogonal, sans effet observable)

Une instruction flottante « classique » de PowerPC coûtait **quatre** appels de
helper : `reset_fpstatus`, l'opération, `compute_fprf`, `float_check_status`.
Le second patch en supprime deux, sans rien changer d'autre :

* **`reset_fpstatus` émis en ligne.** La valeur à écrire est celle du § 2.3 ;
  elle se calcule en six opérations TCG depuis `cpu_fpscr` (qui est déjà un
  global TCG) et depuis `env->fp_prime_mask`, un `uint16_t` qui vaut
  `float_flag_inexact` en mode rapide et 0 sinon. Ce champ est posé **au reset
  et jamais retouché** : pas de cache à invalider, donc pas la classe de bug
  qui va avec. En mode exact la séquence écrit toujours 0.
* **`compute_fprf` + `float_check_status` fusionnés** en
  `helper_fprf_check_float64()`, qui fait les deux choses dans le même ordre.
  `GETPC()` y désigne toujours la même instruction invité, donc une exception
  différée est levée avec la bonne adresse de retour.

Gain mesuré : **2 à 4 %** sur une boucle flottante dense, réparti à parts à peu
près égales entre les deux points. C'est peu, et c'est instructif : l'expérience
de contrôle (supprimer carrément l'appel `fprf`+`check`, ce qui est *faux*) ne
fait gagner que 22 %. Le coût d'une instruction flottante émulée n'est donc
**pas** dominé par les appels de helper — le vrai gain est bien celui du 0001.

---

## 3. Le contrat, en une table

| | mode exact (défaut) | mode rapide (`x-fast-fp=on`) |
| --- | --- | --- |
| Résultats de toutes les opérations | référence | **identiques au bit près** |
| FPRF, FPCC | référence | identiques |
| `FPSCR[OX, UX, ZX, VX*]`, FEX | référence | identiques |
| `FPSCR[XX]` | référence | identique (posé exactement, en logiciel) |
| `FPSCR[FI]` | inexact de la dernière opération | **constant à 1** une fois amorcé |
| `FPSCR[FX]` | posé à chaque inexact | **pas** posé par l'inexact une fois amorcé (plus proche du matériel) |
| `FPSCR[FR]` | 0 (non modélisé) | 0 (non modélisé) |
| trappes (VE/OE/UE/ZE/XE) | référence | identiques — l'amorçage ne s'arme pas si XE/OE/UE sont mis |

---

## 4. Comment l'activer

```sh
./run_tiger.sh               # flottant rapide allumé (défaut)
FASTFP=0 ./run_tiger.sh      # flottant exact (softfloat bit-à-bit)
```

Le lanceur **sonde** le binaire (`qemu_cpu_has_fastfp` dans `scripts/caps.sh`)
avant de poser `-cpu g4,x-fast-fp=on`. La raison est plus dure que d'habitude :
une propriété absente dans `-cpu` ne produit pas un avertissement, elle fait
**quitter** QEMU. Poser l'option à l'aveugle sur un binaire non patché ne
dégraderait pas le lanceur, il l'empêcherait de démarrer. Sans la propriété,
le lanceur prévient et repart en flottant exact. `FASTFP=0` force ce mode.

Le sondage lui-même démarre la machine figée avec la ligne de commande exacte
du lanceur et relit la propriété par `qom-get` : la propriété existe *et* la
syntaxe passe, vérifiées d'un coup. (Pas `qom-list-types` : `x-fast-fp` n'est
pas un type mais une propriété d'un type de CPU, dont le nom dépend de la
version derrière l'alias — `g4` → `7400_v2.9-powerpc-cpu` côté ppc,
`…-powerpc64-cpu` côté ppc64.)

`scripts/build_qemu_qfb.sh` vérifie la propriété sur **les deux** binaires et
l'inscrit dans `bench/build-capabilities.txt`.

---

## 5. Ce qui a été prouvé, et comment

### 5.1 Mode exact = binaire d'aujourd'hui, au bit près

Programme jetable (non gardé dans le dépôt ; la preuve équivalente et plus
forte est au § 5.3, elle, versionnée) : le même code a été compilé deux fois,
une fois contre l'objet `fpu_softfloat.c.o` de l'arbre **non patché**
(`~/src/qemu`), une fois contre celui de l'arbre patché. Il exécute 120 000
jeux d'opérandes (aléatoires, catalogue de cas limites, quatre modes
d'arrondi, `rebias_overflow` / `rebias_underflow`, flush-to-zero) sur
`float64r32_{add,sub,mul,div,sqrt,muladd}`, `float64_*` et `float32_*`, et
imprime résultat + drapeaux.

**2 160 000 lignes, identiques octet pour octet.**

### 5.2 Mode rapide = mode exact, au bit près (hôte)

`tests/fastfp-diff.c`, lancé par `tests/run-all.sh` (section « 4 bis »), se lie
au **vrai** objet softfloat du binaire et compare, pour chaque jeu d'opérandes,
le résultat et les drapeaux obtenus avec un `float_status` exact
(`no_hardfloat`) et avec un `float_status` amorcé (inexact déjà posé) :

* catalogue croisé de 33 valeurs limites (±0, ±Inf, qNaN, sNaN, `FLT_MIN`,
  `FLT_MAX`, `FLT_TRUE_MIN`, plus grand dénormal simple, `DBL_MIN`, `2^24±1`,
  `2^-25`, valeurs non représentables en simple…), pour les 4 modes d'arrondi
  et les 6 combinaisons de drapeaux `muladd` ;
* générateurs aléatoires : float32 normaux élargis, float32 quelconques,
  float64 quelconques, float64 « à un bit près » d'un float32 ;
* générateurs de **demi-ulp** : `a` et `b` à 24-27 binades d'écart (l'arrondi
  au pair de l'addition), mantisses courtes pour que le produit tombe pile sur
  un demi-ulp.

**48 233 079 comparaisons, 0 divergence** (résultats et drapeaux, l'inexact
masqué puisque c'est justement lui qui est amorcé). Reproductible avec deux
graines différentes.

Le même test mesure aussi `float64r32_mul` dans les deux modes : si le chemin
rapide n'était pas réellement emprunté, il comparerait le logiciel à lui-même
et ne prouverait rien.

### 5.3 Au niveau des instructions PowerPC réellement exécutées

La preuve la plus forte, et elle se fait côté hôte. Elle est dans le dépôt :
`tests/fastfp-ppc.c` + `tests/fastfp-ppc.sh` (hors `run-all.sh`, parce qu'elle
demande un `powerpc-linux-gnu-gcc` et un build `ppc-linux-user` que le dépôt ne
fabrique pas) :

```sh
./tests/fastfp-ppc.sh ~/src/qemu-fastfp/build-user/qemu-ppc [qemu-ppc-non-patché]
```

Un binaire PowerPC 32 bits autonome (`-nostdlib -static`, pas de libc) exécute
les vraies instructions et imprime, pour chacune, le résultat **et le FPSCR**
lu par `mffs` :

* arithmétique : `fadds fsubs fmuls fdivs fadd fsub fmul fdiv fmadds fmsubs
  fnmadds fnmsubs fmadd fmsub`, sur un catalogue de 30 opérandes ;
* comparaisons : `fcmpu`, `fcmpo` (avec le CR résultant) ;
* manipulation du FPSCR — celles qui passent aussi par `gen_reset_fpstatus`,
  que le patch 0002 met en ligne : `mffs`, `mcrfs` (quatre champs),
  `mtfsb0 XX`, `mtfsb1 XX`, `mtfsfi`.

Quatre phases : FPSCR remis à 0 avant chaque instruction (XX = 0, l'amorçage ne
doit **jamais** s'armer), FPSCR = XX avant chaque instruction (amorçage armé),
FPSCR laissé s'accumuler (le cas réaliste), et XE armé.

Il est exécuté par le `qemu-ppc` (linux-user) construit depuis les arbres
concernés. **189 920 enregistrements.**

**(a) QEMU non patché du tout  vs  0001+0002 en mode exact :
sortie IDENTIQUE OCTET POUR OCTET.** C'est la garantie « mode exact = binaire
d'aujourd'hui », mesurée sur les instructions elles-mêmes et pas seulement sur
softfloat.

**(b) 0001 seul  vs  0001+0002, dans chaque mode : identiques octet pour
octet** — le patch 0002 n'a bien aucun effet observable.

**(c) mode exact  vs  mode rapide :**

| | |
| --- | --- |
| résultats et CR différents | **9**, tous `mcrfs 2,0`, et uniquement le bit FX |
| FPSCR différents hors FX / FR / FI | **0** |
| FX posé en exact et pas en rapide | 24 395 (jamais l'inverse) |
| FI différent | 62 940 |
| FR différent | **0** (jamais modélisé) |
| phase « XX = 0 » : différences, FX et FI compris | **0** |
| phase « XE armé » : différences | **0** |

Les 9 écarts de valeur sont le même FX, vu à travers `mcrfs` (qui recopie le
champ 0 du FPSCR — FX, FEX, VX, OX — dans un champ de CR). C'est exactement le
contrat de la table du § 3, mesuré.

La ligne « phase XX = 0 » exclut le bloc qui manipule le FPSCR à la main : il
pose XX lui-même en cours de séquence (`mtfsb1 6`), ce qui arme légitimement
l'amorçage pour les instructions suivantes — et on voit alors FI diverger,
exactement là où il doit. Réciproquement, le `mtfsb0 6` du même bloc désarme
l'amorçage, et le FPSCR redevient identique jusqu'au prochain inexact : c'est
la vérification du « ça se ré-arme et se désarme tout seul » du § 2.3.

### 5.4 Le gain

Micro-mesures sur l'objet softfloat (i7-10700F, `-O2`, opérandes float32
normales) :

| | logiciel | rapide | gain |
| --- | --- | --- | --- |
| `float64r32_add` | 16,5 ns | 4,1 ns | ×4,0 |
| `float64r32_mul` | 12,1 ns | 7,7 ns | ×1,6 |
| `float64r32_div` | 31,4 ns | 8,7 ns | ×3,6 |
| `float64r32_sqrt` | 8,0 ns | 5,4 ns | ×1,5 |
| `float64r32_muladd` | 24,8 ns | 6,7 ns | ×3,7 |
| `float64_div` | 30,0 ns | 3,3 ns | ×9,2 |

Boucle PowerPC réelle (24 millions d'instructions simple précision, sous
`qemu-ppc` linux-user), résultat final identique au bit près :

| | exact | rapide |
| --- | --- | --- |
| QEMU non patché | 0,47 s | — |
| 0001 seul | 0,46 s | 0,26 s (**×1,77**) |
| 0001 + 0002 | 0,45 s | 0,25 s (**×1,80**) |

⚠ C'est du linux-user sur une boucle artificielle : aucune mémoire virtuelle à
traverser, aucun périphérique, aucun MTTCG. **La seule mesure qui compte est
l'A/B sur la VM mac99.**

⚠ **Le gain dépend des opérandes.** La même boucle avec des constantes
`double` non représentables en simple ne gagne que 15 % : le chemin rapide
`float64r32_*` exige des opérandes exactement représentables en float32. C'est
le cas normal du code compilé en `float` (les valeurs viennent de `lfs`), mais
pas celui d'un code qui mélange `double` et `float`. La mesure A/B sur la VM
est donc la seule qui compte.

---

## 6. Ce qui n'a PAS été vérifié côté hôte

* **Mac OS X lui-même.** Rien ne garantit que Tiger ne lise pas `FPSCR[FI]`
  quelque part. C'est le point le plus à surveiller — mais FI n'a aucun usage
  raisonnable hors d'un débogueur numérique : il n'est ni collant ni testable
  par branchement direct (il faut `mcrfs`/`mffs`). Le risque le plus concret
  serait une bibliothèque mathématique qui s'en sert pour arrondir « à
  l'ancienne ».
* **Le comportement sous MTTCG (SMP).** `env->fast_fp` et les deux
  `no_hardfloat` sont posés au reset de chaque CPU et ne changent jamais
  ensuite : il n'y a pas d'état partagé. Mais ça n'a pas été exercé.
* **AltiVec en conditions réelles.** L'analyse (§ 2.4) dit que c'est
  invisible ; aucun test n'a exécuté de vraies instructions AltiVec dans les
  deux modes.
* **`frsqrte` / `fres`.** QEMU les implémente par une division exacte, pas par
  une estimation ; le patch ne change pas ça, mais `fres` passe par
  `float64r32_div` et emprunte donc le chemin rapide.
* **La sauvegarde/restauration d'état (migration, `savevm`).** Le champ
  `fast_fp` vient de la ligne de commande, pas de l'image : restaurer un état
  sauvé en mode rapide dans un QEMU lancé en mode exact (ou l'inverse) donnerait
  un FI différent. Sans importance ici, mais c'est une différence réelle.

## 7. Pour l'agent qui teste dans l'invité

Le test différentiel invité doit **masquer FI, FR et FX** avant de comparer les
FPSCR, et comparer les résultats sans masque. Points à regarder de près :

1. **Les opérandes.** Un test qui n'emploie que des `double` non représentables
   en simple ne testera jamais le chemin rapide `float64r32_*`. Il faut des
   valeurs venant de `lfs` / de variables `float`.
2. **La transition.** Vérifier qu'une séquence qui commence avec `FPSCR = 0`
   donne le même XX (et au même moment) dans les deux modes : c'est tout
   l'argument d'exactitude de l'amorçage.
3. **`mtfsb0 XX` en cours de route** doit ramener au logiciel exact — donc
   remettre FI à sa vraie valeur jusqu'au prochain inexact.
4. **Overflow et underflow** : `FLT_MAX * FLT_MAX` (OX), `FLT_MIN / 2^30` (UX),
   et les résultats dénormaux, qui doivent repartir au logiciel.
5. **Vorbis / la physique** : hachage du PCM décodé et d'une trajectoire
   physique longue, dans les deux modes. C'est la vraie preuve d'usage.
