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

- [ ] **[PC, matériel] Plantages du binaire instrumenté et machine checks du 04/11.** Cause inconnue [F].
  Le binaire PGO instrumenté (`-fprofile-generate`, `-fprofile-update=prefer-atomic`) a planté 5 fois le 04/11, sur `CPU 0/TCG` et
  `CPU 1/TCG`. Aucune trace sur le binaire de production : 2 tours plantés sur 1 079 journaux `run_tiger.log`, ces deux-là
  (grep du 04/11 sur `bench/tcg/ab` et `bench/endurance`).
  - [A] 08:36:06, noyau : `trap invalid opcode ip:…353` à l'offset `0x7b9353` de `qemu-system-ppc`.
  - [A] 3 `SIGSEGV` sous gdb sur `CPU 1/TCG`, même offset (`bench/tcg/pgo-crash/gdb-aslr.txt`). À cet offset : `mov %edx,%eax`,
    après `lock incq __gcov0` et `mov $1,%edx`, dans `tlb_hit_page_mask_anyprot`, sous `do_invalidate_BAT` (`helper_store_dbatu`).
    Cette instruction est valide et ne touche pas la mémoire : elle ne devrait pas pouvoir fauter.
  - [A] 08:27:53, noyau : `CPU 0/TCG segfault at 5c0449504330 ip 5c0449504330 error 15`, à une autre adresse, hors du texte du binaire.
    Le bit « réservé » de `error 15` est lui-même anormal.
  - [A] 5 `mce: [Hardware Error]: Machine check events logged`, à 9:22, 9:33, 9:38, 11:01 et 11:02, **non décodées** (`rasdaemon` installé
    à 12:52). Aucune dans les journaux des 3 démarrages précédents. Les deux plantages hors gdb les précèdent : le lien n'est pas établi.
  - [A] Pas reproduit : sous gdb sans ASLR (4 démarrages, mb, zen, ut) ; sous gdb avec ASLR hors cmr ; 31 min de cmr à 14:30, ni plantage ni MCE.
  - [A] Matériel : i7-10700F, microcode `0x100`, DDR4 sans ECC, 8 Gio Crucial + 32 Gio autre marque à 2667 MT/s, 1,2 V. Bridage thermique du paquet :
    97 s en 18 h ; 100 °C sous pleine charge dès juillet (`bench/endurance/thermique-0707.txt`).
  - [F] Hypothèses : défaut du processeur sur cette séquence (l'adresse fixe plaide pour le code, pas pour une panne aléatoire), ou panne matérielle
    naissante. La RAM expliquerait mal un même offset dans deux fichiers compilés à part.
  **À faire au retour sur le PC :** `ras-mc-ctl --errors` (toute MCE est décodée depuis le 04/11, banque et type) ; à la prochaine faute, lire
  `$_siginfo` (`build-fast-gen/qemu-gdb` le relève) et `journalctl -k`. Si une MCE désigne la mémoire, ou reste sans cible : `memtest86+`.
  **Fermeture :** une MCE décodée ou une cause de la faute établie.

## Piste proposée le 01/11 (à arbitrer par l'utilisateur)

4. **Boîte noire des paniques.** Les plantages en jeu (délai de verrou, `exit 138`/`139`) restent
   « sans cause » faute de relevé au moment même. Levier : QEMU arme un point d'arrêt sur
   `_panic` du `mach_kernel` (adresse connue de `tools/re/kpanic.py`) et, à l'entrée, écrit
   registres, pile, RAM et console dans `.run/panique-<date>/` puis laisse l'invité continuer ;
   côté invité, le rapport CrashReporter d'un `exit 138` est rapatrié par ssh. **Fermeture :**
   une vraie occurrence dépouillée de bout en bout. Coût : une journée. Gain : la condition de
   fermeture du chantier « Plantages en jeu sans cause » existe enfin.
