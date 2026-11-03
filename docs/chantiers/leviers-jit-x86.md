# Les leviers génériques restants du JIT x86 (PC)

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Actif sur le PC ; attend le retour sur cet hôte.

- [ ] **[TCG — PC] Les leviers génériques restants du JIT x86** (attendent le retour sur le PC) (`docs/tcg-g4.md` §44-§47) :
  1. **PGO entraîné avec cmr** : `pgo-train.sh --plein-ecran cmr` (01/11), réentraînement
     fait, A/B neuf contre ancien (`bench/tcg/ab/pgo-0111-*`) : neutre en fenêtre, cmr non
     prouvé (+6 % sur 2 parties, l'une polluée par une charge de l'hôte, `neuf-1` quittée
     `exit 138`). **Ancien binaire remis**, nouveau gardé dans `~/src/qemu/build-fast-pgo0111cmr/` :
     recontrôle cmr 3 parties au repos strict, et entraînement de cmr sur un binaire qui
     atteint la course (l'instrumenté n'y arrive pas en 901 s).
  2. **`tcg/0063` `x-dbat-lite`** : témoin cmr sous `TLBPVERIFY=16 RETVERIFY=1` **sans** 0063,
     pour savoir si la panique de délai de verrou du 01/11 vient des vérificateurs ; puis
     rejouer l'A/B en fenêtre à 3 parties (2 parties : d3 −3 %, mb −2,2 %, ut +3,7 %).
  3. **Recherche de blocs** (mb 10 %, zen 14 %, cmr 6 %) : `tcg/0060` x8 (effacement exact, 256
     cases) gagne mb −8,7 % et ut −1,9 % mais **cmr +4,2 %** en course, seule perte propre à
     cmr qui reste inexpliquée. À instruire : compter ce que coûte le parcours exact sous les
     ~2 000 écritures de BAT par seconde de cmr (32 pages chacune), puis essayer **x8 +
     `tcg/0063`** (plus de vidage du cache de sauts sur un DBAT) ; l'un sans l'autre a été
     mesuré, pas les deux.
  4. **Base de temps** : `helper_load_tbu/tbl` 14 % du vCPU sur ut, 3 % sur zen. La boucle
     `0x33f3c-0x33fec` est identifiée le 02/11 **sur le M4** dans `delay_idle`, attente
     active SMP de xnu-792.24.17 (noyau MD5 `def99c6e…`, `docs/re/ut-attente-m4.md`).
     **Vérifier l'empreinte et les octets du noyau sur le PC** avant d'y transférer ce
     résultat (ancien profil : 11,7 % du vCPU d'ut). Instruire alors un sommeil hôte
     respectant les réveils (ut occupe 1,9 cœur sur le PC), puis juger `mftb` en ligne :
     accélérer cette boucle n'est pas en soi un gain utile au jeu (−2,4 % mesuré pour
     helper lent → rapide).
  **Fermeture de chaque levier :** preuve, A/B des cinq jeux, allumé ou archivé avec le chiffre.

## Liste du PC, reprise de `etat-des-hotes.md` §2 (01/11)

Les points 2 et 6 relèvent de [`alleger-plugin.md`](alleger-plugin.md), le point 7 de [`metrologie.md`](metrologie.md).

1. PGO avec cmr : réentraîné le 01/11 (7 h-8 h 20) ; A/B neuf contre ancien neutre en fenêtre,
   cmr +6 % sur 2 parties (une polluée par mes dépouillements de profils, charge 3,1 ; une
   quittée `exit 138`) : **ancien binaire remis** (`build-fast`), nouveau dans
   `~/src/qemu/build-fast-pgo0111cmr/`. À refaire : A/B cmr 3 parties au repos strict avant de
   trancher (`vitesse-doom3-x86.md` §25). Le binaire instrumenté n'atteint pas la course en
   901 s : allonger `delai_scene` pour l'entraînement, ou entraîner cmr sur le binaire -O2.
2. Plugin sur cmr, suite de PACKBITS : profil de la course refait avec packbits
   (`xprof-0111-cmr-pb`, plugin-allegement §8) : plugin 28,2 % du vCPU, coût étalé
   (`geom_draw_client` 3,7 %, `geom_draw_native`, `send_state`, `prog_sync`, `__stub_getrealaddr`
   1-1,5 % chacun). Piste : `texture_unit_ok` ; qualifier d'abord les adresses de
   `__stub_getrealaddr` avant d'en déduire un coût d'appels par pointeur (relevé M4 du 01/11,
   `docs/re/plugin-symboles-tiger.md`).
3. `tcg/0063` : témoin sous `TLBPVERIFY=16 RETVERIFY=1` **sans** 0063 sur cmr. La panique de
   délai de verrou vient-elle des vérificateurs ? A/B en fenêtre mitigé (d3 −3,0 %, mb −2,2 %,
   ut +3,7 %, 2 parties) : à rejouer à 3 parties si le témoin innocente 0063.
4. x8 (0060) : la seule perte propre à cmr qui reste ; à instruire après le point 1.
5. Base de temps en ligne (`mftb` sans helper), gain modeste attendu.
6. Avis borné pour les surfaces non simples (TODO, chantier plugin) : ramener mb-fen sous
   fenêtre posée au niveau d'avant.
7. Les « sans traduction » de `x-ret-verify` en fin de session (`tcg-g4.md` §44) : peut-être
   parents de la rafale « sans PTE » de `tcg/0061` (§45, non reproduite).
