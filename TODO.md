# TODO — priorités de POMPPC

> **Deux hôtes (PC x86-64 et Mac M4) en alternance** : l'état de chacun et ce qui y reste à faire
> sont dans [docs/etat-des-hotes.md](docs/etat-des-hotes.md), à lire en premier.

État au 04/11/2026 (PC : lot 0.4.2 préparé, non publié, machine checks à décoder, `docs/etat-des-hotes.md` §2). Objectif : Mac OS X Tiger sous QEMU, bureau et jeux OpenGL
sur le GPU de l'hôte, avec une matrice de jeux verte comme preuve.

**Où on en est.** La fiabilité a été tenue (endurance PC 683 démarrages sans incident le 28/10,
M4 342 démarrages et 40 cycles de jeu sans incident le 29/10, puis 70 démarrages et 8 jeux
avec le nouveau bundle dans la nuit du 01 au 02/11 ; PC 78 démarrages et 16 cycles sans incident avec le bundle du 02/11, le 04/11 ; Quartz Extreme allumé sur les
deux hôtes). **L'orientation est de nouveau la vitesse, sur les deux hôtes** : la cible du PC reste 50 ms, mais depuis le 04/11 elle
ne conditionne plus la publication de la 0.4.2 (décision de l'utilisateur, `docs/etat-des-hotes.md` §2). Vitesse, ms/image, hôte au repos
(PC du 01/11, M4 du 02/11 ; **sur le M4, DOOM 3 est à 37 ms depuis le 03/11**, voir « Ensuite ») :

| jeu | PC x86-64 (binaire rapide, plugin `packbits`) | M4 (02/11, QE) | rapport |
|---|---:|---:|---:|
| DOOM 3, fenêtre | 78-80 | 28,9 | ×2,7 |
| UT2004, fenêtre | 40 | 19,3 | ×2,1 |
| Marble Blast, fenêtre | 8,3 | 6,2 | ×1,3 |
| Zenerchi, fenêtre | 4,6 | 2,6 | ×1,8 |
| Colin McRae, plein écran, **en course** | 82 | 41,7 (1920×1080) | ×2,0 |

M4 : médianes du témoin sans attente précise de l'A/B `m4-pollns-0211`, trois parties,
frontal Cocoa, profils pris après les fenêtres (`docs/tcg-g4.md` §42.2). Ces valeurs
remplacent le relevé sous Time Machine ; elles ne mesurent pas ImGuiDock.

Cible du PC : DOOM 3 à 50 ms/image (`docs/vitesse-doom3-x86.md` : 148,7 au premier relevé du 03/10, §2 ; 111,5 avec le
binaire rapide le 06/10, §13).

Ce que les profils en tirent : [`vitesse-profils.md`](docs/chantiers/vitesse-profils.md).

**Lire cet index.** Une ligne par chantier : ce qui reste, la preuve qui le ferme, sa fiche dans
`docs/chantiers/`. **Au plus trois chantiers actifs par hôte, communs compris.** Les règles d'épreuve et
de travail sont dans `docs/etat-des-hotes.md` §1, la référence validée au §5, l'état de la matrice dans
`docs/matrice-jeux.md` §0, les travaux clos dans `CHANGELOG.md`, tout le reste via `docs/README.md`.

## Actifs

**Sur les deux hôtes**
- [ ] **[Plugin] L'alléger, suite de PACKBITS, avis borné compris**. **Fermeture :** plugin sous 20 % du vCPU
  sur cmr et d3, aucun jeu en perte, matrice verte. [Fiche](docs/chantiers/alleger-plugin.md)

**Sur le PC.**
- [ ] **[Matériel] Machine checks du 04/11 et plantages de l'instrumenté** : cause inconnue. **Fermeture :** une MCE décodée ou la faute expliquée. [Fiche](docs/chantiers/fiabilite.md)
- [ ] **[TCG] Leviers génériques du JIT x86** : `tcg/0063`, recherche de blocs, base de temps ; PGO neutre le 04/11, course de cmr bloquée par le plantage de l'instrumenté.
  **Fermeture :** par levier, preuve et A/B des cinq jeux, allumé ou archivé. [Fiche](docs/chantiers/leviers-jit-x86.md)

**Sur le M4**
- [ ] **[Jeux] Performances selon la résolution** : gros ralentissements de cmr en 1920×1080, autres jeux et
  modes. Rien ne le ferme encore : la piste ImGuiDock de la fiche en donnerait la preuve.
  [Fiche](docs/chantiers/performances-resolution.md)

## Ensuite

Dans l'ordre, l'hôte entre crochets. Un chantier d'un hôte n'y monte que si une place d'actif s'y libère.

0. **[M4] DOOM 3 à 37 ms/image au lieu de 29 depuis le 03/11** : ni le plugin, ni le binaire, ni QE, ni le son, ni les réglages du jeu ; suspect macOS 27.2 [F]. À trancher avant tout A/B sur d3. [Fiche](docs/chantiers/d3-macos272.md)
1. **[PC, puis M4] Performances selon la résolution** : le pendant du chantier actif sur le M4. [Fiche](docs/chantiers/performances-resolution.md)
2. **[Les deux] Endurance sur la configuration par défaut** : gardien ssh, panique #76, une tranche après chaque allumage. [Fiche](docs/chantiers/fiabilite.md)
3. **[Les deux] Plantages en jeu sans cause** : au prochain gel, `kpanic.py` avant tout reset. [Fiche](docs/chantiers/fiabilite.md)
4. **[Les deux] OS 9** : A/B des profils CPU, autres jeux, application AltiVec. [Fiche](docs/chantiers/jeux.md)
5. **[Les deux] A/B de l'attente** et **troisième facteur de lenteur**. [Fiche](docs/chantiers/metrologie.md)
6. **[M4] Alertes TLB/RETVERIFY du témoin**, cause inconnue [F]. [Fiche](docs/chantiers/metrologie.md)
7. **[PC] « Sans traduction » de `x-ret-verify`** en fin de session. [Fiche](docs/chantiers/metrologie.md)
8. **[Les deux] Jeux** : DOOM 3 et Prey en plein écran, Marble Blast 800×600, Warcraft III, A3, rejeu, `tex14`. [Fiche](docs/chantiers/jeux.md)

## Geste de l'utilisateur

Détail : [`utilisateur.md`](docs/chantiers/utilisateur.md).

- [ ] **[M1, macOS 15.8] Éprouver la 0.4.1** ; ailleurs : clics réels, vrais installateurs, dossier partagé.
- [ ] **[PC] Curseur unique sous ImGuiDock**, à voir à l'œil (réglé et confirmé sur le M4 le 02/11).
- [ ] **[Les deux] Replis du plugin à retirer**, après mesure en jeu.
- [ ] **[Les deux] Captures, dix tours verts** : à jouer sur demande.
- [ ] **[PC] Gouverneur `performance`** : un A/B qui demande la racine.

## Pistes à arbitrer

Proposées le 01/11, dans l'ordre recommandé ; rien n'est lancé sans décision. Une piste retenue
devient active si l'hôte a une place.

1. **ImGuiDock contre le frontal natif** : une journée. [Fiche](docs/chantiers/performances-resolution.md)
2. **Régime de compilation du plugin** : deux à trois jours. [Fiche](docs/chantiers/alleger-plugin.md)
3. **Attributs convertis une fois sur l'hôte** : deux jours ; le profil M4 ne la place pas en tête. [Fiche](docs/chantiers/rendu-hote.md)
4. **Boîte noire des paniques** : une journée. [Fiche](docs/chantiers/fiabilite.md)
5. **Temps de chargement des jeux** : une demi-journée pour la mesure. [Fiche](docs/chantiers/chargement.md)

## Le reste

- **Plus tard** (vitesse, maintenabilité du plugin, distribution 1.0, divers) : [`plus-tard.md`](docs/chantiers/plus-tard.md).
- **Écarté sur chiffres**, à ne pas rouvrir sans fait nouveau : [`ecartes.md`](docs/chantiers/ecartes.md).
- **Bloqué** : RTCW, jeu à réinstaller sur le disque quotidien : [`jeux.md`](docs/chantiers/jeux.md).
