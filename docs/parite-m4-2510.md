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
| 0052, 0053 | TLB par base, AltiVec flottant connu | émetteurs aarch64 | en cours sur le PC (agents) | seulement s'ils sont fusionnés d'ici là |

Pour 0045-0047, 0051 et 0052-0053, la chaîne 0045 → 0051 dépend de 0033 (`ppc_fp32` et le
talon). **0037 doit donc passer avant**, c'est-à-dire la série 0031-0039.

## 2. L'ordre de la séance

0. **Préparer.** Arrêter la VM quotidienne du M4, puis reconstruire `~/src/qemu` avec toute la
   série (`scripts/build_qemu_qfb.sh` applique 0031-0051 et sonde chaque propriété). Garder le
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
5. **0040-0043.** Vérificateurs et empreintes.
6. **A/B en jeu**, VM quotidienne du M4 au repos :
   - d'abord la série entière contre le binaire actuel, sur DOOM 3, UT2004 et Marble Blast ;
   - puis, si l'écart total déçoit, levier par levier.
7. **Allumer** sur `Darwin:arm64` ce qui est prouvé et gagnant. Reconstruire `build-fast` avec
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
