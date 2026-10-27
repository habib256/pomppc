# OS 9 : profils CPU expérimentaux (M4, 27/10/2026)

Les optimisations du processeur émulé de Tiger peuvent être essayées sous
Mac OS 9.2.2, avec le même QEMU patché. Le lanceur conserve **un cœur**, les
périphériques classiques et le rendu logiciel. Aucun device qgpu ni plugin
OpenGL Tiger n'est ajouté.

## Utilisation

```sh
OS9_PROFILE=complet SNAPSHOT=1 ./run_os9.sh disk
```

`SNAPSHOT=1` annule les écritures du disque à la fermeture de QEMU. Le dossier
partagé reste, lui, inscriptible ; les essais ci-dessous utilisent un dossier
partagé de banc distinct. Le profil par défaut reste **`reference`**.

| Profil | Ajouts cumulatifs |
|---|---|
| `reference` | CPU `g4` standard, choix de threading TCG d'origine |
| `memoire` | TLB par segments et précis, lmw/stmw et dcbz en ligne, dirty-skip, PTEG par pointeur, TLB par base |
| `flottant` | Flottant rapide, conversions lfs/stfs, AltiVec et flottant natifs, opérandes connus, FPRF paresseux |
| `complet` | Sorties de blocs, cache de sauts par MMU, icbi synchrone, MSR sans BQL, base de temps rapide ; JIT proche et cache de sauts 2^14, PPC et deux voies |
| `verifie` | Profil complet et vérificateurs disponibles ; invalidations TLB et segments échantillonnées à 1/64 |

Les profils explicites refusent une propriété CPU ou TCG absente : un essai
annoncé complet ne se replie pas silencieusement sur un sous-ensemble. La
bannière enregistre le CPU et l'accélérateur effectivement demandés. Les traces
expérimentales `x-tier2` restent éteintes. Le détail des propriétés est dans
`scripts/os9_cpu.sh` ; les dépendances et les refus sont éprouvés par
`tests/os9_cpu_test.py`, intégré à `tests/run-all.sh`.

## Épreuves

Binaire de référence QEMU 11.1.2, SHA256
`890442d11254d510480d5555a69e9b79f3d00807e17307de22802a40c528b20c`,
M4, 512 Mio, un cœur, Screamer/CoreAudio, partage et tablette virtio, disque
`disks/os9.qcow2` ouvert avec `-snapshot`. Scripts et preuves :
`bench/os9-cpu-2710/`. Les timings de démarrage sont indicatifs : Tiger quotidien
reste actif sur l'hôte, et le placement du JIT varie pour les profils sans
`x-jit-near`.

Chaque passage démarre jusqu'au Finder (détection par une région d'image fixe),
lance DOOM II, laisse sa démonstration tourner 90 secondes et enregistre le PCM.
Une nouvelle partie à 100 PV précède l'écran d'aide fixe, comparé pixel par pixel
au témoin. Ce protocole évite une fausse divergence : l'aide ouverte pendant la
démo peut conserver la palette rouge de blessure. Le premier essai flottant a
mis ce défaut du protocole en évidence ; les captures initiales sont conservées.

### Résultats

Sept passages retenus, dans l'ordre : témoin, mémoire, flottant, complet,
vérifié, complet, témoin. Les quatre premiers sont dans `tcg-origine-01..04`,
les trois derniers dans `fin-verifiee-05..07`. Le contrôle d'image du vérificateur
emploie des touches maintenues une seconde : les touches de 100 ms du premier
protocole pouvaient être manquées dans ce mode ralenti. La capture finale est
attendue après le dessin de l'écran d'aide, pas au rythme du passage normal.

**7/7 démarrages et passages DOOM II réussis**, QEMU vivant jusqu'à la fermeture
volontaire, écran d'aide **identique au pixel près** dans les sept cas. Empreinte
RGB : `6d55c93b6d8a286e1746197d7e3e14d6bbb2ae842d83d929b42a63a0a625a98d`.
PCM présent sur chaque passage (95–103 s enregistrées, crêtes 12 125–12 336 sur
32 768). Cela vérifie la production de son, pas l'absence de tout défaut audible.

Derniers compteurs du passage vérifié, démarrage et jeu compris :

| Contrôle | Opérations contrôlées | Divergences |
|---|---:|---:|
| Flottant natif | 1 058 861 391 | 0 |
| TLB par base | 3 266 858 095 | 0 |
| Recherche PTEG | au moins 402 653 184 | 0 |
| lmw/stmw en ligne | au moins 38 010 880 | 0 |
| MSR sans BQL | 5 705 734 | 0 |
| Base de temps | 324 893 863 | 0 lectures rapides en avance |

**Limite AltiVec :** une seule instruction de rangement `stve` contrôlée,
aucune permutation ni opération flottante AltiVec observée. Le profil les
active, mais DOOM II ne les éprouve pas : il faut une application OS 9 qui
les utilise pour compléter cette couverture.

**Barrack 1.0.4f**, en complément : démarrage du jeu, menus, partie animée et
entrée souris contrôlés visuellement sur les profils témoin et complet.
Captures `reference-073107/` et `complet-074455/`. PCM non nul sur les deux
passages (enregistrements d'environ 400 s, crête 8 224, RMS −30,3 / −30,2 dBFS).
Ce contrôle visuel ne refait pas la comparaison pixel par pixel de DOOM II.

Le SHA256 du disque OS 9 est inchangé avant/après la campagne :
`1c4608501b91855f098980d4ca54f9d23c83cbd4f01132b42174ae68444938d9`.
Bilan vérifié par `bench/os9-cpu-2710/validation.py`, données dans
`validation-finale.json`. Suite du dépôt : **287 OK, 0 échec, 6 ignorés**.

Le profil complet est utilisable explicitement. **Le défaut reste `reference`** :
deux jeux et ces passages courts ne suffisent pas pour une validation générale
OS 9. Aucun gain de FPS n'est revendiqué ; il reste à mesurer sur une charge
reproductible, sans autre VM active, puis à élargir les jeux et AltiVec.
