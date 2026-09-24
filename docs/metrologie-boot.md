# Métrologie du boot et optimisations TCG (extrait du README, archivé le 24/09/2026)

Historique de la couche baseline (Linux x86-64, 2026-08). Rien ici n'a changé depuis ; le protocole de mesure A/B reste la référence pour toute optimisation de QEMU.

## 1. Optimisations (guidées par le profiling)

Méthode : `scripts/profile-boot.sh` (perf record) pour **localiser** un hotspot → patcher `../qemu`
→ recompiler → **valider par A/B déterministe** (voir ci-dessous). ⚠️ **Leçon durement acquise : le
perf self% localise, il ne valide pas.** Baisser le self% d'une fonction chaude ne prouve pas un
gain de débit — le coût peut se déplacer ailleurs. Seul un **A/B stock-vs-patché en** `-snapshot`**,
hôte au repos** tranche.

Validation par **A/B interleave, hôte au repos, garde-fou charge** (médiane sur n=8) :

**État actuel : aucun patch d'optimisation TCG actif.** Après validation, aucune des optimisations
tentées ne survit ; les patches restent dans `patches/01-*.patch` et `patches/02-*.patch` pour
référence. À ne pas confondre avec le binaire utilisé : le build source **est** patché, mais
seulement pour des **fonctionnalités** (SMP mac99, device Screamer, `qfb-pci` — voir
`scripts/build_qemu_qfb.sh`), jamais pour la performance du JIT.


| #     | Cible                                                                                                                       | Médiane boot (CPU_qemu, `-snapshot`)                                               | Statut                                                                                                             |
| ----- | --------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| 01    | Division 128 bits du timebase (`mftb` → `ns_to_tb` → `__divti3`) + indirection wrapper d'horloge (`cpus_get_virtual_clock`) | **23,32s = stock** : **neutre** (aucun gain mesurable).                            | **REVERTÉ** — zéro gain, et l'approximation par réciproque touche le timing (risque gratuit). `patches/01-*.patch` |
| 02/03 | Flush jmp_cache O(N) → **invalidation par génération O(1)** + cache 4× (12→14 bits)                                         | **28,63s (+23%)** ; n'atteint jamais le mode rapide (dispersion 0,25s) ; hang ~3×. | **REVERTÉ** — net-négatif. `patches/02-*.patch`                                                                    |


Patch 01 : l'A/B propre le donne **identique à stock** (23,32 vs 23,32) → il ne gagne rien, et comme
`ns_to_tb_cached` remplace la division exacte par une multiplication-réciproque approchée (≤1 tick),
il ajoute un risque de timing pour rien → reverté.

Patch jmp_cache : **régression réelle et reproductible de ~23%** (confirmée sur A/B propre après une
première campagne faussée par la charge hôte). Sous-résultat : la taille 14 seule (avec effacement
stock) est neutre → le cache 4× n'est pas en cause, c'est la **logique de génération**. Deux modes de
boot (~23,3 rapide / ~28,6 lent) ; le patch force quasi toujours le mode lent. Le self% avait menti :
`tcg_flush_jmp_cache` disparaissait du profil mais le débit chutait — **le perf self% localise, il ne
valide pas**.

**Métrologie (protocole de mesure, durement acquis).** L'outil est `scripts/measure-boot.sh` : il
boote en headless, détecte l'écran bleu de login à la couleur moyenne des `screendump`, et écrit
`wall=…  cpu=…` dans `bench/last-boot.txt` (les captures intermédiaires vont dans `bench/frames/`).
**C'est `CPU_qemu` — et non le temps mur — qui sert de métrique**, car il est immunisé à la
contention hôte. Le chrono n'est fiable qu'avec les TROIS précautions : (a) **hôte vraiment au
repos** (une charge de fond, ex. MAME, gonfle même le CPU_qemu car le guest spin-attend des
timers en retard) ; (b) `EXTRA_ARGS="-snapshot"` — sinon un **fsck sur volume HFS+ sale** (on
tue QEMU avant l'arrêt propre) ajoute ~35s de façon intermittente ; (c) **interleave**
stock/patché (alterner run par run) pour annuler tout biais d'ordre/dérive. Le perf self%
reste l'outil de profiling fin.

Les précautions (a) et le nettoyage sont désormais **appliqués par l'outil**, pas laissés à la
discipline :

- **pré-vol** : `measure-boot.sh` refuse de démarrer si un autre `qemu-system-ppc` tourne ou
  si `/proc/loadavg` dépasse `MAXLOAD` (1.0 par défaut). `POMPPC_FORCE=1` passe outre — et le
  dit : *« ce chiffre n'est pas publiable »*.
- **PID via `-pidfile`** : le `pgrep -f qemu-system-ppc.*POMPPC-measure` d'avant pouvait
  s'accrocher à un QEMU orphelin d'un run précédent et lire `/proc/<mauvais_pid>/stat`. Une
  mesure fausse mais plausible est le pire mode de panne pour une campagne A/B.
- **`trap` de nettoyage** : un Ctrl-C laissait tourner la VM `setsid`, c'est-à-dire exactement
  la charge de fond que (a) interdit. `measure-boot.sh` et `profile-boot.sh` tuent la leur en
  sortie, quoi qu'il arrive.
- **outils vérifiés** : sans ImageMagick, la détection d'écran bleu renvoyait du vide, la
  comparaison arithmétique valait 0, et on obtenait un timeout silencieux au lieu d'une erreur.

```bash
EXTRA_ARGS="-snapshot" scripts/measure-boot.sh               # un run propre (pré-vol inclus)
SMP_N=2 EXTRA_ARGS="-snapshot -accel tcg,thread=multi" \
  scripts/measure-boot.sh                                    # variante SMP/MTTCG
scripts/ab-measure.sh <binA> <binB> 4                        # A/B interleavé, médiane
```

- **variante SMP** : `SMP_N >= 2` fait basculer `measure-boot.sh` sur **`qemu-system-ppc64`**
  (le seul à annoncer `TARGET_SUPPORTS_MTTCG`) et lui passe le **`-bios` de l'OpenBIOS unifié**,
  exactement comme `run_tiger.sh`. Sans ces deux-là, le device tree n'a qu'un nœud CPU et
  l'invité boote mono-cœur : la mesure « SMP » est alors fausse mais parfaitement plausible.
  Le script sonde `-smp N` sur le binaire et **refuse de mesurer** s'il est rejeté.

`scripts/ab-measure.sh` applique le protocole complet (interleave A/B/A/B, `-snapshot` forcé,
médiane sur n paires) et s'arrête de lui-même si le pré-vol refuse un run.

⚠️ **Le coût du device Screamer sur le temps de boot n'a pas encore été mesuré en A/B.** Il est
instancié dans le macio pour toute machine mac99, donc présent même en mesure headless. Deux
binaires de comparaison sont prêts dans `bench/ab/` (gitignoré) :

```bash
scripts/ab-measure.sh bench/ab/qemu-no-screamer bench/ab/qemu-with-screamer 4
```

À lancer machine au repos. Neutraliser le backend audio (`-audiodev none`) ne change rien, donc
si coût il y a, il ne vient pas du flux audio.

`clock_gettime` vdso lui-même (~5%) = lecture d'horloge fondamentale, laissée telle quelle
(la battre = rdtsc maison, risqué). Prochain gros poste : dispatch TCG
(`tb_lookup`+`qht_lookup`+`helper_lookup_tb_ptr` ~20%).

## 2. Prochaine étape TCG (non reprise depuis 2026-08)

Le build source et le profiling sont en place (`scripts/build_qemu_qfb.sh`,
`scripts/profile-boot.sh`) et les deux premières tentatives d'optimisation ont été mesurées puis
revertées. Le prochain gros poste identifié est le **dispatch TCG**
(`tb_lookup` + `qht_lookup` + `helper_lookup_tb_ptr`, ~20 % du temps hôte) : c'est là que se joue
l'optimisation du JIT, avec le même protocole A/B qu'au-dessus.

⚠️ La baseline de 23,32 s a été mesurée avec l'ancien harnais (PID par `pgrep`, sans pré-vol
ni `trap`). **Elle est à refaire** avec `measure-boot.sh` durci avant de servir de référence à
la campagne TCG — c'est le seul chiffre du projet dont la provenance n'est plus vérifiable.

