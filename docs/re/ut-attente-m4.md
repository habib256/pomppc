# UT2004 sur le M4 : les horloges chaudes viennent de `delay_idle`

Étude du 02/11/2026, 4 h 31–4 h 43, après l'A/B de 0058/0065.

## 1. Ce qui est mesuré

[A — hôte] L'A/B sans traces, trois parties par bras sur les cinq jeux, rejette
l'attente sous la milliseconde : UT passe de 19,3340 à 21,2784 ms/image (+10,06 %).
Les profils pris ensuite donnent 1,07→1,63 cœur de vCPU, dont 0,013→0,18 cœur
dans `tbf_load`, `cpu_ppc_load_tbu/tbl`, `cpu_get_clock_offset` ; attente de mutex
0,007→0,034 cœur. Le verrou ne suffit pas à expliquer le surcroît de travail.
Ce sont des mesures de l'hôte sous QEMU, pas d'un PowerMac réel.

Les deux relevés diagnostiques suivants sont séparés de cet A/B. Leurs temps
d'image ne lui sont pas ajoutés. QEMU SHA-256
`8568ff593add448f6fbedf719213ab54aa58795291b09bec6cb9c0c9c6997da6`, défauts
du lanceur, sauf `POLLNS=0` ou `2` ; même noyau dans les deux bras :

- `Darwin 8.11.0`, `xnu-792.24.17~1/RELEASE_PPC`, 10/10/2007 ;
- `mach_kernel` MD5 **`def99c6ef0231ea3c2a627dd26201371`** ;
- copie locale `.run/mach_kernel`, symboles et octets lus par `machonm.py`.

## 2. Relier les piles hôte aux instructions invitées

`m4-ut-mappage-0211` prend un `sample` hôte de 30 s après la scène d'UT, avec
`-d in_asm,out_asm -dfilter 0x0..0x100000,0x90000000..0x91000000`. Le journal
et le profil viennent du **même processus** : une carte d'une autre partie est
inutilisable avec l'ASLR. Pilotage : `bench/tcg/ab/pilotage-ut-0211/`.

[D] QEMU v11.1.2, `accel/tcg/translate-all.c`, sortie `OUT` : chaque morceau
hôte porte le PC invité, le premier contenant aussi le prologue. `samplejit.py`
rapproche les adresses sans attribuer le prologue entier au premier opcode.
Pour un helper, il prend le cadre JIT appelant et vérifie un `BL`/`BLR` arm64
à l'adresse rapportée ou quatre octets avant (retour d'appel). Les coûts sont
propres : un parent et son enfant ne sont pas comptés deux fois.

Le nouvel outil laisse sans attribution les adresses réutilisées pour plusieurs
PC, les groupes de PC distincts et les appels non confirmés ; il ne partage pas
arbitrairement un poids entre les adresses regroupées par `sample`. Il parcourt
le gros journal en flux et ne conserve que les adresses utiles. Six tests, deux
mutants détectés (retour sans recul, ancienne carte écrasée par la dernière).

```sh
python3 tools/tcg/samplejit.py sample-hote-mesure.txt out_asm.log \
  --helper '^(tbf_load|cpu_ppc_load_tb[ul]|cpu_get_clock_offset)$' --json horloges.json
```

[A — hôte] Mode court : 3 462 échantillons propres sélectionnés, 3 399 attribués
(98,2 %), dont **3 350 (96,8 %) aux PC 0xaff40, 0xaff44, 0xaff48**.
Défaut : 195 sélectionnés, 131 attribués, dont 129 aux mêmes PC. Le nombre
d'échantillons n'est pas un nombre d'appels. Les absents du filtre et les piles
incomplètes restent dans le dénominateur et le bilan des non-attributions.

[C] Ces trois PC sont les trois premières instructions de **`_mach_absolute_time`**,
symbole du noyau à `0xaff40`. Octets du fichier :
`7c6d42e6 7c8c42e6 7c0d42e6 7c001800 40c2fff0 4e800020` : lecture haut/bas/haut,
contrôle du débordement puis retour. Les mots vus dans `IN` correspondent au fichier.

## 3. Identifier le code qui appelle l'horloge

`m4-ut-appelants-0211` active brièvement `cpu,nochain` via HMP après la fenêtre
d'UT, filtre `0xaff40..0xaff40`, puis coupe les traces. Les états contiennent le
PC et le registre LR. Les retours observés comprennent **0x33eac, 0x33f7c,
0x33fac**, soit les appels de l'horloge à **0x33ea8, 0x33f78, 0x33fa8**.
Ce traçage perturbe fortement l'exécution : ses nombres de lignes ne mesurent
pas un débit d'appels comparable entre bras.

[C] Désassemblage du même noyau : fonction sans symbole propre à **0x33e8c–0x3401c**,
avant `_idle_thread`. Le symbole public précédent est `_run_queue_remove` :
il ne faut pas donner ce nom à toute la plage. La boucle **0x33f3c–0x33fec**
teste les files d'exécution et l'état d'attente ; elle lit l'horloge, réactive puis
coupe les interruptions et appelle `timer_event`. Fichier de preuve :
`m4-ut-appelants-0211/delay-idle-desassemblage.txt`.

[B — logiciel] Le code primaire Apple de la **version exacte 792.24.17** identifie
la fonction statique **`delay_idle`** : sous certaines conditions SMP, elle retarde
le passage au fil inactif. Son initialisation choisit une limite de **60 µs** et
un pas de **1 µs**. Ses trois lectures d'horloge et son traitement des interruptions
correspondent aux trois sites du binaire. Sources :
[initialisation et fonction `delay_idle`](https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/kern/sched_prim.c#L2319),
[constantes de temps](https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/kern/sched_prim.c#L271).
Cette identification porte sur le contrat logiciel de Tiger.

## 4. Suite

L'attente active du planificateur est donc identifiée sur ce noyau. Les réveils
plus précis s'accompagnent de davantage de travail dans cette attente ; il reste
à mesurer les entrées et interruptions sans le traçage perturbateur pour répartir
la perte de débit. Aucun raccourci de PC invité, aucune attente du noyau supprimée,
aucune modification de l'horloge. 0058 et 0065 restent éteints.

La même adresse chaude figurait dans l'étude du PC, mais son noyau doit avoir
son empreinte et ses octets vérifiés **sur le PC** avant de transférer cette
identification. Accélérer uniquement `mftb` ferait aussi tourner cette attente
plus vite : le gain utile sur le jeu reste à prouver.

## 5. Reprise du matin : compter la cadence, puis la borner

[A — hôte, diagnostic] Deux parties séparées, `m4-attente-diag2-0211`, même
binaire diagnostique, même scène ; compteur local au fil autour du vrai poll,
traces des signaux du décrémenteur. Les temps de jeu instrumentés ne sont pas
utilisés comme preuve de vitesse. Médianes de relevés d'environ une seconde :

| scène et mode | attentes positives expirées / s | signaux DEC levés / s |
|---|---:|---:|
| zen, référence | 220 | 763 |
| zen, mode 2 précis | 1 977 | 1 546 |
| UT, référence | 563 | 917 |
| UT, mode 2 précis | 11 261 | 11 135 |

Intervalles et sources dans `bench/tcg/attente-correction-0211/cadence.json` ;
11–15 secondes par cellule, un seul fil observé dans ces relevés. Un poll
non bloquant n'est pas compté comme une expiration de délai ; un signal DEC
levé n'est pas assimilé à une interruption déjà traitée par le noyau.

Deux premiers essais sont archivés :
`archive/m4-pollns-plancher-250us-0211` (diagnostic avec plancher de 250 µs,
la perte UT reste présente), et `archive/m4-pollns-idle-0211` (précision seulement
quand QEMU voit tous les vCPU inactifs ; son pilote ne retrouve pas le gain zen).
Leurs pilotes à une partie par bras ne prouvent ni gain ni neutralité.

**0067, `POLLNS=3`** : budget local au fil, 128 expirations précises dans une
fenêtre de 32 ms, puis retour au `g_poll` courant pendant 250 ms. Les E/S prêtes,
erreurs, replis FD_SETSIZE et polls non bloquants ne consomment pas le budget.
Une fenêtre écoulée réinitialise le compte. Les modes 0/1/2 sont conservés,
le mode est initialisé par `g_once`, aucune horloge ni instruction invitée
n'est modifiée. [F — choix de politique] Ces seuils sont à juger par l'A/B
complet, pas par une propriété supposée du G4.

**587 cas de contrat/routage réussis**, dont temps simulé pour les frontières
32/250 ms, fenêtres séparées et indépendance des fils. Cinq mutants détectés
(repli supprimé, état partagé, E/S comptées, frontière 1 ms, fenêtre stricte).
`.run/pollns-budget-proof-0211.log`, `mutants-budget/` dans le dossier d'étude.

Pilote sans traces, une partie par bras : zen 2,8686→2,3553 ms/image ;
UT 20,1304→19,7263. **Signal encourageant, pas de conclusion de vitesse** sur
un seul passage. L'A/B entrelacé des cinq jeux avec témoin reste à faire.

Le binaire expose le point de trace `pomppc_poll_budget_backoff`, désactivé
par défaut. Le lanceur exige cette capacité avant `POLLNS=3` : un ancien 0058
interprétait toute chaîne non nulle comme le mode 1. La sonde n'amorce pas de
VM ; absence et échec sont refusés. Tests du vrai garde avec chemin contenant
des espaces, et de la vraie sonde (présent, absent, échec).
