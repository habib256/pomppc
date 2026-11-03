# Métrologie et alertes du traducteur

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Questions de mesure encore ouvertes ; l'hôte figure entre crochets ou dans l'index.

- [ ] **[Métrologie] A/B de l'attente** : `QGPU_GL_FLUSH` et `POMPPC_GL_QFLUSH` allumés le 01/10
  sans l'A/B prévu. `tools/matrice/ab-attente.sh 6 qflush` sur hôte au repos ; éteindre si une
  cellule régresse.

- [ ] **[Métrologie] Troisième facteur de lenteur** (`docs/tcg-g4.md` §14.7) : une partie DOOM 3
  sur huit lente de bout en bout ; la matrice relève la charge avant et après la fenêtre.

- [ ] **[TCG — M4] Alertes TLB/RETVERIFY du témoin** : présentes sans `tcg/0066`, séparées de son portage,
  cause inconnue [F] ; procédure dans `docs/tcg-zone-froide-arm64.md` §6.

- [ ] **[TCG] Les « sans traduction » de `x-ret-verify` en fin de session** (`tcg-g4.md` §44) :
  peut-être parents de la rafale « sans PTE » de `tcg/0061` (§45, non reproduite).
