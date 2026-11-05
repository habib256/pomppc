# Jeux, matrice et OS 9

> Fiche de chantier ; index et priorités : [`TODO.md`](../../TODO.md).

État de la matrice : `docs/matrice-jeux.md` §0.

- [ ] **[Matrice — PC] Un tour complet avec le kext et le plugin du 01/11** : les tours du 01/11
  étaient `--sans-vidage` (cellules rouges par construction) ; il faut des cellules de preuve
  vertes en fenêtre et en plein écran avec le cœur corrigé du 31/10 (dernier tour complet du PC :
  07/10). Le bug de l'ombre est clos (`docs/quartz-extreme.md` journal du 01/11, CHANGELOG) ; sa
  suite, l'avis borné, est dans le chantier plugin.

- [ ] **[Jeux] DOOM 3 et Prey en plein écran** : changement de mode par le jeu (`r_mode 3`,
  `CGDisplaySwitchToMode`, `SURF_PRESENT` sur l'écran redimensionné) et mesure en combat.
- [ ] **[Jeux — session] Marble Blast en fenêtre, bureau 800×600×16** : en session la fenêtre
  échoue sur un bureau trop petit. **Fermeture :** cellules vertes après démarrage frais,
  réglages utilisateur préservés.
- [ ] **[Jeux] Warcraft III** : texte des menus (chemin tableaux, un sommet sans couleur reste
  blanc) ; menu principal juste au 26/09, à revoir en partie.
- [ ] **[Outils] A3, suites** : Zenerchi en plein écran et Warcraft III en fenêtre.
- [ ] **[Outils] Rejeu : `SURF_READBACK` d'une surface jamais liée** — corrigé le 07/10 sur le
  PC. Le tour `20260926-2156` n'existe plus sur le M4 (05/11), et les vidages actuels portent
  `surfaces.txt`, donc n'exercent pas ce cas : leurs rejeux sans `NO_SURF` ne le confirment pas. Reste à
  retrouver un vidage antérieur au 27/09, ou à retirer cette entrée (décision de l'utilisateur).
- [ ] **[Plugin] Mode immédiat sous programme de sommets** : revu sur DOOM 3 (PC, 07/10) ;
  reste Colin McRae au M4 (`POMPPC_GL_VPIMM=0` pour l'A/B).
- [ ] **[Backend GL] `gltest tex14` « λ=2 sans biais »** : défaut du GL de l'hôte macOS.
  Décision : ne réappliquer que sur changement, ou passer le biais dans l'échantillonneur.

- [ ] **[OS 9] Mesurer les gains CPU et élargir les essais** — profils expérimentaux livrés
  le 27/10 (`docs/os9-optimisations-cpu.md`), défaut `g4` conservé. Reste : A/B de vitesse sur
  une charge reproductible, autres jeux et application AltiVec.

## Bloqué

- [ ] **[Jeux] RTCW** quitte après 8 s sans rien dessiner ; absent du disque quotidien.
      **Reprise :** jeu réinstallé sur le disque quotidien, puis diagnostic du lancement.
