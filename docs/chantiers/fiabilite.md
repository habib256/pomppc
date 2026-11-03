# Fiabilité : endurance et plantages

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Commun aux deux hôtes.

- [ ] **[Outils] Endurance sur la configuration par défaut** — banc `tools/endurance/`,
  `docs/endurance.md`. Fait : PC 683 démarrages sans incident (§10), M4 342 démarrages et 40
  cycles de jeu sans incident (§11), QE PC 48 démarrages et 16 cycles (§14), zone froide
  (§15). **Reste pour fermer :** (1) voir le gardien `POMPPCSshGarde` agir sur un vrai « pas
  de ssh » (série longue ou hôte chargé, §8.6) ; (2) la panique #76 (`kalloc.16` corrompue,
  §8.4) : `pmemsave` à la collecte, reproduction par stress de la console, A/B
  `-accel tcg,thread=single` ; (3) une tranche après chaque allumage (PACKBITS compris).

- [ ] **[Système] Plantages en jeu sans cause** — paniques par délai de verrou
  (`LockTimeOut` 250 ms, `_panic+0x254`), `exit 139`/`138` de DOOM 3 et cmr au chargement.
  Nouveaux cas du 01/11 : panique cmr sous vérificateurs avec `tcg/0063` (`tcg-g4.md` §47,
  `bench/tcg/ab/dbat-0111-verif-cmr/v-1/panique-*`), cmr `exit 138` sous le nouveau PGO.
  **Au prochain gel :** `python3 tools/re/kpanic.py mach_kernel` AVANT tout reset.
  **Fermeture :** texte d'une vraie occurrence lu, cause établie, correction et série sans
  incident ; des lancements réussis ne suffisent pas.

## Piste proposée le 01/11 (à arbitrer par l'utilisateur)

4. **Boîte noire des paniques.** Les plantages en jeu (délai de verrou, `exit 138`/`139`) restent
   « sans cause » faute de relevé au moment même. Levier : QEMU arme un point d'arrêt sur
   `_panic` du `mach_kernel` (adresse connue de `tools/re/kpanic.py`) et, à l'entrée, écrit
   registres, pile, RAM et console dans `.run/panique-<date>/` puis laisse l'invité continuer ;
   côté invité, le rapport CrashReporter d'un `exit 138` est rapatrié par ssh. **Fermeture :**
   une vraie occurrence dépouillée de bout en bout. Coût : une journée. Gain : la condition de
   fermeture du chantier « Plantages en jeu sans cause » existe enfin.
