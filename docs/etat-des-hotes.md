# État des deux hôtes : PC x86-64 et Mac M4

L'utilisateur travaille **alternativement** sur deux machines, avec le même dépôt GitHub
(`origin/main`). Une séance d'agent se déroule sur **une seule** de ces machines. Avant d'agir,
l'agent doit savoir sur laquelle il se trouve et où en est le travail sur chacune. Cette page
donne cet état. **La mettre à jour à la fin de chaque séance**, dans le même commit que le
travail, avec la date et l'heure.

## 0. Sur quel hôte suis-je ?

| `uname -sm` | hôte | binaire QEMU quotidien | défauts de `run_tiger.sh` |
|---|---|---|---|
| `Linux x86_64` | **PC** (Ubuntu 26.04, Core i7-10700F, RTX 4060 Ti) | `~/src/qemu/build-fast` (PGO), référence `~/src/qemu/build` | branche `Linux:x86_64` |
| `Darwin arm64` | **M4** (macOS, GL d'Apple sur Metal) | `~/src/qemu/build` (reconstruit par `scripts/build_qemu_qfb.sh`) | branche `Darwin:arm64` |

Au début d'une séance :
1. `git pull` : l'autre hôte a peut-être poussé.
2. Lire la section de **cet** hôte ci-dessous, puis « À faire en passant d'un hôte à l'autre ».
3. Si `patches/` ou `patches/qgpu/` ont changé depuis la dernière construction locale,
   reconstruire QEMU. Sinon, on mesure un binaire qui ne correspond pas au dépôt.

## 1. Règles communes

Avant et après un lot touchant le plugin, le device ou le cœur : `tools/matrice/matrice.py`
(ou `-j … -m …`), résultats dans `bench/matrice/dernier/tableau.md`.
À la reprise : `pgrep -x qemu-system-ppc`, `tools/guest/tssh.sh uptime`. Mesurer au premier
plan ; DOOM 3 se mesure en parties réelles (`timedemo` refusé par la démo).

- **Vitesse générique** : un levier se juge sur toute la matrice (mb, zen, ut, d3, cmr), jamais
  sur DOOM 3 seul ; il n'est allumé que s'il ne perd sur aucun jeu.
- **A/B au repos** : aucune construction, aucun autre QEMU, aucun agent parallèle pendant une
  mesure ; `MATAB_AVANT="python3 tools/tcg/chargehote.py attendre JOURNAL 1800"`. Trois parties
  par bras ne prouvent pas un écart sous 3 % (bruit mesuré, `vitesse-doom3-x86.md` §18.3).
- **Parité** : une accélération x86 a son pendant arm64 au même niveau de preuve avant
  d'être allumée là-bas, et inversement. Le plugin de l'invité est commun : rien à porter.
- **Patch TCG** : une propriété de CPU ou d'accélérateur, éteinte par défaut, une preuve
  d'équivalence, un A/B entrelacé ; **jamais nommé dans `build_qemu_qfb.sh` pendant une
  campagne** (l'empreinte de la série change, `build-fast` est refusé). Les essais écartés
  restent sous `archive/*`.
- **Pas de repli : étendre le protocole.** Sous programme ARB, tout repli vers le rendu d'Apple
  finit dans `gleBuildInterpolateFunc` → `exit(1)`.
- **`qgpu_proto.h` ⇒ QEMU + plugin** ; **`qgpu_abi.h` ⇒ QEMU + kext (redémarrage) + plugin**,
  et c'est rare.
- **Mesurer avant d'optimiser** : `perf` + `-perfmap` + `tools/re/perfpart.py` sur le PC,
  `sample` dans l'invité et sur le M4 ; `frames.csv` à scène égale. Le self % localise, il ne
  valide pas.
- **Un lot = un changement + une épreuve + un commit**, une ligne dans `TODO.md` tant qu'il est ouvert, une
  entrée dans `CHANGELOG.md` quand il est fini.
- **Endurance** : chaque entrée du TODO passe par le banc quand elle s'y prête.
- **Un seul agent sur la VM à la fois** ; les autres travaillent en copie isolée. L'implémentation
  non triviale passe par des sous-agents Opus ; la session principale orchestre, mesure et
  committe (demande de l'utilisateur, 01/11).
- **Après un `killall` de DOOM 3, redémarrer l'invité.** Ne jamais supprimer `.run/tiger.lock`.
  Jamais `pgrep -f`/`pkill -f` avec un motif présent dans sa propre ligne de commande.
- **Parité, précision** : si une accélération ne peut pas recevoir son pendant, la raison est écrite dans la doc du patch.
- **Publication** : la vitesse du PC n'est plus une condition de la 0.4.2 (décision de l'utilisateur, 04/11) : elle s'annonce
  telle quelle. Cible de vitesse maintenue : DOOM 3 à 50 ms/image en fenêtre.
- **Commits** : ils vont sur `main` puis sont poussés. Un essai non retenu est rangé sous une
  étiquette `archive/*`.
- **Preuve d'une entrée du TODO** : une entrée décrit le travail restant et la preuve qui la fermera. Pour les optimisations :
preuve d'équivalence (`plugmd5` à l'octet, `x-*-verify`, mutants) et A/B entrelacé à scène
égale, hôte au repos, sur **toute la matrice** (mb, zen, ut, d3, cmr) : un levier n'est allumé
que s'il ne perd sur aucun jeu ; un écart sous 3 % sur trois parties ne prouve rien (six parties
ou un bras témoin). Pour les défauts : reproduction avant correction et contrôle après. Les
travaux clos sont dans [CHANGELOG.md](../CHANGELOG.md), les études détaillées dans `docs/`.
L'ancien tableau par domaine : `docs/archive/todo-par-domaines-2026-09-27.md`.

## 2. PC x86-64 : état au 04/11/2026, 15 h 05 (VM quotidienne allumée, binaire rapide au profil du 31/10, frontal ImGuiDock ; suite sur le M4)

**Nuit du 03 au 04/11 : lot 0.4.2 préparé, non publié** (demande de l'utilisateur : AppImage pour vieille glib et
Windows, CI à la POM2, pas de Raspberry Pi). Ni tag ni Release.
- **Fermé sur le PC** : QE résolutions (66 changements sans écart, matrice 8/8, `docs/chantiers/qe-resolutions.md`) ;
  PRESENTQ écarté sur chiffres (`docs/chantiers/presentq.md`) ; `tcg/0066` validé sur x86-64 (`bench/tcg/froid-x86-0411`,
  `docs/tcg-zone-froide-arm64.md`) ; bundle du 02/11 installé, A/B sans perte prouvée (`bench/tcg/ab/plugin-0311-*`).
  Tests : 343 OK, 0 échec, 7 ignorés ; `--slow` : un seul échec, `bridge_probe`, disque tenu par la VM quotidienne.
- **Endurance du nouveau bundle faite** (`docs/endurance.md` §18) : 78 démarrages et 16 cycles de jeu, 0 incident, borne
  haute 4,7 % sur les démarrages. Binaire rapide seulement, Colin McRae non piloté. Base `disks/tiger-endurance-pc-0411.qcow2`.
- **PGO réentraîné, neutre** (`docs/vitesse-doom3-x86.md` §26) : aucun écart prouvé, cmr −3,9 %, ut −2,4 %, d3 +1,6 %, mb +4,2 %. **Profil du 31/10 remis**
  (sha256 `aa043403de5f28e5`), neuf dans `build-fast-pgo0411diag/`. Sans la course de cmr : l'instrumenté plante dessus.
- **À lire au retour sur le PC : plantages de l'instrumenté et 5 machine checks non décodés** le 04/11, cause inconnue [F] (`docs/chantiers/fiabilite.md`).
  `rasdaemon` tourne désormais ; commencer par `ras-mc-ctl --errors`. Production sans trace : 2 tours plantés sur 1 079 journaux, sur l'instrumenté.
- **Pas fait** : `tcg/0063` témoin, x8 + 0063, base de temps. Suite du chantier `leviers-jit-x86.md`, hors condition de la 0.4.2.
- **AppImage** : `linux` vert en CI, plancher glibc 2.31, image `ghcr.io/habib256/pomppc-bullseye-builder` épinglée par
  digest. Essayée depuis le paquet sur ce PC : Tiger, `backend gl`, QE, `gltest` OK. Hôte ancien, sans NVIDIA, X11, son et
  assistant complet **non prouvés** (`docs/publier-linux.md` §5). XWayland forcé (`8e41119`) pas revu à l'œil.
- **Vitesse du paquet** ([A], `bench/tcg/ab/generique-imgui-0411-*`) : générique +7 à +12 % en ms/image contre le rapide.
- **CI** : répétition 10 de `release.yml` **entièrement verte** à 6 h 34 (run `41759736190`, commit `9d0a9ec`) : AppImage 27,6 Mio, zip Windows 17,2 Mio (artefacts).
- **Windows** : sur le runner, Open Firmware en SMP, D-Bus, `Win32.Map`, image et verrou passent. Rien n'a tourné sur un vrai PC Windows : Tiger, rendu GL, vitesse, son non prouvés (`docs/publier-windows.md` §5).
- **Tranché par l'utilisateur le 04/11** : pour ce lot, « au même niveau que le M4 » veut dire mêmes fonctions et mêmes
  preuves, pas la même vitesse ; la vitesse réelle s'annonce dans les notes. La cible de 50 ms reste l'objectif du PC.
- **Décisions à l'utilisateur, aucune prise le 04/11** : (1) déposer l'ISO
  `bench/iso-042/pomppc-0.4.2-guest.iso` (sha256 `a1910765…3e1da`, gravée le 04/11) en pré-publication, refaire
  `packaging/guest-iso.lock` : sans ça un tag 0.4.2 échoue, c'est voulu ; (2) le tag.
- **Pièges** : le QEMU du paquet n'a pas GTK, `matab.sh` exige `MATAB_FRONTEND=imgui` pour le mesurer (sinon « pas de ssh »).
  Toute construction Windows passe sans sonde QMP par stdin (`scripts/build_qemu_qfb.sh`).
- **Le M4 devra reconstruire QEMU** : la série a changé (`tcg/0015`, `qgpu-gl.c`), sans effet hors `_WIN32`.

### État précédent, 01/11/2026, 14 h 30 (A/B PRESENTQ raté, repris depuis)

**Fait le 01/11** (matinée close vers 12 h ; l'après-midi : `pgo-train.sh` et PRESENTQ, fin de
cette liste ; la séance suivante se fait sur le M4)
- **Bug de l'ombre QE corrigé** (agent Opus, `docs/quartz-extreme.md` journal du 01/11,
  CHANGELOG) : la bande d'ancien jeu sous l'ombre du Terminal venait du chemin direct, qui
  n'avisait plus le WindowServer ; kext **méthode 6** (`POMPPC_GA_PRESENT2`) + plugin
  `20261101-ombre` (avis cadencé 20 Hz). Prouvé avant/après sur mb et DOOM 3 au chargement ;
  plugmd5 69 scènes 0 écart. Coût seulement sous une fenêtre posée : mb-fen +6,2 %, d3 +0,9 %.
  Installés dans la VM quotidienne (anciens dans `/var/tmp/avant-ombre/` de l'invité) et dans
  `disks/prebuilt` (anciens dans `bench/ombre-0111/prebuilt-avant-ombre-3010.tar`). Le M4 les a
  depuis le 01/11 à 16 h 57 (§3). Suite : avis borné au rectangle non visible (TODO).
- **Frontal ImGuiDock : double curseur corrigé** (`50a9801`, agent Opus) : curseur de l'hôte
  masqué au-dessus de l'image en souris absolue. Reconstruit, tests OK ; **à voir à l'œil au
  prochain `run_tiger.sh`** (un seul curseur sur l'image, flèche de retour sur les menus).
- Référence cmr-pe du PC validée à l'œil (image de course propre, `references-linux.csv`).
- PGO réentraîné avec cmr : neutre en fenêtre, cmr non prouvé (mesure polluée) ; ancien profil
  en production, nouveau dans `~/src/qemu/build-fast-pgo0111cmr/` (`e1eebc2`). **Piège évité à
  12 h 50** : `pgo-train.sh` avait laissé le profil cmr dans `~/src/qemu/pgo-fast/` (une
  reconstruction l'aurait pris) ; dossiers permutés, `pgo-fast/` = profil du 31/10, le profil cmr
  dans `pgo-fast-pgo0111cmr/` (`docs/binaire-rapide-x86.md` §7).
- `~/src/qemu-jcp/` et `~/src/qemu-bat/` effacés par l'utilisateur à 12 h 10 ; `qemu-nuit`
  contient 0060-0063.
- `tools/tcg/pgo-train.sh` cible **ppc32mt** par défaut (`cf6cf61`) : avant, le défaut
  « ppc64 » héritait du `PPC32MT=1` de `run_tiger.sh` et entraînait en fait ppc32mt ; le bras
  ppc64 pose maintenant `PPC32MT=0`. Les profils déjà construits ne changent pas.
- **`POMPPC_GL_PRESENTQ` rouvert** (demande de l'utilisateur) : A/B au repos en plein écran
  lancé à 13 h 28 (`bench/tcg/ab/presentq-0111-{cmr,mb,ut}`, 3 parties par bras) ;
  verdict et suite dans `docs/chantiers/presentq.md`. Code de l'invité : s'il est allumé,
  le M4 le reçoit avec le bundle. **A/B RATÉ, à rejouer entièrement au retour sur le PC** :
  les 5 parties jouées (13 h 28-14 h 17) ont toutes fini « pas de ssh » parce que le binaire
  rapide a été **refusé** : « construit sur une autre série de patches » (`run_tiger.log` des
  parties). Cause : à 12 h 55 j'avais retouché un **commentaire** de `patches/qgpu/qgpu_proto.h`,
  et l'empreinte de la série (`scripts/qemu_fast.sh`) hache le contenu de chaque fichier de
  `patches/` nommé par `build_qemu_qfb.sh`, commentaires compris. Remis à l'identique à 14 h 20
  (empreinte `5ad32b79ece3943b` retrouvée = `build-fast/pomppc-build.txt`). **Règle apprise : ne
  pas toucher un fichier de `patches/`, même un commentaire, tant que le binaire rapide n'est
  pas reconstruit.** Chaîne arrêtée, aucune VM ne tourne. Au retour : `bench/tcg/ab/presentq-0111-chaine.sh`
  au repos (cmr, mb, ut pe), trancher PRESENTQ (règle dans le TODO), relancer la VM avec
  `./run_tiger.sh`.
- **Dérive de documentation nettoyée en deux passes** (huit agents Opus, `633c444`, `634797b`) :
  voir les messages de commit ; questions laissées ouvertes dans le TODO et les notes datées.
- **x-tb-head fusionné sous `tcg/0064`** (14 h 30, demande de l'utilisateur ; branche
  `worktree-agent-a15011…` renumérotée de 0051 à 0064, worktree et branche supprimés ;
  `docs/anatomie-ppc32.md`, `tools/tcg/codeanat32.py`, `tbhproof.*`). Patch nommé dans
  `build_qemu_qfb.sh`, propriété de l'accélérateur **éteinte par défaut** (`TBHEAD=N`) ; il
  s'applique au bout de la série (essai à blanc sur `~/src/qemu`, 0 rejet). **Conséquence : l'empreinte
  de la série change ; `build/` et `build-fast/` du PC sont à reconstruire avant toute mesure**
  (`./scripts/build_qemu_qfb.sh` puis `QEMU_FAST=1 ./scripts/build_qemu_qfb.sh`, profil
  `pgo-fast/` = 31/10), et le M4 reconstruit de toute façon. A/B en jeu de `TBHEAD=3` à faire
  (`docs/chantiers/plus-tard.md`, Vitesse).

**Fait dans la nuit du 31/10 au 01/11** (objectif de l'utilisateur : vitesse x86 jusqu'à 7 h)
- **Fenêtre de cmr** : depuis le 28/10, le PC mesurait une transition d'avant la course, pas la
  course (`6054367`, `vitesse-doom3-x86.md` §25). En course, la production fait ~82 ms/image.
  Le « 16 bits ×2 » du 31/10 n'était que cette différence de phase : **point clos**.
- **0060 et 0061 rejoués en course** (`tcg-g4.md` §46) : x8 cmr +4,2 % (éteint), 0061 neutre
  partout (abandonné). `tcg/0062` `x-bat-old-mask` (diagnostic du masque d'origine) écrit,
  éteint, non nommé.
- **Plugin `POMPPC_GL_PACKBITS` allumé** (révision `20261101-packbits`, `plugin-allegement.md`
  §8) : bornage des flottants sur leurs bits, octets par table ; plugmd5 69 scènes 0 écart ; A/B
  d3 −3,9 %, ut −4,5 %, zen −4,2 %, mb −1,2 %, cmr −3,5 %. Installé dans la VM quotidienne ;
  `disks/prebuilt` mis à jour (l'ancien bundle dans `bench/packbits-0111/`). **Le M4 le reçoit
  avec le bundle** (code de l'invité, rien à porter).
- **Pourquoi cmr ralentit** (question de l'utilisateur, `tcg-g4.md` §47) :
  - le plugin GL fait 31 % de son vCPU, contre 11-22 % ailleurs ;
  - le PGO n'est entraîné que sur mb, zen, ut, d3 ;
  - les écritures de BAT (`copypv` des `read()`) sont communes à tous les jeux.
  `tcg/0063` `x-dbat-lite` (vidage d'un DBAT réduit à son mmu_idx, sans cache de sauts) est
  écrit et prouvé sans divergence. Il est **neutre sur cmr**, mitigé ailleurs (d3 −3 %, ut +3,7 %), et une session cmr sous
  vérificateurs a fini en **panique noyau** (délai de verrou) : éteint, non nommé.
- Arbre de mesure `~/src/qemu-nuit/build-x` : série + 0060-0063, tout éteint, -O2. La source de
  `target/ppc/mmu_helper.c` y est sans le diagnostic `POMPPC_BATSTAT` ; son diff est dans le
  scratchpad de la séance, à remettre au besoin.

**Vitesse** (binaire rapide, plugin packbits) : DOOM 3 ~78-80 ms/image en fenêtre, Colin McRae
~82 ms/image **en course** (plein écran).

**À faire sur le PC, dans l'ordre** (au retour du M4)
0. Le frontal a changé (`9f7788b`) : `run_frontend.sh` le reconstruit au lancement. **Reconstruire `build/` et `build-fast/` depuis la série du 03/11** (`tcg/0064` à `0067`,
   `usbhid/0002`, `guest-driver` de `qgpu-pci.c`), avant toute mesure (profil PGO du 31/10). Le cœur
   qgpu a aussi changé le 02/11 au soir ; installer
   le kext et le plugin `20261102-modes` du bundle, redémarrer l'invité, puis balayer les modes
   avec `guest/gltest/setmode.c` (`quartz-extreme.md`, journal du 02/11 ; jamais essayé sous NVIDIA). 0066 modifie le finaliseur générique de pools :
   valider les chemins x86-64 sur cet hôte ; les preuves natives M4 ne le remplacent pas.
   Vérifier à l'œil le curseur unique du frontal et la bande d'ombre avec le kext du 01/11 ;
   une cellule de preuve verte de la matrice en fenêtre (les tours du 01/11 étaient sans vidage).
1. Puis la liste de `TODO.md`, section « Sur le PC » ; l'ancienne liste détaillée de cette page est dans
   `docs/chantiers/leviers-jit-x86.md`.

**Défauts du PC** (`run_tiger.sh`, branche `Linux:x86_64`) : Quartz Extreme (depuis le 30/10 au
soir, `QE=0` l'éteint) et la série TCG jusqu'à `tcg/0056`, dont la **zone froide `x-tcg-cold`**
(depuis le 31/10, `TCGCOLD=0` l'éteint). Plugin `20261101-ombre` (PACKBITS, UVWIDE et leviers
A+B allumés, avis d'ombre à 20 Hz), kext du 01/11. `tcg/0057`-`0059` sont compilés et
éteints ; `tcg/0064` (`x-tb-head`, `TBHEAD=N`) est nommé depuis le 01/11 et éteint ; `tcg/0065`-`0067` sont
nommés depuis le 02/11, éteints ou propres à l'autre hôte ;
`tcg/0060`-`0063` sont éteints et **non nommés** dans `build_qemu_qfb.sh` (arbre de
mesure `~/src/qemu-nuit`). Binaire rapide : profil PGO du 31/10 (2 bis), construit avant
`tcg/0064` : refusé par le lanceur jusqu'à sa reconstruction.

**Propres au PC** : `patches/diag/` (auto-profileur, plafonds), `tools/tcg/perfhw.sh` (compteurs
Intel), et l'arbre de mesure `~/src/qemu-ceil` (série et diag, -O2, sans PGO). `perf` exige
`perf_event_paranoid` ≤ 1, que l'utilisateur règle à la main ; la valeur revient à 4 à chaque
redémarrage.

## 2 bis. PC x86-64 : ce qui reste utile des séances du 30 et du 31/10

Réduit le 01/11 : ce qui est repris ou dépassé par la section 2 est retiré ; ses « À faire »
sont fondus dans la liste de la section 2. Le détail est dans les journaux cités.

- **Binaire rapide en production** : profil PGO réentraîné dans la nuit du 30 au 31/10 (cible
  ppc32mt, zone froide, 1 255 `.gcda`, construction à 2 h 43). A/B neuf contre ancien neutre en
  tout (mb, zen, ut −0,6 à −1,4 % ; cmr +0,3 % ; d3 +1,0 %, dans la dispersion) : c'est le
  profil du `build-fast/` d'aujourd'hui (`vitesse-doom3-x86.md` §23), reconstruit avec lui le
  31/10 à 16 h 04 après le correctif des reflets (`binaire-rapide-x86.md` §7). `~/src/qemu/build-fast-pgo2810/` est
  effacé.
- **Zone froide allumée** (31/10, `80aae4e`, `vitesse-doom3-x86.md` §23, `endurance.md` §15) :
  6 parties par bras sans gel de cmr ; A/B d3 −1,8 %, mb −1,2 %, zen −2,2 %, cmr −2,2 %, ut
  neutre. Avec la zone froide, cmr compte 20 parties pilotées et 1 gel (le 28/10), aucun dans
  les 17 suivantes. Endurance : 10 démarrages et 8 cycles de jeu, 0 incident.
- **Profil croisé des 5 jeux** (§23) : postes communs, la recherche de blocs (`qht`,
  `lookup_tb_ptr` : 10 % mb, 13,7 % zen, 6,1 % cmr, 3-4 % d3/ut) et la base de temps (`mftb`
  13,8 % sur ut, 1-3 % ailleurs ; sur ut, surtout la boucle au repos du noyau, `tcg-g4.md` §44).
- **`matab.sh --restore`** retire les variables de binaire héritées et affiche le binaire lancé
  (`6510319`, cause de la VM restée sur `~/src/qemu-ceil/build` le 28/10).
- **Reflets de la carrosserie de cmr** (31/10, `928a5f0`, `docs/re/cmr-rendu-vers-texture.md`
  §7) : défaut du cœur qgpu de l'hôte (`unit_texture_bound` respectait mal l'unité éteinte par
  le plugin sous programme de fragments) ; plus de reflet, comme sur un vrai Tiger/PowerPC qui
  se juge lent. `build/` et `build-fast/` reconstruits ; nouvelle référence cmr-pe (tour
  `cmr-nouvref-3110`) **validée le 01/11**, l'ancienne dans
  `bench/matrice/ref-linux/cmr-pe.faux-reflet-2810/`. Plancher de cmr : `plancher_ms_linux`
  105 depuis la mesure en course (`6054367`).
- `gltrap` : `POMPPC_GLTRAP_BINDS=1` (unité active et liaisons depuis le lancement), déposé
  dans `/Users/tiger/gltrap/` de la VM quotidienne.
- **Premiers essais de `tcg/0060` et `tcg/0061`** (`tcg-g4.md` §44-§45) : repris en course le
  01/11 (§46, section 2). De `0061` reste un défaut d'origine de QEMU : l'ancien BAT est vidé
  avec le masque de la nouvelle valeur (`tcg/0062` le relève ; `docs/chantiers/plus-tard.md`).
- **cmr à plusieurs résolutions** (`bench/tcg/ab/cmr-res-3110/`, sous ImGuiDock) : rejeux
  identiques partout (0,00 %), captures `<mode>/capture.png`. Les temps de ce tour sont pris
  **avant la course** (fenêtre de mesure d'avant `6054367`) : le « 16 bits ×2 » qui en sortait
  est clos (section 2).
- cmr, « couleurs, flous, reflets bizarres » signalés pendant la campagne du 31/10 au matin :
  tour avec preuve d'image vert, identique à la référence d'alors (0,00 %), non reproduit ; un
  premier essai avait gelé au lancement (aucune image GL).

## 3. Mac M4 : état au 04/11/2026, 21 h 24 (écart DOOM 3 expliqué ; VM quotidienne sous ImGuiDock ; push bloqué par l'authentification)

**Reprise du 04/11 au soir, priorité DOOM 3 du TODO.**

- **Chantier clos : 29 → 37 ms expliqué par `r_shadows=0` → `1`**, et non par une régression macOS démontrée
  (`docs/chantiers/d3-macos272.md`). Sous macOS 27.2 : copie du disque immuable du 01/11 à 29,361 ms,
  copie actuelle à 37,349 ms avec le paquet 0.4.1 ; sur la copie actuelle et le QEMU reconstruit,
  seul `D3_SET=r_shadows=0` puis `=1` donne **29,221 puis 37,039 ms**. Quatre parties, zéro repli,
  deux captures après profils regardées. **Référence courante : 37 ms avec ombres activées.**
  Les anciens profils à 29 ms étaient sans ombres ; TODO, profils et CHANGELOG précisés.
- **QEMU reconstruit depuis `fd78c51`**, `.run/build-reprise-0411.log`, capacités complètes,
  SHA-256 `d6e6e3e2caf092f66…`. Aucun levier supplémentaire allumé, aucun fichier de `patches/`
  modifié ici, pas de nouvelle installation du kext ou du plugin.
- **Piège du contexte de lancement** : depuis le service des outils, QEMU était à `PRI=4` et
  très lent, avec les deux binaires ; depuis le Terminal, application reconnue et vitesse normale.
  Les deux tentatives lentes sont conservées mais ne fournissent aucun temps de niveau.
  Reprendre les A/B **depuis le Terminal**, écran réveillé. Le mécanisme fin reste [F], ne pas l'appeler App Nap.
- **Défaut du banc reproduit, à corriger** : avec un `DISK` explicite, `pgrep -f` peut choisir le
  shell parent pour le profil hôte et l'attente du bureau. Profils hôte de `m4-d3-terminal-0411` = bash,
  écartés ; pour l'A/B isolé, attente fixe et sélection exacte du QEMU via `POMPPC_PERF_CMD`, PID vérifié.
  Procédure et fermeture dans `docs/chantiers/metrologie.md`.
- **État final** : campagnes arrêtées proprement, essais sur copies isolées ; VM quotidienne relancée
  depuis le Terminal par `matab.sh --restore`, ImGuiDock et QEMU vivants après la fin du lanceur,
  bureau regardé et SSH contrôlé à 21 h 18. Kext GPU et Fsqrt chargés, `r_shadows=1` conservé.
  Aucun verrou supprimé. Contrôles de cette séance : capacités QEMU, quatre tours de vitesse,
  deux captures et cohérence des documents ; aucune nouvelle matrice complète ni endurance revendiquée.
- **Suite utile sur ce M4** : A/B ImGuiDock contre natif et résolutions (`performances-resolution.md`),
  puis avis d'ombre actif / plugin. Le doute sur les 29/37 ms ne bloque plus ces comparaisons.
  À la remarque de l'utilisateur sur les +8 ms : profils comparés, passe stencil 13,2 % du fil
  principal, 65/114 échantillons de cette passe dans `geom_draw_client`. Piste géométrique générique
  ajoutée à `alleger-plugin.md`, avec les ombres conservées ; aucune économie récupérable encore prouvée.
- **Push à terminer après réauthentification** : `git push origin main` échoue sur les identifiants
  HTTPS, `gh auth status` indique un jeton invalide ; `ssh -T git@github.com` refuse la clé publique.
  Travail committé sur `main` en local, arbre propre après commit ; utilisateur invité à refaire
  `gh auth login -h github.com`, puis pousser. Aucun tag ni Release créé.

### Suite du 04/11, 23 h 16 : récupération du coût des ombres, contrôle CMR en cours

- **STPATCH prototype éteint par défaut**, `docs/plugin-stpatch.md` : preuves natives et
  mutants ; premier prototype compilé dans Tiger sur copie isolée, 69 images plugmd5 égales,
  mb/zen/ut/d3 verts, mode 2 D3 à 1 225 512 candidats sans divergence. **Aucun A/B de vitesse**.
  b1 rétablit une frontière de flux omise pour les candidats sans clé ; natif passé,
  compilation et preuves VM b1 restent à faire. QEMU et kext inchangés.
- **CMR signalé défectueux par l’utilisateur au titre 3D**, aplats blancs en 1920×1080.
  Reproduit en mode 0 ; diagnostic de résolution prioritaire. Essai 800×600 en cours dans
  la copie `bench/tcg/ab/m4-d3-disque-0411/disques/actuel/disks/tiger.qcow2`, frontal natif.
  Le disque quotidien n’a reçu aucun plugin expérimental ; il est arrêté pendant cet essai.
  Pas de nouvelle référence CMR validée sur une image défectueuse.
- **Référence D3 M4 renouvelée avec ombres**, témoin `m4-ombres-base-0411-fen/ref-1`,
  ancienne sans ombres conservée ; seul `references.csv` modifié, pas celui du PC.
- Contrôle général depuis Terminal : **522 OK / 0 échec / 5 ignorés** ; correctif Bash 3.2
  du test d’empaquetage Windows committé séparément. L’ancien échec POLLNS sous service
  ne se reproduit pas depuis Terminal ; ne pas lancer les bancs depuis le service.
- Push toujours à terminer après réauthentification. Preuves locales sous
  `bench/plugin/ombres-0411` et `bench/tcg/ab/m4-stpatch-*`.

### État précédent, 03/11/2026, 16 h 55

**Après-midi du 03/11, demande de l'utilisateur : suivre le TODO jusqu'à 16 h 30.**
- **Preuve QE aux résolutions : faite sur le M4** (`docs/chantiers/qe-resolutions.md`) : 16 cellules vertes sur 16, Colin McRae depuis quatre bureaux, A/B du plugin `20261102-modes` sans perte. Neuf références refaites et regardées.
- **Frontal : plantage corrigé** (`9f7788b`, non publié) : lancé écran du Mac éteint, `pomppc` mourait à la première image, **0.4.1 touchée**. Correctif de version à décider par l'utilisateur.
- **DOOM 3 à 37 ms/image au lieu de 29 sur ce M4** depuis le 03/11 : le soupçon macOS 27.2 et
  l'exclusion des réglages étaient erronés, corrigés le 04/11 ci-dessus (`docs/chantiers/d3-macos272.md`).
  **Ne comparer qu'à `r_shadows` égal.**
- **A/B ImGuiDock contre natif : pas fait**, les premières parties tournaient sans frontal (`docs/chantiers/performances-resolution.md`).
- **Pièges :** écran éteint, mb-fen 8,5-9 ms contre 6,2-6,9 : allumer avant de mesurer (`caffeinate -u -t 3`, `caffeinate -d` seul ne réveille pas). `tools/tcg/matab.sh` gagne `MATAB_APRES`. Le disque quotidien démarre en **1024×768×16** (préférences du 02/11 à 18 h 51) ; le bureau en 32 bits se règle pour la session par `/var/tmp/qe0311/setmode` (invité du M4 seulement). Kext et plugin neufs reposés, empreintes vérifiées ; copies des deux bras dans `/var/tmp/bras0311/` de l'invité.
- Aucun fichier de `patches/` touché depuis `9702c26`. `tests/run-all.sh` après le correctif du frontal : 485 OK, 0 échec, 4 ignorés. `bench/` reste local : les preuves citées ne suivent pas sur le PC.

**TODO réorganisé (3/11, 13 h 07, demande de l'utilisateur).** `TODO.md` devient un index par hôte, au plus trois chantiers actifs par hôte, avec une fiche par chantier dans `docs/chantiers/`. Texte déplacé tel quel. Règles de travail au §1 de cette page, référence validée au §5, état de la matrice dans `docs/matrice-jeux.md` §0, table des lectures dans `docs/README.md`. Aucun fichier de `patches/` touché : l'empreinte de la série ne change pas. Rien de mesuré, aucune VM lancée.

**Dérive de documentation (3/11, matin, demande de l'utilisateur).** Relecture contre le code de ce qui a bougé
depuis le 01/11 : README, TODO, frontal, recette de publication, `patches/README`, `tcg-g4`, parité M4 ; puis
Quartz Extreme, `gpu-3d-tiger` §6, architecture §9, index `docs/re`, aide de `run_os9.sh` ; enfin la doc confrontée
au code en sens inverse (libellés du README, symboles cités). Le chantier
« TCG — M4 » du TODO a atteint sa fermeture et en sort (détail au CHANGELOG 0.4) ; « TCG — PC » passe en tête
d'« Ensuite », car il attend le PC. Aucun fichier haché de `patches/` touché : l'empreinte de la série ne change pas.

**Soir du 02/11, demande de l'utilisateur : QE doit tenir toutes les résolutions.** Corrigé
(`docs/quartz-extreme.md`, journal du 02/11) : kext (VRAM entière), plugin `20261102-modes`, cœur
qgpu (rectangle jusqu'à `QGPU_MAX_SURF_DIM`). `~/src/qemu/build` reconstruit, `disks/prebuilt` à
jour (ancien : `bench/prebuilt-0211/avant-modes.tar`), VM quotidienne installée et redémarrée à
18 h 16 (anciens binaires dans `/var/tmp/avant-0211/` de l'invité). 44 changements de mode justes,
467 OK. **Le disque quotidien reste réglé en 1920×1080×16 par l'utilisateur** : la matrice de
preuve y sortirait d'un autre réglage que ses références ; revenir au réglage d'origine pour la
jouer. Matrice, A/B et DOOM 3/UT2004/cmr à ces résolutions **pas faits** (`docs/chantiers/qe-resolutions.md`).
**Curseur en plein écran : réglé, confirmé par l'utilisateur le 02/11.**

**Version 0.4 publiée le 02/11 à 20 h 10** à la demande de l'utilisateur
(https://github.com/habib256/pomppc/releases/tag/0.4, tag `0.4` sur `c7cd63a`). Cible **macOS 15.0,
sans Homebrew** (les 25 binaires, garde-fou `MIN_MACOS`) ; recette et arbres dans
`docs/publier-macos.md`. Archive éprouvée sous macOS 27 sur un M4 seulement, les notes le disent.

**Version 0.4.1 publiée le 03/11 à la demande de l'utilisateur** (https://github.com/habib256/pomppc/releases/tag/0.4.1,
tag `0.4.1` sur `2fac68c` ; `.dmg`, archive, CD et sommes vérifiés après retéléchargement). L'essai sur le M1 de
l'utilisateur sous macOS 15.8 reste à faire : c'est le plus gros doute restant.

**0.4.1, ce qui a été préparé (21 h).** `POMPPC.app` dans `dist/POMPPC-0.4.1-macos-arm64.dmg`, commit
`f5ea5a4` : assistant de premier lancement, combo 10.4.11 téléchargé chez Apple avant le pilote, données dans `~/POMPPC`, frontal en anglais. Essayée
ici depuis le `.dmg` avec Tiger et OS 9 ; assistant couvert par `tests/app_wizard_test.sh`, réponses
simulées. L'assistant ouvre la page de Macintosh Garden et attend le fichier : il ne télécharge rien
lui-même. **Pas éprouvé :** accord de Gatekeeper sur une app réellement téléchargée, clics réels dans
l'assistant, installation complète (pas de média ici), macOS 15 et M1 (M1 de l'utilisateur sous
15.8), Colin McRae, matrice. Sans système installé, l'autre système ne s'ajoute pas depuis le
frontal : son entrée du menu OS reste grisée.

**Ergonomie, lots 1 et 2 (22 h, dans la 0.4.1).** Arrêt propre par la touche d'alimentation (`send-key
power` ; `system_powerdown` sans effet sur mac99), confirmations, écran d'arrêt ou d'échec, panneaux fermés
et fenêtre à l'invité, pomme d'Apple dans l'app (`POMPPC_BOOT_ARGS`). `POMPPC_FE_SCRIPT` joue de vrais clics
dans ImGui, et `glshot` capture la fenêtre seule : à utiliser plutôt qu'une capture de l'écran.

**Ergonomie, lot 4 (3/11, après la 0.4.1, non publié).** Le lot 4 de l'étude (dossier partagé, glisser-déposer,
Preferences) était déjà livré pour l'essentiel. Restait : son et manette dans Preferences, sous le même
**Apply and Restart**, la définition courante avec l'endroit où la changer, et **Show Folder** dans une Library
vide. Pas de réglage de résolution côté hôte : `RES` n'atteint pas le bureau des invités [A]. Reste hors lot : point 1
(veille pendant l'installation), barre de menus native.

**Dossier partagé (3/11, dans la 0.4.1).** Demande de l'utilisateur : un disque correspondant à un dossier du Mac.
`vvfat` essayé puis écarté (503 Mo, fichiers vus vides). Image HFS+ et retour à l'éjection ou à l'arrêt
(`SharedFolder`, panneau *Disks*) : **une copie, pas une vue en direct**. Éprouvé au clic et par ssh, disque quotidien
en `SNAPSHOT=1` : Tiger (éjection, `kill -9` puis reprise sans suppression, arrêt normal, *Connect Again*), OS 9
(montage, éjection vue par `tray_open`, fichier dupliqué et dossier créé revenus). **Pas éprouvé :** accord de
confidentialité de macOS sur Documents ou Bureau pour l'app signée ad hoc, très gros dossier, écriture sous OS 9
d'un fichier avec resource fork. La vraie Corbeille (`/usr/bin/trash`) a été éprouvée depuis l'app du `.dmg`.

**Ergonomie, points 2 à 8 (3/11 minuit, dans la 0.4.1).** `usbhid/0002` corrige la mort de l'OHCI (cause : le
garde-fou `ED_LINK_LIMIT` comptait les paquets ; **QEMU à reconstruire sur le PC**). Glisser-déposer par disque USB
à chaud (`AppExtras`), police Roboto, rappel de capture, menu Help, Preferences, filtre net, Try Without Saving,
capture. Le lecteur CD IDE ne convient pas au dépôt : Tiger le verrouille, et un échange forcé lui échappe. Le
point 1 (veille pendant l'installation) n'a pas été demandé : toujours ouvert.
Éprouvé au clic sur l'app du `.dmg` (commit `43ea833`, sur des clones de disque) : dépôt USB dans Tiger (lecture,
écriture, relecture sur le Mac) et OS 9 (« From Mac » sur le bureau), 10 Mio sur l'OHCI sans erreur, Preferences
(`-m 512` après Apply, réglages relus au lancement), Try Without Saving (`-snapshot`), Sharp/Smooth, capture,
détection de mise à jour, toast plein écran ; 484 tests OK. Une épreuve `POMPPC_FE_SCRIPT` n'écrit pas
`imguidock.conf` : y vérifier un réglage dans la même exécution. **Pas éprouvé :** écran Retina, M1 sous macOS 15,
vrai installateur, A/B de vitesse non rejoué.

**Ergonomie, lot 3 (23 h, dans la 0.4.1).** Assistant unique dans le frontal (`SetupFlow`, `SetupUi`), reprise
par `.run/setup.state` ; `-no-reboot` enchaîne les étapes d'installation ; installateur `Install POMPPC.app`
compilé dans Tiger (job `prebuilt`) ; propriété `guest-driver` de qgpu-pci. Essayé au clic (`POMPPC_FE_SCRIPT`) depuis le `.dmg` : pages, reprise, faux CD (Open Firmware), étape écourtée, vrai combo téléchargé chez Apple et son CD dans Tiger, puis pilote retiré d'un clone, **Install POMPPC**, mot de passe, redémarrage, détection. **Pas éprouvé : un vrai installateur de Tiger ou d'OS 9 (pas de média ici : le démarrage `cd:,\\\\:tbxi`, le `-no-reboot` à la fin réelle), l'application réelle du combo sur une 10.4.6, la fenêtre de fichiers de macOS (remplacée par `POMPPC_SETUP_CHOOSE`), le vrai navigateur.** **`patches/qgpu/qgpu-pci.c` a changé :
le PC reconstruit `build/` et `build-fast/`.**

**Panneau Disks (21 h 30, dans la 0.4.1).** Démarrage + 2 disques au plus, HD ou SSD, création.
Essayé ici : Tiger voit 3 disques, formate le SSD, le volume revient au redémarrage ; démarrage en SSD
seul ; lancement d'OS 9 avec un disque attaché (QEMU l'attache, sa vue dans OS 9 n'est pas contrôlée).
Le SSD n'est qu'une étiquette, Tiger n'en a pas la notion.

**État du 02/11 à 13 h 34, avant le soir :** QEMU reconstruit avec **0066/0067**, kext,
plugin et paquet `prebuilt` à jour. Zone froide arm64 implantée et validée ; attente
courte corrigée par un budget progressif. Matrice des cinq jeux close, **60 cellules
sans anomalie ni repli**, six preuves d'image séparées vertes. Aucun nouveau levier
allumé : gain zen, petit coût possible d3, pas de gain générique. Contrôle général
**470 OK, zéro échec, une exclusion Linux**. VM quotidienne restaurée à 13 h 33 sous
ImGuiDock, frontal détaché (parent PID 1), QEMU vivant, SSH et kext contrôlés à 13 h 34.
Reste le coût des avis d'ombre actifs (§4).

**Matin du 02/11, demande de correction et de portage** (base `5067b22`) :
- **Attente 0067, `POLLNS=3`** : 128 expirations précises en 32 ms par fil, puis
  retour temporaire au `g_poll` courant ; si les rafales reprennent, paliers de
  250 ms à 2 s. Les E/S ne consomment pas le budget, initialisation synchronisée.
  1 635 contrôles, neuf mutants. Aucun code ni horloge de Tiger modifié. Le diagnostic
  séparé compte ~11 000 expirations/s sous UT en mode 2, ~2 000 sous zen ; les temps
  instrumentés ne servent pas d'A/B. Trois prototypes rejetés sont sous étiquettes
  `archive/m4-pollns-{plancher-250us,idle,budget250}-0211`.
- **Zone froide arm64 0066** : relais `B` imm26 en fin du chaud pour les cinq
  callbacks, régions nominales ≤64 Mio même en SMP=1, pools froids séparés et état
  chaud rétabli sur erreur. 53 cas natifs M4, cinq mutants ; tests invités sans/avec
  froid en SMP=1/JIT éloigné et SMP=2/tampon 64 Mio, sorties identiques, 137
  renouvellements de tampon avec froid. Fautes PC/DAR exactes (500 984 cas tlbb,
  309 224 fautes) ; **2 560 exceptions FP au PC exact**, aucune erreur. Les alertes
  TLB/RETVERIFY déjà présentes dans les témoins restent [F], séparées du portage,
  avec procédure pour les instruire (`tcg-zone-froide-arm64.md` §6).
- **Matrice finale 0066/0067, close à 13 h 28** : quatre bras référence, témoin
  identique, attente progressive, attente + froid ; trois passages chacun, mb/zen/ut/d3
  en fenêtre, cmr en course 1920×1080. Hôte au repos et charge continue valide,
  60 cellules sans anomalie/repli. Attente seule : zen −9,08 %, UT +1,01 % ;
  ensemble : zen −10,06 %, UT +0,23 %, d3 +1,09 %, mb −0,76 %, cmr +0,65 %.
  UT recouvre les contrôles, CMR dans leur dispersion ; petit coût d3 possible.
  **Options conservées éteintes**, aucun gain générique revendiqué.
  Données `bench/tcg/ab/m4-progressif-0211-{fen,pe}/`, provenance et synthèse dans
  `m4-progressif-0211/` ; détails `re/ut-attente-m4.md` §5–7. Six preuves d'image
  distinctes vertes avec 3/1, références validées réutilisées, zéro repli.
- **49 scripts ShellCheck corrigés** sans désactivation globale ; nettoyages testés
  sur noms avec espaces, retour à la ligne et tiret initial. Différentiel softfloat
  QEMU 11 réactivé : 1 673 079 cas sans divergence ; QFB/QGPU/pont D-Bus passent.
  Contrôle final isolé `./tests/run-all.sh --slow` : **470 OK, zéro échec, un ignoré**
  (binaire rapide Linux absent par nature sur ce M4), `.run/tests-cloture-0211.log`.
- **Binaire mesuré et restauré** : `~/src/qemu/build`, reconstruction progressive
  `.run/build-budget-progressif-0211.log`, sources figées avant A/B ; empreintes exactes
  dans `bench/tcg/ab/m4-progressif-0211/provenance.json`. Construit sur l'arbre de
  travail du matin avant son commit ; ne pas attribuer ce binaire au seul HEAD de
  base inscrit par les anciens rapports. Aucun QEMU de campagne restant, verrou
  conservé. Retour sur PC : reconstruire **référence et rapide** puis valider le
  finaliseur générique des pools modifié par 0066 ; aucun contrôle Linux revendiqué.

**Nuit du 01 au 02/11, reprise à 22 h 31** (demande de l'utilisateur : suivre le TODO jusqu'à
6 h 40, heure Réunion). Dépôt à jour sur `aad7721`. À la reprise, `.run/` et `bench/` sont
absents : anciens A/B, références binaires et script de reprise indisponibles. Le manifeste
versionné des références reste présent ; **ne pas confondre la nouvelle preuve avec un rejeu
des anciennes références**.
- QEMU reconstruit depuis HEAD, capacités complètes (`.run/build-nuit-0111.log`), aucun levier
  supplémentaire allumé. VM quotidienne : plugin `20261101-ombre` et kext chargés ; accès ssh
  du banc rétabli (nouvelle clé locale, jamais versionnée).
- Job `prebuilt` compilé par ssh dans la VM, sans installation : kext, plugin, GA, gltest,
  glwin et accelprobe rapportés dans `disks/prebuilt`, `VERSION=20261101-ombre`. Ancien paquet
  dans `bench/prebuilt-0111n/avant.tar`, journaux de compilation à côté.
- Preuve mb-fen `bench/matrice/preuve-nuit-0111-mb` : 6,2 ms/image, zéro repli, rejeu/VM
  0,03 / 0,02 %. Nouvelle référence regardée (bille au départ de gems.mis) et validée ;
  ancien enregistrement conservé dans l'historique Git.
- Campagnes **neuves prévues** `m4-bundle-0111n-{fen,pe}`, puis `m4-profils-0111n-{fen,pe}` par
  `.run/chaine-nuit-0111.sh`, après les jeux du banc (`.run/nuit-suite-0111.sh`, journal
  `.run/nuit-suite-0111.log`). Trois bras entrelacés :
  `avant` (PACKBITS=0, QE_NONSIMPLE=0), `bits` (1,0), `actuel` (1,1) ; trois parties par bras,
  mb/zen/ut/d3 en fenêtre, mb/cmr en plein écran. PACKBITS est ainsi séparé du coût de l'avis
  d'ombre. Barrière `chargehote.py attendre` et relevé continu de charge ; Time Machine arrêté.
  Ensuite profils hôte 30 s et invité 10 s avec les défauts, sur les cinq jeux.
- À 22 h 39, l'utilisateur confirme l'ombre correcte sur DOOM 3 avec la correction active ;
  le défaut qu'il venait de voir appartenait au bras `avant` (QE_NONSIMPLE=0). Campagne
  interrompue pour lever le doute, début conservé dans `m4-bundle-0111n-fen.interrompu-ombre` ; captures de la
  vérification dans `bench/ombre-m4-0111n/`. Rien à rouvrir dans le correctif.
- Un autre agent compile Swift sur l'hôte : pas d'A/B pendant ce travail. Barrière locale
  `.run/repos-strict-0111.py` avant les parties. En attendant, preuves d'image neuves
  `preuve-nuit-0111-{fen,pe}` (temps non retenus comme preuve de vitesse) ; Zenerchi regardé
  et validé aussi. `threadbusy.py --fonctions N` préparé pour le dépouillement des profils,
  coûts propres par fil et attentes séparées, quatre tests hors VM réussis.
- Tests de départ : 397 OK, 49 signalements ShellCheck préexistants, 5 ignorés. Les campagnes
  de vitesse sans vidage restent rouges par construction ; elles ne prouvent pas les images.
- Symboles du plugin et de libSystem identifiés par les binaires de cette VM
  (`docs/re/plugin-symboles-tiger.md`, empreintes et adresses exactes). Le coût attribué à
  `___stub_getrealaddr` n'établit pas à lui seul un coût d'appels par pointeur : distinguer
  le résolveur des stubs voisins sur les prochains profils. `0x90131200` appartient ici à
  `memcmp`/`bcmp` ; appelants et identité de la bibliothèque sur le PC restent à vérifier.
  `machonm.py --stubs` donne les 82 importations, identiques à `otool -Iv` ; trois tests
  hors VM passent. Aucun code de rendu changé.
- **Preuves neuves terminées à 22 h 54 : six cellules vertes sur six**, après validation à
  l'œil des références et réanalyse : `preuve-nuit-0111-fen` (mb, zen, d3, ut),
  `preuve-nuit-0111-pe` (mb, cmr). Zéro repli partout ; rejeu/VM 0,00/0,00 %, sauf mb-fen
  0,03/0,02 % et mb-pe 0,02/0,01 %. cmr-pe en course, 1920×1080, image sans le faux reflet.
  Les temps (6,6 / 3,1 / 30,3 / 19,9 ms en fenêtre, mb-pe 6,7, cmr-pe 44,6) passent les
  planchers mais **ne servent pas d'A/B** : autre agent actif, contrôles et analyses sur
  l'hôte. Les références des autres jeux/modes restent à reconstituer si leur tour est repris.
- VM éteinte proprement par le banc à 22 h 53 ; première barrière de l'A/B neuf en attente
  de la fin de l'agent externe. Avant la reprise, base d'endurance fraîche créée depuis la
  quotidienne arrêtée : `disks/tiger-endurance-m4-ombre-0111n.qcow2`, clone APFS autonome
  sous verrou, sources et tailles dans `bench/endurance/base-ombre-0111n.json`.
- Contrôle général après les outils : 400 OK, mêmes 49 signalements ShellCheck qu'au départ,
  5 ignorés ; nouveau test Mach-O exécuté et compilé séparément (non encore indexé lors du
  parcours `git ls-files` du harnais).

- **Contrat de 0058 corrigé**, sans allumage : `tcg/fixes/0058-poll-ns-contract.patch`,
  nommé par le constructeur. Quatre écarts reproduits contre `g_poll` de glib 2.90.1 sur
  ce M4 ; 42 cas sans écart après, deux mutants détectés (`pollnsproof.*`, `tcg-g4.md` §42.1).
  QEMU reconstruit, capacités complètes (`.run/build-pollns-contract-0111.log`), fonctions
  réellement construites recontrôlées. L'empreinte de la série change : le PC reconstruit
  toujours `build/` et `build-fast/` à son retour, même si ce chemin n'y est pas compilé.
- **Endurance : 10 démarrages froids sans incident** (22 h 57–23 h 05, binaire de 22 h 31,
  `977c844cd5d8b18b`, bundle ombre). Défaut du banc découvert à l'interruption de la phase
  suivante : Ctrl-C pendant l'écart initial, hors du `try`, laissait sa VM tourner. Instance
  isolée fermée proprement par ssh ; nettoyage du banc corrigé, quatre tests réussis et
  contrôle réel à deux instances prévues (seule la première démarre), aucun QEMU résiduel.
  `docs/endurance.md` §16. **Jeux terminés** : `m4-ombre-0111n-jeux-corr`, 5 démarrages et 4 cycles mb/zen/ut/d3
  sans incident, binaire `b8921ddf205e6b22`, `POLLNS` toujours éteint. Le premier essai `m4-ombre-0111n-jeux`
  interrompu n'est pas compté comme un cycle de jeu réussi.
- **Tranche d'endurance close à 23 h 36** : 15 démarrages de contrôle et 4 cycles de jeu
  sains (contrôle d'interruption séparé), base sans erreur à `qemu-img check`. Sources du
  banc corrigé précisées dans les dossiers de campagnes (lancées avant le commit, HEAD du
  rapport `aca7459`, code du banc ensuite committé `04574f7`). `endurance.md` §16.
- **A/B neuf démarré à 23 h 36**, journal `.run/nuit-suite-0111.log` ; première barrière
  calme, zéro QEMU, 0,48 cœur étranger. Barrière explicite sur agents externes, compilations,
  autres campagnes d'endurance, Time Machine, puis `chargehote.py attendre`. Pilotage conservé
  dans `bench/tcg/ab/pilotage-m4-0111n/`. Le frontal compilé à 18 h est à jour (aucun fichier
  suivi plus récent) ; validation visuelle du curseur encore à faire.
- **Tests après corrections : 405 OK, mêmes 49 signalements ShellCheck, 5 ignorés.**
- **A/B neuf clos à 1 h 36**, 54 mesures, 18 parties au repos strict, aucun incident ni
  repli : PACKBITS **UT −3,19 %, cmr en course −7,08 %** ; autres cellules sous 3 %,
  inconclusives sur trois parties. Aucun réglage changé. `actuel/bits` sous 1 % partout :
  ne prouve pas le coût des avis d'ombre effectivement actifs, non comptés par ces relevés.
  Détail et valeurs sans arrondi : `docs/profil-m4-0211.md` §1. Profils hôte/invité des cinq
  jeux démarrés dans `m4-profils-0111n-{fen,pe}`.
- **Profils clos à 1 h 55**, cinq relevés hôte 30 s et invité 10 s. `profil-m4-0211.md`
  §2 : recherche de blocs 4,8–8,1 % des vCPU actifs ; base de temps d'UT 0,74 % (ne pas
  lui appliquer les 14 % du PC). d3 : rendu 0,178 cœur, attente de condition 82,2 %,
  conversion d'attributs non prioritaire sur ces données. Chargement de cmr long (410 s),
  course confirmée par capture et sommets ; premier profil à recouper avec les témoins 0058.
- La matrice garde maintenant `ms_image_precis` dans ses CSV ; `matabsum.py` calcule ses
  rapports dessus, avec compatibilité des anciennes archives. Cas reproduit : zen 2,7→2,6
  arrondi suggérait −3,7 %, contre −1,78 % en réalité ; test hors VM du rapport réussi,
  facteur de trois échanges par image conservé.
- **Variante 0058 préparée**, `tcg/0065` : `POLLNS=2`, `select` seulement pour un délai
  positif strictement sous 1 ms, mode 1 inchangé, défaut éteint. QEMU reconstruit à
  1 h 59, capacités complètes (`.run/build-pollns-court-0211.log`). 56 cas réussis,
  mutants à 1 ms et « tout positif » détectés, patch appliqué à une copie avant et
  résultat identique aux sources construites. A/B des cinq jeux avant toute décision.
  Binaire SHA-256 `8568ff593add448f6fbedf719213ab54aa58795291b09bec6cb9c0c9c6997da6`.
  Contrôle général : 405 OK, mêmes 49 signalements ShellCheck préexistants, 5 ignorés
  (`.run/tests-pollns-court-0211.log`).
- **A/B 0058 lancé à 2 h 00**, `m4-pollns-0211-{fen,pe}` : trois bras ref=0,
  tout=1, court=2, trois parties par bras, mb/zen/ut/d3 fen puis cmr pe. Profil hôte
  10 s après chaque fenêtre, pour comparer notamment UT sans peser sur sa mesure.
  Même barrière stricte et relevé de charge que le rattrapage ; provenance et pilotage
  conservés dans `bench/tcg/ab/pilotage-pollns-0211/`, journal `.run/pollns-0211.log`.
- **0058 en fenêtre clos à 3 h 04**, 36 mesures : mode 2 court zen −8,20 % mais
  **UT +10,06 %**, perte présente sur les trois parties ; mode 1 corrigé zen −9,60 %,
  UT +10,59 %. Autres écarts sous 3 %, inconclusifs. Aucun incident/repli, neuf parties
  valides ; un pic système de cinq secondes hors fenêtre de d3 est précisé au §42.2 de
  `tcg-g4.md`. Sur UT, charge des vCPU 1,07→1,63 cœur, plus de travail d'horloge et de code
  généré ; le mutex n'explique pas seul ce constat. Plein écran cmr continue, aucun allumage.
- **0058 clos sur les cinq jeux à 4 h 30**, 45 mesures sans incident ni repli : cmr
  mode 1 −1,20 %, mode 2 −0,31 %, inconclusifs ; neuf parties pe valides. Le mode court
  conserve UT +10,06 % : **écarté**, gardé comme option éteinte de diagnostic,
  `archive/m4-pollns-court-0211`. Contrat corrigé validé, mais aucun gain générique.
  Relevé séparé `m4-ut-mappage-0211` démarré à 4 h 31 : `in_asm,out_asm` filtré sur
  noyau/libSystem et `sample` hôte, pour identifier l'appelant des horloges. Aucun chiffre
  de vitesse de ce relevé instrumenté n'est ajouté à l'A/B.
- **Attente d'UT identifiée à 4 h 43**, `re/ut-attente-m4.md` : 96,8 % des coûts
  propres des helpers d'horloge du relevé diagnostique court vont aux trois lectures de
  `mach_absolute_time`. Traces LR séparées : appels dans `delay_idle`, fonction statique
  0x33e8c–0x3401c, boucle 0x33f3c–0x33fec ; source Apple xnu-792.24.17 identique à la
  version du noyau MD5 `def99c6e…`. Le PC vérifie son noyau avant de reprendre cette
  identification. Aucune attente supprimée, aucun levier allumé.
- `samplejit.py` relie `sample` au journal `out_asm` du même processus et vérifie
  `BL`/`BLR` avant d'attribuer un helper ; cartes réutilisées ou adresses groupées
  ambiguës restent inconnues. Six tests et deux mutants réussis.
- **VM normale relancée à 4 h 43** sous ImGuiDock, QEMU et frontal vivants. Le contrôle
  visuel du curseur reste à confirmer : l'exécutable sans bundle du frontal n'est pas
  exposé à l'outil UI (échecs d'accès par chemin et par nom, absent de l'inventaire).
  Le test de souris fenêtre/plein écran du frontal reste dans le contrôle général ;
  il ne remplace pas cette validation à l'œil.
- **Contrôle général après le mappage : 408 OK**, mêmes 49 signalements ShellCheck
  préexistants (liste de fichiers comparée), 5 ignorés ; test de souris du frontal réussi.
  `.run/tests-samplejit-0211.log`.
- **Tranche finale démarrée à 4 h 50**, VM quotidienne arrêtée proprement et frontal
  fermé : `m4-nuit-0211-fin-dem` (50 démarrages froids au plus, borne 5 h 35), puis
  `m4-nuit-0211-fin-jeux` (mb, zen, ut, d3, aucun nouveau cycle après 6 h 05).
  Base immuable du 01/11, QEMU `8568ff59…`, tous les défauts, attente précise éteinte.
  Journal `.run/nuit-fin-0211.log` ; remise de la VM normale après ces contrôles.
- **Zone froide arm64, préparation faite à 5 h 05** : revue des cinq familles de
  chemins lents et des pools littéraux, taille des régions non fixe (cas SMP=1 à couvrir).
  **36 portes longues exécutées sur le M4** à ±48/96 Mio, relocations réelles extraites
  du backend construit, deux mutants de condition/largeur détectés. Le portage complet
  et l'A/B restent à faire ; le garde x86-64 est inchangé, aucun allumage sur le M4.
  `docs/tcg-zone-froide-arm64.md`, `tools/tcg/coldbranchproof.*`.
- Contrôle général final des sources : **410 OK**, mêmes 49 signalements ShellCheck
  préexistants (liste comparée), 5 ignorés (`.run/tests-coldbranch-0211.log`).
- **Endurance finale close à 5 h 53**, 50 froids puis 5 démarrages/4 jeux, aucun incident,
  base sans erreur, aucune VM privée résiduelle. Nuit entière : **70 démarrages de
  contrôle et 8 jeux sains**, binaires distingués dans `endurance.md` §16–17.
- **Restauration corrigée à 5 h 59** : le frontal restait dans le groupe du lanceur avec
  `nohup` et s'arrêtait après lui. `host_detach` crée une session réelle sur macOS ;
  `matab.sh` l'utilise. Deux tests négatifs avant, positifs après (mort du groupe parent,
  enfant vivant, arguments préservés). VM quotidienne sous ImGuiDock, frontal/QEMU
  vivants après la fin du lanceur, SSH et kext contrôlés ; suivi au repos jusqu'à 6 h 40.
- Après cette correction : **412 OK**, mêmes 49 scripts en échec ShellCheck (fichiers
  comparés, diagnostics des trois scripts modifiés inchangés), 5 ignorés.
  `.run/tests-detach-0211.log`. Aucun changement de binaire QEMU ni de levier.
- **Clôture à 6 h 40** : frontal dans sa propre session et QEMU quotidien vivants,
  SSH répond, invité démarré depuis 42 minutes ; aucun QEMU privé ni campagne résiduelle.
  Suivi `bench/endurance/restauration-finale-0211.jsonl` : 48 relevés de 6 h 04 à 6 h 40,
  aucun échec SSH ni disparition des deux processus. VM laissée ouverte sous ImGuiDock,
  aucun verrou supprimé. Travaux poussés sur `main` ; reprise détaillée au §4.

**Fait le 01/11** (après-midi ; Time Machine et hermes tournaient en parallèle sur l'hôte)
- **QEMU reconstruit** (`a0e028e`, `.run/build-0111.log`) : `qgpu-core.c` du 31/10, `tcg/0059`
  et `tcg/0064` compilent sur arm64 (`✔ x-tb-head`), rien d'allumé de plus.
- **Kext, plugin `20261101-ombre` et GA** compilés **dans la VM quotidienne** (elle a gcc-4.0 ;
  sources par ssh dans `~/pomppc-build`, `install.sh` sans `QE`), invité redémarré :
  `QGPUVersion` 22, 4 clients. Anciens binaires dans `/var/tmp/avant-0111/` de l'invité.
  `disks/prebuilt` n'est pas refait sur le M4 (`run_tiger.sh` le dit plus ancien que les
  sources : job `tools/guest/jobs/prebuilt` à repasser).
- **Matrice de preuve** `bench/matrice/preuve-0111-m4` : mb-fen 6,3, mb-pe 6,5, d3-fen 29,7,
  d3-pe 30,1 ms/image, **vertes** (temps pris sous Time Machine). cmr-pe rouge : sa référence
  du 27/09 portait le **faux reflet** (même cause que le PC ; le jeu se range aussi en
  « machine lente » sur le M4 : `ValLowMachine` 1, `docs/re/cmr-rendu-vers-texture.md` §7).
  Ancienne référence dans `bench/matrice/ref/cmr-pe.faux-reflet-2709/`, nouvelle prise par
  `bench/matrice/cmr-nouvref-m4-0111` (41,8-42,6 ms/image en course), **à valider à l'œil**
  (`matrice.py --valider cmr-pe`). Le M4 mesurait déjà la course avant `6054367`.
- **A/B `m4-bundle-0111`** (`bench/tcg/ab/m4-bundle-0111-chaine.sh`) : comportement d'avant
  (`POMPPC_GL_PACKBITS=0 POMPPC_GL_QE_NONSIMPLE=0`) contre défaut, mb/d3 fen et mb/cmr pe,
  3 parties par bras ; barrière : fin de Time Machine puis hôte au repos ; finit par
  `matab.sh --restore`. Bilans : `bench/tcg/ab/m4-bundle-0111-{fen,pe}/bilan.txt`.

- **Passes de cohérence doc/code (soir)** : six passes de documentation, puis dix passes
  « code ↔ doc » en worktrees fusionnées dans `main` (le code fait foi). Elles ont touché des
  **commentaires** de `patches/` (qgpu-pci/core/gl, `qgpu_proto.h`, `qgpu_abi.h` et sa copie du
  kext, `screamer.c`, en-têtes de `tcg/0037` et `0038`) : l'empreinte de la série change. Le M4
  a reconstruit `~/src/qemu/build` sur `d086f03` (`.run/build-0111b.log`) ; **le PC reconstruit
  `build/` et `build-fast/`** (de toute façon dû à `tcg/0064`). Le kext, le plugin et le GA de la
  VM quotidienne restent valables (seuls des commentaires ont changé ; code du plugin vérifié
  identique hors commentaires). Bugs corrigés dans le code : paquet invité publié et CD des
  sources sans plugin GA (QE), CD des sources non regravé sur les en-têtes du contrat, test
  §5 bis de `run-all.sh` en échec depuis le 29/10 (copies QE du kext, maintenant vérifiées par
  valeur), deux libellés du frontal, deux messages de `run_tiger.sh`. `tests/run-all.sh` :
  397 OK ; les 49 échecs sont des avertissements de shellcheck 0.11, tous présents avant.
- **A/B relancé** après la reconstruction, puis **arrêté** à la demande de l'utilisateur
  (redémarrage de l'hôte) ; VM éteinte proprement, verrou non touché. Joué en fenêtre seulement,
  2 parties par bras (avant-3 interrompue, rangée en `.interrompu`) : **d3-fen 29,1 → 29,0
  (−0,0 %)**, **mb-fen 6,0 → 6,2 (+2,5 %)** (`matabsum.py bench/tcg/ab/m4-bundle-0111-fen`).
  PACKBITS ne se voit pas sur d3 au M4 ; mb-fen paie l'avis d'ombre (Terminal posé par la
  matrice), moins que le +6 % du PC. **À reprendre** : 3ᵉ partie par bras en fenêtre, puis le
  plein écran (mb-pe, cmr-pe) : `bench/tcg/ab/m4-bundle-0111-chaine.sh` (ranger d'abord
  `m4-bundle-0111-fen` en `.partiel`). Puis `matab.sh --restore` si la chaîne ne finit pas.
  Référence cmr-pe du M4 toujours à valider à l'œil.

Ci-dessous, l'état du 30/10 (fusionné le 01/11 avec l'ancienne « 3 bis » du 29/10).

**Défauts sur Darwin:arm64** : depuis le 26/10 (`550b2a8`), la série du PC est allumée et
prouvée : `PPC32MT TLBPRECISE LMWINLINE DCBZINLINE FPNATIVECMP VFPNATIVECMP DIRTYSKIP PTEGMAP
JC2WAY FPKNOWN FPRFLAZY STFSKNOWN LFSKNOWN TLBBASE`. A/B : DOOM 3 −23,9 %, UT2004 −18,6 %,
Marble Blast −22,7 %. Garde : un binaire sans tcg/0037-0039 les laisse éteints. Quartz Extreme
allumé depuis le 29/10 (`c792464` : `run_tiger.sh` pose `qe=on`, `QE=0` l'éteint ; le kext en
personnalité « auto » le suit). `tcg/0057`–`0059`, `0064`–`0067` compilés ;
options éteintes. `x-tcg-cold` est disponible sur arm64 système depuis `0066`,
**éteint par défaut sur le M4** ; attente progressive via `POLLNS=3`, éteinte aussi.

**Binaire et VM quotidienne** : `~/src/qemu/build` reconstruit le 04/11 vers 20 h 29,
SHA-256 `d6e6e3e2caf092f66…`, série actuelle jusqu'à `0059` et `0064`–`0067`, `usbhid/0002`,
`guest-driver` et port Windows du cœur qgpu (hors chemin Apple). Aucun levier nouveau.
Kext du 02/11 et plugin `20261102-modes` conservés ; VM quotidienne restaurée le 04/11 à 21 h 18.

**Fait le 30/10**
- **Émulateurs voisins, A/B au repos** (`docs/tcg-g4.md` §42, 4 parties par bras, mb zen ut
  d3 en fenêtre). `tcg/0057` `x-vcpu-qos` (`VCPUQOS=1`, fils des vCPU en QoS interactive) :
  **neutre**. `tcg/0058` `POMPPC_POLL_NS` (`POLLNS=1`, attente de la boucle principale à la
  microseconde par `select`) : zen **−18 %** mais ut **+6,6 %**. Les deux sont **éteints**
  par défaut, compilés dans `~/src/qemu`. Le PC n'est pas concerné : le code est propre à
  macOS.
- **Bug QE corrigé : bande blanche, noire ou « ancien jeu » sous l'ombre d'une fenêtre posée
  sur un jeu en fenêtre** (signalé par l'utilisateur). Dans le WindowServer, le chemin
  rapide des clés d'état liait la texture de surface vide au lieu de la texture d'export du
  jeu. Correctif : `tex_bind_id` dans le plugin, sans changer le protocole, ni QEMU, ni le
  kext. Vérifié en VM jetable sur Zenerchi, Marble Blast et DOOM 3 ; matrice mb/zen/d3 en
  fenêtre dans `bench/matrice/qe-ombre-3010`. Coins noirs de DOOM 3 et Prey : la fenêtre
  est `NSBackingStoreRetained` (relevé sous gdb), comme sur un vrai Mac ; pas un bug.
  (01/11 : la bande « ancien jeu » sous DOOM 3 avait une seconde cause, corrigée sur le PC par
  le kext du 01/11 et le plugin `20261101-ombre`, section 2.)
- **Fin de séance, 11 h 30** : la VM jetable est arrêtée et `git` est propre. La VM
  quotidienne a été relancée par `matab.sh --restore`, puis s'est fermée ; aucune VM ne tourne.
  Elle repart avec `./run_tiger.sh`.

**Fait du 28 au 29/10** (détail : CHANGELOG, `docs/quartz-extreme.md`, `docs/endurance.md`)
- Vidage autonome par bandes (`403593e`, `QGPU_CAP_TEX_READBACK_ROWS`, sans changer
  `QGPU_PROTO_VERSION`) : cmr-pe en 1920×1080 vert. Une texture hôte sans image est comptée à
  part (« 2/2 (1 sans image) ») ; un refus du device se distingue par l'en-tête prérempli à
  0xFF (`917ae47`).
- **Matrice entière `20261028-1854` : 17 cellules vertes sur 17**, dont Warcraft III. Gain sur
  le 26/10 non attribué (frontal et état changés, aucun A/B) ; cmr +11 % mais passé de 1024×768
  à 1920×1080.
- **Endurance du 28 au 29/10** (`endurance.md` §11) : 342 démarrages et 40 cycles de jeu, aucun
  incident ; QE : §12 (ancien chemin) et §13 (chemin direct), 156 démarrages et 32 cycles de
  jeu en fenêtre, aucun incident.
- **Quartz Extreme P1 à P3** (`quartz-extreme.md` §1 ter et journal) : bureau juste, fenêtres
  GL par l'hôte, puis le jeu présente lui-même son échange par le noyau (méthode 5). A/B au
  repos en fenêtre : QE au niveau de QE éteint ou mieux (mb 6,1-6,3 contre 6,4, zen 2,9-3,1
  contre 3,5, d3 28,8-29,7 contre 29,8, ut 19,6-19,7 contre 19,9 ms/image).
- Outil : `/var/tmp/pomppc-env` passe les variables `POMPPC_*` au WindowServer.

**Pièges du M4**
- Lancement d'un banc depuis le service des outils : priorité très basse observée le 04/11 ;
  **lancer depuis le Terminal** et vérifier le vrai PID QEMU. Un `DISK` explicite dans la ligne
  du shell peut aussi tromper le `pgrep -f` des profils (`chantiers/metrologie.md`).
- `matab.sh --restore` lance la VM sous ImGuiDock, les parties de `matab.sh` sous le frontal
  natif : la résolution des jeux en plein écran en dépend (1920×1080 contre 1024×768).
- Un frontal ImGuiDock resté ouvert après l'arrêt de sa VM tourne à 60 % d'un cœur et double
  les temps en fenêtre ; la fenêtre de mesure d'UT (60 images) demande 3 passes.
- Les traces du plugin (`POMPPC_GLTRACE`, vidages de tampons) ont **rempli le disque de
  l'invité** (16 Go) le 29/10 : les effacer après usage.

**À faire sur le M4** : `TODO.md`, sections « Actifs » et « Geste de l'utilisateur » (essai de la 0.4.1 sur le M1, preuve de QE aux
résolutions, plugin et coût d'un avis d'ombre actif) ; la liste venue du PC, au §4, est faite. Seulement
si besoin : parité de **tcg/0055**
(si les traces de second niveau reprennent, arrêtées depuis le 22/10) ; `vperm` en ligne
(TBL/TBX, gain plafonné vers 0,3 %).

**Propres au M4** : pas de `qemu-ppc` linux-user (les preuves passent par l'invité), outils
`ppc32mt-bench.sh` et `chargehote.py` portés sur macOS, et `references.csv`, le fichier de
références de la matrice **du M4** (le PC utilise `references-linux.csv` et ne touche jamais
`references.csv`).

## 4. À faire en passant d'un hôte à l'autre

- **Venu du PC le 01/11 (pour le M4), dans l'ordre** — reprend les entrées du 30/10 et du
  31/10, réduites le 01/11 à ce qui n'y figure pas :
  (Bilan du 02/11 : points 1-2 faits, la VM **quotidienne** servant de VM de dev ; preuves
  et mesures neuves faites, profils clos. Matin : attente progressive et portage
  complet de la zone froide validés, A/B clos, options éteintes. Reste le coût
  d'un avis d'ombre effectivement actif ; §3.)
  1. `git pull` ; **reconstruire QEMU** (`qgpu-core.c` du 31/10 : reflets de cmr ; `tcg/0059`
     nommé depuis le 30/10) ; ni le protocole ni l'ABI n'ont changé. Vérifier que la série se
     compile sur arm64 avec `tcg/0059` (l'opcode générique n'y est jamais émis ; rien ne change
     sans `VPERMINLINE=1`).
  2. Dans la VM de dev du M4 : **reconstruire kext, plugin et GA** depuis HEAD (kext du 01/11
     avec la méthode 6 ; plugin `20261101-ombre` qui contient aussi PACKBITS et UVWIDE),
     `install.sh` sans `QE`, puis **redémarrer l'invité**. Les kext du 30/10 (`805f504`, curseur
     immobile) et plugins du 30/10 (`f01d2d7`, `pomppc_qe_same` ; `tex_bind_id`) y sont inclus.
  3. Ombre de DOOM 3 confirmée par l'utilisateur à 22 h 39, reflets de cmr validés sur la
     nouvelle preuve en course. Curseur confirmé réglé par l'utilisateur le 02/11.
  4. **Mesures neuves faites**, `m4-bundle-0111n-{fen,pe}` : trois bras du même plugin
     séparent PACKBITS et chemin non-simple, pas une ancienne compilation du bundle
     (artefacts antérieurs absents). PACKBITS UT −3,19 %, cmr −7,08 %, autres écarts
     inconclusifs ; mb-fen et mb-pe inclus. **Reste : coût d'un avis d'ombre actif sous
     une fenêtre posée sur le jeu**, ces relevés ne le prouvent pas. cmr mesuré en course,
     référence sans faux reflet, `references.csv` du M4 renouvelé. `profil-m4-0211.md`.
  5. **Profils des cinq jeux faits**, puis variante courte `tcg/0065` mesurée et écartée
     (UT +10,06 %, `tcg-g4.md` §42.2). Attente active `delay_idle` identifiée sur ce noyau
     (`re/ut-attente-m4.md`), empreinte du PC à vérifier. **Matin : attente progressive
     0067 et portage froid 0066 terminés**, FP/tlbb/pools et SMP=1 couverts ; matrice
     complète close, aucun allumage (`tcg-zone-froide-arm64.md` §6–7).
  6. **Endurance faite** : 70 démarrages de contrôle et 8 jeux sains dans la nuit,
     résultats au §3 et dans `endurance.md` §16–17. Restauration détachée corrigée.

- **Avant de quitter un hôte** : arrêter les campagnes en cours, remettre la VM quotidienne
  (`tools/tcg/matab.sh --restore`), mettre à jour cette page, puis commit et push.
- **En arrivant** : `git pull`, reconstruire si `patches/` a bougé, lire la section de l'hôte.
- Le disque Tiger raw de la VM quotidienne peut circuler par USB entre les hôtes ; il se
  prépare sur le M4 (`docs/gpu-3d-tiger.md` §5.1).
- Ce qui est commun aux deux hôtes : le frontal, le protocole qgpu, le kext et le plugin de
  l'invité, la matrice et ses jeux. Un changement de `patches/qgpu/qgpu_abi.h` impose de
  reconstruire le kext et de redémarrer l'invité sur **les deux** hôtes ; un changement de
  `qgpu_proto.h` impose de reconstruire QEMU et le plugin.

## 5. Référence validée

Repères au 04/11 ; le détail de chaque hôte est aux §2 et §3.

| Élément | État |
|---|---|
| Protocole / ABI | GL **v22**, transport kext **v19** ; capacités ajoutées sans changer de version (`TEX_PRESENT`, `TEX_READBACK_ROWS`, …) |
| QEMU | 11.1.2 + série `patches/` ; PC : `~/src/qemu/build-fast` (PGO du 31/10, -O3, -march=native ; le profil avec cmr du 01/11 attend son recontrôle dans `build-fast-pgo0111cmr/`) et `build/` (référence) ; M4 : `~/src/qemu/build` (reconstruit le 04/11 vers 20 h 29, `d6e6e3e2caf092f66…`, série actuelle incluant `0066/0067`, `usbhid/0002` et le port Windows de qgpu) |
| Défauts TCG | PC : série jusqu'à `0056` (zone froide) ; M4 : série du PC jusqu'à `0052` (avec `0037`-`0039`) ; `0057`-`0059`, `0064`-`0067` compilés sur le M4, options éteintes ; zone froide arm64 disponible via `0066`. PC : reconstruire référence + rapide depuis `0066/0067` et `usbhid/0002`, puis valider le finaliseur de pools. `0060`-`0063` éteints, non nommés |
| Invité | Tiger 10.4.11 ; PC : kext du 01/11 (méthode 6, surface non simple) et plugin `20261101-ombre`, ceux du 02/11 à installer (`docs/etat-des-hotes.md` §2) ; M4 : kext du 02/11 et plugin `20261102-modes`, VM quotidienne depuis le 02/11 à 18 h 16 (anciens dans `/var/tmp/avant-0211/` de l'invité) |
| Matrice | PC : `references-linux.csv`, 8 références toutes validées à l'œil (d3-fen et d3-pe le 30/10, `a89515b` ; cmr-pe le 01/11), 5 cellules dans les A/B (mb, zen, ut, d3 en fenêtre, cmr plein écran **en course** depuis `6054367`) ; M4 : `references.csv` (jamais touché par le PC), 17/17 vertes le 28/10 ; le 01/11, mb et d3 fen/pe vertes avec le nouveau bundle, cmr-pe reprise (faux reflet, `docs/re/cmr-rendu-vers-texture.md` §7), six références renouvelées et validées à l'œil dans la nuit, six preuves vertes avec `0066/0067` le 02/11 |
| Contrôle général | PC 245 OK / 0 échec (14/10, `docs/binaire-rapide-x86.md`) ; M4 : 485 OK / 0 échec / 4 ignorés (03/11) ; `--slow` : 470 OK / 0 échec / 1 ignoré (02/11 matin), 49 scripts ShellCheck corrigés |
