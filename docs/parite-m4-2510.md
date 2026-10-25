# Parité M4 : la séance du 25/10/2026 au soir, préparée depuis le PC

Règle du projet : toute accélération d'un hôte a son pendant sur l'autre, **au même niveau de
preuve**, puis un A/B avant d'être allumée. Depuis le 06/10, le PC a pris de l'avance. Ce
document fixe l'ordre de la séance, du plus grand gain attendu au plus petit, et renvoie pour
chaque patch aux preuves déjà écrites.

Défauts actuels de `run_tiger.sh` sur `Darwin:arm64` : `VFPNATIVE LMWVEC JCWORD` seulement.
Tout le reste ci-dessous est éteint sur le M4.

## 1. Le retard, patch par patch

| patches | levier (PC) | code propre à l'hôte | état arm64 | preuves à refaire sur le M4 |
|---|---|---|---|---|
| 0049-0050 | G4 sur la cible 32 bits en MTTCG (`PPC32MT`) : DOOM 3 −7,7 %, UT2004 −5 % | aucun (frontal, code commun) | à construire | `atomtest` (sur arm64, `stwcx.` passe par `casal`/LL-SC), suite et vérificateurs avec `PPC32MT=1`, endurance courte (`docs/ppc32-mttcg.md` §8, §11) |
| 0045-0047 | flottant simple sans contrôles redondants (`FPKNOWN FPRFLAZY STFSKNOWN`) : DOOM 3 −6,3 % | émetteur aarch64 de `ppc_fp32` | **écrit, jamais exécuté** | `fpproof.sh`, `fptest k/s/d/c` contre la référence, mutants de l'émetteur (`docs/tcg-g4.md` §37.3, §39.2, §39.6) |
| 0051 | `lfs` producteur (`LFSKNOWN`) : DOOM 3 −9,5 % | émetteur aarch64 (`fcvt d, s`, `ccmp`) | **écrit, jamais exécuté** | `fpproof.sh -DFPPROOF_LFSK`, `lfskuser.sh … l 0 100000000` et `cmp` avec un `qemu-ppc` linux-user du M4, `lfsk-mut.py` à porter sur aarch64 (§40.7) |
| 0031-0035, 0037-0039 | `TLBPRECISE LMWINLINE DCBZINLINE FPNATIVECMP VFPNATIVECMP` | 0033-0034 : émetteurs `tcg/0037`, `0038` | **écrits, jamais exécutés** | `tools/tcg/m4-serie-0031-0035-proofs.sh prep host guest mut` (`docs/parite-arm64-0031-0035.md` §5) |
| 0040-0043 | `DIRTYSKIP PTEGMAP JC2WAY` : DOOM 3 −4,4 % | aucun (C commun) | à construire | vérificateurs, empreintes (`docs/tcg-g4.md` §36.7) |
| 0052 | TLB par base (`TLBBASE`, qemu-system-ppc seul) : DOOM 3 −2,2 % | émetteur aarch64 (chemin court des chargements couverts) | **écrit, jamais exécuté** (7 encodages relus) | `tlbbproof` (modèle aarch64), job `tlbbtest` dans la VM, `tlbbnu.S`, `tlbb-mut.py` / `tlbbnu-mut.sh` à porter sur aarch64, partie vérifiée `x-tlb-base-verify` (`docs/tcg-g4.md` §41.3, §41.7) |
| 0053 | AltiVec flottant connu | — | **non fusionné** (juste, neutre en jeu sur le PC ; `archive/0053-vfp-known`) | rien à faire |

Pour 0045-0047 et 0051, la chaîne 0045 → 0051 dépend de 0033 (`ppc_fp32` et le
talon). **0037 doit donc passer avant**, c'est-à-dire la série 0031-0039.

## 2. L'ordre de la séance

0. **Préparer.** Arrêter la VM quotidienne du M4, puis reconstruire `~/src/qemu` avec toute la
   série (`scripts/build_qemu_qfb.sh` applique 0031-0052 et sonde chaque propriété). Garder le
   binaire actuel pour l'A/B. Construire aussi le `qemu-ppc` linux-user de banc
   (`tools/tcg/fpnatcmp-userhack.py`, avec ses neutralisations pour 0031-0050). Préparer une copie
   du disque de dev.
1. **0049-0050 (MTTCG 32 bits).** Pas d'émetteur, et c'est le plus grand gain. Lancer `atomtest`
   en SMP=2, puis la suite et les vérificateurs sur la VM de dev avec `PPC32MT=1`, puis 20
   démarrages. Si tout est vert, l'A/B DOOM 3 contre ppc64.
2. **Série 0031-0039.** Lancer `m4-serie-0031-0035-proofs.sh prep host guest mut` et lire le
   bilan. C'est la condition des étapes 3 et 4.
3. **0045-0047.** Lancer `fpproof`, `fptest` (modes k, s, d, c) contre la référence, les
   mutants, puis une partie vérifiée de Marble Blast.
4. **0051.** Lancer `fpproof` en variante LFSK, `lfskuser.sh` (2^32 mots), les mutants
   aarch64, puis une partie DOOM 3 vérifiée (`x-fp-verify`).
5. **0052.** Lancer `tlbbproof`, le job `tlbbtest` (ref contre tlbb, même empreinte), `tlbbnu`, les mutants
   aarch64, puis une partie vérifiée (`x-tlb-base-verify`). 0052 n'agit que sur qemu-system-ppc,
   donc avec `PPC32MT=1` : il passe après l'étape 1.
6. **0040-0043.** Vérificateurs et empreintes.
7. **A/B en jeu**, VM quotidienne du M4 au repos :
   - d'abord la série entière contre le binaire actuel, sur DOOM 3, UT2004 et Marble Blast ;
   - puis, si l'écart total déçoit, levier par levier.
8. **Allumer** sur `Darwin:arm64` ce qui est prouvé et gagnant. Reconstruire `build-fast` avec
   un profil PGO pris sur les jeux (`pgo-train.sh --cible ppc32mt`). Noter les chiffres dans la
   doc de chaque patch.

Ce qui ne passe pas reste éteint sur le M4, avec la raison écrite dans la doc du patch.
Une séance trop courte s'arrête proprement après n'importe quelle étape : chaque étape se
suffit à elle-même.

## 3. Pièges connus

- Ne jamais supprimer `tiger.lock` (flock).
- Le disque Tiger raw est partagé par USB entre les deux hôtes. Le préparer sur le M4.
- Sur le PC, les agents ne doivent jamais faire tourner de VM pendant un A/B. Sur le M4, c'est
  pareil.
- `perf` n'existe pas sous macOS. Les bancs du PC qui s'en servent (`ppc32mt-bench.sh`) sont à
  adapter, ou à remplacer par `tools/tcg/jit-m4-proof.sh` avec `PPC32MT=1`.

## 4. État du PC au départ (25/10, nuit)

- Dépôt : `main` poussé. Les essais non fusionnés sont sous les étiquettes `archive/*` (plugin vers
  l'hôte, lot d3q, régions LLVM, 0053).
- Défauts sur Linux x86-64 : ceux du 21/10, plus `PPC32MT`, `LFSKNOWN` et `TLBBASE`.
- DOOM 3 sur le PC : 79,0 ms/image en fenêtre (UT2004 41,0), binaire quotidien avec 0052 (§41.6 bis
  de `docs/tcg-g4.md`).
- Sur le M4, commencer par `git pull`, puis l'étape 0 ci-dessus.

## 5. La séance sur le M4 (25/10, 16 h 45 →)

Hôte partagé avec d'autres travaux pendant toute la séance (charge jusqu'à 96) : tout tourne en
`nice 15`, **aucune mesure de vitesse** ; les preuves ci-dessous n'en dépendent pas.

### 5.1 Étape 0 : ce qui a changé par rapport au plan

- `~/src/qemu` était déjà reconstruit avec 0031-0052 (binaire du 25/10 16 h 49, toutes les
  propriétés présentes) : l'A/B se fera propriété par propriété, même binaire.
- **0037 ne s'appliquait plus** (écrit le 06/10 contre l'arbre à 0035 ; 0045-0052 ont touché les
  mêmes lignes de l'émetteur aarch64). Reposé à la main, et mis au niveau de ce que l'émetteur
  x86_64 a reçu entre-temps (note en tête de `patches/tcg/0037`) : bits `PFP_K_G`/`PFP_K_B` de
  0045 pour `frsp` et `fctiw(z)`, forme paresseuse de 0046 pour `frsp`, `fcmpo` qui pose
  `fprf_src = PPC_FPRF_NONE` comme `fcmpu`, et **`fctiw(z)` qui ne touche pas `fprf_src`** (la
  sortie commune de l'aarch64 l'aurait écrit pour tout op paresseux). Sans 0037,
  `TCG_TARGET_HAS_ppc_fp_cmp` vaut 0 sur arm64 : rien de 0033 ni de 0045-0051 n'y prend le
  chemin natif. 0038 et 0039 se posent tels quels par-dessus. 0037-0039 posés sur `~/src/qemu`
  redonnent exactement `~/src/qemu-arm64` ; **ils restent hors de `build_qemu_qfb.sh`** jusqu'à
  la fin des preuves, et se posent désormais **après 0052**.
- **Disque de dev en 10.4.11** (comme celui du PC) : `disks/tiger-dev-m4-1011.raw`, clone de
  `tiger-dev-m4.raw` (resté en 10.4.6) passé par le combo au bureau, job
  `tools/guest/jobs/combo` (installer 115 s ; `sw_vers` 10.4.11 8S165, Darwin 8.11.0, kext
  POMPPCGPU chargé). Agent invité remplacé seul (l'ancien ne rendait pas le code de sortie des
  jobs) ; **pas de `devloop.py prepare`**, qui aurait déplacé les boîtes aux lettres de
  `bench/devloop/mailbox.json`, commun aux disques de dev. Noyau copié dans
  `bench/endurance/invite-10.4.11/mach_kernel`. Preuves invitées sur des clones de ce disque.
- `tools/tcg/ppc32mt-bench.sh` porté sur macOS (`perf` facultatif, `ps` au lieu de `/proc`,
  `lsof` au lieu de `fuser`, dont la version macOS rend 0 même sans détenteur) ; `CPU_BASE` /
  `TCG_BASE` pour lancer avec les défauts du M4 au lieu de ceux du PC.
- Pas de `qemu-ppc` linux-user sous macOS : les preuves que le PC fait en linux-user (`fptest`
  k, `mut.sh`, `lfskuser.sh`) passent ici par l'invité (vrai émetteur, mode système).

### 5.2 Étape 1 : 0049-0050 (MTTCG 32 bits) — prouvé, A/B à jouer

`bench/ppc32mt/m4-etape1.sh` (non versionné, comme `bench/`), SMP=2, défauts du M4 (`run_tiger.sh`,
`Darwin:arm64`), binaire `~/src/qemu/build`. En 10.4.6 puis en **10.4.11**, mêmes résultats :

| | ppc MTTCG, référence | ppc MTTCG, vérifié | ppc64, vérifié (témoin) |
|---|---|---|---|
| empreintes `vfptest` `vmxtest` `lmwtest` `smctest` A-F, `jctest`, `tbtest` | celles du PC (§4.1 de `ppc32-mttcg.md`) | identiques | identiques |
| `atomtest` A, B, C | exacts (8 000 000) | exacts | exacts |
| `x-vfp-native-verify` / `x-vmx-verify` | — | 0 / 0 + 0 divergence | 0 / 0 + 0 |
| `x-ret-verify` (10.4.11) | — | 3,66 G en ligne, 0 divergence, 0 « sans traduction », 213 courses | 3,55 G, 0, 0, 427 courses |
| `x-tb-verify` (10.4.11) | — | 2,64 G lectures, 0 positif | 2,30 G, 0 |

Aucun « Guest not yet converted to MTTCG ». 0 « sans traduction » (14 sur le PC, tous de la classe
du §35 de `tcg-g4.md`, liée à `x-tlb-precise`, éteint ici).

**Démarrages du bureau** (`devdemarrages.py`, 10.4.11, 5 froids + 15 reboots) : **19 ok sur 20**,
bureau en 77 s par reboot, 98-170 s à froid. **Un incident** (cycle 3, 17 h 45, hôte à charge
~96) : après `shutdown -r`, l'ancien système est resté debout plus de 4 min (la sonde l'a vu,
« up 6 mins »), puis l'arrêt s'est fait (« syncing disks... Killing all processes … continuing »)
et la remise à zéro par le PMU n'est jamais venue : deux vCPU dans `_machine_idle`, EE = 1,
`panicstr` nul (`bench/endurance-dev/m4-1011-p32/incidents/0003-gel`). Lecture non tranchée :
famine de l'invité (un délai du pilote PMU expiré, l'horloge avançant sans les vCPU) ou défaut
du redémarrage sous MTTCG 32 bits. **À reproduire au calme** (et sur le témoin ppc64 s'il revient)
avant d'allumer `PPC32MT` sur le M4.

### 5.3 Étape 2 : série 0031-0039

`host` (`bench/m4-serie/host-2510/`, sur `~/src/qemu-arm64`) : **vert** — 49 encodages A64 relus,
0 écart ; modèle NEON de 0038 identique à la référence x86 ; `fpproof` 0 divergence (dont
`fcmpo`, `fdivs`, `fdiv` : 2 058 800 chacun ; talon 83,7 M vecteurs), ses mutants tous détectés ;
`vfpcmpproof` 422,7 M vecteurs, 0 divergence, mutants NEON détectés ; `tlbpproof` 5 mutants sur
5 ; propriétés présentes dans les deux binaires.
`guest` (`bench/m4-serie/guest-mut-2510/`, disque 10.4.11, ppc64 SMP=2 puis ppc SMP=1, démarrages
ref / nat (+ les cinq propriétés) / ver (+ leurs vérificateurs)) : **vert**. `fptest` c, s, d,
`vfptest` c et o, `tlbtest`, `lmwtest`, `dcbztest` : **mêmes empreintes** dans les trois démarrages
et que le PC (`67cf96ef…`, `e80ec802…`, `fb3e6006…`, `65be3233…`, `bc13e93c…`, `deee3d70…`,
`2575eafc…`, `e2f15e6f…`) ; `dcbztest` ne diffère que par la réservation (`stwcx` après `dcbz`,
attendu, §32.4 de `tcg-g4.md`). Vérificateurs (SMP=2) : `lmw-inline` 2,18 G, `tlb-precise`
(12 suspects, tous résolus), `dcbz-inline` 33 M, `vfp-native`, `vfp-native-cmp` (67 M
comparaisons, 33,5 M conversions), **et le scalaire de 0037, première exécution de l'émetteur
aarch64** : `frsp` 8,69 M, `fctiw` 2,89 M, `fctiwz` 2,94 M, `fcmpo` 3,17 M, `fdivs` 3,19 M,
`fdiv` 10,8 M, `fsel` 3,17 M vérifiés : **0 divergence partout**.

`mut` (émetteur arm64 réel, `MUT_SRC=~/src/qemu-arm64-mut`) : **20 mutants sur 21 détectés** (les 9
de 0037 par `fptest c`, les 12 de 0038 par le vérificateur). **Non détecté : n° 20**, `uim` oublié
dans le talon de `vcf*`. Le talon n'est pris que porte fermée, c'est-à-dire drapeau inexact de
`vec_status` à zéro : 2 fois sur 8,4 M `vcfux`, jamais pour `vcfsx`, dans toute la suite. Ce
drapeau est commun à tous les processus invités et reste posé dès la première opération
vectorielle inexacte après le démarrage : aucun test invité ne peut forcer ce chemin. Le code est
relu : `l->vfp_off[3]` = `args[4]` = `uim`, comme l'émetteur x86 (dont la liste de mutants,
`vfpcmp-mut.sh`, n'a pas celui-ci : **trou de couverture commun aux deux hôtes**, pas un défaut).

### 5.4 Étapes 3-5 : 0045-0047, 0051, 0052 — un défaut trouvé et corrigé

Job invité `tools/guest/jobs/fpk-suite` (nouveau ; pendant des bancs linux-user du PC) : `fptest`
simple, `d 65536`, `c 65536`, `k 4096`, `k 4096 7`, `l 0 100000000` (les 2^32 mots de `lfs`,
auto-vérifié), `tlbbtest 20000`. Binaire `~/src/qemu-arm64`, SMP=2 MTTCG, disque 10.4.11, trois
démarrages (`bench/m4-serie/etapes345.sh`) : réf. (défauts M4 + série 0031-0039), leviers
(+ `x-fp-known x-fprf-lazy x-stfs-known x-lfs-known x-tlb-base`), vérifiés (+ `x-fp-verify
x-fp-native-cmp-verify x-tlb-base-verify`).

**Premier passage : le mode `k` change d'empreinte sous les leviers** (`5b4563c8…` au lieu de
`3f42dddd…`), le reste identique. Sous les vérificateurs : 60,2 G opérations, **16 divergences,
toutes en `fdivs` (9) et `fdiv` (7)**. Cause, dans la reprise de 0037 (§5.1) : le test du
diviseur nul lisait `t = frB << 1` laissé par `pfp_zon()`, qui ne tourne plus quand frB est
connu (`PFP_K_B`, 0045) ; or « connu » veut dire nul **ou** normal : un diviseur nul prenait le
chemin court (ni ZX ni VXZDZ, résultat faux). x86_64 recalcule `t` (`pfp_shl1`) : l'aarch64 aussi
désormais (`patches/tcg/0037`, note en tête). Le 0037 d'origine, écrit avant 0045, était juste
dans son monde ; c'est sa mise au niveau qui a manqué ce cas.

**Après correction** (`bench/m4-serie/correctif-div.sh`) :

| | réf. | leviers | vérifiés |
|---|---|---|---|
| `fptest` simple / d / c | `e80ec802…` / `fb3e6006…` / `e6bba641…` | identiques | identiques |
| `fptest k` / `k … 7` | `3f42dddd…` / `4181b7cf…` (= PC, §37.3 de `tcg-g4.md`) | **identiques** | identiques |
| `fptest l` (2^32 mots) | 0 écart, `56f282c1…` | identique | identique |
| `tlbbtest` modes 3 / 4 | 0 erreur, `9108eca7…` / `028069d6…` | identiques | identiques |

Vérificateurs (premier passage, hors `fdiv(s)`) : `x-tlb-base-verify` **31,4 G chargements couverts,
0 divergence** ; `x-lfs-known` 134,7 M mots hors ligne ; `x-fp-known` 108 M sorties de bloc.
Après correction (`bench/ppc32mt/m4-fpk-ver2/`) : `x-fp-verify` **60,2 G opérations, 0 divergence**
(dont `lfs` 17,2 G, `frsp` 8,6 G, `fdivs` 1,15 M, `fdiv` 3,23 M) ; `x-tlb-base-verify` **31,4 G
chargements couverts, 0 divergence**.
