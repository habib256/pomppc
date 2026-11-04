# Métrologie et alertes du traducteur

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

Questions de mesure encore ouvertes ; l'hôte figure entre crochets ou dans l'index.

- [ ] **[Banc] PID du profil hôte avec un `DISK` explicite** (reproduit sur le M4 le 04/11) :
  le `pgrep -f "qemu-system.*disks/tiger.qcow2"` de `matrice.py` et de `matab.sh` peut retenir
  le shell parent, dont la ligne contient les arguments du bras. Dans
  `bench/tcg/ab/m4-d3-terminal-0411`, les profils hôte sont ceux de **bash**, donc inutilisables ;
  les profils invités et les temps restent distincts. **Fermeture :** sélection du vrai QEMU,
  contrôle du nom de l'exécutable dans le profil et test négatif avec un shell parent contenant
  le chemin du disque. Pour l'essai isolé des ombres : `MATAB_REPOS=fixe` et
  `POMPPC_PERF_CMD` sélectionnant l'unique `pgrep -x qemu-system-ppc`, PID conservé à côté du profil.

- [ ] **[Métrologie] A/B de l'attente** : `QGPU_GL_FLUSH` et `POMPPC_GL_QFLUSH` allumés le 01/10
  sans l'A/B prévu. `tools/matrice/ab-attente.sh 6 qflush` sur hôte au repos ; éteindre si une
  cellule régresse.

- [ ] **[Métrologie] Troisième facteur de lenteur** (`docs/tcg-g4.md` §14.7) : une partie DOOM 3
  sur huit lente de bout en bout ; la matrice relève la charge avant et après la fenêtre.

- [ ] **[TCG — M4] Alertes TLB/RETVERIFY du témoin** : présentes sans `tcg/0066`, séparées de son portage,
  cause inconnue [F] ; procédure dans `docs/tcg-zone-froide-arm64.md` §6.

- [ ] **[TCG] Les « sans traduction » de `x-ret-verify` en fin de session** (`tcg-g4.md` §44) :
  peut-être parents de la rafale « sans PTE » de `tcg/0061` (§45, non reproduite).

## Hypothèse du 04/11 : mémoire asymétrique du PC [F]

8 + 32 Gio (`dmidecode`) : le double canal ne devrait couvrir que les 16 premiers Gio, 24 Gio restant en simple canal. La RAM de la VM y tombe-t-elle ?
L'émulation dépend sans doute plus de la latence que du débit, donc rien n'est mesuré. Trancherait : une mesure de débit selon l'adresse physique, ou un A/B après changement de barrettes.

